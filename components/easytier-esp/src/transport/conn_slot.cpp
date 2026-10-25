/**
 * @file conn_slot.cpp
 * @brief ConnSlot 的实现：连接建立、退避重试与接收授权。
 */
#include "transport/conn_slot.hpp"

#include "support/time.hpp"
#include "transport/tcp.hpp"

namespace et {

// TCP 收发缓冲较大，放在堆上由槽独占；把构造放在 .cpp 还能让头文件只前置声明 TcpConn。
ConnSlot::ConnSlot() noexcept : conn_(std::make_unique<TcpConn>()) {}

// 先撤销会话，防止之后到达的帧通过检查；TcpConn 析构时会关闭 socket。
ConnSlot::~ConnSlot() noexcept { revoke_receive(); }

void ConnSlot::reset_io(size_t peer_index, uint32_t now) noexcept {
    // 每次启动都从头开始：丢弃旧代际，按初始退避重试，不沿用上次运行的长退避。
    (void)close();  // 启动阶段不需要断线事件
    generation_.store(0);
    peer_index_ = peer_index;
    reset_retry(now);
}

bool ConnSlot::begin_connect(std::string_view host, uint16_t port) noexcept {
    // 正在建连或已连接的槽不能重复发起，否则会覆盖还没上报断线的会话。
    if (state_ == ConnectState::InProgress || state_ == ConnectState::Connected) return false;
    // 新连接必须重新握手，不能沿用上一会话的 Data 权限。
    revoke_receive();
    if (!conn_->begin_connect(host, port)) {
        state_ = ConnectState::Failed;
        return false;
    }
    // 即使底层立即连上，也统一交给 poll_connect() 确认并发布事件。
    state_ = ConnectState::InProgress;
    return true;
}

ConnectState ConnSlot::poll_connect() noexcept {
    // 只推进 InProgress；Connected 要保留到 close()，I/O 靠它判断是否上报断线。
    if (state_ == ConnectState::InProgress) {
        state_ = conn_->poll_connect();
        if (state_ == ConnectState::Connected) reset_backoff();
    }
    return state_;
}

ConnectState ConnSlot::close() noexcept {
    const ConnectState previous = state_;
    // 先撤销授权再关 socket，队列里还没消费的旧 Data 会立刻被拒绝。
    revoke_receive();
    conn_->close();
    state_ = ConnectState::Idle;
    return previous;
}

bool ConnSlot::retry_ready(uint32_t now) const noexcept {
    // 有符号差值比较，兼容毫秒计数回绕。
    return deadline_reached(now, retry_at_ms_);
}

uint32_t ConnSlot::schedule_retry(uint32_t now) noexcept {
    // 本次按当前时长等待，翻倍后的值留给下次失败。
    retry_at_ms_ = now + backoff_ms_;
    const uint32_t delay = backoff_ms_;
    const uint32_t doubled = backoff_ms_ * 2;
    backoff_ms_ = doubled > kMaxBackoffMs ? kMaxBackoffMs : doubled;
    return delay;
}

void ConnSlot::reset_backoff() noexcept { backoff_ms_ = kInitialBackoffMs; }

void ConnSlot::reset_retry(uint32_t now) noexcept {
    // 除了复位时长，还要把截止时间提前，避免被之前安排的长退避拖住。
    reset_backoff();
    retry_at_ms_ = now + kInitialBackoffMs;
}

uint32_t ConnSlot::generation() const noexcept {
    return generation_.load();
}

uint32_t ConnSlot::peer_id(uint32_t expected_generation) const noexcept {
    // 一次读出完整会话，避免代际和身份来自两次不同的连接。
    const uint64_t session = receive_session_.load();
    return expected_generation != 0 && static_cast<uint32_t>(session >> 32) == expected_generation
               ? static_cast<uint32_t>(session) : 0;
}

void ConnSlot::prepare_receive(uint32_t new_generation) noexcept {
    // 先登记“等待握手”的会话，再发布代际，保证别人看到新代际时还没被授权。
    receive_session_.store(static_cast<uint64_t>(new_generation) << 32);
    generation_.store(new_generation);
}

bool ConnSlot::authenticate_receive(uint32_t expected_generation, uint32_t peer_id) noexcept {
    if (expected_generation == 0 || peer_id == 0) return false;
    // 只有“本代际 + 身份 0”能变成“本代际 + peer_id”；已授权或已换代都会失败。
    uint64_t expected = static_cast<uint64_t>(expected_generation) << 32;
    return receive_session_.compare_exchange_strong(expected, expected | peer_id);
}

void ConnSlot::revoke_receive(uint32_t expected_generation) noexcept {
    if (expected_generation == 0) {
        receive_session_.store(0);
        return;
    }
    uint64_t current = receive_session_.load();
    // 只清指定代际；CAS 失败会刷新 current，换代后立即停止，不会误清新会话。
    while (static_cast<uint32_t>(current >> 32) == expected_generation &&
           !receive_session_.compare_exchange_weak(current, 0)) {}
}

bool ConnSlot::accepts_data(uint32_t expected_generation) const noexcept {
    // 有身份就说明该代际已握手通过。
    return peer_id(expected_generation) != 0;
}

}  // namespace et
