/**
 * @file aes_gcm_psa.cpp
 * @brief 目标平台 AES-128-GCM 实现（PSA Crypto）。
 *
 * @details ESP-IDF 6.1 的 mbedTLS 4.x 不再公开 gcm.h，改用 psa_aead_encrypt/decrypt
 *          + PSA_ALG_GCM。PSA 输出 [密文|tag]，本实现再追加 nonce 得到线上布局
 *          [密文|tag|nonce]；解密走临时缓冲，不依赖 PSA 的原位操作。
 */
#include "crypto/aes_gcm.hpp"

#include <cstring>
#include <new>

#include "protocol/wire.hpp"
#include "psa/crypto.h"
#include "support/log.hpp"
#include "support/random.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.AES";

/** @brief 单帧 [密文|tag|nonce] 上限：TCP body 上限减去 PM 头（与 wire.hpp 同步）。 */
inline constexpr size_t kMaxCiphertext = kMaxTcpBodyLen - kPeerManagerHeaderSize;

/**
 * @brief 保证 PSA Crypto 已初始化。
 * @return 1 表示可用，0 表示初始化失败。
 * @note psa_crypto_init() 可重复调用；这里用状态缓存避免重复初始化。构造发生在
 *       生命周期串行阶段（两个 AEAD 依次构造，任务在其后才创建），不存在递归进入。
 */
int ensure_psa_initialized() noexcept {
    static int status = -1;  // -1 = 尚未尝试
    if (status >= 0) {
        return status;
    }
    const psa_status_t st = psa_crypto_init();
    status = (st == PSA_SUCCESS) ? 1 : 0;
    if (status == 0) {
        ET_LOGE(kTag, "psa_crypto_init failed: %d", static_cast<int>(st));
    }
    return status;
}

}  // namespace

/** @brief 实现细节：导入后的 PSA 密钥句柄 + 解密临时缓冲。 */
struct AesGcm128::Impl {
    psa_key_id_t key = 0;  ///< PSA 导入的 AES-128 密钥
    bool ok = false;       ///< 构造是否成功
    /// 解密临时缓冲：PSA 不保证原位解密，因此先解密到这里再拷回。
    std::array<uint8_t, kMaxCiphertext> scratch{};
};

/**
 * @brief 构造：初始化 PSA 并导入 AES-128 密钥。
 * @note 使用 nothrow new：组件关闭了异常，初始化阶段允许分配。
 */
AesGcm128::AesGcm128(const std::array<uint8_t, 16>& key) noexcept
    : impl_(new (std::nothrow) Impl()) {
    if (impl_ == nullptr) {
        return;
    }
    if (ensure_psa_initialized() == 0) {
        return;
    }
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
    psa_set_key_algorithm(&attr, PSA_ALG_GCM);
    psa_set_key_type(&attr, PSA_KEY_TYPE_AES);
    psa_set_key_bits(&attr, 128);
    const psa_status_t st = psa_import_key(&attr, key.data(), key.size(), &impl_->key);
    psa_reset_key_attributes(&attr);
    if (st != PSA_SUCCESS) {
        ET_LOGE(kTag, "psa_import_key failed: %d", static_cast<int>(st));
        return;
    }
    impl_->ok = true;
}

/** @brief 析构：销毁 PSA 密钥句柄。 */
AesGcm128::~AesGcm128() noexcept {
    if (impl_ != nullptr) {
        if (impl_->ok) {
            (void)psa_destroy_key(impl_->key);
        }
        delete impl_;
    }
}

/** @brief 密钥是否可用。 */
bool AesGcm128::ready() const noexcept { return impl_ != nullptr && impl_->ok; }

/** @brief GCM 加密：PSA 输出 [密文|tag]，随后追加 nonce。 */
size_t AesGcm128::encrypt(Bytes plain, MutBytes out,
                          const std::array<uint8_t, kAeadNonceSize>& nonce) noexcept {
    if (!ready()) {
        return 0;
    }
    if (out.size() < plain.size() + kAeadTailSize) {
        return 0;
    }
    size_t out_len = 0;
    const psa_status_t st =
        psa_aead_encrypt(impl_->key, PSA_ALG_GCM, nonce.data(), nonce.size(), nullptr, 0,
                         plain.data(), plain.size(), out.data(), out.size(), &out_len);
    if (st != PSA_SUCCESS) {
        ET_LOGE(kTag, "psa_aead_encrypt failed: %d", static_cast<int>(st));
        return 0;
    }
    std::memcpy(out.data() + out_len, nonce.data(), kAeadNonceSize);
    return out_len + kAeadNonceSize;
}

/** @brief 使用随机 nonce 加密。 */
size_t AesGcm128::encrypt_random_nonce(Bytes plain, MutBytes out) noexcept {
    std::array<uint8_t, kAeadNonceSize> nonce{};
    fill_random(nonce);
    return encrypt(plain, out, nonce);
}

/**
 * @brief 解密 [密文|tag|nonce]。
 * @details tag_and_ct = ct_len + 16 交给 PSA（GCM 约定密文缓冲包含 tag）；
 *          认证失败时直接返回 false，调用方必须整包丢弃。
 */
bool AesGcm128::decrypt_in_place(MutBytes& buf) noexcept {
    if (!ready() || buf.size() < kAeadTailSize) {
        return false;
    }
    const size_t ct_len = buf.size() - kAeadTailSize;
    const size_t tag_and_ct = ct_len + kAeadTagSize;
    if (tag_and_ct > impl_->scratch.size()) {
        return false;
    }
    const uint8_t* nonce = buf.data() + ct_len + kAeadTagSize;

    size_t plain_len = 0;
    const psa_status_t st =
        psa_aead_decrypt(impl_->key, PSA_ALG_GCM, nonce, kAeadNonceSize, nullptr, 0, buf.data(),
                         tag_and_ct, impl_->scratch.data(), impl_->scratch.size(), &plain_len);
    if (st != PSA_SUCCESS) {
        return false;
    }
    std::memcpy(buf.data(), impl_->scratch.data(), plain_len);
    buf = buf.first(plain_len);
    return true;
}

}  // namespace et
