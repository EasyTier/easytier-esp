/**
 * @file io_task.cpp
 * @brief I/O 任务：所有 socket 的唯一所有者，维护 kMaxConnSlots 个入口连接槽。
 *
 * @details 一次 select 等待全部槽，读取并切帧后按类型投入控制面/数据面队列；连接
 *          断开时关闭 socket 并上报事件，新 TCP 会话建立时分配代际。选路规则见
 *          `frame_sendable`：
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
 * @note 事件只带槽位与代际；控制面保留已确认会话的对端身份，用它处理断线收尾。
 *       不能从当前槽读取旧身份，因为 I/O 可能已撤权或建立了下一代连接。
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

/** @brief 按原子接收会话核对握手权限；拒绝未认证、已断开或已换代的 Data。 */
bool Context::data_generation_authenticated(uint32_t generation) const noexcept {
    if (!connection_enabled.load() || stop_requested.load()) return false;
    for (const ConnSlot& slot : slots) {
        if (slot.accepts_data(generation)) return true;
    }
    return false;
}

/** @brief TcpConn 读回调适配器：取回所属实例后转调非静态 post_frame。 */
void Context::post_frame_cb(void* arg, Bytes body) noexcept {
    auto* sink = static_cast<FrameSink*>(arg);
    sink->ctx->post_frame(sink->generation, body);
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

/**
 * @brief 从 index 起环形寻找空闲入口；成功时将选中的下标写回 index。
 * @details 正在建连和已经连通的槽都会占用入口，防止重试时两个槽连到同一 host:port。
 *          空闲/失败的槽不占用入口；一轮找不到空位就返回 false，留到下轮再试。
 */
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
 * @details 各槽从同一个实例计数器分配代际，用它区分当前会话：控制面自己
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
    const uint32_t generation = conn_slot.generation();
    // close 会把槽置为 Idle，因此必须用它返回的旧状态判断是否建立过 TCP 会话。
    const ConnectState previous = conn_slot.close();
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
            if (conn_slot.state() != ConnectState::Connected) {
                continue;
            }
            const uint32_t generation = conn_slot.generation();
            if (!frame_sendable(tx, generation, conn_slot.peer_id(generation))) {
                continue;
            }
            matched = true;
            if (conn_slot.connection().tx_full()) {
                continue;  // 背压：留给下一轮，不丢帧
            }
            if (!conn_slot.connection().enqueue(Bytes(tx.data.data(), tx.len))) {
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
            // 命令带的是该实例分配的会话代际；核对当前值，避免旧命令关闭新连接。
            size_t slot = kMaxConnSlots;
            for (size_t i = 0; i < kMaxConnSlots; ++i) {
                if (slots[i].generation() == cmd.generation &&
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
            // 轮换后的候选入口仍要重新去重；被其它槽占用时继续寻找下一个入口。
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
            // TCP 连通后才分配代际。先登记“等待握手”的接收会话，再通知控制面；
            // 此时尚不能接收 Data，控制面验证握手后才会为本代际授权。
            uint32_t generation = this->generation.fetch_add(1) + 1;
            // 0 是“没有会话”的保留值，计数器回绕时必须跳过。
            if (generation == 0) generation = this->generation.fetch_add(1) + 1;
            conn_slot.prepare_receive(generation);
            state.store(State::Connecting);
            post_event(IoEvent::Connected, static_cast<uint8_t>(i), generation);
        }

        // 5) 把待发帧搬进各槽。
        pump_tx();

        // 6) select：一次等待所有已连接槽的可读/可写，是本任务唯一的阻塞点。
        //
        //    每个槽只贡献一个 fd，因此不需要"每连接一个任务"；select 返回后按
        //    FD_ISSET 把就绪事件精确分派回槽。注意集合是**关注列表**而非结果列表，
        //    select 会原地改写传入的集合，所以每轮都必须重新 FD_ZERO + FD_SET，
        //    不能跨轮复用（否则会漏掉上一轮未就绪的 socket）。
        fd_set rfds;  // 读集合：所有已连接槽都登记，等数据、FIN 或 socket 错误
        fd_set wfds;  // 写集合：只为"发送环里确实有积压"的槽登记
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        // select 只扫描 [0, max_fd]，第一个参数要传"最大 fd + 1"；-1 表示无连接可等。
        int max_fd = -1;
        // 是否有槽登记了写事件，决定下面给 select 传 &wfds 还是 nullptr。
        bool any_pending = false;
        // 记录建集合时每个槽的写意图，供 select 返回后判断该槽的可写事件是否
        // 属于本轮等待目标；不能在返回后重查 has_pending_tx()（那时集合已就绪，
        // 两者可能不一致，会去 FD_ISSET 一个没登记过的 fd）。
        std::array<bool, kMaxConnSlots> want_write{};
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = slots[i];
            if (conn_slot.state() != ConnectState::Connected) {
                // 建连中的槽由非阻塞 connect + poll_connect() 轮询推进，不参与 select。
                continue;
            }
            const int fd = conn_slot.connection().fd();
            if (fd == net::kInvalidSocket) {
                // 防御：ConnSlot 保证 Connected 必有有效 fd；真出现也只是跳过本轮，
                // 不能让 -1 进入 FD_SET（lwIP 会静默忽略，但 select 参数会失去意义）。
                continue;
            }
            // 读事件无条件关注：可读可能是正常数据，也可能是 FIN/错误；这些都要交给
            // read_available() 去分辨（返回 false 即断线），这里不做预先判断。
            FD_SET(fd, &rfds);
            // 写事件只在发送环非空时关注。TCP socket 默认几乎总是可写，若无条件登记，
            // select 会立即返回，主循环退化成忙等（并且会饿死 IDLE 任务/触发看门狗）。
            // 真正可写时由 7.2 的 flush() 按预算写出，flush() 内部非阻塞。
            want_write[i] = conn_slot.connection().has_pending_tx();
            if (want_write[i]) {
                FD_SET(fd, &wfds);
                any_pending = true;
            }
            // 维护 select 需要的"最大 fd + 1"；fd 由 lwIP 分配，不保证与槽下标同序，
            // 必须逐槽取最大值。
            if (fd > max_fd) {
                max_fd = fd;
            }
        }

        // 返回值：>0 为就绪 fd 总数，0 为超时，<0 为错误。当前把错误按超时处理：
        // 后面所有 FD_ISSET 都要求 ready > 0，因此 select 出错时（此时集合内容按
        // POSIX 属未定义）不会误用集合，只是退化成 20 ms 一轮的轮询。
        int ready = 0;
        if (max_fd >= 0) {
            // 超时必须用微秒表达，不能经 tick 换算：100 Hz 下 pdMS_TO_TICKS 会把
            // 不足一个 tick 的时长取整成 0，传到 tv_usec 就成了非阻塞的 select。
            // 每一轮都重新构造 timeval，因为 select 可能回写 tv（POSIX 允许）。
            timeval tv{};
            tv.tv_sec = 0;
            tv.tv_usec = static_cast<decltype(tv.tv_usec)>(kSelectTimeoutMs) * 1000;
            // 没有待发帧时给 wfds 传 nullptr 而不是空集合：只等读事件与超时。
            // 异常集合不需要，固定传 nullptr。
            ready = ::select(max_fd + 1, &rfds, any_pending ? &wfds : nullptr, nullptr, &tv);
        } else {
            // 一个已连接槽都没有（首次启动、断线退避中或应用已 disconnect()）：
            // select 无从等待，用延时让出 CPU。kSliceDelayMs 为 20 ms，在 100 Hz 下
            // 至少 2 tick，不会退化成 0 延时忙等；同时保证停止请求仍能及时被检查。
            vTaskDelay(pdMS_TO_TICKS(kSliceDelayMs));
            continue;
        }

        // 7) 逐槽处理可读/可写。
        //    每槽获得均等的写入预算（总量仍有上限），否则第二个槽只能每轮写 1 字节。
        //    kMaxConnSlots 上限 8、kTxBudgetBytes 为 8192，因此 tx_share 至少 1024，
        //    不会因整数除零截断成 0 而永远写不出去。
        const size_t tx_share = kTxBudgetBytes / kMaxConnSlots;
        for (size_t i = 0; i < kMaxConnSlots; ++i) {
            ConnSlot& conn_slot = slots[i];
            if (conn_slot.state() != ConnectState::Connected) {
                // 槽状态只由本任务修改，本轮建集合后不会变化，这里是防御性复查；
                // 保留它可避免将来在本段插入会改状态的逻辑时误用已关闭的槽。
                continue;
            }
            const int fd = conn_slot.connection().fd();
            // 读回调上下文绑定"本槽当前代际"：本次切出的每一帧都归属这一代会话，
            // post_frame() 会用代际核对 Data 是否已通过握手授权（未授权的 Data 丢弃）。
            FrameSink sink{this, conn_slot.generation()};

            // 7.1 读：只在 select 明确报告该 fd 可读时读。
            //     read_available() 返回 false 表示对端关闭、读错误或分帧非法（长度前缀
            //     越界），三者都无法继续使用这条连接，统一关槽并安排重连。
            if (ready > 0 && FD_ISSET(fd, &rfds)) {
                size_t frames = 0;  // 本次切出的完整帧数；帧本身由回调入队，这里不看内容
                if (!conn_slot.connection().read_available(&sink, &Context::post_frame_cb, frames)) {
                    ET_LOGI(kTag, "slot %u peer connection lost", static_cast<unsigned>(i));
                    // drop_slot 内部 close 会立刻让 fd 失效，并上报 Disconnected、轮换
                    // 入口、安排指数退避；必须 continue，不能再用旧 fd 做写或继续切帧。
                    drop_slot(i);
                    continue;
                }
            }
            // 7.2 写：两个触发入口。
            //     前一项是"写事件正常唤醒"：本轮登记过该槽、且 select 报告可写；
            //     后一项是兜底：select 超时（ready == 0）或被其它槽的读事件唤醒时，
            //     只要发送环里还有积压就顺手尝试一次。flush() 是非阻塞的（EAGAIN 直接
            //     返回 true），所以没有可写事件也不会忙等，更不会把"暂时写不动"误判成
            //     "连接坏了"。
            if ((ready > 0 && want_write[i] && FD_ISSET(fd, &wfds)) ||
                conn_slot.connection().has_pending_tx()) {
                if (!conn_slot.connection().flush(tx_share)) {
                    // 写失败（对端复位、管道错误等）同样只能关槽；发送环里未写出的帧
                    // 随连接一起丢弃，本层不做重排/重传，可靠性由上层协议自行保证。
                    ET_LOGW(kTag, "slot %u send failed, dropping connection",
                            static_cast<unsigned>(i));
                    drop_slot(i);
                    continue;
                }
            }

            // 7.3 把接收缓冲里已经完整、但上一轮因"每轮帧数预算"没切完的帧切出来。
            //     read_available() 单次最多处理 kMaxFramesPerRound 帧（保证多连接公平），
            //     剩余字节已经 recv 进本槽缓冲、不在内核里了，不会再触发 select 的可读
            //     事件；若不在这里继续切分，就要等对端再发新数据才会被处理，表现为
            //     莫名的延迟/吞吐下降。循环条件先挡掉两种无意义调用：槽已不 Connected
            //     （read_available 会返回 false 并被当成断线）和不足一个长度前缀。
            while (conn_slot.connection().connected() &&
                   conn_slot.connection().rx_buffered() >= kTcpTunnelHeaderSize) {
                // 每次重新取当前代际而不是复用上面的 sink：保证回调拿到的代际与槽
                // 状态一致；当前实现里断线会直接 drop_slot + break，这里是防御写法。
                FrameSink more_sink{this, conn_slot.generation()};
                size_t frames = 0;
                if (!conn_slot.connection().read_available(&more_sink, &Context::post_frame_cb, frames)) {
                    // 切帧过程中发现非法长度/对端关闭：关槽并退出内层循环，
                    // 外层继续处理下一个槽（不能 break 整个逐槽循环）。
                    drop_slot(i);
                    break;
                }
                if (frames == 0) {
                    break;  // 只剩半帧，等下一轮 select 收齐剩余字节
                }
                // frames > 0 表示本次至少完整消费一帧，rx 缓冲必然缩短，循环会推进，
                // 不会因"反复切出同一帧"而死循环。
            }
        }
    }

    // 8) 退出主循环（stop_requested 已置位）后关闭所有槽，负责最后的事件上报。
    for (size_t i = 0; i < kMaxConnSlots; ++i) {
        ConnSlot& conn_slot = slots[i];
        if (conn_slot.close() == ConnectState::Connected) {
            post_event(IoEvent::Disconnected, static_cast<uint8_t>(i),
                       conn_slot.generation());
        }
    }

    (void)xSemaphoreGive(task_exit);
}

}  // namespace et

#endif  // ESP_PLATFORM
