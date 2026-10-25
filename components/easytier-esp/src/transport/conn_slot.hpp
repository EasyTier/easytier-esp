/**
 * @file conn_slot.hpp
 * @brief 单个入口连接槽：连接状态机、入口轮换信息、重试退避与接收授权。
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace et {

class TcpConn;

/**
 * @brief TCP 与连接槽共用的连接状态。
 */
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
 * @details 构造时分配 TCP 对象，析构时自动关闭并释放；连接关闭后保留缓冲供重连复用。
 *          连接状态和重试信息仅由 I/O 任务访问；跨任务共享的代际、身份通过原子接口访问。
 *          构造不打开 socket。内存不足时与 std::string/vector 一样终止进程。
 */
class ConnSlot final {
public:
    // ---- 资源生命周期：大块 TCP 缓冲留在堆上，槽对象自身只保存指针与少量状态。 ----
    /** @brief 构造即分配 TCP 缓冲；在 .cpp 定义以保持 TcpConn 的前置声明。 */
    ConnSlot() noexcept;
    /** @brief 撤销接收授权，随后由智能指针析构 TCP 对象、关闭 socket 并释放缓冲。 */
    ~ConnSlot() noexcept;

    ConnSlot(const ConnSlot&) = delete;
    ConnSlot& operator=(const ConnSlot&) = delete;
    ConnSlot(ConnSlot&&) = delete;
    ConnSlot& operator=(ConnSlot&&) = delete;

    /**
     * @brief 借用始终存在的 TCP 对象，供 I/O 任务收发及 select 使用。
     * @note 所有权仍属于本槽，调用方不得 delete；建连和关闭应走本槽的方法。
     */
    [[nodiscard]] TcpConn& connection() noexcept { return *conn_; }

    // ---- 单槽连接状态：是否需要投递连接/断线事件，由 I/O 任务根据转换结果决定。 ----
    /** @brief 每次 I/O 任务启动时关闭旧会话、清零共享身份并初始化入口与首次重试时间。 */
    void reset_io(size_t peer_index, uint32_t now) noexcept;
    /** @brief 发起连接；成功提交后进入 InProgress，由 poll_connect 确认 TCP 建立。 */
    [[nodiscard]] bool begin_connect(std::string_view host, uint16_t port) noexcept;
    /** @brief 推进 InProgress 到 Connected/Failed；成功时复位退避，其它状态保持不变。 */
    ConnectState poll_connect() noexcept;
    /**
     * @brief 撤权、关闭 TCP 并进入 Idle，返回关闭前的状态以决定是否上报断线。
     * @note 清除接收身份，保留最近的 generation 供断线事件使用；不释放 TCP 缓冲。
     */
    ConnectState close() noexcept;
    /** @brief 槽状态仅由 I/O 任务读写，Connected 不表示握手已通过。 */
    [[nodiscard]] ConnectState state() const noexcept { return state_; }

    // ---- 入口与退避：只记录下标和截止时间，不在槽内睡眠等待。 ----
    /** @brief 当前配置入口下标，并非固定绑定的服务器；仅由 I/O 任务使用。 */
    [[nodiscard]] size_t peer_index() const noexcept { return peer_index_; }
    /** @brief 设置入口下标；入口去重与轮换顺序由 Context 的多槽调度决定。 */
    void set_peer_index(size_t index) noexcept { peer_index_ = index; }
    /** @brief 当前是否已到达重试截止时间（支持 32 位毫秒时间戳回绕）。 */
    [[nodiscard]] bool retry_ready(uint32_t now) const noexcept;
    /**
     * @brief 安排重试并指数退避至上限，返回本次实际等待的毫秒数。
     * @details 连续失败时依次等待 1、2、4、8、16、30 秒，此后保持 30 秒。
     *          只更新截止时间，等待期间 I/O 任务仍可服务其它槽。
     */
    uint32_t schedule_retry(uint32_t now) noexcept;
    /** @brief 仅复位下次退避长度，不改变已经安排的重试截止时间。 */
    void reset_backoff() noexcept;
    /** @brief 复位退避并安排初始等待，用于应用重新允许连接。 */
    void reset_retry(uint32_t now) noexcept;

    // ---- 跨任务共享会话：身份与授权一起原子更新，不暴露可单独修改的字段。 ----
    /**
     * @brief 最近一次 TCP 会话的代际；0 表示尚未分配，断线后保留，reset_io 时清零。
     * @note 单独读取代际不能判断是否有接收权限，请使用 accepts_data。
     */
    [[nodiscard]] uint32_t generation() const noexcept;
    /** @brief 读取指定代际已授权的对端身份；代际不匹配、未握手或已撤权时返回 0。 */
    [[nodiscard]] uint32_t peer_id(uint32_t expected_generation) const noexcept;

    /** @brief 登记新代际并清零 peer_id；握手通过前禁止接收 Data。仅由 I/O 任务调用。 */
    void prepare_receive(uint32_t new_generation) noexcept;
    /**
     * @brief 为仍在等待握手的指定代际登记对端身份，并放行 Data。
     * @return 连接已失效、已换代或重复握手时返回 false；旧握手不能授权新连接。
     */
    [[nodiscard]] bool authenticate_receive(uint32_t expected_generation,
                                            uint32_t peer_id) noexcept;
    /**
     * @brief 清除接收会话（身份及授权）；代际为 0 时无条件撤销，否则只撤销指定会话。
     * @note 控制面处理旧事件时指定代际，避免撤销 I/O 已建立的新会话。
     */
    void revoke_receive(uint32_t expected_generation = 0) noexcept;
    /** @brief 指定代际是否仍为本槽已通过握手的接收会话。 */
    [[nodiscard]] bool accepts_data(uint32_t expected_generation) const noexcept;

private:
    std::unique_ptr<TcpConn> conn_;                   ///< 独占 TCP 对象，自动管理其堆内存
    ConnectState state_ = ConnectState::Idle;         ///< 槽的连接阶段，仅由 I/O 任务读写
    size_t peer_index_ = 0;                           ///< 当前配置入口下标
    uint32_t backoff_ms_ = kInitialBackoffMs;          ///< 下次重试的等待长度
    uint32_t retry_at_ms_ = 0;                        ///< 独立重试截止时间
    std::atomic<uint32_t> generation_{0};             ///< 最近一次代际，仅 I/O 写，撤权后保留
    /**
     * @brief 原子接收会话：高 32 位为代际，低 32 位为对端身份。
     * @details 0 表示已撤销；非零代际 + 身份 0 表示等待握手；身份非零即已授权。
     *          授权用一次 CAS 同时提交身份，撤权直接清零，迟到握手无法重新授权。
     *          这是原子一致性保证；ESP32-S3 上的 64 位原子由 ESP-IDF 临界区实现。
     */
    std::atomic<uint64_t> receive_session_{0};
};

}  // namespace et
