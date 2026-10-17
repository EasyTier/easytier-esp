/**
 * @file wire.hpp
 * @brief EasyTier 传输分帧与 PeerManagerHeader 定义。
 *
 * @details
 * - 所有多字节字段均为小端。
 * - 布局用常量 + static_assert 描述（见 wire.cpp），运行期不通过 packed 结构体
 *   访问线上数据。
 * - 两种长度含义不同，务必区分：
 *   - @ref PeerManagerHeader::len 保存**加密前的明文 payload 长度**；
 *   - TCP/UDP 传输头中的长度保存**实际字节数**，包含 16 字节 PM 头、实际
 *     payload 以及 28 字节 AEAD 尾。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "support/bytes.hpp"

namespace et {

/** @brief TCP 帧头长度（u32 小端长度前缀）。 */
inline constexpr size_t kTcpTunnelHeaderSize = 4;
/**
 * @brief 上游 UDP mux 头长度。
 * @note 本组件只实现 TCP 传输；这个常量仅用于对齐上游计算 RPC 分片预算的口径
 *       （分片按 UDP payload 预算切分，中继转 UDP 时也合法）。
 */
inline constexpr size_t kUdpTunnelHeaderSize = 8;
/** @brief PeerManagerHeader 长度。 */
inline constexpr size_t kPeerManagerHeaderSize = 16;
/** @brief 上游 FramedReader 允许的最大 TCP body 长度（PM 头 + payload + AEAD 尾）。 */
inline constexpr size_t kMaxTcpBodyLen = 2000;
/** @brief 无压缩 AES profile 下最大明文 payload（2000 - 16 - 28）。 */
inline constexpr size_t kMaxPlainPayload = kMaxTcpBodyLen - kPeerManagerHeaderSize - 28;
/** @brief 转发计数上限，超过即丢弃，用于防环兜底。 */
inline constexpr uint8_t kForwardCounterLimit = 7;

/** @brief 握手 magic（接收端不校验，但发送端必须与上游一致）。 */
inline constexpr uint32_t kHandshakeMagic = 0xd1e1a5e1u;
/** @brief 握手协议版本。 */
inline constexpr uint32_t kProtocolVersion = 1;

/**
 * @brief PeerManagerHeader 中的 packet_type 取值（与上游 PacketType 对齐）。
 * @note 标注"不支持/上游内部使用"的枚举值只用于对齐上游线格式，代码中不解析。
 */
enum class PacketType : uint8_t {
    Invalid = 0,                ///< 非法
    Data = 1,                   ///< 虚拟 IP 数据报文
    HandShake = 2,              ///< 握手
    RoutePacket = 3,            ///< 上游已废弃
    Ping = 4,                   ///< 保活探测
    Pong = 5,                   ///< 保活应答
    TaRpc = 6,                  ///< 上游已废弃
    Route = 7,                  ///< 上游已废弃
    RpcReq = 8,                 ///< RPC 请求
    RpcResp = 9,                ///< RPC 响应
    ForeignNetworkPacket = 10,  ///< 外部网络报文（本组件不支持）
    KcpSrc = 11,                ///< KCP（不支持）
    KcpDst = 12,                ///< KCP（不支持）
    NoiseHandshakeMsg1 = 13,    ///< Noise 安全模式（不支持）
    NoiseHandshakeMsg2 = 14,    ///< Noise 安全模式（不支持）
    NoiseHandshakeMsg3 = 15,    ///< Noise 安全模式（不支持）
    QuicSrc = 16,               ///< QUIC（不支持）
    QuicDst = 17,               ///< QUIC（不支持）
    DataWithKcpSrcModified = 18,   ///< 上游内部使用
    DataWithQuicSrcModified = 19,  ///< 上游内部使用
    RelayHandshake = 20,           ///< 中继凭证握手（不支持）
    RelayHandshakeAck = 21,        ///< 中继凭证握手应答（不支持）
};

/**
 * @brief PeerManagerHeader 的 flags 位定义。
 * @note 本组件只产生/解释 @ref Encrypted 与 @ref Compressed 两位；其余常量按上游
 *       线格式记录，便于对照，代码中不使用。
 */
namespace pm_flags {

inline constexpr uint8_t Encrypted = 0x01;      ///< payload 已 AES-GCM 加密
inline constexpr uint8_t LatencyFirst = 0x02;   ///< 延迟优先
inline constexpr uint8_t ExitNode = 0x04;       ///< 出口节点
inline constexpr uint8_t NoProxy = 0x08;        ///< 不做代理
inline constexpr uint8_t Compressed = 0x10;     ///< 已压缩（本 profile 不产生，收到即丢弃）
inline constexpr uint8_t LivenessProbe = 0x20;  ///< 上游已废弃，本组件不产生
inline constexpr uint8_t NotSendToTun = 0x40;   ///< 不投递到网卡
inline constexpr uint8_t LivenessEcho = 0x80;   ///< 上游 liveness 扩展，本组件不宣告

}  // namespace pm_flags

/**
 * @brief 16 字节 PeerManagerHeader。
 *
 * @note 转发时 @ref to_peer_id 始终保持最终目的节点不变，只更新
 *       @ref forward_counter 与所选传输连接。
 */
struct PeerManagerHeader {
    uint32_t from_peer_id = 0;    ///< 源节点 peer id
    uint32_t to_peer_id = 0;      ///< 目的节点 peer id；0 表示未指定（握手阶段）
    uint8_t packet_type = 0;      ///< @ref PacketType
    uint8_t flags = 0;            ///< @ref pm_flags
    uint8_t forward_counter = 0;  ///< 转发跳数，源节点填 1
    uint8_t reserved = 0;         ///< 上游 liveness token，本 profile 恒为 0
    uint32_t len = 0;             ///< payload 长度（不含本头，且为加密前长度）
};

/**
 * @brief 编码 PeerManagerHeader。
 * @param out 输出缓冲区，长度不足 16 时不做任何写入
 * @param hdr 头内容
 */
void encode_peer_manager_header(MutBytes out, const PeerManagerHeader& hdr) noexcept;

/**
 * @brief 解码 PeerManagerHeader。
 * @param in 输入字节
 * @param out 输出头
 * @return true 表示输入至少 16 字节且解码成功（不校验 len 的合理性）
 */
[[nodiscard]] bool decode_peer_manager_header(Bytes in, PeerManagerHeader& out) noexcept;

/**
 * @brief 读取 TCP 帧的 u32 小端长度前缀。
 * @param in 输入字节，至少 4 字节
 * @param body_len 输出 body 长度（含 PM 头与 AEAD 尾）
 */
[[nodiscard]] bool decode_tcp_tunnel_header(Bytes in, uint32_t& body_len) noexcept;

/** @brief 写入 TCP 帧的 u32 小端长度前缀。 */
void encode_tcp_tunnel_header(MutBytes out, uint32_t body_len) noexcept;

}  // namespace et
