/**
 * @file plane.hpp
 * @brief 数据面决策与报文构造（可在宿主测试的纯逻辑部分）。
 *
 * @details 数据面规则（与上游 `peer_manager` 一致）：
 * - 出站：目的虚拟 IP → 目标 peer → 下一跳；源节点填 `forward_counter = 1`，并
 *   **由源节点加密**；
 * - 入站：`to_peer_id == 本机` → 解密后投递到 lwIP；否则**保持 `to_peer_id`、
 *   密文与 AEAD 尾不变**，只把 `forward_counter` 加一并按下一跳转发；
 * - `forward_counter > 7` 直接丢弃（防环）；广播/组播出站不支持，计数丢弃。
 */
#ifndef EASYTIER_ESP_DATA_PLANE_HPP_
#define EASYTIER_ESP_DATA_PLANE_HPP_

#include <cstddef>
#include <cstdint>
#include <span>

#include "protocol/wire.hpp"
#include "route/table.hpp"

namespace et {

/** @brief 入站数据帧的处理决策。 */
enum class DataAction : uint8_t {
    Drop,          ///< 丢弃（无路由、计数超限、非数据报文等）
    DeliverLocal,  ///< 目的为本机：解密后投递 lwIP
    Forward,       ///< 需要转发：按下一跳发出
};

/** @brief 入站数据帧的处理计划。 */
struct DataPlan {
    DataAction action = DataAction::Drop;  ///< 动作
    uint32_t next_hop = kInvalidPeerId;    ///< 转发时的下一跳
};

/**
 * @brief 决定一个入站 Data 帧如何处理。
 * @param snapshot 路由快照（控制面发布，数据面只读副本）
 * @param hdr 已解析的 PM 头
 * @param counter_limit 转发计数上限（kForwardCounterLimit）
 */
[[nodiscard]] DataPlan plan_inbound(const RouteSnapshot& snapshot,
                                    const PeerManagerHeader& hdr,
                                    uint8_t counter_limit) noexcept;

/**
 * @brief 转发前的计数更新。
 * @return false 表示计数超限，应丢弃
 */
[[nodiscard]] bool prepare_forward(PeerManagerHeader& hdr, uint8_t counter_limit) noexcept;

/** @brief 从 IPv4 报文中取出目的地址（数值表示）；非法报文返回 0。 */
[[nodiscard]] uint32_t ipv4_destination(std::span<const uint8_t> ip_packet) noexcept;

/** @brief 校验 IPv4 头（版本/IHL/总长/MTU），返回 false 表示必须丢弃。 */
[[nodiscard]] bool ipv4_is_valid(std::span<const uint8_t> ip_packet, uint16_t mtu) noexcept;

/** @brief 是否为出站广播/组播（本组件不支持，直接丢弃并计数）。 */
[[nodiscard]] bool ipv4_is_broadcast_or_multicast(uint32_t dst) noexcept;

}  // namespace et

#endif
