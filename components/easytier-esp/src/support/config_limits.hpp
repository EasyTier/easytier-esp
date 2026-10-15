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

/** @brief TOML 文本/文件最大字节数。 */
#if defined(CONFIG_EASYTIER_ESP_TOML_MAX_BYTES)
inline constexpr size_t kTomlMaxBytes = CONFIG_EASYTIER_ESP_TOML_MAX_BYTES;
#else
inline constexpr size_t kTomlMaxBytes = 16 * 1024;
#endif

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
static_assert(kTomlMaxBytes >= 1024, "TOML 上限过小：至少 1 KiB");
static_assert(kTomlMaxBytes <= 65536, "TOML 上限过大：与静态缓冲/栈预算不符");
static_assert(kTomlHeapBudget >= 16 * 1024, "TOML 堆预算过小");
static_assert(kTomlHeapBudget <= 1024 * 1024, "TOML 堆预算过大");
static_assert(kRpcMaxTransactions >= 1 && kRpcMaxTransactions <= 16,
              "RPC 事务上限必须落在 [1,16]");
static_assert(kRpcReassemblySlots >= 1 && kRpcReassemblySlots <= 8,
              "RPC 重组槽必须落在 [1,8]（每槽约 16 KiB）");
static_assert(kMaxConnectedPeers >= 1 && kMaxConnectedPeers <= 8,
              "同时连接的入口数必须落在 [1,8]（每槽约 12 KiB 收发缓冲）");

}  // namespace et
