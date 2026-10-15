/**
 * @file config.hpp
 * @brief 校验后的、拥有所有权的配置。
 *
 * @details 编程式入口与 TOML 入口最终都汇聚到本结构，避免两套语义漂移。
 *          除入口地址列表外全部为定长数组；入口列表按**实际条数**在堆上分配，
 *          因此配置里写多少个入口都不再有固定上限（只受 TOML 文本与解析预算约束）。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>

#include "easytier-esp.h"

namespace et {

/** @brief instance_name 最大字节数。 */
inline constexpr size_t kMaxInstanceNameBytes = 127;
/** @brief hostname 最大字节数（规范化后不超过 32 个码点）。 */
inline constexpr size_t kMaxHostnameBytes = 128;
/** @brief network_name 最大字节数。 */
inline constexpr size_t kMaxNetworkNameBytes = 128;
/** @brief network_secret 最大字节数。 */
inline constexpr size_t kMaxNetworkSecretBytes = 256;
/** @brief 入口主机名最大字节数。 */
inline constexpr size_t kMaxPeerHostBytes = 255;

/** @brief MTU 下限。 */
inline constexpr uint16_t kMinMtu = 576;
/** @brief MTU 上限（与上游默认值一致）。 */
inline constexpr uint16_t kMaxMtu = kDefaultMtu;

/** @brief 上游在新网络中回退使用的自举子网：10.126.126.0/24。 */
inline constexpr uint32_t kDefaultDhcpSubnet = 0x0A7E7E00u;  // 10.126.126.0
/** @brief 自举子网前缀长度。 */
inline constexpr uint8_t kDefaultDhcpPrefix = 24;

/** @brief 一个已解析的入口地址。 */
struct PeerEndpoint {
    std::array<char, kMaxPeerHostBytes + 1> host{};  ///< 主机名或点分 IPv4 文本
    uint16_t port = 0;                               ///< 端口
};

/**
 * @brief 入口地址列表：按实际条数在堆上分配，因此配置里的入口数量没有固定上限。
 *
 * @details 只支持移动、不支持拷贝（避免隐式的大块拷贝与重复分配）。分配失败通过
 *          返回值报告而不是异常——组件以 @c -fno-exceptions 编译。容量只增不减，
 *          重复使用同一个解析缓冲时不会反复分配。
 */
class PeerEndpointList {
public:
    PeerEndpointList() noexcept = default;
    ~PeerEndpointList() noexcept { release(); }

    PeerEndpointList(const PeerEndpointList&) = delete;
    PeerEndpointList& operator=(const PeerEndpointList&) = delete;
    PeerEndpointList(PeerEndpointList&& other) noexcept { move_from(other); }
    PeerEndpointList& operator=(PeerEndpointList&& other) noexcept {
        if (this != &other) {
            release();
            move_from(other);
        }
        return *this;
    }

    /** @brief 预留至少 @p n 条容量（只增长，不改变 size）。 */
    [[nodiscard]] esp_err_t reserve(size_t n) noexcept {
        if (n <= capacity_) {
            return ESP_OK;
        }
        PeerEndpoint* fresh = new (std::nothrow) PeerEndpoint[n];
        if (fresh == nullptr) {
            return ESP_ERR_NO_MEM;
        }
        for (size_t i = 0; i < size_; ++i) {
            fresh[i] = items_[i];
        }
        delete[] items_;
        items_ = fresh;
        capacity_ = n;
        return ESP_OK;
    }

    /** @brief 追加一条；调用方需先保证容量足够（见 @ref full ）。 */
    void push_back(const PeerEndpoint& value) noexcept {
        if (size_ < capacity_) {
            items_[size_++] = value;
        }
    }

    /** @brief 是否已达预留容量。 */
    [[nodiscard]] bool full() const noexcept { return size_ >= capacity_; }
    /** @brief 清空内容（保留已分配的容量）。 */
    void clear() noexcept { size_ = 0; }
    /** @brief 当前条数。 */
    [[nodiscard]] size_t size() const noexcept { return size_; }
    /** @brief 首元素指针（可能为 nullptr）。 */
    [[nodiscard]] const PeerEndpoint* data() const noexcept { return items_; }
    /** @brief 首元素指针（可能为 nullptr）。 */
    [[nodiscard]] PeerEndpoint* data() noexcept { return items_; }
    /** @brief 下标访问（不做越界检查，调用方保证 i < size()）。 */
    [[nodiscard]] const PeerEndpoint& operator[](size_t i) const noexcept { return items_[i]; }
    /** @brief 下标访问（不做越界检查，调用方保证 i < size()）。 */
    [[nodiscard]] PeerEndpoint& operator[](size_t i) noexcept { return items_[i]; }

private:
    void release() noexcept {
        delete[] items_;
        items_ = nullptr;
        size_ = 0;
        capacity_ = 0;
    }
    void move_from(PeerEndpointList& other) noexcept {
        items_ = other.items_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.items_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    PeerEndpoint* items_ = nullptr;  ///< 堆缓冲
    size_t size_ = 0;                ///< 有效条数
    size_t capacity_ = 0;            ///< 已分配容量
};

/**
 * @brief 校验后的配置实体。
 *
 * @note IPv4 地址以"四段字节按大端组成的数值"保存（例如 10.144.144.10 =
 *       0x0A90900A），与 protobuf Ipv4Addr 的语义一致；与 lwIP 的网络序存储
 *       之间的转换在网卡模块显式完成。
 */
struct StoredConfig {
    std::array<char, kMaxInstanceNameBytes + 1> instance_name{};  ///< 实例名
    std::array<char, kMaxHostnameBytes> hostname{};               ///< 主机名（已规范化）
    std::array<uint8_t, 16> instance_id{};                        ///< 实例 UUID 字节
    bool has_instance_id = false;  ///< 区分未指定 UUID 与显式全零 UUID

