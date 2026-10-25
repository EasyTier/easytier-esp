/**
 * @file easytier-esp.hpp
 * @brief 一个 EasyTier 协议的 ESP32 轻量化实现。把 ESP32 接入既有 EasyTier 虚拟网络的客户端组件（ESP-IDF 6.1.x，C++20）。
 * @author Myqfeng
 * @version 0.0.1
 * @date 2025-10-17
 * @copyright Copyright (c) 2026 Myqfeng
 * 
 * @note
 * 开源地址：
 * - https://cnb.cool/myqfeng/qteasytier/easytier-esp
 * - https://github.com/EasyTier/easytier-esp
 * 
 * 本组件对内存有一定要求，建议开启PSRAM，当出现问题时，可以考虑在 KConfig 中调整有关选项。
 *
 * 基本用法：
 *   1. 用一段 TOML 文本构造 @ref et::Inst；
 *   2. @ref et::Inst::valid 为 true 后调用 @ref et::Inst::start；
 *   3. WiFi/以太网取得 IP 后调用 @ref et::Inst::connect，网络断开时调用
 *      @ref et::Inst::disconnect；
 *   4. @ref et::Inst::stop 或析构释放资源。
 *
 * TOML 是唯一配置入口（不提供编程式 setter）；未支持的字段会被忽略，支持字段取值
 * 非法会导致构造失败。多个实例可以并存。
 */
#ifndef EASYTIER_ESP_HPP_
#define EASYTIER_ESP_HPP_

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

/** @brief 虚拟 MTU 下限。 */
inline constexpr uint16_t kMinMtu = 576;
/** @brief 虚拟 MTU 上限与缺省值。 */
inline constexpr uint16_t kDefaultMtu = 1380;

/** @brief DHCP 自举子网（10.126.126.0/24）。 */
inline constexpr uint32_t kDefaultDhcpSubnet = 0x0A7E7E00u;  // 10.126.126.0
/** @brief DHCP 自举子网前缀长度。 */
inline constexpr uint8_t kDefaultDhcpPrefix = 24;

/** @brief 未配置 hostname 时的回退名。 */
inline constexpr std::string_view kDefaultHostname = "Espressif";
/** @brief 未配置 network_name 时的缺省网络名。 */
inline constexpr std::string_view kDefaultNetworkName = "default";

/** @brief 一个入口地址（来自 TOML 的 `[[peer]].uri`）。 */
struct PeerEndpoint {
    std::string host{};  ///< 主机名或点分 IPv4 文本
    uint16_t port = 0;   ///< 端口
};

/**
 * @brief 生效设置（@ref et::Inst::get_config_info 返回的副本）。
 *
 * 反映 TOML 应用缺省值并补齐身份后的最终取值；@ref network_secret 恒为空，
 * 可以安全打印。
 */
struct Settings {
    std::string instance_name{};                    ///< 实例名（缺省取 hostname）
    std::string hostname{};                         ///< 主机名（缺省 Espressif，最多 32 码点）
    std::array<uint8_t, 16> instance_id{};          ///< 实例 UUID 字节
    bool has_instance_id = false;                   ///< 是否已确定 UUID（内部状态）
    bool instance_name_explicit = false;            ///< instance_name 是否显式配置（内部状态）

    std::string network_name{std::string(kDefaultNetworkName)};  ///< 网络名
    std::string network_secret{};                   ///< 网络密钥（对外副本恒为空）

    std::vector<PeerEndpoint> peers{};              ///< 入口地址列表（条数无上限）

    bool has_static_ipv4 = false;                   ///< 是否配置了静态虚拟 IPv4
    uint32_t static_ipv4 = 0;                       ///< 静态虚拟 IPv4（数值表示）
    uint8_t static_prefix = 24;                     ///< 静态前缀长度

    bool dhcp = false;                              ///< 是否启用虚拟地址自分配
    bool dhcp_has_subnet = false;                   ///< 是否显式配置了 DHCP 子网（内部状态）
    uint32_t dhcp_subnet = kDefaultDhcpSubnet;      ///< DHCP 子网网络号
    uint8_t dhcp_prefix = kDefaultDhcpPrefix;       ///< DHCP 子网前缀长度

    bool enable_encryption = true;                  ///< 是否加密数据与 RPC 报文
    uint16_t mtu = kDefaultMtu;                     ///< 虚拟 MTU（576..1380）
};

// ---------------------------------------------------------------------------
// 运行期状态与事件
// ---------------------------------------------------------------------------

/**
 * @brief 实例状态。
 *
 * Stopped → WaitingUnderlay → Connecting → Handshaking → Syncing → Ready；
 * disconnect() 回到 WaitingUnderlay，stop() 回到 Stopped。
 */
enum class State : uint8_t {
    Uninitialized,    ///< 尚未成功配置
    Stopped,          ///< 已配置但未启动
    WaitingUnderlay,  ///< 等待应用调用 connect()
    Connecting,       ///< 正在连接入口节点
    Handshaking,      ///< 正在握手
    Syncing,          ///< 已握手，正在同步路由
    Ready,            ///< 组网可用
    Stopping,         ///< 正在停止
    Error,            ///< 出错（见 Status::last_error）
};

