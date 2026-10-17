/**
 * @file easytier-esp.h
 * @brief EasyTier-ESP 公开接口：把本设备接入一个已有的 EasyTier 虚拟网络。
 *
 * @details
 * - 公开接口仅提供 C++20 API，不提供 C 包装层；内部实现位于 @c namespace et。
 * - **只有一个公开类 @ref et::Inst**：用 TOML 文本构造即完成全部设置（组件**不接受
 *   文件路径**，应用自己读取文件后把文本交给构造函数），析构即停止任务并释放资源。
 * - 没有配置类：设置项仍由 TOML 全量构造，应用只能在构造后通过
 *   @ref et::Inst::get_config_info 取一份 @ref Settings 副本（网络密钥字段已抹除）。
 * - **允许多个实例并存**（组件内部没有全局单实例状态）。
 * - 配置格式与上游 EasyTier 核心保持一致（TOML）：**支持的字段写错**（类型/取值非法）
 *   会让构造失败并用 Error 日志说明，错误码见 @ref et::Inst::last_error ；**不支持的
 *   字段不解析也不报错**（**不缓存诊断**）。唯一例外：`ipv4` 字符串内容无法解析时
 *   静默忽略（与上游 `get_ipv4().parse().ok()` 一致）。
 * - 组件不初始化 WiFi、不挂载文件系统、不修改物理默认网卡，这些由应用负责。
 *
 * @note 首版仅适配 ESP-IDF 6.1.x。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

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

/** @brief 一个已解析的入口地址（来自 TOML 的 `[[peer]].uri`）。 */
struct PeerEndpoint {
    std::string host{};  ///< 主机名或点分 IPv4 文本
    uint16_t port = 0;   ///< 端口
};

/**
 * @brief 实例的生效设置（@ref et::Inst::get_config_info 返回的副本）。
 *
 * @details 字段与组件内部的设置模型一致：构造 `et::Inst` 时由 TOML 覆盖缺省值，
 *          再补齐身份（hostname 回退、instance_name 推导、随机 UUID v4）。
 *          @ref network_secret 是组件内部派生密钥用的字段：**对外返回的副本里
 *          始终为空**，应用拿不到真实网络密钥。
 *
 * @note 入口列表与文本字段都是 `std::string` / `std::vector`，长度没有固定上限；
 *       返回的是深拷贝，可以安全地跨任务保存与打印。
 */
struct Settings {
    std::string instance_name{};                    ///< 实例名（缺省取 hostname）
    std::string hostname{};                         ///< 主机名（缺省 Espressif，规范化后最多 32 码点）
    std::array<uint8_t, 16> instance_id{};          ///< 实例 UUID 字节
    bool has_instance_id = false;                   ///< 是否已确定 UUID（显式或随机 v4）
    bool instance_name_explicit = false;            ///< instance_name 是否由 TOML 显式给出

    std::string network_name{std::string(kDefaultNetworkName)};  ///< 网络名
    std::string network_secret{};                   ///< 网络密钥（**对外返回的副本恒为空**）

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
};

// ---------------------------------------------------------------------------
// 运行期状态与事件
// ---------------------------------------------------------------------------

/**
 * @brief 实例生命周期状态。
 *
 * @details 正常流程为：
 * @code
 * Uninitialized → Stopped → WaitingUnderlay → Connecting
 *               → Handshaking → Syncing → Ready
 * @endcode
 * disconnect() 或连接失败会回到 WaitingUnderlay / Connecting 重试；
 * stop() 期间为 Stopping，之后回到 Stopped。
 */
