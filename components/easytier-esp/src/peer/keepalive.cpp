/**
 * @file keepalive.cpp
 * @brief 保活状态机实现（与上游 PeerConnPinger 的参数和语义对齐）。
 */
#include "peer/keepalive.hpp"

namespace et {
namespace {

/** @brief 时间是否已到（处理毫秒回绕）。 */
[[nodiscard]] bool due(uint32_t now_ms, uint32_t target_ms) noexcept {
    return static_cast<int32_t>(now_ms - target_ms) >= 0;
}

}  // namespace

/** @brief 复位状态：间隔回到最小、失败清零。 */
void Keepalive::reset(uint32_t now_ms, uint32_t first_delay_ms) noexcept {
    seq_ = 0;
    interval_ms_ = kMinIntervalMs;
    next_send_ms_ = now_ms + first_delay_ms;
    deadline_ms_ = 0;
    failures_ = 0;
    awaiting_ = false;
    closed_ = false;
}

/** @brief 推进状态机：先判失败，再判是否该发新 Ping。 */
KeepaliveAction Keepalive::tick(uint32_t now_ms) noexcept {
    if (closed_) {
        return KeepaliveAction::None;
    }
    // 1) 未完成的 Ping 超时（固定 2 秒，与发送间隔无关）。
    if (awaiting_ && due(now_ms, deadline_ms_)) {
        awaiting_ = false;
        ++failures_;
        // 出现丢包时立刻回到最小间隔快速探测，便于网络恢复后尽快发现。
        interval_ms_ = kMinIntervalMs;
        next_send_ms_ = now_ms;
        if (failures_ >= kMaxConsecutiveFailures) {
            closed_ = true;
            return KeepaliveAction::Close;
        }
    }
    // 2) 到点则发送新的 Ping（若上一个仍在等待响应，等它超时后再发）。
    if (!awaiting_ && due(now_ms, next_send_ms_)) {
        awaiting_ = true;
        deadline_ms_ = now_ms + kResponseTimeoutMs;
        ++seq_;
        if (seq_ == 0) {
            seq_ = 1;  // 序号 0 保留给"无 Ping"
        }
        return KeepaliveAction::SendPing;
    }
    return KeepaliveAction::None;
}

/** @brief 收到匹配的 Pong：清零失败并放长间隔。 */
void Keepalive::on_pong(uint32_t now_ms) noexcept {
    if (closed_) {
        return;
    }
    awaiting_ = false;
    failures_ = 0;
    if (interval_ms_ < kMaxIntervalMs) {
        interval_ms_ = (interval_ms_ > kMaxIntervalMs / 2) ? kMaxIntervalMs : interval_ms_ * 2;
    }
    next_send_ms_ = now_ms + interval_ms_;
}

}  // namespace et
