/**
 * @file kdf.cpp
 * @brief 密钥与网络密钥摘要派生（与上游逐字节一致）。
 */
#include "crypto/kdf.hpp"

#include "crypto/siphash.hpp"
#include "support/bytes.hpp"

namespace et {
namespace {

/** @brief 以大端写入 64 位值（上游使用 to_be_bytes）。 */
constexpr void be64_store(uint8_t* p, uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v >> (8 * (7 - i)));
    }
}

}  // namespace

/**
 * @brief 派生 AES-128 密钥。
 * @details 与上游 derive_key_128 一致：写入 secret 后取 8 字节，再写入该 8 字节
 *          后取第二个 8 字节。
 */
std::array<uint8_t, 16> derive_key_128(std::string_view secret) noexcept {
    std::array<uint8_t, 16> key{};
    SipHash13 h;
    h.write(bytes_of(secret));
    be64_store(key.data(), h.finish());
    h.write(std::span<const uint8_t>(key.data(), 8));
    be64_store(key.data() + 8, h.finish());
    // 上游在返回前还会把整个 key 再写入 hasher；该写入不影响 key[] 内容，故省略。
    return key;
}

/**
 * @brief 派生 32 字节网络密钥摘要。
 * @details 逐轮 finish 取 8 字节，并把已生成的摘要前缀回灌进 hasher（上游
 *          generate_digest_from_str 的做法）。
 */
std::array<uint8_t, 32> secret_digest(std::string_view network_name,
                                      std::string_view secret) noexcept {
    std::array<uint8_t, 32> digest{};
    SipHash13 h;
    h.write(bytes_of(network_name));
    h.write(bytes_of(secret));
    for (size_t i = 0; i < digest.size() / 8; ++i) {
        be64_store(digest.data() + i * 8, h.finish());
        h.write(std::span<const uint8_t>(digest.data(), (i + 1) * 8));
    }
    return digest;
}

}  // namespace et
