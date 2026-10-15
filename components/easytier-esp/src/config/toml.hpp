/**
 * @file toml.hpp
 * @brief 与上游兼容的 TOML 配置加载。
 *
 * @details 处理策略分三类：
 * 1. **支持并应用**：字段类型与取值都合法；
 * 2. **非 peer 字段无法采用则忽略**：未知字段、未实现的表、类型错误、取值非法或字段
 *    超出存储容量时跳过并保留默认值，只记录不含敏感值的忽略诊断；
 * 3. **peer 严格校验**：无效/不支持的 URI、错误类型、缺 uri、超过入口容量均报错。
 * 4. **解析失败**：语法错误或解析资源超限也返回错误。空文档、无入口、无虚拟
 *    地址均可加载，但不保证网络可用。编程式 Config 入口仍执行严格校验。
 */
#pragma once

#include <cstddef>
#include <string_view>

#include "config/config.hpp"
#include "easytier-esp.h"

namespace et {

/** @brief TOML 语法嵌套深度上限（数组 / inline table / 表头路径）。 */
inline constexpr size_t kTomlMaxDepth = 32;
/** @brief 单个 key 允许的点号分段数上限（防止 dotted key 放大嵌套）。 */
inline constexpr size_t kTomlMaxKeySegments = 16;

/** @brief 清空调用方的配置报告。 */
void clear_config_report(ConfigReport* report) noexcept;

/**
 * @brief 解析并映射 TOML 文本。
 * @param text TOML 文本；允许 EMBED_TXTFILES 追加的单个末尾 NUL，内部 NUL 仍非法
 * @param out 输出配置，进入前会先被复位
 * @param report 可为空的诊断输出
 * @return @c ESP_OK / @c ESP_ERR_INVALID_ARG / @c ESP_ERR_NO_MEM
 */
esp_err_t parse_toml_config(std::string_view text, StoredConfig& out,
                            ConfigReport* report) noexcept;

/**
 * @brief 深度与 dotted key 限额预扫描。
 *
 * @details 该扫描只用于限额（识别字符串与注释状态），不承担 TOML 解析本身。
 * @return false 表示超过 kTomlMaxDepth 或 kTomlMaxKeySegments
 */
[[nodiscard]] bool toml_scan_limits(std::string_view text) noexcept;

}  // namespace et
