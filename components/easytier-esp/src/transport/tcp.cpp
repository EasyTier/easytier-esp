/**
 * @file tcp.cpp
 * @brief 非阻塞 TCP 传输实现。
 */
#include "transport/tcp.hpp"

#include <cstdio>
#include <cstring>

#include "support/log.hpp"

namespace et {
namespace {

constexpr const char* kTag = "ET.TCP";

/** @brief 把 string_view 复制到定长缓冲并 NUL 结尾。 */
[[nodiscard]] bool copy_cstr(std::string_view src, char* dst, size_t cap) noexcept {
    if (src.empty() || src.size() >= cap) {
        return false;
    }
    std::memcpy(dst, src.data(), src.size());
    dst[src.size()] = '\0';
    return true;
}

}  // namespace

TcpConn::~TcpConn() noexcept { close(); }

/**
 * @brief 发起非阻塞连接。
 * @details 用 getaddrinfo 解析主机名（支持点分 IPv4 与 DNS 名称），只取第一个
 *          结果；连接立即失败则返回 false，仍在进行中则返回 true 并由
 *          poll_connect() 继续推进。
 */
bool TcpConn::begin_connect(std::string_view host, uint16_t port) noexcept {
    close();
    char host_buf[256];
    char port_buf[8];
    if (!copy_cstr(host, host_buf, sizeof(host_buf))) {
        return false;
    }
    std::snprintf(port_buf, sizeof(port_buf), "%u", static_cast<unsigned>(port));

    addrinfo hints{};
    hints.ai_family = AF_INET;  // 首版仅 IPv4
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host_buf, port_buf, &hints, &res) != 0 || res == nullptr) {
        ET_LOGW(kTag, "cannot resolve %s", host_buf);
        return false;
    }
    const net::Socket fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return false;
    }
    if (!net::set_nonblocking(fd)) {
        net::close_socket(fd);
        ::freeaddrinfo(res);
        return false;
    }
    net::set_nodelay(fd);
    const int rc = ::connect(fd, res->ai_addr, static_cast<socklen_t>(res->ai_addrlen));
    const int err = (rc == 0) ? 0 : net::last_error();
    ::freeaddrinfo(res);

    if (rc == 0) {
        state_ = ConnectState::Connected;
    } else if (net::in_progress(err)) {
        state_ = ConnectState::InProgress;
    } else {
        net::close_socket(fd);
        return false;
    }
    fd_ = fd;
    return true;
}

/** @brief 用 select + SO_ERROR 推进非阻塞连接。 */
ConnectState TcpConn::poll_connect() noexcept {
    if (state_ != ConnectState::InProgress) {
        return state_;
    }
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd_, &wfds);
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    const int rc = ::select(fd_ + 1, nullptr, &wfds, nullptr, &tv);
    if (rc <= 0) {
        return state_;  // 尚未完成或被打断，等待下一轮
    }
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
        close();
        state_ = ConnectState::Failed;
        return state_;
    }
    state_ = ConnectState::Connected;
    return state_;
}

/** @brief 关闭并复位收发状态（rx 中未处理的字节与完整帧会被丢弃，调用方需先行处理）。 */
void TcpConn::close() noexcept {
    net::close_socket(fd_);
    fd_ = net::kInvalidSocket;
    state_ = ConnectState::Idle;
    rx_len_ = 0;
    tx_head_ = 0;
    tx_count_ = 0;
    for (TxSlot& slot : tx_) {
        slot.len = 0;
        slot.off = 0;
    }
}

/**
 * @brief 读取并切分完整帧。
 * @details 处理流程：先把缓冲中已有的数据切成完整帧，再 recv 新数据；每轮最多
 *          处理 max_frames 帧，避免单个活跃连接饿死其它连接。分帧非法（长度不在
 *          [16,2000]）或对端关闭时返回 false。
 */
