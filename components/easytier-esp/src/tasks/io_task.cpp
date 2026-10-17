/**
 * @file io_task.cpp
 * @brief I/O 任务：所有 socket 的唯一所有者（支持多个并发入口连接）。
 *
 * @details 职责：
 * - 维护 `kMaxConnSlots` 个连接槽，每个槽独立连接一个配置入口；配置里的其它入口
 *   作为轮换备用（无并发要求），失败时轮换并按 1/2/4/.../30 秒退避；
 * - 用一次 select 同时等待所有槽的可读/可写，读取并切分完整帧后按类型投入控制面或
 *   数据面队列；
 * - 帧的选路规则（见 `frame_sendable`）：
 *   * 带 `to_peer` 的帧（数据面/转发的 RPC）：只允许 peer id 匹配的槽，不校验代际
 *     （同一网络密钥下，重连后的密文依然有效，按 peer 选路更稳）；
 *   * 带代际的帧（控制面自己发的握手/Ping/RPC/同步）：只有该代际所属的槽；
 *   * 两者都未指定：任一已连接槽。
 * - 连接断开/物理网络断开时上报事件（带槽位与代际）、递增该槽代际并关闭 socket；
 * - 绝不做解析、加解密或路由判断。
 */
#include "context.h"

#if defined(ESP_PLATFORM)

#include <cstring>

#include "support/log.hpp"
#include "support/time.hpp"
#include "transport/tcp.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.IO";
/// 单轮最多写入的字节数（全部槽共享），避免慢速连接独占 I/O 任务。
constexpr size_t kTxBudgetBytes = 8192;
/// select 超时（毫秒），同时决定停止请求的响应延迟上限。
constexpr uint32_t kSelectTimeoutMs = 20;
/// 无连接可用时的轮询间隔（毫秒）。
constexpr uint32_t kSliceDelayMs = 20;

/** @brief 收到一帧时的回调上下文。 */
struct FrameSink {
    Context* ctx = nullptr;   ///< 组件上下文
    uint32_t generation = 0;  ///< 帧所属的连接代际
};

}  // namespace

/** @brief 把槽的入口下标轮换到下一条（入口数为 0 时保持不变）。 */
void Context::advance_peer_index(const Context& ctx, size_t& index) noexcept {
    const size_t count = ctx.settings.peers.size();
    index = count > 0 ? (index + 1) % count : 0;
}

/**
 * @brief 安排一次重试并指数退避（封顶 @ref kMaxBackoffMs ）。
 * @return 本次实际等待的毫秒数，与 @p st.retry_at_ms 一致，便于日志打印准确值
 */
static uint32_t schedule_retry(SlotState& st) noexcept {
    st.retry_at_ms = now_ms() + st.backoff_ms;
    const uint32_t delay = st.backoff_ms;
    // 先翻倍再钳位，避免出现 32 秒（封顶值必须是 30 秒）。
    const uint32_t doubled = st.backoff_ms * 2;
    st.backoff_ms = doubled > kMaxBackoffMs ? kMaxBackoffMs : doubled;
    return delay;
}

/**
 * @brief 投递连接事件给控制面。
 * @note 事件只带槽位与代际：对端 peer id 由控制面自己从 `ctx.slots[slot].peer_id`
 *       读取，避免同一条信息有两个真值来源。
 */
void Context::post_event(Context& ctx, IoEvent kind, uint8_t slot, uint32_t generation) noexcept {
    SmallMessage msg{};
    msg.kind = static_cast<uint8_t>(kind);
    msg.slot = slot;
    msg.generation = generation;
    if (xQueueSend(ctx.q_evt, &msg, 0) != pdTRUE) {
        // 事件队列满：控制面下一轮会发现连接状态变化，这里只记丢弃。
        ctx.dropped.fetch_add(1);
    }
}

/**
 * @brief 把收到的帧投递给控制面或数据面（拷贝进队列消息，不共享缓冲）。
 * @details 只"偷看"PM 头的 packet_type 一个字节做分流：Data 给数据面，其余给控制面。
 *          分流后各自的队列都有独立额度，数据洪泛不会挤掉握手/Ping/RPC。
 */
