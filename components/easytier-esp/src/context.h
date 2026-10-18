/**
 * @file context.h
 * @brief 组件内部运行上下文（每个 et::Inst 对象独占一份）。
 *
 * @details socket 只属于 I/O 任务，控制面/数据面各持自己的 AEAD；跨任务只通过队列传
 *          数据、原子量传状态。队列与缓冲都是固定容量；连接代际用于丢弃旧数据。
 *          数据成员全部 private，任务实现是私有成员函数，@ref et::Inst 通过友元读快照。
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "easytier-esp.hpp"
#include "config/config.hpp"
#include "support/config_limits.hpp"
#include "crypto/aes_gcm.hpp"
#include "protocol/wire.hpp"
#include "data/plane.hpp"
#include "netif/virtual_netif.hpp"
#include "rpc/fragment.hpp"
#include "rpc/rpc.hpp"
#include "route/dhcp.hpp"
#include "route/sync_engine.hpp"
#include "route/table.hpp"
#include "route/topology.hpp"

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

namespace et {

/** @brief TCP 传输对象（仅 I/O 任务使用）。 */
class TcpConn;

/// 支持的最大并发入口连接数（Kconfig `EASYTIER_ESP_MAX_CONNECTED_PEERS`，默认 6）。
/// 配置里可以写任意多个入口，多出的入口作为轮换备用。
inline constexpr size_t kMaxConnSlots = kMaxConnectedPeers;
/// 任务数量
inline constexpr size_t kTaskCount = 3;

/** @brief 应用 → 控制面的连接控制命令。 */
enum class AppCommand : uint8_t {
    Connect,     ///< 应用允许连接
    Disconnect,  ///< 应用要求断开
};

/** @brief 控制面 → I/O 的命令。 */
enum class IoCommand : uint8_t {
    Reconnect,  ///< 立即断开并按退避重连（握手失败/超时时使用）
};

/** @brief I/O → 控制面的连接事件。 */
enum class IoEvent : uint8_t {
    Connected,     ///< TCP 已建立
    Disconnected,  ///< TCP 已断开（对端关闭、错误或应用 disconnect()）
};

#if defined(ESP_PLATFORM)
/** @brief 队列中传递的一帧（固定容量，随队列静态分配）。 */
struct FrameMessage {
    uint32_t generation = 0;  ///< 连接代际（该槽建立连接时分配）
    uint32_t to_peer = 0;     ///< 目标对端 peer id（0 = 未指定，由 I/O 按槽位选择）
    uint16_t len = 0;         ///< 有效字节数
    std::array<uint8_t, kMaxTcpBodyLen> data{};  ///< PM 头 + payload
};

/** @brief 一个连接槽的共享状态（I/O 任务管理生命周期，控制面读写 peer id）。 */
struct ConnSlot {
    TcpConn* conn = nullptr;              ///< 连接对象（堆分配，含收发缓冲）
    std::atomic<uint32_t> peer_id{0};     ///< 握手后的对端 peer id（0 = 未握手/未知）
    std::atomic<uint32_t> generation{0};  ///< 该槽当前代际（0 = 空闲）
};

/** @brief 队列中传递的小消息（事件/命令）。 */
struct SmallMessage {
    uint8_t kind = 0;         ///< IoEvent / IoCommand / AppCommand
    uint8_t slot = 0;         ///< 涉及的连接槽（0..kMaxConnSlots-1）
    uint32_t generation = 0;  ///< 连接代际（事件使用）
};

/// I/O 任务重连退避起点（毫秒）。
inline constexpr uint32_t kInitialBackoffMs = 1000;
/// I/O 任务重连退避上限（毫秒）。
inline constexpr uint32_t kMaxBackoffMs = 30000;

/** @brief 一个连接槽的 I/O 任务私有状态（仅 I/O 任务访问）。 */
struct SlotState {
    size_t peer_index = 0;                        ///< 该槽当前使用的配置入口下标
    uint32_t backoff_ms = kInitialBackoffMs;      ///< 该槽的重连退避
    uint32_t retry_at_ms = 0;  ///< 独立重试截止时间，等待期间仍服务其它连接
    bool connecting = false;                      ///< 是否正在非阻塞连接中
    bool session_open = false;                    ///< 是否已建立过 TCP 会话
};
#endif

