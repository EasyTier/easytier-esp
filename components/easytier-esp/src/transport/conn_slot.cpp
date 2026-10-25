/**
 * @file conn_slot.cpp
 * @brief 连接槽的资源管理、连接状态转换、退避与跨任务接收授权。
 */
#include "transport/conn_slot.hpp"

#include "support/time.hpp"
#include "transport/tcp.hpp"

namespace et {

// TcpConn 带有大块收发缓冲，构造槽时即分配到堆上，槽存活期间始终有效。
ConnSlot::ConnSlot() noexcept : conn_(std::make_unique<TcpConn>()) {}

// TcpConn 的析构函数负责关闭 socket，无需在这里再显式 close 一遍。
ConnSlot::~ConnSlot() noexcept { revoke_receive(); }

void ConnSlot::reset_io(size_t peer_index, uint32_t now) noexcept {
    // 每次启动 I/O 任务都重新安排首次重试，不继承上次运行积累的长退避。
    (void)close();
    generation_.store(0);
    peer_index_ = peer_index;
    reset_retry(now);
}

bool ConnSlot::begin_connect(std::string_view host, uint16_t port) noexcept {
    // 多槽调度只能从空闲/失败状态发起，避免覆盖尚未上报断线的已建立会话。
    if (state_ == ConnectState::InProgress || state_ == ConnectState::Connected) return false;
    // 新连接不能沿用上一次握手的接收权限，即使配置入口没有变化。
    revoke_receive();
    if (!conn_->begin_connect(host, port)) {
        state_ = ConnectState::Failed;
        return false;
    }
    // 即使底层立即连通，也先等 poll_connect 统一确认，再由 I/O 分配代际和上报事件。
    state_ = ConnectState::InProgress;
    return true;
}

ConnectState ConnSlot::poll_connect() noexcept {
    // 只推进正在建立的连接，
    // 保留 Connected 直到 close，以便 I/O 根据旧状态决定是否投递断线事件。
    if (state_ == ConnectState::InProgress) {
        state_ = conn_->poll_connect();
        if (state_ == ConnectState::Connected) reset_backoff();
    }
    return state_;
}

ConnectState ConnSlot::close() noexcept {
    const ConnectState previous = state_;
    // 先撤权，让尚在队列中等待数据面消费的旧 Data 无法继续通过准入检查。
    revoke_receive();
    conn_->close();
    state_ = ConnectState::Idle;
    return previous;
}

bool ConnSlot::retry_ready(uint32_t now) const noexcept {
    // 用支持回绕的截止时间比较，避免毫秒计数器回到 0 后错误地提前/延后重试。
    return deadline_reached(now, retry_at_ms_);
}

uint32_t ConnSlot::schedule_retry(uint32_t now) noexcept {
    // 本次按当前 backoff_ms_ 等待；翻倍后的值留给下一次失败使用。
    retry_at_ms_ = now + backoff_ms_;
    const uint32_t delay = backoff_ms_;
    const uint32_t doubled = backoff_ms_ * 2;
    backoff_ms_ = doubled > kMaxBackoffMs ? kMaxBackoffMs : doubled;
    return delay;
}

void ConnSlot::reset_backoff() noexcept { backoff_ms_ = kInitialBackoffMs; }

void ConnSlot::reset_retry(uint32_t now) noexcept {
    // 与 reset_backoff 不同，这里同时重排截止时间，适用于应用重新允许连接。
    reset_backoff();
    retry_at_ms_ = now + kInitialBackoffMs;
}

uint32_t ConnSlot::generation() const noexcept {
    return generation_.load();
}

uint32_t ConnSlot::peer_id(uint32_t expected_generation) const noexcept {
    // 只读一次快照，不能分别读取代际和身份，否则可能把两次连接的信息拼在一起。
    const uint64_t session = receive_session_.load();
    return expected_generation != 0 && static_cast<uint32_t>(session >> 32) == expected_generation
               ? static_cast<uint32_t>(session) : 0;
}

void ConnSlot::prepare_receive(uint32_t new_generation) noexcept {
    // 在投递 Connected 事件前完整登记新会话，清除旧身份，等待控制面验证握手。
    receive_session_.store(static_cast<uint64_t>(new_generation) << 32);
    generation_.store(new_generation);
}

bool ConnSlot::authenticate_receive(uint32_t expected_generation, uint32_t peer_id) noexcept {
    if (expected_generation == 0 || peer_id == 0) return false;
    uint64_t expected = static_cast<uint64_t>(expected_generation) << 32;
    // 仅允许“本代际等待握手”变为“本代际已授权”。身份随 CAS 一起提交，
    // 不留“授权成功后再写身份”的窗口，避免旧握手覆盖 I/O 刚准备好的新会话。
    return receive_session_.compare_exchange_strong(expected, expected | peer_id);
}

void ConnSlot::revoke_receive(uint32_t expected_generation) noexcept {
    if (expected_generation == 0) {
        receive_session_.store(0);
        return;
    }
    uint64_t current = receive_session_.load();
    // CAS 失败会更新 current：仍为本代际则继续撤销，已换代就停止。
    // 清除整个会话而非只清身份，否则迟到握手会把它误认为仍在等待授权。
    while (static_cast<uint32_t>(current >> 32) == expected_generation &&
           !receive_session_.compare_exchange_weak(current, 0)) {}
}

bool ConnSlot::accepts_data(uint32_t expected_generation) const noexcept {
    return peer_id(expected_generation) != 0;
}

}  // namespace et
