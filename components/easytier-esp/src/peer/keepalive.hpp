/**
 * @file keepalive.hpp
 * @brief 保活（Ping/Pong）状态机：发送间隔、响应超时与连续失败关闭。
 *
 * @details 与上游 `PeerConnPinger` 对齐的参数与语义：
 * - 发送间隔按 1/2/4/8/16/32 秒变化（成功一次翻倍，上限 32 秒；直连/监听连接的上游
 *   上限就是 32 秒，只有 TCP 打洞连接才是 1 秒）；
 * - **每次 Ping 的响应超时是固定的 2 秒**（与发送间隔无关）；
 * - **连续 5 次失败即关闭连接**（上游 `loss_counter >= 5`）；
 * - 只有"真正的往返成功"（收到匹配的 Pong）才清零失败计数并放长间隔：**入站数据流量
 *   不能掩盖失败的往返**（上游专门有用例覆盖这一点），因此本类不提供"收到任意报文即
 *   算存活"的接口。
 *
 * 本类只做判定、不碰 socket/任务，因此可以在宿主测试里把时序边界逐个钉住。
 */
#pragma once

#include <cstdint>

namespace et {

/** @brief 保活状态机下一步要做什么。 */
enum class KeepaliveAction : uint8_t {
    None,      ///< 什么都不做
    SendPing,  ///< 到点，应发送一个新的 Ping（序号见 last_seq()）
    Close,     ///< 连续失败达到上限，应关闭该连接
};

/** @brief 单个连接的保活状态机。 */
class Keepalive {
public:
    /** @brief 最小发送间隔（毫秒）。 */
    static constexpr uint32_t kMinIntervalMs = 1000;
    /** @brief 最大发送间隔（毫秒）：直连/监听连接的上游上限。 */
    static constexpr uint32_t kMaxIntervalMs = 32000;
    /** @brief 单次 Ping 的响应超时（毫秒），与发送间隔无关。 */
    static constexpr uint32_t kResponseTimeoutMs = 2000;
    /** @brief 连续失败达到该次数即关闭连接。 */
    static constexpr uint32_t kMaxConsecutiveFailures = 5;

    /**
     * @brief 复位状态。
     * @param now_ms 当前时间（毫秒）
     * @param first_delay_ms 首次 Ping 的延迟；传 0 表示"立即发第一个 Ping"
     */
    void reset(uint32_t now_ms, uint32_t first_delay_ms = 0) noexcept;

    /**
     * @brief 推进状态机。
     * @param now_ms 当前时间（毫秒）
     * @return 需要执行的动作
     * @note 返回 @ref KeepaliveAction::SendPing 时，本类已经记下"等待响应"与 2 秒截止；
     *       调用方用 last_seq() 构造 Ping 即可。
     */
    [[nodiscard]] KeepaliveAction tick(uint32_t now_ms) noexcept;

    /**
     * @brief 收到一个匹配的 Pong：清零失败计数并把间隔翻倍（不超过上限）。
     */
    void on_pong(uint32_t now_ms) noexcept;

    /** @brief 最近一次发送的 Ping 序号（用于匹配 Pong）。 */
    [[nodiscard]] uint32_t last_seq() const noexcept { return seq_; }
    /** @brief 当前发送间隔（毫秒）。 */
    [[nodiscard]] uint32_t interval_ms() const noexcept { return interval_ms_; }
    /** @brief 连续失败次数。 */
    [[nodiscard]] uint32_t failures() const noexcept { return failures_; }
    /** @brief 是否正在等待 Pong。 */
    [[nodiscard]] bool awaiting_pong() const noexcept { return awaiting_; }
    /** @brief 是否已判定关闭。 */
    [[nodiscard]] bool closed() const noexcept { return closed_; }

private:
    uint32_t seq_ = 0;           ///< Ping 序号（从 1 递增）
    uint32_t interval_ms_ = kMinIntervalMs;  ///< 当前发送间隔
    uint32_t next_send_ms_ = 0;  ///< 下次可发送时间
    uint32_t deadline_ms_ = 0;   ///< 未完成 Ping 的响应截止时间
    uint32_t failures_ = 0;      ///< 连续失败次数
    bool awaiting_ = false;      ///< 是否有未完成的 Ping
    bool closed_ = false;        ///< 是否已关闭
};

}  // namespace et
