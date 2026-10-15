/**
 * @file aes_gcm.hpp
 * @brief AES-128-GCM 与 EasyTier 的 AEAD 尾布局。
 *
 * @details 线上加密报文的尾部固定为：
 * @code
 * [ 密文 ][ tag(16) ][ nonce(12) ]
 * @endcode
 * AAD 为空；nonce 位于 tag 之后是上游的既定布局，不能为了"更安全"把
 * PeerManagerHeader 放进 AAD，否则无法与上游互通。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "support/bytes.hpp"

namespace et {

/** @brief GCM tag 长度。 */
inline constexpr size_t kAeadTagSize = 16;
/** @brief GCM nonce 长度。 */
inline constexpr size_t kAeadNonceSize = 12;
/** @brief 尾部总长度：tag + nonce。 */
inline constexpr size_t kAeadTailSize = kAeadTagSize + kAeadNonceSize;
static_assert(kAeadTailSize == 28);

/**
 * @brief 一个 AES-128-GCM 上下文（持有导入后的密钥）。
 *
 * @note 后端是 mbedTLS 4.x 的 PSA Crypto 接口（psa_aead_encrypt/psa_aead_decrypt），
 *       组件仅适配 ESP-IDF 6.1，因此没有第二个后端实现。
 * @warning 控制面与数据面各自持有一个实例，禁止跨任务共享同一个实例。
 */
class AesGcm128 {
public:
    /**
     * @brief 用 16 字节密钥构造上下文。
     * @param key AES-128 密钥
     */
    explicit AesGcm128(const std::array<uint8_t, 16>& key) noexcept;
    /** @brief 释放密钥上下文。 */
    ~AesGcm128() noexcept;

    AesGcm128(const AesGcm128&) = delete;
    AesGcm128& operator=(const AesGcm128&) = delete;
    AesGcm128(AesGcm128&&) = delete;
    AesGcm128& operator=(AesGcm128&&) = delete;

    /** @brief 密钥是否导入成功。 */
    [[nodiscard]] bool ready() const noexcept;

    /**
     * @brief 加密并写出 @c [密文|tag|nonce] 。
     * @param plain 明文
     * @param out 输出缓冲区，容量必须 >= plain.size() + kAeadTailSize
     * @param nonce 12 字节 nonce（由调用方提供，便于测试向量复现）
     * @return 写入的字节数；0 表示失败
     */
    [[nodiscard]] size_t encrypt(Bytes plain, MutBytes out,
                                 const std::array<uint8_t, kAeadNonceSize>& nonce) noexcept;

    /**
     * @brief 使用随机 nonce 加密（目标平台为硬件随机源）。
     * @return 写入的字节数；0 表示失败
     */
    [[nodiscard]] size_t encrypt_random_nonce(Bytes plain, MutBytes out) noexcept;

    /**
     * @brief 就地解密 @c [密文|tag|nonce] 。
     * @param[in,out] buf 输入密文；成功时被收缩为明文长度
     * @return true 认证并解密成功；false 认证失败（此时 @p buf 内容未定义，
     *         调用方必须整包丢弃，不能投递可能已被改写的缓冲区）
     */
    [[nodiscard]] bool decrypt_in_place(MutBytes& buf) noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace et
