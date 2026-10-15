/**
 * @file table.cpp
 * @brief Dijkstra（O(N^2)，N ≤ 32）与路由快照构建。
 */
#include "route/table.hpp"

#include "support/log.hpp"

namespace et {

/** @brief 精确查找 IP。 */
uint32_t RouteSnapshot::lookup_ip(uint32_t ip) const noexcept {
    for (size_t i = 0; i < ip_count; ++i) {
        if (ip_map[i].ip == ip) {
            return ip_map[i].peer;
        }
    }
    return kInvalidPeerId;
}

/** @brief 查找下一跳。 */
uint32_t RouteSnapshot::lookup_next_hop(uint32_t dst) const noexcept {
    for (size_t i = 0; i < nh_count; ++i) {
        if (nh[i].dst == dst) {
            return nh[i].nh;
        }
    }
    return kInvalidPeerId;
}

/** @brief 可达条目数（不含自身）。 */
size_t RouteSnapshot::reachable_count() const noexcept {
    size_t count = 0;
    for (size_t i = 0; i < nh_count; ++i) {
        if (nh[i].dst != self_peer_id) {
            ++count;
        }
    }
    return count;
}

/**
 * @brief 重新计算路由。
 *
 * @details 步骤：
 * 1. 把参与建图的节点映射到 0..n-1 的紧凑下标；
 * 2. 以本节点为源做朴素 Dijkstra：边为"有向邻接"，边权为 1（avoid_relay 出边加
 *    kAvoidRelayPenalty 惩罚跳数）；
 * 3. 记录每个目的节点的第一跳作为下一跳，并构建 IP 映射（含自身 IP）。
 *
 * 快照在计算开始时先被清空，因此任何"不可达"都会立即从下一跳与 IP 映射中消失，
 * 不需要等待 90 秒的元数据清理（与设计一致）。
 */
void RouteTable::recompute(const Topology& topo, uint32_t self_peer_id) noexcept {
    RouteSnapshot next{};
    next.self_peer_id = self_peer_id;

    const std::span<const TopoPeer> peers = topo.peers();

    // 1) 紧凑下标（只包含参与建图的节点）。
    std::array<uint32_t, kMaxRoutePeers> node_peer{};
    size_t node_count = 0;
    size_t self_node = kMaxRoutePeers;
    for (const TopoPeer& peer : peers) {
        if (!peer.in_graph) {
            continue;
        }
        if (node_count >= kMaxRoutePeers) {
            break;
        }
        node_peer[node_count] = peer.peer_id;
        if (peer.peer_id == self_peer_id) {
            self_node = node_count;
        }
        ++node_count;
    }
    if (self_node == kMaxRoutePeers) {
        // 自身尚未进入拓扑：没有任何可达信息。
        snapshot_ = next;
        return;
    }
    // peer_id → 紧凑下标（线性查找，N ≤ 32）。
    const auto index_of_peer = [&node_peer, node_count](uint32_t peer_id) -> size_t {
        for (size_t i = 0; i < node_count; ++i) {
            if (node_peer[i] == peer_id) {
                return i;
            }
        }
        return kMaxRoutePeers;
    };

    // 2) Dijkstra（朴素实现）。
    std::array<uint16_t, kMaxRoutePeers> dist{};
    std::array<uint32_t, kMaxRoutePeers> first_hop{};
    std::array<bool, kMaxRoutePeers> done{};
    dist.fill(kInfiniteHops);
    first_hop.fill(kInvalidPeerId);
    dist[self_node] = 0;
    first_hop[self_node] = self_peer_id;

    for (size_t iteration = 0; iteration < node_count; ++iteration) {
        // 选未处理节点中距离最小者。
        size_t u = kMaxRoutePeers;
        for (size_t i = 0; i < node_count; ++i) {
            if (!done[i] && dist[i] != kInfiniteHops &&
                (u == kMaxRoutePeers || dist[i] < dist[u])) {
                u = i;
            }
        }
        if (u == kMaxRoutePeers || dist[u] == kInfiniteHops) {
            break;
        }
        done[u] = true;

        // 取节点 u 的能力位（avoid_relay 会对它的出边加大代价）。
        uint16_t feature_flags = 0;
        for (const TopoPeer& peer : peers) {
            if (peer.in_graph && peer.peer_id == node_peer[u]) {
                feature_flags = peer.feature_flags;
                break;
            }
        }
        const uint16_t penalty =
            ((feature_flags & peer_feature::AvoidRelayData) != 0) ? kAvoidRelayPenalty : 0;

        // 松弛 u 的所有有向出边。
        for (const TopoConnRow& row : topo.conn_rows()) {
            if (row.peer_id != node_peer[u] || !row.valid) {
                continue;
            }
            for (size_t c = 0; c < row.connected_count; ++c) {
                const size_t v = index_of_peer(row.connected[c]);
                if (v == kMaxRoutePeers || done[v]) {
                    continue;
                }
                const uint32_t candidate =
                    static_cast<uint32_t>(dist[u]) + 1u + static_cast<uint32_t>(penalty);
                const uint16_t candidate_hops = candidate >= kInfiniteHops
                                                    ? kInfiniteHops
                                                    : static_cast<uint16_t>(candidate);
                if (candidate_hops < dist[v]) {
                    dist[v] = candidate_hops;
                    first_hop[v] = (u == self_node) ? node_peer[v] : first_hop[u];
                }
            }
        }
    }

    // 3) 构建下一跳表与 IP 映射。
    //    自身 IP 也写入映射：数据面用它判断"目的 IP 是否就是本机"，从而决定投递到
    //    lwIP 还是继续转发。
    for (const TopoPeer& peer : peers) {
        if (peer.peer_id == self_peer_id && peer.has_ipv4) {
            next.self_ipv4 = peer.ipv4;
            break;
        }
    }
    for (size_t i = 0; i < node_count; ++i) {
        if (dist[i] == kInfiniteHops) {
            continue;  // 不可达：不进入快照
        }
        const bool is_self = (node_peer[i] == self_peer_id);
        if (!is_self && next.nh_count < next.nh.size()) {
            next.nh[next.nh_count].dst = node_peer[i];
            next.nh[next.nh_count].nh = first_hop[i];
            next.nh[next.nh_count].hops = dist[i];
            ++next.nh_count;
        }
        for (const TopoPeer& peer : peers) {
            if (peer.peer_id != node_peer[i] || !peer.has_ipv4) {
                continue;
            }
            if (next.ip_count < next.ip_map.size()) {
                next.ip_map[next.ip_count].ip = peer.ipv4;
                next.ip_map[next.ip_count].peer = peer.peer_id;
                ++next.ip_count;
            }
            break;
        }
    }

    snapshot_ = next;
    ET_LOGD("ET.ROUTE", "route table recomputed: %u reachable, %u ip entries",
            static_cast<unsigned>(snapshot_.nh_count), static_cast<unsigned>(snapshot_.ip_count));
}

}  // namespace et
