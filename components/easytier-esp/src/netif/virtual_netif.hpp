/**
 * @file virtual_netif.hpp
 * @brief lwIP 虚拟网卡（raw L3，不模拟以太网/ARP）。
 *
 * @details 采用 lwIP 的 raw L3 网卡：
 * - 不设置 `NETIF_FLAG_ETHERNET`/`NETIF_FLAG_ARP`，因此入站/出站都是**完整 IP 报文**；
 * - 入站用 `tcpip_input`，把解密后的报文交给 TCPIP 线程（不能在其他任务里直接调
 *   `ip_input`，否则 TCP 控制块会被无锁访问）；
 * - 网卡的注册、地址变更、up/down 与删除都通过 `netifapi_*`（线程安全）；
 * - `output` 回调运行在 TCPIP 线程，只做 `pbuf_ref + 零等待入队`，加密与查表都在
 *   数据面任务里完成；
 * - 不把该网卡设为 default，物理网卡继续承载入口连接与普通上网。
 */
#pragma once

#include <atomic>
#include <cstdint>

#include "easytier-esp.hpp"

#if defined(ESP_PLATFORM)
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#endif

namespace et {

#if defined(ESP_PLATFORM)
/** @brief 数据面接收队列的一项（pbuf 已 pbuf_ref）。 */
struct NetifRxItem {
    struct pbuf* p = nullptr;  ///< 待处理 pbuf（数据面负责释放）
    uint32_t generation = 0;   ///< 入队时的连接代际
};

/** @brief 虚拟网卡上下文；不占用 IDF 保留的 netif->state。 */
class VirtualNetif {
public:
    /**
     * @brief 创建并注册虚拟网卡。
     * @param ip 虚拟 IPv4（数值表示）
     * @param prefix 前缀长度
     * @param mtu 虚拟 MTU
     * @param rx_queue 数据面接收队列（由实例创建）
     * @param generation 代际指针（用于标记队列项）
     * @return @c ESP_OK 或错误码
     */
    esp_err_t create(uint32_t ip, uint8_t prefix, uint16_t mtu, void* rx_queue,
                     const std::atomic<uint32_t>* generation) noexcept;

    /** @brief 更新地址（dhcp 换址时调用）。 */
    esp_err_t set_address(uint32_t ip, uint8_t prefix) noexcept;

    /** @brief 链路 up/down（应用 disconnect() 或最后一个可用 peer 丢失时）。 */
    esp_err_t set_link(bool up) noexcept;

    /** @brief 移除网卡并释放状态。 */
    void destroy() noexcept;

    /** @brief 创建参数中的 MTU，供 TCPIP 初始化回调读取。 */
    [[nodiscard]] uint16_t configured_mtu() const noexcept { return mtu_; }

    /** @brief 从本类拥有的首成员 netif 找回所属对象，仅供本网卡回调使用。 */
    [[nodiscard]] static VirtualNetif* from_netif(struct netif* ni) noexcept;

    /** @brief 底层 netif（供数据面投递使用）。 */
    [[nodiscard]] struct netif* netif() noexcept { return &netif_; }

    /**
     * @brief 出站回调使用的接收队列句柄。
     * @note C 链接的回调无法访问私有成员，因此提供访问器。
     */
    [[nodiscard]] void* rx_queue() const noexcept { return rx_queue_; }
    /** @brief 出站回调入队时使用的当前连接代际。 */
    [[nodiscard]] uint32_t generation() const noexcept {
        return generation_ != nullptr ? generation_->load() : 0;
    }

private:
    // 必须保持首成员；from_netif 用标准布局类首成员的指针互转规则找回对象。
    struct netif netif_{};        ///< lwIP 网卡结构，state 始终为空
    uint16_t mtu_ = kDefaultMtu;  ///< create() 设置，TCPIP 初始化回调读取后写入 netif
    void* rx_queue_ = nullptr;    ///< 数据面接收队列
    const std::atomic<uint32_t>* generation_ = nullptr;  ///< 代际指针（属于 Context）
    bool created_ = false;        ///< 是否已注册
};

#endif

}  // namespace et
