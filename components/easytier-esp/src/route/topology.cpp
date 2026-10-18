/**
 * @file topology.cpp
 * @brief 路由拓扑表实现（版本合并、容量控制与过期清理）。
 */
#include "route/topology.hpp"

#include <cstring>

#include "support/log.hpp"
#include "support/time.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.TOPO";

/** @brief 版本是否更新（只有更大才覆盖）。 */
[[nodiscard]] bool version_is_newer(uint32_t incoming, uint32_t current, bool exists) noexcept {
    return !exists || incoming > current;
}

}  // namespace

/** @brief 清空拓扑。 */
void Topology::reset() noexcept {
    peers_ = {};
    rows_ = {};
    peer_count_ = 0;
    row_count_ = 0;
    dirty_ = false;
}

/** @brief 按 peer id 查找槽位索引。 */
size_t Topology::index_of(uint32_t peer_id) const noexcept {
    for (size_t i = 0; i < peer_count_; ++i) {
        if (peers_[i].peer_id == peer_id) {
            return i;
        }
    }
    return kMaxPeers;
}

/** @brief 按 peer id 查找节点。 */
const TopoPeer* Topology::find_peer(uint32_t peer_id) const noexcept {
    const size_t index = index_of(peer_id);
    return index == kMaxPeers ? nullptr : &peers_[index];
}

/** @brief 找到或分配槽位。 */
TopoPeer* Topology::find_or_allocate(uint32_t peer_id) noexcept {
    const size_t index = index_of(peer_id);
    if (index != kMaxPeers) {
        return &peers_[index];
    }
    if (peer_count_ >= kMaxPeers) {
        return nullptr;
    }
    TopoPeer& peer = peers_[peer_count_++];
    peer = TopoPeer{};
    peer.peer_id = peer_id;
    return &peer;
}

/**
 * @brief 清空某个节点已保存的信息（保留 id 作为占位，不参与建图）。
 * @details 上游在"对端 session id 变化"时会清空该对端已保存的信息并要求重新同步
 *          （`session id mismatch, clear saved info`）。清空后 version == 0 即占位，
 *          等对端重新宣告后再进入建图。
 */
void Topology::reset_peer(uint32_t peer_id) noexcept {
    for (TopoPeer& peer : peers_) {
        if (peer.peer_id != peer_id || peer.version == 0) {
            continue;
        }
        const uint32_t keep_id = peer.peer_id;
        peer = TopoPeer{};
        peer.peer_id = keep_id;
        peer.version = 0;  // 占位
        peer.raw_len = 0;
        dirty_ = true;
        return;
    }
}

void Topology::set_self(uint32_t peer_id, bool has_ipv4, uint32_t ipv4,
                        uint32_t now_ms) noexcept {
    TopoPeer* peer = find_or_allocate(peer_id);
    if (peer == nullptr) {
        return;
    }
    peer->has_ipv4 = has_ipv4;
    peer->ipv4 = ipv4;
    peer->in_graph = true;
    peer->last_update_ms = now_ms;
    if (peer->version == 0) {
        peer->version = 1;
    }
    dirty_ = true;
}

/** @brief 合并节点信息（版本更大才覆盖；原始字节有界保留）。 */
Topology::MergeResult Topology::merge_peer(const RoutePeerInfoView& info,
                                           uint32_t now_ms) noexcept {
    if (info.peer_id == kInvalidPeerId) {
        return MergeResult::Invalid;
    }
    if (info.raw.size() > kMaxRawRoutePeerInfoBytes) {
        // 原始字节超限：拒绝本次更新，避免为了转发而无限占用内存。
        ET_LOGW(kTag, "route peer info too large (%u bytes)",
                static_cast<unsigned>(info.raw.size()));
        return MergeResult::RejectedFull;
    }
    const size_t index = index_of(info.peer_id);
    const bool exists = index != kMaxPeers;
    if (exists && !version_is_newer(info.version, peers_[index].version, true)) {
        return MergeResult::Unchanged;
    }
    TopoPeer* peer = exists ? &peers_[index] : find_or_allocate(info.peer_id);
    if (peer == nullptr) {
        return MergeResult::RejectedFull;
    }
    peer->version = info.version;
    peer->has_ipv4 = info.has_ipv4;
    peer->ipv4 = info.ipv4;
    // 上游未填 network_length 时按 /24 处理，避免把 0 当成"整个地址空间已占用"。
    const uint32_t prefix = info.network_length == 0 ? 24 : info.network_length;
    peer->network_length = static_cast<uint8_t>(prefix > 32 ? 32 : prefix);
    peer->feature_flags = info.feature_flags;
    peer->peer_route_id = info.peer_route_id;
    peer->last_update_ms = now_ms;
    // version == 0 只作为占位，不参与建图（与上游一致）。
    peer->in_graph = info.version != 0;
    peer->raw_len = static_cast<uint16_t>(info.raw.size());
    if (peer->raw_len != 0) {
        std::memcpy(peer->raw.data(), info.raw.data(), peer->raw_len);
    }
    dirty_ = true;
    return MergeResult::Updated;
}

