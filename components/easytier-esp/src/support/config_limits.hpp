/**
 * @file config_limits.hpp
 * @brief 编译期资源上限。
 *
 * @details 默认值与设计文档一致；ESP-IDF 构建时通过 Kconfig 覆盖。宿主测试
 * 没有 sdkconfig，因此这里提供同样的默认值，保证两端行为一致。
 */
#pragma once

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#include <cstddef>
#include <cstdint>

namespace et {

/** @brief 单次 TOML 解析允许累计申请的堆字节上限（硬上限，不是估算值）。 */
#if defined(CONFIG_EASYTIER_ESP_TOML_HEAP_BUDGET)
inline constexpr size_t kTomlHeapBudget = CONFIG_EASYTIER_ESP_TOML_HEAP_BUDGET;
#else
inline constexpr size_t kTomlHeapBudget = 256 * 1024;
#endif

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
static_assert(kTomlHeapBudget >= 16 * 1024, "TOML 堆预算过小");
static_assert(kTomlHeapBudget <= 1024 * 1024, "TOML 堆预算过大");
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

}  // namespace et
