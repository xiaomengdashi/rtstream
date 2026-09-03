#pragma once

// socket 工具：Linux/epoll 为主，macOS 侧回退 poll/kqueue 兼容实现，
// 便于源码级检查。仅封装服务端/客户端用到的最小集合。

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <string>

// macOS/BSD 没有 MSG_NOSIGNAL；SIGPIPE 统一用 ignore_sigpipe() 屏蔽
#if defined(__APPLE__) && !defined(MSG_NOSIGNAL)
#define MSG_NOSIGNAL 0
#endif

namespace rtstream {

inline void ignore_sigpipe() {
    std::signal(SIGPIPE, SIG_IGN);
}

inline int create_tcp_socket() {
    return ::socket(AF_INET, SOCK_STREAM, 0);
}

inline int create_udp_socket() {
    return ::socket(AF_INET, SOCK_DGRAM, 0);
}

inline bool set_nonblocking(int fd, bool on) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return ::fcntl(fd, F_SETFL, flags) == 0;
}

inline bool set_reuseaddr(int fd) {
    int on = 1;
    return ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == 0;
}

inline bool set_recv_buffer(int fd, int bytes) {
    return ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes)) == 0;
}

inline bool set_send_buffer(int fd, int bytes) {
    return ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bytes, sizeof(bytes)) == 0;
}

inline bool set_tcp_nodelay(int fd) {
    int on = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) == 0;
}

inline bool bind_socket(int fd, uint16_t port) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    return ::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0;
}

// UDP 发送目标
inline bool udp_sendto(int fd, const std::string& ip, uint16_t port,
                       const uint8_t* data, size_t len) {
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) return false;
    addr.sin_port = htons(port);
    ssize_t n = ::sendto(fd, data, len, 0,
                         reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr));
    return n == static_cast<ssize_t>(len);
}

inline void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

inline std::string errno_str() {
    return std::string(std::strerror(errno));
}

} // namespace rtstream
