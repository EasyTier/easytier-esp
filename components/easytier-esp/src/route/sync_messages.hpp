/**
 * @file sync_messages.hpp
 * @brief OSPF 路由同步请求/响应的完整编解码。
 *
 * @details
 * - 请求：`my_peer_id(1)`、`my_session_id(2)`、`is_initiator(3)`、
 *   `peer_infos(4)`、`conn_info` oneof { `conn_bitmap(5)` / `conn_peer_list(7)` }；
 * - 响应：`is_initiator(1)`、`session_id(2)`、`error(3, optional)`、
 *   `missing_peer_ids(4)`、`missing_conn_peer_ids(5)`；
 * - `error` 是 proto3 optional：必须区分"字段缺省"与"存在且为 0"
 *   （0 = DuplicatePeerId，是真实错误）；
 * - repeated 数字字段按上游（prost）默认使用 packed 编码，解码时两种都接受。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "protocol/protobuf.hpp"
#include "route/route_info.hpp"

namespace et {

/** @brief OSPF 服务标识常量（domain 需要等于 network_name，由调用方填充）。 */
inline constexpr std::string_view kOspfProtoName = "OspfRouteRpc";
inline constexpr std::string_view kOspfServiceName = "OspfRouteRpc";
inline constexpr uint32_t kOspfSyncMethodIndex = 1;

/** @brief 同步请求的公共头部。 */
struct SyncRequestHeader {
    uint32_t my_peer_id = 0;      ///< tag 1
    uint64_t my_session_id = 0;   ///< tag 2
    bool is_initiator = false;    ///< tag 3
};

/**
 * @brief 编码同步请求头。
 * @details 字段 1 与 2 使用显式写入，因此取值全为 0 时也不会产生 0 字节输出，
 *          这样"编码失败(0)"与"空消息"可以区分。
 * @return 写入字节数；0 表示缓冲区不足
 */
[[nodiscard]] size_t encode_sync_request_header(const SyncRequestHeader& header,
                                                MutBytes out) noexcept;

/** @brief 解码同步请求头（其余字段跳过）。 */
[[nodiscard]] bool decode_sync_request_header(Bytes in, SyncRequestHeader& out) noexcept;

/** @brief 解析后的同步请求：各字段以视图给出，指向输入缓冲区。 */
struct SyncRequestView {
    SyncRequestHeader header{};        ///< 公共头部
    bool has_peer_infos = false;       ///< tag 4 是否存在
    Bytes peer_infos{};                ///< 已编码的 RoutePeerInfos
    bool has_conn_bitmap = false;      ///< tag 5（oneof conn_info）
    Bytes conn_bitmap{};               ///< 已编码的 RouteConnBitmap
    bool has_conn_list = false;        ///< tag 7（oneof conn_info）
    Bytes conn_list{};                 ///< 已编码的 RouteConnPeerList
};

/** @brief 解码完整同步请求。 */
[[nodiscard]] bool decode_sync_request(Bytes in, SyncRequestView& out) noexcept;

/** @brief 待发送的同步请求（各子消息需已编码）。 */
struct SyncRequestFields {
    SyncRequestHeader header{};  ///< 公共头部
    Bytes peer_infos{};          ///< RoutePeerInfos 编码结果（可为空）
    Bytes conn_bitmap{};         ///< RouteConnBitmap 编码结果（与 conn_list 二选一）
    Bytes conn_list{};           ///< RouteConnPeerList 编码结果
};

/** @brief 编码完整同步请求。 */
[[nodiscard]] size_t encode_sync_request(const SyncRequestFields& fields, MutBytes out) noexcept;

/** @brief 解码后的同步响应。 */
struct SyncResponseView {
    bool is_initiator = false;   ///< tag 1
    uint64_t session_id = 0;     ///< tag 2
    bool has_error = false;      ///< tag 3 是否存在（存在且为 0 表示 DuplicatePeerId）
    int32_t error_code = 0;      ///< tag 3 的取值
    std::array<uint32_t, kPbMaxRepeatedItems> missing_peer_ids{};  ///< tag 4
    size_t missing_peer_count = 0;                                 ///< tag 4 元素个数
    std::array<uint32_t, kPbMaxRepeatedItems> missing_conn_peer_ids{};  ///< tag 5
    size_t missing_conn_peer_count = 0;                                 ///< tag 5 元素个数
};

/** @brief 解码同步响应。 */
[[nodiscard]] bool decode_sync_response(Bytes in, SyncResponseView& out) noexcept;

/** @brief 编码同步响应（missing 列表按 packed 写出）。 */
[[nodiscard]] size_t encode_sync_response(const SyncResponseView& view, MutBytes out) noexcept;

}  // namespace et
