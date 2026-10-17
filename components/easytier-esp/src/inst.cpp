/**
 * @file inst.cpp
 * @brief @c et::Inst 的实现：把 TOML 变成 @ref Settings，再交给 @ref Context 管理。
 *
 * @details 生命周期：
 * @code
 * Inst(toml, options) → 解析 TOML、补齐身份、校验结构、分配并准备 Context
 * start()             → 委托 Context 建队列与任务
 * stop()              → 委托 Context 置停止标志并等待任务退出
 * ~Inst()             → 释放 Context（其析构函数负责停任务、释放资源、清零密钥）
 * @endcode
 * 组件内**没有**全局单实例状态：每个 Inst 独占自己的 Settings 与 Context，因此可以
 * 同时存在多个实例（内存与任务数按实例叠加）。
 */
#include <new>

#include "context.h"
#include "config/config.hpp"
#include "config/settings.hpp"
#include "config/toml.hpp"
#include "support/log.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.INSTANCE";

/**
 * @brief 离开作用域时清除局部 @ref Settings 副本里的网络密钥。
 * @details 解析/校验成功后密钥已拷进 Context；失败路径上也不会留下可复用的栈残留。
 */
struct SettingsSecretGuard {
    Settings& settings;
    ~SettingsSecretGuard() noexcept { zeroize_string(settings.network_secret); }
};

}  // namespace

/**
 * @brief 构造实例：解析 TOML → 补齐身份 → 校验结构 → 分配并准备运行上下文。
 * @note 组件禁用异常，构造不能返回错误码；失败时对象 valid()==false、错误码见
 *       last_error()，具体错误已由 toml.cpp 直接输出日志（不缓存诊断），析构对失败对象安全。
 */
Inst::Inst(std::string_view toml, const RuntimeOptions& options) noexcept {
    // 从缺省构造的设置开始：缺省值由 Settings 的成员初始化器给出，因此这里天然是
    // "全量替换"语义，不需要额外的复位步骤。
    Settings settings{};
    const SettingsSecretGuard secret_guard{settings};
    last_error_ = parse_toml_into(settings, toml);
    // 即使解析失败也补齐身份，保证错误信息里能看到实例名/hostname。
    settings.finalize_identity();
    if (last_error_ != ESP_OK) {
        ET_LOGE(kTag, "TOML parse failed: 0x%x", last_error_);
        return;
    }
    last_error_ = validate_config(settings);
    if (last_error_ != ESP_OK) {
        return;
    }
    // 运行上下文 92,808 B ≈ 91 KiB。未启用 PSRAM 时可能分配失败，这里给出明确提示。
    ctx_ = new (std::nothrow) Context();
    if (ctx_ == nullptr) {
        ET_LOGE(kTag, "context allocation failed (%u bytes); enable PSRAM or shrink buffers",
                static_cast<unsigned>(sizeof(Context)));
        last_error_ = ESP_ERR_NO_MEM;
        return;
    }
    ctx_->prepare(settings, options);
    last_error_ = ESP_OK;
}

/** @brief 析构：释放 Context（其析构函数停止任务、释放资源、清零密钥）。 */
Inst::~Inst() noexcept {
    delete ctx_;
    ctx_ = nullptr;
}

esp_err_t Inst::start() noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return ctx_->start();
}

esp_err_t Inst::set_underlay_ready(bool ready) noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return ctx_->set_underlay_ready(ready);
}

esp_err_t Inst::get_status(Status& out) const noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    ctx_->get_status(out);
    return ESP_OK;
}

esp_err_t Inst::get_config_info(ConfigInfo& out) const noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    ctx_->get_config_info(out);
    return ESP_OK;
}

esp_err_t Inst::stop(uint32_t timeout_ms) noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return ctx_->stop(timeout_ms);
}

}  // namespace et
