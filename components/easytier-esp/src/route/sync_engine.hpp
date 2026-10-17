/**
 * @file sync_engine.hpp
 * @brief 路由同步引擎：拓扑合并、自身信息构造、请求/响应编解码与路由重算。
 *
 * @details 把原本散落在控制面任务里的"纯协议逻辑"集中到这里，好处有两个：
 * 1. 可以在宿主测试里让**多个引擎互相对话**，验证多节点（含中继）收敛，而这些
 *    逻辑在设备上只能靠真机验证；
 * 2. 控制面任务只剩下 I/O 编排（收发、定时、事件），职责更清晰。
 *
 * @note 本类不依赖 FreeRTOS/lwIP：所有方法都是确定性纯逻辑，所有缓冲都是成员数组。
 *       它**不是**线程安全的，只应由控制面任务（或宿主测试）单线程访问。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include "easytier-esp.h"
#include "protocol/messages.hpp"
#include "route/dhcp.hpp"
#include "route/route_info.hpp"
#include "route/sync_messages.hpp"
#include "route/table.hpp"
#include "route/topology.hpp"
#include "support/config_limits.hpp"

namespace et {

/**
 * @brief 同时保持的直连对端上限。
 * @details 取 Kconfig 的 `EASYTIER_ESP_MAX_CONNECTED_PEERS`（默认 6），必须不小于
 *          连接槽数量，否则超出部分的对端会被静默忽略。数值来自
 *          `support/config_limits.hpp`（宿主构建同样有默认值），因此本头文件与引擎
 *          仍可在宿主测试里独立编译。
 */
inline constexpr size_t kMaxActivePeers = kMaxConnectedPeers;

/**
 * @brief 路由同步引擎。
 *
 * @details 典型用法（控制面任务）：
 * @code
 * engine.configure(config, my_peer_id);
 * engine.on_peer_up(remote_peer_id, session_id, now);   // 握手成功
 * engine.set_direct_peers(&remote_peer_id, 1, now);
 * size_t len = engine.build_request(buf, true);         // 主动宣告自身
 * engine.handle_request(req, resp, resp_len, now);       // 应答对端
 * engine.handle_response(resp, need_resend);             // 处理 missing
 * if (engine.recompute_if_dirty()) { ...发布快照... }
 * @endcode
 */
class SyncEngine {
public:
    /** @brief 我方请求缓冲的建议大小。 */
    static constexpr size_t kMaxRequestBytes = 1792;
    /** @brief 请求里 peer_infos 字段允许占用的最大字节数（有界，超出则减少条目）。 */
    static constexpr size_t kMaxPeerInfosBytes = 1000;
    /** @brief 请求里 conn_list 字段允许占用的最大字节数。 */
    static constexpr size_t kMaxConnListBytes = 700;
    /**
     * @brief 配置引擎（复制设置中需要的字段）。
     * @param settings 已校验的实例设置
     * @param my_peer_id 本节点 peer id
     */
    void configure(const Settings& settings, uint32_t my_peer_id) noexcept;

    /** @brief 复位为初始状态（由 configure 调用）。 */
    void reset() noexcept;

    /** @brief 是否启用 dhcp 自分配。 */
    [[nodiscard]] bool dhcp_enabled() const noexcept { return config_.dhcp; }

    /**
     * @brief 登记一次握手成功（可并发多个直连对端）。
     * @param remote_peer_id 对端 peer id
     * @param session_id 本节点本次组网会话 id（非 0）
     * @param now 当前时间（毫秒）
     */
    void on_peer_up(uint32_t remote_peer_id, uint64_t session_id, uint32_t now) noexcept;

    /**
     * @brief 登记某个直连对端断开。
     * @details 还有其它直连时保留拓扑（它们仍能提供可达性），全部断开才清空拓扑并
     *          立即撤销所有路由。
     */
    void on_peer_down(uint32_t remote_peer_id) noexcept;

    /**
     * @brief 设置当前直连对端集合。
     * @details 自身邻接行会列出这些节点；单连接时只有一个元素，多连接时会有多个。**本地拓扑也会写入这一行**，否则本节点在图上没有任何出边，
     *          无法把流量交给直连对端（上游的 conn_map 同样包含自身行）。
     * @param now 当前时间（毫秒）
     */
    void set_direct_peers(const uint32_t* peers, size_t count, uint32_t now) noexcept;


    /** @brief 是否至少有一个已握手对端。 */
    [[nodiscard]] bool peer_up() const noexcept { return active_peer_count_ != 0; }
    /**
     * @brief 查询某个直连对端的会话 id（0 表示未知）。
     * @details 会话 id 是**每个连接各自一份**的（上游 `my_session_id` 属于 PeerRoute
     *          实例）。早期实现在引擎里只保存一个值，第二个入口握手时会覆盖它，导致
     *          第一个对端看到"我们的会话 id 变了"而清空已保存的信息。
     */
    [[nodiscard]] uint64_t session_id_of(uint32_t peer_id) const noexcept;

    /**
     * @brief 按拓扑中通告的地址做 dhcp 自分配（未启用 dhcp 时直接返回 false）。
     * @return true 表示地址发生了变化（调用方需要更新网卡并重新通告）
     */
    bool update_dhcp(uint32_t now, uint32_t random_hint) noexcept;

    /** @brief 当前虚拟 IPv4（静态或 dhcp 结果；0 表示未知）。 */
    [[nodiscard]] uint32_t my_ipv4() const noexcept { return my_ipv4_; }
    /** @brief 当前虚拟前缀长度。 */
    [[nodiscard]] uint8_t my_prefix() const noexcept { return my_prefix_; }

