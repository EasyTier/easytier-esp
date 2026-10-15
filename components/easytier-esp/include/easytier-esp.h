/**
 * @file easytier-esp.h
 * @brief EasyTier-ESP 公开接口：把本设备接入一个已有的 EasyTier 虚拟网络。
 *
 * @details
 * - 公开接口仅提供 C++20 API，不提供 C 包装层；内部实现位于 @c namespace et。
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
#include <span>
#include <string_view>

#include "esp_err.h"

namespace et {

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
/** @brief 虚拟网卡默认 MTU（与上游默认值一致）。 */
inline constexpr uint16_t kDefaultMtu = 1380;

/**
 * @brief 编程式配置。
 *
 * @details 所有视图（string_view / span）只在 init() 调用期间被读取，init()
 * 会把内容复制到内部固定缓冲区，因此调用返回后即可释放这些视图指向的对象。
 */
struct Config {
    std::string_view instance_name = "default";  ///< 实例名，仅用于日志与状态展示
    std::string_view hostname = "Espressif";     ///< 主机名，缺省使用 ESP 平台回退名
    std::string_view instance_id{};              ///< 实例 UUID 文本，空表示随机生成
    std::string_view network_name = "default";   ///< 网络名，必须与对端一致
    std::string_view network_secret{};           ///< 网络密钥，缺省为空字符串
    /// 入口地址，形如 "tcp://host:port"。条目数量没有上限；同一时刻实际建立的连接数
    /// 由 Kconfig 的 `EASYTIER_ESP_MAX_CONNECTED_PEERS`（默认 6）决定，其余轮换备用。
    std::span<const std::string_view> peers{};
    std::string_view virtual_ipv4{};             ///< 静态虚拟 IPv4，可带 "/前缀"；非空时忽略 dhcp
    bool dhcp = false;                           ///< 未配置静态地址时按上游语义自分配虚拟 IPv4
    std::string_view dhcp_subnet{};              ///< dhcp 子网，空表示使用上游默认 10.126.126.0/24
    uint16_t mtu = kDefaultMtu;                  ///< 虚拟网卡 MTU，取值 576..1380
    /**
     * @brief 是否对数据与 RPC 报文做 AES-128-GCM 加密（与上游 `flags.enable_encryption` 同义）。
     * @details 缺省开启。关闭后发送的报文不带 AEAD 尾也不置加密位，并且会拒绝收到的
     *          已加密报文（与上游 `NullCipher` 语义一致）；收到的明文报文在开启时同样接受。
     */
    bool enable_encryption = true;
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
 * @brief 当前生效配置的诊断快照。
 *
 * @details 用固定缓冲区承载，不引用 @ref Instance 内部对象，可以安全地跨任务拷贝与
 *          打印——联调时"实际生效的配置到底是什么"往往是最关键的信息（例如 TOML 里
 *          写了 ipv4 又被 dhcp 覆盖、前缀被规范成 /24、入口地址被忽略等）。
 *          入口数量本身没有上限，本结构只快照前 @ref kConfigInfoMaxPeers 条，
 *          因此额外提供 @ref peer_total_count 与 @ref peers_truncated 。
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
 * @brief 实例状态快照。
 *
 * @note 由 get_status() 返回调用方副本，不引用组件内部对象。
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
 * 回调内可以调用 get_status()/get_config_info()/set_underlay_ready()，但
 * **禁止**调用会等待任务的 stop()/deinit()/析构（会死锁），详见 @ref Instance
 * 的"线程与回调约束"。
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
 * @note 该结构约 4.5 KiB，建议使用静态存储；超过 @ref kMaxDiagnostics 条的诊断
 * 会被计数到 @ref omitted_count，不会因为报告容量不足而拒绝配置。
 */
struct ConfigReport {
    std::array<ConfigDiagnostic, kMaxDiagnostics> items{};  ///< 诊断列表
    size_t count = 0;                                       ///< items 中有效条数
    size_t omitted_count = 0;                               ///< 因容量不足被省略的条数
};

/**
 * @brief 单个 EasyTier 实例。
 *
 * @details 不可复制、不可移动。首版只允许同时存在一个已初始化的实例，
 * 第二个实例调用 init*() 会得到 @c ESP_ERR_INVALID_STATE。
 *
 * 典型的应用侧调用顺序：
 * @code
 * static et::Instance instance;
 * static et::ConfigReport report;
 *
 * // 应用已完成 NVS / esp-netif / 事件循环初始化，并连上 WiFi。
 * ESP_ERROR_CHECK(instance.init_from_toml(kTomlText, {}, &report));
 * ESP_ERROR_CHECK(instance.start());
 * ESP_ERROR_CHECK(instance.set_underlay_ready(true));
 * // 退出前：
 * ESP_ERROR_CHECK(instance.stop());
 * ESP_ERROR_CHECK(instance.deinit());
 * @endcode
 *
 * @section thread_safety 线程与回调约束
 *
 * | 接口 | 可从任意任务调用 | 允许在事件回调中调用 | 说明 |
 * | --- | --- | --- | --- |
 * | @ref get_status / @ref get_config_info | 是 | 是 | 只读原子量与不可变配置，返回副本 |
 * | @ref set_underlay_ready | 是 | 是 | 非阻塞（队列投递），物理网络事件里直接调用 |
 * | @ref init / @ref init_from_toml / @ref init_from_file | 仅初始化阶段 | 否 | 全组件只允许一个实例 |
 * | @ref start / @ref stop / @ref deinit | 同一任务串行 | **否** | 会等待任务，见下方死锁说明 |
 * | 析构 | 否 | **否** | 会无限等待任务退出 |
 *
 * 规则：
 * - **生命周期接口必须串行**：不要并发调用 start/stop/deinit，也不要在别的任务正在
 *   调用它们时销毁实例。
 * - **事件回调运行在控制面任务（`et_proto`）里**，是同步回调（不是投递到应用队列）。
 *   因此在回调内调用会等待组件任务的接口都不安全：`stop()` 会等到超时并返回
 *   @c ESP_ERR_TIMEOUT（它等待的正是当前任务），带无限超时的析构则会**永久阻塞**。
 *   回调也**不要**长时间阻塞，否则会拖慢握手、保活与路由同步。
 * - `get_status()`/`get_config_info()`/`set_underlay_ready()` 都只读原子量或做零等待
 *   入队，因此**在回调内或任意任务中调用都安全**。
 * - 回调中可以做：读取 @ref Event 内容、调用 @ref get_status / @ref get_config_info、
 *   调用 @ref set_underlay_ready，以及投递消息给应用自己的任务。
 * - `stop()` 正常返回后，任务已全部退出，**不会再产生任何事件回调**；此时才可释放
 *   @ref RuntimeOptions::user_data 指向的对象。
 */
class Instance final {
public:
    /** @brief 构造一个未初始化的实例。 */
    Instance() noexcept;
    /**
     * @brief 析构实例。
     * @warning 析构会请求停止并等待所有任务退出（没有超时），因此如果任务因
     * 内部故障无法退出，析构会一直等待；建议应用显式调用 stop() 以便观察超时。
     */
    ~Instance() noexcept;

