/**
 * @file proto_task.cpp
 * @brief 控制面任务：握手、保活、RPC（分片/重组/压缩协商）与状态机。
 *
 * @details 职责（M1 + M2）：
 * - 消费应用命令（物理网络就绪/断开）与 I/O 事件（连接建立/断开）；
 * - 连接建立后立即发送握手，5 秒内未完成握手则要求 I/O 重连；
 * - 校验对端握手（网络名、摘要、peer id 冲突），成功后进入 Syncing 并上报 PeerUp；
 * - 保活：按 1/2/4/8/16/32 秒退避发送 Ping，收到 Pong 后拉长间隔；
 * - RPC：控制面报文按上游规则加密（对直连对端一律加密），支持分片发送、乱序重组、
 *   事务匹配与压缩协商（只接受 None，收到 Zstd 视为不支持并上报）；
 * - 服务：M2 注册 `network_name/OspfRouteRpc/SyncRouteInfo/1` 并返回空拓扑响应，
 *   M3 在同一入口填充真实拓扑与会话语义；
 * - 数据面报文（Data）在 M4 实现，这里只计数丢弃。
 */
#include "context.hpp"

#if defined(ESP_PLATFORM)

#include <cstring>

#include "esp_netif.h"
#include "lwip/inet.h"
#include "peer/handshake.hpp"
#include "peer/keepalive.hpp"
#include "route/route_info.hpp"
#include "route/sync_messages.hpp"
#include "route/table.hpp"
#include "route/topology.hpp"
#include "support/log.hpp"
#include "support/random.hpp"
#include "support/time.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.PROTO";
/// 控制面循环周期（毫秒）。
constexpr uint32_t kLoopPeriodMs = 5;
// 100 Hz 时 5 ms 被 pdMS_TO_TICKS 截为 0；vTaskDelay(0) 不能让低优先级 IDLE 运行。
constexpr TickType_t kLoopDelayTicks = pdMS_TO_TICKS(kLoopPeriodMs) > 0
    ? pdMS_TO_TICKS(kLoopPeriodMs) : 1;
constexpr size_t kMaxMessagesPerLoop = 8;  ///< 持续入队也必须给定时器和 IDLE 留出机会
static_assert(kLoopDelayTicks > 0);

/// 握手超时（毫秒），与上游一致。
constexpr uint32_t kHandshakeTimeoutMs = 5000;

/// 路由同步间隔（毫秒）。M3 会按上游会话规则调整。
constexpr uint32_t kRouteSyncIntervalMs = 5000;
/// RPC 事务超时（毫秒）。
constexpr uint32_t kRpcTimeoutMs = 10000;
/// 收到对方拓扑后主动反向宣告的最小间隔（毫秒），避免同步风暴。
constexpr uint32_t kMinReverseSyncMs = 1000;

/** @brief 把一帧投递给 I/O 任务发送（带代际，旧代际会被丢弃）。 */
bool send_frame(Context& ctx, uint32_t generation, Bytes body) noexcept {
    FrameMessage msg{};
    msg.generation = generation;
    msg.len = static_cast<uint16_t>(body.size());
    std::memcpy(msg.data.data(), body.data(), body.size());
    if (xQueueSend(ctx.q_tx, &msg, 0) != pdTRUE) {
        ctx.dropped.fetch_add(1);
        return false;
    }
    return true;
}

/**
 * @brief 组一个控制面帧并按需加密后发送。
 * @param encrypt 是否加密（RPC 跟随 `enable_encryption`；握手/Ping/Pong 永不加密）
 * @note PM 头的 len 保存**加密前的明文长度**，这是与上游互通的关键。
 */