/**
 * @brief 组件运行上下文（RAII：持有队列/任务/连接/AEAD/虚拟网卡与密钥）。
 *
 * @details 构造只准备设置与密钥，@ref start 创建资源，@ref stop 等待任务退出。
 *          数据成员全部 private；多个实例各持一份，互不共享。
 */
class Context final {
public:
    /**
     * @brief 构造并准备上下文：拷贝设置与运行选项、派生密钥/摘要、生成随机 peer id。
     * @note 不创建任何运行资源（队列/任务在 @ref start 中创建）。设置文本的深拷贝在
     *       分配失败时按组件约定终止进程（-fno-exceptions），因此没有错误码。
     */
    Context(const Settings& new_settings, const RuntimeOptions& new_options) noexcept;

    /** @brief 析构：若仍在运行则无限等待停止，然后释放全部资源并清零密钥。 */
    ~Context() noexcept;

    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;
    Context(Context&&) = delete;
    Context& operator=(Context&&) = delete;

    /**
     * @brief 启动：创建队列/AEAD/TcpConn/虚拟网卡与三个任务。
     * @return @c ESP_OK；已启动时返回 @c ESP_ERR_INVALID_STATE，资源不足返回
     *         @c ESP_ERR_NO_MEM（失败时已回滚并置 @ref state 为 Error）。
     */
    esp_err_t start() noexcept;

    /**
     * @brief 停止（幂等）：置停止标志并等待任务退出。
     * @param timeout_ms 等待上限；@c UINT32_MAX 表示无限等待（析构路径使用）。
     * @return @c ESP_OK、@c ESP_ERR_TIMEOUT（资源保留，可重试）或 @c ESP_ERR_INVALID_STATE。
     */
    esp_err_t stop(uint32_t timeout_ms) noexcept;

    /** @brief 应用连接开关（非阻塞入队；未启动返回 @c ESP_ERR_INVALID_STATE）。 */
    esp_err_t set_connection(bool connected) noexcept;

private:
    friend class Inst;  ///< 公共快照（get_status/get_config_info）由 Inst 直接读取

#if defined(ESP_PLATFORM)
    // ---- 任务入口（平台回调必须是普通函数指针，因此保持 static 适配器）----------------
    static void io_entry(void* arg) noexcept;     ///< xTaskCreate 入口 → run_io()
    static void proto_entry(void* arg) noexcept;  ///< xTaskCreate 入口 → run_proto()
    static void data_entry(void* arg) noexcept;   ///< xTaskCreate 入口 → run_data()
    static void post_frame_cb(void* arg, Bytes body) noexcept;  ///< TcpConn 读回调适配器
    static esp_err_t handle_ospf_sync_cb(void* ctx_ptr, Bytes request, MutBytes response,
                                         size_t& response_len) noexcept;  ///< RPC 服务回调

    // ---- 任务主循环（非静态，直接使用 this）-------------------------------------------
    void run_io() noexcept;     ///< I/O 任务主循环（定义见 tasks/io_task.cpp）
    void run_proto() noexcept;  ///< 控制面任务主循环（定义见 tasks/proto_task.cpp）
    void run_data() noexcept;   ///< 数据面任务主循环（定义见 tasks/data_task.cpp）

    // I/O 任务
    void advance_peer_index(size_t& index) const noexcept;
    void post_event(IoEvent kind, uint8_t slot, uint32_t generation) noexcept;
    void post_frame(uint32_t generation, Bytes body) noexcept;
    bool select_entry(const std::array<SlotState, kMaxConnSlots>& states, size_t slot,
                      size_t& index) const noexcept;
    void drop_slot(size_t slot, SlotState& st) noexcept;
    void pump_tx(std::array<SlotState, kMaxConnSlots>& states) noexcept;

