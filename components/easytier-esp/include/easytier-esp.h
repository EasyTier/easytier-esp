/**
 * @file easytier-esp.h
 * @brief EasyTier-ESP 公开接口：把本设备接入一个已有的 EasyTier 虚拟网络。
 *
 * @details
 * - 公开接口仅提供 C++20 API，不提供 C 包装层；内部实现位于 @c namespace et。
 * - 配置对象是 @ref et::Config：用 TOML 文本构造（或从另一个 @ref et::Config 拷贝），
 *   组件**不再接受文件路径**——应用自己读取文件后把文本交给 @ref et::Config::init 。
 * - 运行期只有**一个全局实例**，接口位于 @c namespace et::Inst，没有实例类。
 * - 配置格式与上游 EasyTier 核心保持一致（TOML），无法实现的参数会被忽略并
 *   通过 @ref et::ConfigReport 报告，而不是直接判定配置非法。
 * - 组件不初始化 WiFi、不挂载文件系统、不修改物理默认网卡，这些由应用负责。
 *
 * @note 首版仅适配 ESP-IDF 6.1.x。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string_view>

#include "esp_err.h"

namespace et {

// ---------------------------------------------------------------------------
// 容量与缺省值
// ---------------------------------------------------------------------------

/** @brief MTU 下限。 */
inline constexpr uint16_t kMinMtu = 576;
/** @brief 虚拟网卡默认 MTU（与上游默认值一致），也是 MTU 上限。 */
inline constexpr uint16_t kDefaultMtu = 1380;

/** @brief 上游在新网络中回退使用的自举子网：10.126.126.0/24。 */
inline constexpr uint32_t kDefaultDhcpSubnet = 0x0A7E7E00u;  // 10.126.126.0
/** @brief 自举子网前缀长度。 */
inline constexpr uint8_t kDefaultDhcpPrefix = 24;

/** @brief 未配置 hostname 时的 ESP 平台回退名。 */
inline constexpr std::string_view kDefaultHostname = "Espressif";
/** @brief 未配置 network_name 时的网络名。 */
inline constexpr std::string_view kDefaultNetworkName = "default";

/**
 * @brief @ref ConfigInfo 能快照的入口数量上限（仅影响诊断输出，不影响实际配置）。
 * @note 实际入口数量没有上限；超过该数量时 @ref ConfigInfo::peers_truncated 为 true，
 *       完整数量见 @ref ConfigInfo::peer_total_count 。
 */
inline constexpr size_t kConfigInfoMaxPeers = 16;
/** @brief @ref ConfigReport 能携带的最大诊断条数。 */
inline constexpr size_t kMaxDiagnostics = 32;
/** @brief 每条诊断记录的字段路径最大长度（含结尾 '\0'）。 */
inline constexpr size_t kMaxDiagnosticKey = 128;

// ---------------------------------------------------------------------------
// 配置诊断
// ---------------------------------------------------------------------------

/** @brief 配置诊断类别。 */
enum class ConfigIssue : uint8_t {
    IgnoredParameter,      ///< 合法但未实现的参数/表，已忽略
    IgnoredPeer,           ///< 入口地址合法但当前不支持（如非 tcp 协议、IPv6）
    SyntaxError,           ///< TOML 语法错误
    InvalidValue,          ///< 已支持字段的取值非法
    MissingRequiredValue,  ///< 缺少必需字段
    ResourceLimit,         ///< 超出文本长度/嵌套深度/节点数/内存预算等容量限制
    IoError,               ///< 配置文件读取失败
};

/** @brief 单条配置诊断，只记录字段路径，不记录任何敏感取值。 */
struct ConfigDiagnostic {
    ConfigIssue issue = ConfigIssue::IgnoredParameter;  ///< 诊断类别
    uint32_t line = 0;                                  ///< 行号，0 表示未知
    uint32_t column = 0;                                ///< 列号，0 表示未知
    std::array<char, kMaxDiagnosticKey> key{};          ///< 字段路径，如 "flags.mtu"
};

/**
 * @brief 配置加载报告。
 *
 * @note 该结构约 4.5 KiB；超过 @ref kMaxDiagnostics 条的诊断会被计数到
 *       @ref omitted_count，不会因为报告容量不足而拒绝配置。
 */
