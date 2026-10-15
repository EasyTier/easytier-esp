/**
 * @file messages.hpp
 * @brief 本组件用到的 EasyTier protobuf 消息的类型化编解码。
 *
 * @details 字段号与上游 .proto 严格一致（peer_rpc.proto / common.proto），并
 * 用 Google protobuf 运行期生成的黄金样本做回归验证。
 *
 * @warning 解码得到的视图（string_view / Bytes）指向调用方的输入缓冲区，
 *          在该视图使用期间，输入缓冲区必须保持有效且内容不变。
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "protocol/protobuf.hpp"
#include "protocol/wire.hpp"

namespace et {

/**
 * @brief 实例 UUID。
 *
 * @note 上游把它编码为**四个 uint32 字段**，不是 16 字节 bytes；两者不能互换。
 */
struct Uuid {
    uint32_t part1 = 0;  ///< 第 1 段
    uint32_t part2 = 0;  ///< 第 2 段
    uint32_t part3 = 0;  ///< 第 3 段
    uint32_t part4 = 0;  ///< 第 4 段
};

/** @brief 编码 UUID，返回写入字节数（0 表示缓冲区不足）。 */
size_t encode_uuid(const Uuid& id, MutBytes out) noexcept;
/** @brief 解码 UUID；字段缺失按 0 处理。 */
bool decode_uuid(Bytes in, Uuid& out) noexcept;

/**
 * @brief 握手请求（peer_rpc.HandshakeRequest）。
 *
 * @note 本 profile 不发送 features，但必须能解析对端发来的 features 字符串，
 *       因此 @ref feature_count 记录解析到的条数。
 */
struct HandshakeRequestView {
    uint32_t magic = 0;                        ///< 固定 kHandshakeMagic，接收端不校验
    uint32_t my_peer_id = 0;                   ///< 发送方 peer id（接收端以此为准）
    uint32_t version = 0;                      ///< 协议版本，当前为 1
    std::string_view network_name{};           ///< 网络名
    Bytes network_secret_digest{};             ///< 32 字节网络密钥摘要
    size_t feature_count = 0;                  ///< 解析到的 features 条数（内容被跳过）
};

/** @brief 解码握手请求；字段类型不符或超长返回 false。 */
bool decode_handshake_request(Bytes in, HandshakeRequestView& out) noexcept;
/** @brief 编码握手请求，返回写入字节数（0 表示缓冲区不足）。 */
size_t encode_handshake_request(const HandshakeRequestView& req, MutBytes out) noexcept;

/** @brief RPC 描述符（common.RpcDescriptor）。 */
struct RpcDescriptorRef {
    std::string_view domain_name{};   ///< 域，等于 network_name
    std::string_view proto_name{};    ///< proto 名，如 "OspfRouteRpc"
    std::string_view service_name{};  ///< 服务名，如 "OspfRouteRpc"
    uint32_t method_index = 0;        ///< 方法序号，从 1 开始
};

/** @brief RPC 压缩协商信息（common.RpcCompressionInfo）。 */
struct RpcCompressionRef {
    int32_t algo = 0;           ///< 本包使用的压缩算法：0 Invalid / 1 None / 2 Zstd
    int32_t accepted_algo = 0;  ///< 期望对端后续使用的算法
};

/**
 * @brief RPC 报文（common.RpcPacket）。
 *
 * @note 分片字段：新版本单包为 total_pieces=1、piece_idx=0；历史单包为 0/0。
 *       片 0 提供权威的 descriptor 与 compression 信息，后续分片可以不带。
 */
struct RpcPacketRef {
    uint32_t from_peer = 0;            ///< 发起方 peer id
    uint32_t to_peer = 0;              ///< 目标 peer id
    int64_t transaction_id = 0;        ///< 事务 id（u64 随机数按位解释为 int64）
    bool has_descriptor = false;       ///< 是否携带 descriptor
    RpcDescriptorRef descriptor{};     ///< 描述符（片 0 权威）
    Bytes body{};                      ///< RpcRequest 或 RpcResponse 序列化字节
    bool is_request = false;           ///< true=请求，false=响应
    uint32_t total_pieces = 0;         ///< 分片总数
    uint32_t piece_idx = 0;            ///< 当前分片序号
    int32_t trace_id = 0;              ///< 追踪 id，可为 0
    bool has_compression = false;      ///< 是否携带压缩信息
    RpcCompressionRef compression{};   ///< 压缩信息（片 0 权威）
};

/** @brief 解码 RPC 报文。 */
bool decode_rpc_packet(Bytes in, RpcPacketRef& out) noexcept;
/** @brief 编码 RPC 报文，返回写入字节数（0 表示缓冲区不足）。 */
size_t encode_rpc_packet(const RpcPacketRef& packet, MutBytes out) noexcept;

/** @brief RPC 请求体（common.RpcRequest）。 */
struct RpcRequestRef {
    Bytes request{};        ///< 服务入参的序列化字节
    int32_t timeout_ms = 0;  ///< 期望超时
};

/** @brief 解码 RpcRequest。 */
bool decode_rpc_request(Bytes in, RpcRequestRef& out) noexcept;
/** @brief 编码 RpcRequest。 */
size_t encode_rpc_request(Bytes request, int32_t timeout_ms, MutBytes out) noexcept;

/**
 * @brief RPC 响应体（common.RpcResponse）。
 *
 * @note @ref error_raw 是 error.Error 的原始字节：本组件不解析错误枚举，只做
 *       原样透传，避免因上游新增错误类型而误解码。
 */
struct RpcResponseRef {
    Bytes response{};      ///< 服务返回值的序列化字节
    bool has_error = false;  ///< 是否携带 error 字段（与 error=0 有存在性区别）
    Bytes error_raw{};     ///< error.Error 原始字节
    uint64_t runtime_us = 0;  ///< 服务端执行耗时
};

/** @brief 解码 RpcResponse。 */
bool decode_rpc_response(Bytes in, RpcResponseRef& out) noexcept;
/**
 * @brief 编码 RpcResponse。
 * @return 写入字节数；0 表示缓冲区不足**或编码结果本身就是空消息**，需要区分时
 *         请使用 encode_rpc_response_checked()。
 */
size_t encode_rpc_response(Bytes response, Bytes error_raw, uint64_t runtime_us,
                           MutBytes out) noexcept;

/**
 * @brief 编码 RpcResponse，并明确区分"空消息"与"编码失败"。
 * @param out_len 输出实际写入长度（可以为 0，表示合法的空 RpcResponse）
 * @return false 表示缓冲区不足
 */
bool encode_rpc_response_checked(Bytes response, Bytes error_raw, uint64_t runtime_us,
                                 MutBytes out, size_t& out_len) noexcept;

/**
 * @brief 编码 `error.Error{execute_error{error_message}}`。
 * @details RPC 失败时用它填充 RpcResponse.error；本组件只负责生成这一种错误，
 *          收到的错误仍然原样透传。
 * @return 写入字节数；0 表示缓冲区不足
 */
size_t encode_error_execute(std::string_view message, MutBytes out) noexcept;

/** @brief 压缩算法取值：Invalid。 */
inline constexpr int32_t kCompressionInvalid = 0;
/** @brief 压缩算法取值：None（本 profile 发送与接受的值）。 */
inline constexpr int32_t kCompressionNone = 1;
/** @brief 压缩算法取值：Zstd（本 profile 不支持）。 */
inline constexpr int32_t kCompressionZstd = 2;

}  // namespace et
