/**
 * @file siphash.cpp
 * @brief SipHash-1-3 实现（算法对照 SipHash 参考实现与 Rust std 的 SipHasher13）。
 */
#include "crypto/siphash.hpp"

namespace et {
namespace {

/** @brief 64 位循环左移。 */
[[nodiscard]] constexpr uint64_t rotl64(uint64_t x, int b) noexcept {
    return (x << b) | (x >> (64 - b));
}

}  // namespace

/** @brief 执行一轮 SipHash 混合。 */
void SipHash13::round() noexcept {
    v0_ += v1_;
    v1_ = rotl64(v1_, 13);
    v1_ ^= v0_;
    v0_ = rotl64(v0_, 32);
    v2_ += v3_;
    v3_ = rotl64(v3_, 16);
    v3_ ^= v2_;
    v0_ += v3_;
    v3_ = rotl64(v3_, 21);
    v3_ ^= v0_;
    v2_ += v1_;
    v1_ = rotl64(v1_, 17);
    v1_ ^= v2_;
    v2_ = rotl64(v2_, 32);
}

/** @brief 压缩一个 8 字节小端字（1 轮）。 */
void SipHash13::compress(uint64_t w) noexcept {
    v3_ ^= w;
    round();
    v0_ ^= w;
}

/**
 * @brief 追加数据。
 * @details 先补齐上一次遗留的尾部，再按 8 字节整块压缩，最后缓存不足 8 字节的
 *          尾部；length_ 累计所有写入字节数。
 */
void SipHash13::write(std::span<const uint8_t> p) noexcept {
    length_ += p.size();
    size_t i = 0;

    // 1) 先补齐上次遗留的尾部。
    if (ntail_ != 0) {
        const size_t need = 8 - static_cast<size_t>(ntail_);
        if (p.size() < need) {
            for (; i < p.size(); ++i) {
                tail_ |= static_cast<uint64_t>(p[i]) << (8 * (ntail_ + static_cast<int>(i)));
            }
            ntail_ += static_cast<int>(p.size());
            return;
        }
        for (size_t j = 0; j < need; ++j) {
            tail_ |= static_cast<uint64_t>(p[j]) << (8 * (ntail_ + static_cast<int>(j)));
        }
        compress(tail_);
        tail_ = 0;
        ntail_ = 0;
        i = need;
    }

    // 2) 整块处理。
    for (; i + 8 <= p.size(); i += 8) {
        uint64_t w = 0;
        for (int j = 0; j < 8; ++j) {
            w |= static_cast<uint64_t>(p[i + static_cast<size_t>(j)]) << (8 * j);
        }
        compress(w);
    }

    // 3) 缓存剩余不足 8 字节的部分。
    for (; i < p.size(); ++i) {
        tail_ |= static_cast<uint64_t>(p[i]) << (8 * ntail_++);
    }
}

/**
 * @brief 计算摘要（不修改自身状态）。
 * @details 用副本完成最后 1 轮 + 3 轮收尾，因此可以连续 finish() 后再继续 write()，
 *          这正是上游密钥派生所依赖的语义。
 */
uint64_t SipHash13::finish() const noexcept {
    SipHash13 copy = *this;
    const uint64_t b = ((copy.length_ & 0xff) << 56) | copy.tail_;
    copy.v3_ ^= b;
    copy.round();
    copy.v0_ ^= b;
    copy.v2_ ^= 0xff;
    copy.round();
    copy.round();
    copy.round();
    return copy.v0_ ^ copy.v1_ ^ copy.v2_ ^ copy.v3_;
}

}  // namespace et