enum class State : uint8_t {
    Uninitialized,    ///< 尚未成功配置
    Stopped,          ///< 已配置但未启动
    WaitingUnderlay,  ///< 等待应用调用 connect()
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
 * @ref Inst::connect / @ref Inst::disconnect，但**禁止**调用会等待任务的 @ref Inst::stop 或
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
 * @brief 实例的运行上下文（组件内部 RAII 类，定义在 @c src/context.h/.cpp ）。
 * @note 这里只做前向声明：@ref Inst 以裸指针持有它，因此公开头不需要暴露
 *       FreeRTOS/lwIP/内部模块头，也不受上下文大小（约 91 KiB）影响。
 */
class Context;

/**
 * @brief 一个 EasyTier 运行实例（RAII，也是本组件唯一的公开类）。
 *
 * @details
 * - **构造即初始化**：解析 TOML 文本、补齐实例身份、校验结构、在堆上分配运行上下文并
 *   派生密钥（不创建任务）。TOML 是唯一配置入口，应用自己读取文件后把文本交给构造函数；
 * - **析构即释放**：若已 @ref start 则先做无限等待的停止，再释放队列/连接/AEAD 并清零密钥；
 * - **设置项只由 TOML 提供**：构造后不可修改；应用可用 @ref get_config_info 取一份
 *   @ref Settings 副本（网络密钥字段已抹除），不能反向修改实例的设置；
 * - **允许多个实例并存**：组件内部没有全局单实例状态，每个对象独占自己的上下文、队列、
 *   任务、socket、虚拟网卡与事件回调。常驻内存、任务与连接槽按实例数量叠加，多实例时
 *   应用需自行避免虚拟 IP/网段冲突；
 * - 不可拷贝、不可移动：任务与回调持有上下文地址；
 * - 组件禁用异常，构造不能返回错误码：用 @ref valid / @ref last_error 检查；解析失败
 *   直接输出 Error 日志（不支持的字段不解析、不报错），**不缓存诊断**。
 *
 * 典型调用顺序：
 * @code
 * // text 由应用读取（或像示例那样用 EMBED_TXTFILES 内嵌）；组件不接受文件路径。
 * // 构造会解析 TOML 并分配约 91 KiB 上下文，请放在栈足够大的任务里。
 * static std::optional<et::Inst> inst;      // 存储方式由应用决定
 * inst.emplace(kTomlText, options);
 * if (!inst->valid()) {
 *     // 失败原因已由组件以 Error 级日志输出；这里只需要看错误码
 *     ESP_LOGE(TAG, "easytier init failed: 0x%x", inst->last_error());
 *     inst.reset();
 *     return;
 * }
 * ESP_ERROR_CHECK(inst->start());
 * ESP_ERROR_CHECK(inst->connect());
 * // 退出前：可直接让对象析构（自动停止并释放），也可先显式 stop() 观察超时。
 * ESP_ERROR_CHECK(inst->stop());
 * inst.reset();
 * @endcode
 *
 * @section thread_safety 线程与回调约束
 *
 * | 接口 | 可从任意任务调用 | 允许在事件回调中调用 | 说明 |
 * | --- | --- | --- | --- |
 * | @ref get_status / @ref get_config_info / @ref last_error | 是 | 是 | 只读原子量 / 深拷贝设置，返回值与实例无共享 |
 * | @ref connect / @ref disconnect | 是 | 是 | 非阻塞（队列投递），WiFi/IP 事件里直接调用 |
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
 * - `get_status()`/`get_config_info()`/`connect()`/`disconnect()` 都只读原子量或做零等待
 *   入队，因此**在回调内或任意任务中调用都安全**。
 * - `stop()` 正常返回后，任务已全部退出，**不会再产生任何事件回调**；此时才可释放
 *   @ref RuntimeOptions::user_data 指向的对象。
 * - 构造会解析 TOML（递归下降），**不要在小栈任务（如 WiFi 事件任务）里构造实例**。
 */
class Inst final {
public:
    /**
     * @brief 用 TOML 文本构造实例（不启动任务）。
     * @param toml TOML 文本，可带 EMBED_TXTFILES 追加的单个末尾 NUL；内部非法 NUL 仍拒绝
     * @param options 事件回调等运行期选项
     * @note 构造无法返回错误码：失败（TOML 语法/peer 错误、支持字段取值非法、结构校验
     *       失败、上下文分配失败）时对象 @ref valid 为 false，原因见 @ref last_error ；
     *       具体错误已由组件以 Error 级日志输出，**不缓存诊断**。唯一例外：`ipv4`
     *       字符串内容无法解析时静默忽略（视为未配置静态地址）。
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
        return ctx_ != nullptr && last_error_ == ESP_OK;
    }
    /** @brief 构造阶段的错误码；构造成功后的运行期错误见 @ref Status::last_error 。 */
    [[nodiscard]] esp_err_t last_error() const noexcept { return last_error_; }

    /**
     * @brief 启动实例（异步）。
     *
     * @note 返回成功只表示运行资源已创建，并不代表组网可用；组网可用会通过
     * @ref EventType::NetworkReady 事件或 @ref State::Ready 状态体现。
     * @return @c ESP_OK 或 @c ESP_ERR_INVALID_STATE（构造失败/重复启动）
     */
    esp_err_t start() noexcept;

    /**
     * @brief 通知组件可以连接：开始/恢复组网。
     *
     * @details 由应用决定何时允许连接（例如 WiFi/以太网取得 IP 后调用）。在调用它
     *          之前实例一直停在 @ref State::WaitingUnderlay，不会连接任何入口节点。
     *          该接口非阻塞、可从事件回调中调用，可重复调用（幂等）。
     *          **WiFi 掉线重连后必须再次调用**，否则组件不会再连接入口。
     * @return @c ESP_OK；@c ESP_ERR_INVALID_STATE（未 start 或构造失败）；
     *         @c ESP_ERR_NO_MEM（内部命令队列满，稍后重试）
     */
    esp_err_t connect() noexcept;

    /**
     * @brief 通知组件断开：撤销组网连接与路由。
     *
     * @details 由应用决定何时断开（例如 WiFi 断开时调用）。组件会立即关闭所有入口
     *          TCP 连接、从拓扑/路由中移除对端并回到 @ref State::WaitingUnderlay；
     *          再次 @ref connect 后重新开始组网。非阻塞、可从事件回调中调用，
     *          可重复调用（幂等）。
     * @return @c ESP_OK；@c ESP_ERR_INVALID_STATE（未 start 或构造失败）；
     *         @c ESP_ERR_NO_MEM（内部命令队列满，稍后重试）
     */
    esp_err_t disconnect() noexcept;

    /**
     * @brief 读取状态快照。
     * @param out 输出参数
     * @return @c ESP_OK，或 @c ESP_ERR_INVALID_STATE（构造失败）
     */
    esp_err_t get_status(Status& out) const noexcept;

    /**
     * @brief 取回当前生效设置的一份副本。
     * @details 字段来自构造时的 TOML 覆盖 + 身份补齐（随机 UUID、hostname 回退、
     *          IPv4 归一化等），因此不等同于 TOML 原文。返回的 @ref Settings 副本里
     *          @ref Settings::network_secret 已被抹除（恒为空串），可以安全打印。
     * @return 生效设置深拷贝；构造失败时返回缺省构造的 @ref Settings
     */
    [[nodiscard]] Settings get_config_info() const noexcept;

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
    Context* ctx_ = nullptr;    ///< 堆上的运行上下文（92,248 B ≈ 91 KiB，RAII，建议启用 PSRAM）
    esp_err_t last_error_ = ESP_ERR_INVALID_STATE;  ///< 构造阶段错误码
};

}  // namespace et
