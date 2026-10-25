/**
 * @file rpc.hpp
 * @brief RPC 服务注册表、事务表与本地分发。
 *
 * @details
 * - **服务注册表**：把 (domain, proto, service, method_index) 映射到处理函数；
 *   当前只注册 `network_name` 下的 `OspfRouteRpc.SyncRouteInfo`（method_index=1）；
 * - **事务表**：为发出的请求分配事务 id，按 (事务 id, 代际) 关联响应，超时回收；
 * - **分发**：目标为本机的请求才进入本地服务；非本机请求由调用方按 overlay 路由
 *   转发，绝不当成本地调用处理。
 */
#ifndef EASYTIER_ESP_RPC_RPC_HPP_
#define EASYTIER_ESP_RPC_RPC_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "esp_err.h"
#include "protocol/messages.hpp"
#include "support/config_limits.hpp"

namespace et {

/** @brief 服务标识（与上游 RpcDescriptor 的四个字段一一对应）。 */
struct RpcServiceId {
    std::string_view domain_name{};   ///< 域，等于 network_name
    std::string_view proto_name{};    ///< proto 名
    std::string_view service_name{};  ///< 服务名
    uint32_t method_index = 0;        ///< 方法序号
};

/**
 * @brief 服务处理函数。
 * @param ctx 注册时传入的上下文
 * @param request 已序列化的服务入参
 * @param response 输出缓冲区
 * @param response_len 输出实际写入长度
 * @return @c ESP_OK 表示成功；其它值表示服务返回错误
 */
using RpcHandler = esp_err_t (*)(void* ctx, Bytes request, MutBytes response,
                                 size_t& response_len) noexcept;

/**
 * @brief RPC 服务注册表。
 * @note 容量固定为 kMaxRpcServices，注册发生在 start() 阶段，运行期只读。
 */
class RpcRegistry {
public:
    /** @brief 最多支持的服务数量。 */
    static constexpr size_t kMaxRpcServices = 8;

    /**
     * @brief 注册一个服务方法。
     * @return false 表示容量已满或参数非法
     */
    bool add(const RpcServiceId& id, RpcHandler handler, void* ctx) noexcept;

    /**
     * @brief 按描述符查找并调用服务。
     * @return @c ESP_OK 成功；@c ESP_ERR_NOT_FOUND 未注册；其它为服务错误
     */
    esp_err_t dispatch(const RpcDescriptorRef& descriptor, Bytes request, MutBytes response,
                       size_t& response_len) noexcept;

    /** @brief 已注册数量。 */
    [[nodiscard]] size_t size() const noexcept { return count_; }

private:
    /** @brief 一个注册项。 */
    struct Entry {
        RpcServiceId id{};
        RpcHandler handler = nullptr;
        void* ctx = nullptr;
    };

    std::array<Entry, kMaxRpcServices> entries_{};
    size_t count_ = 0;
};

/**
 * @brief RPC 事务表。
 *
 * @note 事务 id 使用随机 u64（按位解释为 int64），因此无需全局单调计数器，也
 *       不会因为重启而与旧事务冲突；代际用于丢弃重连前发出的响应。
 */
class RpcTransactions {
public:
    /** @brief 并发事务上限。 */
    static constexpr size_t kMaxPending = kRpcMaxTransactions;

    /**
     * @brief 登记一个待响应事务。
     * @param generation 当前连接代际
     * @param now_ms 当前时间
     * @param timeout_ms 超时时间
     * @return 事务 id；0 表示表已满（0 不是合法事务 id）
     */
    [[nodiscard]] int64_t allocate(uint32_t generation, uint32_t now_ms,
                                   uint32_t timeout_ms) noexcept;

    /**
     * @brief 匹配并结束一个事务。
     * @return true 表示存在且代际一致（调用方可以处理响应）
     */
    bool complete(int64_t transaction_id, uint32_t generation) noexcept;

    /** @brief 回收超时事务。 */
    void expire(uint32_t now_ms) noexcept;

private:
    /** @brief 一个挂起事务。 */
    struct Entry {
        bool used = false;
        int64_t transaction_id = 0;
        uint32_t generation = 0;
        uint32_t deadline_ms = 0;
    };

    std::array<Entry, kMaxPending> entries_{};
};

}  // namespace et

#endif
