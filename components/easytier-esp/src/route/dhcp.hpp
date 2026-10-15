/**
 * @file dhcp.hpp
 * @brief 虚拟 IPv4 自分配（对齐上游 `DhcpIpv4Allocator` 的语义）。
 *
 * @details 上游在 `dhcp = true` 时由节点自己算出一个不与别人冲突的虚拟地址，并把
 * 它写进自己的 `RoutePeerInfo` 通告出去；地址来源优先级：
 * 1. 优先沿用**当前子网**内其他节点通告过的子网；
 * 2. 否则选择通告子网中 `first_address` 最小、前缀最短的那个；
 * 3. 都没有时：若已持有地址则保持不变；否则等待 `BOOTSTRAP_WAIT_ROUNDS` 轮之后
 *    回落到默认子网（10.126.126.0/24）。
 * 候选地址在子网主机段内以随机起点线性探测，跳过已被占用的地址。首次分配
 * （或已有地址时）使用随机起点，从而避免多个节点同时自举时选出同一个地址。
 *
 * @note 本组件**不实现**为其他节点分配地址的服务端；这里只是本机自分配。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "route/topology.hpp"

namespace et {

/** @brief 一个已被占用的虚拟 IPv4（地址 + 前缀）。 */
struct Ipv4Use {
    uint32_t addr = 0;   ///< 数值表示的地址
    uint8_t prefix = 0;  ///< 前缀长度
};

/** @brief 有界的"已用地址"集合（通常来自拓扑中各节点通告的地址）。 */
struct Ipv4UseSet {
    std::array<Ipv4Use, kMaxRoutePeers> items{};  ///< 元素
    size_t count = 0;                             ///< 数量

    /** @brief 追加一个地址（去重，容量满则忽略）。 */
    void add(uint32_t addr, uint8_t prefix) noexcept;
    /** @brief 是否已包含完全相同的地址与前缀。 */
    [[nodiscard]] bool contains(uint32_t addr, uint8_t prefix) const noexcept;
    /** @brief 是否为空。 */
    [[nodiscard]] bool empty() const noexcept { return count == 0; }
};

/** @brief 自分配决策。 */
enum class DhcpDecision : uint8_t {
    WaitForPeers,  ///< 再等一轮（让已持地址的节点先通告子网）
    Unchanged,     ///< 保持当前地址
    Change,        ///< 使用新地址（可能首次分配，也可能是冲突后换址）
};

/** @brief 自分配结果。 */
struct DhcpResult {
    DhcpDecision decision = DhcpDecision::WaitForPeers;  ///< 决策
    bool has_address = false;                            ///< 结果是否携带地址
    uint32_t address = 0;                                ///< 分配的地址（数值表示）
    uint8_t prefix = 24;                                 ///< 前缀长度
};

/** @brief 自分配器（每个实例一个）。 */
class DhcpAllocator {
public:
    /** @brief 未通告地址时的等待轮数（与上游一致）。 */
    static constexpr uint32_t kBootstrappedWaitRounds = 2;

    /**
     * @brief 配置默认子网。
     * @param has_subnet 配置里是否显式给了 dhcp 子网
     * @param subnet 子网网络号（数值表示）
     * @param prefix 子网前缀长度
     */
    void configure(bool has_subnet, uint32_t subnet, uint8_t prefix) noexcept;

    /** @brief 复位（连接断开/重新组网时调用）。 */
    void reset() noexcept;

    /**
     * @brief 计算下一步决策。
     * @param has_routes 是否已经收到过任何拓扑（决定是否允许回落到默认子网）
     * @param used 已用地址集合
     * @param random_hint 随机数（用于打散自举时的地址选择；测试可传固定值）
     */
    [[nodiscard]] DhcpResult evaluate(bool has_routes, const Ipv4UseSet& used,
                                      uint32_t random_hint) noexcept;

    /** @brief 提交一次决策结果（Change 之后调用）。 */
    void commit(const DhcpResult& result) noexcept;

    /** @brief 当前地址是否已知。 */
    [[nodiscard]] bool has_current() const noexcept { return current_valid_; }
    /** @brief 当前地址。 */
    [[nodiscard]] uint32_t current() const noexcept { return current_addr_; }
    /** @brief 当前前缀。 */
    [[nodiscard]] uint8_t current_prefix() const noexcept { return current_prefix_; }

private:
    bool has_default_subnet_ = false;  ///< 是否配置了子网
    uint32_t default_subnet_ = 0;      ///< 配置的子网网络号
    uint8_t default_prefix_ = 24;      ///< 配置的子网前缀
    bool current_valid_ = false;       ///< 当前地址是否有效
    uint32_t current_addr_ = 0;        ///< 当前地址
    uint8_t current_prefix_ = 24;      ///< 当前前缀
    uint32_t no_ipv4_rounds_ = 0;      ///< 连续未看到任何通告子网的轮数
};

}  // namespace et
