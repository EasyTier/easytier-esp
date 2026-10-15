/**
 * @file config.hpp
 * @brief 配置解析与校验的辅助函数（配置数据本身见公开的 @ref et::Config ）。
 *
 * @details 公开的 @ref et::Config 同时是"用户输入"与"生效配置"，因此本头文件只保留
 *          纯函数式助手：字符串/地址/UUID 解析、主机名规范化、结构校验。TOML 入口与
 *          编程式 setter 复用同一批函数，避免两套语义漂移。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "easytier-esp.h"

namespace et {

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
 * @brief 解析 8-4-4-4-12 形式的 UUID 文本。
 * @param text 输入文本
 * @param out 输出 16 字节
 */
[[nodiscard]] bool parse_uuid_text(std::string_view text, std::array<uint8_t, 16>& out) noexcept;

/**
 * @brief 把 16 字节 UUID 格式化为标准文本（36 字符 + 结尾 NUL）。
 * @param id 16 字节 UUID
 * @param out 输出缓冲（至少 37 字节）
 */
void format_uuid_text(const uint8_t* id, char* out) noexcept;

/** @brief 把数值形式的 IPv4 转换为文本，返回的视图指向 @p buf 。 */
[[nodiscard]] std::string_view ipv4_to_text(uint32_t value, std::array<char, 16>& buf) noexcept;

/**
 * @brief 按上游 hostname 规则规范化后写入 @p out 。
 *
 * @details 去除控制字符，最多保留 32 个 Unicode 码点（上游 `hostname` 规则），因此结果
 *          不会超过 128 字节。字段本身按需分配、没有固定容量限制，这里保留该规则只是
 *          为了与上游语义一致。
 * @param in 原始主机名
 * @param out 输出文本缓冲
 * @return @c ESP_OK，或 @c ESP_ERR_NO_MEM
 */
[[nodiscard]] esp_err_t normalize_hostname(std::string_view in, TextBuffer& out) noexcept;

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
[[nodiscard]] esp_err_t validate_config(const Config& cfg) noexcept;

}  // namespace et
