/**
 * @file data_task.cpp
 * @brief 数据面任务：虚拟网卡收发、加解密与转发。
 *
 * @details 职责：
 * - 消费路由快照邮箱（长度 1，按值覆盖），持有**自己的只读副本**；
 * - 处理 netif 出站 pbuf：展平 IP 报文 → 校验 → 目的 IP 查目标节点 → 查下一跳 →
 *   构造 Data 帧并**由源节点加密** → 交给 I/O 任务；
 * - 处理入站 Data 帧：目的为本机则解密后投递 lwIP（`tcpip_input`）；否则保持
 *   `to_peer_id`、密文与 AEAD 尾不变，只递增 `forward_counter` 后转发；
 * - 所有路径都有计数，绝不静默丢弃。
 *
 * @note 控制面与数据面各持一个 AEAD 实例（互不共享）；本任务不做握手/RPC/路由计算。
 */
#include "context.hpp"

#if defined(ESP_PLATFORM)

#include <cstring>

#include "data/plane.hpp"
#include "protocol/wire.hpp"
#include "support/log.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.DATA";
/// 数据面轮询周期（毫秒）。
constexpr uint32_t kLoopPeriodMs = 5;
constexpr TickType_t kLoopDelayTicks = pdMS_TO_TICKS(kLoopPeriodMs) > 0
    ? pdMS_TO_TICKS(kLoopPeriodMs) : 1;
constexpr size_t kMaxFramesPerLoop = 8;
static_assert(kLoopDelayTicks > 0);

/// 单个 IP 报文的最大长度（虚拟 MTU 上限 1380，留出余量）。
constexpr size_t kMaxIpPacket = 1600;

/** @brief 投递一个 IP 报文到 lwIP（成功后 pbuf 所有权归 TCPIP 线程）。 */
void deliver_to_netif(Context& ctx, Bytes ip_packet) noexcept {
    if (!ctx.netif_created || ip_packet.empty()) {
        ctx.dropped.fetch_add(1);
        return;
    }
    struct pbuf* p = pbuf_alloc(PBUF_RAW, static_cast<u16_t>(ip_packet.size()), PBUF_POOL);
    if (p == nullptr) {
        ctx.dropped.fetch_add(1);
        return;
    }
    if (pbuf_take(p, ip_packet.data(), static_cast<u16_t>(ip_packet.size())) != ERR_OK) {
        pbuf_free(p);
        ctx.dropped.fetch_add(1);
        return;
    }
    struct netif* ni = ctx.virtual_netif.netif();
    if (ni->input(p, ni) != ERR_OK) {
        pbuf_free(p);  // 失败时由我们释放；成功时 TCPIP 线程接管
        ctx.dropped.fetch_add(1);
    }
}

/**
 * @brief 构造并发送一个出站 Data 帧（源节点加密；关闭加密时为明文）。
 * @param to_peer PM 头的最终目的节点
 * @param next_hop 实际使用的连接（下一跳）对应的对端 peer id
 * @note 与上游一致：`enable_encryption=false` 时既不加 AEAD 尾也不置加密位。
 *       PM 头的 len 始终保存**明文** IP 报文长度。
 */
