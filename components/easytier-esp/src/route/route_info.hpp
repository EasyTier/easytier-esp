/**
 * @file route_info.hpp
 * @brief 路由拓扑元素（RoutePeerInfo / 邻接表 / 邻接位图）的编解码。
 *
 * @details 与上游对齐的要点：
 * - `RoutePeerInfo.version == 0` 的节点**不参与建图**（上游 `build_graph` 会跳过）；
 * - 邻接是**有向**的：`src` 行里声明的 `connected_peers` 才构成 `src → dst` 边；
 * - 声明 `avoid_relay_data` 的节点，其**出边**会被加大代价（上游用 i32::MAX）；
 * - 邻接位图按 `bit = row * N + col`、字节内低位优先（LSB-first）。
 */
#ifndef EASYTIER_ESP_ROUTE_ROUTE_INFO_HPP_
#define EASYTIER_ESP_ROUTE_ROUTE_INFO_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "protocol/messages.hpp"
#include "protocol/protobuf.hpp"

namespace et {

/** @brief PeerFeatureFlag 的位掩码（位号 = 上游字段号 - 1）。 */
namespace peer_feature {

inline constexpr uint16_t AvoidRelayData = 1u << 1;         ///< field 2
inline constexpr uint16_t SupportConnListSync = 1u << 4;    ///< field 5
inline constexpr uint16_t DisableP2p = 1u << 9;             ///< field 10

}  // namespace peer_feature

/** @brief 本组件宣告的 easytier_version 字符串。 */
inline constexpr std::string_view kReportedEasyTierVersion = "0.1.0-esp";

/** @brief 每个节点保留的原始 RoutePeerInfo 字节上限（用于向他人转发未知字段）。 */
inline constexpr size_t kMaxRawRoutePeerInfoBytes = 512;
/** @brief 单个 ConnPeerInfo 里允许的邻接数量上限。 */
inline constexpr size_t kMaxConnectedPerRow = 32;
/** @brief 邻接/节点数量上限（与拓扑上限一致）。 */
inline constexpr size_t kMaxRoutePeers = 32;

/** @brief `PeerIdVersion`（邻接表/位图中重复出现的节点+版本对）。 */
struct PeerIdVersion {
    uint32_t peer_id = 0;  ///< tag 1
    uint32_t version = 0;  ///< tag 2
};

/**
 * @brief 解析后的 RoutePeerInfo。
 *
 * @note @ref raw 指向输入缓冲区；需要长期保留时由拓扑模块复制到自己的存储里。
 */
struct RoutePeerInfoView {
    uint32_t peer_id = 0;         ///< tag 1
    Uuid inst_id{};               ///< tag 2（四个 uint32）
    uint32_t cost = 0;            ///< tag 3
    bool has_ipv4 = false;        ///< tag 4 是否存在
    uint32_t ipv4 = 0;            ///< tag 4 的 addr（数值表示）
    uint32_t version = 0;         ///< tag 9
    uint32_t network_length = 0;  ///< tag 13
    uint16_t feature_flags = 0;   ///< tag 11（展开为位掩码，位号 = 字段号-1）
    uint64_t peer_route_id = 0;   ///< tag 12
    std::string_view hostname{};  ///< tag 6（可选）
    uint64_t last_update_secs = 0;  ///< tag 8 的 seconds
    Bytes raw{};                  ///< 原始编码字节
};

/**
 * @brief 遍历 `RoutePeerInfos{ repeated RoutePeerInfo items = 1 }` 中的每个元素。
 * @tparam Fn 形如 void(Bytes item) 的回调
 * @return false 表示负载损坏或元素数超限
 */
template <typename Fn>
[[nodiscard]] bool for_each_route_peer_info(Bytes payload, size_t max_items, Fn&& fn) noexcept {
    ProtoReader reader(payload);
    PbField field;
    size_t count = 0;
    while (reader.next(field)) {
        if (field.field != 1) {
            continue;  // 未知字段跳过
        }
        if (field.type != PbWireType::LengthDelimited || ++count > max_items) {
            return false;
        }
        fn(field.data);
    }
    return !reader.failed();
}

/** @brief 解码一个 RoutePeerInfo；字段类型非法返回 false。 */
[[nodiscard]] bool decode_route_peer_info(Bytes in, RoutePeerInfoView& out) noexcept;

/** @brief 本节点要宣告的 RoutePeerInfo 字段。 */
struct SelfRouteInfo {
    uint32_t peer_id = 0;                        ///< 本节点 peer id
    Uuid inst_id{};                              ///< 实例 UUID
    uint32_t version = 1;                        ///< 信息版本（变化时 +1）
    bool has_ipv4 = false;                       ///< 是否已有虚拟 IPv4
    uint32_t ipv4 = 0;                           ///< 虚拟 IPv4（数值表示）
    uint32_t network_length = 24;                ///< 虚拟网段前缀长度
    uint16_t feature_flags = 0;                  ///< 见 peer_feature
    uint64_t peer_route_id = 0;                  ///< 随机 u64
    std::string_view hostname{};                 ///< 主机名（可为空）
    uint32_t last_update_secs = 0;               ///< 当前 Unix 秒
};

/** @brief 编码本节点的 RoutePeerInfo。 */
[[nodiscard]] size_t encode_route_peer_info(const SelfRouteInfo& info, MutBytes out) noexcept;

/**
 * @brief 编码 `RoutePeerInfos{ repeated RoutePeerInfo items = 1 }` 外层消息。
 * @param items 各节点已编码的 RoutePeerInfo 原始字节
 */
[[nodiscard]] size_t encode_route_peer_infos(const Bytes* items, size_t count,
                                             MutBytes out) noexcept;

/** @brief 一行邻接（`RouteConnPeerList.PeerConnInfo`）。 */
struct ConnRow {
    uint32_t peer_id = 0;                                       ///< tag 1 的 peer_id
    uint32_t version = 0;                                       ///< tag 1 的 version
    std::array<uint32_t, kMaxConnectedPerRow> connected{};      ///< tag 2 repeated
    size_t connected_count = 0;                                 ///< 邻接数量
};

/** @brief 邻接表（`RouteConnPeerList`）。 */
struct ConnList {
    std::array<ConnRow, kMaxRoutePeers> rows{};  ///< 各行
    size_t count = 0;                            ///< 行数
};

/** @brief 编码邻接表。 */
[[nodiscard]] size_t encode_conn_list(const ConnList& list, MutBytes out) noexcept;

/** @brief 解码邻接表。 */
[[nodiscard]] bool decode_conn_list(Bytes in, ConnList& out) noexcept;

/** @brief 邻接位图（`RouteConnBitmap`）。 */
struct ConnBitmap {
    std::array<PeerIdVersion, kMaxRoutePeers> peer_ids{};  ///< tag 1：行列顺序
    size_t count = 0;                                      ///< 节点数
    std::array<uint8_t, (kMaxRoutePeers * kMaxRoutePeers) / 8> bitmap{};  ///< tag 2
    size_t bitmap_len = 0;                                 ///< 位图字节数

    /** @brief 第 row 个节点是否直连第 col 个节点（LSB-first）。 */
    [[nodiscard]] bool has_edge(size_t row, size_t col) const noexcept;
};

/** @brief 解码邻接位图（含长度校验）。 */
[[nodiscard]] bool decode_conn_bitmap(Bytes in, ConnBitmap& out) noexcept;

/**
 * @brief 把邻接位图展开成邻接表（便于与 list 形式统一合并）。
 * @param bitmap 已解码的位图
 * @param out 输出邻接表（行数等于位图节点数）
 */
[[nodiscard]] bool expand_conn_bitmap(const ConnBitmap& bitmap, ConnList& out) noexcept;

}  // namespace et

#endif
