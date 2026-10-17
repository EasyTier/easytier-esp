/**
 * @file protobuf.hpp
 * @brief 有界 protobuf 编解码器。
 *
 * @details 只实现 EasyTier 控制面需要的子集，但对读取方严格做边界检查，并且
 * 能跳过任意线型的未知字段，从而在较新版本的对端出现时不会越界读取。
 *
 * 支持范围：
 * - varint / fixed32 / fixed64 / length-delimited；
 * - 未知字段跳过（wire type 0/1/2/5）；
 * - packed 形式的 repeated 标量（见 for_each_packed_varint）；unpacked 由 ProtoReader 逐个读取；
 * - 负数 int32/int64 的 10 字节 zigzag-free 编码（符号扩展到 64 位）；
 * - proto3 的零值省略语义与 optional 字段的显式存在性。
 *
 * 不支持：group（wire type 3/4）。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "support/bytes.hpp"

namespace et {

/** @brief protobuf 线型。 */
enum class PbWireType : uint8_t {
    Varint = 0,           ///< 变长整数
    Fixed64 = 1,          ///< 8 字节小端
    LengthDelimited = 2,  ///< 长度前缀
    Fixed32 = 5,          ///< 4 字节小端
};

/** @brief 重复字段元素数上限（解码时强制）。 */
inline constexpr size_t kPbMaxRepeatedItems = 64;
/** @brief 单个字符串字段长度上限（解码时强制）。 */
inline constexpr size_t kPbMaxStringLen = 512;

/** @brief varint 编码后的字节数（1..10）。 */
[[nodiscard]] constexpr size_t pb_varint_size(uint64_t value) noexcept {
    size_t n = 1;
    while (value >= 0x80) {
        value >>= 7;
        ++n;
    }
    return n;
}

/**
 * @brief 向固定缓冲区写入 protobuf 字段。
 *
 * @note 所有 put_* 遵循 proto3 语义：零值/空值默认省略（这正是上游 prost 的
 *       行为）；需要显式表达 optional 存在性时使用 *_present 版本。
 * @warning 缓冲区不足时 ok() 变为 false；调用方必须在结束时检查 ok()，
 *          不能只依赖返回值。
 */
class ProtoWriter {
public:
    /**
     * @brief 在给定缓冲区上构造写入器。
     * @param out 输出缓冲区（不拥有，生命周期由调用方保证）
     */
    explicit ProtoWriter(MutBytes out) noexcept : out_(out) {}

    /** @brief 是否所有写入都成功。 */
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    /** @brief 已写入字节数。 */
    [[nodiscard]] size_t size() const noexcept { return len_; }
    /** @brief 已写入内容的视图。 */
    [[nodiscard]] Bytes written() const noexcept { return out_.first(len_); }

    bool put_uint32(uint32_t field, uint32_t value) noexcept;  ///< 写入 uint32（0 省略）
    bool put_int32(uint32_t field, int32_t value) noexcept;    ///< 写入 int32（0 省略，负数 10 字节）
    bool put_uint64(uint32_t field, uint64_t value) noexcept;  ///< 写入 uint64（0 省略）
    bool put_int64(uint32_t field, int64_t value) noexcept;    ///< 写入 int64（0 省略）
    bool put_bool(uint32_t field, bool value) noexcept;        ///< 写入 bool（false 省略）
    bool put_enum(uint32_t field, int32_t value) noexcept;     ///< 写入枚举（0 省略）
    bool put_bytes(uint32_t field, Bytes value) noexcept;      ///< 写入 bytes（空省略）
    bool put_string(uint32_t field, std::string_view value) noexcept;  ///< 写入 string（空省略）

    bool put_uint32_present(uint32_t field, uint32_t value) noexcept;  ///< 显式写入 uint32
    bool put_int32_present(uint32_t field, int32_t value) noexcept;    ///< 显式写入 int32
    bool put_enum_present(uint32_t field, int32_t value) noexcept;     ///< 显式写入枚举

    /** @brief 追加已编码好的字段（用于原样透传未知字段）。 */
    bool put_raw(Bytes raw) noexcept;

    /** @brief 写入字段 tag。 */
    bool put_tag(uint32_t field, PbWireType type) noexcept;
    /** @brief 写入一个 varint。 */
    bool put_varint(uint64_t value) noexcept;

private:
    MutBytes out_;      ///< 输出缓冲区
    size_t len_ = 0;    ///< 已写入长度
    bool ok_ = true;    ///< 写入状态
};

/** @brief 读取到的一个字段。 */
struct PbField {
    uint32_t field = 0;                    ///< 字段号
    PbWireType type = PbWireType::Varint;  ///< 线型
    uint64_t varint = 0;                   ///< varint 值，或 fixed 字段的原始小端值
    Bytes data{};                          ///< length-delimited 字段的负载
};

/**
 * @brief 顺序读取 protobuf 字段。
 *
 * @note 遇到非法输入时 next() 返回 false，并且 failed() 变为 true；
 *       failed() 为 false 表示正常读到末尾。
 */
class ProtoReader {
public:
    /** @brief 在给定输入上构造读取器。 */
    explicit ProtoReader(Bytes in) noexcept : in_(in) {}

    /**
     * @brief 读取下一个字段。
     * @param out 输出字段
     * @return true 读到字段；false 表示结束或出错（用 failed() 区分）
     */
    [[nodiscard]] bool next(PbField& out) noexcept;

    /** @brief 是否因为输入非法而失败。 */
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    /** @brief 读取一个 varint。 */
    bool read_varint(uint64_t& out) noexcept;

    Bytes in_;         ///< 输入
    size_t pos_ = 0;   ///< 当前偏移
    bool failed_ = false;  ///< 失败标记
};

/**
 * @brief 遍历 packed repeated varint 字段的元素。
 *
 * @details @p data 是 wire type 2 字段的负载，即**不带 tag** 的连续 varint 序列。
 * @tparam Fn 形如 void(uint64_t) 的回调
 * @param data packed 负载
 * @param max_items 允许的最大元素数
 * @param fn 对每个元素调用
 * @return false 表示负载非法或元素数超限
 */
template <typename Fn>
[[nodiscard]] bool for_each_packed_varint(Bytes data, size_t max_items, Fn&& fn) noexcept {
    size_t pos = 0;
    size_t count = 0;
    while (pos < data.size()) {
        uint64_t value = 0;
        bool done = false;
        for (int shift = 0; shift <= 63; shift += 7) {
            if (pos >= data.size()) {
                return false;
            }
            const uint8_t byte = data[pos++];
            value |= static_cast<uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                done = true;
                break;
            }
        }
        if (!done || ++count > max_items) {
            return false;
        }
        fn(value);
    }
    return true;
}

}  // namespace et
