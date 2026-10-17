/**
 * @file topology.hpp
 * @brief 固定容量的路由拓扑表（节点信息 + 邻接行）。
 *
 * @details 合并规则与上游一致：
 * - 只有 `version` **更大**时才覆盖已存在的条目，避免乱序/回退的旧信息破坏拓扑；
 * - `version == 0` 的节点只作为占位记录（用于请求缺片信息），**不参与建图**；
 * - 每个节点的原始 `RoutePeerInfo` 字节会被有界保留，便于在转发时保留未知字段；
 * - 容量不足时明确拒绝更新（RejectedFull），不会静默丢弃后仍报告成功；
 * - 节点超过 90 秒无更新则退出建图，超过 3600 秒彻底删除；邻接行同样按 90 秒失效。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "route/route_info.hpp"

namespace et {

/** @brief 无效 peer id（0 不是合法节点）。 */
inline constexpr uint32_t kInvalidPeerId = 0;
/** @brief 节点信息退出建图的时间（毫秒）。 */
inline constexpr uint32_t kTopoPeerStaleMs = 90 * 1000;
/** @brief 节点信息彻底删除的时间（毫秒）。 */
inline constexpr uint32_t kTopoPeerRemoveMs = 3600 * 1000;
/** @brief 邻接行的失效时间（毫秒）。 */
inline constexpr uint32_t kTopoConnStaleMs = 90 * 1000;

/** @brief 拓扑中的一个节点。 */
struct TopoPeer {
    uint32_t peer_id = kInvalidPeerId;  ///< 节点 id
    uint32_t version = 0;               ///< 信息版本（0 表示占位，不建图）
    bool has_ipv4 = false;              ///< 是否已知虚拟 IPv4
    uint32_t ipv4 = 0;                  ///< 虚拟 IPv4（数值表示）
    uint8_t network_length = 24;        ///< 通告的虚拟网段前缀长度
    uint16_t feature_flags = 0;         ///< 能力位（见 peer_feature）
    uint64_t peer_route_id = 0;         ///< 节点路由 id
    uint32_t last_update_ms = 0;        ///< 最近一次更新的本地时间
    bool in_graph = false;              ///< 是否参与建图（version != 0 且未过期）
    std::array<uint8_t, kMaxRawRoutePeerInfoBytes> raw{};  ///< 原始 RoutePeerInfo 字节
    uint16_t raw_len = 0;                                  ///< 原始字节长度
};

/** @brief 拓扑中的一行邻接（有向）。 */
struct TopoConnRow {
    uint32_t peer_id = kInvalidPeerId;  ///< 源节点
    uint32_t version = 0;               ///< 邻接信息版本
    /// 直连的节点 id 列表（保存 id 而不是槽位下标，避免节点删除后下标错位）。
    std::array<uint32_t, kMaxConnectedPerRow> connected{};
    size_t connected_count = 0;         ///< 直连数量
    uint32_t last_update_ms = 0;        ///< 最近一次更新的本地时间
    bool valid = false;                 ///< 是否有效
};

/**
 * @brief 路由拓扑表。
 * @note 非线程安全：只由控制面任务访问（路由计算也在同一任务内完成）。
 */
class Topology {
public:
    /** @brief 节点数量上限。 */
    static constexpr size_t kMaxPeers = kMaxRoutePeers;

    /** @brief 合并结果。 */
    enum class MergeResult : uint8_t {
        Updated,       ///< 已更新（版本更高或首次出现）
        Unchanged,     ///< 版本不高于本地，忽略
        RejectedFull,  ///< 容量不足（节点数或原始字节超限）
        Invalid,       ///< 输入非法（peer_id 为 0 等）
    };

    /** @brief 清空所有状态。 */
    void reset() noexcept;

    /**
     * @brief 清空某个节点已保存的信息（保留其 id 作为占位）。
     * @details 上游在"对端 session id 变化"时会清空该对端已保存的信息并要求重新同步
     *          （`session id mismatch, clear saved info`）。清空后该节点不参与建图
     *          （version == 0 即占位），等对端重新宣告后再进入。
     */
    void reset_peer(uint32_t peer_id) noexcept;

    /** @brief 登记本节点（必须参与建图，作为最短路起点）。 */
    void set_self(uint32_t peer_id, bool has_ipv4, uint32_t ipv4, uint32_t now_ms) noexcept;

    /** @brief 合并一个节点的 RoutePeerInfo。 */
    MergeResult merge_peer(const RoutePeerInfoView& info, uint32_t now_ms) noexcept;

    /**
     * @brief 合并一行邻接。
     * @param connected 直连的 peer id 列表（未知节点 id 原样保留，缺失补发由 merge_request 负责）
     */
    MergeResult merge_conn_row(uint32_t peer_id, uint32_t version, const uint32_t* connected,
                               size_t count, uint32_t now_ms) noexcept;

    /** @brief 按时间清理过期节点与邻接行。 */
    void expire(uint32_t now_ms) noexcept;

    /** @brief 拓扑是否发生了变化（需要重新计算路由）。 */
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }
    /** @brief 清除变化标记。 */
    void clear_dirty() noexcept { dirty_ = false; }

    /** @brief 按 peer id 查找节点。 */
    [[nodiscard]] const TopoPeer* find_peer(uint32_t peer_id) const noexcept;
    /** @brief 节点槽位（供路由计算遍历）。 */
    [[nodiscard]] std::span<const TopoPeer> peers() const noexcept {
        return {peers_.data(), peer_count_};
    }
    /** @brief 邻接行槽位。 */
    [[nodiscard]] std::span<const TopoConnRow> conn_rows() const noexcept {
        return {rows_.data(), row_count_};
    }


    /** @brief 拓扑槽位索引（不存在返回 kMaxPeers）。 */
    [[nodiscard]] size_t index_of(uint32_t peer_id) const noexcept;

private:
    /** @brief 找到或分配一个槽位。 */
    [[nodiscard]] TopoPeer* find_or_allocate(uint32_t peer_id) noexcept;

    std::array<TopoPeer, kMaxPeers> peers_{};
    std::array<TopoConnRow, kMaxPeers> rows_{};
    size_t peer_count_ = 0;
    size_t row_count_ = 0;
    bool dirty_ = false;
};

}  // namespace et
