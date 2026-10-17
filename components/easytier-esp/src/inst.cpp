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
 */
#include <new>

#include "context.h"
#include "config/config.hpp"
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
 *       last_error()，具体错误已由 toml.cpp / config.cpp 直接输出日志（不缓存诊断），
 *       析构对失败对象安全。
 */
Inst::Inst(std::string_view toml, const RuntimeOptions& options) noexcept {
    // 从缺省构造的设置开始：缺省值由 Settings 的成员初始化器给出，因此这里天然是
    // "全量替换"语义，不需要额外的复位步骤。
    Settings settings{};
    const SettingsSecretGuard secret_guard{settings};
    last_error_ = parse_toml_into(settings, toml);
    if (last_error_ != ESP_OK) {
        ET_LOGE(kTag, "TOML parse failed: 0x%x", last_error_);
        return;
    }
    // 补齐身份：hostname 回退 Espressif、instance_name 取 hostname、UUID v4（显式值优先）。
    finalize_identity(settings);
    last_error_ = validate_config(settings);
    if (last_error_ != ESP_OK) {
        return;
    }
    // 运行上下文 92,248 B ≈ 91 KiB。未启用 PSRAM 时可能分配失败，这里给出明确提示。
    ctx_ = new (std::nothrow) Context(settings, options);
    if (ctx_ == nullptr) {
        ET_LOGE(kTag, "context allocation failed (%u bytes); enable PSRAM or shrink buffers",
                static_cast<unsigned>(sizeof(Context)));
        last_error_ = ESP_ERR_NO_MEM;
        return;
    }
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

esp_err_t Inst::connect() noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // Context 仍用统一的物理网络开关实现：connect() = underlay ready。
    return ctx_->set_underlay_ready(true);
}

esp_err_t Inst::disconnect() noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return ctx_->set_underlay_ready(false);
}

esp_err_t Inst::get_status(Status& out) const noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    // 友元读取 Context 的原子状态，逐字段拷贝，保证快照自洽。
    out.state = ctx_->state.load();
    out.peer_id = ctx_->my_peer_id.load();
    out.virtual_ipv4 = ctx_->my_ipv4.load();
    out.prefix_len = ctx_->my_prefix.load();
    out.direct_peer_count = ctx_->direct_peers.load();
    out.reachable_route_count = ctx_->reachable_routes.load();
    out.last_error = ctx_->last_error.load();
    out.dropped_packets = ctx_->dropped.load();
    return ESP_OK;
}

Settings Inst::get_config_info() const noexcept {
    Settings cfg{};
    if (ctx_ == nullptr) {
        return cfg;  // 构造失败：返回缺省设置，调用方仍可用 valid() 判断
    }
    cfg = ctx_->settings;
    // 网络密钥只用于组件内部派生密钥，绝不随快照外泄。
    zeroize_string(cfg.network_secret);
    return cfg;
}

esp_err_t Inst::stop(uint32_t timeout_ms) noexcept {
    if (ctx_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    return ctx_->stop(timeout_ms);
}

}  // namespace et
