/**
 * @file sync_messages.cpp
 * @brief 路由同步请求/响应编解码实现。
 */
#include "route/sync_messages.hpp"

#include "protocol/messages.hpp"

namespace et {
namespace {

/** @brief 读取一个可能是 packed 或逐个 varint 的 repeated uint32 字段。 */
[[nodiscard]] bool read_repeated_u32(const PbField& field,
                                     std::array<uint32_t, kPbMaxRepeatedItems>& out,
                                     size_t& count) noexcept {
    if (field.type == PbWireType::Varint) {
        if (count >= out.size()) {
            return false;
        }
        out[count++] = static_cast<uint32_t>(field.varint);
        return true;
    }
    if (field.type != PbWireType::LengthDelimited) {
        return false;
    }
    return for_each_packed_varint(field.data, kPbMaxRepeatedItems - count,
                                  [&out, &count](uint64_t value) {
                                      out[count++] = static_cast<uint32_t>(value);
                                  });
}

/**
 * @brief 写出一个 packed repeated uint32 字段（tag + 长度 + 逐个 varint）。
 * @note 与 read_repeated_u32 对称；两个 missing 列表共用同一编码形态。
 */
[[nodiscard]] bool put_packed_u32(ProtoWriter& w, uint32_t field, const uint32_t* values,
                                  size_t count) noexcept {
    if (count == 0) {
        return true;
    }
    uint8_t payload[256];
    ProtoWriter payload_writer(payload);
    for (size_t i = 0; i < count; ++i) {
        if (!payload_writer.put_varint(values[i])) {
            return false;
        }
    }
    return w.put_bytes(field, payload_writer.written());
}

}  // namespace

/** @brief 解码完整同步请求（含 peer_infos 与 conn_info oneof）。 */
bool decode_sync_request(Bytes in, SyncRequestView& out) noexcept {
    out = SyncRequestView{};
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        switch (field.field) {
            case 1:
                if (field.type != PbWireType::Varint) return false;
                out.header.my_peer_id = static_cast<uint32_t>(field.varint);
                break;
            case 2:
                if (field.type != PbWireType::Varint) return false;
                out.header.my_session_id = field.varint;
                break;
            case 3:
                if (field.type != PbWireType::Varint) return false;
                out.header.is_initiator = field.varint != 0;
                break;
            case 4:
                if (field.type != PbWireType::LengthDelimited) return false;
                out.has_peer_infos = true;
                out.peer_infos = field.data;
                break;
            case 5:
                if (field.type != PbWireType::LengthDelimited) return false;
                out.has_conn_bitmap = true;
                out.conn_bitmap = field.data;
                break;
            case 7:
                if (field.type != PbWireType::LengthDelimited) return false;
                out.has_conn_list = true;
                out.conn_list = field.data;
                break;
            default:
                break;  // tag 6 = foreign_network_infos，未使用
        }
    }
    return !reader.failed();
}

/** @brief 编码完整同步请求（本实现只发 tag 7 的 conn_list 形式）。 */
size_t encode_sync_request(const SyncRequestFields& fields, MutBytes out) noexcept {
    ProtoWriter w(out);
    w.put_uint32_present(1, fields.header.my_peer_id);
    if (!w.put_tag(2, PbWireType::Varint) || !w.put_varint(fields.header.my_session_id)) {
        return 0;
    }
    w.put_bool(3, fields.header.is_initiator);
    if (!fields.peer_infos.empty() && !w.put_bytes(4, fields.peer_infos)) {
        return 0;
    }
    if (!fields.conn_list.empty() && !w.put_bytes(7, fields.conn_list)) {
        return 0;
    }
    return w.ok() ? w.size() : 0;
}

/** @brief 解码同步响应（missing 列表支持 packed 与逐个 varint）。 */
bool decode_sync_response(Bytes in, SyncResponseView& out) noexcept {
    out = SyncResponseView{};
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        switch (field.field) {
            case 1:
                if (field.type != PbWireType::Varint) return false;
                out.is_initiator = field.varint != 0;
                break;
            case 2:
                if (field.type != PbWireType::Varint) return false;
                out.session_id = field.varint;
                break;
            case 3:
                if (field.type != PbWireType::Varint) return false;
                // 字段存在即视为有错误，即使取值为 0（DuplicatePeerId）。
                out.has_error = true;
                out.error_code = static_cast<int32_t>(field.varint);
                break;
            case 4:
                if (!read_repeated_u32(field, out.missing_peer_ids, out.missing_peer_count)) {
                    return false;
                }
                break;
            case 5:
                if (!read_repeated_u32(field, out.missing_conn_peer_ids,
                                       out.missing_conn_peer_count)) {
                    return false;
                }
                break;
            default:
                break;
        }
    }
    return !reader.failed();
}

/** @brief 编码同步响应；has_error 时用显式存在性写出 error（含 0）。 */
size_t encode_sync_response(const SyncResponseView& view, MutBytes out) noexcept {
    ProtoWriter w(out);
    w.put_bool(1, view.is_initiator);
    w.put_uint64(2, view.session_id);
    if (view.has_error && !w.put_enum_present(3, view.error_code)) {
        return 0;
    }
    if (!put_packed_u32(w, 4, view.missing_peer_ids.data(), view.missing_peer_count) ||
        !put_packed_u32(w, 5, view.missing_conn_peer_ids.data(), view.missing_conn_peer_count)) {
        return 0;
    }
    return w.ok() ? w.size() : 0;
}

}  // namespace et
