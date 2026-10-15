/**
 * @file rpc.cpp
 * @brief RPC 注册表与事务表实现。
 */
#include "rpc/rpc.hpp"

#include "support/log.hpp"
#include "support/random.hpp"
#include "support/time.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.RPC";

/** @brief 判断服务标识是否匹配描述符。 */
[[nodiscard]] bool matches(const RpcServiceId& id, const RpcDescriptorRef& desc) noexcept {
    return id.method_index == desc.method_index && id.domain_name == desc.domain_name &&
           id.proto_name == desc.proto_name && id.service_name == desc.service_name;
}

}  // namespace

/** @brief 注册服务方法（描述符字符串由调用方保证生命周期覆盖整个实例）。 */
bool RpcRegistry::add(const RpcServiceId& id, RpcHandler handler, void* ctx) noexcept {
    if (handler == nullptr || id.service_name.empty() || id.method_index == 0) {
        return false;
    }
    if (count_ >= kMaxRpcServices) {
        ET_LOGE(kTag, "rpc registry full");
        return false;
    }
    entries_[count_].id = id;
    entries_[count_].handler = handler;
    entries_[count_].ctx = ctx;
    ++count_;
    return true;
}

/** @brief 查找并调用服务。 */
esp_err_t RpcRegistry::dispatch(const RpcDescriptorRef& descriptor, Bytes request,
                                MutBytes response, size_t& response_len) noexcept {
    response_len = 0;
    for (size_t i = 0; i < count_; ++i) {
        if (matches(entries_[i].id, descriptor)) {
            return entries_[i].handler(entries_[i].ctx, request, response, response_len);
        }
    }
    return ESP_ERR_NOT_FOUND;
}

/** @brief 分配事务 id（随机非零，避免与旧事务冲突）。 */
int64_t RpcTransactions::allocate(uint32_t generation, uint32_t to_peer, uint32_t now_ms,
                                  uint32_t timeout_ms) noexcept {
    for (Entry& entry : entries_) {
        if (entry.used) {
            continue;
        }
        int64_t id = static_cast<int64_t>(random_u64());
        if (id == 0) {
            id = 1;
        }
        entry.used = true;
        entry.transaction_id = id;
        entry.generation = generation;
        entry.to_peer = to_peer;
        entry.deadline_ms = now_ms + timeout_ms;
        return id;
    }
    ET_LOGW(kTag, "rpc transaction table full");
    return 0;
}

/** @brief 结束一个事务（要求代际一致，避免旧连接的响应误配）。 */
bool RpcTransactions::complete(int64_t transaction_id, uint32_t generation) noexcept {
    for (Entry& entry : entries_) {
        if (entry.used && entry.transaction_id == transaction_id) {
            if (entry.generation != generation) {
                return false;  // 旧代际的迟到响应
            }
            entry.used = false;
            return true;
        }
    }
    return false;
}

/** @brief 回收超时事务。 */
void RpcTransactions::expire(uint32_t now_ms) noexcept {
    for (Entry& entry : entries_) {
        if (entry.used && deadline_reached(now_ms, entry.deadline_ms)) {
            ET_LOGW(kTag, "rpc transaction timed out");
            entry.used = false;
        }
    }
}

/** @brief 清空事务表。 */
void RpcTransactions::reset() noexcept {
    for (Entry& entry : entries_) {
        entry.used = false;
    }
}

}  // namespace et