void send_data_frame(Context& ctx, uint32_t generation, uint32_t to_peer, uint32_t next_hop,
                     Bytes ip_packet) noexcept {
    uint8_t frame[kPeerManagerHeaderSize + kMaxIpPacket + kAeadTailSize];
    if (kPeerManagerHeaderSize + ip_packet.size() > sizeof(frame)) {
        ctx.dropped.fetch_add(1);
        return;
    }
    size_t payload_len = 0;
    uint8_t flags = 0;
    if (ctx.settings.enable_encryption) {
        if (ctx.data_cipher == nullptr || !ctx.data_cipher->ready()) {
            ctx.dropped.fetch_add(1);
            return;
        }
        // 明文 payload = IP 报文；PM.len 保存明文长度（加密不改动它）。
        const size_t written = ctx.data_cipher->encrypt_random_nonce(
            ip_packet, MutBytes(frame + kPeerManagerHeaderSize,
                                sizeof(frame) - kPeerManagerHeaderSize));
        if (written == 0) {
            ctx.set_error(ESP_FAIL);
            ctx.dropped.fetch_add(1);
            return;
        }
        payload_len = written;
        flags = pm_flags::Encrypted;
    } else {
        std::memcpy(frame + kPeerManagerHeaderSize, ip_packet.data(), ip_packet.size());
        payload_len = ip_packet.size();
    }
    PeerManagerHeader hdr{};
    hdr.from_peer_id = ctx.my_peer_id.load();
    hdr.to_peer_id = to_peer;
    hdr.packet_type = static_cast<uint8_t>(PacketType::Data);
    hdr.flags = flags;
    hdr.forward_counter = 1;  // 源节点从 1 开始
    hdr.reserved = 0;
    hdr.len = static_cast<uint32_t>(ip_packet.size());
    encode_peer_manager_header(MutBytes(frame, kPeerManagerHeaderSize), hdr);

    // 用 Context 里的暂存消息（约 2 KiB），避免把整帧消息放在数据面任务栈上。
    FrameMessage& msg = ctx.data_tx_scratch;
    msg.generation = generation;
    msg.slot = kAnySlot;   // 由 I/O 任务按 to_peer 选择连接槽
    msg.to_peer = next_hop;
    msg.len = static_cast<uint16_t>(kPeerManagerHeaderSize + payload_len);
    std::memcpy(msg.data.data(), frame, msg.len);
    if (xQueueSend(ctx.q_tx, &msg, 0) != pdTRUE) {
        ctx.dropped.fetch_add(1);
    }
}

/** @brief 处理 netif 出站 pbuf（来自 TCPIP 线程的队列）。 */
void handle_netif_output(Context& ctx, const NetifRxItem& item) noexcept {
    if (item.p == nullptr) {
        return;
    }
    const size_t len = item.p->tot_len;
    if (len < 20 || len > kMaxIpPacket) {
        ctx.dropped.fetch_add(1);
        return;
    }
    uint8_t ip_buf[kMaxIpPacket];
    if (pbuf_copy_partial(item.p, ip_buf, static_cast<u16_t>(len), 0) != len) {
        ctx.dropped.fetch_add(1);
        return;
    }
    const Bytes ip_packet(ip_buf, len);
    if (!ipv4_is_valid(ip_packet, ctx.settings.mtu)) {
        ctx.dropped.fetch_add(1);
        return;
    }
    const uint32_t dst = ipv4_destination(ip_packet);
    if (ipv4_is_broadcast_or_multicast(dst)) {
        // 首版不支持出站广播/组播（设计已明确），显式计数。
        ctx.dropped.fetch_add(1);
        return;
    }
    const uint32_t target = ctx.data_snapshot.lookup_ip(dst);
    if (target == kInvalidPeerId) {
        ctx.dropped.fetch_add(1);  // 无路由
        return;
    }
    if (target == ctx.my_peer_id.load()) {
        // 目的就是自己：直接回注 lwIP。
        deliver_to_netif(ctx, ip_packet);
        return;
    }
    const uint32_t next_hop = ctx.data_snapshot.lookup_next_hop(target);
    if (next_hop == kInvalidPeerId) {
        ctx.dropped.fetch_add(1);
        return;
    }
    // I/O 任务会按 to_peer 选择到下一跳的连接；这里只负责填好目标。
    send_data_frame(ctx, item.generation, target, next_hop, ip_packet);
}

