/**
 * @file context.hpp
 * @brief 组件内部运行上下文（每个 et::Inst 对象经 Inst::Impl 独占一份）。
 *
 * @details 设计要点：
 * - **单一所有者**：socket 只属于 I/O 任务，加解密上下文分属控制面与数据面，
 *   跨任务只通过队列传递数据、通过原子量传递状态；
 * - **有界资源**：队列和缓冲都是编译期固定容量，运行期不做无界分配；
 * - **代际（generation）**：每次新建连接都会递增，旧代际的收发数据一律丢弃，
 *   从而避免 fd 复用或旧数据串到新连接上。
 */
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

#include "easytier-esp.h"
#include "config/config.hpp"
#include "config/settings.hpp"
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
/// FrameMessage.slot 的特殊值：不指定槽位（由 I/O 任务按 to_peer 选择）。
inline constexpr uint8_t kAnySlot = 0xFF;

/// 任务数量
inline constexpr size_t kTaskCount = 3;

/** @brief 应用 → 控制面的命令。 */
enum class AppCommand : uint8_t {
    UnderlayUp,    ///< 物理网络就绪
    UnderlayDown,  ///< 物理网络断开
};

/** @brief 控制面 → I/O 的命令。 */
enum class IoCommand : uint8_t {
    Reconnect,  ///< 立即断开并按退避重连（握手失败/超时时使用）
};

/** @brief I/O → 控制面的连接事件。 */
enum class IoEvent : uint8_t {
    Connected,     ///< TCP 已建立
    Disconnected,  ///< TCP 已断开（对端关闭、错误或物理网络断开）
};

#if defined(ESP_PLATFORM)
/** @brief 队列中传递的一帧（固定容量，随队列静态分配）。 */
struct FrameMessage {
    uint32_t generation = 0;  ///< 连接代际（该槽建立连接时分配）
    uint32_t to_peer = 0;     ///< 目标对端 peer id（0 = 未指定，由 I/O 按槽位选择）
    uint8_t slot = kAnySlot;  ///< 指定槽位（控制面自己发的帧）；kAnySlot 表示按 to_peer 选
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
#endif

/**
 * @brief 组件运行上下文。
 *
 * @note 由某个 et::Inst 对象经 @c Inst::Impl 持有；生命周期覆盖该实例的所有任务，
 *       任务退出确认后才释放。多个实例各自持有一份，互不共享。
 */
struct Context {
    Settings settings{};                ///< 校验后的生效设置（构造后不可变）
    RuntimeOptions options{};           ///< 事件回调等运行期选项
    std::array<uint8_t, 16> key{};      ///< 派生的 AES-128 密钥
    std::array<uint8_t, 32> digest{};   ///< 网络密钥摘要

    std::atomic<State> state{State::Uninitialized};   ///< 当前状态
    std::atomic<bool> underlay_ready{false};          ///< 物理网络是否就绪
    std::atomic<bool> stop_requested{false};          ///< 停止请求
    std::atomic<uint32_t> generation{1};              ///< 当前连接代际
    std::atomic<uint32_t> my_peer_id{0};              ///< 本端 peer id
    std::atomic<uint32_t> remote_peer_id{0};          ///< 对端 peer id（握手后）
    std::atomic<size_t> direct_peers{0};              ///< 已握手的直连数量
    std::atomic<uint64_t> dropped{0};                 ///< 丢弃计数
    std::atomic<esp_err_t> last_error{ESP_OK};        ///< 最近错误
    std::atomic<uint32_t> my_ipv4{0};                 ///< 当前虚拟 IPv4（静态或 dhcp 分配）
    std::atomic<uint8_t> my_prefix{24};               ///< 当前虚拟前缀长度
    std::atomic<size_t> reachable_routes{0};          ///< 当前可达路由数（快照统计）

    // ---- 控制面（仅控制面任务访问）----------------------------------------
    RpcReassembler reassembler{};    ///< RPC 分片重组（约 17 KiB，故放在堆上的 Context 里）
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
    QueueHandle_t q_rx_ctrl = nullptr;  ///< I/O → 控制面：收到的帧
    QueueHandle_t q_tx = nullptr;       ///< 控制面 → I/O：待发送的帧
    QueueHandle_t q_evt = nullptr;      ///< I/O → 控制面：连接事件
    QueueHandle_t q_io_cmd = nullptr;   ///< 控制面 → I/O：重连命令
    QueueHandle_t q_app_cmd = nullptr;  ///< 应用 → 控制面：物理网络状态
    QueueHandle_t q_rx_data = nullptr;  ///< I/O → 数据面：Data 帧
    QueueHandle_t q_netif_rx = nullptr; ///< TCPIP 线程 → 数据面：出站 pbuf
    QueueHandle_t q_snapshot = nullptr; ///< 控制面 → 数据面：路由快照（长度 1，按值覆盖）
    SemaphoreHandle_t task_exit = nullptr;  ///< 任务退出确认

    TaskHandle_t io_task_handle = nullptr;     ///< I/O 任务句柄
    TaskHandle_t proto_task_handle = nullptr;  ///< 控制面任务句柄
    TaskHandle_t data_task_handle = nullptr;   ///< 数据面任务句柄
    /// 入口连接槽：每个槽一个 TcpConn（内含约 12 KiB 收发缓冲）放在堆上而不是
    /// 任务栈上；生命周期由 I/O 任务管理，控制面通过 peer_id/generation 共享状态。
    std::array<ConnSlot, kMaxConnSlots> slots{};
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
        // 事件里的状态必须与 get_status() 一致：虚拟地址取"当前实际生效"的地址
        // （静态或 dhcp 分配结果），可达路由数取控制面发布的最新值。早期版本这里
        // 回显配置里的静态地址并把路由数写死为 0，导致 dhcp 节点的 NetworkReady
        // 事件里虚拟地址为 0，接入方据此判断会出错。
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
};

#if defined(ESP_PLATFORM)
/** @brief I/O 任务入口（C++ 实现见 io_task.cpp）。 */
void io_task(void* arg) noexcept;
/** @brief 控制面任务入口（C++ 实现见 proto_task.cpp）。 */
void proto_task(void* arg) noexcept;
/** @brief 数据面任务入口（C++ 实现见 data_task.cpp）。 */
void data_task(void* arg) noexcept;

/** @brief FreeRTOS 任务入口包装（xTaskCreate 需要 C 函数指针）。 */
extern "C" void et_io_task_entry(void* arg);
/** @brief FreeRTOS 任务入口包装（xTaskCreate 需要 C 函数指针）。 */
extern "C" void et_proto_task_entry(void* arg);
/** @brief FreeRTOS 任务入口包装（xTaskCreate 需要 C 函数指针）。 */
extern "C" void et_data_task_entry(void* arg);
#endif

}  // namespace et
