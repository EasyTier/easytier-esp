/**
 * @file messages.cpp
 * @brief EasyTier protobuf 消息的类型化编解码实现。
 *
 * @details 字段号与上游 .proto 一致；未知字段一律跳过，已知字段线型不匹配时
 * 判定为非法输入（与上游 prost 的行为一致）。
 */
#include "protocol/messages.hpp"

#include <cstring>

namespace et {
namespace {

/**
 * @brief 把 length-delimited 字段转成 string_view。
 * @return false 表示超过 kPbMaxStringLen
 */
[[nodiscard]] bool to_view(Bytes data, std::string_view& out) noexcept {
    if (data.size() > kPbMaxStringLen) {
        return false;
    }
    out = {reinterpret_cast<const char*>(data.data()), data.size()};
    return true;
}

/** @brief 解码 RpcDescriptor（域/proto/服务/方法序号）。 */
bool decode_descriptor(Bytes in, RpcDescriptorRef& out) noexcept {
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        switch (field.field) {
            case 1:
                if (field.type != PbWireType::LengthDelimited ||
                    !to_view(field.data, out.domain_name)) {
                    return false;
                }
                break;
            case 2:
                if (field.type != PbWireType::LengthDelimited ||
                    !to_view(field.data, out.proto_name)) {
                    return false;
                }
                break;
            case 3:
                if (field.type != PbWireType::LengthDelimited ||
                    !to_view(field.data, out.service_name)) {
                    return false;
                }
                break;
            case 4:
                if (field.type != PbWireType::Varint) {
                    return false;
                }
                out.method_index = static_cast<uint32_t>(field.varint);
                break;
            default:
                break;  // unknown field already skipped
        }
    }
    return !reader.failed();
}

/** @brief 解码 RpcCompressionInfo（algo / accepted_algo）。 */
bool decode_compression(Bytes in, RpcCompressionRef& out) noexcept {
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        if (field.type != PbWireType::Varint) {
            if (field.field == 1 || field.field == 2) {
                return false;
            }
            continue;
        }
        if (field.field == 1) {
            out.algo = static_cast<int32_t>(field.varint);
        } else if (field.field == 2) {
            out.accepted_algo = static_cast<int32_t>(field.varint);
        }
    }
    return !reader.failed();
}

}  // namespace

/**
 * @brief 编码 UUID（四个 uint32 字段）。
 * @details 字段 1 使用显式写入：这样即使四个字段全为 0，编码结果也不为 0 字节，
 *          调用方才能把"编码失败(0)"与"全零 UUID"区分开。
 */
size_t encode_uuid(const Uuid& id, MutBytes out) noexcept {
    ProtoWriter w(out);
    w.put_uint32_present(1, id.part1);
    w.put_uint32(2, id.part2);
    w.put_uint32(3, id.part3);
    w.put_uint32(4, id.part4);
    return w.ok() ? w.size() : 0;
}

/** @brief 解码 UUID；缺失的段按 0 处理。 */
bool decode_uuid(Bytes in, Uuid& out) noexcept {
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        if (field.type != PbWireType::Varint) {
            continue;
        }
        const uint32_t value = static_cast<uint32_t>(field.varint);
        switch (field.field) {
            case 1: out.part1 = value; break;
            case 2: out.part2 = value; break;
            case 3: out.part3 = value; break;
            case 4: out.part4 = value; break;
            default: break;
        }
    }
    return !reader.failed();
}

/**
 * @brief 解码握手请求。
 * @note features 只计数不保存（本 profile 不宣告该扩展）；digest 以视图返回，
 *       指向输入缓冲区。
 */