struct ConfigReport {
    std::array<ConfigDiagnostic, kMaxDiagnostics> items{};  ///< 诊断列表
    size_t count = 0;                                       ///< items 中有效条数
    size_t omitted_count = 0;                               ///< 因容量不足被省略的条数
};

// ---------------------------------------------------------------------------
// 配置数据
// ---------------------------------------------------------------------------

/**
 * @brief 拥有的文本缓冲：按实际长度在堆上分配，**长度没有固定上限**。
 *
 * @details 分配失败通过返回值报告而不是异常（组件以 @c -fno-exceptions 编译）；缓冲
 *          始终以 @c '\\0' 结尾，因此 @ref c_str 可以直接当 C 字符串用。只支持移动，
 *          需要深拷贝时用 @ref copy_from 。容量只增不减，重复赋值不会反复分配。
 */
class TextBuffer {
public:
    TextBuffer() noexcept = default;
    ~TextBuffer() noexcept { release(); }

    TextBuffer(const TextBuffer&) = delete;
    TextBuffer& operator=(const TextBuffer&) = delete;
    TextBuffer(TextBuffer&& other) noexcept { move_from(other); }
    TextBuffer& operator=(TextBuffer&& other) noexcept {
        if (this != &other) {
            release();
            move_from(other);
        }
        return *this;
    }

    /**
     * @brief 赋值（按实际长度分配）。
     * @return @c ESP_OK，或 @c ESP_ERR_NO_MEM
     */
    [[nodiscard]] esp_err_t assign(std::string_view text) noexcept {
        if (text.size() + 1 > capacity_) {
            char* fresh = new (std::nothrow) char[text.size() + 1];
            if (fresh == nullptr) {
                return ESP_ERR_NO_MEM;
            }
            delete[] data_;
            data_ = fresh;
            capacity_ = text.size() + 1;
        }
        for (size_t i = 0; i < text.size(); ++i) {
            data_[i] = text[i];
        }
        data_[text.size()] = '\0';
        size_ = text.size();
        return ESP_OK;
    }

    /**
     * @brief 深拷贝另一个缓冲。
     * @return @c ESP_OK，或 @c ESP_ERR_NO_MEM
     */
    [[nodiscard]] esp_err_t copy_from(const TextBuffer& other) noexcept {
        if (this == &other) {
            return ESP_OK;
        }
        return assign(other.view());
    }

    /** @brief 文本视图（未赋值时为空视图）。 */
    [[nodiscard]] std::string_view view() const noexcept {
        return data_ != nullptr ? std::string_view(data_, size_) : std::string_view{};
    }
    /** @brief C 字符串；未赋值时返回指向静态空串的指针，永不返回 nullptr。 */
    [[nodiscard]] const char* c_str() const noexcept {
        static constexpr char kEmpty[] = "";
        return data_ != nullptr ? data_ : kEmpty;
    }
    /** @brief 是否为空。 */
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    /** @brief 当前长度（不含结尾 NUL）。 */
    [[nodiscard]] size_t size() const noexcept { return size_; }

    /** @brief 清空内容（保留容量）。 */
    void clear() noexcept {
        if (data_ != nullptr) {
            data_[0] = '\0';
        }
        size_ = 0;
    }

    /** @brief 用 0 覆写内容后再清空，用于擦除密钥等敏感文本。 */
    void zeroize() noexcept {
        for (size_t i = 0; i < capacity_; ++i) {
            data_[i] = '\0';
        }
        size_ = 0;
    }

private:
    void release() noexcept {
        delete[] data_;
        data_ = nullptr;
        size_ = 0;
        capacity_ = 0;
    }
    void move_from(TextBuffer& other) noexcept {
        data_ = other.data_;
        size_ = other.size_;
        capacity_ = other.capacity_;
        other.data_ = nullptr;
        other.size_ = 0;
        other.capacity_ = 0;
    }

    char* data_ = nullptr;  ///< 堆缓冲（始终 NUL 结尾）
    size_t size_ = 0;       ///< 文本长度（不含 NUL）
    size_t capacity_ = 0;   ///< 缓冲容量（含结尾 NUL）
};