/** @brief 处理入站 Data 帧：本机投递或按下一跳转发。 */
void handle_inbound_data(Context& ctx, FrameMessage& frame) noexcept {
    PeerManagerHeader hdr{};
    if (!decode_peer_manager_header(Bytes(frame.data.data(), kPeerManagerHeaderSize), hdr)) {
        ctx.dropped.fetch_add(1);
        return;
    }
    const bool encrypted = (hdr.flags & pm_flags::Encrypted) != 0;
    const size_t expected =
        kPeerManagerHeaderSize + hdr.len + (encrypted ? kAeadTailSize : 0);
    if (frame.len != expected || hdr.len == 0) {
        ctx.dropped.fetch_add(1);
        return;
    }
    const DataPlan plan = plan_inbound(ctx.data_snapshot, hdr, kForwardCounterLimit);
    if (plan.action == DataAction::DeliverLocal) {
        MutBytes payload(frame.data.data() + kPeerManagerHeaderSize,
                         frame.len - kPeerManagerHeaderSize);
        if (encrypted) {
            // 关闭加密时与上游 NullCipher 一致：明文可收，已加密报文直接拒绝。
            if (!ctx.settings.enable_encryption || ctx.data_cipher == nullptr ||
                !ctx.data_cipher->ready() || !ctx.data_cipher->decrypt_in_place(payload)) {
                ctx.dropped.fetch_add(1);
                return;
            }
        }
        if (!ipv4_is_valid(payload, ctx.settings.mtu)) {
            ctx.dropped.fetch_add(1);
            return;
        }
        deliver_to_netif(ctx, payload);
        return;
    }
    if (plan.action == DataAction::Forward) {
        if (!prepare_forward(hdr, kForwardCounterLimit)) {
            ctx.dropped.fetch_add(1);  // forward_counter 超限，防环丢弃
            return;
        }
        // 只更新 PM 头（计数），密文、AEAD 尾与 to_peer_id 原样保留。
        encode_peer_manager_header(MutBytes(ctx.data_tx_scratch.data.data(),
                                            kPeerManagerHeaderSize),
                                   hdr);
        FrameMessage out = frame;
        std::memcpy(out.data.data(), ctx.data_tx_scratch.data.data(), kPeerManagerHeaderSize);
        out.slot = kAnySlot;
        out.to_peer = plan.next_hop;  // 保持最终目的（PM 头）不变，只改使用的连接
        if (xQueueSend(ctx.q_tx, &out, 0) != pdTRUE) {
            ctx.dropped.fetch_add(1);
        }
        return;
    }
    ctx.dropped.fetch_add(1);  // 无路由或非法报文
}

}  // namespace

/** @brief 数据面任务主循环。 */
void data_task(void* arg) noexcept {
    auto& ctx = *static_cast<Context*>(arg);

    while (!ctx.stop_requested.load()) {
        // 1) 路由快照：长度 1 邮箱，按值复制为自己的只读副本。
        RouteSnapshot snapshot{};
        if (xQueueReceive(ctx.q_snapshot, &snapshot, 0) == pdTRUE) {
            ctx.data_snapshot = snapshot;
        }

        // 2) netif 出站报文（pbuf 由本任务释放我们增加的引用）。
        NetifRxItem item{};
        for (size_t n = 0; n < kMaxFramesPerLoop &&
             xQueueReceive(ctx.q_netif_rx, &item, 0) == pdTRUE; ++n) {
            handle_netif_output(ctx, item);
            if (item.p != nullptr) {
                pbuf_free(item.p);
            }
        }

        // 3) 入站数据帧同样限额，避免持续流量下无法回到循环末尾。
        for (size_t n = 0; n < kMaxFramesPerLoop &&
             xQueueReceive(ctx.q_rx_data, &ctx.data_rx_scratch, 0) == pdTRUE; ++n) {
            handle_inbound_data(ctx, ctx.data_rx_scratch);
        }
        // 无论队列是否有流量都真正阻塞至少 1 tick，不能仅用 taskYIELD 喂养高优先级任务。
        vTaskDelay(kLoopDelayTicks);
    }

    (void)xSemaphoreGive(ctx.task_exit);
}

}  // namespace et

/** @brief FreeRTOS 任务入口（C 链接）。 */
extern "C" void et_data_task_entry(void* arg) { et::data_task(arg); }

#endif  // ESP_PLATFORM
