/**
 * @file bytes.hpp
 * @brief 显式的字节序读写工具：线上字段一律显式读写，绝不映射结构体。
 *
 * @details 线上报文的字段一律通过本文件的函数显式读写，绝不把结构体直接
 * 映射到缓冲区上解释：这样既不会出现未对齐访问，也不会因主机字节序不同而
 * 静默破坏协议。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace et {

/** @brief 只读字节视图。 */
using Bytes = std::span<const uint8_t>;
/** @brief 可写字节视图。 */
using MutBytes = std::span<uint8_t>;

/** @brief 从缓冲区读取小端 u32。 */
[[nodiscard]] constexpr uint32_t get_u32_le(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
           static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

/** @brief 从缓冲区读取小端 u64。 */
[[nodiscard]] constexpr uint64_t get_u64_le(const uint8_t* p) noexcept {
    return static_cast<uint64_t>(get_u32_le(p)) |
           static_cast<uint64_t>(get_u32_le(p + 4)) << 32;
}

/** @brief 从缓冲区读取大端 u32（IPv4 头中的地址字段使用该表示）。 */
[[nodiscard]] constexpr uint32_t get_u32_be(const uint8_t* p) noexcept {
    return static_cast<uint32_t>(p[0]) << 24 | static_cast<uint32_t>(p[1]) << 16 |
           static_cast<uint32_t>(p[2]) << 8 | static_cast<uint32_t>(p[3]);
}

/** @brief 从缓冲区读取大端 u16（IPv4 头的长度/标志字段）。 */
[[nodiscard]] constexpr uint16_t get_u16_be(const uint8_t* p) noexcept {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

/** @brief 向缓冲区写入小端 u32。 */
constexpr void put_u32_le(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

/** @brief 向缓冲区写入小端 u64。 */
constexpr void put_u64_le(uint8_t* p, uint64_t v) noexcept {
    put_u32_le(p, static_cast<uint32_t>(v));
    put_u32_le(p + 4, static_cast<uint32_t>(v >> 32));
}

/**
 * @brief 把字符串视图转换为字节视图。
 * @note 不要求字符串以 '\0' 结尾。
 */
[[nodiscard]] inline Bytes bytes_of(std::string_view s) noexcept {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

}  // namespace et
