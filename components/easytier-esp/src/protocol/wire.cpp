/**
 * @file wire.cpp
 * @brief 传输分帧与 PeerManagerHeader 的编码/解码实现。
 *
 * @details 这里只把 packed 结构体用于**编译期布局校验**（static_assert），运行期
 * 一律通过显式的小端读写函数处理，避免未对齐访问与字节序问题。
 */
#include "protocol/wire.hpp"

#include <cstring>

namespace et {
namespace {

// 仅用于布局校验的镜像结构体。
#pragma pack(push, 1)
struct TcpTunnelHeaderLayout {
    uint32_t len;
};
struct UdpTunnelHeaderLayout {
    uint32_t conn_id;
    uint8_t msg_type;
    uint8_t padding;
    uint16_t len;
};
struct PeerManagerHeaderLayout {
    uint32_t from_peer_id;
    uint32_t to_peer_id;
    uint8_t packet_type;
    uint8_t flags;
    uint8_t forward_counter;
    uint8_t reserved;
    uint32_t len;
};
#pragma pack(pop)

static_assert(sizeof(TcpTunnelHeaderLayout) == kTcpTunnelHeaderSize, "TCP 帧头必须为 4 字节");
static_assert(sizeof(UdpTunnelHeaderLayout) == kUdpTunnelHeaderSize, "UDP 头必须为 8 字节");
static_assert(sizeof(PeerManagerHeaderLayout) == kPeerManagerHeaderSize, "PM 头必须为 16 字节");
static_assert(offsetof(PeerManagerHeaderLayout, from_peer_id) == 0, "PM 字段顺序变化");
static_assert(offsetof(PeerManagerHeaderLayout, to_peer_id) == 4, "PM 字段顺序变化");
static_assert(offsetof(PeerManagerHeaderLayout, packet_type) == 8, "PM 字段顺序变化");
static_assert(offsetof(PeerManagerHeaderLayout, flags) == 9, "PM 字段顺序变化");
static_assert(offsetof(PeerManagerHeaderLayout, forward_counter) == 10, "PM 字段顺序变化");
static_assert(offsetof(PeerManagerHeaderLayout, reserved) == 11, "PM 字段顺序变化");
static_assert(offsetof(PeerManagerHeaderLayout, len) == 12, "PM 字段顺序变化");
// 无压缩 AES profile 下，TCP body(2000) 能承载的最大明文 payload。
static_assert(kMaxPlainPayload == 1956, "明文 payload 上限应等于 2000-16-28");

}  // namespace

/** @brief 按小端写入 16 字节 PeerManagerHeader。 */
void encode_peer_manager_header(MutBytes out, const PeerManagerHeader& hdr) noexcept {
    if (out.size() < kPeerManagerHeaderSize) {
        return;
    }
    put_u32_le(out.data(), hdr.from_peer_id);
    put_u32_le(out.data() + 4, hdr.to_peer_id);
    out[8] = hdr.packet_type;
    out[9] = hdr.flags;
    out[10] = hdr.forward_counter;
    out[11] = hdr.reserved;
    put_u32_le(out.data() + 12, hdr.len);
}

/** @brief 解码 16 字节 PeerManagerHeader（不校验 len 合理性）。 */
bool decode_peer_manager_header(Bytes in, PeerManagerHeader& out) noexcept {
    if (in.size() < kPeerManagerHeaderSize) {
        return false;
    }
    out.from_peer_id = get_u32_le(in.data());
    out.to_peer_id = get_u32_le(in.data() + 4);
    out.packet_type = in[8];
    out.flags = in[9];
    out.forward_counter = in[10];
    out.reserved = in[11];
    out.len = get_u32_le(in.data() + 12);
    return true;
}

/** @brief 读取 TCP 帧的 u32 小端长度前缀。 */
bool decode_tcp_tunnel_header(Bytes in, uint32_t& body_len) noexcept {
    if (in.size() < kTcpTunnelHeaderSize) {
        return false;
    }
    body_len = get_u32_le(in.data());
    return true;
}

/** @brief 写入 TCP 帧的 u32 小端长度前缀。 */
void encode_tcp_tunnel_header(MutBytes out, uint32_t body_len) noexcept {
    if (out.size() < kTcpTunnelHeaderSize) {
        return;
    }
    put_u32_le(out.data(), body_len);
}

/** @brief 编码 8 字节 UDP mux 头。 */
void encode_udp_tunnel_header(MutBytes out, const UdpTunnelHeader& hdr) noexcept {
    if (out.size() < kUdpTunnelHeaderSize) {
        return;
    }
    put_u32_le(out.data(), hdr.conn_id);
    out[4] = hdr.msg_type;
    out[5] = hdr.padding;
    put_u16_le(out.data() + 6, hdr.len);
}

/** @brief 解码 8 字节 UDP mux 头。 */
bool decode_udp_tunnel_header(Bytes in, UdpTunnelHeader& out) noexcept {
    if (in.size() < kUdpTunnelHeaderSize) {
        return false;
    }
    out.conn_id = get_u32_le(in.data());
    out.msg_type = in[4];
    out.padding = in[5];
    out.len = get_u16_le(in.data() + 6);
    return true;
}

}  // namespace et
