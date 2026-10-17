/**
 * @file config_limits.hpp
 * @brief 编译期资源上限。
 *
 * @details 默认值与组件约定一致；ESP-IDF 构建时通过 Kconfig 覆盖。宿主构建
 * 没有 sdkconfig，因此这里提供同样的默认值，保证两端行为一致。
 */
#pragma once

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#include <cstddef>
#include <cstdint>

namespace et {

/** @brief 同时连接的入口节点数上限（超出部分的入口作为轮换备用）。 */
#if defined(CONFIG_EASYTIER_ESP_MAX_CONNECTED_PEERS)
inline constexpr size_t kMaxConnectedPeers = CONFIG_EASYTIER_ESP_MAX_CONNECTED_PEERS;
#else
inline constexpr size_t kMaxConnectedPeers = 6;
#endif

/** @brief 并发 RPC 事务上限。 */
#if defined(CONFIG_EASYTIER_ESP_RPC_MAX_TRANSACTIONS)
inline constexpr size_t kRpcMaxTransactions = CONFIG_EASYTIER_ESP_RPC_MAX_TRANSACTIONS;
#else
inline constexpr size_t kRpcMaxTransactions = 4;
#endif

/** @brief 并发 RPC 分片重组槽数量。 */
#if defined(CONFIG_EASYTIER_ESP_RPC_REASSEMBLY_SLOTS)
inline constexpr size_t kRpcReassemblySlots = CONFIG_EASYTIER_ESP_RPC_REASSEMBLY_SLOTS;
#else
inline constexpr size_t kRpcReassemblySlots = 2;
#endif

// ---------------------------------------------------------------------------
// 队列深度（Kconfig menu "队列深度"；每帧约 2 KiB，扩大即增加常驻堆内存）
// ---------------------------------------------------------------------------

/** @brief I/O → 控制面的帧队列深度（`q_rx_ctrl`）。 */
#if defined(CONFIG_EASYTIER_ESP_QUEUE_RX_CTRL_DEPTH)
inline constexpr size_t kQueueRxCtrlDepth = CONFIG_EASYTIER_ESP_QUEUE_RX_CTRL_DEPTH;
#else
inline constexpr size_t kQueueRxCtrlDepth = 8;
#endif

/** @brief 控制面/数据面 → I/O 的发送队列深度（`q_tx`）。 */
#if defined(CONFIG_EASYTIER_ESP_QUEUE_TX_DEPTH)
inline constexpr size_t kQueueTxDepth = CONFIG_EASYTIER_ESP_QUEUE_TX_DEPTH;
#else
inline constexpr size_t kQueueTxDepth = 8;
#endif

/** @brief I/O → 数据面的 Data 帧队列深度（`q_rx_data`）。 */
#if defined(CONFIG_EASYTIER_ESP_QUEUE_RX_DATA_DEPTH)
inline constexpr size_t kQueueRxDataDepth = CONFIG_EASYTIER_ESP_QUEUE_RX_DATA_DEPTH;
#else
inline constexpr size_t kQueueRxDataDepth = 8;
#endif

/** @brief 事件/命令小消息队列深度（`q_evt` / `q_io_cmd` / `q_app_cmd`）。 */
#if defined(CONFIG_EASYTIER_ESP_QUEUE_SMALL_DEPTH)
inline constexpr size_t kQueueSmallDepth = CONFIG_EASYTIER_ESP_QUEUE_SMALL_DEPTH;
#else
inline constexpr size_t kQueueSmallDepth = 8;
#endif

/** @brief TCPIP 线程 → 数据面的 pbuf 队列深度（`q_netif_rx`）。 */
#if defined(CONFIG_EASYTIER_ESP_QUEUE_NETIF_DEPTH)
inline constexpr size_t kQueueNetifDepth = CONFIG_EASYTIER_ESP_QUEUE_NETIF_DEPTH;
#else
inline constexpr size_t kQueueNetifDepth = 16;
#endif

/** @brief 路由快照队列深度：固定 1（"只保留最新快照"的覆盖语义，不提供 Kconfig）。 */
inline constexpr size_t kQueueSnapshotDepth = 1;

// ---------------------------------------------------------------------------
// 任务栈与优先级（Kconfig menu "任务栈与优先级"）
//
// 栈大小按字节计（IDF 的 xTaskCreate 栈深度单位就是字节）。宿主构建没有 sdkconfig，
// 使用与 Kconfig 相同的默认值。
// ---------------------------------------------------------------------------

/** @brief I/O 任务栈（字节）。 */
#if defined(CONFIG_EASYTIER_ESP_IO_TASK_STACK)
inline constexpr uint32_t kIoTaskStack = CONFIG_EASYTIER_ESP_IO_TASK_STACK;
#else
inline constexpr uint32_t kIoTaskStack = 6144;
#endif

/** @brief 控制面任务栈（字节）。 */
#if defined(CONFIG_EASYTIER_ESP_PROTO_TASK_STACK)
inline constexpr uint32_t kProtoTaskStack = CONFIG_EASYTIER_ESP_PROTO_TASK_STACK;
#else
inline constexpr uint32_t kProtoTaskStack = 10240;
#endif

/** @brief 数据面任务栈（字节）。 */
#if defined(CONFIG_EASYTIER_ESP_DATA_TASK_STACK)
inline constexpr uint32_t kDataTaskStack = CONFIG_EASYTIER_ESP_DATA_TASK_STACK;
#else
inline constexpr uint32_t kDataTaskStack = 8192;
#endif

/** @brief I/O 任务优先级（FreeRTOS 优先级数，必须低于 WiFi/TCPIP）。 */
#if defined(CONFIG_EASYTIER_ESP_IO_TASK_PRIORITY)
inline constexpr uint32_t kIoTaskPriority = CONFIG_EASYTIER_ESP_IO_TASK_PRIORITY;
#else
inline constexpr uint32_t kIoTaskPriority = 7;
#endif

/** @brief 控制面任务优先级。 */
#if defined(CONFIG_EASYTIER_ESP_PROTO_TASK_PRIORITY)
inline constexpr uint32_t kProtoTaskPriority = CONFIG_EASYTIER_ESP_PROTO_TASK_PRIORITY;
#else
inline constexpr uint32_t kProtoTaskPriority = 5;
#endif

/** @brief 数据面任务优先级。 */
#if defined(CONFIG_EASYTIER_ESP_DATA_TASK_PRIORITY)
inline constexpr uint32_t kDataTaskPriority = CONFIG_EASYTIER_ESP_DATA_TASK_PRIORITY;
#else
inline constexpr uint32_t kDataTaskPriority = 7;
#endif

/** @brief TOML 语法树节点数上限。 */
inline constexpr size_t kTomlMaxNodes = 1024;
/** @brief 单个 TOML 数组的元素数上限。 */
inline constexpr size_t kTomlMaxArrayItems = 256;
/** @brief 单张 TOML 表的键数量上限。 */
inline constexpr size_t kTomlMaxTableKeys = 256;

// ---------------------------------------------------------------------------
// 上限守卫
//
// Kconfig 的 range 只约束 menuconfig 路径；直接编辑 sdkconfig 可以绕过它。这里把
// 代码真正能处理的区间再钉一遍：越界会让固件**编译失败**，而不是在运行期以越界/溢出
// 的形式暴露。
// ---------------------------------------------------------------------------
static_assert(kRpcMaxTransactions >= 1 && kRpcMaxTransactions <= 16,
              "RPC 事务上限必须落在 [1,16]");
static_assert(kRpcReassemblySlots >= 1 && kRpcReassemblySlots <= 8,
              "RPC 重组槽必须落在 [1,8]（每槽约 16 KiB）");
static_assert(kMaxConnectedPeers >= 1 && kMaxConnectedPeers <= 8,
              "同时连接的入口数必须落在 [1,8]（每槽约 12 KiB 收发缓冲）");
static_assert(kQueueRxCtrlDepth >= 1 && kQueueRxCtrlDepth <= 32,
              "q_rx_ctrl 深度必须落在 [1,32]（每元素约 2 KiB）");
static_assert(kQueueTxDepth >= 1 && kQueueTxDepth <= 32,
              "q_tx 深度必须落在 [1,32]（每元素约 2 KiB）");
static_assert(kQueueRxDataDepth >= 1 && kQueueRxDataDepth <= 32,
              "q_rx_data 深度必须落在 [1,32]（每元素约 2 KiB）");
static_assert(kQueueSmallDepth >= 1 && kQueueSmallDepth <= 32,
              "事件/命令队列深度必须落在 [1,32]");
static_assert(kQueueNetifDepth >= 1 && kQueueNetifDepth <= 64,
              "q_netif_rx 深度必须落在 [1,64]");

// 任务栈：太小会直接踩爆（编译期先挡一道），太大则白白吃内部 RAM。
static_assert(kIoTaskStack >= 2048 && kIoTaskStack <= 32768,
              "I/O 任务栈必须落在 [2048,32768] 字节");
static_assert(kProtoTaskStack >= 4096 && kProtoTaskStack <= 65536,
              "控制面任务栈必须落在 [4096,65536] 字节");
static_assert(kDataTaskStack >= 2048 && kDataTaskStack <= 32768,
              "数据面任务栈必须落在 [2048,32768] 字节");
// 优先级上限 24 对应 FreeRTOS configMAX_PRIORITIES=25（0 保留给 IDLE）。
static_assert(kIoTaskPriority >= 1 && kIoTaskPriority <= 24,
              "I/O 任务优先级必须落在 [1,24]");
static_assert(kProtoTaskPriority >= 1 && kProtoTaskPriority <= 24,
              "控制面任务优先级必须落在 [1,24]");
static_assert(kDataTaskPriority >= 1 && kDataTaskPriority <= 24,
              "数据面任务优先级必须落在 [1,24]");

}  // namespace et
