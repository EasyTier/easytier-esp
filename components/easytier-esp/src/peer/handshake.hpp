/**
 * @file handshake.hpp
 * @brief 非安全模式握手与保活报文的构造/校验。
 *
 * @details 握手（packet_type=2, to_peer_id=0）的 payload 是 protobuf
 * peer_rpc.HandshakeRequest。校验规则与上游一致：
 *  - network_secret_digest 必须为 32 字节；
 *  - network_name 与 digest 必须与本端一致；
 *  - 对端 my_peer_id 不得等于本端 peer id（否则是连到自己）。
 */
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include "protocol/messages.hpp"
#include "protocol/wire.hpp"

namespace et {

/** @brief 握手时期望的本端身份。 */
struct HandshakeExpectation {
    std::string_view network_name{};           ///< 本端网络名
    std::array<uint8_t, 32> digest{};          ///< 本端网络密钥摘要
    uint32_t my_peer_id = 0;                   ///< 本端 peer id
};

/** @brief 握手校验结果。 */
enum class HandshakeResult : uint8_t {
    Ok,              ///< 校验通过
    NotHandshake,    ///< packet_type 不是 HandShake
    Malformed,       ///< protobuf 解析失败
    SelfPeer,        ///< 对端 peer id 与本端相同
    NameMismatch,    ///< 网络名不一致
    DigestMismatch,  ///< digest 长度不足 32 或内容不一致
};

/**
 * @brief 校验收到的握手报文。
 * @param hdr 已解析的 PeerManagerHeader
 * @param payload PM 头之后的 payload
 * @param expect 本端期望身份
 * @param remote_peer_id 输出对端 peer id
 * @return 校验结果
 */
[[nodiscard]] HandshakeResult validate_handshake(const PeerManagerHeader& hdr, Bytes payload,
                                                 const HandshakeExpectation& expect,
                                                 uint32_t& remote_peer_id) noexcept;

/**
 * @brief 构造完整握手帧（PM 头 + payload）。
 * @param expect 本端身份
 * @param from_peer_id 本端 peer id
 * @param out 输出缓冲区，容量需 >= 16 + payload
 * @return 写入字节数；0 表示失败
 */
[[nodiscard]] size_t build_handshake_packet(const HandshakeExpectation& expect,
                                            uint32_t from_peer_id, MutBytes out) noexcept;

/**
 * @brief 构造 Ping 报文。
 * @note 序号为 **u32 小端（4 字节）**，这是上游实现的实际格式（不是 u64）。
 * @return 写入字节数；0 表示失败
 */
[[nodiscard]] size_t build_ping_packet(uint32_t from_peer_id, uint32_t to_peer_id, uint32_t seq,
                                       MutBytes out) noexcept;

/**
 * @brief 构造 Pong 报文。
 * @details 与上游一致：保留 ping 的 PM from/to 与 payload，仅把 packet_type 改为 Pong。
 * @return 写入字节数；0 表示失败
 */
[[nodiscard]] size_t build_pong_packet(const PeerManagerHeader& ping_hdr, Bytes ping_payload,
                                       MutBytes out) noexcept;

/**
 * @brief 校验 Pong 是否匹配期望序号。
 * @return true 表示 payload 至少 4 字节且序号匹配
 */
[[nodiscard]] bool pong_matches(Bytes payload, uint32_t expected_seq) noexcept;

}  // namespace et