bool decode_handshake_request(Bytes in, HandshakeRequestView& out) noexcept {
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        switch (field.field) {
            case 1:
                if (field.type != PbWireType::Varint) return false;
                out.magic = static_cast<uint32_t>(field.varint);
                break;
            case 2:
                if (field.type != PbWireType::Varint) return false;
                out.my_peer_id = static_cast<uint32_t>(field.varint);
                break;
            case 3:
                if (field.type != PbWireType::Varint) return false;
                out.version = static_cast<uint32_t>(field.varint);
                break;
            case 4:
                if (field.type != PbWireType::LengthDelimited) return false;
                if (++out.feature_count > kPbMaxRepeatedItems) return false;
                break;
            case 5:
                if (field.type != PbWireType::LengthDelimited ||
                    !to_view(field.data, out.network_name)) {
                    return false;
                }
                break;
            case 6:
                if (field.type != PbWireType::LengthDelimited) return false;
                out.network_secret_digest = field.data;
                break;
            default:
                break;
        }
    }
    return !reader.failed();
}

/** @brief 编码握手请求（不写 features 字段）。 */
size_t encode_handshake_request(const HandshakeRequestView& req, MutBytes out) noexcept {
    ProtoWriter w(out);
    w.put_uint32(1, req.magic);
    w.put_uint32(2, req.my_peer_id);
    w.put_uint32(3, req.version);
    w.put_string(5, req.network_name);
    w.put_bytes(6, req.network_secret_digest);
    return w.ok() ? w.size() : 0;
}

/**
 * @brief 解码 RPC 报文。
 * @note descriptor 与 compression 可能缺失（历史单包或非首片）；调用方需按
 *       "片 0 权威"的原则处理。
 */
bool decode_rpc_packet(Bytes in, RpcPacketRef& out) noexcept {
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        switch (field.field) {
            case 1:
                if (field.type != PbWireType::Varint) return false;
                out.from_peer = static_cast<uint32_t>(field.varint);
                break;
            case 2:
                if (field.type != PbWireType::Varint) return false;
                out.to_peer = static_cast<uint32_t>(field.varint);
                break;
            case 3:
                if (field.type != PbWireType::Varint) return false;
                out.transaction_id = static_cast<int64_t>(field.varint);
                break;
            case 4:
                if (field.type != PbWireType::LengthDelimited) return false;
                if (!decode_descriptor(field.data, out.descriptor)) return false;
                out.has_descriptor = true;
                break;
            case 5:
                if (field.type != PbWireType::LengthDelimited) return false;
                out.body = field.data;
                break;
            case 6:
                if (field.type != PbWireType::Varint) return false;
                out.is_request = field.varint != 0;
                break;
            case 7:
                if (field.type != PbWireType::Varint) return false;
                out.total_pieces = static_cast<uint32_t>(field.varint);
                break;
            case 8:
                if (field.type != PbWireType::Varint) return false;
                out.piece_idx = static_cast<uint32_t>(field.varint);
                break;
            case 9:
                if (field.type != PbWireType::Varint) return false;
                out.trace_id = static_cast<int32_t>(field.varint);
                break;
            case 10:
                if (field.type != PbWireType::LengthDelimited) return false;
                if (!decode_compression(field.data, out.compression)) return false;
                out.has_compression = true;
                break;
            default:
                break;
        }
    }
    return !reader.failed();
}

/** @brief 编码 RPC 报文；descriptor/compression 先编码到栈上小缓冲再整体写入。 */
size_t encode_rpc_packet(const RpcPacketRef& packet, MutBytes out) noexcept {
    ProtoWriter w(out);
    w.put_uint32(1, packet.from_peer);
    w.put_uint32(2, packet.to_peer);
    w.put_int64(3, packet.transaction_id);
    if (packet.has_descriptor) {
        uint8_t scratch[512];
        ProtoWriter d(scratch);
        d.put_string(1, packet.descriptor.domain_name);
        d.put_string(2, packet.descriptor.proto_name);
        d.put_string(3, packet.descriptor.service_name);
        d.put_uint32(4, packet.descriptor.method_index);
        if (!d.ok() || !w.put_bytes(4, d.written())) {
            return 0;
        }
    }
    w.put_bytes(5, packet.body);
    w.put_bool(6, packet.is_request);
    w.put_uint32(7, packet.total_pieces);
    w.put_uint32(8, packet.piece_idx);
    w.put_int32(9, packet.trace_id);
    if (packet.has_compression) {
        uint8_t scratch[32];
        ProtoWriter c(scratch);
        c.put_enum(1, packet.compression.algo);
        c.put_enum(2, packet.compression.accepted_algo);
        if (!c.ok() || !w.put_bytes(10, c.written())) {
            return 0;
        }
    }
    return w.ok() ? w.size() : 0;
}

