/**
 * @file toml.hpp
 * @brief 与上游兼容的 TOML 配置加载。
 *
 * @details 字段策略与上游 `toml.rs` 一致：支持的字段写错（类型/取值非法）直接报错；
 *          不支持的字段不解析也不报错；缺失字段保留组件缺省值；`ipv4` 内容无法
 *          解析时按未配置处理。语法错误与资源超限同样以 Error 日志报告，不缓存诊断。
 *          本函数按字段覆盖 @p out，全量替换语义由 @ref et::Inst 构造保证。
 */
#ifndef EASYTIER_ESP_CONFIG_TOML_HPP_
#define EASYTIER_ESP_CONFIG_TOML_HPP_

#include <cstddef>
#include <string_view>

#include "config/config.hpp"
#include "easytier-esp.hpp"

namespace et {

/** @brief TOML 语法嵌套深度上限（数组 / inline table / 表头路径）。 */
inline constexpr size_t kTomlMaxDepth = 32;
/** @brief 单个 key 允许的点号分段数上限（防止 dotted key 放大嵌套）。 */
inline constexpr size_t kTomlMaxKeySegments = 16;

/**
 * @brief 解析 TOML 文本并更新设置项。
 * @param out 目标设置；只覆盖 TOML 中出现的字段
 * @param text TOML 文本；允许 EMBED_TXTFILES 追加的单个末尾 NUL，内部 NUL 仍非法
 * @return @c ESP_OK / @c ESP_ERR_INVALID_ARG / @c ESP_ERR_NO_MEM / @c ESP_ERR_INVALID_STATE
 */
esp_err_t parse_toml_into(Settings& out, std::string_view text) noexcept;

/**
 * @brief 深度与 dotted key 限额预扫描。
 *
 * @details 该扫描只用于限额（识别字符串与注释状态），不承担 TOML 解析本身。
 * @return false 表示超过 kTomlMaxDepth 或 kTomlMaxKeySegments
 */
[[nodiscard]] bool toml_scan_limits(std::string_view text) noexcept;

}  // namespace et

#endif