void Context::post_frame(void* arg, Bytes body) noexcept {
    auto* sink = static_cast<FrameSink*>(arg);
    FrameMessage msg{};
    msg.generation = sink->generation;
    msg.len = static_cast<uint16_t>(body.size());
    std::memcpy(msg.data.data(), body.data(), body.size());

    // PeerManagerHeader.packet_type 位于偏移 8（from_peer_id + to_peer_id 之后）。
    const bool is_data =
        body.size() > 8 && body[8] == static_cast<uint8_t>(PacketType::Data);
    QueueHandle_t target = is_data ? sink->ctx->q_rx_data : sink->ctx->q_rx_ctrl;
    if (xQueueSend(target, &msg, 0) != pdTRUE) {
        sink->ctx->dropped.fetch_add(1);
        ET_LOGW(kTag, "%s rx queue full, dropping frame", is_data ? "data" : "control");
    }
}

/** @brief 选择未被其它槽连接/连接中的入口；相同 host:port 只能占用一个槽。 */
bool Context::select_entry(const Context& ctx, const std::array<SlotState, kMaxConnSlots>& states,
                   size_t slot, size_t& index) noexcept {
    const size_t count = ctx.settings.peers.size();
    for (size_t offset = 0; offset < count; ++offset) {
        const size_t candidate = (index + offset) % count;
        const auto& peer = ctx.settings.peers[candidate];
        bool occupied = false;
        for (size_t other = 0; other < kMaxConnSlots; ++other) {
            if (other == slot || (!states[other].connecting && !states[other].session_open))
                continue;
            const auto& used = ctx.settings.peers[states[other].peer_index];
            if (used.port == peer.port && used.host == peer.host) {
                occupied = true;
                break;
            }
        }
        if (!occupied) {
            index = candidate;
            return true;
        }
    }
    return false;
}

/**
 * @brief 判断一帧能否通过指定的槽发送。
 * @details 代际由全局计数器分配，因此"代际"本身就唯一标识了一个连接槽：控制面自己
 *          发的帧（握手/Ping/RPC/同步）都带代际，只会走它所属的槽；数据面与转发的
 *          帧带 to_peer，按目标对端选路且不校验代际（同一网络密钥下，重连后的密文
 *          依然有效，按 peer 选路更稳）。
 */
static bool frame_sendable(const FrameMessage& tx, uint32_t slot_generation,
                    uint32_t slot_peer_id) noexcept {
    if (tx.to_peer != 0) {
        return tx.to_peer == slot_peer_id;
    }
    if (tx.generation != 0) {
        return tx.generation == slot_generation;
    }
    return true;  // 未指定：任一已连接槽都可以
}

/** @brief 关闭一个槽并上报断开（仅在确实建立过会话时上报）。 */
void Context::drop_slot(Context& ctx, size_t slot, SlotState& st) noexcept {
    ConnSlot& conn_slot = ctx.slots[slot];
    const uint32_t generation = conn_slot.generation.load();
    if (conn_slot.conn != nullptr) {
        conn_slot.conn->close();
    }
    conn_slot.peer_id.store(0);
    st.connecting = false;
    if (st.session_open) {
        post_event(ctx, IoEvent::Disconnected, static_cast<uint8_t>(slot), generation);
        st.session_open = false;
    }
    // 轮换入口并指数退避，避免对同一入口高频重试。
    advance_peer_index(ctx, st.peer_index);
    (void)schedule_retry(st);
}

/**
 * @brief 把待发帧搬进各槽的发送队列（按选路规则）。
 *
 * @details 关键区分（曾经的缺陷）：**有槽匹配但暂时写不进去**（发送槽位已满 = 正常
 *          背压）必须保留队头等下一轮，绝不能丢；只有"根本没有任何槽匹配该帧"（目标
 *          连接不存在或代际已过期）才丢弃，否则会阻塞整个队列。
 */
void Context::pump_tx(Context& ctx, std::array<SlotState, kMaxConnSlots>& states) noexcept {
    FrameMessage tx{};
    while (xQueuePeek(ctx.q_tx, &tx, 0) == pdTRUE) {
        bool matched = false;  ///< 是否有槽"认领"了这帧（即使暂时忙）
        bool queued = false;   ///< 是否已成功放进某个槽
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = ctx.slots[i];
            if (!states[i].session_open || conn_slot.conn == nullptr ||
                !conn_slot.conn->connected()) {
                continue;
            }
            if (!frame_sendable(tx, conn_slot.generation.load(), conn_slot.peer_id.load())) {
                continue;
            }
            matched = true;
            if (conn_slot.conn->tx_full()) {
                continue;  // 背压：留给下一轮，不丢帧
            }
            if (!conn_slot.conn->enqueue(Bytes(tx.data.data(), tx.len))) {
                continue;  // 槽位竞争失败，同样留给下一轮
            }
            queued = true;
            break;
        }
        if (queued) {
            (void)xQueueReceive(ctx.q_tx, &tx, 0);  // 已入槽，出队
            continue;
        }
        if (matched) {
            break;  // 有槽认领但暂时忙：保留队头，等可写事件后再搬
        }
        // 没有任何槽匹配：目标连接不存在/代际过期。此时必须丢弃，否则队头会一直
        // 阻塞后面的帧（例如对端断开后残留的同步请求）。
        if (tx.generation != 0 || tx.to_peer != 0) {
            ctx.dropped.fetch_add(1);
            (void)xQueueReceive(ctx.q_tx, &tx, 0);
            continue;
        }
        break;  // 未指定目标的帧：等任一槽可用
    }
}

