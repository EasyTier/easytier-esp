/**
 * @file route_info.cpp
 * @brief 路由拓扑元素编解码实现。
 */
#include "route/route_info.hpp"

#include <cstring>

#include "support/log.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.ROUTE";

/** @brief 展开 PeerFeatureFlag 的 bool 字段为位掩码（字段号 n → 位 n-1）。 */
[[nodiscard]] bool decode_feature_flags(Bytes in, uint16_t& out) noexcept {
    ProtoReader reader(in);
    PbField field;
    uint16_t flags = 0;
    while (reader.next(field)) {
        if (field.type != PbWireType::Varint) {
            return false;
        }
        if (field.varint == 0) {
            continue;
        }
        if (field.field == 0 || field.field > 11) {
            continue;  // 未知能力位忽略
        }
        flags |= static_cast<uint16_t>(1u << (field.field - 1));
    }
    if (reader.failed()) {
        return false;
    }
    out = flags;
    return true;
}

/** @brief 编码 PeerFeatureFlag（只写已置位的字段）。 */
bool encode_feature_flags(uint16_t flags, MutBytes out, size_t& out_len) noexcept {
    ProtoWriter w(out);
    for (uint32_t bit = 0; bit < 11; ++bit) {
        if ((flags & (1u << bit)) != 0) {
            w.put_bool(bit + 1, true);
        }
    }
    if (!w.ok()) {
        return false;
    }
    out_len = w.size();
    return true;
}

}  // namespace

/** @brief 解码 RoutePeerInfo（未知字段跳过，保留原始字节）。 */
bool decode_route_peer_info(Bytes in, RoutePeerInfoView& out) noexcept {
    out = RoutePeerInfoView{};
    out.raw = in;
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        switch (field.field) {
            case 1:
                if (field.type != PbWireType::Varint) return false;
                out.peer_id = static_cast<uint32_t>(field.varint);
                break;
            case 2:
                if (field.type != PbWireType::LengthDelimited) return false;
                if (!decode_uuid(field.data, out.inst_id)) return false;
                break;
            case 3:
                if (field.type != PbWireType::Varint) return false;
                out.cost = static_cast<uint32_t>(field.varint);
                break;
            case 4: {
                if (field.type != PbWireType::LengthDelimited) return false;
                // Ipv4Addr{ addr = 1 }
                ProtoReader ip_reader(field.data);
                PbField ip_field;
                while (ip_reader.next(ip_field)) {
                    if (ip_field.field == 1) {
                        if (ip_field.type != PbWireType::Varint) return false;
                        out.ipv4 = static_cast<uint32_t>(ip_field.varint);
                        out.has_ipv4 = true;
                    }
                }
                if (ip_reader.failed()) return false;
                break;
            }
            case 6:
                if (field.type != PbWireType::LengthDelimited) return false;
                out.hostname = std::string_view(reinterpret_cast<const char*>(field.data.data()),
                                                field.data.size());
                break;
            case 8: {
                if (field.type != PbWireType::LengthDelimited) return false;
                // google.protobuf.Timestamp{ seconds = 1; nanos = 2 }
                ProtoReader ts_reader(field.data);
                PbField ts_field;
                while (ts_reader.next(ts_field)) {
                    if (ts_field.field == 1 && ts_field.type == PbWireType::Varint) {
                        out.last_update_secs = ts_field.varint;
                    }
                }
                if (ts_reader.failed()) return false;
                break;
            }
            case 9:
                if (field.type != PbWireType::Varint) return false;
                out.version = static_cast<uint32_t>(field.varint);
                break;
            case 11:
                if (field.type != PbWireType::LengthDelimited) return false;
                if (!decode_feature_flags(field.data, out.feature_flags)) return false;
                break;
            case 12:
                if (field.type != PbWireType::Varint) return false;
                out.peer_route_id = field.varint;
                break;
            case 13:
                if (field.type != PbWireType::Varint) return false;
                out.network_length = static_cast<uint32_t>(field.varint);
                break;
            default:
                break;  // 未知/未使用字段：跳过，raw 中已保留
        }
    }
    return !reader.failed();
}

