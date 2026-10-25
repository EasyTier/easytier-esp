/**
 * @file time.hpp
 * @brief 单调时间戳（毫秒），目标平台用 esp_timer，宿主测试用 std::chrono。
 */
#ifndef EASYTIER_ESP_SUPPORT_TIME_HPP_
#define EASYTIER_ESP_SUPPORT_TIME_HPP_

#include <cstdint>

#if defined(ESP_PLATFORM)
#include "esp_timer.h"
#else
#include <chrono>
#endif

namespace et {

/**
 * @brief 取单调递增的毫秒时间戳。
 * @note 32 位会回绕（约 49.7 天），比较时一律使用无符号差值。
 */
[[nodiscard]] inline uint32_t now_ms() noexcept {
#if defined(ESP_PLATFORM)
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
#else
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
#endif
}

/**
 * @brief 无符号时间差（正确处理 32 位回绕）。
 * @note 仅用于已保证 @p since 不晚于 @p now 的"回绕差值"。判断"记录时间戳是否
 *       超时"请用 @ref expired —— 同一轮任务循环里时间戳可能比比较基准晚几毫秒。
 */
[[nodiscard]] inline uint32_t elapsed_ms(uint32_t now, uint32_t since) noexcept {
    return static_cast<uint32_t>(now - since);
}

/**
 * @brief @p since 之后是否已经超过 @p timeout_ms。
 * @details 用有符号差值：正确处理 32 位回绕；@p since 比 @p now 略晚（例如同一轮
 *          循环里重新取了 `now_ms()`）时返回 false，不会把刚写入的记录判成过期。
 */
[[nodiscard]] inline bool expired(uint32_t now, uint32_t since, uint32_t timeout_ms) noexcept {
    return static_cast<int32_t>(now - since) > static_cast<int32_t>(timeout_ms);
}

/**
 * @brief 判断是否到达（或超过）某个截止时间。
 * @details 用有符号差值比较，因此能正确处理 32 位回绕，并且对"未来 2^31 毫秒
 *          以内"的截止时间都成立。
 */
[[nodiscard]] inline bool deadline_reached(uint32_t now, uint32_t deadline) noexcept {
    return static_cast<int32_t>(now - deadline) >= 0;
}

}  // namespace et

#endif
