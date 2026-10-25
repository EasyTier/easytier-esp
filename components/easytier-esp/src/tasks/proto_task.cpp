/**
 * @file proto_task.cpp
 * @brief 控制面任务：握手、保活、RPC 与路由同步；Data 帧由 I/O 直接分流到数据面。
 *
 * @details 状态机由 connect/disconnect 与 I/O 事件推进；RPC 支持分片/乱序重组/事务
 *          匹配，压缩只接受 None（Zstd 视为不支持并上报）。
 */
#include "context.h"

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

constexpr const char* kTag = "ET.TASKSe.PROTO";
/// 控制面循环周期（毫秒）。
constexpr uint32_t kLoopPeriodMs = 5;
// 100 Hz 时 5 ms 被 pdMS_TO_TICKS 截为 0；vTaskDelay(0) 不能让低优先级 IDLE 运行。
constexpr TickType_t kLoopDelayTicks = pdMS_TO_TICKS(kLoopPeriodMs) > 0
    ? pdMS_TO_TICKS(kLoopPeriodMs) : 1;
constexpr size_t kMaxMessagesPerLoop = 8;  ///< 持续入队也必须给定时器和 IDLE 留出机会
static_assert(kLoopDelayTicks > 0);

/// 握手超时（毫秒），与上游一致。
constexpr uint32_t kHandshakeTimeoutMs = 5000;

/// 路由同步间隔（毫秒）。
constexpr uint32_t kRouteSyncIntervalMs = 5000;
/// RPC 事务超时（毫秒）。
constexpr uint32_t kRpcTimeoutMs = 10000;
/// 收到对方拓扑后主动反向宣告的最小间隔（毫秒），避免同步风暴。
constexpr uint32_t kMinReverseSyncMs = 1000;

}  // namespace

/** @brief xTaskCreate 入口：运行主循环，退出后自删任务，禁止返回 FreeRTOS 包装器。 */
void Context::proto_entry(void* arg) noexcept {
    static_cast<Context*>(arg)->run_proto_task();
    vTaskDelete(nullptr);
}

/** @brief 把一帧投递给 I/O 任务发送（带代际，旧代际会被丢弃）。 */
bool Context::send_frame(uint32_t generation, Bytes body) noexcept {
    FrameMessage msg{};
    msg.generation = generation;
    msg.len = static_cast<uint16_t>(body.size());
    std::memcpy(msg.data.data(), body.data(), body.size());
    if (xQueueSend(q_tx, &msg, 0) != pdTRUE) {
        dropped.fetch_add(1);
        return false;
    }
    return true;
}

/**
 * @brief 组一个控制面帧并按需加密后发送。
 * @param encrypt 是否加密（RPC 仅在启用加密且对端是同网络节点时为 true；
 *        握手/Ping/Pong 永不加密）
 * @note PM 头的 len 保存**加密前的明文长度**，这是与上游互通的关键。
 */
bool Context::send_control_frame(uint32_t generation, PacketType type, uint32_t from_peer,
                                 uint32_t to_peer, Bytes plain, bool encrypt) noexcept {
    if (plain.size() > kMaxPlainPayload) {
        dropped.fetch_add(1);
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
        if (control_cipher == nullptr || !control_cipher->ready()) {
            set_error(ESP_ERR_INVALID_STATE);
            return false;
        }
        const size_t written = control_cipher->encrypt_random_nonce(
            plain, MutBytes(rpc_frame.data() + kPeerManagerHeaderSize,
                            rpc_frame.size() - kPeerManagerHeaderSize));
        if (written == 0) {
            set_error(ESP_FAIL);
            dropped.fetch_add(1);
            return false;
        }
        body_len = kPeerManagerHeaderSize + written;
    } else {
        if (kPeerManagerHeaderSize + plain.size() > rpc_frame.size()) {
            dropped.fetch_add(1);
            return false;
        }
        std::memcpy(rpc_frame.data() + kPeerManagerHeaderSize, plain.data(), plain.size());
        body_len = kPeerManagerHeaderSize + plain.size();
    }
    encode_peer_manager_header(MutBytes(rpc_frame.data(), kPeerManagerHeaderSize), hdr);
    return send_frame(generation, Bytes(rpc_frame.data(), body_len));
}

