/**
 * @file conn_slot.hpp
 * @brief 入口连接槽：管理 TCP 连接、入口轮换、重试退避与接收授权。
 */
#ifndef EASYTIER_ESP_TRANSPORT_CONN_SLOT_HPP_
#define EASYTIER_ESP_TRANSPORT_CONN_SLOT_HPP_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace et {

class TcpConn;

/**
 * @brief 连接槽与 TcpConn 共用的连接状态。
 * @details Idle → InProgress → Connected/Failed，close() 后回到 Idle。Connected
 *          只表示 TCP 已连通，能否收发 Data 由接收授权单独判断。
 */
enum class ConnectState : uint8_t {
    Idle,        ///< 未连接或已关闭
    InProgress,  ///< 正在建立连接
    Connected,   ///< TCP 已连通（不代表握手通过）
    Failed,      ///< 连接失败，等待重试
};

/// 首次重试等待时间（毫秒）。
inline constexpr uint32_t kInitialBackoffMs = 1000;
/// 重试等待上限（毫秒）。
inline constexpr uint32_t kMaxBackoffMs = 30000;

/**
 * @brief 一个入口连接槽：独占 TCP 收发缓冲，保存连接与重试状态。
 * @details 构造时分配 TcpConn 但不建立 socket，连接关闭后缓冲继续复用。连接状态、
 *          入口下标和退避时间只由 I/O 任务访问；代际与接收授权通过原子变量跨任务共享。
 */
class ConnSlot final {
public:
    /** @brief 构造槽并分配 TCP 对象，不建立 socket。 */
    ConnSlot() noexcept;

    /** @brief 析构：撤销接收授权并释放 TCP 对象；调用前 I/O 任务必须已停止。 */
    ~ConnSlot() noexcept;

    // 槽地址和共享状态必须稳定，禁止拷贝和移动。
    ConnSlot(const ConnSlot&) = delete;
    ConnSlot& operator=(const ConnSlot&) = delete;
    ConnSlot(ConnSlot&&) = delete;
    ConnSlot& operator=(ConnSlot&&) = delete;

    /** @brief 借用 TCP 对象做收发和 select；仅限 I/O 任务调用，所有权归本槽。 */
    [[nodiscard]] TcpConn& connection() noexcept { return *conn_; }

    /** @brief I/O 任务启动时复位：关闭旧连接、清空代际并设置入口和首次重试时间。 */
    void reset_io(size_t peer_index, uint32_t now) noexcept;

    /**
     * @brief 发起非阻塞连接。
     * @return true 表示已提交，等 poll_connect() 确认；false 表示槽忙或发起失败。
     * @note 发起前撤销旧会话的接收授权，新连接必须重新握手。
     */
    [[nodiscard]] bool begin_connect(std::string_view host, uint16_t port) noexcept;

    /** @brief 推进连接建立，成功后复位退避；非 InProgress 状态原样返回。 */
    ConnectState poll_connect() noexcept;

    /**
     * @brief 撤销接收授权、关闭 TCP，并把槽置为 Idle。
     * @return 关闭前的状态，供 I/O 任务判断是否上报断线。
     * @note 先撤销授权再关 socket，队列里的旧 Data 会立即失效；缓冲保留供重连。
     */
    ConnectState close() noexcept;

    /** @brief 当前连接状态；Connected 不代表握手通过。 */
    [[nodiscard]] ConnectState state() const noexcept { return state_; }

    /** @brief 当前入口在 Settings::peers 中的下标（会轮换，不固定）。 */
    [[nodiscard]] size_t peer_index() const noexcept { return peer_index_; }
    /** @brief 更新入口下标；去重和轮换由 Context 的多槽调度决定。 */
    void set_peer_index(size_t index) noexcept { peer_index_ = index; }

    /** @brief 是否已到重试时间；比较方式兼容 32 位时间戳回绕。 */
    [[nodiscard]] bool retry_ready(uint32_t now) const noexcept;

    /**
     * @brief 安排下一次重试，并把退避时间翻倍（最长不超过 kMaxBackoffMs）。
     * @return 本次等待的毫秒数；连续失败依次等待 1、2、4、8、16、30 秒。
     */
    uint32_t schedule_retry(uint32_t now) noexcept;

    /** @brief 只复位退避时长，不改已安排的重试时间。 */
    void reset_backoff() noexcept;

    /** @brief 复位退避并重新安排首次重试，用于应用重新允许连接。 */
    void reset_retry(uint32_t now) noexcept;

    /** @brief 最近一次 TCP 会话的代际，0 表示尚未分配；断线后仍保留，直到 reset_io()。 */
    [[nodiscard]] uint32_t generation() const noexcept;

    /** @brief 查询指定代际已授权的对端 peer id；代际不匹配或未握手时返回 0。 */
    [[nodiscard]] uint32_t peer_id(uint32_t expected_generation) const noexcept;

    /**
     * @brief 登记新会话：发布代际并清空身份，等待控制面完成握手授权。
     * @note 在投递 Connected 事件前调用，此时 Data 仍会被拒绝。
     */
    void prepare_receive(uint32_t new_generation) noexcept;

    /**
     * @brief 为等待握手的会话登记对端身份并放行 Data。
     * @return 成功返回 true；参数非法、代际失效或重复握手返回 false。
     * @details 用一次 CAS 同时写入身份和授权，迟到握手无法覆盖新会话。
     */
    [[nodiscard]] bool authenticate_receive(uint32_t expected_generation,
                                            uint32_t peer_id) noexcept;

    /**
     * @brief 撤销接收会话；expected_generation 为 0 时无条件撤销，非 0 时只撤销
     *        代际匹配的会话。
     * @note 控制面处理延迟事件时应传入事件里的代际，避免误清 I/O 刚建立的新会话。
     */
    void revoke_receive(uint32_t expected_generation = 0) noexcept;

    /** @brief 指定代际是否已授权接收 Data。 */
    [[nodiscard]] bool accepts_data(uint32_t expected_generation) const noexcept;

private:
    std::unique_ptr<TcpConn> conn_;            ///< TCP 对象，析构时关闭 socket、释放缓冲
    ConnectState state_ = ConnectState::Idle;  ///< 连接状态，仅 I/O 任务访问
    size_t peer_index_ = 0;                    ///< 当前入口下标，仅 I/O 任务访问
    uint32_t backoff_ms_ = kInitialBackoffMs;  ///< 下次失败后的重试间隔（毫秒）
    uint32_t retry_at_ms_ = 0;                 ///< 下次允许重试的时间（毫秒）
    std::atomic<uint32_t> generation_{0};      ///< 最近会话代际，I/O 写、其它任务读
    /**
     * @brief 接收会话：高 32 位为代际，低 32 位为已授权的 peer id。
     * @details 0 表示无会话，身份为 0 表示等待握手，身份非 0 表示已授权；身份和
     *          授权在同一个原子变量里更新，避免分别读写产生竞态。
     */
    std::atomic<uint64_t> receive_session_{0};
};

}  // namespace et

#endif
