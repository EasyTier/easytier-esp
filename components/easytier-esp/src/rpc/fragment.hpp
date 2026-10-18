/**
 * @file fragment.hpp
 * @brief RPC 分片与重组。
 *
 * @details 分片预算按 UDP 计算（每个 RpcPacket 编码后 ≤1248 B），当前走 TCP 也用
 *          同一预算，保证中继转 UDP 时合法。首片携带 descriptor/compression（None
 *          时每片都带）；接收侧兼容新式单包（1/0）与旧版单包（0/0）。重组键含
 *          from/to、事务、方向与连接代际；乱序/一致重复允许，冲突重复拒绝；超片数、
 *          字节数或时限立即回收。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "crypto/aes_gcm.hpp"
#include "protocol/messages.hpp"
#include "protocol/wire.hpp"
#include "support/config_limits.hpp"

namespace et {

/** @brief 上游 RPC 的 UDP payload 预算。 */
inline constexpr size_t kRpcPacketUdpPayloadBudget = 1300;
/** @brief 走 UDP 时 RpcPacket 之前的固定开销：UDP 头 + PM 头 + AEAD 尾。 */
inline constexpr size_t kUdpRpcTunnelOverhead =
    kUdpTunnelHeaderSize + kPeerManagerHeaderSize + kAeadTailSize;
/** @brief 单个 RpcPacket 编码后的最大长度。 */
inline constexpr size_t kMaxRpcPacketEncodedLen =
    kRpcPacketUdpPayloadBudget - kUdpRpcTunnelOverhead;
/** @brief 单个 RPC 消息允许的最大分片数。 */
inline constexpr size_t kRpcMaxPieces = 32;
/**
 * @brief 单个 RPC 消息重组后的最大字节数。
 * @note 每个槽位需要"到达顺序"与"片号顺序"两份缓冲，因此这里取 8 KiB：两个槽位
 *       合计约 32 KiB，与组件的 RPC 重组预算一致。超过该上限的消息会被拒绝
 *       （可见的 ResourceLimit），本组件拓扑上限为 32 个节点，正常同步远小于该值。
 */
inline constexpr size_t kRpcMaxBodyBytes = 8 * 1024;
/** @brief 重组超时（毫秒）。上游保留 10 秒，本实现取 3 秒（更早回收半包）。 */
inline constexpr uint32_t kRpcReassemblyTimeoutMs = 3000;

/** @brief 发送一个 RPC 报文所需的模板字段。 */
struct RpcSendTemplate {
    uint32_t from_peer = 0;      ///< 源 peer id
    uint32_t to_peer = 0;        ///< 目标 peer id
    int64_t transaction_id = 0;  ///< 事务 id
    bool is_request = true;      ///< 请求/响应
    int32_t trace_id = 0;        ///< 追踪 id
    RpcDescriptorRef descriptor{};  ///< 描述符（首片权威）
    RpcCompressionRef compression{};  ///< 压缩信息（None / None）
};

/** @brief 一片内容在原始 body 中的区间。 */
struct RpcSlice {
    uint32_t offset = 0;  ///< 相对 body 起始的偏移
    uint32_t len = 0;     ///< 本片长度
};

/** @brief 分片方案（最多 kRpcMaxPieces 片）。 */
struct RpcPlan {
    std::array<RpcSlice, kRpcMaxPieces> slices{};
    size_t count = 0;
};

/**
 * @brief 计算分片方案。
 * @param tmpl 报文模板（descriptor 参与编码长度计算）
 * @param content_len 待发送内容长度（RpcRequest/RpcResponse 编码后的字节数）
 * @param out 输出分片方案
 * @return false 表示片数超过 kRpcMaxPieces（内容过大）
 */
[[nodiscard]] bool plan_rpc_slices(const RpcSendTemplate& tmpl, size_t content_len,
                                   RpcPlan& out) noexcept;

/**
 * @brief 编码一个分片。
 * @param tmpl 报文模板
 * @param content 本片内容
 * @param total_pieces 分片总数
 * @param piece_idx 本片序号
 * @param out 输出缓冲区（建议 >= kMaxRpcPacketEncodedLen）
 * @return 写入字节数；0 表示缓冲区不足或编码失败
 */
[[nodiscard]] size_t encode_rpc_piece(const RpcSendTemplate& tmpl, Bytes content,
                                      uint32_t total_pieces, uint32_t piece_idx,
                                      MutBytes out) noexcept;

/** @brief 重组结果。 */
enum class RpcReassembleResult : uint8_t {
    Incomplete,  ///< 还需要更多分片
    Complete,    ///< 已得到完整 body
    Rejected,    ///< 非法或冲突的分片序列，调用方应丢弃并计数
};

/**
 * @brief RPC 分片重组器。
 *
 * @note 每个槽位约 16 KiB，因此**不要放在任务栈上**；本组件把它作为 Context 成员
 *       （堆上）持有。非线程安全：只由控制面任务访问。
 */
class RpcReassembler {
public:
    /** @brief 重组键：来源、目标、事务、方向、连接代际。 */
    struct Key {
        uint32_t from_peer = 0;
        uint32_t to_peer = 0;
        int64_t transaction_id = 0;
        bool is_request = false;
        uint32_t generation = 0;

        [[nodiscard]] bool operator==(const Key& other) const noexcept {
            return from_peer == other.from_peer && to_peer == other.to_peer &&
                   transaction_id == other.transaction_id && is_request == other.is_request &&
                   generation == other.generation;
        }
    };

    /**
     * @brief 送入一个分片。
     * @param generation 当前连接代际
     * @param packet 已解析的 RPC 报文
     * @param now_ms 当前时间（毫秒）
     * @param out_body 完成时指向内部缓冲中的完整 body
     * @return 重组状态
     */
    RpcReassembleResult feed(uint32_t generation, const RpcPacketRef& packet, uint32_t now_ms,
                             Bytes& out_body) noexcept;

    /** @brief 回收超时槽位。 */
    void expire(uint32_t now_ms) noexcept;

private:
    /** @brief 一个重组槽位。 */
    struct Slot {
        bool used = false;                              ///< 是否占用
        Key key{};                                      ///< 重组键
        uint32_t total_pieces = 0;                      ///< 分片总数
        uint32_t received = 0;                          ///< 已收到片数
        uint32_t body_len = 0;                          ///< 已累积字节数
        uint32_t first_seen_ms = 0;                     ///< 首次收到时间
        std::array<uint16_t, kRpcMaxPieces> piece_len{};  ///< 各片长度（按片号）
        std::array<uint16_t, kRpcMaxPieces> piece_pos{};  ///< 各片在到达缓冲中的偏移
        std::array<bool, kRpcMaxPieces> have{};         ///< 各片是否已收到
        /// 按到达顺序累积的内容。
        std::array<uint8_t, kRpcMaxBodyBytes> rx{};
        /// 组装完成后按片号顺序排好的内容（乱序到达时使用）。
        std::array<uint8_t, kRpcMaxBodyBytes> ordered{};
    };

    [[nodiscard]] Slot* find_or_allocate(const Key& key, uint32_t total_pieces,
                                         uint32_t now_ms) noexcept;
    [[nodiscard]] Slot* find(const Key& key) noexcept;

    std::array<Slot, kRpcReassemblySlots> slots_{};
};

}  // namespace et
