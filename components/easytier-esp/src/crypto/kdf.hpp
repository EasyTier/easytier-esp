/**
 * @file kdf.hpp
 * @brief EasyTier 的密钥与网络密钥摘要派生。
 */
#ifndef EASYTIER_ESP_CRYPTO_KDF_HPP_
#define EASYTIER_ESP_CRYPTO_KDF_HPP_

#include <array>
#include <cstdint>
#include <string_view>

namespace et {

/**
 * @brief 按上游 derive_key_128 派生 AES-128 密钥。
 * @param secret 网络密钥（network_secret），可为空串
 * @return 16 字节 AES 密钥
 * @note 验证向量：derive_key_128("secret") == 565a19db4ef0c121a8ac580edaf84ea6
 */
[[nodiscard]] std::array<uint8_t, 16> derive_key_128(std::string_view secret) noexcept;

/**
 * @brief 派生握手使用的 32 字节网络密钥摘要。
 * @param network_name 网络名
 * @param secret 网络密钥
 * @return 32 字节摘要，握手报文中必须逐字节一致
 */
[[nodiscard]] std::array<uint8_t, 32> secret_digest(std::string_view network_name,
                                                    std::string_view secret) noexcept;

}  // namespace et

#endif