bool send_control_frame(Context& ctx, uint32_t generation, PacketType type, uint32_t from_peer,
                        uint32_t to_peer, Bytes plain, bool encrypt) noexcept {
    if (plain.size() > kMaxPlainPayload) {
        ctx.dropped.fetch_add(1);
        return false;
    }
    PeerManagerHeader hdr{};
    hdr.from_peer_id = from_peer;
    hdr.to_peer_id = to_peer;
    hdr.packet_type = static_cast<uint8_t>(type);
    hdr.flags = encrypt ? pm_flags::Encrypted : 0;
    hdr.forward_counter = 1;
    hdr.reserved = 0;
    hdr.len = static_cast<uint32_t>(plain.size());

    size_t body_len = 0;
    if (encrypt) {
        if (ctx.control_cipher == nullptr || !ctx.control_cipher->ready()) {
            ctx.set_error(ESP_ERR_INVALID_STATE);
            return false;
        }
        const size_t written = ctx.control_cipher->encrypt_random_nonce(
            plain, MutBytes(ctx.rpc_frame.data() + kPeerManagerHeaderSize,
                            ctx.rpc_frame.size() - kPeerManagerHeaderSize));
        if (written == 0) {
            ctx.set_error(ESP_FAIL);
            ctx.dropped.fetch_add(1);
            return false;
        }
        body_len = kPeerManagerHeaderSize + written;
    } else {
        if (kPeerManagerHeaderSize + plain.size() > ctx.rpc_frame.size()) {
            ctx.dropped.fetch_add(1);
            return false;
        }
        std::memcpy(ctx.rpc_frame.data() + kPeerManagerHeaderSize, plain.data(), plain.size());
        body_len = kPeerManagerHeaderSize + plain.size();
    }
    encode_peer_manager_header(MutBytes(ctx.rpc_frame.data(), kPeerManagerHeaderSize), hdr);
    return send_frame(ctx, generation, Bytes(ctx.rpc_frame.data(), body_len));
}

/**
 * @brief 按上游预算分片发送一个 RPC 消息（内容为 RpcRequest/RpcResponse 的编码）。
 * @param content 已编码的服务入参/返回值
 * @param put_in_rpc_body 为 true 时 content 表示"RpcPacket.body 的原始内容"
 *        （即已编码的 RpcRequest/RpcResponse）
 */
