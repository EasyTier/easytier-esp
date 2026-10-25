/**
 * @file conn_slot.hpp
 * @brief 单个入口连接槽：连接状态机、入口轮换信息、重试退避与接收授权。
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace et {

class TcpConn;

/** @brief TCP 与连接槽共用的连接状态。 */
enum class ConnectState : uint8_t {
    Idle,        ///< 尚未发起连接或已关闭
    InProgress,  ///< 连接建立中，等待 poll_connect 确认结果
    Connected,   ///< TCP 已建立（是否通过握手由接收授权单独判断）
    Failed,      ///< 建立失败，等待退避重试
};

/// I/O 任务重连退避起点（毫秒）。
inline constexpr uint32_t kInitialBackoffMs = 1000;
/// I/O 任务重连退避上限（毫秒）。
inline constexpr uint32_t kMaxBackoffMs = 30000;

/**
 * @brief 一个入口连接槽，独占堆上的 TCP 收发缓冲及该槽的重连状态。
 * @details 除 peer_id、generation 与接收授权方法外，其余操作仅由 I/O 任务执行；
 *          allocate/release 在任务启动前/退出后由 Context 调用。
 *          TCP 建立与握手授权分开管理，旧代际不能授权或撤销新会话。
 */
class ConnSlot final {
public:
    ConnSlot() noexcept = default;
    ~ConnSlot() noexcept;

    ConnSlot(const ConnSlot&) = delete;
    ConnSlot& operator=(const ConnSlot&) = delete;
    ConnSlot(ConnSlot&&) = delete;
    ConnSlot& operator=(ConnSlot&&) = delete;

    /** @brief 分配 TCP 对象；已分配时复用，内存不足返回 false。 */
    [[nodiscard]] bool allocate() noexcept;
    /** @brief 撤权并释放 TCP 对象，清零共享身份与 I/O 状态；可重复调用。 */
    void release() noexcept;
    /** @brief 获取仅供 I/O 任务收发及 select 使用的 TCP 对象。 */
    [[nodiscard]] TcpConn* connection() const noexcept { return conn_; }

    /** @brief 每次 I/O 任务启动时关闭旧会话并初始化入口与首次重试时间。 */
    void reset_io(size_t peer_index, uint32_t now) noexcept;
    /** @brief 发起连接；成功提交后进入 InProgress，由 poll_connect 确认 TCP 建立。 */
    [[nodiscard]] bool begin_connect(std::string_view host, uint16_t port) noexcept;
    /** @brief 推进 InProgress 到 Connected/Failed；其它状态保持不变。 */
    ConnectState poll_connect() noexcept;
    /** @brief 撤权、关闭 TCP 并进入 Idle，返回关闭前的状态以决定是否上报断线。 */
    ConnectState close() noexcept;
    /** @brief 槽状态仅由 I/O 任务读写，Connected 不表示握手已通过。 */
    [[nodiscard]] ConnectState state() const noexcept { return state_; }

    /** @brief 当前配置入口下标，仅由 I/O 任务使用。 */
    [[nodiscard]] size_t peer_index() const noexcept { return peer_index_; }
    /** @brief 设置入口下标；入口去重与轮换顺序由 Context 的多槽调度决定。 */
    void set_peer_index(size_t index) noexcept { peer_index_ = index; }
    /** @brief 当前是否已到达重试截止时间（支持 32 位毫秒时间戳回绕）。 */
    [[nodiscard]] bool retry_ready(uint32_t now) const noexcept;
    /** @brief 安排重试并指数退避至上限，返回本次实际等待的毫秒数。 */
    uint32_t schedule_retry(uint32_t now) noexcept;
    /** @brief 仅复位下次退避长度，不改变已经安排的重试截止时间。 */
    void reset_backoff() noexcept;
    /** @brief 复位退避并安排初始等待，用于应用重新允许连接。 */
    void reset_retry(uint32_t now) noexcept;

    /** @brief 握手后的对端 peer id（0 = 未握手/未知），供控制面与 I/O 共享。 */
    std::atomic<uint32_t> peer_id{0};
    /** @brief 当前连接代际（0 = 空闲），由 I/O 写入，控制面与数据面读取。 */
    std::atomic<uint32_t> generation{0};

    /** @brief 建立新接收会话；握手通过前禁止接收 Data。仅由 I/O 任务调用。 */
    void prepare_receive(uint32_t new_generation) noexcept;
    /**
     * @brief 为仍在等待握手的指定代际放行 Data。
     * @return 连接已失效、已换代或重复握手时返回 false；旧握手不能授权新连接。
     */
    [[nodiscard]] bool authenticate_receive(uint32_t expected_generation) noexcept;
    /**
     * @brief 撤销接收权限；代际为 0 时无条件撤销，否则只撤销指定会话。
     * @note 控制面处理旧事件时指定代际，避免撤销 I/O 已建立的新会话。
     */
    void revoke_receive(uint32_t expected_generation = 0) noexcept;
    /** @brief 指定代际是否仍为本槽已通过握手的接收会话。 */
    [[nodiscard]] bool accepts_data(uint32_t expected_generation) const noexcept;

private:
    TcpConn* conn_ = nullptr;                         ///< 独占 TCP 对象（含大块收发缓冲）
    ConnectState state_ = ConnectState::Idle;         ///< 替代 connecting/session_open
    size_t peer_index_ = 0;                           ///< 当前配置入口下标
    uint32_t backoff_ms_ = kInitialBackoffMs;          ///< 下次重试的等待长度
    uint32_t retry_at_ms_ = 0;                        ///< 独立重试截止时间
    /// 高位保存完整 32 位代际，最低位表示握手通过；原子更新避免换代与授权交错。
    std::atomic<uint64_t> receive_session_{0};
};

}  // namespace et