/** @brief 解码 RpcRequest（服务入参 + 超时）。 */
bool decode_rpc_request(Bytes in, RpcRequestRef& out) noexcept {
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        if (field.field == 2) {
            if (field.type != PbWireType::LengthDelimited) return false;
            out.request = field.data;
        } else if (field.field == 3) {
            if (field.type != PbWireType::Varint) return false;
            out.timeout_ms = static_cast<int32_t>(field.varint);
        }
    }
    return !reader.failed();
}

/** @brief 编码 RpcRequest。 */
size_t encode_rpc_request(Bytes request, int32_t timeout_ms, MutBytes out) noexcept {
    ProtoWriter w(out);
    w.put_bytes(2, request);
    w.put_int32(3, timeout_ms);
    return w.ok() ? w.size() : 0;
}

/**
 * @brief 解码 RpcResponse。
 * @note error 字段原样保留为 error_raw；has_error 用于区分"字段不存在"与
 *       "字段存在但取值为 0"（例如 DuplicatePeerId）。
 */
bool decode_rpc_response(Bytes in, RpcResponseRef& out) noexcept {
    ProtoReader reader(in);
    PbField field;
    while (reader.next(field)) {
        switch (field.field) {
            case 1:
                if (field.type != PbWireType::LengthDelimited) return false;
                out.response = field.data;
                break;
            case 2:
                if (field.type != PbWireType::LengthDelimited) return false;
                out.has_error = true;
                out.error_raw = field.data;
                break;
            case 3:
                if (field.type != PbWireType::Varint) return false;
                out.runtime_us = field.varint;
                break;
            default:
                break;
        }
    }
    return !reader.failed();
}

/**
 * @brief 编码 RpcResponse，并区分"空消息"与"失败"。
 * @details 服务返回空内容且无错误时，RpcResponse 的全部字段都是 proto3 默认值，
 *          编码结果合法地为 0 字节；调用方不能把它当成失败。
 */
bool encode_rpc_response_checked(Bytes response, Bytes error_raw, uint64_t runtime_us,
                                 MutBytes out, size_t& out_len) noexcept {
    ProtoWriter w(out);
    w.put_bytes(1, response);
    w.put_bytes(2, error_raw);
    w.put_uint64(3, runtime_us);
    out_len = w.ok() ? w.size() : 0;
    return w.ok();
}

/** @brief 编码 RpcResponse（error_raw 原样透传）。 */
size_t encode_rpc_response(Bytes response, Bytes error_raw, uint64_t runtime_us,
                           MutBytes out) noexcept {
    size_t out_len = 0;
    return encode_rpc_response_checked(response, error_raw, runtime_us, out, out_len) ? out_len : 0;
}

/**
 * @brief 编码 error.Error{execute_error{error_message}}。
 * @details 结构：Error.execute_error(6) = ExecuteError{error_message(1) = msg}。
 */
size_t encode_error_execute(std::string_view message, MutBytes out) noexcept {
    uint8_t inner[192];
    ProtoWriter inner_writer(inner);
    inner_writer.put_string(1, message);
    if (!inner_writer.ok()) {
        return 0;
    }
    ProtoWriter w(out);
    w.put_bytes(6, inner_writer.written());
    return w.ok() ? w.size() : 0;
}

}  // namespace et