/** @brief 编码本节点 RoutePeerInfo（字段顺序与上游一致，零值按 proto3 省略）。 */
size_t encode_route_peer_info(const SelfRouteInfo& info, MutBytes out) noexcept {
    ProtoWriter w(out);
    w.put_uint32(1, info.peer_id);

    // inst_id（四个 uint32）
    {
        uint8_t buf[32];
        const size_t len = encode_uuid(info.inst_id, buf);
        if (len == 0 || !w.put_bytes(2, Bytes(buf, len))) {
            return 0;
        }
    }
    // cost（tag 3）：本节点固定为 0，proto3 语义下省略。
    if (info.has_ipv4) {
        uint8_t buf[16];
        ProtoWriter ip(buf);
        ip.put_uint32(1, info.ipv4);
        if (!ip.ok() || !w.put_bytes(4, ip.written())) {
            return 0;
        }
    }
    w.put_string(6, info.hostname);
    // 时间戳：接收端会用本地时间覆盖，仅作参考。
    if (info.last_update_secs != 0) {
        uint8_t buf[16];
        ProtoWriter ts(buf);
        ts.put_uint64(1, info.last_update_secs);
        if (!ts.ok() || !w.put_bytes(8, ts.written())) {
            return 0;
        }
    }
    w.put_uint32(9, info.version);
    w.put_string(10, kReportedEasyTierVersion);
    {
        uint8_t buf[32];
        size_t len = 0;
        if (!encode_feature_flags(info.feature_flags, buf, len)) {
            return 0;
        }
        if (len != 0 && !w.put_bytes(11, Bytes(buf, len))) {
            return 0;
        }
    }
    w.put_uint64(12, info.peer_route_id);
    w.put_uint32(13, info.network_length);
    return w.ok() ? w.size() : 0;
}

/** @brief 编码 RoutePeerInfos 外层消息。 */
size_t encode_route_peer_infos(const Bytes* items, size_t count, MutBytes out) noexcept {
    ProtoWriter w(out);
    for (size_t i = 0; i < count; ++i) {
        if (!w.put_bytes(1, items[i])) {
            return 0;
        }
    }
    return w.ok() ? w.size() : 0;
}

/** @brief 编码邻接表（connected 列表按 packed 编码，与上游 prost 一致）。 */
size_t encode_conn_list(const ConnList& list, MutBytes out) noexcept {
    ProtoWriter w(out);
    for (size_t i = 0; i < list.count; ++i) {
        const ConnRow& row = list.rows[i];
        uint8_t row_buf[256];
        ProtoWriter row_writer(row_buf);
        // PeerIdVersion{ peer_id = 1; version = 2 }
        {
            uint8_t id_buf[24];
            ProtoWriter id_writer(id_buf);
            id_writer.put_uint32(1, row.peer_id);
            id_writer.put_uint32(2, row.version);
            if (!id_writer.ok() || !row_writer.put_bytes(1, id_writer.written())) {
                return 0;
            }
        }
        if (row.connected_count != 0) {
            // proto3 的 repeated uint32 默认 packed：tag + 长度 + 连续 varint。
            uint8_t payload[256];
            ProtoWriter payload_writer(payload);
            for (size_t c = 0; c < row.connected_count; ++c) {
                if (!payload_writer.put_varint(row.connected[c])) {
                    return 0;
                }
            }
            if (!row_writer.put_bytes(2, payload_writer.written())) {
                return 0;
            }
        }
        if (!row_writer.ok() || !w.put_bytes(1, row_writer.written())) {
            return 0;
        }
    }
    return w.ok() ? w.size() : 0;
}

