/**
 * @file keepalive.hpp
 * @brief 保活（Ping/Pong）状态机：发送间隔、响应超时与连续失败关闭。
 *
 * @details 发送间隔按 1/2/4/8/16/32 秒变化（成功一次翻倍，上限 32 秒）；每次 Ping
 *          的响应超时固定 2 秒；连续 5 次失败关闭连接。只有匹配的 Pong 才清零失败
 *          计数，入站数据流量不能掩盖失败的往返。本类只做判定，不碰 socket/任务。
 */
#ifndef EASYTIER_ESP_PEER_KEEPALIVE_HPP_
#define EASYTIER_ESP_PEER_KEEPALIVE_HPP_

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
     * @note 首个 Ping 在 @ref kMinIntervalMs 之后发出（两个调用点原本都传这个值）。
     */
    void reset(uint32_t now_ms) noexcept;

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

#endif
