/**
 * @file io_task.cpp
 * @brief I/O 任务：所有 socket 的唯一所有者，维护 kMaxConnSlots 个入口连接槽。
 *
 * @details 一次 select 等待全部槽，读取并切帧后按类型投入控制面/数据面队列；连接
 *          断开时上报事件、递增代际并关闭 socket。选路规则见 `frame_sendable`：
 *          带 `to_peer` 的帧按 peer 选槽（不校验代际），带代际的控制帧按代际选槽，
 *          其余任选已连接槽。本任务不做解析、加解密或路由判断。
 */
#include "context.h"

#if defined(ESP_PLATFORM)

#include <cstring>

#include "support/log.hpp"
#include "support/time.hpp"
#include "transport/tcp.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.TASKS.IO";
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

/** @brief xTaskCreate 入口：运行主循环，退出后自删任务，禁止返回 FreeRTOS 包装器。 */
void Context::io_entry(void* arg) noexcept {
    static_cast<Context*>(arg)->run_io_task();
    vTaskDelete(nullptr);
}

/**
 * @brief 计算下一条入口的下标，不修改传入的下标。
 * @param index 当前入口下标。
 * @return 轮换后的下标；入口列表为空时返回 0。
 */
size_t Context::next_peer_index(size_t index) const noexcept {
    const size_t count = settings.peers.size();
    return count > 0 ? (index + 1) % count : 0;
}

/**
 * @brief 投递连接事件给控制面。
 * @note 事件只带槽位与代际：对端 peer id 由控制面自己从 `slots[slot].peer_id`
 *       读取，避免同一条信息有两个真值来源。
 */
void Context::post_event(IoEvent kind, uint8_t slot, uint32_t generation) noexcept {
    SmallMessage msg{};
    msg.kind = static_cast<uint8_t>(kind);
    msg.slot = slot;
    msg.generation = generation;
    if (xQueueSend(q_evt, &msg, 0) != pdTRUE) {
        // 事件队列满：控制面下一轮会发现连接状态变化，这里只记丢弃。
        dropped.fetch_add(1);
    }
}

/** @brief TcpConn 读回调适配器：取回所属实例后转调非静态 post_frame。 */
void Context::post_frame_cb(void* arg, Bytes body) noexcept {
    auto* sink = static_cast<FrameSink*>(arg);
    sink->ctx->post_frame(sink->generation, body);
}

/** @brief 按原子接收会话核对握手权限；拒绝未认证、已断开或已换代的 Data。 */
bool Context::data_generation_authenticated(uint32_t generation) const noexcept {
    if (!connection_enabled.load() || stop_requested.load()) return false;
    for (const ConnSlot& slot : slots) {
        if (slot.accepts_data(generation)) return true;
    }
    return false;
}

/**
 * @brief 把收到的帧投递给控制面或数据面（拷贝进队列消息，不共享缓冲）。
 * @details 只"偷看"PM 头偏移 8 的 packet_type：已通过握手的 Data 给数据面，
 *          握手前的 Data 直接丢弃，其余给控制面。
 *          分流后各自的队列都有独立额度，数据洪泛不会挤掉握手/Ping/RPC。
 */
void Context::post_frame(uint32_t generation, Bytes body) noexcept {
    // 在入队时检查，不能让握手前收到的 Data 等到握手通过后被追认。
    const bool is_data =
        body.size() > 8 && body[8] == static_cast<uint8_t>(PacketType::Data);
    if (is_data && !data_generation_authenticated(generation)) {
        dropped.fetch_add(1);
        return;
    }

    FrameMessage msg{};
    msg.generation = generation;
    msg.len = static_cast<uint16_t>(body.size());
    std::memcpy(msg.data.data(), body.data(), body.size());

    QueueHandle_t target = is_data ? q_rx_data : q_rx_ctrl;
    if (xQueueSend(target, &msg, 0) != pdTRUE) {
        dropped.fetch_add(1);
        ET_LOGW(kTag, "%s rx queue full, dropping frame", is_data ? "data" : "control");
    }
}

