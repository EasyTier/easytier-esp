/**
 * @file settings.hpp
 * @brief @ref et::Inst 的私有设置项（组件内部类型，不出现在公开 API）。
 *
 * @details
 * - TOML 是唯一配置入口：@ref Settings 的每个字段都由 `config/toml.cpp` 从 TOML 覆盖，
 *   缺省值由成员初始化器给出，因此"构造 Inst"天然是**全量替换**（缺省值 + TOML）。
 * - 文本字段与入口列表使用 `std::string` / `std::vector`：短字段走 SSO，不额外占用栈；
 *   组件以 `-fno-exceptions` 编译，分配失败会终止进程（不再是字段级错误码）。
 * - 本类型只作为 @ref Inst 的私有实现存在，应用无法直接构造或读取；对外可读的
 *   诊断快照是公开的 @ref ConfigInfo 。
 */
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "easytier-esp.h"

namespace et {

/** @brief 一个已解析的入口地址。 */
struct PeerEndpoint {
    std::string host{};  ///< 主机名或点分 IPv4 文本
    uint16_t port = 0;   ///< 端口
};

/**
 * @brief 实例的全部设置项（缺省值与上游默认语义一致）。
 *
 * @note 可拷贝（`std::string`/`std::vector` 自带深拷贝）：构造 Inst 时会整份拷进
 *       运行上下文，之后应用无法再修改其中的取值。
 */
struct Settings {
    std::string instance_name{};                    ///< 实例名（缺省取 hostname）
    std::string hostname{};                         ///< 主机名（缺省 Espressif，规范化后最多 32 码点）
    std::array<uint8_t, 16> instance_id{};          ///< 实例 UUID 字节
    bool has_instance_id = false;                   ///< 区分未指定 UUID 与显式全零 UUID
    bool instance_name_explicit = false;            ///< instance_name 是否由 TOML 显式给出

    std::string network_name{std::string(kDefaultNetworkName)};  ///< 网络名
    std::string network_secret{};                   ///< 网络密钥（长度无固定上限）

    std::vector<PeerEndpoint> peers{};              ///< 入口地址列表（条数无固定上限）

    bool has_static_ipv4 = false;                   ///< 是否配置了静态虚拟 IPv4
    uint32_t static_ipv4 = 0;                       ///< 静态虚拟 IPv4（数值表示）
    uint8_t static_prefix = 24;                     ///< 静态前缀长度

    bool dhcp = false;                              ///< 是否启用虚拟地址自分配
    bool dhcp_has_subnet = false;                   ///< 是否显式配置了 dhcp 子网
    uint32_t dhcp_subnet = kDefaultDhcpSubnet;      ///< dhcp 子网网络号
    uint8_t dhcp_prefix = kDefaultDhcpPrefix;       ///< dhcp 子网前缀长度

    /** @brief 是否对数据与 RPC 报文做 AES-128-GCM 加密（上游 `flags.enable_encryption`）。 */
    bool enable_encryption = true;
    uint16_t mtu = kDefaultMtu;                     ///< 虚拟网卡 MTU，取值 576..1380

    /**
     * @brief 补齐实例身份（每次构造 Inst 解析完 TOML 后调用）。
     * @details hostname 为空回退 @ref kDefaultHostname ；instance_name 未显式配置时取
     *          hostname；instance_id 未配置时生成随机 UUID v4（显式 UUID 含全零都保留）。
     */
    void finalize_identity() noexcept;

    /** @brief 在 peer 通告其它子网之前使用的 dhcp 子网。 */
    [[nodiscard]] uint32_t dhcp_subnet_or_default() const noexcept {
        return dhcp_has_subnet ? dhcp_subnet : kDefaultDhcpSubnet;
    }
};

/** @brief 用 0 覆写字符串内容再清空（用于清除网络密钥等敏感文本）。 */
void zeroize_string(std::string& text) noexcept;

}  // namespace et