    // 控制面任务
    bool send_frame(uint32_t generation, Bytes body) noexcept;
    bool send_control_frame(uint32_t generation, PacketType type, uint32_t from_peer,
                            uint32_t to_peer, Bytes plain, bool encrypt) noexcept;
    bool send_rpc_message(uint32_t generation, const RpcSendTemplate& tmpl,
                          Bytes content) noexcept;
    void request_reconnect(uint32_t generation) noexcept;
    uint32_t peer_of_generation(uint32_t generation) const noexcept;
    void revoke_ready(esp_err_t err) noexcept;
    void handle_rpc_request(uint32_t generation, const RpcPacketRef& packet, Bytes body) noexcept;
    bool send_sync_request(uint32_t generation, uint32_t remote_peer_id, bool include_topology,
                           uint32_t now) noexcept;
    esp_err_t handle_ospf_sync(Bytes request, MutBytes response, size_t& response_len) noexcept;
    void sync_netif_address() noexcept;
    void handle_rpc_response(uint32_t generation, const RpcPacketRef& packet, Bytes body) noexcept;

    // 数据面任务
    void deliver_to_netif(Bytes ip_packet) noexcept;
    void send_data_frame(uint32_t generation, uint32_t to_peer, uint32_t next_hop,
                         Bytes ip_packet) noexcept;
    void handle_netif_output(const NetifRxItem& item) noexcept;
    void handle_inbound_data(FrameMessage& frame) noexcept;
#endif

    /** @brief 记录一次错误（保留首个错误，便于定位根因）。 */
    void set_error(esp_err_t err) noexcept {
        if (err != ESP_OK) {
            esp_err_t expected = ESP_OK;
            (void)last_error.compare_exchange_strong(expected, err);
        }
    }

    /** @brief 投递事件回调（仅控制面任务调用）。 */
    void emit(EventType type, uint32_t peer_id = 0) noexcept {
        if (options.on_event == nullptr) {
            return;
        }
        Event event{};
        event.type = type;
        event.peer_id = peer_id;
        // 事件快照与 Inst::get_status() 保持一致：虚拟地址取当前生效值，
        // 可达路由数取控制面最近发布的值。
        event.status.state = state.load();
        event.status.peer_id = my_peer_id.load();
        event.status.virtual_ipv4 = my_ipv4.load();
        event.status.prefix_len = my_prefix.load();
        event.status.direct_peer_count = direct_peers.load();
        event.status.reachable_route_count = reachable_routes.load();
        event.status.last_error = last_error.load();
        event.status.dropped_packets = dropped.load();
        options.on_event(event, options.user_data);
    }

    /** @brief 创建队列/AEAD/TcpConn/网卡与任务；失败时由调用方回滚。 */
    esp_err_t create_runtime() noexcept;
    /** @brief 释放队列/连接/AEAD/网卡（可重复调用；析构与 start 失败回滚共用）。 */
    void release_runtime() noexcept;
    /**
     * @brief 等待三个任务确认退出。
     * @param timeout_ms 等待上限；@c UINT32_MAX 表示无限等待（析构路径使用）。
     */
    bool wait_tasks(uint32_t timeout_ms) noexcept;

    // ---- 设置与密钥 -------------------------------------------------------
    Settings settings{};                ///< 校验后的生效设置（构造后不可变）
    RuntimeOptions options{};           ///< 事件回调等运行期选项
    std::array<uint8_t, 16> key{};      ///< 派生的 AES-128 密钥
    std::array<uint8_t, 32> digest{};   ///< 网络密钥摘要

    // ---- 共享状态（原子量，跨任务读取）------------------------------------
    std::atomic<State> state{State::Uninitialized};   ///< 当前状态
    std::atomic<bool> connection_enabled{false};      ///< 应用是否允许连接（connect/disconnect）
    std::atomic<bool> stop_requested{false};          ///< 停止请求
    std::atomic<uint32_t> generation{1};              ///< 当前连接代际
    std::atomic<uint32_t> my_peer_id{0};              ///< 本端 peer id
    std::atomic<size_t> direct_peers{0};              ///< 已握手的直连数量
    std::atomic<uint64_t> dropped{0};                 ///< 丢弃计数
    std::atomic<esp_err_t> last_error{ESP_OK};        ///< 最近错误
    std::atomic<uint32_t> my_ipv4{0};                 ///< 当前虚拟 IPv4（静态或 dhcp 分配）
    std::atomic<uint8_t> my_prefix{24};               ///< 当前虚拟前缀长度
    std::atomic<size_t> reachable_routes{0};          ///< 当前可达路由数（快照统计）

