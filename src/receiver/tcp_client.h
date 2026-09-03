#pragma once

// TCP 客户端（桌面渲染端用）：连接服务端，读 4B 长度前缀 + 20B 帧头 + Annex-B。

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>

#include "common/logger.h"
#include "common/net_utils.h"
#include "common/thread_safe_queue.h"
#include "stats/metrics.h"
#include "transport/transport_server.h"  // TcpWire

namespace rtstream {

class TcpClient {
public:
    ~TcpClient() { stop(); }

    ThreadSafeQueue<EncodedFramePtr> out{8};

    bool start(const std::string& ip, uint16_t port, std::string& error) {
        fd_ = create_tcp_socket();
        if (fd_ < 0) { error = "socket: " + errno_str(); return false; }
        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
            error = "invalid ip: " + ip;
            return false;
        }
        addr.sin_port = htons(port);
        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            error = "connect: " + errno_str();
            return false;
        }
        set_tcp_nodelay(fd_);
        set_recv_buffer(fd_, 1024 * 1024);
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        RTS_LOGI("tcp-rx", "connected %s:%u", ip.c_str(), port);
        return true;
    }

    void stop() {
        running_ = false;
        close_fd(fd_);
        if (thread_.joinable()) thread_.join();
        out.close();
    }

private:
    static bool read_all(int fd, uint8_t* p, size_t len) {
        size_t off = 0;
        while (off < len) {
            ssize_t n = ::recv(fd, p + off, len - off, 0);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }

    void loop() {
        uint8_t lenbuf[4];
        std::vector<uint8_t> buf;
        while (running_) {
            if (!read_all(fd_, lenbuf, 4)) break;
            uint32_t total;
            std::memcpy(&total, lenbuf, 4);
            total = ntohl(total);
            if (total < TcpWire::kHeaderSize || total > 16 * 1024 * 1024) {
                RTS_LOGE("tcp-rx", "bad frame length %u", total);
                break;
            }
            buf.resize(total);
            if (!read_all(fd_, buf.data(), total)) break;

            if (!TcpWire::magic_ok(buf.data())) continue;
            auto ef = std::make_shared<EncodedFrame>();
            ef->keyframe = buf[4] & 1;
            uint32_t fid;
            std::memcpy(&fid, buf.data() + 8, 4);
            ef->frame_id = ntohl(fid);
            ef->capture_us = static_cast<int64_t>(load_be64(buf.data() + 12));
            ef->wall_us = 0;
            ef->data.assign(buf.begin() + static_cast<long>(TcpWire::kHeaderSize), buf.end());
            ef->encode_done_us = now_us();

            Metrics::instance().net_packets_recv++;
            Metrics::instance().net_bytes_recv += total + 4;
            if (!out.push(ef)) break;
        }
        if (running_) RTS_LOGW("tcp-rx", "server disconnected");
        running_ = false;
    }

    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace rtstream
