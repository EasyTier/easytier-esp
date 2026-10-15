/**
 * @file fragment.cpp
 * @brief RPC 分片与重组实现（分片尺寸计算与上游 pick_piece_len_for_budget 一致）。
 */
#include "rpc/fragment.hpp"

#include <cstring>

#include "support/log.hpp"
#include "support/time.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.RPC";

/** @brief 用空 body 编码一片，得到该片的"基础长度"（用于计算可分给 body 的空间）。 */
size_t base_encoded_len(const RpcSendTemplate& tmpl, uint32_t total_pieces,
                        uint32_t piece_idx) noexcept {
    std::array<uint8_t, kMaxRpcPacketEncodedLen> scratch{};
    RpcPacketRef packet{};
    packet.from_peer = tmpl.from_peer;
    packet.to_peer = tmpl.to_peer;
    packet.transaction_id = tmpl.transaction_id;
    packet.is_request = tmpl.is_request;
    packet.trace_id = tmpl.trace_id;
    packet.total_pieces = total_pieces;
    packet.piece_idx = piece_idx;
    // 上游规则：首片一定带 descriptor；压缩算法为 None 时其余分片也带。
    packet.has_descriptor = (piece_idx == 0) || (tmpl.compression.algo == kCompressionNone);
    packet.descriptor = tmpl.descriptor;
    packet.has_compression = (piece_idx == 0);
    packet.compression = tmpl.compression;
    return encode_rpc_packet(packet, scratch);
}

/**
 * @brief 计算本片可分到的 body 长度。
 * @details 与上游 pick_piece_len_for_budget 等价：
 *          budget = max_encoded - base；为 body 的长度前缀预留 1 + varint 长度。
 */
size_t pick_piece_len(size_t base_len, size_t remaining) noexcept {
    if (remaining == 0) {
        return 0;
    }
    if (base_len + 3 > kMaxRpcPacketEncodedLen) {
        ET_LOGW(kTag, "rpc metadata exceeds udp budget, falling back to a minimal piece");
        return 1;
    }
    const size_t budget = kMaxRpcPacketEncodedLen - base_len;
    const size_t reserved_for_body_header = 1 + pb_varint_size(budget);
    const size_t available = (budget > reserved_for_body_header)
                                 ? (budget - reserved_for_body_header)
                                 : 1;
    const size_t piece = (remaining < available) ? remaining : available;
    return piece == 0 ? 1 : piece;
}

}  // namespace

/** @brief 逐片推算分片方案（片数上限 kRpcMaxPieces）。 */
bool plan_rpc_slices(const RpcSendTemplate& tmpl, size_t content_len, RpcPlan& out) noexcept {
    out = RpcPlan{};
    if (content_len == 0) {
        // 上游对空内容返回单个 (0,0) 分片。
        out.slices[0] = RpcSlice{0, 0};
        out.count = 1;
        return true;
    }
    size_t offset = 0;
    while (offset < content_len) {
        if (out.count >= kRpcMaxPieces) {
            ET_LOGE(kTag, "rpc content too large for %u pieces", static_cast<unsigned>(kRpcMaxPieces));
            return false;
        }
        const uint32_t piece_idx = static_cast<uint32_t>(out.count);
        const size_t base_len = base_encoded_len(tmpl, /*total_pieces placeholder*/ 0xFFFFFFFFu,
                                                 piece_idx == 0 ? 0u : 0xFFFFFFFFu);
        const size_t piece_len = pick_piece_len(base_len, content_len - offset);
        out.slices[out.count] = RpcSlice{static_cast<uint32_t>(offset),
                                         static_cast<uint32_t>(piece_len)};
        ++out.count;
        offset += piece_len;
    }
    return true;
}

/** @brief 编码一个分片（首片带 compression_info，None 算法时各片都带 descriptor）。 */
size_t encode_rpc_piece(const RpcSendTemplate& tmpl, Bytes content, uint32_t total_pieces,
                        uint32_t piece_idx, MutBytes out) noexcept {
    RpcPacketRef packet{};
    packet.from_peer = tmpl.from_peer;
    packet.to_peer = tmpl.to_peer;
    packet.transaction_id = tmpl.transaction_id;
    packet.body = content;
    packet.is_request = tmpl.is_request;
    packet.total_pieces = total_pieces;
    packet.piece_idx = piece_idx;
    packet.trace_id = tmpl.trace_id;
    packet.has_descriptor = (piece_idx == 0) || (tmpl.compression.algo == kCompressionNone);
    packet.descriptor = tmpl.descriptor;
    packet.has_compression = (piece_idx == 0);
    packet.compression = tmpl.compression;
    return encode_rpc_packet(packet, out);
}

/** @brief 找到已存在的槽位。 */
RpcReassembler::Slot* RpcReassembler::find(const Key& key) noexcept {
    for (Slot& slot : slots_) {
        if (slot.used && slot.key == key) {
            return &slot;
        }
    }
    return nullptr;
}

/**
 * @brief 找到或分配槽位。
 * @details 优先复用同键槽位；否则使用空槽位；都不可用时先回收最旧的槽位
 *          （重组是尽力而为的：宁可丢弃最旧的不完整消息，也不阻塞新消息）。
 */
