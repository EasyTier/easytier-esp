/**
 * @file config.hpp
 * @brief 设置项的解析与校验辅助函数（设置数据本身见 @ref et::Settings ）。
 *
 * @details 公开 API 只有一个 @ref et::Inst 类，设置项是它的私有实现；因此本头文件只保留
 *          纯函数式助手：字符串/地址/UUID 解析、主机名规范化、结构校验。TOML 入口与
 *          校验复用同一批函数，避免两套语义漂移。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "easytier-esp.hpp"

namespace et {

/**
 * @brief 补齐实例身份（每次构造 Inst 解析完 TOML 后调用）。
 * @details hostname 为空回退 @ref kDefaultHostname ；instance_name 未显式配置时取
 *          hostname；instance_id 未配置时生成随机 UUID v4（显式 UUID 含全零都保留）。
 */
void finalize_identity(Settings& settings) noexcept;

/** @brief 用 0 覆写字符串内容再释放容量（用于清除网络密钥等敏感文本）。 */
void zeroize_string(std::string& text) noexcept;

/**
 * @brief 严格解析受支持的 TCP/IPv4 或 DNS 入口（TOML 是唯一配置入口）。
 * @param uri 输入，形如 "tcp://host:port"
 * @param host 输出主机文本视图；指向 @p uri 内部，仅在 @p uri 有效期内可用
 * @param port 输出端口
 * @return false 表示 URI 无效或协议/地址形式不支持；失败不修改输出
 */
[[nodiscard]] bool parse_peer_endpoint(std::string_view uri, std::string_view& host,
                                       uint16_t& port) noexcept;

/**
 * @brief 严格解析点分十进制 IPv4。
 * @param text 形如 "10.0.0.1"
 * @param out 输出数值表示
 * @return false 表示格式非法或某一段超过 255
 */
[[nodiscard]] bool parse_ipv4_numeric(std::string_view text, uint32_t& out) noexcept;

/**
 * @brief 解析 "10.0.0.1" 或 "10.0.0.1/24"。
 * @param text 输入
 * @param addr 输出地址
 * @param prefix 输出前缀长度（无 "/" 时为 24）
 * @param had_prefix 是否显式带了 "/前缀"
 */
[[nodiscard]] bool parse_ipv4_cidr(std::string_view text, uint32_t& addr, uint8_t& prefix,
                                   bool& had_prefix) noexcept;

/**
 * @brief 解析 UUID 文本：忽略任意位置的连字符，要求恰好 32 个十六进制位。
 * @param text 输入文本
 * @param out 输出 16 字节
 */
[[nodiscard]] bool parse_uuid_text(std::string_view text, std::array<uint8_t, 16>& out) noexcept;

/**
 * @brief 按上游 hostname 规则规范化后写入 @p out 。
 *
 * @details 去除控制字符，最多保留 32 个 Unicode 码点（上游 `hostname` 规则），因此结果
 *          不会超过 128 字节。
 * @param in 原始主机名
 * @param out 输出文本；失败（含全是控制字符）时被清空
 */
void normalize_hostname(std::string_view in, std::string& out) noexcept;

/** @brief 取地址的网络号，如 prefix=0 返回 0。 */
[[nodiscard]] uint32_t ipv4_network_of(uint32_t addr, uint8_t prefix) noexcept;

/** @brief 判断两个 IPv4 网段是否重叠（用于拒绝虚拟网段与物理网段冲突）。 */
[[nodiscard]] bool ipv4_prefixes_overlap(uint32_t a, uint8_t prefix_a, uint32_t b,
                                         uint8_t prefix_b) noexcept;

/**
 * @brief 结构完整性校验：至少 1 个入口、必须配置静态 IPv4 或 dhcp、MTU 在范围内、
 *        网络名非空、每个入口都有主机名与端口。
 * @return @c ESP_OK 或 @c ESP_ERR_INVALID_ARG
 * @note 入口**数量**没有上限；可同时连接的数量由 Kconfig 决定。
 */
[[nodiscard]] esp_err_t validate_config(const Settings& cfg) noexcept;

}  // namespace et