/** @brief 选择未被其它槽连接/连接中的入口；相同 host:port 只能占用一个槽。 */
bool Context::select_entry(size_t slot, size_t& index) const noexcept {
    const size_t count = settings.peers.size();
    for (size_t offset = 0; offset < count; ++offset) {
        const size_t candidate = (index + offset) % count;
        const auto& peer = settings.peers[candidate];
        bool occupied = false;
        for (size_t other = 0; other < kMaxConnSlots; ++other) {
            const ConnectState state = slots[other].state();
            if (other == slot || (state != ConnectState::InProgress &&
                                  state != ConnectState::Connected)) continue;
            const auto& used = settings.peers[slots[other].peer_index()];
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
void Context::drop_slot(size_t slot) noexcept {
    ConnSlot& conn_slot = slots[slot];
    const uint32_t generation = conn_slot.generation.load();
    const ConnectState previous = conn_slot.close();
    conn_slot.peer_id.store(0);
    if (previous == ConnectState::Connected) {
        post_event(IoEvent::Disconnected, static_cast<uint8_t>(slot), generation);
    }
    // 轮换入口并指数退避，避免对同一入口高频重试。
    conn_slot.set_peer_index(next_peer_index(conn_slot.peer_index()));
    (void)conn_slot.schedule_retry(now_ms());
}

/**
 * @brief 把待发帧搬进各槽的发送队列（按选路规则）。
 *
 * @details 关键区分：**有槽匹配但暂时写不进去**（发送槽位已满 = 正常背压）必须保留
 *          队头等下一轮，绝不能丢；只有"根本没有任何槽匹配该帧"（目标连接不存在或
 *          代际已过期）才丢弃，否则会阻塞整个队列。
 */
void Context::pump_tx() noexcept {
    FrameMessage tx{};
    while (xQueuePeek(q_tx, &tx, 0) == pdTRUE) {
        bool matched = false;  ///< 是否有槽"认领"了这帧（即使暂时忙）
        bool queued = false;   ///< 是否已成功放进某个槽
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = slots[i];
            if (conn_slot.state() != ConnectState::Connected || conn_slot.connection() == nullptr ||
                !conn_slot.connection()->connected()) {
                continue;
            }
            if (!frame_sendable(tx, conn_slot.generation.load(), conn_slot.peer_id.load())) {
                continue;
            }
            matched = true;
            if (conn_slot.connection()->tx_full()) {
                continue;  // 背压：留给下一轮，不丢帧
            }
            if (!conn_slot.connection()->enqueue(Bytes(tx.data.data(), tx.len))) {
                continue;  // 槽位竞争失败，同样留给下一轮
            }
            queued = true;
            break;
        }
        if (queued) {
            (void)xQueueReceive(q_tx, &tx, 0);  // 已入槽，出队
            continue;
        }
        if (matched) {
            break;  // 有槽认领但暂时忙：保留队头，等可写事件后再搬
        }
        // 没有任何槽匹配：目标连接不存在/代际过期。此时必须丢弃，否则队头会一直
        // 阻塞后面的帧（例如对端断开后残留的同步请求）。
        if (tx.generation != 0 || tx.to_peer != 0) {
            dropped.fetch_add(1);
            (void)xQueueReceive(q_tx, &tx, 0);
            continue;
        }
        break;  // 未指定目标的帧：等任一槽可用
    }
}

/** @brief I/O 任务主循环。 */
void Context::run_io_task() noexcept {
    const size_t peer_count = settings.peers.size();
    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        if (slots[i].connection() == nullptr) {
            ET_LOGE(kTag, "connection slot %u has no TcpConn", static_cast<unsigned>(i));
            (void)xSemaphoreGive(task_exit);
            return;
        }
        slots[i].reset_io(peer_count > 0 ? (i % peer_count) : 0, now_ms());
    }

    bool connection_was_enabled = false;  ///< 上一轮的连接开关（用于检测恢复）

    while (!stop_requested.load()) {
        // 0) 应用重新允许连接：复位退避，按初始节奏（1 秒）尽快重连，而不是等最长 30 秒。
        const bool connection_enabled_now = connection_enabled.load();
        if (connection_enabled_now && !connection_was_enabled) {
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                slots[i].reset_retry(now_ms());
            }
            ET_LOGI(kTag, "connection enabled, reconnecting entry peers");
        }
        connection_was_enabled = connection_enabled_now;

        // 1) 控制面要求某个槽重连（握手失败/超时）。
        SmallMessage cmd{};
        while (xQueueReceive(q_io_cmd, &cmd, 0) == pdTRUE) {
            if (cmd.kind != static_cast<uint8_t>(IoCommand::Reconnect)) {
                continue;
            }
            // 命令带的是该槽的会话代际（全局唯一），据此定位槽位。
            size_t slot = kMaxConnSlots;
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                if (slots[i].generation.load() == cmd.generation &&
                    cmd.generation != 0) {
                    slot = i;
                    break;
                }
            }
            if (slot == kMaxConnSlots) {
                continue;  // 该代际已不是当前会话，忽略
            }
            drop_slot(slot);
            slots[slot].reset_backoff();
        }

        // 2) 应用要求断开：关闭所有会话并等待 connect()。
        if (!connection_enabled.load()) {
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                if (slots[i].state() == ConnectState::Connected ||
                    slots[i].state() == ConnectState::InProgress) {
                    drop_slot(i);
                    slots[i].reset_backoff();
                }
            }
            vTaskDelay(pdMS_TO_TICKS(kSliceDelayMs));
            continue;
        }

        // 3) 逐槽建立连接（独立截止时间退避，不阻塞其它槽的收发与保活）。
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = slots[i];
            if (conn_slot.state() == ConnectState::Connected ||
                conn_slot.state() == ConnectState::InProgress) {
                continue;
            }
            if (i >= peer_count) break;  // 入口少于槽数时让多出的槽闲置，不能取模重复连接
            if (!conn_slot.retry_ready(now_ms())) continue;
            size_t peer_index = conn_slot.peer_index();
            if (!select_entry(i, peer_index)) continue;
            conn_slot.set_peer_index(peer_index);
            const PeerEndpoint& peer = settings.peers[peer_index];
            if (!conn_slot.begin_connect(peer.host, peer.port)) {
                conn_slot.set_peer_index(next_peer_index(peer_index));
                (void)conn_slot.schedule_retry(now_ms());
            }
        }

        // 4) 处理连接中的槽。
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = slots[i];
            if (conn_slot.state() != ConnectState::InProgress) {
                continue;
            }
            const ConnectState st_connect = conn_slot.poll_connect();
            if (st_connect == ConnectState::Failed) {
                conn_slot.set_peer_index(next_peer_index(conn_slot.peer_index()));
                const uint32_t retry_ms = conn_slot.schedule_retry(now_ms());
                ET_LOGW(kTag, "slot %u connect failed, retry in %u ms", static_cast<unsigned>(i),
                        static_cast<unsigned>(retry_ms));
                continue;
            }
            if (st_connect != ConnectState::Connected) {
                continue;
            }
            conn_slot.reset_backoff();
            const uint32_t generation = this->generation.fetch_add(1) + 1;
            conn_slot.prepare_receive(generation);
            conn_slot.generation.store(generation);
            conn_slot.peer_id.store(0);  // 握手成功后由控制面写入
            state.store(State::Connecting);
            post_event(IoEvent::Connected, static_cast<uint8_t>(i), generation);
        }

        // 5) 把待发帧搬进各槽。
        pump_tx();

        // 6) select：同时等待所有已连接槽的可读与可写。
        fd_set rfds;
        fd_set wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        int max_fd = -1;
        bool any_pending = false;
        std::array<bool, kMaxConnSlots> want_write{};
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = slots[i];
            if (conn_slot.state() != ConnectState::Connected || !conn_slot.connection()->connected()) {
                continue;
            }
            const int fd = conn_slot.connection()->fd();
            if (fd == net::kInvalidSocket) {
                continue;
            }
            FD_SET(fd, &rfds);
            want_write[i] = conn_slot.connection()->has_pending_tx();
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
            ConnSlot& conn_slot = slots[i];
            if (conn_slot.state() != ConnectState::Connected || !conn_slot.connection()->connected()) {
                continue;
            }
            const int fd = conn_slot.connection()->fd();
            FrameSink sink{this, conn_slot.generation.load()};

            if (ready > 0 && FD_ISSET(fd, &rfds)) {
                size_t frames = 0;
                if (!conn_slot.connection()->read_available(&sink, &Context::post_frame_cb, frames)) {
                    ET_LOGI(kTag, "slot %u peer connection lost", static_cast<unsigned>(i));
                    drop_slot(i);
                    continue;
                }
            }
            if ((ready > 0 && want_write[i] && FD_ISSET(fd, &wfds)) ||
                conn_slot.connection()->has_pending_tx()) {
                if (!conn_slot.connection()->flush(tx_share)) {
                    ET_LOGW(kTag, "slot %u send failed, dropping connection",
                            static_cast<unsigned>(i));
                    drop_slot(i);
                    continue;
                }
            }

            // 接收缓冲里可能还有完整帧（上一轮帧预算用尽），立刻切分。
            while (conn_slot.connection()->connected() &&
                   conn_slot.connection()->rx_buffered() >= kTcpTunnelHeaderSize) {
                FrameSink more_sink{this, conn_slot.generation.load()};
                size_t frames = 0;
                if (!conn_slot.connection()->read_available(&more_sink, &Context::post_frame_cb, frames)) {
                    drop_slot(i);
                    break;
                }
                if (frames == 0) {
                    break;  // 只剩半帧，等下一轮
                }
            }
        }
    }

    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        ConnSlot& conn_slot = slots[i];
        if (conn_slot.close() == ConnectState::Connected) {
            post_event(IoEvent::Disconnected, static_cast<uint8_t>(i),
                       conn_slot.generation.load());
        }
    }
    (void)xSemaphoreGive(task_exit);
}

}  // namespace et

#endif  // ESP_PLATFORM
