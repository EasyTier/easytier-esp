/**
 * @file dhcp.cpp
 * @brief 虚拟 IPv4 自分配实现（对齐上游 DhcpIpv4Allocator）。
 */
#include "route/dhcp.hpp"

#include "config/config.hpp"
#include "support/log.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.DHCP";

/**
 * @brief 子网的广播地址（上游 `last_address()`）。
 * @note 上游用 `first_address()`（网络号）到 `last_address()`（广播地址）之间的
 *       偏移量作为候选，偏移从 1 开始，因此天然跳过网络号与广播地址。
 */
[[nodiscard]] constexpr uint32_t subnet_broadcast(uint32_t subnet, uint8_t prefix) noexcept {
    if (prefix == 0) {
        return 0xFFFFFFFFu;
    }
    if (prefix >= 32) {
        return subnet;
    }
    return subnet | (~0u >> prefix);
}

}  // namespace

/** @brief 追加已用地址（去重）。 */
void Ipv4UseSet::add(uint32_t addr, uint8_t prefix) noexcept {
    if (addr == 0 || contains(addr, prefix) || count >= items.size()) {
        return;
    }
    items[count].addr = addr;
    items[count].prefix = prefix;
    ++count;
}

/** @brief 精确匹配（地址与前缀都相同才算占用）。 */
bool Ipv4UseSet::contains(uint32_t addr, uint8_t prefix) const noexcept {
    for (size_t i = 0; i < count; ++i) {
        if (items[i].addr == addr && items[i].prefix == prefix) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 配置默认子网。
 * @details 与上游一致：只要启用了 dhcp，就**总是**存在一个默认子网（配置值或内置的
 *          10.126.126.0/24），它只是"最后手段"——先等两轮，看有没有节点通告子网。
 */
void DhcpAllocator::configure(bool has_subnet, uint32_t subnet, uint8_t prefix) noexcept {
    has_default_subnet_ = true;
    default_subnet_ = has_subnet ? subnet : kDefaultDhcpSubnet;
    default_prefix_ = has_subnet ? prefix : kDefaultDhcpPrefix;
}

/** @brief 复位当前地址与计数。 */
void DhcpAllocator::reset() noexcept {
    current_valid_ = false;
    current_addr_ = 0;
    current_prefix_ = default_prefix_;
    no_ipv4_rounds_ = 0;
}

/** @brief 提交决策结果。 */
void DhcpAllocator::commit(const DhcpResult& result) noexcept {
    current_valid_ = result.has_address;
    current_addr_ = result.address;
    current_prefix_ = result.prefix;
}

/**
 * @brief 计算自分配决策。
 * @details 与上游逐步对应：选子网 → 判断是否需要换址 → 线性探测候选地址。
 */
DhcpResult DhcpAllocator::evaluate(bool has_routes, const Ipv4UseSet& used,
                                   uint32_t random_hint) noexcept {
    DhcpResult result{};

    // 理论上不会发生（启用 dhcp 时默认子网总是存在）；保留判断以对齐上游结构。
    if (!has_routes && !has_default_subnet_) {
        result.decision = DhcpDecision::WaitForPeers;
        return result;
    }

    // 1) 选择子网：优先沿用当前子网内的通告，其次取通告子网中最靠前的那个。
    const Ipv4Use* chosen = nullptr;
    for (size_t i = 0; i < used.count; ++i) {
        const Ipv4Use& item = used.items[i];
        if (!current_valid_) {
            continue;
        }
        if (ipv4_network_of(current_addr_, current_prefix_) !=
            ipv4_network_of(item.addr, item.prefix)) {
            continue;
        }
        if (chosen == nullptr || item.addr < chosen->addr ||
            (item.addr == chosen->addr && item.prefix < chosen->prefix)) {
            chosen = &item;
        }
    }
    if (chosen == nullptr) {
        for (size_t i = 0; i < used.count; ++i) {
            const Ipv4Use& item = used.items[i];
            if (chosen == nullptr || item.addr < chosen->addr ||
                (item.addr == chosen->addr && item.prefix < chosen->prefix)) {
                chosen = &item;
            }
        }
    }

    uint32_t subnet = 0;
    uint8_t prefix = default_prefix_;
    if (chosen != nullptr) {
        no_ipv4_rounds_ = 0;
        subnet = ipv4_network_of(chosen->addr, chosen->prefix);
        prefix = chosen->prefix;
    } else if (current_valid_) {
        result.decision = DhcpDecision::Unchanged;
        result.has_address = true;
        result.address = current_addr_;
        result.prefix = current_prefix_;
        return result;
    } else {
        if (!has_default_subnet_) {
            result.decision = DhcpDecision::WaitForPeers;
            return result;
        }
        // 先等几轮，让已持地址的节点有机会先通告子网。
        if (++no_ipv4_rounds_ <= kBootstrappedWaitRounds) {
            result.decision = DhcpDecision::WaitForPeers;
            return result;
        }
        subnet = default_subnet_;
        prefix = default_prefix_;
    }

    // 2) 当前地址仍在本子网且未被占用：保持不变。
    if (current_valid_ && ipv4_network_of(current_addr_, current_prefix_) == subnet &&
        !used.contains(current_addr_, current_prefix_)) {
        result.decision = DhcpDecision::Unchanged;
        result.has_address = true;
        result.address = current_addr_;
        result.prefix = current_prefix_;
        return result;
    }

    // 3) 线性探测：候选 = 网络号 + offset（offset ∈ [1, span)），
    //    首次分配或已有地址时使用随机起点，避免多个节点同时自举选中同一地址。
    const uint32_t first = subnet;
    const uint32_t last = subnet_broadcast(subnet, prefix);
    const uint32_t span = last - first;  // 可用偏移范围 [1, span)
    if (span <= 1) {
        ET_LOGW(kTag, "dhcp subnet has no usable host address");
        result.decision = DhcpDecision::WaitForPeers;
        return result;
    }
    uint32_t start = 1;
    if (used.empty() || (current_valid_ && random_hint != 0)) {
        start = 1 + (random_hint % (span - 1));
    }
    uint32_t next_addr = 0;
    for (uint32_t probe = 0; probe + 1 < span; ++probe) {
        const uint32_t offset = ((start - 1 + probe) % (span - 1)) + 1;
        const uint32_t candidate = first + offset;
        if (!used.contains(candidate, prefix)) {
            next_addr = candidate;
            break;
        }
    }
    if (next_addr == 0) {
        ET_LOGW(kTag, "dhcp subnet is full");
        result.decision = DhcpDecision::WaitForPeers;
        return result;
    }
    if (current_valid_ && current_addr_ == next_addr && current_prefix_ == prefix) {
        result.decision = DhcpDecision::Unchanged;
        result.has_address = true;
        result.address = current_addr_;
        result.prefix = current_prefix_;
        return result;
    }
    result.decision = DhcpDecision::Change;
    result.has_address = true;
    result.address = next_addr;
    result.prefix = prefix;
    return result;
}

}  // namespace et
