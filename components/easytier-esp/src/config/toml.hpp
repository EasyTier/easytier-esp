/**
 * @file toml.hpp
 * @brief 与上游兼容的 TOML 配置加载。
 *
 * @details 字段策略与上游 `easytier-core/src/config/toml.rs` 一致：
 * 1. **支持的字段必须写对**：类型错误、取值非法（UUID/CIDR/MTU 越界/URI 非法等）一律
 *    以 Error 日志说明原因并返回失败；唯一例外是 `ipv4` 字符串的内容——与上游
 *    `get_ipv4().parse().ok()` 一样无法解析时静默忽略（视为未配置静态地址）；
 * 2. **不支持的字段根本不解析也不报错**：未知键、上游有但本组件未实现的表/键
 *    （listeners、routes、proxy_network、flags 里的其它标志、peer 表内的其它键……）；
 * 3. **缺失的字段**保留组件缺省值（如 network_name 缺省 `default`、instance_id 随机生成）；
 * 4. 语法错误或解析资源超限同样以 Error 日志说明并返回错误。空文档、无入口、无虚拟
 *    地址都可以解析成功，但不保证网络可用（结构完整性由 @ref validate_config 校验）。
 *
 * **不缓存任何诊断**：本函数直接写日志，调用方只需看返回码。
 *
 * 本函数按字段覆盖 @p out ；**全量替换语义由 @ref et::Inst 的构造函数实现**——它总是
 * 从缺省构造的 @ref Settings 开始，因此 @p out 里不会有上一次配置的取值。
 */
#pragma once

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