/** @brief 一个已解析的入口地址。 */
struct PeerEndpoint {
    TextBuffer host{};  ///< 主机名或点分 IPv4 文本（长度无固定上限）
    uint16_t port = 0;  ///< 端口
};

/**
 * @brief 入口地址列表：按实际条数在堆上分配，因此配置里的入口数量没有固定上限。
 *
 * @details 只支持移动、不支持拷贝（避免隐式的大块拷贝与重复分配）。分配失败通过
 *          返回值报告而不是异常——组件以 @c -fno-exceptions 编译。容量只增不减，
 *          重复解析同一份配置时不会反复分配。
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
            fresh[i] = std::move(items_[i]);
        }
        delete[] items_;
        items_ = fresh;
        capacity_ = n;
        return ESP_OK;
    }

    /**
     * @brief 深拷贝另一个列表（拷贝时可能分配失败）。
     * @return @c ESP_OK，或 @c ESP_ERR_NO_MEM
     */
    [[nodiscard]] esp_err_t copy_from(const PeerEndpointList& other) noexcept {
        clear();
        const esp_err_t err = reserve(other.size());
        if (err != ESP_OK) {
            return err;
        }
        for (size_t i = 0; i < other.size(); ++i) {
            PeerEndpoint endpoint{};
            const esp_err_t host_err = endpoint.host.copy_from(other.items_[i].host);
            if (host_err != ESP_OK) {
                return host_err;
            }
            endpoint.port = other.items_[i].port;
            items_[size_++] = std::move(endpoint);
        }
        return ESP_OK;
    }

    /** @brief 追加一条（移动语义）；调用方需先保证容量足够（见 @ref full ）。 */
    void push_back(PeerEndpoint&& value) noexcept {
        if (size_ < capacity_) {
            items_[size_++] = std::move(value);
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
 * @brief 生效的 EasyTier 配置。
 *
 * @details 配置**数据**是公开成员，便于应用直接检查"实际生效的值"；配置的**来源**
 *          与**错误**由本类统一管理：
 *          - 用 TOML 文本构造 / @ref init 解析（解析失败不会破坏已有取值，详见 @ref init）；
 *          - 构造与解析完成时会补齐实例身份（见 @ref finalize_identity ）；
 *          - 解析诊断与错误码保存在对象内，通过 @ref report / @ref last_error 读取。
 *
 *          数据成员中的定长 C 字符串一律以 @c '\\0' 结尾；IPv4 以"四段字节按大端组成
 *          的数值"保存（例如 10.144.144.10 = 0x0A90900A），与 protobuf Ipv4Addr 一致。
 *
 * @note 本类不持有任何运行期资源，拷贝是安全的（入口列表为深拷贝，失败时把错误记录在
 *       副本的 @ref last_error 中）。
 */
class Config {
public:
    /**
     * @brief 构造缺省配置。
     * @details 身份在构造时就补齐：hostname 回退 @ref kDefaultHostname ，
     *          instance_name 取 hostname，instance_id 生成随机 UUID v4。
     */
    Config() noexcept;

    /** @brief 拷贝构造：入口列表深拷贝；分配失败时副本记录 @c ESP_ERR_NO_MEM 。 */
    Config(const Config& other) noexcept;
    /** @brief 拷贝赋值；语义同拷贝构造。 */
    Config& operator=(const Config& other) noexcept;
    /** @brief 移动构造（不分配，恒成功）。 */
    Config(Config&& other) noexcept;
    /** @brief 移动赋值（不分配，恒成功）。 */
    Config& operator=(Config&& other) noexcept;
    /** @brief 析构（释放入口列表）。 */
    ~Config() noexcept = default;

    /**
     * @brief 用 TOML 文本构造配置（转发到 @ref init）。
     * @param toml TOML 文本，可带 EMBED_TXTFILES 追加的单个末尾 NUL
     * @note 构造无法返回错误码，解析结果请查 @ref last_error 与 @ref report 。
     */
    explicit Config(std::string_view toml) noexcept;

    /**
     * @brief 用 TOML 文本**全量替换**配置。
     *
     * @details 语义是"重建"而不是"增量更新"：
     *          1. 先把配置复位为缺省值（入口列表清空，但保留已分配容量）；
     *          2. 再按 TOML 逐字段覆盖；
     *          3. 最后重新补齐实例身份——**随机生成的缺省值也会重新生成**，
     *             即未在 TOML 里显式给出 `instance_id` 时会得到一个新的 UUID v4，
     *             `hostname` 回退 `Espressif`，`instance_name` 重新按 hostname 推导。
     *          因此上一次 @ref init 的残留不会影响本次结果。
     *
     *          peer 错误（非 TCP、缺 uri、无效主机/端口等）与真正语法错误会返回失败；
     *          未知或无法采用的**非 peer** 字段只记录诊断。失败时已解析的字段保留，
     *          错误码写入 @ref last_error ，诊断写入 @ref report 。
     *
     * @param toml TOML 文本，可带单个末尾 NUL
     * @return
     *  - @c ESP_OK 解析成功（忽略项见 @ref report ）
     *  - @c ESP_ERR_INVALID_ARG TOML 语法错误，或 peer 配置错误/不支持
     *  - @c ESP_ERR_NO_MEM 超出文本长度/嵌套深度/解析内存预算
     */
    esp_err_t init(std::string_view toml) noexcept;

    // ---- 诊断 ----------------------------------------------------------
    /** @brief 最近一次 @ref init 的错误码（构造成功时为 @c ESP_OK）。 */
    [[nodiscard]] esp_err_t last_error() const noexcept { return last_error_; }
    /** @brief 最近一次 @ref init 的配置诊断。 */
    [[nodiscard]] const ConfigReport& report() const noexcept { return report_; }

    // ---- 组件内部使用（不属于公开 API）--------------------------------
    /** @brief 清空诊断报告。 */
    void clear_report() noexcept;
    /** @brief 追加一条诊断（key 只记录字段路径）。 */
    void add_diagnostic(ConfigIssue issue, uint32_t line, uint32_t column,
                        std::string_view key) noexcept;
    /** @brief 记录错误码（保留首个非 @c ESP_OK 的值）。 */
    void set_error(esp_err_t err) noexcept;
    /** @brief 标记 instance_name 由配置显式给出，使身份推导不再覆盖它。 */
    void mark_instance_name_explicit() noexcept { instance_name_explicit_ = true; }

    [[nodiscard]] std::string_view instance_name_view() const noexcept {
        return instance_name.view();
    }
    [[nodiscard]] std::string_view hostname_view() const noexcept { return hostname.view(); }
    [[nodiscard]] std::string_view network_name_view() const noexcept {
        return network_name.view();
    }
    [[nodiscard]] std::string_view network_secret_view() const noexcept {
        return network_secret.view();
    }
    /** @brief 在 peer 通告其它子网之前使用的 dhcp 子网。 */
    [[nodiscard]] uint32_t dhcp_subnet_or_default() const noexcept {
        return dhcp_has_subnet ? dhcp_subnet : kDefaultDhcpSubnet;
    }

    // ---- 生效取值（由 Config 自己维护；应用只读使用）------------------
    /// 实例名（缺省取 hostname）。长度无固定上限，按需分配。
    TextBuffer instance_name{};
    /// 主机名（缺省 Espressif）。长度无固定上限；规范化后最多 32 个码点。
    TextBuffer hostname{};
    std::array<uint8_t, 16> instance_id{};                        ///< 实例 UUID 字节
    bool has_instance_id = false;  ///< 区分未指定 UUID 与显式全零 UUID

    TextBuffer network_name{};      ///< 网络名（长度无固定上限）
    TextBuffer network_secret{};    ///< 网络密钥（长度无固定上限）

    PeerEndpointList peers{};  ///< 入口地址列表（条数由配置决定，无固定上限）

    bool has_static_ipv4 = false;  ///< 是否配置了静态虚拟 IPv4
    uint32_t static_ipv4 = 0;      ///< 静态虚拟 IPv4（数值表示）
    uint8_t static_prefix = 24;    ///< 静态前缀长度

    bool dhcp = false;                          ///< 是否启用虚拟地址自分配
    bool dhcp_has_subnet = false;               ///< 是否显式配置了 dhcp 子网
    uint32_t dhcp_subnet = kDefaultDhcpSubnet;  ///< dhcp 子网网络号
    uint8_t dhcp_prefix = kDefaultDhcpPrefix;   ///< dhcp 子网前缀长度

    /** @brief 是否对数据与 RPC 报文做 AES-128-GCM 加密（上游 `flags.enable_encryption`）。 */
    bool enable_encryption = true;
    uint16_t mtu = kDefaultMtu;  ///< 虚拟网卡 MTU，取值 576..1380

private:
    /**
     * @brief 复位为缺省值（不补齐身份，由 @ref finalize_identity 负责）。
     * @note 入口列表只清空内容、保留已分配容量。
     */
    void reset_to_defaults() noexcept;

    /**
     * @brief 补齐实例身份（构造与每次 @ref init 之后自动调用）。
     * @details 规则与用户约定一致：
     *          1. hostname 为空时回退 @ref kDefaultHostname ；
     *          2. instance_name 未显式配置时取 hostname；
     *          3. instance_id 未配置时生成随机 UUID v4（显式 UUID 含全零都保留）。
     */
    void finalize_identity() noexcept;

    ConfigReport report_{};        ///< 最近一次解析的诊断
    esp_err_t last_error_ = ESP_OK;  ///< 最近一次解析的错误码
    bool instance_name_explicit_ = false;  ///< instance_name 是否由配置显式给出
};

// ---------------------------------------------------------------------------
// 运行期状态与事件
// ---------------------------------------------------------------------------

/**
 * @brief 当前生效配置的诊断快照。
 *
 * @details 用固定缓冲区承载，不引用组件内部对象，可以安全地跨任务拷贝与打印——
 *          联调时"实际生效的配置到底是什么"往往是最关键的信息。入口数量本身没有
 *          上限，本结构只快照前 @ref kConfigInfoMaxPeers 条，因此额外提供
 *          @ref peer_total_count 与 @ref peers_truncated 。
 */
struct ConfigInfo {
    /** @brief 一个入口地址（已格式化为 host:port）。 */
    struct Peer {
        std::array<char, 200> text{};  ///< "host:port"
    };

    std::array<char, 64> instance_name{};  ///< 实例名
    std::array<char, 64> hostname{};       ///< 主机名（可能为空）
    std::array<char, 64> network_name{};   ///< 网络名
    std::array<char, 37> instance_id{};    ///< 实例 UUID 文本
    std::array<Peer, kConfigInfoMaxPeers> peers{};  ///< 入口列表（最多 kConfigInfoMaxPeers 条）
    size_t peer_count = 0;         ///< @ref peers 中已填充的条数
    size_t peer_total_count = 0;   ///< 实际配置的入口总条数（可能大于 peer_count）
    bool peers_truncated = false;  ///< 入口总条数是否超过 @ref peers 的容量
    bool has_static_ipv4 = false;          ///< 是否配置了静态虚拟 IPv4
    uint32_t static_ipv4 = 0;              ///< 静态虚拟 IPv4（数值表示）
    uint8_t static_prefix = 24;            ///< 静态前缀长度
    bool dhcp = false;                     ///< 是否启用虚拟地址自分配
    std::array<char, 24> dhcp_subnet{};    ///< dhcp 子网文本（"10.126.126.0/24"）
    bool enable_encryption = true;         ///< 数据与 RPC 报文是否加密
    uint16_t mtu = kDefaultMtu;            ///< 虚拟网卡 MTU
};

/**
 * @brief 实例生命周期状态。
 *
 * @details 正常流程为：
 * @code
 * Uninitialized → Stopped → WaitingUnderlay → Connecting
 *               → Handshaking → Syncing → Ready
 * @endcode
 * 物理网络断开或连接失败会回到 WaitingUnderlay / Connecting 重试；
 * stop() 期间为 Stopping，之后回到 Stopped。
 */
enum class State : uint8_t {
    Uninitialized,    ///< 尚未成功配置
    Stopped,          ///< 已配置但未启动
    WaitingUnderlay,  ///< 等待应用通知物理网络就绪
    Connecting,       ///< 正在连接入口 peer
    Handshaking,      ///< 正在握手
    Syncing,          ///< 已握手，正在同步路由
    Ready,            ///< 组网可用
    Stopping,         ///< 正在停止
    Error,            ///< 处于错误状态（详见 Status::last_error）
};

/** @brief 事件类型。 */
enum class EventType : uint8_t {
    Started,       ///< start() 已创建运行资源
    PeerUp,        ///< 一个入口 peer 握手完成
    NetworkReady,  ///< 握手与有效路由同步完成，组网可用
    PeerDown,      ///< 一个直连 peer 断开
    Error,         ///< 发生错误
    Stopped,       ///< 已停止
};

/**
 * @brief 实例状态快照。
 *
 * @note 由 Inst::get_status() 返回调用方副本，不引用组件内部对象。
 */
struct Status {
    State state = State::Uninitialized;  ///< 当前状态
    uint32_t peer_id = 0;                ///< 本节点 peer id，0 表示尚未分配
    /// 虚拟 IPv4，按四段地址的数值表示（例如 192.168.1.1 表示为 0xC0A80101）。
    uint32_t virtual_ipv4 = 0;
    uint8_t prefix_len = 24;                  ///< 虚拟网段前缀长度
    size_t direct_peer_count = 0;             ///< 已握手的直连 peer 数
    size_t reachable_route_count = 0;         ///< 可达路由条数
    esp_err_t last_error = ESP_OK;            ///< 最近一次错误码
    uint64_t dropped_packets = 0;             ///< 因无路由/认证失败等原因丢弃的报文数
};

/** @brief 一次事件回调的内容。 */
struct Event {
    EventType type = EventType::Error;  ///< 事件类型
    Status status{};                    ///< 事件发生时的状态快照
    uint32_t peer_id = 0;               ///< 相关 peer（PeerUp/PeerDown 有效）
};

/**
 * @brief 事件回调。
 *
 * @warning 回调在控制面任务（`et_proto`）上下文中**同步**执行，必须快速返回；
 * 回调内可以调用 Inst::get_status()/Inst::get_config_info()/Inst::set_underlay_ready()，
 * 但**禁止**调用会等待任务的 Inst::stop()/Inst::deinit()（会死锁），详见
 * @ref Inst 的"线程与回调约束"。
 * @param event 事件内容，仅在回调期间有效
 * @param user_data 注册时传入的用户上下文
 */
using EventCallback = void (*)(const Event& event, void* user_data) noexcept;

/** @brief 运行期选项。 */
struct RuntimeOptions {
    EventCallback on_event = nullptr;  ///< 事件回调，可为空
    /// 回调上下文；从 start() 到 stop() 返回期间必须保持有效（stop 返回后即无回调）。
    void* user_data = nullptr;
};

// ---------------------------------------------------------------------------
// 全局单实例接口
// ---------------------------------------------------------------------------

/**
 * @brief 全局单实例的运行期接口。
 *
 * @details 本项目只支持**一个** EasyTier 实例，因此不再提供实例类：所有运行期接口都是
 *          @c et::Inst 下的自由函数，内部状态是文件级静态数据。
 *
 * 典型调用顺序：
 * @code
 * static et::Config config;      // 或 static et::Config config(kTomlText);
 *
 * // 应用已完成 NVS / esp-netif / 事件循环初始化，并连上 WiFi。
 * ESP_ERROR_CHECK(config.init(kTomlText));
 * for (size_t i = 0; i < config.report().count; ++i) { ...打印被忽略的配置项... }
 * ESP_ERROR_CHECK(et::Inst::init(config));
 * ESP_ERROR_CHECK(et::Inst::start());
 * ESP_ERROR_CHECK(et::Inst::set_underlay_ready(true));
 * // 退出前（本接口没有析构函数，必须显式释放）：
 * ESP_ERROR_CHECK(et::Inst::stop());
 * ESP_ERROR_CHECK(et::Inst::deinit());
 * @endcode
 *
 * @section thread_safety 线程与回调约束
 *
 * | 接口 | 可从任意任务调用 | 允许在事件回调中调用 | 说明 |
 * | --- | --- | --- | --- |
 * | @ref get_status / @ref get_config_info | 是 | 是 | 只读原子量与不可变配置，返回副本 |
 * | @ref set_underlay_ready | 是 | 是 | 非阻塞（队列投递），物理网络事件里直接调用 |
 * | @ref init | 仅初始化阶段 | 否 | 全局只有一个实例 |
 * | @ref start / @ref stop / @ref deinit | 同一任务串行 | **否** | 会等待任务，见下方死锁说明 |
 *
 * 规则：
 * - **生命周期接口必须串行**：不要并发调用 start/stop/deinit。
 * - **事件回调运行在控制面任务（`et_proto`）里**，是同步回调（不是投递到应用队列）。
 *   因此在回调内调用会等待组件任务的接口都不安全：`stop()` 会等到超时并返回
 *   @c ESP_ERR_TIMEOUT（它等待的正是当前任务）。
 *   回调也**不要**长时间阻塞，否则会拖慢握手、保活与路由同步。
 * - `get_status()`/`get_config_info()`/`set_underlay_ready()` 都只读原子量或做零等待
 *   入队，因此**在回调内或任意任务中调用都安全**。
 * - `stop()` 正常返回后，任务已全部退出，**不会再产生任何事件回调**；此时才可释放
 *   @ref RuntimeOptions::user_data 指向的对象。
 * - 因为不再有实例对象，**没有析构函数帮你收尾**：应用必须显式调用 @ref stop 与
 *   @ref deinit （例如在关机或重启流程里）。
 */
namespace Inst {

/**
 * @brief 用配置初始化全局实例（不启动任务）。
 * @param config 已构造/已解析的配置；入口列表会被深拷贝
 * @param options 事件回调等运行期选项
 * @return
 *  - @c ESP_OK 配置有效
 *  - @c ESP_ERR_INVALID_ARG 配置非法；配置自身的解析错误也会在此原样返回
 *  - @c ESP_ERR_NO_MEM 上下文或入口列表分配失败
 *  - @c ESP_ERR_INVALID_STATE 已经初始化过（必须先 deinit）
 * @note 若 @p config 的 @ref Config::last_error 非 @c ESP_OK ，直接返回该错误，
 *       避免把一份解析失败的配置带进运行期。
 */
esp_err_t init(const Config& config, const RuntimeOptions& options = {}) noexcept;

/**
 * @brief 启动实例（异步）。
 *
 * @note 返回成功只表示运行资源已创建，并不代表组网可用；组网可用会通过
 * @ref EventType::NetworkReady 事件或 @ref State::Ready 状态体现。
 * @return @c ESP_OK 或 @c ESP_ERR_INVALID_STATE（未初始化/重复启动）
 */
esp_err_t start() noexcept;

/**
 * @brief 通知组件物理网络（WiFi/以太网）是否可用。
 *
 * @details 该接口非阻塞、可从事件回调中调用。物理网络断开时组件会撤销
 * 连接与路由，恢复后自动重连并重新同步。
 * @param ready true 表示物理网络已就绪
 * @return @c ESP_OK，或 @c ESP_ERR_INVALID_STATE、队列满错误
 */
esp_err_t set_underlay_ready(bool ready) noexcept;

/**
 * @brief 读取状态快照。
 * @param out 输出参数
 * @return @c ESP_OK 或 @c ESP_ERR_INVALID_ARG（out 无效）
 */
esp_err_t get_status(Status& out) noexcept;

/**
 * @brief 取回当前生效配置的诊断快照（仅用于日志/诊断）。
 * @return 未成功初始化时返回 @c ESP_ERR_INVALID_STATE
 */
esp_err_t get_config_info(ConfigInfo& out) noexcept;

/**
 * @brief 停止实例（幂等）。
 * @param timeout_ms 等待任务退出的最长时间
 * @return
 *  - @c ESP_OK 已停止
 *  - @c ESP_ERR_TIMEOUT 超时；内部资源仍然保留，可重试 stop()
 *  - @c ESP_ERR_INVALID_STATE 未初始化
 */
esp_err_t stop(uint32_t timeout_ms = 5000) noexcept;

/**
 * @brief 释放已停止实例的资源并清除密钥。
 * @return @c ESP_OK，或 @c ESP_ERR_INVALID_STATE（尚未停止或未初始化）
 */
esp_err_t deinit() noexcept;

}  // namespace Inst

}  // namespace et