    /** @brief 不可复制：实例持有任务与 socket 所有权。 */
    Instance(const Instance&) = delete;
    /** @brief 不可复制赋值。 */
    Instance& operator=(const Instance&) = delete;
    /** @brief 不可移动：任务/回调持有实例地址，移动会使其悬空。 */
    Instance(Instance&&) = delete;
    /** @brief 不可移动赋值。 */
    Instance& operator=(Instance&&) = delete;

    /**
     * @brief 使用编程式配置初始化。
     * @param config 配置内容，所有视图会被复制
     * @param options 事件回调等运行期选项
     * @return
     *  - @c ESP_OK 配置有效
     *  - @c ESP_ERR_INVALID_ARG 字段非法（长度、IPv4、MTU、入口数量等）
     *  - @c ESP_ERR_INVALID_STATE 已有实例被初始化
     */
    esp_err_t init(const Config& config, const RuntimeOptions& options = {}) noexcept;

    /**
     * @brief 使用与上游 EasyTier 相同格式的 TOML 文本初始化。
     * @details 非 peer 字段仅提取已支持且有效的内容；无法采用时忽略并保留默认值。
     *          peer 必须为受支持的 TCP 入口；类型错误、缺 uri、无效/不支持的 URI
     *          或超过入口数量上限均报错。空文档与缺少入口/虚拟地址均可加载，加载成功
     *          不代表网络可用。与严格校验的编程式 @ref init 路径不同。
     * @param text TOML 文本；可带 EMBED_TXTFILES 追加的单个末尾 NUL
     * @param options 事件回调等运行期选项
     * @param report 可为空的诊断输出；调用开始时会先被清空
     * @return
     *  - @c ESP_OK 解析成功并采用可用字段（忽略项见 report）
     *  - @c ESP_ERR_INVALID_ARG TOML 语法错误或 peer 配置错误/不支持/超过入口上限
     *  - @c ESP_ERR_NO_MEM 超出文本长度/嵌套深度/解析内存预算
     *  - @c ESP_ERR_INVALID_STATE 已有实例被初始化
     */
    esp_err_t init_from_toml(std::string_view text, const RuntimeOptions& options = {},
                             ConfigReport* report = nullptr) noexcept;

    /**
     * @brief 从已挂载的 VFS 路径读取 TOML 配置并初始化。
     * @param path 例如 "/spiffs/easytier.toml"；组件不负责挂载文件系统
     * @param options 事件回调等运行期选项
     * @param report 可为空的诊断输出
     * @return 除 @ref init_from_toml 的错误码外，还可能返回
     *         @c ESP_ERR_NOT_FOUND（文件不存在）或 @c ESP_ERR_INVALID_SIZE（文件过大）
     */
    esp_err_t init_from_file(std::string_view path, const RuntimeOptions& options = {},
                             ConfigReport* report = nullptr) noexcept;

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
    esp_err_t get_status(Status& out) const noexcept;

    /**
     * @brief 取回当前生效配置的诊断快照（仅用于日志/诊断）。
     * @return 未成功初始化时返回 @c ESP_ERR_INVALID_STATE
     */
    esp_err_t get_config_info(ConfigInfo& out) const noexcept;

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
     * @return @c ESP_OK，或 @c ESP_ERR_INVALID_STATE（尚未停止）
     */
    esp_err_t deinit() noexcept;

private:
    struct Impl;
    Impl* impl_ = nullptr;

    /**
     * @brief 三个 init* 入口共用的初始化路径。
     * @param stored_config 指向内部 StoredConfig 的指针（为避免公开内部类型而用 void*）；
     *        入口列表的所有权会被移走，因此必须传非 const 指针
     * @note 仅组件内部使用，不属于公开 API。
     */
    esp_err_t init_from_internal(void* stored_config,
                                 const RuntimeOptions& options) noexcept;
};

}  // namespace et
