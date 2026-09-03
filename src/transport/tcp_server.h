#pragma once

// TCP 链路服务器：4 字节长度前缀 + 20 字节帧头 + H.264 Annex-B。
// 每客户端一个发送队列 + 发送线程，慢客户端丢帧不阻塞流水线。
// TCP 自带可靠性，无需 FEC/NACK。

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "common/logger.h"
#include "common/net_utils.h"
#include "common/thread_safe_queue.h"
#include "stats/metrics.h"
#include "transport_server.h"

namespace rtstream {

class TcpServer : public TransportServer {
public:
    explicit TcpServer(const ServerConfig& cfg) : cfg_(cfg) {}
    ~TcpServer() override { stop(); }

    bool start(std::string& error) override {
        listen_fd_ = create_tcp_socket();
        if (listen_fd_ < 0) { error = "socket: " + errno_str(); return false; }
        set_reuseaddr(listen_fd_);
        if (!bind_socket(listen_fd_, cfg_.tcp_port)) {
            error = "bind tcp " + std::to_string(cfg_.tcp_port) + ": " + errno_str();
            return false;
        }
        if (::listen(listen_fd_, 4) < 0) { error = "listen: " + errno_str(); return false; }
        running_ = true;
        accept_thread_ = std::thread([this] { accept_loop(); });
        RTS_LOGI("tcp", "listening on %u", cfg_.tcp_port);
        return true;
    }

    void stop() override {
        running_ = false;
        close_fd(listen_fd_);
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& c : clients_) {
                c->queue.close();
                close_fd(c->fd);   // 唤醒阻塞中的 send/queue
            }
        }
        cv_clients_.notify_all();
        if (accept_thread_.joinable()) accept_thread_.join();
        // join 所有发送线程
        std::vector<std::thread*> to_join;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (auto& c : clients_) to_join.push_back(&c->thread);
        }
        for (auto* t : to_join)
            if (t->joinable()) t->join();
        {
            std::lock_guard<std::mutex> lk(mtx_);
            clients_.clear();
        }
    }

    void broadcast(const EncodedFramePtr& frame) override {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& c : clients_) {
            if (!c->alive) continue;
            // 有界队列满则丢最旧，保证不阻塞分发线程
            if (!c->queue.try_push(frame)) Metrics::instance().queue_dropped++;
        }
    }

    const char* name() const override { return "tcp"; }

private:
    struct Client {
        int fd = -1;
        ThreadSafeQueue<EncodedFramePtr> queue{16};
        std::thread thread;
        std::atomic<bool> alive{true};
    };

    void accept_loop() {
        while (running_) {
            int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                if (!running_) break;
                if (errno == EINTR || errno == EAGAIN) continue;
                break;
            }
            set_tcp_nodelay(fd);
            set_send_buffer(fd, 512 * 1024);
            auto c = std::make_shared<Client>();
            c->fd = fd;
            c->thread = std::thread([this, c] { send_loop(c); });
            {
                std::lock_guard<std::mutex> lk(mtx_);
                clients_.push_back(c);
            }
            RTS_LOGI("tcp", "client connected fd=%d total=%zu", fd, client_count());
        }
    }

    void send_loop(std::shared_ptr<Client> c) {
        EncodedFramePtr frame;
        while (c->queue.pop(frame)) {
            std::vector<uint8_t> pkt(kHeaderLen + frame->data.size());
            TcpWire::write_header(pkt.data(), *frame);
            std::memcpy(pkt.data() + kHeaderLen, frame->data.data(), frame->data.size());
            uint32_t total = htonl(static_cast<uint32_t>(pkt.size()));
            // 发送：4B 长度 + 头 + 数据；失败即断开
            if (!send_all(c->fd, &total, 4) ||
                !send_all(c->fd, pkt.data(), pkt.size())) {
                RTS_LOGW("tcp", "client fd=%d disconnected", c->fd);
                break;
            }
            Metrics::instance().net_packets_sent++;
            Metrics::instance().net_bytes_sent += pkt.size() + 4;
        }
        c->alive = false;
        close_fd(c->fd);
    }

    static bool send_all(int fd, const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t off = 0;
        while (off < len) {
            ssize_t n = ::send(fd, p + off, len - off, MSG_NOSIGNAL);
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                return false;
            }
            off += static_cast<size_t>(n);
        }
        return true;
    }

    size_t client_count() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return clients_.size();
    }

    static constexpr size_t kHeaderLen = TcpWire::kHeaderSize;

    ServerConfig cfg_;
    int listen_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    mutable std::mutex mtx_;
    std::condition_variable cv_clients_;
    std::vector<std::shared_ptr<Client>> clients_;
};

} // namespace rtstream