/** @brief 解码邻接表（展开每行的 connected_peer_ids，支持 packed/unpacked）。 */
bool decode_conn_list(Bytes in, ConnList& out) noexcept {
    out = ConnList{};
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        if (field.field != 1) {
            continue;  // 未知字段跳过
        }
        if (field.type != PbWireType::LengthDelimited || out.count >= kMaxRoutePeers) {
            return false;
        }
        ConnRow& row = out.rows[out.count];
        ProtoReader row_reader(field.data);
        PbField row_field;
        while (row_reader.next(row_field)) {
            switch (row_field.field) {
                case 1: {
                    if (row_field.type != PbWireType::LengthDelimited) return false;
                    ProtoReader id_reader(row_field.data);
                    PbField id_field;
                    while (id_reader.next(id_field)) {
                        if (id_field.field == 1 && id_field.type == PbWireType::Varint) {
                            row.peer_id = static_cast<uint32_t>(id_field.varint);
                        } else if (id_field.field == 2 && id_field.type == PbWireType::Varint) {
                            row.version = static_cast<uint32_t>(id_field.varint);
                        }
                    }
                    if (id_reader.failed()) return false;
                    break;
                }
                case 2: {
                    if (row_field.type == PbWireType::Varint) {
                        if (row.connected_count >= kMaxConnectedPerRow) return false;
                        row.connected[row.connected_count++] = static_cast<uint32_t>(row_field.varint);
                        break;
                    }
                    if (row_field.type != PbWireType::LengthDelimited) return false;
                    bool ok = true;
                    ok = for_each_packed_varint(row_field.data, kMaxConnectedPerRow,
                                               [&row](uint64_t value) {
                                                   if (row.connected_count < kMaxConnectedPerRow) {
                                                       row.connected[row.connected_count++] =
                                                           static_cast<uint32_t>(value);
                                                   }
                                               });
                    if (!ok) return false;
                    break;
                }
                default:
                    break;
            }
        }
        if (row_reader.failed()) {
            return false;
        }
        ++out.count;
    }
    return !reader.failed();
}

/** @brief 把位图展开成邻接表：每一行对应位图中的一个节点。 */
bool expand_conn_bitmap(const ConnBitmap& bitmap, ConnList& out) noexcept {
    out = ConnList{};
    if (bitmap.count > kMaxRoutePeers) {
        return false;
    }
    out.count = bitmap.count;
    for (size_t row = 0; row < bitmap.count; ++row) {
        out.rows[row].peer_id = bitmap.peer_ids[row].peer_id;
        out.rows[row].version = bitmap.peer_ids[row].version;
        for (size_t col = 0; col < bitmap.count; ++col) {
            if (bitmap.has_edge(row, col)) {
                if (out.rows[row].connected_count >= kMaxConnectedPerRow) {
                    return false;
                }
                out.rows[row].connected[out.rows[row].connected_count++] =
                    bitmap.peer_ids[col].peer_id;
            }
        }
    }
    return true;
}

/** @brief 位图查询：bit = row * N + col，字节内 LSB-first。 */
bool ConnBitmap::has_edge(size_t row, size_t col) const noexcept {
    if (row >= count || col >= count) {
        return false;
    }
    const size_t bit = row * count + col;
    const size_t byte = bit / 8;
    if (byte >= bitmap_len) {
        return false;
    }
    return (bitmap[byte] & static_cast<uint8_t>(1u << (bit % 8))) != 0;
}

/** @brief 解码邻接位图，并校验位图长度足以容纳 N*N 位。 */
bool decode_conn_bitmap(Bytes in, ConnBitmap& out) noexcept {
    out = ConnBitmap{};
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        switch (field.field) {
            case 1: {
                if (field.type != PbWireType::LengthDelimited) return false;
                ProtoReader id_reader(field.data);
                PbField id_field;
                PeerIdVersion entry{};
                while (id_reader.next(id_field)) {
                    if (id_field.field == 1 && id_field.type == PbWireType::Varint) {
                        entry.peer_id = static_cast<uint32_t>(id_field.varint);
                    } else if (id_field.field == 2 && id_field.type == PbWireType::Varint) {
                        entry.version = static_cast<uint32_t>(id_field.varint);
                    }
                }
                if (id_reader.failed() || out.count >= kMaxRoutePeers) {
                    return false;
                }
                out.peer_ids[out.count++] = entry;
                break;
            }
            case 2: {
                if (field.type != PbWireType::LengthDelimited) return false;
                if (field.data.size() > out.bitmap.size()) {
                    return false;
                }
                std::memcpy(out.bitmap.data(), field.data.data(), field.data.size());
                out.bitmap_len = field.data.size();
                break;
            }
            default:
                break;
        }
    }
    if (reader.failed()) {
        return false;
    }
    // 位图至少要有 ceil(N*N/8) 字节，否则视为损坏。
    const size_t needed = (out.count * out.count + 7) / 8;
    if (out.bitmap_len < needed) {
        ET_LOGW(kTag, "conn bitmap too short: %u < %u", static_cast<unsigned>(out.bitmap_len),
                static_cast<unsigned>(needed));
        return false;
    }
    return true;
}

}  // namespace et