    std::array<char, kMaxNetworkNameBytes + 1> network_name{};      ///< 网络名
    std::array<char, kMaxNetworkSecretBytes + 1> network_secret{};  ///< 网络密钥

    PeerEndpointList peers{};  ///< 入口地址列表（条数由配置决定，无固定上限）

    bool has_static_ipv4 = false;  ///< 是否配置了静态虚拟 IPv4
    uint32_t static_ipv4 = 0;      ///< 静态虚拟 IPv4（数值表示）
    uint8_t static_prefix = 24;    ///< 静态前缀长度

    bool dhcp = false;                ///< 是否启用虚拟地址自分配
    bool dhcp_has_subnet = false;     ///< 是否显式配置了 dhcp 子网
    uint32_t dhcp_subnet = kDefaultDhcpSubnet;  ///< dhcp 子网网络号
    uint8_t dhcp_prefix = kDefaultDhcpPrefix;   ///< dhcp 子网前缀长度

    /// 是否对数据与 RPC 报文加密（上游 `flags.enable_encryption`，缺省 true）。
    bool enable_encryption = true;

    uint16_t mtu = kDefaultMtu;  ///< 虚拟网卡 MTU

    /** @brief 实例名视图。 */
    [[nodiscard]] std::string_view instance_name_view() const noexcept {
        return {instance_name.data()};
    }
    /** @brief 主机名视图。 */
    [[nodiscard]] std::string_view hostname_view() const noexcept { return {hostname.data()}; }
    /** @brief 网络名视图。 */
    [[nodiscard]] std::string_view network_name_view() const noexcept {
        return {network_name.data()};
    }
    /** @brief 网络密钥视图。 */
    [[nodiscard]] std::string_view network_secret_view() const noexcept {
        return {network_secret.data()};
    }
    /** @brief 在 peer 通告其它子网之前使用的 dhcp 子网。 */
    [[nodiscard]] uint32_t dhcp_subnet_or_default() const noexcept {
        return dhcp_has_subnet ? dhcp_subnet : kDefaultDhcpSubnet;
    }
};

/**
 * @brief 复位为默认值：instance_name/network_name 为 "default"，密钥为空串。
 */
void apply_config_defaults(StoredConfig& cfg) noexcept;

/** @brief 初始化运行身份：缺省 UUID 生成 v4，缺省主机名回退 Espressif，结果缓存。 */
void finalize_config_identity(StoredConfig& cfg) noexcept;


/**
 * @brief 严格解析受支持的 TCP/IPv4 或 DNS 入口，TOML 与编程式配置共用。
 * @return false 表示 URI 无效或协议/地址形式不支持；失败不修改输出。
 */
[[nodiscard]] bool parse_peer_endpoint(std::string_view uri, PeerEndpoint& out) noexcept;


/**
 * @brief 复制字符串到定长数组。
 * @tparam N 目标数组长度
 * @return false 表示源串过长（不进行截断）
 */
template <size_t N>
[[nodiscard]] bool set_fixed_string(std::array<char, N>& dst, std::string_view src) noexcept {
    static_assert(N > 0);
    if (src.size() > N - 1) {
        return false;
    }
    for (size_t i = 0; i < src.size(); ++i) {
        dst[i] = src[i];
    }
    dst[src.size()] = '\0';
    return true;
}

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
 * @brief 按上游 hostname 规则规范化。
 *
 * @details 去除控制字符，最多保留 32 个 Unicode 码点，UTF-8 输出不超过
 *          kMaxHostnameBytes-1 字节。
 * @param in 原始主机名
 * @param out 输出缓冲区
 * @return 规范化结果视图
 */
[[nodiscard]] std::string_view normalize_hostname(std::string_view in,
                                                  std::array<char, kMaxHostnameBytes>& out) noexcept;

/** @brief 取地址的网络号，如 prefix=0 返回 0。 */
[[nodiscard]] uint32_t ipv4_network_of(uint32_t addr, uint8_t prefix) noexcept;

/** @brief 判断两个 IPv4 网段是否重叠（用于拒绝虚拟网段与物理网段冲突）。 */
[[nodiscard]] bool ipv4_prefixes_overlap(uint32_t a, uint8_t prefix_a, uint32_t b,
                                         uint8_t prefix_b) noexcept;

/**
 * @brief 结构完整性校验。
 * @return @c ESP_OK 或 @c ESP_ERR_INVALID_ARG
 */
[[nodiscard]] esp_err_t validate_stored_config(const StoredConfig& cfg) noexcept;

/**
 * @brief 由编程式 Config 构造 StoredConfig。
 * @param config 输入配置，所有视图会被复制
 * @param out 输出配置
 * @param report 可为空的诊断输出（被忽略的入口等）
 */
[[nodiscard]] esp_err_t stored_config_from_config(const Config& config, StoredConfig& out,
                                                  ConfigReport* report) noexcept;

}  // namespace et
