/**
 * @file random.cpp
 * @brief 随机数实现：ESP-IDF 使用硬件随机源，宿主使用标准库引擎。
 */
#include "support/random.hpp"

#include "support/bytes.hpp"

#if defined(ESP_PLATFORM)
#include "esp_random.h"
#else
#include <random>
#endif

namespace et {

#if defined(ESP_PLATFORM)

/** @brief 目标平台：直接使用 ESP-IDF 的硬件随机源。 */
void fill_random(std::span<uint8_t> out) noexcept {
    if (!out.empty()) {
        esp_fill_random(out.data(), out.size());
    }
}

#else

namespace {

/**
 * @brief 宿主测试用的进程级随机引擎。
 * @note 用 std::random_device 做 8 轮 seed_seq 播种，测试场景足够；生产路径
 *       使用目标平台实现。
 */
std::mt19937_64& host_rng() {
    static std::mt19937_64 rng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }();
    return rng;
}

}  // namespace

/** @brief 宿主平台：按小端填充分块随机字节。 */
void fill_random(std::span<uint8_t> out) noexcept {
    auto& rng = host_rng();
    size_t i = 0;
    while (i < out.size()) {
        const uint64_t word = rng();
        uint8_t tmp[8];
        put_u64_le(tmp, word);
        const size_t n = (out.size() - i < 8) ? out.size() - i : 8;
        for (size_t j = 0; j < n; ++j) {
            out[i + j] = tmp[j];
        }
        i += n;
    }
}

#endif

/** @brief 生成一个随机 u32（小端解释 4 字节）。 */
uint32_t random_u32() noexcept {
    uint8_t buf[4];
    fill_random(buf);
    return get_u32_le(buf);
}

/** @brief 生成一个随机 u64（用于 RPC 事务 id 等）。 */
uint64_t random_u64() noexcept {
    uint8_t buf[8];
    fill_random(buf);
    return get_u64_le(buf);
}

}  // namespace et