bool TcpConn::read_available(void* ctx, FrameCallback cb, size_t& frames_out) noexcept {
    frames_out = 0;
    if (state_ != ConnectState::Connected) {
        return false;
    }
    for (;;) {
        // 1) 尽可能切出完整帧。
        while (rx_len_ >= kTcpTunnelHeaderSize) {
            uint32_t body_len = 0;
            (void)decode_tcp_tunnel_header(Bytes(rx_.data(), kTcpTunnelHeaderSize), body_len);
            if (body_len < kPeerManagerHeaderSize || body_len > kMaxTcpBodyLen) {
                ET_LOGW(kTag, "invalid frame length %u", static_cast<unsigned>(body_len));
                return false;
            }
            const size_t frame_len = kTcpTunnelHeaderSize + body_len;
            if (rx_len_ < frame_len) {
                break;  // 等待后续字节
            }
            if (cb != nullptr) {
                cb(ctx, Bytes(rx_.data() + kTcpTunnelHeaderSize, body_len));
            }
            const size_t rest = rx_len_ - frame_len;
            if (rest != 0) {
                std::memmove(rx_.data(), rx_.data() + frame_len, rest);
            }
            rx_len_ = rest;
            if (++frames_out >= kMaxFramesPerRound) {
                return true;  // 本轮预算用尽，剩余字节下轮继续切分
            }
        }

        // 2) 缓冲已满却没有切出帧：长度前缀必然非法（已在上面拦截），此处兜底。
        if (rx_len_ == rx_.size()) {
            ET_LOGW(kTag, "rx buffer full without a complete frame");
            return false;
        }

        const ssize_t got = ::recv(fd_, rx_.data() + rx_len_, rx_.size() - rx_len_, 0);
        if (got > 0) {
            rx_len_ += static_cast<size_t>(got);
            continue;
        }
        if (got == 0) {
            return false;  // 对端关闭
        }
        const int err = net::last_error();
        if (net::interrupted(err)) {
            continue;
        }
        if (net::would_block(err)) {
            return true;  // 暂时无数据
        }
        ET_LOGW(kTag, "recv failed: %d", err);
        return false;
    }
}

/** @brief 入队一帧（自动补长度前缀）；槽位满时返回 false。 */
bool TcpConn::enqueue(Bytes body) noexcept {
    if (state_ != ConnectState::Connected) {
        return false;
    }
    if (body.size() < kPeerManagerHeaderSize || body.size() > kMaxTcpBodyLen) {
        return false;
    }
    if (tx_count_ >= kTxSlots) {
        return false;
    }
    TxSlot& slot = tx_[(tx_head_ + tx_count_) % kTxSlots];
    encode_tcp_tunnel_header(MutBytes(slot.data.data(), kTcpTunnelHeaderSize),
                             static_cast<uint32_t>(body.size()));
    std::memcpy(slot.data.data() + kTcpTunnelHeaderSize, body.data(), body.size());
    slot.len = kTcpTunnelHeaderSize + body.size();
    slot.off = 0;
    ++tx_count_;
    return true;
}

/**
 * @brief 发送待发数据。
 * @details 按槽位顺序发送；一个槽位写完后才推进到下一帧，绝不把两帧的内容
 *          交错写入，避免破坏对端的分帧。EAGAIN 直接返回（下轮继续）。
 */
bool TcpConn::flush(size_t budget_bytes) noexcept {
    if (state_ != ConnectState::Connected) {
        return false;
    }
    size_t written = 0;
    while (tx_count_ != 0 && written < budget_bytes) {
        TxSlot& slot = tx_[tx_head_];
        const size_t remain = slot.len - slot.off;
        const ssize_t sent = ::send(fd_, slot.data.data() + slot.off, remain, 0);
        if (sent > 0) {
            slot.off += static_cast<size_t>(sent);
            written += static_cast<size_t>(sent);
            if (slot.off >= slot.len) {
                slot.len = 0;
                slot.off = 0;
                tx_head_ = (tx_head_ + 1) % kTxSlots;
                --tx_count_;
            }
            continue;
        }
        if (sent < 0) {
            const int err = net::last_error();
            if (net::interrupted(err)) {
                continue;
            }
            if (net::would_block(err)) {
                return true;  // 发送缓冲满，等待可写事件
            }
            ET_LOGW(kTag, "send failed: %d", err);
        }
        return false;  // sent == 0 或不可恢复错误
    }
    return true;
}

}  // namespace et