RpcReassembler::Slot* RpcReassembler::find_or_allocate(const Key& key, uint32_t total_pieces,
                                                       uint32_t now_ms) noexcept {
    if (Slot* existing = find(key); existing != nullptr) {
        return existing;
    }
    for (Slot& slot : slots_) {
        if (!slot.used) {
            slot = Slot{};
            slot.used = true;
            slot.key = key;
            slot.total_pieces = total_pieces;
            slot.first_seen_ms = now_ms;
            return &slot;
        }
    }
    // 无空闲槽位：回收最早创建的槽位并计数。
    Slot* oldest = &slots_[0];
    for (Slot& slot : slots_) {
        if (elapsed_ms(now_ms, slot.first_seen_ms) > elapsed_ms(now_ms, oldest->first_seen_ms)) {
            oldest = &slot;
        }
    }
    ET_LOGW(kTag, "reassembly slots exhausted, dropping oldest message");
    *oldest = Slot{};
    oldest->used = true;
    oldest->key = key;
    oldest->total_pieces = total_pieces;
    oldest->first_seen_ms = now_ms;
    return oldest;
}

/** @brief 送入一个分片并尝试完成重组。 */
RpcReassembleResult RpcReassembler::feed(uint32_t generation, const RpcPacketRef& packet,
                                         uint32_t now_ms, Bytes& out_body) noexcept {
    out_body = Bytes{};
    const uint32_t total = packet.total_pieces;
    const uint32_t idx = packet.piece_idx;

    // 历史单包：total_pieces == 0 且 piece_idx == 0。
    if (total == 0) {
        if (idx != 0) {
            return RpcReassembleResult::Rejected;
        }
        if (packet.body.size() > kRpcMaxBodyBytes) {
            return RpcReassembleResult::Rejected;
        }
        // 单包直接返回输入缓冲中的 body（由调用方持有，生命周期覆盖本次处理）。
        out_body = packet.body;
        return RpcReassembleResult::Complete;
    }

    if (total > kRpcMaxPieces || idx >= total) {
        return RpcReassembleResult::Rejected;
    }
    // 片 0 必须携带 descriptor（与上游一致），乱序到达时也按此规则校验。
    if (idx == 0 && !packet.has_descriptor) {
        return RpcReassembleResult::Rejected;
    }

    Key key{};
    key.from_peer = packet.from_peer;
    key.to_peer = packet.to_peer;
    key.transaction_id = packet.transaction_id;
    key.is_request = packet.is_request;
    key.generation = generation;

    Slot* slot = find(key);
    if (slot != nullptr && slot->total_pieces != total) {
        // 同一事务出现不同的分片总数：视为冲突，丢弃该槽位。
        ET_LOGW(kTag, "conflicting total_pieces for the same rpc transaction");
        slot->used = false;
        return RpcReassembleResult::Rejected;
    }
    if (slot == nullptr) {
        slot = find_or_allocate(key, total, now_ms);
        if (slot == nullptr) {
            return RpcReassembleResult::Rejected;
        }
    }

    if (slot->have[idx]) {
        // 一致重复片：忽略；内容不同则视为冲突。
        if (slot->piece_len[idx] != packet.body.size()) {
            slot->used = false;
            return RpcReassembleResult::Rejected;
        }
        return RpcReassembleResult::Incomplete;
    }

    if (slot->body_len + packet.body.size() > kRpcMaxBodyBytes) {
        slot->used = false;
        return RpcReassembleResult::Rejected;
    }

    // 按到达顺序累积，并记录该片在缓冲中的位置，完成时按片号重组。
    std::memcpy(slot->rx.data() + slot->body_len, packet.body.data(), packet.body.size());
    slot->piece_len[idx] = static_cast<uint16_t>(packet.body.size());
    slot->piece_pos[idx] = static_cast<uint16_t>(slot->body_len);
    slot->have[idx] = true;
    slot->body_len += static_cast<uint32_t>(packet.body.size());
    ++slot->received;

    if (slot->received != slot->total_pieces) {
        return RpcReassembleResult::Incomplete;
    }

    // 所有分片到齐：按片号顺序拼接（分片可以乱序到达）。
    uint32_t offset = 0;
    for (uint32_t i = 0; i < slot->total_pieces; ++i) {
        const uint16_t len = slot->piece_len[i];
        if (len != 0) {
            std::memcpy(slot->ordered.data() + offset, slot->rx.data() + slot->piece_pos[i], len);
        }
        offset += len;
    }
    out_body = Bytes(slot->ordered.data(), offset);
    slot->used = false;  // 完成后立即回收
    return RpcReassembleResult::Complete;
}

/** @brief 回收超时槽位。 */
void RpcReassembler::expire(uint32_t now_ms) noexcept {
    for (Slot& slot : slots_) {
        if (slot.used && elapsed_ms(now_ms, slot.first_seen_ms) > kRpcReassemblyTimeoutMs) {
            ET_LOGW(kTag, "rpc reassembly timed out");
            slot.used = false;
        }
    }
}

/** @brief 清空全部槽位。 */
void RpcReassembler::reset() noexcept {
    for (Slot& slot : slots_) {
        slot.used = false;
    }
}

/** @brief 统计占用中的槽位。 */
size_t RpcReassembler::active_slots() const noexcept {
    size_t count = 0;
    for (const Slot& slot : slots_) {
        if (slot.used) {
            ++count;
        }
    }
    return count;
}

}  // namespace et