/** @brief 事件类型。 */
enum class EventType : uint8_t {
    Started,       ///< 已启动
    PeerUp,        ///< 一个入口节点握手完成
    NetworkReady,  ///< 组网可用
    PeerDown,      ///< 预留（当前不产生）
    Error,         ///< 发生错误
    Stopped,       ///< 已停止
};

/** @brief 状态快照（@ref Inst::get_status 的返回值）。 */
struct Status {
    State state = State::Uninitialized;  ///< 当前状态
    uint32_t peer_id = 0;                ///< 本节点 peer id（0 表示尚未分配）
    uint32_t virtual_ipv4 = 0;           ///< 虚拟 IPv4（数值表示，如 0xC0A80101 表示 192.168.1.1）
    uint8_t prefix_len = 24;             ///< 虚拟网段前缀长度
    size_t direct_peer_count = 0;        ///< 已握手的直连节点数
    size_t reachable_route_count = 0;    ///< 可达路由条数
    esp_err_t last_error = ESP_OK;       ///< 最近一次错误码
    uint64_t dropped_packets = 0;        ///< 丢弃的报文数
};

/** @brief 一次事件。 */
struct Event {
    EventType type = EventType::Error;  ///< 事件类型
    Status status{};                    ///< 事件发生时的状态快照
    uint32_t peer_id = 0;               ///< 相关节点（PeerUp 有效）
};

/**
 * @brief 事件回调。
 *
 * 在组件任务中同步执行，必须尽快返回；可调用只读接口与 connect()/disconnect()，
 * 不要调用 stop() 或析构实例。
 */
using EventCallback = void (*)(const Event& event, void* user_data) noexcept;

/** @brief 运行期选项。 */
struct RuntimeOptions {
    EventCallback on_event = nullptr;  ///< 事件回调，可为空
    void* user_data = nullptr;         ///< 回调上下文，在 stop() 返回前保持有效
};

// ---------------------------------------------------------------------------
// 运行期实例
// ---------------------------------------------------------------------------

/** @brief 实例内部运行上下文（仅声明）。 */
class Context;

/**
 * @brief 一个 EasyTier 实例（RAII，本组件唯一的公开类）。
 *
 * 构造解析 TOML 并分配资源，析构自动停止并释放。
 *
 * @code
 * et::RuntimeOptions options;
 * options.on_event = on_event;
 * std::optional<et::Inst> inst;
 * inst.emplace(toml_text, options);
 * if (!inst->valid()) { ...inst->last_error()... }
 * inst->start();
 * inst->connect();      // WiFi/以太网取得 IP 后
 * ...
 * inst->disconnect();   // 网络断开时
 * @endcode
 *
 * 约束：
 * - 构造 / start / stop / 析构必须串行，且应在较大栈的任务里调用；
 * - 事件回调中禁止调用 stop() 或析构实例；
 * - 不同实例互不影响。
 */
class Inst final {
public:
    /**
     * @brief 用 TOML 文本构造实例（不启动）。
     * @param toml TOML 文本
     * @param options 事件回调等运行期选项
     * @note 失败时 valid() 为 false，错误码见 last_error()；`ipv4` 内容无法解析时
     *       按未配置处理（构造不会失败）。
     */
    explicit Inst(std::string_view toml, const RuntimeOptions& options = {}) noexcept;

    /** @brief 停止并释放资源；不要在事件回调中析构。 */
    ~Inst() noexcept;

    Inst(const Inst&) = delete;
    Inst& operator=(const Inst&) = delete;
    Inst(Inst&&) = delete;
    Inst& operator=(Inst&&) = delete;

    /** @brief 构造是否成功。 */
    [[nodiscard]] bool valid() const noexcept {
        return ctx_ != nullptr && last_error_ == ESP_OK;
    }
    /** @brief 构造失败的错误码；运行期错误见 @ref Status::last_error。 */
    [[nodiscard]] esp_err_t last_error() const noexcept { return last_error_; }

    /**
     * @brief 启动实例（异步）。
     * @note 返回成功只代表资源已创建；组网可用见 NetworkReady 事件或 Ready 状态。
     * @return @c ESP_OK 或错误码
     */
    esp_err_t start() noexcept;

    /**
     * @brief 允许组件连接入口节点。
     * @note 取得 IP 后调用；WiFi 重连后需再次调用。非阻塞、幂等。
     * @return @c ESP_OK 或错误码
     */
    esp_err_t connect() noexcept;

    /**
     * @brief 断开组网：关闭连接并撤销路由。
     * @note 非阻塞、幂等；之后可再次 connect()。
     * @return @c ESP_OK 或错误码
     */
    esp_err_t disconnect() noexcept;

    /**
     * @brief 读取状态快照。
     * @param out 输出参数
     * @return @c ESP_OK 或错误码
     */
    esp_err_t get_status(Status& out) const noexcept;

    /**
     * @brief 返回生效设置副本。
     * @return @ref Settings；network_secret 恒为空
     */
    [[nodiscard]] Settings get_config_info() const noexcept;

    /**
     * @brief 停止实例（幂等）。
     * @param timeout_ms 等待退出的上限
     * @return @c ESP_OK、@c ESP_ERR_TIMEOUT（可重试）或错误码
     */
    esp_err_t stop(uint32_t timeout_ms = 5000) noexcept;

private:
    Context* ctx_ = nullptr;    ///< 内部运行上下文
    esp_err_t last_error_ = ESP_ERR_INVALID_STATE;  ///< 构造错误码
};

}  // namespace et

#endif
