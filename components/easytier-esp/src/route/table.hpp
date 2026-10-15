/**
 * @file table.hpp
 * @brief 最短路计算与路由快照。
 *
 * @details
 * - 代价按上游语义采用"跳数优先"的字典序；本组件第一版只用跳数，并给声明
 *   `avoid_relay_data` 的节点的出边加大惩罚（上游用 i32::MAX，这里用一个足够大的
 *   跳数惩罚，效果是"除非无路可走否则不经过它"）；
 * - 只有 `version != 0` 且未过期的节点参与建图；
 * - 出边是**有向**的，与上游 `build_graph` 一致；
 * - 快照按值发布给数据面（长度 1 队列），因此这里只做纯计算，无副作用、无锁。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "route/topology.hpp"

namespace et {

/** @brief 不可达标记。 */
inline constexpr uint16_t kInfiniteHops = 0x7FFF;
/** @brief avoid_relay 节点出边的惩罚跳数。 */
inline constexpr uint16_t kAvoidRelayPenalty = 0x4000;

/**
 * @brief 路由快照：数据面只读的"目的 → 下一跳 / IP → 节点"映射。
 * @note 按值复制发布，读者持有自己的副本，不存在与写者竞争。
 */
struct RouteSnapshot {
    /** @brief 虚拟 IP → 节点（精确匹配）。 */
    struct IpEntry {
        uint32_t ip = 0;    ///< 虚拟 IPv4（数值表示）
        uint32_t peer = 0;  ///< 该 IP 所属节点
    };
    /** @brief 目的节点 → 下一跳。 */
    struct NhEntry {
        uint32_t dst = 0;  ///< 目的节点
        uint32_t nh = 0;   ///< 下一跳节点（直连）
        uint16_t hops = 0; ///< 跳数
    };

    std::array<IpEntry, kMaxRoutePeers> ip_map{};  ///< IP 映射表
    size_t ip_count = 0;                           ///< 条目数
    std::array<NhEntry, kMaxRoutePeers> nh{};      ///< 下一跳表
    size_t nh_count = 0;                           ///< 条目数
    uint32_t self_peer_id = 0;                     ///< 本节点
    uint32_t self_ipv4 = 0;                        ///< 本节点虚拟 IP（0 表示未知）

    /** @brief 精确查找某虚拟 IP 对应的节点；0 表示不可达。 */
    [[nodiscard]] uint32_t lookup_ip(uint32_t ip) const noexcept;
    /** @brief 查找某目的节点的下一跳；0 表示不可达。 */
    [[nodiscard]] uint32_t lookup_next_hop(uint32_t dst) const noexcept;
    /** @brief 可达条目数（不含自身）。 */
    [[nodiscard]] size_t reachable_count() const noexcept;
};

/**
 * @brief 路由表计算器。
 * @note 非线程安全：由控制面任务在拓扑变化时调用。
 */
class RouteTable {
public:
    /**
     * @brief 重新计算下一跳与 IP 映射。
     * @param topo 已合并的拓扑
     * @param self_peer_id 本节点 peer id（必须已存在于拓扑中）
     */
    void recompute(const Topology& topo, uint32_t self_peer_id) noexcept;

    /** @brief 最近一次计算的快照。 */
    [[nodiscard]] const RouteSnapshot& snapshot() const noexcept { return snapshot_; }

private:
    RouteSnapshot snapshot_{};
};

}  // namespace et