    /**
     * @brief 构造我方的同步请求。
     * @param out 输出缓冲（需 >= kMaxRequestBytes）
     * @param include_topology 是否携带自身 RoutePeerInfo 与邻接行
     * @param target_peer 目标对端（用于取该连接的会话 id；0 表示取第一个直连对端）
     * @return 写入长度；0 表示失败（缓冲不足或未握手）
     */
    [[nodiscard]] size_t build_request(MutBytes out, bool include_topology,
                                       uint32_t target_peer = 0) noexcept;

    /**
     * @brief 处理对端发来的完整同步请求。
     * @param request 已重组的 SyncRouteInfoRequest 字节
     * @param response 输出缓冲（调用方提供，通常为 4 KiB 的 rpc_service_out）
     * @param response_len 输出长度
     * @param now 当前时间（毫秒）
     */
    esp_err_t handle_request(Bytes request, MutBytes response, size_t& response_len,
                             uint32_t now) noexcept;

    /**
     * @brief 处理对端发来的完整同步响应。
     * @param response 已重组的 SyncRouteInfoResponse 字节
     * @param need_resend 输出：对端缺少我们的信息，需要立即补发
     */
    esp_err_t handle_response(Bytes response, bool& need_resend) noexcept;

    /** @brief 拓扑有变化时重算路由。 */
    bool recompute_if_dirty() noexcept;

    /** @brief 当前路由快照。 */
    [[nodiscard]] const RouteSnapshot& snapshot() const noexcept { return table_.snapshot(); }
    /** @brief 拓扑（只读，测试与诊断用）。 */
    [[nodiscard]] const Topology& topology() const noexcept { return topology_; }

    /** @brief 清理过期拓扑条目（控制面每轮调用；见 Topology::expire）。 */
    void expire_topology(uint32_t now) noexcept;

private:
    /** @brief 构造/刷新本节点 RoutePeerInfo 的编码。 */
    bool build_self_info(uint32_t now) noexcept;
    /** @brief 收集拓扑里的已用地址。 */
    void collect_used_addresses() noexcept;
    /** @brief 合并请求里的 peer_infos 与 conn_info，并收集缺失条目。 */
    bool merge_request(const SyncRequestView& request, SyncResponseView& response,
                       uint32_t now) noexcept;
    /** @brief 把缺失的 peer id 加入列表（去重、有界）。 */
    static void add_missing(std::array<uint32_t, kPbMaxRepeatedItems>& list, size_t& count,
                            uint32_t peer_id) noexcept;

    /**
     * @brief 引擎需要的配置子集。
     * @details 刻意只保存少数字段、**不含入口地址列表**：入口列表按实际条数动态分配，
     *          引擎没有必要持有它，这样既避免拷贝也缩小 `Context` 常驻内存。
     */
    struct EngineConfig {
        std::array<uint8_t, 16> instance_id{};            ///< 实例 UUID
        std::string hostname{};                            ///< 主机名（已规范化）
        bool has_static_ipv4 = false;                      ///< 是否配置了静态虚拟 IPv4
        uint32_t static_ipv4 = 0;                          ///< 静态虚拟 IPv4
        uint8_t static_prefix = 24;                        ///< 静态前缀长度
        bool dhcp = false;                                 ///< 是否启用虚拟地址自分配
    };

    EngineConfig config_{};              ///< 配置子集（只用到其中少数字段）
    uint32_t my_peer_id_ = 0;            ///< 本节点 peer id
    /** @brief 一个已握手直连对端（会话 id 每连接一份）。 */
    struct ActivePeer {
        uint32_t peer_id = kInvalidPeerId;  ///< 对端 peer id
        uint64_t session_id = 0;            ///< 该连接的会话 id（0 = 未知）
    };

    std::array<ActivePeer, kMaxActivePeers> active_peers_{};  ///< 已握手直连对端集合
    size_t active_peer_count_ = 0;  ///< 已握手直连对端数量
    uint64_t default_session_id_ = 0;  ///< 第一个直连对端的会话 id（兼容无目标的取用）
    uint32_t self_info_version_ = 1;     ///< 自身信息版本
    uint64_t self_peer_route_id_ = 0;    ///< 自身路由 id（整个生命周期固定）
    uint32_t conn_row_version_ = 1;      ///< 自身邻接行版本
    uint32_t my_ipv4_ = 0;               ///< 当前虚拟地址
    uint8_t my_prefix_ = 24;             ///< 当前前缀

    std::array<uint32_t, kMaxRoutePeers> direct_peers_{};  ///< 直连对端集合
    size_t direct_peer_count_ = 0;                         ///< 直连数量

    Topology topology_{};             ///< 拓扑
    RouteTable table_{};              ///< 路由表
    DhcpAllocator dhcp_{};            ///< dhcp 自分配
    Ipv4UseSet dhcp_used_{};          ///< 已用地址

    std::array<uint8_t, kMaxRawRoutePeerInfoBytes> self_peer_info_{};  ///< 自身信息编码
    uint16_t self_peer_info_len_ = 0;                                 ///< 自身信息长度
    std::array<uint8_t, kMaxConnListBytes> conn_list_{};      ///< 邻接表编码缓冲
    std::array<uint8_t, kMaxPeerInfosBytes> peer_infos_{};    ///< RoutePeerInfos 编码缓冲
    /// 构造请求用的邻接表（约 4.6 KiB：32 行 × 32 邻接）。放在成员里而不是栈上，
    /// 否则控制面任务栈会被单个函数吃掉（见 tools/check_stack_usage.py）。
    ConnList conn_list_build_{};
    /// 处理请求时解析出的邻接表（同样避免占用任务栈）。
    ConnList conn_list_recv_{};
};

}  // namespace et
