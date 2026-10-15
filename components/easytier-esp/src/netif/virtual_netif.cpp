/**
 * @file virtual_netif.cpp
 * @brief lwIP raw L3 虚拟网卡实现（ESP-IDF 6.1 / lwIP 2.x）。
 */
#include "netif/virtual_netif.hpp"

#if defined(ESP_PLATFORM)

#include <cstring>
#include <cstddef>
#include <type_traits>

#include "config/config.hpp"
#include "lwip/ip4_addr.h"
#include "lwip/netifapi.h"
#include "lwip/tcpip.h"
#include "support/log.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.NETIF";

/**
 * @brief 出站回调（运行在 TCPIP 线程）。
 * @details 只做 `pbuf_ref` + 零等待入队：查路由与加密都在数据面任务里完成，绝不在
 *          TCPIP 线程里做重活。队列满时归还我们增加的引用并返回 ERR_MEM，由 lwIP
 *          按丢包处理（绝不阻塞 TCPIP 线程）。
 */
extern "C" err_t et_netif_output(struct netif* ni, struct pbuf* p,
                                 const ip4_addr_t* /*ipaddr*/) {
    auto* self = VirtualNetif::from_netif(ni);
    if (self == nullptr || p == nullptr || self->rx_queue() == nullptr) {
        return ERR_ARG;
    }
    NetifRxItem item{};
    item.p = p;
    item.generation = self->generation();
    pbuf_ref(p);  // 队列持有额外引用；output 返回后 lwIP 会释放它自己的那一份
    if (xQueueSend(static_cast<QueueHandle_t>(self->rx_queue()), &item, 0) != pdTRUE) {
        pbuf_free(p);  // 归还我们增加的引用
        return ERR_MEM;
    }
    return ERR_OK;
}

/**
 * @brief 网卡初始化回调（netifapi 会在 TCPIP 线程里调用它）。
 */
extern "C" err_t et_netif_init(struct netif* ni) {
    ni->name[0] = 'e';
    ni->name[1] = 't';
    ni->output = et_netif_output;
    ni->mtu = VirtualNetif::from_netif(ni)->configured_mtu();
    ni->flags = 0;  // 不设置 ETHERNET/ARP：raw L3
    return ERR_OK;
}

/** @brief 把数值 IPv4 转成 lwIP 的 ip4_addr_t（网络字节序）。 */
[[nodiscard]] ip4_addr_t to_lwip(uint32_t addr) noexcept {
    ip4_addr_t out{};
    // ip4_addr_t 内部按网络字节序保存，用 ip4_addr_set_u32 显式转换。
    ip4_addr_set_u32(&out, lwip_htonl(addr));
    return out;
}

}  // namespace

/** @brief 判断虚拟网段与物理网段是否重叠（复用配置模块的工具函数）。 */
bool virtual_netif_overlaps(uint32_t virt_ip, uint8_t virt_prefix, uint32_t phys_ip,
                            uint8_t phys_prefix) noexcept {
    return ipv4_prefixes_overlap(virt_ip, virt_prefix, phys_ip, phys_prefix);
}

/** @brief 标准布局类与其首成员可互转，不借用 esp-netif 的 state 存储。 */
VirtualNetif* VirtualNetif::from_netif(struct netif* ni) noexcept {
    static_assert(std::is_standard_layout_v<VirtualNetif>);
    static_assert(offsetof(VirtualNetif, netif_) == 0);
    return reinterpret_cast<VirtualNetif*>(ni);
}

/** @brief 创建并注册虚拟网卡。 */
esp_err_t VirtualNetif::create(uint32_t ip, uint8_t prefix, uint16_t mtu, void* rx_queue,
                               const std::atomic<uint32_t>* generation) noexcept {
    if (created_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (prefix > 32 || mtu < kMinMtu || mtu > kDefaultMtu) return ESP_ERR_INVALID_ARG;
    mtu_ = mtu;
    rx_queue_ = rx_queue;
    generation_ = generation;

    const ip4_addr_t addr = to_lwip(ip);
    // /0 的前缀没有意义：掩码按 prefix 计算。
    const uint32_t mask_value = (prefix == 0) ? 0 : (~0u << (32 - prefix));
    const ip4_addr_t mask = to_lwip(mask_value);
    const ip4_addr_t gw = to_lwip(ip);  // 点对点语义，网关用自身地址

    // IDF 6.1 默认将 state 解释为 esp_netif_t*。raw 网卡不属于 esp-netif，
    // 必须传 nullptr，让其全局地址变化回调跳过本网卡；不能传 VirtualNetif*。
    std::memset(&netif_, 0, sizeof(netif_));
    const err_t err = netifapi_netif_add(&netif_, &addr, &mask, &gw, nullptr, et_netif_init,
                                         tcpip_input);
    if (err != ERR_OK) {
        ET_LOGE(kTag, "netifapi_netif_add failed: %d", static_cast<int>(err));
        return ESP_FAIL;
    }
    const err_t up_err = netifapi_netif_set_up(&netif_);
    if (up_err != ERR_OK) {
        ET_LOGE(kTag, "netifapi_netif_set_up failed: %d", static_cast<int>(up_err));
        (void)netifapi_netif_remove(&netif_);
        rx_queue_ = nullptr;
        generation_ = nullptr;
        return ESP_FAIL;
    }
    created_ = true;
    ET_LOGI(kTag, "virtual netif up: ip=%08x prefix=%u mtu=%u", static_cast<unsigned>(ip),
            static_cast<unsigned>(prefix), static_cast<unsigned>(mtu));
    return ESP_OK;
}

/** @brief 更新虚拟地址。 */
esp_err_t VirtualNetif::set_address(uint32_t ip, uint8_t prefix) noexcept {
    if (!created_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (prefix > 32) return ESP_ERR_INVALID_ARG;
    const ip4_addr_t addr = to_lwip(ip);
    const uint32_t mask_value = (prefix == 0) ? 0 : (~0u << (32 - prefix));
    const ip4_addr_t mask = to_lwip(mask_value);
    const ip4_addr_t gw = to_lwip(ip);
    const err_t err = netifapi_netif_set_addr(&netif_, &addr, &mask, &gw);
    if (err != ERR_OK) {
        return ESP_FAIL;
    }
    ET_LOGI(kTag, "virtual netif address updated: ip=%08x prefix=%u", static_cast<unsigned>(ip),
            static_cast<unsigned>(prefix));
    return ESP_OK;
}

/** @brief 链路 up/down。 */
esp_err_t VirtualNetif::set_link(bool up) noexcept {
    if (!created_) {
        return ESP_ERR_INVALID_STATE;
    }
    const err_t err =
        up ? netifapi_netif_set_link_up(&netif_) : netifapi_netif_set_link_down(&netif_);
    return err == ERR_OK ? ESP_OK : ESP_FAIL;
}

/** @brief 移除网卡。 */
void VirtualNetif::destroy() noexcept {
    if (!created_) {
        return;
    }
    (void)netifapi_netif_set_link_down(&netif_);
    (void)netifapi_netif_set_down(&netif_);
    (void)netifapi_netif_remove(&netif_);
    created_ = false;
    rx_queue_ = nullptr;
    generation_ = nullptr;
}

}  // namespace et

#endif  // ESP_PLATFORM
