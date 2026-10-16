/**
 * @file easytier-esp.h
 * @brief EasyTier-ESP 公开接口：把本设备接入一个已有的 EasyTier 虚拟网络。
 *
 * @details
 * - 公开接口仅提供 C++20 API，不提供 C 包装层；内部实现位于 @c namespace et。
 * - **只有一个公开类 @ref et::Inst**：用 TOML 文本构造即完成全部设置（组件**不接受
 *   文件路径**，应用自己读取文件后把文本交给构造函数），析构即停止任务并释放资源。
 * - 没有独立的配置类：全部设置项都是 @ref et::Inst 的私有实现细节，应用只能在构造后
 *   通过 @ref et::Inst::report / @ref et::Inst::get_config_info 读取诊断快照。
 * - **允许多个实例并存**（组件内部没有全局单实例状态）。
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
    SyntaxError,           ///< TOML 语法错误
    InvalidValue,          ///< 已支持字段的取值非法
    MissingRequiredValue,  ///< 缺少必需字段
    ResourceLimit,         ///< 超出文本长度/嵌套深度/节点数/内存预算等容量限制
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

    /** @brief 清空全部诊断（count / omitted_count 归零）。 */
    void clear() noexcept;
    /**
     * @brief 追加一条诊断；超出 @ref kMaxDiagnostics 时只增加 @ref omitted_count 。
     * @param key 字段路径（只记录路径，绝不记录取值）
     */
    void add(ConfigIssue issue, uint32_t line, uint32_t column,
             std::string_view key) noexcept;
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
 * @note 由 @ref Inst::get_status() 返回调用方副本，不引用组件内部对象。
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
 * 回调内可以调用 @ref Inst::get_status / @ref Inst::get_config_info /
 * @ref Inst::set_underlay_ready，但**禁止**调用会等待任务的 @ref Inst::stop 或
 * 析构该实例（会死锁），详见 @ref Inst 的"线程与回调约束"。
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
// 运行期实例
// ---------------------------------------------------------------------------

/**
 * @brief 一个 EasyTier 运行实例（RAII，也是本组件唯一的公开类）。
 *
 * @details
 * - **构造即初始化**：解析 TOML 文本、补齐实例身份、校验结构、在堆上分配运行上下文并
 *   派生密钥（不创建任务）。TOML 是唯一配置入口，应用自己读取文件后把文本交给构造函数；
 * - **析构即释放**：若已 @ref start 则先做无限等待的停止，再释放队列/连接/AEAD 并清零密钥；
 * - **设置项没有独立的配置类**：全部字段是实例的私有实现细节，构造后不可修改；应用只能
 *   通过 @ref report / @ref get_config_info 读取诊断快照；
 * - **允许多个实例并存**：组件内部没有全局单实例状态，每个对象独占自己的上下文、队列、
 *   任务、socket、虚拟网卡与事件回调。常驻内存、任务与连接槽按实例数量叠加，多实例时
 *   应用需自行避免虚拟 IP/网段冲突；
 * - 不可拷贝、不可移动：任务与回调持有上下文地址；
 * - 组件禁用异常，构造不能返回错误码：用 @ref valid / @ref last_error / @ref report 检查。
 *
 * 典型调用顺序：
 * @code
 * // text 由应用读取（或像示例那样用 EMBED_TXTFILES 内嵌）；组件不接受文件路径。
 * // 构造会解析 TOML 并分配约 91 KiB 上下文，请放在栈足够大的任务里。
 * static std::optional<et::Inst> inst;      // 存储方式由应用决定
 * inst.emplace(kTomlText, options);
 * if (!inst->valid()) {
 *     // 解析/校验失败：report() 里有字段路径级别的诊断
 *     inst.reset();
 *     return;
 * }
 * for (size_t i = 0; i < inst->report().count; ++i) { ...打印被忽略的配置项... }
 * ESP_ERROR_CHECK(inst->start());
 * ESP_ERROR_CHECK(inst->set_underlay_ready(true));
 * // 退出前：可直接让对象析构（自动停止并释放），也可先显式 stop() 观察超时。
 * ESP_ERROR_CHECK(inst->stop());
 * inst.reset();
 * @endcode
 *
 * @section thread_safety 线程与回调约束
 *
 * | 接口 | 可从任意任务调用 | 允许在事件回调中调用 | 说明 |
 * | --- | --- | --- | --- |
 * | @ref get_status / @ref get_config_info / @ref report / @ref last_error | 是 | 是 | 只读原子量与不可变数据，返回副本/引用 |
 * | @ref set_underlay_ready | 是 | 是 | 非阻塞（队列投递），物理网络事件里直接调用 |
 * | 构造 | 仅初始化阶段（大栈任务） | 否 | 会解析 TOML 并分配上下文 |
 * | @ref start / @ref stop / 析构 | 同一任务串行 | **否** | 会等待任务，见下方死锁说明 |
 *
 * 规则：
 * - **同一实例的生命周期接口必须串行**：不要并发调用 start/stop，也不要在别的任务正在
 *   调用它们时销毁对象；不同实例之间互不影响。
 * - **事件回调运行在控制面任务（`et_proto`）里**，是同步回调（不是投递到应用队列）。
 *   因此在回调内调用会等待组件任务的接口都不安全：`stop()` 会等到超时并返回
 *   @c ESP_ERR_TIMEOUT（它等待的正是当前任务），析构则会**永久阻塞**。
 *   回调也**不要**长时间阻塞，否则会拖慢握手、保活与路由同步。
 * - `get_status()`/`get_config_info()`/`set_underlay_ready()` 都只读原子量或做零等待
 *   入队，因此**在回调内或任意任务中调用都安全**。
 * - `stop()` 正常返回后，任务已全部退出，**不会再产生任何事件回调**；此时才可释放
 *   @ref RuntimeOptions::user_data 指向的对象。
 * - 构造会解析 TOML（递归下降），并且对象自身含约 4.5 KiB 的诊断报告，**不要在小栈任务
 *   （如 WiFi 事件任务）里构造实例**。
 */