    // ---- 控制面（仅控制面任务访问）----------------------------------------
    RpcReassembler reassembler{};    ///< RPC 分片重组（默认 2 槽 × 约 16.6 KiB，故放在堆上）
    RpcRegistry registry{};          ///< RPC 服务注册表
    RpcTransactions transactions{};  ///< RPC 事务表
    /// 控制面 AEAD：与数据面实例相互独立（start() 时按派生密钥创建）。
    AesGcm128* control_cipher = nullptr;
    /// 数据面 AEAD（同样独立实例）。
    AesGcm128* data_cipher = nullptr;
    /// 数据面私有的路由快照副本。
    RouteSnapshot data_snapshot{};
    /// 数据面接收队列的临时项（避免占用任务栈）。
    FrameMessage data_rx_scratch{};
    /// 转发时改写 PM 头用的临时项。
    FrameMessage data_tx_scratch{};
    /// 单个 RPC 报文的明文缓冲（编码后的 RpcPacket）。
    std::array<uint8_t, kMaxRpcPacketEncodedLen> rpc_plain{};
    /// 单个 RPC 帧的组帧缓冲：PM 头 + 明文 + AEAD 尾。
    std::array<uint8_t, kPeerManagerHeaderSize + kMaxRpcPacketEncodedLen + kAeadTailSize>
        rpc_frame{};
    /// RPC 响应缓冲（服务返回值序列化输出）。
    std::array<uint8_t, 4096> rpc_service_out{};
    /// 编码后的 RpcResponse 缓冲：可能被分成多片发送，因此需要容纳完整 body。
    std::array<uint8_t, kRpcMaxBodyBytes> rpc_body{};
    /// RPC 错误信息编码缓冲。
    std::array<uint8_t, 256> rpc_error{};

    // ---- 路由（仅控制面任务访问）------------------------------------------
    /// 路由同步引擎：拓扑、最短路、自身信息与 dhcp 自分配都在其中（可宿主测试）。
    SyncEngine sync{};
    /// 同步请求与 RPC 包装缓冲（放在 Context 里，避免占用控制面任务栈）。
    std::array<uint8_t, SyncEngine::kMaxRequestBytes> sync_request_raw{};
    std::array<uint8_t, SyncEngine::kMaxRequestBytes + 64> sync_rpc_raw{};
    bool network_ready_reported = false;  ///< 本会话是否已上报 NetworkReady

    // ---- 虚拟网卡 ---------------------------------------------------------
    VirtualNetif virtual_netif{};  ///< 虚拟网卡
    bool netif_created = false;    ///< 网卡是否已创建
    uint32_t netif_ip = 0;         ///< 网卡当前地址（用于判断是否需要更新）

#if defined(ESP_PLATFORM)
    // ---- 队列与任务句柄 ---------------------------------------------------
    QueueHandle_t q_rx_ctrl = nullptr;  ///< I/O → 控制面：收到的帧
    QueueHandle_t q_tx = nullptr;       ///< 控制面 → I/O：待发送的帧
    QueueHandle_t q_evt = nullptr;      ///< I/O → 控制面：连接事件
    QueueHandle_t q_io_cmd = nullptr;   ///< 控制面 → I/O：重连命令
    QueueHandle_t q_app_cmd = nullptr;  ///< 应用 → 控制面：连接控制
    QueueHandle_t q_rx_data = nullptr;  ///< I/O → 数据面：Data 帧
    QueueHandle_t q_netif_rx = nullptr; ///< TCPIP 线程 → 数据面：出站 pbuf
    QueueHandle_t q_snapshot = nullptr; ///< 控制面 → 数据面：路由快照（长度 1，按值覆盖）
    SemaphoreHandle_t task_exit = nullptr;  ///< 任务退出确认

    /// 入口连接槽：每个槽一个 TcpConn（内含约 12 KiB 收发缓冲）放在堆上而不是
    /// 任务栈上；生命周期由 I/O 任务管理，控制面通过 peer_id/generation 共享状态。
    std::array<ConnSlot, kMaxConnSlots> slots{};
#endif

    bool started_ = false;  ///< 是否已 start()
};

}  // namespace et
