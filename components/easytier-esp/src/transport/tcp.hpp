/**
 * @file tcp.hpp
 * @brief 非阻塞 TCP 传输：连接建立、按帧重组、带游标的部分写。
 *
 * @details
 * - 一个 TcpConn 只被 I/O 任务读写（socket 所有权的唯一持有者）；
 * - 收到的字节先进入 rx 缓冲，再按 4 字节小端长度前缀切分完整帧；
 * - 发送侧使用固定数量的槽位：一帧占一个槽位，写不完时保留写游标，保证不会
 *   出现"半帧插入另一帧"破坏分帧的情况；
 * - 帧 body 长度必须落在 [16, 2000]，否则视为致命错误（与上游 FramedReader 一致）。
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "protocol/wire.hpp"
#include "support/bytes.hpp"
#include "support/net.hpp"

namespace et {

/** @brief 连接建立状态。 */
enum class ConnectState : uint8_t {
    Idle,        ///< 尚未发起连接
    InProgress,  ///< 非阻塞连接进行中
    Connected,   ///< 已建立
    Failed,      ///< 建立失败
};

/**
 * @brief 单条 TCP 隧道连接。
 *
 * @note 非线程安全：所有方法都必须在 I/O 任务上下文中调用。
 */
class TcpConn {
public:
    /** @brief 每帧完整数据的回调（body = PeerManagerHeader + payload）。 */
    using FrameCallback = void (*)(void* ctx, Bytes body);

    TcpConn() noexcept = default;
    ~TcpConn() noexcept;

    TcpConn(const TcpConn&) = delete;
    TcpConn& operator=(const TcpConn&) = delete;

    /**
     * @brief 发起非阻塞连接。
     * @param host 主机名或点分 IPv4
     * @param port 端口
     * @return false 表示解析失败或 socket 创建失败（连接失败由 poll_connect 报告）
     */
    [[nodiscard]] bool begin_connect(std::string_view host, uint16_t port) noexcept;

    /**
     * @brief 推进连接建立。
     * @return 当前状态；Failed 表示连接失败（调用方应关闭并退避重连）
     */
    ConnectState poll_connect() noexcept;

    /** @brief 关闭连接并复位收发状态。 */
    void close() noexcept;

    /** @brief 是否已连接。 */
    [[nodiscard]] bool connected() const noexcept { return state_ == ConnectState::Connected; }
    /** @brief 底层句柄（用于 select）。 */
    [[nodiscard]] net::Socket fd() const noexcept { return fd_; }

    /**
     * @brief 读取并切分完整帧。
     * @param ctx 传给回调的上下文
     * @param cb 每收到一个完整帧调用一次
     * @param frames_out 本次处理的完整帧数
     * @return false 表示连接已关闭或分帧非法（调用方应关闭连接）
     */
    bool read_available(void* ctx, FrameCallback cb, size_t& frames_out) noexcept;

    /**
     * @brief 入队一帧待发送数据（自动补 4 字节长度前缀）。
     * @param body PM 头 + payload（含 AEAD 尾）
     * @return false 表示槽位已满（控制面应重试或丢弃，数据面可直接丢弃）
     */
    [[nodiscard]] bool enqueue(Bytes body) noexcept;

    /**
     * @brief 尽可能发送待发数据。
     * @param budget_bytes 本轮最多写入的字节数（公平调度用）
     * @return false 表示写失败（调用方应关闭连接）
     */
    bool flush(size_t budget_bytes) noexcept;

    /** @brief 是否还有待发送数据。 */
    [[nodiscard]] bool has_pending_tx() const noexcept { return tx_count_ != 0; }
    /** @brief 待发送字节数（含长度前缀）。 */
    [[nodiscard]] size_t pending_tx_bytes() const noexcept;

    /** @brief 发送侧是否已满（用于背压判断）。 */
    [[nodiscard]] bool tx_full() const noexcept { return tx_count_ == kTxSlots; }

    /** @brief 是否有已接收但未切分的字节。 */
    [[nodiscard]] size_t rx_buffered() const noexcept { return rx_len_; }

private:
    /** @brief 一个发送槽位（一帧）。 */
    struct TxSlot {
        size_t len = 0;  ///< 本帧长度（含 4 字节前缀）
        size_t off = 0;  ///< 已写出的字节数
        std::array<uint8_t, kTcpTunnelHeaderSize + kMaxTcpBodyLen> data{};
    };

    /** @brief 接收缓冲区：至少容纳 2 个最大帧，保证一次 read 后能切出多帧。 */
    static constexpr size_t kRxCapacity = (kTcpTunnelHeaderSize + kMaxTcpBodyLen) * 2;
    /** @brief 发送槽位数量（控制面优先策略在更高层实现）。 */
    static constexpr size_t kTxSlots = 4;
    /** @brief 单次 read_available() 最多处理的帧数，保证多连接间的公平性。 */
    static constexpr size_t kMaxFramesPerRound = 8;

    net::Socket fd_ = net::kInvalidSocket;                    ///< 套接字
    ConnectState state_ = ConnectState::Idle;                 ///< 连接状态
    std::array<uint8_t, kRxCapacity> rx_{};                   ///< 接收缓冲
    size_t rx_len_ = 0;                                       ///< 已缓冲字节数
    std::array<TxSlot, kTxSlots> tx_{};                       ///< 发送槽位（环形）
    size_t tx_head_ = 0;                                      ///< 队首槽位下标
    size_t tx_count_ = 0;                                     ///< 待发帧数
};

}  // namespace et
