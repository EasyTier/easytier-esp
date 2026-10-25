/**
 * @file net.hpp
 * @brief 套接字接口的薄封装：目标平台使用 lwIP，宿主测试使用 POSIX socket。
 *
 * @details 传输层只使用两端都提供的 BSD 风格接口（socket/select/recv/send/
 * fcntl/getaddrinfo），因此拆帧与部分写逻辑可以在 PC 上用回环 socket 直接测试。
 */
#ifndef EASYTIER_ESP_SUPPORT_NET_HPP_
#define EASYTIER_ESP_SUPPORT_NET_HPP_

#include <cstddef>
#include <cstdint>

#if defined(ESP_PLATFORM)
#include "lwip/errno.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace et {
namespace net {

/** @brief 套接字句柄类型。 */
using Socket = int;
/** @brief 非法套接字。 */
inline constexpr Socket kInvalidSocket = -1;

/** @brief 关闭套接字（容错：非法句柄直接忽略）。 */
inline void close_socket(Socket fd) noexcept {
    if (fd != kInvalidSocket) {
        ::close(fd);
    }
}

/** @brief 设置为非阻塞。 */
inline bool set_nonblocking(Socket fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/** @brief 关闭 Nagle，降低控制面小报文的往返延迟。 */
inline void set_nodelay(Socket fd) noexcept {
    const int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

/** @brief 最近一次 errno。 */
inline int last_error() noexcept { return errno; }

/** @brief 是否为"暂时无法读写"（应等待下一次可读/可写事件）。 */
inline bool would_block(int err) noexcept { return err == EAGAIN || err == EWOULDBLOCK; }

/** @brief 是否处于连接建立中。 */
inline bool in_progress(int err) noexcept { return err == EINPROGRESS; }

/** @brief 是否被信号中断（宿主测试可能出现）。 */
inline bool interrupted(int err) noexcept { return err == EINTR; }

}  // namespace net
}  // namespace et

#endif
