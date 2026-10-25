/**
 * @file siphash.hpp
 * @brief SipHash-1-3（k0 = k1 = 0）实现。
 *
 * @details EasyTier 使用 Rust 的 std::collections::hash_map::DefaultHasher 派生
 * AES 密钥与网络密钥摘要，而 DefaultHasher 就是 k0=k1=0 的 SipHash-1-3。这是
 * Rust 标准库的实现细节，因此算法以上游提交号固定，并用黄金向量做回归卡口。
 *
 * @note finish() 不修改状态，可以连续多次调用后再继续 write()，这与 Rust
 *       Hasher 的语义一致，也是密钥派生算法正确性的前提。
 */
#ifndef EASYTIER_ESP_CRYPTO_SIPHASH_HPP_
#define EASYTIER_ESP_CRYPTO_SIPHASH_HPP_

#include <cstdint>
#include <span>

namespace et {

/**
 * @brief 增量式 SipHash-1-3 计算器。
 *
 * @code
 * et::SipHash13 h;
 * h.write(et::bytes_of("secret"));
 * uint64_t first = h.finish();     // 不改变状态
 * h.write(某 8 字节);
 * uint64_t second = h.finish();
 * @endcode
 */
class SipHash13 {
public:
    /** @brief 构造一个零密钥的 SipHash-1-3 计算器。 */
    SipHash13() noexcept = default;  // k0 = k1 = 0

    /**
     * @brief 追加数据。
     * @param data 待处理字节，允许任意长度（内部按 8 字节压缩，尾部缓存）
     */
    void write(std::span<const uint8_t> data) noexcept;

    /**
     * @brief 计算当前摘要。
     * @return 64 位摘要值
     * @note 不改变内部状态，可重复调用
     */
    [[nodiscard]] uint64_t finish() const noexcept;

private:
    /** @brief SipHash 的一轮混合运算。 */
    void round() noexcept;
    /** @brief 压缩一个 8 字节小端字。 */
    void compress(uint64_t word) noexcept;

    uint64_t v0_ = 0x736f6d6570736575ULL;  ///< 状态字 v0
    uint64_t v1_ = 0x646f72616e646f6dULL;  ///< 状态字 v1
    uint64_t v2_ = 0x6c7967656e657261ULL;  ///< 状态字 v2
    uint64_t v3_ = 0x7465646279746573ULL;  ///< 状态字 v3
    uint64_t tail_ = 0;                    ///< 未满 8 字节的尾部数据
    uint64_t length_ = 0;                  ///< 累计写入字节数
    int ntail_ = 0;                        ///< 尾部已缓存字节数
};

}  // namespace et

#endif