/**
 * @brief 按上游预算分片发送一个 RPC 消息（内容为 RpcRequest/RpcResponse 的编码）。
 * @param content 已编码的服务入参/返回值
 * @param encrypt 是否加密；发往共享节点/外部网络中继时必须为 false（它没有本网络
 *        密钥，上游对 public server 的 RPC 同样保持明文）
 */
bool Context::send_rpc_message(uint32_t generation, const RpcSendTemplate& tmpl,
                               Bytes content, bool encrypt) noexcept {
    RpcPlan plan;
    if (!plan_rpc_slices(tmpl, content.size(), plan)) {
        set_error(ESP_ERR_INVALID_SIZE);
        dropped.fetch_add(1);
        return false;
    }
    for (size_t i = 0; i < plan.count; ++i) {
        const RpcSlice& slice = plan.slices[i];
        const size_t encoded = encode_rpc_piece(
            tmpl, Bytes(content.data() + slice.offset, slice.len),
            static_cast<uint32_t>(plan.count), static_cast<uint32_t>(i), rpc_plain);
        if (encoded == 0) {
            set_error(ESP_ERR_INVALID_SIZE);
            dropped.fetch_add(1);
            return false;
        }
        const PacketType type = tmpl.is_request ? PacketType::RpcReq : PacketType::RpcResp;
        // 只有同网络对端才加密；共享节点无法解密，加密会让路由同步直接失效。
        if (!send_control_frame(generation, type, tmpl.from_peer, tmpl.to_peer,
                                Bytes(rpc_plain.data(), encoded), encrypt)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 要求 I/O 断开并重连某个槽。
 * @param generation 该槽当前的会话代际（I/O 任务据此定位槽位）
 */
void Context::request_reconnect(uint32_t generation) noexcept {
    if (generation != 0) {
        for (ConnSlot& slot : slots) slot.revoke_receive(generation);
    }
    SmallMessage msg{};
    msg.kind = static_cast<uint8_t>(IoCommand::Reconnect);
    msg.generation = generation;
    if (xQueueSend(q_io_cmd, &msg, 0) != pdTRUE) {
        dropped.fetch_add(1);
    }
}

/**
 * @brief 由会话代际反查该连接的对端 peer id。
 * @details 代际由全局计数器分配，因此一个代际唯一对应一个连接槽。
 */
uint32_t Context::peer_of_generation(uint32_t generation) const noexcept {
    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        const uint32_t peer_id = slots[i].peer_id(generation);
        if (peer_id != 0) return peer_id;
    }
    return kInvalidPeerId;
}


/**
 * @brief 撤销 Ready：遇到无法协商的协议不兼容（当前只有对端使用 zstd 压缩）时调用。
 *
 * @details 只设置 `last_error` 不够：节点会停在 `ready` 却学不到路由，接入方无法
 *          判断组网不可用。这里切到 @ref State::Error，并在已上报过 Ready 时补发
 *          一次错误事件（避免每个 Zstd 报文都刷事件）。
 */
void Context::revoke_ready(esp_err_t err) noexcept {
    set_error(err);
    dropped.fetch_add(1);
    if (network_ready_reported) {
        network_ready_reported = false;
        state.store(State::Error);
        emit(EventType::Error);
    }
}

/**
 * @brief 处理一个完整的 RPC 请求（目标为本机）。
 * @param encrypt 响应是否加密；请求方是共享节点/中继时必须为 false。
 */
void Context::handle_rpc_request(uint32_t generation, const RpcPacketRef& packet,
                                Bytes body, bool encrypt) noexcept {
    const uint32_t my_peer_id = this->my_peer_id.load();
    if (packet.to_peer != my_peer_id) {
        // 调用方已按 to_peer 把非本地目标的 RPC 转发走；这里只做防御性丢弃。
        dropped.fetch_add(1);
        return;
    }
    if (!packet.has_descriptor) {
        dropped.fetch_add(1);
        return;
    }
    if (packet.has_compression && packet.compression.algo == kCompressionZstd) {
        ET_LOGE(kTag, "peer used zstd compression, which this profile does not support");
        revoke_ready(ESP_ERR_NOT_SUPPORTED);
        return;
    }
    RpcRequestRef request{};
    if (!decode_rpc_request(body, request)) {
        dropped.fetch_add(1);
        return;
    }

    size_t service_len = 0;
    const esp_err_t service_err = registry.dispatch(packet.descriptor, request.request,
                                                        rpc_service_out, service_len);

    // 组装 RpcResponse（成功时带服务返回值，失败时带 error.Error）。
    Bytes error_raw{};
    if (service_err != ESP_OK) {
        const size_t error_len = encode_error_execute("rpc failed", rpc_error);
        if (error_len != 0) {
            error_raw = Bytes(rpc_error.data(), error_len);
        }
    }
    size_t body_len = 0;
    if (!encode_rpc_response_checked(
            service_err == ESP_OK ? Bytes(rpc_service_out.data(), service_len) : Bytes{},
            error_raw, 0, rpc_body, body_len)) {
        dropped.fetch_add(1);
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
    (void)send_rpc_message(generation, tmpl, Bytes(rpc_body.data(), body_len), encrypt);
}

/** @brief 检查虚拟网段是否与物理网卡网段重叠（定义见下方）。 */
static bool physical_subnet_overlaps(uint32_t addr, uint8_t prefix) noexcept;

/**
 * @brief 发送一次同步请求（请求内容由 SyncEngine 构造）。
 * @param include_topology 是否携带本节点信息与邻接（首包与补发时为 true）
 * @param encrypt 是否加密；目标是共享节点/外部网络中继时必须为 false。
 */
bool Context::send_sync_request(uint32_t generation, uint32_t remote_peer_id,
                               bool include_topology, bool encrypt, uint32_t now) noexcept {
    const int64_t transaction_id =
        transactions.allocate(generation, now, kRpcTimeoutMs);
    if (transaction_id == 0) {
        return false;
    }
    const size_t request_len =
        sync.build_request(sync_request_raw, include_topology, remote_peer_id);
    if (request_len == 0) {
        dropped.fetch_add(1);
        return false;
    }
    const size_t body_len = encode_rpc_request(
        Bytes(sync_request_raw.data(), request_len), kRpcTimeoutMs, sync_rpc_raw);
    if (body_len == 0) {
        dropped.fetch_add(1);
        return false;
    }

    RpcSendTemplate tmpl{};
    tmpl.from_peer = my_peer_id.load();
    tmpl.to_peer = remote_peer_id;
    tmpl.transaction_id = transaction_id;
    tmpl.is_request = true;
    tmpl.trace_id = 0;
    tmpl.descriptor.domain_name = settings.network_name;
    tmpl.descriptor.proto_name = kOspfProtoName;
    tmpl.descriptor.service_name = kOspfServiceName;
    tmpl.descriptor.method_index = kOspfSyncMethodIndex;
    tmpl.compression.algo = kCompressionNone;
    tmpl.compression.accepted_algo = kCompressionNone;
    return send_rpc_message(generation, tmpl, Bytes(sync_rpc_raw.data(), body_len), encrypt);
}

/**
 * @brief OSPF 路由同步服务处理函数（应答方）。
 * @details 合并对端拓扑、收集 missing 列表并返回本节点会话信息；**响应本身不携带
 *          拓扑**，因此应答方还要主动发送自己的同步请求（由主循环触发）。
 */
esp_err_t Context::handle_ospf_sync_cb(void* ctx_ptr, Bytes request, MutBytes response,
                              size_t& response_len) noexcept {
    return static_cast<Context*>(ctx_ptr)->handle_ospf_sync(request, response, response_len);
}

esp_err_t Context::handle_ospf_sync(Bytes request, MutBytes response,
                                    size_t& response_len) noexcept {
    response_len = 0;
    const esp_err_t err = sync.handle_request(request, response, response_len, now_ms());
    if (err != ESP_OK) {
        ET_LOGW(kTag, "cannot handle SyncRouteInfo request: 0x%x", err);
    }
    return err;
}

/**
 * @brief 把引擎当前的虚拟地址落到网卡上（创建或改址），并做物理网段重叠检查。
 */
void Context::sync_netif_address() noexcept {
    const uint32_t address = sync.my_ipv4();
    const uint8_t prefix = sync.my_prefix();
    if (address == 0) {
        return;
    }
    if (physical_subnet_overlaps(address, prefix)) {
        ET_LOGE(kTag, "virtual subnet overlaps the physical LAN, refusing to touch the netif");
        set_error(ESP_ERR_INVALID_ARG);
        return;
    }
    if (!netif_created) {
        const esp_err_t err = virtual_netif.create(address, prefix, settings.mtu,
                                                       static_cast<void*>(q_netif_rx),
                                                       &generation);
        netif_created = err == ESP_OK;
        if (err != ESP_OK) {
            set_error(err);
            return;
        }
    } else if (netif_ip != address || my_prefix.load() != prefix) {
        const esp_err_t err = virtual_netif.set_address(address, prefix);
        if (err != ESP_OK) {
            set_error(err);
            return;
        }
    }
    // 网卡成功采用地址后更新公开状态，必须先于 NetworkReady 回调。
    my_ipv4.store(address);
    my_prefix.store(prefix);
    netif_ip = address;
    (void)virtual_netif.set_link(true);
}

/**
 * @brief 检查虚拟网段是否与物理网卡网段重叠。
 *
 * @details 虚拟网段与物理局域网重叠会导致"本应走隧道的流量被物理网卡直接发出去"，
 *          因此创建网卡前必须拒绝。重叠判定使用物理 STA 网卡的当前 IP/掩码。
 */
static bool physical_subnet_overlaps(uint32_t addr, uint8_t prefix) noexcept {
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

/**
 * @brief 处理一个完整的 RPC 响应。
 * @details 响应只带 missing 列表：如果对方缺少我们的信息（例如它比我们更早启动），
 *          必须立即补发一次带拓扑的同步请求；压缩信息为 Zstd 时按不支持处理。
 * @param encrypt 补发的同步请求是否加密（目标为共享节点时必须为 false）。
 */
void Context::handle_rpc_response(uint32_t generation, const RpcPacketRef& packet,
                                 Bytes body, bool encrypt) noexcept {
    if (!transactions.complete(packet.transaction_id, generation)) {
        dropped.fetch_add(1);  // 未知事务或旧代际的迟到响应
        return;
    }
    if (packet.has_compression && packet.compression.algo == kCompressionZstd) {
        ET_LOGE(kTag, "peer responded with zstd compression, unsupported");
        revoke_ready(ESP_ERR_NOT_SUPPORTED);
        return;
    }
    RpcResponseRef response{};
    if (!decode_rpc_response(body, response)) {
        dropped.fetch_add(1);
        return;
    }
    if (response.has_error) {
        ET_LOGW(kTag, "rpc response carries an error (%u bytes)",
                static_cast<unsigned>(response.error_raw.size()));
        return;
    }
    bool need_resend = false;
    if (sync.handle_response(response.response, need_resend) != ESP_OK) {
        dropped.fetch_add(1);
        return;
    }
    // 对端缺少我们的信息：立即补发（受主循环的最小间隔限制）。
    const uint32_t peer_id = peer_of_generation(generation);
    if (need_resend && peer_id != kInvalidPeerId) {
        (void)send_sync_request(generation, peer_id, /*include_topology=*/true, encrypt,
                                now_ms());
    }
}

/** @brief 控制面任务主循环。 */
void Context::run_proto_task() noexcept {

    HandshakeExpectation expect{};
    expect.network_name = settings.network_name;
    expect.digest = digest;
    expect.my_peer_id = my_peer_id.load();

    // 注册 OSPF 路由同步服务（由 SyncEngine 合并拓扑并返回本机会话信息）。
    if (registry.size() == 0) {
        const RpcServiceId id{settings.network_name, kOspfProtoName, kOspfServiceName,
                              kOspfSyncMethodIndex};
        if (!registry.add(id, &Context::handle_ospf_sync_cb, this)) {
            set_error(ESP_FAIL);
        }
    }

    // 每个连接槽一份会话状态：并发连接多个入口时互不干扰。
    std::array<uint32_t, kMaxConnSlots> session_generation{};  ///< 各槽会话代际（0=无）
    std::array<bool, kMaxConnSlots> handshake_sent{};          ///< 各槽是否已发握手
    std::array<uint32_t, kMaxConnSlots> handshake_deadline{};  ///< 各槽握手截止时间
    /// 各槽的保活状态机：间隔 1/2/4/8/16/32 秒、响应超时 2 秒、连续 5 次失败关闭。
    std::array<Keepalive, kMaxConnSlots> keepalive{};
    std::array<uint64_t, kMaxConnSlots> session_id{};          ///< 各槽会话 id
    // 控制面持有自己已确认的身份；即使 I/O 已撤权/换代，仍能正确撤销旧路由。
    std::array<uint32_t, kMaxConnSlots> session_peer_id{};     ///< 对端身份，0 表示尚未握手
    /// 各槽对端是否为共享节点/外部网络中继（其网络名与本端不同，没有本网络密钥）。
    std::array<bool, kMaxConnSlots> session_shared_node{};
    uint32_t last_sync_ms = 0;  ///< 上次发起路由同步的时间

    /** @brief 把各槽已握手的对端集合同步给 SyncEngine（拓扑的邻接行由此得出）。 */
    const auto publish_direct_peers = [this, &session_peer_id]() {
        std::array<uint32_t, kMaxConnSlots> peers{};
        size_t count = 0;
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            if (session_peer_id[i] != 0) {
                peers[count++] = session_peer_id[i];
            }
        }
        sync.set_direct_peers(peers.data(), count, now_ms());
        direct_peers.store(count);
    };

    /** @brief 复位某个槽的会话状态（断开/超时/换入口时调用）。 */
    const auto reset_slot = [this, &session_peer_id, &session_generation, &session_shared_node,
                             &handshake_sent, &keepalive, &publish_direct_peers](size_t slot) {
        const uint32_t generation = session_generation[slot];
        if (generation != 0) slots[slot].revoke_receive(generation);
        const uint32_t peer_id = session_peer_id[slot];
        if (peer_id != 0) {
            sync.on_peer_down(peer_id);
        }
        session_peer_id[slot] = 0;
        session_shared_node[slot] = false;
        session_generation[slot] = 0;
        handshake_sent[slot] = false;
        keepalive[slot].reset(now_ms());
        publish_direct_peers();
    };

    emit(EventType::Started);  // 在控制面任务中上报，保证回调上下文一致

    while (!stop_requested.load()) {
        const uint32_t now = now_ms();

        // 0) 自身信息心跳（对齐上游 3600s）：版本变化后立即触发一次同步通告。
        if (sync.refresh_self_info(now)) {
            last_sync_ms = 0;
        }

        // 1) 应用命令：连接开关（由控制面单写 connection_enabled）。
        SmallMessage app{};
        for (size_t n = 0; n < kMaxMessagesPerLoop &&
             xQueueReceive(q_app_cmd, &app, 0) == pdTRUE; ++n) {
            if (app.kind == static_cast<uint8_t>(AppCommand::Connect)) {
                connection_enabled.store(true);
                if (!sync.peer_up()) {
                    state.store(State::Connecting);
                }
            } else {
                connection_enabled.store(false);
                for (size_t i = 0; i < kMaxConnSlots; ++i) {
                    reset_slot(i);
                }
                state.store(State::WaitingUnderlay);
            }
        }

        // 2) I/O 事件：连接建立/断开。
        SmallMessage evt{};
        for (size_t n = 0; n < kMaxMessagesPerLoop &&
             xQueueReceive(q_evt, &evt, 0) == pdTRUE; ++n) {
            const size_t slot = evt.slot < kMaxConnSlots ? evt.slot : 0;
            if (evt.kind == static_cast<uint8_t>(IoEvent::Connected)) {
                // 新连接：复位该槽后开始握手（不影响其它槽已建立的会话）。
                reset_slot(slot);
                session_generation[slot] = evt.generation;
                handshake_sent[slot] = false;
                handshake_deadline[slot] = now + kHandshakeTimeoutMs;
                state.store(State::Connecting);
                ET_LOGI(kTag, "slot %u tcp connected (gen=%u)", static_cast<unsigned>(slot),
                        static_cast<unsigned>(evt.generation));
            } else if (session_generation[slot] == evt.generation) {
                reset_slot(slot);
                state.store(connection_enabled.load() ? State::Connecting
                                                          : State::WaitingUnderlay);
            }
        }

        // 3) 收到的控制帧。
        FrameMessage frame{};
        for (size_t n = 0; n < kMaxMessagesPerLoop &&
             xQueueReceive(q_rx_ctrl, &frame, 0) == pdTRUE; ++n) {
            // 代际全局唯一，因此可以从帧的代际反推它属于哪个连接槽。
            size_t slot = kMaxConnSlots;
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                if (session_generation[i] != 0 && session_generation[i] == frame.generation) {
                    slot = i;
                    break;
                }
            }
            if (slot == kMaxConnSlots) {
                dropped.fetch_add(1);  // 旧代际/未知会话的数据
                continue;
            }
            PeerManagerHeader hdr{};
            if (!decode_peer_manager_header(Bytes(frame.data.data(), frame.len), hdr)) {
                dropped.fetch_add(1);
                continue;
            }
            const bool encrypted = (hdr.flags & pm_flags::Encrypted) != 0;
            if ((hdr.flags & pm_flags::Compressed) != 0) {
                ET_LOGE(kTag, "compressed data-plane flags are not supported");
                set_error(ESP_ERR_NOT_SUPPORTED);
                dropped.fetch_add(1);
                continue;
            }
            // 传输长度 = PM 头 + 实际 payload（加密时再加 28 字节 AEAD 尾）。
            const size_t expected =
                kPeerManagerHeaderSize + hdr.len + (encrypted ? kAeadTailSize : 0);
            if (frame.len != expected) {
                dropped.fetch_add(1);
                continue;
            }
            MutBytes payload(frame.data.data() + kPeerManagerHeaderSize,
                             frame.len - kPeerManagerHeaderSize);
            // 只有发往本机（或握手的 to=0）才解密；发往其他节点的帧必须保持密文，
            // 否则转发出去的就是"加密标志 + 明文"，对端会认证失败。
            const bool to_me = (hdr.to_peer_id == my_peer_id.load() ||
                                hdr.to_peer_id == 0);
            if (encrypted) {
                if (!to_me) {
                    // 非本地目标：交给下面的 RPC 转发分支处理（不解密）。
                } else if (!settings.enable_encryption) {
                    // 关闭加密时与上游 NullCipher 一致：明文可收，已加密报文直接拒绝。
                    dropped.fetch_add(1);
                    continue;
                } else if (control_cipher == nullptr || !control_cipher->ready() ||
                           !control_cipher->decrypt_in_place(payload)) {
                    dropped.fetch_add(1);
                    continue;
                }
            }

            switch (static_cast<PacketType>(hdr.packet_type)) {
                case PacketType::HandShake: {
                    uint32_t remote = 0;
                    const HandshakeResult result =
                        validate_handshake(hdr, payload, expect, remote);
                    if (result != HandshakeResult::Ok &&
                        result != HandshakeResult::SharedNode) {
                        if (result == HandshakeResult::DigestMismatch) {
                            ET_LOGE(kTag,
                                    "handshake rejected: digest mismatch (wrong network secret, "
                                    "or shared node uses the same network name)");
                        } else {
                            ET_LOGE(kTag, "handshake rejected: %d", static_cast<int>(result));
                        }
                        set_error(ESP_ERR_INVALID_RESPONSE);
                        request_reconnect(session_generation[slot]);
                        break;
                    }
                    // 用一次 CAS 核对代际、登记身份并授权；断线/换代后的迟到握手不能放行 Data。
                    if (!slots[slot].authenticate_receive(session_generation[slot], remote)) {
                        dropped.fetch_add(1);
                        break;
                    }
                    state.store(State::Syncing);
                    session_peer_id[slot] = remote;
                    // 网络名不同 = 共享节点/外部网络中继：它没有本网络密钥，RPC 必须保持明文。
                    session_shared_node[slot] = (result == HandshakeResult::SharedNode);
                    keepalive[slot].reset(now);
                    last_sync_ms = 0;  // 下一轮立即发起路由同步
                    session_id[slot] = random_u64();
                    if (session_id[slot] == 0) {
                        session_id[slot] = 1;
                    }
                    network_ready_reported = false;
                    sync.on_peer_up(remote, session_id[slot], now);
                    publish_direct_peers();
                    if (settings.has_static_ipv4) {
                        sync_netif_address();  // 静态地址：握手后拉起虚拟网卡
                    } else if (netif_created) {
                        (void)virtual_netif.set_link(true);
                    }
                    if (session_shared_node[slot]) {
                        ET_LOGI(kTag, "shared node handshake ok, peer=%u (relay)",
                                static_cast<unsigned>(remote));
                    } else {
                        ET_LOGI(kTag, "handshake ok, peer=%u", static_cast<unsigned>(remote));
                    }
                    emit(EventType::PeerUp, remote);
                    break;
                }
                case PacketType::Ping: {
                    // 与上游一致：保留 payload 与 PM from/to，仅把类型改为 Pong。
                    std::array<uint8_t, kPeerManagerHeaderSize + kMaxPlainPayload> pong{};
                    const size_t len = build_pong_packet(hdr, payload, pong);
                    if (len != 0) {
                        (void)send_frame(session_generation[slot], Bytes(pong.data(), len));
                    } else {
                        dropped.fetch_add(1);
                    }
                    break;
                }
                case PacketType::Pong: {
                    if (!pong_matches(payload, keepalive[slot].last_seq())) {
                        dropped.fetch_add(1);
                        break;
                    }
                    keepalive[slot].on_pong(now);
                    break;
                }
                case PacketType::RpcReq:
                case PacketType::RpcResp: {
                    RpcPacketRef packet{};
                    if (!decode_rpc_packet(payload, packet)) {
                        dropped.fetch_add(1);
                        break;
                    }
                    Bytes body{};
                    const RpcReassembleResult result =
                        reassembler.feed(session_generation[slot], packet, now, body);
                    if (result == RpcReassembleResult::Rejected) {
                        set_error(ESP_ERR_INVALID_RESPONSE);
                        dropped.fetch_add(1);
                        break;
                    }
                    if (result != RpcReassembleResult::Complete) {
                        break;  // 继续等待其余分片
                    }
                    // 本地发起的 RPC 只在同网络对端上加密；共享节点没有本网络密钥。
                    const bool rpc_encrypt =
                        settings.enable_encryption && !session_shared_node[slot];
                    if (packet.is_request) {
                        if (packet.to_peer != my_peer_id.load()) {
                            // 非本地目标的 RPC：按 overlay 路由转发，绝不当成本地调用。
                            PeerManagerHeader forwarded = hdr;
                            const uint32_t next_hop =
                                sync.snapshot().lookup_next_hop(packet.to_peer);
                            if (!prepare_forward(forwarded, kForwardCounterLimit) ||
                                next_hop == kInvalidPeerId) {
                                dropped.fetch_add(1);
                                break;
                            }
                            encode_peer_manager_header(
                                MutBytes(frame.data.data(), kPeerManagerHeaderSize), forwarded);
                            // 交回 I/O 任务，由它按 to_peer 选择连接槽。
                            frame.to_peer = next_hop;
                            if (xQueueSend(q_tx, &frame, 0) != pdTRUE) {
                                dropped.fetch_add(1);
                            }
                            break;
                        }
                        handle_rpc_request(session_generation[slot], packet, body, rpc_encrypt);
                    } else {
                        handle_rpc_response(session_generation[slot], packet, body, rpc_encrypt);
                    }
                    break;
                }
                default:
                    // 未知/不该出现在控制面的报文显式计数，而非静默忽略。
                    dropped.fetch_add(1);
                    break;
            }
        }

        // 4) 逐槽发送握手 / 处理握手超时。
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            if (session_generation[i] == 0 || handshake_sent[i]) {
                continue;
            }
            std::array<uint8_t, kPeerManagerHeaderSize + 256> buf{};
            const size_t len = build_handshake_packet(expect, my_peer_id.load(), buf);
            if (len == 0) {
                set_error(ESP_ERR_INVALID_SIZE);
                request_reconnect(session_generation[i]);
            } else if (send_frame(session_generation[i], Bytes(buf.data(), len))) {
                handshake_sent[i] = true;
                state.store(State::Handshaking);
            }
        }
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            if (!handshake_sent[i] || session_peer_id[i] != 0 || session_generation[i] == 0 ||
                !deadline_reached(now, handshake_deadline[i])) {
                continue;
            }
            ET_LOGW(kTag, "slot %u handshake timeout", static_cast<unsigned>(i));
            set_error(ESP_ERR_TIMEOUT);
            request_reconnect(session_generation[i]);
            handshake_sent[i] = false;
            session_shared_node[i] = false;
            session_generation[i] = 0;
            state.store(State::Connecting);
        }

        // 5) 逐槽保活：发送间隔 1/2/4/8/16/32 秒，响应超时固定 2 秒，
        //    连续 5 次失败即关闭该连接（与上游 PeerConnPinger 一致；入站数据流量不算
        //    存活，只有匹配的 Pong 才清零失败计数）。
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            if (session_peer_id[i] == 0 || session_generation[i] == 0 || !handshake_sent[i]) {
                continue;
            }
            const KeepaliveAction action = keepalive[i].tick(now);
            if (action == KeepaliveAction::SendPing) {
                std::array<uint8_t, kPeerManagerHeaderSize + 4> ping{};
                const size_t len = build_ping_packet(my_peer_id.load(),
                                                     session_peer_id[i],
                                                     keepalive[i].last_seq(), ping);
                if (len != 0) {
                    (void)send_frame(session_generation[i], Bytes(ping.data(), len));
                }
            } else if (action == KeepaliveAction::Close) {
                ET_LOGW(kTag, "slot %u lost %u ping responses, closing connection",
                        static_cast<unsigned>(i),
                        static_cast<unsigned>(Keepalive::kMaxConsecutiveFailures));
                set_error(ESP_ERR_TIMEOUT);
                request_reconnect(session_generation[i]);
            }
        }

        // 6) 路由同步（双向主动，向**每个**已握手对端各发一次）。
        const bool sync_due = elapsed_ms(now, last_sync_ms) >= kRouteSyncIntervalMs;
        const bool reverse_due = !sync_due && sync.topology().dirty() &&
                                 elapsed_ms(now, last_sync_ms) >= kMinReverseSyncMs;
        if (sync_due || reverse_due) {
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                if (session_peer_id[i] == 0 || session_generation[i] == 0) {
                    continue;
                }
                (void)send_sync_request(
                    session_generation[i], session_peer_id[i], /*include_topology=*/true,
                    settings.enable_encryption && !session_shared_node[i], now);
            }
            last_sync_ms = now;
        }

        // 6b) 拓扑变化 → 重算路由并发布快照；可达即上报 Ready。
        if (sync.recompute_if_dirty()) {
            // dhcp 自分配：拓扑变化可能带来新地址或新子网（会再次改动快照）。
            if (sync.dhcp_enabled() && sync.peer_up() &&
                sync.update_dhcp(now, random_u32())) {
                sync_netif_address();
                last_sync_ms = 0;  // 地址变了：立即重新通告
                (void)sync.recompute_if_dirty();
            }
            const RouteSnapshot& snapshot = sync.snapshot();
            reachable_routes.store(snapshot.reachable_count());
            // 长度 1 邮箱：按值覆盖，数据面取自己的副本。
            (void)xQueueOverwrite(q_snapshot, &snapshot);
            const size_t reachable = snapshot.reachable_count();
            const bool address_ready = netif_created && my_ipv4.load() != 0;
            if (reachable != 0 && address_ready && !network_ready_reported && sync.peer_up()) {
                state.store(State::Ready);
                network_ready_reported = true;
                ET_LOGI(kTag, "network ready: %u reachable peer(s)",
                        static_cast<unsigned>(reachable));
                emit(EventType::NetworkReady);
            } else if (reachable == 0 && network_ready_reported) {
                network_ready_reported = false;
                state.store(State::Syncing);
            }
        }

        // 7) 超时回收（事务、不完整的分片重组与拓扑清理）。
        transactions.expire(now);
        reassembler.expire(now);
        sync.expire_topology(now);

        vTaskDelay(kLoopDelayTicks);
    }

    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        reset_slot(i);
    }
    state.store(State::Stopped);
    emit(EventType::Stopped);
    (void)xSemaphoreGive(task_exit);
}

}  // namespace et

#endif  // ESP_PLATFORM
