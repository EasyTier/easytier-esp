/**
 * @file plane.cpp
 * @brief 数据面决策实现。
 */
#include "data/plane.hpp"

#include "support/bytes.hpp"

namespace et {
namespace {

/** @brief IPv4 广播地址。 */
constexpr uint32_t kLimitedBroadcast = 0xFFFFFFFFu;
/** @brief 224.0.0.0/4 组播前缀。 */
constexpr uint32_t kMulticastBase = 0xE0000000u;
constexpr uint32_t kMulticastMask = 0xF0000000u;

}  // namespace

/** @brief 入站决策：本机投递 / 按下一跳转发 / 丢弃。 */
DataPlan plan_inbound(const RouteSnapshot& snapshot, const PeerManagerHeader& hdr,
                      uint8_t counter_limit) noexcept {
    DataPlan plan{};
    if (hdr.packet_type != static_cast<uint8_t>(PacketType::Data)) {
        return plan;  // 非数据报文
    }
    if (hdr.to_peer_id == snapshot.self_peer_id) {
        plan.action = DataAction::DeliverLocal;
        return plan;
    }
    // 转发：保持最终目的与密文不变，只递增计数并选择下一跳。
    if (hdr.forward_counter > counter_limit) {
        return plan;  // 防环兜底
    }
    const uint32_t next_hop = snapshot.lookup_next_hop(hdr.to_peer_id);
    if (next_hop == kInvalidPeerId) {
        return plan;  // 无路由
    }
    plan.action = DataAction::Forward;
    plan.next_hop = next_hop;
    return plan;
}

/** @brief 转发计数更新（超限返回 false）。 */
bool prepare_forward(PeerManagerHeader& hdr, uint8_t counter_limit) noexcept {
    if (hdr.forward_counter > counter_limit) {
        return false;
    }
    hdr.forward_counter = static_cast<uint8_t>(hdr.forward_counter + 1);
    return true;
}

/** @brief 取 IPv4 目的地址（不做完整校验，仅解析）。 */
uint32_t ipv4_destination(std::span<const uint8_t> ip_packet) noexcept {
    if (ip_packet.size() < 20 || ip_packet[0] != 0x45) {
        return 0;
    }
    return get_u32_be(ip_packet.data() + 16);
}

/** @brief 校验 IPv4 报文结构。 */
bool ipv4_is_valid(std::span<const uint8_t> ip_packet, uint16_t mtu) noexcept {
    if (ip_packet.size() < 20) {
        return false;
    }
    if ((ip_packet[0] >> 4) != 4) {
        return false;  // 只支持 IPv4
    }
    const size_t ihl = static_cast<size_t>(ip_packet[0] & 0x0F) * 4;
    if (ihl < 20 || ihl > ip_packet.size()) {
        return false;
    }
    const uint16_t total_length = get_u16_be(ip_packet.data() + 2);
    if (total_length < ihl || total_length > ip_packet.size()) {
        return false;  // 长度字段与实际不符
    }
    if (total_length > mtu) {
        return false;  // 超过虚拟 MTU
    }
    // 分片报文（本组件不重组 IP 分片，交由上层避免）。
    const uint16_t flags_offset = get_u16_be(ip_packet.data() + 6);
    if ((flags_offset & 0x1FFF) != 0) {
        return false;
    }
    return true;
}

/** @brief 判断广播/组播（本组件出站不支持）。 */
bool ipv4_is_broadcast_or_multicast(uint32_t dst) noexcept {
    if (dst == kLimitedBroadcast) {
        return true;
    }
    return (dst & kMulticastMask) == kMulticastBase;
}

}  // namespace et