bool send_rpc_message(Context& ctx, uint32_t generation, const RpcSendTemplate& tmpl,
                      Bytes content) noexcept {
    RpcPlan plan;
    if (!plan_rpc_slices(tmpl, content.size(), plan)) {
        ctx.set_error(ESP_ERR_INVALID_SIZE);
        ctx.dropped.fetch_add(1);
        return false;
    }
    for (size_t i = 0; i < plan.count; ++i) {
        const RpcSlice& slice = plan.slices[i];
        const size_t encoded = encode_rpc_piece(
            tmpl, Bytes(content.data() + slice.offset, slice.len),
            static_cast<uint32_t>(plan.count), static_cast<uint32_t>(i), ctx.rpc_plain);
        if (encoded == 0) {
            ctx.set_error(ESP_ERR_INVALID_SIZE);
            ctx.dropped.fetch_add(1);
            return false;
        }
        const PacketType type = tmpl.is_request ? PacketType::RpcReq : PacketType::RpcResp;
        // RPC 与数据共用同一个加密开关：关闭时不加 AEAD 尾也不置加密位。
        if (!send_control_frame(ctx, generation, type, tmpl.from_peer, tmpl.to_peer,
                                Bytes(ctx.rpc_plain.data(), encoded),
                                /*encrypt=*/ctx.settings.enable_encryption)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 要求 I/O 断开并重连某个槽。
 * @param generation 该槽当前的会话代际（I/O 任务据此定位槽位）
 */
void request_reconnect(Context& ctx, uint32_t generation) noexcept {
    SmallMessage msg{};
    msg.kind = static_cast<uint8_t>(IoCommand::Reconnect);
    msg.generation = generation;
    if (xQueueSend(ctx.q_io_cmd, &msg, 0) != pdTRUE) {
        ctx.dropped.fetch_add(1);
    }
}

/**
 * @brief 由会话代际反查该连接的对端 peer id。
 * @details 代际由全局计数器分配，因此一个代际唯一对应一个连接槽。
 */
uint32_t peer_of_generation(const Context& ctx, uint32_t generation) noexcept {
    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        if (ctx.slots[i].generation.load() == generation) {
            return ctx.slots[i].peer_id.load();
        }
    }
    return kInvalidPeerId;
}


/**
 * @brief 撤销 Ready：遇到无法协商的协议不兼容（当前只有对端使用 zstd 压缩）时调用。
 *
 * @details 设计文档要求"收到 Zstd 上报不支持、取消事务并撤销 Ready"。只设置
 *          `last_error` 是不够的：节点会一直停在 `ready` 却永远学不到路由，接入方
 *          无法判断组网其实已不可用。这里把状态切到 @ref State::Error 并在**曾经
 *          上报过 Ready** 时补发一次错误事件（避免每个 zstd 报文都刷事件）。
 */
void revoke_ready(Context& ctx, esp_err_t err) noexcept {
    ctx.set_error(err);
    ctx.dropped.fetch_add(1);
    if (ctx.network_ready_reported) {
        ctx.network_ready_reported = false;
        ctx.state.store(State::Error);
        ctx.emit(EventType::Error);
    }
}

/** @brief 处理一个完整的 RPC 请求（目标为本机）。 */
void handle_rpc_request(Context& ctx, uint32_t generation, const RpcPacketRef& packet,
                        Bytes body) noexcept {
    const uint32_t my_peer_id = ctx.my_peer_id.load();
    if (packet.to_peer != my_peer_id) {
        // 非本地目标的 RPC 需要按 overlay 路由转发，M4 实现。
        ctx.dropped.fetch_add(1);
        return;
    }
    if (!packet.has_descriptor) {
        ctx.dropped.fetch_add(1);
        return;
    }
    if (packet.has_compression && packet.compression.algo == kCompressionZstd) {
        ET_LOGE(kTag, "peer used zstd compression, which this profile does not support");
        revoke_ready(ctx, ESP_ERR_NOT_SUPPORTED);
        return;
    }
    RpcRequestRef request{};
    if (!decode_rpc_request(body, request)) {
        ctx.dropped.fetch_add(1);
        return;
    }

    size_t service_len = 0;
    const esp_err_t service_err = ctx.registry.dispatch(packet.descriptor, request.request,
                                                        ctx.rpc_service_out, service_len);

    // 组装 RpcResponse（成功时带服务返回值，失败时带 error.Error）。
    Bytes error_raw{};
    if (service_err != ESP_OK) {
        const size_t error_len = encode_error_execute("rpc failed", ctx.rpc_error);
        if (error_len != 0) {
            error_raw = Bytes(ctx.rpc_error.data(), error_len);
        }
    }
    size_t body_len = 0;
    if (!encode_rpc_response_checked(
            service_err == ESP_OK ? Bytes(ctx.rpc_service_out.data(), service_len) : Bytes{},
            error_raw, 0, ctx.rpc_body, body_len)) {
        ctx.dropped.fetch_add(1);
        return;
    }

    RpcSendTemplate tmpl{};
    tmpl.from_peer = my_peer_id;
    tmpl.to_peer = packet.from_peer;
    tmpl.transaction_id = packet.transaction_id;
    tmpl.is_request = false;
    tmpl.trace_id = packet.trace_id;
    tmpl.compression.algo = kCompressionNone;
    tmpl.compression.accepted_algo = kCompressionNone;
    (void)send_rpc_message(ctx, generation, tmpl, Bytes(ctx.rpc_body.data(), body_len));
}

/**
 * @brief 处理一个完整的 RPC 响应。
 * @details 响应只带 missing 列表：如果对方缺少我们的信息（例如它比我们更早启动），
 *          必须立即补发一次带拓扑的同步请求；压缩信息为 Zstd 时按不支持处理。
 */
/** @brief 检查虚拟网段是否与物理网卡网段重叠（定义见下方）。 */
bool physical_subnet_overlaps(uint32_t addr, uint8_t prefix) noexcept;

/**
 * @brief 发送一次同步请求（请求内容由 SyncEngine 构造）。
 * @param include_topology 是否携带本节点信息与邻接（首包与补发时为 true）
 */
bool send_sync_request(Context& ctx, uint32_t generation, uint32_t remote_peer_id,
                       bool include_topology, uint32_t now) noexcept {
    const int64_t transaction_id =
        ctx.transactions.allocate(generation, remote_peer_id, now, kRpcTimeoutMs);
    if (transaction_id == 0) {
        return false;
    }
    const size_t request_len =
        ctx.sync.build_request(ctx.sync_request_raw, include_topology, remote_peer_id);
    if (request_len == 0) {
        ctx.dropped.fetch_add(1);
        return false;
    }
    const size_t body_len = encode_rpc_request(
        Bytes(ctx.sync_request_raw.data(), request_len), kRpcTimeoutMs, ctx.sync_rpc_raw);
    if (body_len == 0) {
        ctx.dropped.fetch_add(1);
        return false;
    }

    RpcSendTemplate tmpl{};
    tmpl.from_peer = ctx.my_peer_id.load();
    tmpl.to_peer = remote_peer_id;
    tmpl.transaction_id = transaction_id;
    tmpl.is_request = true;
    tmpl.trace_id = 0;
    tmpl.descriptor.domain_name = ctx.settings.network_name;
    tmpl.descriptor.proto_name = kOspfProtoName;
    tmpl.descriptor.service_name = kOspfServiceName;
    tmpl.descriptor.method_index = kOspfSyncMethodIndex;
    tmpl.compression.algo = kCompressionNone;
    tmpl.compression.accepted_algo = kCompressionNone;
    return send_rpc_message(ctx, generation, tmpl, Bytes(ctx.sync_rpc_raw.data(), body_len));
}

/**
 * @brief OSPF 路由同步服务处理函数（应答方）。
 * @details 合并对端拓扑、收集 missing 列表并返回本节点会话信息；**响应本身不携带
 *          拓扑**，因此应答方还要主动发送自己的同步请求（由主循环触发）。
 */
esp_err_t handle_ospf_sync(void* ctx_ptr, Bytes request, MutBytes response,
                           size_t& response_len) noexcept {
    auto& ctx = *static_cast<Context*>(ctx_ptr);
    response_len = 0;
    const esp_err_t err = ctx.sync.handle_request(request, response, response_len, now_ms());
    if (err != ESP_OK) {
        ET_LOGW(kTag, "cannot handle SyncRouteInfo request: 0x%x", err);
    }
    return err;
}

/**
 * @brief 把引擎当前的虚拟地址落到网卡上（创建或改址），并做物理网段重叠检查。
 */
void sync_netif_address(Context& ctx) noexcept {
    const uint32_t address = ctx.sync.my_ipv4();
    const uint8_t prefix = ctx.sync.my_prefix();
    if (address == 0) {
        return;
    }
    if (physical_subnet_overlaps(address, prefix)) {
        ET_LOGE(kTag, "virtual subnet overlaps the physical LAN, refusing to touch the netif");
        ctx.set_error(ESP_ERR_INVALID_ARG);
        return;
    }
    if (!ctx.netif_created) {
        const esp_err_t err = ctx.virtual_netif.create(address, prefix, ctx.settings.mtu,
                                                       static_cast<void*>(ctx.q_netif_rx),
                                                       &ctx.generation);
        ctx.netif_created = err == ESP_OK;
        if (err != ESP_OK) {
            ctx.set_error(err);
            return;
        }
    } else if (ctx.netif_ip != address || ctx.my_prefix.load() != prefix) {
        const esp_err_t err = ctx.virtual_netif.set_address(address, prefix);
        if (err != ESP_OK) {
            ctx.set_error(err);
            return;
        }
    }
    // 网卡成功采用地址后更新公开状态，必须先于 NetworkReady 回调。
    ctx.my_ipv4.store(address);
    ctx.my_prefix.store(prefix);
    ctx.netif_ip = address;
    (void)ctx.virtual_netif.set_link(true);
}

/**
 * @brief 检查虚拟网段是否与物理网卡网段重叠。
 *
 * @details 虚拟网段与物理局域网重叠会导致"本应走隧道的流量被物理网卡直接发出去"，
 *          因此创建网卡前必须拒绝。重叠判定使用物理 STA 网卡的当前 IP/掩码。
 */
bool physical_subnet_overlaps(uint32_t addr, uint8_t prefix) noexcept {
    esp_netif_t* sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (sta == nullptr) {
        return false;  // 没有物理网卡（例如以太网或尚未初始化）：无法判定，放行
    }
    esp_netif_ip_info_t info{};
    if (esp_netif_get_ip_info(sta, &info) != ESP_OK || info.ip.addr == 0) {
        return false;
    }
    const uint32_t phys_ip = ntohl(info.ip.addr);
    const uint32_t phys_mask = ntohl(info.netmask.addr);
    uint8_t phys_prefix = 0;
    for (uint32_t bit = 0x80000000u; bit != 0 && (phys_mask & bit) != 0; bit >>= 1) {
        ++phys_prefix;
    }
    return ipv4_prefixes_overlap(addr, prefix, phys_ip, phys_prefix);
}

void handle_rpc_response(Context& ctx, uint32_t generation, const RpcPacketRef& packet,
                         Bytes body) noexcept {
    if (!ctx.transactions.complete(packet.transaction_id, generation)) {
        ctx.dropped.fetch_add(1);  // 未知事务或旧代际的迟到响应
        return;
    }
    if (packet.has_compression && packet.compression.algo == kCompressionZstd) {
        ET_LOGE(kTag, "peer responded with zstd compression, unsupported");
        revoke_ready(ctx, ESP_ERR_NOT_SUPPORTED);
        return;
    }
    RpcResponseRef response{};
    if (!decode_rpc_response(body, response)) {
        ctx.dropped.fetch_add(1);
        return;
    }
    if (response.has_error) {
        ET_LOGW(kTag, "rpc response carries an error (%u bytes)",
                static_cast<unsigned>(response.error_raw.size()));
        return;
    }
    bool need_resend = false;
    if (ctx.sync.handle_response(response.response, need_resend) != ESP_OK) {
        ctx.dropped.fetch_add(1);
        return;
    }
    // 对端缺少我们的信息：立即补发（受主循环的最小间隔限制）。
    const uint32_t peer_id = peer_of_generation(ctx, generation);
    if (need_resend && peer_id != kInvalidPeerId) {
        (void)send_sync_request(ctx, generation, peer_id, /*include_topology=*/true, now_ms());
    }
}

}  // namespace

/** @brief 控制面任务主循环。 */
void proto_task(void* arg) noexcept {
    auto& ctx = *static_cast<Context*>(arg);

    HandshakeExpectation expect{};
    expect.network_name = ctx.settings.network_name;
    expect.digest = ctx.digest;
    expect.my_peer_id = ctx.my_peer_id.load();

    // 注册 OSPF 路由同步服务（M2 返回空拓扑，M3 填充真实信息）。
    if (ctx.registry.size() == 0) {
        const RpcServiceId id{ctx.settings.network_name, kOspfProtoName, kOspfServiceName,
                              kOspfSyncMethodIndex};
        if (!ctx.registry.add(id, &handle_ospf_sync, &ctx)) {
            ctx.set_error(ESP_FAIL);
        }
    }

    // 每个连接槽一份会话状态：并发连接多个入口时互不干扰。
    std::array<uint32_t, kMaxConnSlots> session_generation{};  ///< 各槽会话代际（0=无）
    std::array<bool, kMaxConnSlots> handshake_sent{};          ///< 各槽是否已发握手
    std::array<uint32_t, kMaxConnSlots> handshake_deadline{};  ///< 各槽握手截止时间
    /// 各槽的保活状态机：间隔 1/2/4/8/16/32 秒、响应超时 2 秒、连续 5 次失败关闭。
    std::array<Keepalive, kMaxConnSlots> keepalive{};
    std::array<uint64_t, kMaxConnSlots> session_id{};          ///< 各槽会话 id
    std::array<bool, kMaxConnSlots> peer_up{};                 ///< 各槽是否已握手成功
    uint32_t last_sync_ms = 0;  ///< 上次发起路由同步的时间

    /** @brief 把各槽已握手的对端集合同步给 SyncEngine（拓扑的邻接行由此得出）。 */
    const auto publish_direct_peers = [&ctx, &peer_up, &session_generation]() {
        std::array<uint32_t, kMaxConnSlots> peers{};
        size_t count = 0;
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            if (peer_up[i] && session_generation[i] != 0) {
                peers[count++] = ctx.slots[i].peer_id.load();
            }
        }
        ctx.sync.set_direct_peers(peers.data(), count, now_ms());
        ctx.direct_peers.store(count);
    };

    /** @brief 复位某个槽的会话状态（断开/超时/换入口时调用）。 */
    const auto reset_slot = [&ctx, &peer_up, &session_generation, &handshake_sent,
                             &keepalive, &publish_direct_peers](size_t slot) {
        const uint32_t peer_id = ctx.slots[slot].peer_id.load();
        if (peer_up[slot] && peer_id != 0) {
            ctx.sync.on_peer_down(peer_id);
        }
        peer_up[slot] = false;
        session_generation[slot] = 0;
        handshake_sent[slot] = false;
        keepalive[slot].reset(now_ms());
        ctx.slots[slot].peer_id.store(0);
        publish_direct_peers();
    };

    ctx.emit(EventType::Started);  // 在控制面任务中上报，保证回调上下文一致

    while (!ctx.stop_requested.load()) {
        const uint32_t now = now_ms();

        // 1) 应用命令：物理网络就绪状态（由控制面单写 underlay_ready）。
        SmallMessage app{};
        for (size_t n = 0; n < kMaxMessagesPerLoop &&
             xQueueReceive(ctx.q_app_cmd, &app, 0) == pdTRUE; ++n) {
            if (app.kind == static_cast<uint8_t>(AppCommand::UnderlayUp)) {
                ctx.underlay_ready.store(true);
                if (!ctx.sync.peer_up()) {
                    ctx.state.store(State::Connecting);
                }
            } else {
                ctx.underlay_ready.store(false);
                for (size_t i = 0; i < kMaxConnSlots; ++i) {
                    reset_slot(i);
                }
                ctx.state.store(State::WaitingUnderlay);
            }
        }

        // 2) I/O 事件：连接建立/断开。
        SmallMessage evt{};
        for (size_t n = 0; n < kMaxMessagesPerLoop &&
             xQueueReceive(ctx.q_evt, &evt, 0) == pdTRUE; ++n) {
            const size_t slot = evt.slot < kMaxConnSlots ? evt.slot : 0;
            if (evt.kind == static_cast<uint8_t>(IoEvent::Connected)) {
                // 新连接：复位该槽后开始握手（不影响其它槽已建立的会话）。
                reset_slot(slot);
                session_generation[slot] = evt.generation;
                handshake_sent[slot] = false;
                handshake_deadline[slot] = now + kHandshakeTimeoutMs;
                ctx.state.store(State::Connecting);
                ET_LOGI(kTag, "slot %u tcp connected (gen=%u)", static_cast<unsigned>(slot),
                        static_cast<unsigned>(evt.generation));
            } else if (session_generation[slot] == evt.generation) {
                reset_slot(slot);
                ctx.state.store(ctx.underlay_ready.load() ? State::Connecting
                                                          : State::WaitingUnderlay);
            }
        }

        // 3) 收到的控制帧。
        FrameMessage frame{};
        for (size_t n = 0; n < kMaxMessagesPerLoop &&
             xQueueReceive(ctx.q_rx_ctrl, &frame, 0) == pdTRUE; ++n) {
            // 代际全局唯一，因此可以从帧的代际反推它属于哪个连接槽。
            size_t slot = kMaxConnSlots;
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                if (session_generation[i] != 0 && session_generation[i] == frame.generation) {
                    slot = i;
                    break;
                }
            }
            if (slot == kMaxConnSlots) {
                ctx.dropped.fetch_add(1);  // 旧代际/未知会话的数据
                continue;
            }
            PeerManagerHeader hdr{};
            if (!decode_peer_manager_header(Bytes(frame.data.data(), frame.len), hdr)) {
                ctx.dropped.fetch_add(1);
                continue;
            }
            const bool encrypted = (hdr.flags & pm_flags::Encrypted) != 0;
            if ((hdr.flags & pm_flags::Compressed) != 0) {
                ET_LOGE(kTag, "compressed data-plane flags are not supported");
                ctx.set_error(ESP_ERR_NOT_SUPPORTED);
                ctx.dropped.fetch_add(1);
                continue;
            }
            // 传输长度 = PM 头 + 实际 payload（加密时再加 28 字节 AEAD 尾）。
            const size_t expected =
                kPeerManagerHeaderSize + hdr.len + (encrypted ? kAeadTailSize : 0);
            if (frame.len != expected) {
                ctx.dropped.fetch_add(1);
                continue;
            }
            MutBytes payload(frame.data.data() + kPeerManagerHeaderSize,
                             frame.len - kPeerManagerHeaderSize);
            // 只有发往本机（或握手的 to=0）才解密；发往其他节点的帧必须保持密文，
            // 否则转发出去的就是"加密标志 + 明文"，对端会认证失败。
            const bool to_me = (hdr.to_peer_id == ctx.my_peer_id.load() ||
                                hdr.to_peer_id == 0);
            if (encrypted) {
                if (!to_me) {
                    // 非本地目标：交给下面的 RPC 转发分支处理（不解密）。
                } else if (!ctx.settings.enable_encryption) {
                    // 关闭加密时与上游 NullCipher 一致：明文可收，已加密报文直接拒绝。
                    ctx.dropped.fetch_add(1);
                    continue;
                } else if (ctx.control_cipher == nullptr || !ctx.control_cipher->ready() ||
                           !ctx.control_cipher->decrypt_in_place(payload)) {
                    ctx.dropped.fetch_add(1);
                    continue;
                }
            }

            switch (static_cast<PacketType>(hdr.packet_type)) {
                case PacketType::HandShake: {
                    uint32_t remote = 0;
                    const HandshakeResult result =
                        validate_handshake(hdr, payload, expect, remote);
                    if (result != HandshakeResult::Ok) {
                        ET_LOGE(kTag, "handshake rejected: %d", static_cast<int>(result));
                        ctx.set_error(ESP_ERR_INVALID_RESPONSE);
                        request_reconnect(ctx, session_generation[slot]);
                        break;
                    }
                    ctx.slots[slot].peer_id.store(remote);
                    ctx.state.store(State::Syncing);
                    peer_up[slot] = true;
                    keepalive[slot].reset(now);
                    last_sync_ms = 0;  // 下一轮立即发起路由同步
                    session_id[slot] = random_u64();
                    if (session_id[slot] == 0) {
                        session_id[slot] = 1;
                    }
                    ctx.network_ready_reported = false;
                    ctx.sync.on_peer_up(remote, session_id[slot], now);
                    publish_direct_peers();
                    if (ctx.settings.has_static_ipv4) {
                        sync_netif_address(ctx);  // 静态地址：握手后拉起虚拟网卡
                    } else if (ctx.netif_created) {
                        (void)ctx.virtual_netif.set_link(true);
                    }
                    ET_LOGI(kTag, "handshake ok, peer=%u", static_cast<unsigned>(remote));
                    ctx.emit(EventType::PeerUp, remote);
                    break;
                }
                case PacketType::Ping: {
                    // 与上游一致：保留 payload 与 PM from/to，仅把类型改为 Pong。
                    std::array<uint8_t, kPeerManagerHeaderSize + kMaxPlainPayload> pong{};
                    const size_t len = build_pong_packet(hdr, payload, pong);
                    if (len != 0) {
                        (void)send_frame(ctx, session_generation[slot], Bytes(pong.data(), len));
                    } else {
                        ctx.dropped.fetch_add(1);
                    }
                    break;
                }
                case PacketType::Pong: {
                    if (!pong_matches(payload, keepalive[slot].last_seq())) {
                        ctx.dropped.fetch_add(1);
                        break;
                    }
                    keepalive[slot].on_pong(now);
                    break;
                }
                case PacketType::RpcReq:
                case PacketType::RpcResp: {
                    RpcPacketRef packet{};
                    if (!decode_rpc_packet(payload, packet)) {
                        ctx.dropped.fetch_add(1);
                        break;
                    }
                    Bytes body{};
                    const RpcReassembleResult result =
                        ctx.reassembler.feed(session_generation[slot], packet, now, body);
                    if (result == RpcReassembleResult::Rejected) {
                        ctx.set_error(ESP_ERR_INVALID_RESPONSE);
                        ctx.dropped.fetch_add(1);
                        break;
                    }
                    if (result != RpcReassembleResult::Complete) {
                        break;  // 继续等待其余分片
                    }
                    if (packet.is_request) {
                        if (packet.to_peer != ctx.my_peer_id.load()) {
                            // 非本地目标的 RPC：按 overlay 路由转发，绝不当成本地调用。
                            PeerManagerHeader forwarded = hdr;
                            const uint32_t next_hop =
                                ctx.sync.snapshot().lookup_next_hop(packet.to_peer);
                            if (!prepare_forward(forwarded, kForwardCounterLimit) ||
                                next_hop == kInvalidPeerId) {
                                ctx.dropped.fetch_add(1);
                                break;
                            }
                            encode_peer_manager_header(
                                MutBytes(frame.data.data(), kPeerManagerHeaderSize), forwarded);
                            // 交回 I/O 任务，由它按 to_peer 选择连接槽。
                            frame.slot = kAnySlot;
                            frame.to_peer = next_hop;
                            if (xQueueSend(ctx.q_tx, &frame, 0) != pdTRUE) {
                                ctx.dropped.fetch_add(1);
                            }
                            break;
                        }
                        handle_rpc_request(ctx, session_generation[slot], packet, body);
                    } else {
                        handle_rpc_response(ctx, session_generation[slot], packet, body);
                    }
                    break;
                }
                default:
                    // Data 等数据面报文在 M4 实现，这里显式计数而非静默忽略。
                    ctx.dropped.fetch_add(1);
                    break;
            }
        }

        // 4) 逐槽发送握手 / 处理握手超时。
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            if (session_generation[i] == 0 || handshake_sent[i]) {
                continue;
            }
            std::array<uint8_t, kPeerManagerHeaderSize + 256> buf{};
            const size_t len = build_handshake_packet(expect, ctx.my_peer_id.load(), buf);
            if (len == 0) {
                ctx.set_error(ESP_ERR_INVALID_SIZE);
                request_reconnect(ctx, session_generation[i]);
            } else if (send_frame(ctx, session_generation[i], Bytes(buf.data(), len))) {
                handshake_sent[i] = true;
                ctx.state.store(State::Handshaking);
            }
        }
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            if (!handshake_sent[i] || peer_up[i] || session_generation[i] == 0 ||
                !deadline_reached(now, handshake_deadline[i])) {
                continue;
            }
            ET_LOGW(kTag, "slot %u handshake timeout", static_cast<unsigned>(i));
            ctx.set_error(ESP_ERR_TIMEOUT);
            request_reconnect(ctx, session_generation[i]);
            handshake_sent[i] = false;
            session_generation[i] = 0;
            ctx.state.store(State::Connecting);
        }

        // 5) 逐槽保活：发送间隔 1/2/4/8/16/32 秒，响应超时固定 2 秒，
        //    连续 5 次失败即关闭该连接（与上游 PeerConnPinger 一致；入站数据流量不算
        //    存活，只有匹配的 Pong 才清零失败计数）。
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            if (!peer_up[i] || session_generation[i] == 0 || !handshake_sent[i]) {
                continue;
            }
            const KeepaliveAction action = keepalive[i].tick(now);
            if (action == KeepaliveAction::SendPing) {
                std::array<uint8_t, kPeerManagerHeaderSize + 4> ping{};
                const size_t len = build_ping_packet(ctx.my_peer_id.load(),
                                                     ctx.slots[i].peer_id.load(),
                                                     keepalive[i].last_seq(), ping);
                if (len != 0) {
                    (void)send_frame(ctx, session_generation[i], Bytes(ping.data(), len));
                }
            } else if (action == KeepaliveAction::Close) {
                ET_LOGW(kTag, "slot %u lost %u ping responses, closing connection",
                        static_cast<unsigned>(i),
                        static_cast<unsigned>(Keepalive::kMaxConsecutiveFailures));
                ctx.set_error(ESP_ERR_TIMEOUT);
                request_reconnect(ctx, session_generation[i]);
            }
        }

        // 6) 路由同步（双向主动，向**每个**已握手对端各发一次）。
        const bool sync_due = elapsed_ms(now, last_sync_ms) >= kRouteSyncIntervalMs;
        const bool reverse_due = !sync_due && ctx.sync.topology().dirty() &&
                                 elapsed_ms(now, last_sync_ms) >= kMinReverseSyncMs;
        if (sync_due || reverse_due) {
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                if (!peer_up[i] || session_generation[i] == 0) {
                    continue;
                }
                (void)send_sync_request(ctx, session_generation[i], ctx.slots[i].peer_id.load(),
                                        /*include_topology=*/true, now);
            }
            last_sync_ms = now;
        }

        // 6b) 拓扑变化 → 重算路由并发布快照；可达即上报 Ready。
        if (ctx.sync.recompute_if_dirty()) {
            // dhcp 自分配：拓扑变化可能带来新地址或新子网（会再次改动快照）。
            if (ctx.sync.dhcp_enabled() && ctx.sync.peer_up() &&
                ctx.sync.update_dhcp(now, random_u32())) {
                sync_netif_address(ctx);
                last_sync_ms = 0;  // 地址变了：立即重新通告
                (void)ctx.sync.recompute_if_dirty();
            }
            const RouteSnapshot& snapshot = ctx.sync.snapshot();
            ctx.reachable_routes.store(snapshot.reachable_count());
            // 长度 1 邮箱：按值覆盖，数据面取自己的副本。
            (void)xQueueOverwrite(ctx.q_snapshot, &snapshot);
            const size_t reachable = snapshot.reachable_count();
            const bool address_ready = ctx.netif_created && ctx.my_ipv4.load() != 0;
            if (reachable != 0 && address_ready && !ctx.network_ready_reported && ctx.sync.peer_up()) {
                ctx.state.store(State::Ready);
                ctx.network_ready_reported = true;
                ET_LOGI(kTag, "network ready: %u reachable peer(s)",
                        static_cast<unsigned>(reachable));
                ctx.emit(EventType::NetworkReady);
            } else if (reachable == 0 && ctx.network_ready_reported) {
                ctx.network_ready_reported = false;
                ctx.state.store(State::Syncing);
            }
        }

        // 7) 超时回收（事务与不完整的分片重组）。
        ctx.transactions.expire(now);
        ctx.reassembler.expire(now);

        vTaskDelay(kLoopDelayTicks);
    }

    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        reset_slot(i);
    }
    ctx.state.store(State::Stopped);
    ctx.emit(EventType::Stopped);
    (void)xSemaphoreGive(ctx.task_exit);
}

}  // namespace et

/** @brief FreeRTOS 任务入口（C 链接）。 */
extern "C" void et_proto_task_entry(void* arg) { et::proto_task(arg); }

#endif  // ESP_PLATFORM
