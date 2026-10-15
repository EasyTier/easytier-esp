/**
 * @file handshake.cpp
 * @brief 握手与 Ping/Pong 报文实现。
 */
#include "peer/handshake.hpp"

#include <cstring>

#include "support/bytes.hpp"

namespace et {

/** @brief 校验收到的握手报文（网络名、digest、peer id 冲突）。 */
HandshakeResult validate_handshake(const PeerManagerHeader& hdr, Bytes payload,
                                   const HandshakeExpectation& expect,
                                   uint32_t& remote_peer_id) noexcept {
    if (hdr.packet_type != static_cast<uint8_t>(PacketType::HandShake)) {
        return HandshakeResult::NotHandshake;
    }
    HandshakeRequestView view;
    if (!decode_handshake_request(payload, view)) {
        return HandshakeResult::Malformed;
    }
    // 对端 peer id 是权威值。
    remote_peer_id = view.my_peer_id;
    if (remote_peer_id == expect.my_peer_id) {
        return HandshakeResult::SelfPeer;
    }
    if (view.network_name != expect.network_name) {
        return HandshakeResult::NameMismatch;
    }
    if (view.network_secret_digest.size() != expect.digest.size()) {
        return HandshakeResult::DigestMismatch;
    }
    if (std::memcmp(view.network_secret_digest.data(), expect.digest.data(), expect.digest.size()) !=
        0) {
        return HandshakeResult::DigestMismatch;
    }
    return HandshakeResult::Ok;
}

/** @brief 构造完整握手帧（PM 头 to_peer_id 固定为 0）。 */
size_t build_handshake_packet(const HandshakeExpectation& expect, uint32_t from_peer_id,
                              MutBytes out) noexcept {
    if (out.size() < kPeerManagerHeaderSize + 16) {
        return 0;
    }
    HandshakeRequestView req;
    req.magic = kHandshakeMagic;
    req.my_peer_id = from_peer_id;
    req.version = kProtocolVersion;
    req.network_name = expect.network_name;
    req.network_secret_digest = expect.digest;

    const size_t payload_len =
        encode_handshake_request(req, out.subspan(kPeerManagerHeaderSize));
    if (payload_len == 0) {
        return 0;
    }
    PeerManagerHeader hdr;
    hdr.from_peer_id = from_peer_id;
    hdr.to_peer_id = 0;  // 握手阶段目的未指定
    hdr.packet_type = static_cast<uint8_t>(PacketType::HandShake);
    hdr.flags = 0;
    hdr.forward_counter = 1;
    hdr.reserved = 0;
    hdr.len = static_cast<uint32_t>(payload_len);
    encode_peer_manager_header(out, hdr);
    return kPeerManagerHeaderSize + payload_len;
}

/** @brief 构造 Ping 帧（payload 为 4 字节小端序号）。 */
size_t build_ping_packet(uint32_t from_peer_id, uint32_t to_peer_id, uint32_t seq,
                         MutBytes out) noexcept {
    if (out.size() < kPeerManagerHeaderSize + 4) {
        return 0;
    }
    PeerManagerHeader hdr;
    hdr.from_peer_id = from_peer_id;
    hdr.to_peer_id = to_peer_id;
    hdr.packet_type = static_cast<uint8_t>(PacketType::Ping);
    hdr.flags = 0;
    hdr.forward_counter = 1;
    hdr.reserved = 0;
    hdr.len = 4;
    encode_peer_manager_header(out, hdr);
    put_u32_le(out.data() + kPeerManagerHeaderSize, seq);
    return kPeerManagerHeaderSize + 4;
}

/** @brief 构造 Pong 帧（保留请求的 PM from/to 与 payload，只改类型）。 */
size_t build_pong_packet(const PeerManagerHeader& ping_hdr, Bytes ping_payload,
                         MutBytes out) noexcept {
    if (out.size() < kPeerManagerHeaderSize + ping_payload.size()) {
        return 0;
    }
    PeerManagerHeader hdr = ping_hdr;
    hdr.packet_type = static_cast<uint8_t>(PacketType::Pong);
    hdr.len = static_cast<uint32_t>(ping_payload.size());
    encode_peer_manager_header(out, hdr);
    if (!ping_payload.empty()) {
        std::memcpy(out.data() + kPeerManagerHeaderSize, ping_payload.data(), ping_payload.size());
    }
    return kPeerManagerHeaderSize + ping_payload.size();
}

/** @brief Pong 序号匹配（payload 至少 4 字节）。 */
bool pong_matches(Bytes payload, uint32_t expected_seq) noexcept {
    if (payload.size() < 4) {
        return false;
    }
    return get_u32_le(payload.data()) == expected_seq;
}

}  // namespace et
