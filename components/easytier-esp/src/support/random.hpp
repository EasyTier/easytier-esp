/**
 * @file random.hpp
 * @brief 随机数接口：目标平台使用硬件随机源，宿主测试使用标准库随机引擎。
 */
#pragma once

#include <cstdint>
#include <span>

namespace et {

/**
 * @brief 用密码学强度的随机字节填充 @p out。
 * @note 目标平台调用 esp_fill_random()，需要物理网络就绪后（WiFi 已取得 IP）
 *       才具备足够熵，因此示例在取得 IP 后才 start() 组件。
 */
void fill_random(std::span<uint8_t> out) noexcept;

/** @brief 生成一个随机 u32。 */
[[nodiscard]] uint32_t random_u32() noexcept;
/** @brief 生成一个随机 u64（用于 RPC 事务 id、peer_route_id 等）。 */
[[nodiscard]] uint64_t random_u64() noexcept;

}  // namespace et
