/**
 * @file conn_slot.cpp
 * @brief 连接槽的资源管理、连接状态转换、退避与跨任务接收授权。
 */
#include "transport/conn_slot.hpp"

#include <new>

#include "support/time.hpp"
#include "transport/tcp.hpp"

namespace et {

ConnSlot::~ConnSlot() noexcept { release(); }

bool ConnSlot::allocate() noexcept {
    if (conn_ == nullptr) conn_ = new (std::nothrow) TcpConn();
    return conn_ != nullptr;
}

void ConnSlot::release() noexcept {
    (void)close();
    delete conn_;
    conn_ = nullptr;
    peer_id.store(0);
    generation.store(0);
    peer_index_ = 0;
    backoff_ms_ = kInitialBackoffMs;
    retry_at_ms_ = 0;
}

void ConnSlot::reset_io(size_t peer_index, uint32_t now) noexcept {
    (void)close();
    peer_index_ = peer_index;
    reset_retry(now);
}

bool ConnSlot::begin_connect(std::string_view host, uint16_t port) noexcept {
    // 多槽调度只能从空闲/失败状态发起，避免覆盖尚未上报断线的已建立会话。
    if (state_ == ConnectState::InProgress || state_ == ConnectState::Connected) return false;
    revoke_receive();
    if (conn_ == nullptr || !conn_->begin_connect(host, port)) {
        state_ = ConnectState::Failed;
        return false;
    }
    // 即使底层立即连通，也先等 poll_connect 统一确认，再由 I/O 分配代际和上报事件。
    state_ = ConnectState::InProgress;
    return true;
}

ConnectState ConnSlot::poll_connect() noexcept {
    if (state_ == ConnectState::InProgress) state_ = conn_->poll_connect();
    return state_;
}

ConnectState ConnSlot::close() noexcept {
    const ConnectState previous = state_;
    revoke_receive();
    if (conn_ != nullptr) conn_->close();
    state_ = ConnectState::Idle;
    return previous;
}

bool ConnSlot::retry_ready(uint32_t now) const noexcept {
    return deadline_reached(now, retry_at_ms_);
}

uint32_t ConnSlot::schedule_retry(uint32_t now) noexcept {
    retry_at_ms_ = now + backoff_ms_;
    const uint32_t delay = backoff_ms_;
    const uint32_t doubled = backoff_ms_ * 2;
    backoff_ms_ = doubled > kMaxBackoffMs ? kMaxBackoffMs : doubled;
    return delay;
}

void ConnSlot::reset_backoff() noexcept { backoff_ms_ = kInitialBackoffMs; }

void ConnSlot::reset_retry(uint32_t now) noexcept {
    reset_backoff();
    retry_at_ms_ = now + kInitialBackoffMs;
}

void ConnSlot::prepare_receive(uint32_t new_generation) noexcept {
    receive_session_.store(static_cast<uint64_t>(new_generation) << 1);
}

bool ConnSlot::authenticate_receive(uint32_t expected_generation) noexcept {
    if (expected_generation == 0) return false;
    uint64_t expected = static_cast<uint64_t>(expected_generation) << 1;
    return receive_session_.compare_exchange_strong(expected, expected | 1u);
}

void ConnSlot::revoke_receive(uint32_t expected_generation) noexcept {
    if (expected_generation == 0) {
        receive_session_.store(0);
        return;
    }
    uint64_t current = receive_session_.load();
    while ((current >> 1) == expected_generation &&
           !receive_session_.compare_exchange_weak(current, 0)) {}
}

bool ConnSlot::accepts_data(uint32_t expected_generation) const noexcept {
    return expected_generation != 0 &&
           receive_session_.load() == ((static_cast<uint64_t>(expected_generation) << 1) | 1u);
}

}  // namespace et