/** @brief I/O 任务主循环。 */
void Context::io_task(void* arg) noexcept {
    auto& ctx = *static_cast<Context*>(arg);
    const size_t peer_count = ctx.settings.peers.size();
    std::array<SlotState, kMaxConnSlots> states{};
    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        if (ctx.slots[i].conn == nullptr) {
            ET_LOGE(kTag, "connection slot %u has no TcpConn", static_cast<unsigned>(i));
            (void)xSemaphoreGive(ctx.task_exit);
            return;
        }
        states[i].peer_index = peer_count > 0 ? (i % peer_count) : 0;
        states[i].retry_at_ms = now_ms() + kInitialBackoffMs;
    }

    bool underlay_was_ready = false;  ///< 上一轮的物理网络状态（用于检测恢复）

    while (!ctx.stop_requested.load()) {
        // 0) 物理网络恢复：复位退避，按初始节奏（1 秒）尽快重连，而不是等最长 30 秒。
        const bool underlay_ready_now = ctx.underlay_ready.load();
        if (underlay_ready_now && !underlay_was_ready) {
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                states[i].backoff_ms = kInitialBackoffMs;
                states[i].retry_at_ms = now_ms() + kInitialBackoffMs;
            }
            ET_LOGI(kTag, "underlay ready, reconnecting entry peers");
        }
        underlay_was_ready = underlay_ready_now;

        // 1) 控制面要求某个槽重连（握手失败/超时）。
        SmallMessage cmd{};
        while (xQueueReceive(ctx.q_io_cmd, &cmd, 0) == pdTRUE) {
            if (cmd.kind != static_cast<uint8_t>(IoCommand::Reconnect)) {
                continue;
            }
            // 命令带的是该槽的会话代际（全局唯一），据此定位槽位。
            size_t slot = kMaxConnSlots;
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                if (ctx.slots[i].generation.load() == cmd.generation &&
                    cmd.generation != 0) {
                    slot = i;
                    break;
                }
            }
            if (slot == kMaxConnSlots) {
                continue;  // 该代际已不是当前会话，忽略
            }
            drop_slot(ctx, slot, states[slot]);
            states[slot].backoff_ms = kInitialBackoffMs;
        }

        // 2) 物理网络未就绪：关闭所有会话并等待。
        if (!ctx.underlay_ready.load()) {
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                if (ctx.slots[i].conn->fd() != net::kInvalidSocket || states[i].connecting) {
                    drop_slot(ctx, i, states[i]);
                    states[i].backoff_ms = kInitialBackoffMs;
                }
                states[i].connecting = false;
            }
            vTaskDelay(pdMS_TO_TICKS(kSliceDelayMs));
            continue;
        }

        // 3) 逐槽建立连接（独立截止时间退避，不阻塞其它槽的收发与保活）。
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = ctx.slots[i];
            SlotState& st = states[i];
            if (conn_slot.conn->connected() || st.connecting) {
                continue;
            }
            if (i >= peer_count) continue;  // 入口少于槽数时让多出的槽闲置，不能取模重复连接
            if (!deadline_reached(now_ms(), st.retry_at_ms)) continue;
            if (!select_entry(ctx, states, i, st.peer_index)) continue;
            const PeerEndpoint& peer = ctx.settings.peers[st.peer_index];
            if (!conn_slot.conn->begin_connect(peer.host, peer.port)) {
                advance_peer_index(ctx, st.peer_index);
                (void)schedule_retry(st);
                continue;
            }
            st.connecting = true;
        }

        // 4) 处理连接中的槽。
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = ctx.slots[i];
            SlotState& st = states[i];
            if (!st.connecting) {
                continue;
            }
            const ConnectState st_connect = conn_slot.conn->poll_connect();
            if (st_connect == ConnectState::Failed) {
                st.connecting = false;
                advance_peer_index(ctx, st.peer_index);
                const uint32_t retry_ms = schedule_retry(st);
                ET_LOGW(kTag, "slot %u connect failed, retry in %u ms", static_cast<unsigned>(i),
                        static_cast<unsigned>(retry_ms));
                continue;
            }
            if (st_connect != ConnectState::Connected) {
                continue;
            }
            st.connecting = false;
            st.backoff_ms = kInitialBackoffMs;
            st.session_open = true;
            const uint32_t generation = ctx.generation.fetch_add(1) + 1;
            conn_slot.generation.store(generation);
            conn_slot.peer_id.store(0);  // 握手成功后由控制面写入
            ctx.state.store(State::Connecting);
            post_event(ctx, IoEvent::Connected, static_cast<uint8_t>(i), generation);
        }

        // 5) 把待发帧搬进各槽。
        pump_tx(ctx, states);

        // 6) select：同时等待所有已连接槽的可读与可写。
        fd_set rfds;
        fd_set wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        int max_fd = -1;
        bool any_pending = false;
        std::array<bool, kMaxConnSlots> want_write{};
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = ctx.slots[i];
            if (!states[i].session_open || !conn_slot.conn->connected()) {
                continue;
            }
            const int fd = conn_slot.conn->fd();
            if (fd == net::kInvalidSocket) {
                continue;
            }
            FD_SET(fd, &rfds);
            want_write[i] = conn_slot.conn->has_pending_tx();
            if (want_write[i]) {
                FD_SET(fd, &wfds);
                any_pending = true;
            }
            if (fd > max_fd) {
                max_fd = fd;
            }
        }

        int ready = 0;
        if (max_fd >= 0) {
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>(kSelectTimeoutMs) * 1000;
            ready = ::select(max_fd + 1, &rfds, any_pending ? &wfds : nullptr, nullptr, &tv);
        } else {
            vTaskDelay(pdMS_TO_TICKS(kSliceDelayMs));
            continue;
        }

        // 7) 逐槽处理可读/可写。
        //    每槽获得均等的写入预算（总量仍有上限），否则第二个槽只能每轮写 1 字节。
        const size_t tx_share = kTxBudgetBytes / kMaxConnSlots;
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = ctx.slots[i];
            SlotState& st = states[i];
            if (!st.session_open || !conn_slot.conn->connected()) {
                continue;
            }
            const int fd = conn_slot.conn->fd();
            FrameSink sink{&ctx, conn_slot.generation.load()};

            if (ready > 0 && FD_ISSET(fd, &rfds)) {
                size_t frames = 0;
                if (!conn_slot.conn->read_available(&sink, &post_frame, frames)) {
                    ET_LOGI(kTag, "slot %u peer connection lost", static_cast<unsigned>(i));
                    drop_slot(ctx, i, st);
                    continue;
                }
            }
            if ((ready > 0 && want_write[i] && FD_ISSET(fd, &wfds)) ||
                conn_slot.conn->has_pending_tx()) {
                if (!conn_slot.conn->flush(tx_share)) {
                    ET_LOGW(kTag, "slot %u send failed, dropping connection",
                            static_cast<unsigned>(i));
                    drop_slot(ctx, i, st);
                    continue;
                }
            }

            // 接收缓冲里可能还有完整帧（上一轮帧预算用尽），立刻切分。
            while (conn_slot.conn->connected() &&
                   conn_slot.conn->rx_buffered() >= kTcpTunnelHeaderSize) {
                FrameSink more_sink{&ctx, conn_slot.generation.load()};
                size_t frames = 0;
                if (!conn_slot.conn->read_available(&more_sink, &post_frame, frames)) {
                    drop_slot(ctx, i, st);
                    break;
                }
                if (frames == 0) {
                    break;  // 只剩半帧，等下一轮
                }
            }
        }
    }

    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        ConnSlot& conn_slot = ctx.slots[i];
        if (conn_slot.conn != nullptr) {
            conn_slot.conn->close();
        }
        if (states[i].session_open) {
            post_event(ctx, IoEvent::Disconnected, static_cast<uint8_t>(i),
                       conn_slot.generation.load());
        }
    }
    (void)xSemaphoreGive(ctx.task_exit);
}

}  // namespace et

#endif  // ESP_PLATFORM