/** @brief 合并邻接行（有向；未知节点先忽略，由调用方请求补发）。 */
Topology::MergeResult Topology::merge_conn_row(uint32_t peer_id, uint32_t version,
                                               const uint32_t* connected, size_t count,
                                               uint32_t now_ms) noexcept {
    if (peer_id == kInvalidPeerId) {
        return MergeResult::Invalid;
    }
    if (count > kMaxConnectedPerRow) {
        return MergeResult::RejectedFull;
    }
    // 行按 peer_id 唯一。
    TopoConnRow* row = nullptr;
    for (size_t i = 0; i < row_count_; ++i) {
        if (rows_[i].peer_id == peer_id) {
            row = &rows_[i];
            break;
        }
    }
    if (row != nullptr && !version_is_newer(version, row->version, true)) {
        return MergeResult::Unchanged;
    }
    if (row == nullptr) {
        if (row_count_ >= kMaxPeers) {
            return MergeResult::RejectedFull;
        }
        row = &rows_[row_count_++];
        *row = TopoConnRow{};
        row->peer_id = peer_id;
    }
    row->version = version;
    row->last_update_ms = now_ms;
    row->valid = true;
    // 保存原始 id 列表：**不**过滤未知节点。原因是对端信息可能晚于邻接到达，
    // 若此时把未知 id 丢掉，等 peer_info 补齐后这一行就永远缺失该边了。
    // 建图时会自然跳过"没有 peer_info 的 id"（index_of_peer 返回无效下标）。
    row->connected_count = 0;
    for (size_t i = 0; i < count && row->connected_count < kMaxConnectedPerRow; ++i) {
        if (connected[i] == kInvalidPeerId || connected[i] == peer_id) {
            continue;  // 自身或非法 id 不构成边
        }
        row->connected[row->connected_count++] = connected[i];
    }
    dirty_ = true;
    return MergeResult::Updated;
}

/**
 * @brief 清理死亡节点与长时间不可达节点。
 * @details 与上游一致：90s 只清理"图中不可达"的节点，3660s 无条件删除死亡节点；
 *          可达节点不会因为最后更新时间旧而被踢出建图。自身与邻接行不做时间清理。
 */
void Topology::expire(uint32_t now_ms, uint32_t self_peer_id, PeerReachableFn reachable,
                      const void* reachable_ctx) noexcept {
    // 启动/reset 瞬态下自身可能还没进入拓扑：此时快照为空，不做不可达裁剪。
    const bool self_known = find_peer(self_peer_id) != nullptr;
    for (size_t i = 0; i < peer_count_;) {
        TopoPeer& peer = peers_[i];
        if (peer.peer_id == self_peer_id) {
            ++i;  // 自身永不过期
            continue;
        }
        // 时间戳只是"最后一次版本更新"，不是心跳：只有死亡删除和不可达清理两种。
        const bool dead = expired(now_ms, peer.last_update_ms, kTopoPeerRemoveMs);
        const bool unreachable =
            !dead && self_known && reachable != nullptr &&
            !reachable(reachable_ctx, peer.peer_id) &&
            expired(now_ms, peer.last_update_ms, kTopoUnreachableRemoveMs);
        if (!dead && !unreachable) {
            ++i;
            continue;
        }
        ET_LOGI(kTag, "removing %s peer %u", dead ? "dead" : "unreachable",
                static_cast<unsigned>(peer.peer_id));
        // 先删该节点的邻接行，再删节点（swap-with-last，避免遍历失效）。
        for (size_t r = 0; r < row_count_;) {
            if (rows_[r].peer_id != peer.peer_id) {
                ++r;
                continue;
            }
            rows_[r] = rows_[row_count_ - 1];
            rows_[--row_count_] = TopoConnRow{};
        }
        peers_[i] = peers_[peer_count_ - 1];
        peers_[--peer_count_] = TopoPeer{};
        dirty_ = true;
        // 槽位被替换，重新检查同一位置
    }
}

}  // namespace et
