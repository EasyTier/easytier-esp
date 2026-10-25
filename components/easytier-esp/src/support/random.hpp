/**
 * @file random.hpp
 * @brief 随机数接口：目标平台使用硬件随机源，宿主测试使用标准库随机引擎。
 */
#ifndef EASYTIER_ESP_SUPPORT_RANDOM_HPP_
#define EASYTIER_ESP_SUPPORT_RANDOM_HPP_

#include <cstdint>
#include <span>

namespace et {

/**
 * @brief 用密码学强度的随机字节填充 @p out。
 * @note 目标平台调用 esp_fill_random()（硬件随机源，不依赖网络状态）；示例在
 *       取得 IP 后才 start() 组件只是为了尽早产生日志，与随机源无关。
 */
void fill_random(std::span<uint8_t> out) noexcept;

/** @brief 生成一个随机 u32。 */
[[nodiscard]] uint32_t random_u32() noexcept;
/** @brief 生成一个随机 u64（用于 RPC 事务 id、peer_route_id 等）。 */
[[nodiscard]] uint64_t random_u64() noexcept;

}  // namespace et

#endif
