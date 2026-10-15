/**
 * @file protobuf.cpp
 * @brief 有界 protobuf 写入器与读取器实现。
 */
#include "protocol/protobuf.hpp"

#include <bit>
#include <cstring>

namespace et {

// fixed32/fixed64 按主机字节序直接拷贝，因此要求本平台是小端。
static_assert(std::endian::native == std::endian::little, "protobuf fixed 字段要求小端平台");

namespace {

/** @brief 字段号上限：2^29-1（tag 占 3 位线型）。 */
constexpr uint32_t kFieldNumberMax = (1u << 29) - 1;

}  // namespace

/** @brief 写入一个 varint（最多 10 字节）。 */
bool ProtoWriter::put_varint(uint64_t value) noexcept {
    uint8_t tmp[10];
    size_t n = 0;
    while (value >= 0x80) {
        tmp[n++] = static_cast<uint8_t>(value) | 0x80;
        value >>= 7;
    }
    tmp[n++] = static_cast<uint8_t>(value);
    if (len_ + n > out_.size()) {
        ok_ = false;
        return false;
    }
    std::memcpy(out_.data() + len_, tmp, n);
    len_ += n;
    return true;
}

/** @brief 写入字段 tag（field<<3 | wire_type）。 */
bool ProtoWriter::put_tag(uint32_t field, PbWireType type) noexcept {
    if (field == 0 || field > kFieldNumberMax) {
        ok_ = false;
        return false;
    }
    return put_varint((static_cast<uint64_t>(field) << 3) | static_cast<uint64_t>(type));
}

/** @brief 写入 uint32，0 省略（proto3 语义）。 */
bool ProtoWriter::put_uint32(uint32_t field, uint32_t value) noexcept {
    if (value == 0) {
        return true;
    }
    return put_tag(field, PbWireType::Varint) && put_varint(value);
}

/** @brief 显式写入 uint32（optional 字段取 0 时也写）。 */
bool ProtoWriter::put_uint32_present(uint32_t field, uint32_t value) noexcept {
    return put_tag(field, PbWireType::Varint) && put_varint(value);
}

/** @brief 写入 int32；负值符号扩展到 64 位，编码为 10 字节。 */
bool ProtoWriter::put_int32(uint32_t field, int32_t value) noexcept {
    if (value == 0) {
        return true;
    }
    return put_tag(field, PbWireType::Varint) &&
           put_varint(static_cast<uint64_t>(static_cast<int64_t>(value)));
}

/** @brief 显式写入 int32。 */
bool ProtoWriter::put_int32_present(uint32_t field, int32_t value) noexcept {
    return put_tag(field, PbWireType::Varint) &&
           put_varint(static_cast<uint64_t>(static_cast<int64_t>(value)));
}

/** @brief 写入 uint64，0 省略。 */
bool ProtoWriter::put_uint64(uint32_t field, uint64_t value) noexcept {
    if (value == 0) {
        return true;
    }
    return put_tag(field, PbWireType::Varint) && put_varint(value);
}

/** @brief 写入 int64，0 省略；负值为 10 字节 varint。 */
bool ProtoWriter::put_int64(uint32_t field, int64_t value) noexcept {
    if (value == 0) {
        return true;
    }
    return put_tag(field, PbWireType::Varint) && put_varint(static_cast<uint64_t>(value));
}

/** @brief 写入 bool，false 省略。 */
bool ProtoWriter::put_bool(uint32_t field, bool value) noexcept {
    if (!value) {
        return true;
    }
    return put_tag(field, PbWireType::Varint) && put_varint(1);
}

/** @brief 写入枚举（按 int32 处理）。 */
bool ProtoWriter::put_enum(uint32_t field, int32_t value) noexcept {
    return put_int32(field, value);
}

/** @brief 显式写入枚举。 */
bool ProtoWriter::put_enum_present(uint32_t field, int32_t value) noexcept {
    return put_int32_present(field, value);
}

/** @brief 写入 bytes，空值省略。 */
bool ProtoWriter::put_bytes(uint32_t field, Bytes value) noexcept {
    if (value.empty()) {
        return true;
    }
    return put_tag(field, PbWireType::LengthDelimited) && put_varint(value.size()) &&
           put_raw(value);
}

/** @brief 写入 string，空串省略。 */
bool ProtoWriter::put_string(uint32_t field, std::string_view value) noexcept {
    if (value.empty()) {
        return true;
    }
    return put_tag(field, PbWireType::LengthDelimited) && put_varint(value.size()) &&
           put_raw({reinterpret_cast<const uint8_t*>(value.data()), value.size()});
}

/** @brief 追加原始字节（缓冲区不足时置失败标记）。 */
bool ProtoWriter::put_raw(Bytes raw) noexcept {
    if (len_ + raw.size() > out_.size()) {
        ok_ = false;
        return false;
    }
    if (!raw.empty()) {
        std::memcpy(out_.data() + len_, raw.data(), raw.size());
        len_ += raw.size();
    }
    return true;
}

/** @brief 读取一个 varint；超过 10 字节或越界视为非法。 */
bool ProtoReader::read_varint(uint64_t& out) noexcept {
    uint64_t result = 0;
    for (int shift = 0; shift <= 63; shift += 7) {
        if (pos_ >= in_.size()) {
            failed_ = true;
            return false;
        }
        const uint8_t byte = in_[pos_++];
        result |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            out = result;
            return true;
        }
    }
    failed_ = true;
    return false;
}

/**
 * @brief 读取下一个字段。
 * @details 支持 wire type 0/1/2/5；遇到 group(3/4)、字段号 0、长度越界、varint
 *          超长或 fixed 字段不足时置 failed_ 并返回 false。
 */
bool ProtoReader::next(PbField& out) noexcept {
    if (pos_ >= in_.size()) {
        return false;  // 正常结束
    }
    uint64_t key = 0;
    if (!read_varint(key)) {
        return false;
    }
    const uint32_t field = static_cast<uint32_t>(key >> 3);
    const uint32_t wire = static_cast<uint32_t>(key & 0x7);
    if (field == 0 || field > kFieldNumberMax) {
        failed_ = true;
        return false;
    }
    out = PbField{};
    out.field = field;
    switch (wire) {
        case 0: {
            out.type = PbWireType::Varint;
            return read_varint(out.varint);
        }
        case 1: {
            out.type = PbWireType::Fixed64;
            if (in_.size() - pos_ < 8) {
                failed_ = true;
                return false;
            }
            out.varint = 0;
            for (int i = 0; i < 8; ++i) {
                out.varint |= static_cast<uint64_t>(in_[pos_ + static_cast<size_t>(i)]) << (8 * i);
            }
            pos_ += 8;
            return true;
        }
        case 2: {
            out.type = PbWireType::LengthDelimited;
            uint64_t len = 0;
            if (!read_varint(len)) {
                return false;
            }
            if (len > in_.size() - pos_) {
                failed_ = true;
                return false;
            }
            out.data = in_.subspan(pos_, static_cast<size_t>(len));
            pos_ += static_cast<size_t>(len);
            return true;
        }
        case 5: {
            out.type = PbWireType::Fixed32;
            if (in_.size() - pos_ < 4) {
                failed_ = true;
                return false;
            }
            out.varint = 0;
            for (int i = 0; i < 4; ++i) {
                out.varint |= static_cast<uint64_t>(in_[pos_ + static_cast<size_t>(i)]) << (8 * i);
            }
            pos_ += 4;
            return true;
        }
        default:
            // group(3/4) 等线型 EasyTier 不使用。
            failed_ = true;
            return false;
    }
}

}  // namespace et