class Inst final {
public:
    /**
     * @brief 用 TOML 文本构造实例（不启动任务）。
     * @param toml TOML 文本，可带 EMBED_TXTFILES 追加的单个末尾 NUL；内部非法 NUL 仍拒绝
     * @param options 事件回调等运行期选项
     * @note 构造无法返回错误码。失败（TOML 语法/peer 错误、结构校验失败、上下文分配失败）
     *       时对象 @ref valid 为 false，原因见 @ref last_error ，诊断见 @ref report 。
     */
    explicit Inst(std::string_view toml, const RuntimeOptions& options = {}) noexcept;

    /**
     * @brief 析构：停止实例并释放全部资源、清零密钥。
     * @warning 若实例仍在运行，析构会先做**无限等待**的停止（等价于
     *          stop(UINT32_MAX)）。事件回调运行在控制面任务里，因此**禁止在事件回调中
     *          析构**，否则永久阻塞。
     */
    ~Inst() noexcept;

    /** @brief 不可复制：实例持有任务与 socket 所有权。 */
    Inst(const Inst&) = delete;
    /** @brief 不可复制赋值。 */
    Inst& operator=(const Inst&) = delete;
    /** @brief 不可移动：任务与回调持有上下文地址，移动会使其悬空。 */
    Inst(Inst&&) = delete;
    /** @brief 不可移动赋值。 */
    Inst& operator=(Inst&&) = delete;

    /** @brief 构造是否成功（等价于 @ref last_error 为 @c ESP_OK ）。 */
    [[nodiscard]] bool valid() const noexcept {
        return impl_ != nullptr && last_error_ == ESP_OK;
    }
    /** @brief 构造阶段的错误码；构造成功后的运行期错误见 @ref Status::last_error 。 */
    [[nodiscard]] esp_err_t last_error() const noexcept { return last_error_; }
    /**
     * @brief 最近一次 TOML 解析的诊断（构造失败时为出错原因，构造成功时为被忽略的字段）。
     * @note 返回内部引用，仅在实例存活期间有效；只记录字段路径，不含任何敏感取值。
     */
    [[nodiscard]] const ConfigReport& report() const noexcept { return report_; }

    /**
     * @brief 启动实例（异步）。
     *
     * @note 返回成功只表示运行资源已创建，并不代表组网可用；组网可用会通过
     * @ref EventType::NetworkReady 事件或 @ref State::Ready 状态体现。
     * @return @c ESP_OK 或 @c ESP_ERR_INVALID_STATE（构造失败/重复启动）
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
     * @return @c ESP_OK，或 @c ESP_ERR_INVALID_STATE（构造失败）
     */
    esp_err_t get_status(Status& out) const noexcept;

    /**
     * @brief 取回当前生效配置的诊断快照（仅用于日志/诊断）。
     * @return 构造失败时返回 @c ESP_ERR_INVALID_STATE
     */
    esp_err_t get_config_info(ConfigInfo& out) const noexcept;

    /**
     * @brief 停止实例（幂等）。
     * @param timeout_ms 等待任务退出的最长时间
     * @return
     *  - @c ESP_OK 已停止
     *  - @c ESP_ERR_TIMEOUT 超时；内部资源仍然保留，可重试 stop()
     *  - @c ESP_ERR_INVALID_STATE 构造失败
     */
    esp_err_t stop(uint32_t timeout_ms = 5000) noexcept;

private:
    /** @brief 私有实现：运行上下文（含全部设置项）+ 运行标记（定义在 inst.cpp）。 */
    struct Impl;

    Impl* impl_ = nullptr;      ///< 堆上的上下文（92,800 B ≈ 91 KiB，建议启用 PSRAM）
    ConfigReport report_{};     ///< 最近一次 TOML 解析诊断（约 4.5 KiB）
    esp_err_t last_error_ = ESP_ERR_INVALID_STATE;  ///< 构造阶段错误码
};

}  // namespace et
