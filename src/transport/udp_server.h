#pragma once

// UDP 链路服务器：自研分片协议（common/media_types.h 的 UdpHeader）。
// 每帧 → N 个数据分片 + 1 个 XOR FEC 恢复包（同一 group_id=帧组号）。
// 发送路径：LossSimulator（可选丢包/延迟注入）→ udp_sendto。
// 接收路径：NACK → NackManager 重传缓存；StatsPing → 注册客户端 + 回 Pong（RTT）。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "common/crc32.h"
#include "common/logger.h"
#include "common/net_utils.h"
#include "common/timer.h"
#include "stats/metrics.h"
#include "fec.h"
#include "loss_simulator.h"
#include "nack_manager.h"
#include "transport_server.h"

namespace rtstream {

class UdpServer : public TransportServer {
public:
    explicit UdpServer(const ServerConfig& cfg) : cfg_(cfg), nack_(cfg.nack_cache) {}
    ~UdpServer() override { stop(); }

    bool start(std::string& error) override {
        fd_ = create_udp_socket();
        if (fd_ < 0) { error = "udp socket: " + errno_str(); return false; }
        set_reuseaddr(fd_);
        set_recv_buffer(fd_, 4 * 1024 * 1024);
        if (!bind_socket(fd_, cfg_.udp_port)) {
            error = "bind udp " + std::to_string(cfg_.udp_port) + ": " + errno_str();
            return false;
        }
        // 发送链：simulator → 真实 socket
        sim_.set_sender([this](const uint8_t* d, size_t n) {
            std::lock_guard<std::mutex> lk(clients_mtx_);
            for (auto& c : clients_)
                udp_sendto(fd_, c.ip, c.port, d, n);
        });
        nack_.set_sender([this](const uint8_t* d, size_t n) { sim_.inject(d, n); });
        sim_.start();
        running_ = true;
        recv_thread_ = std::thread([this] { recv_loop(); });
        stats_thread_ = std::thread([this] { stats_loop(); });
        RTS_LOGI("udp", "listening on %u (mtu=%zu fecK=%u loss=%.2f delay=%ums jitter=%ums)",
                 cfg_.udp_port, cfg_.udp_mtu, cfg_.fec_group_k,
                 cfg_.loss_rate, cfg_.delay_ms, cfg_.jitter_ms);
        return true;
    }

    void stop() override {
        running_ = false;
        cv_stats_.notify_all();
        sim_.stop();
        close_fd(fd_);
        if (recv_thread_.joinable()) recv_thread_.join();
        if (stats_thread_.joinable()) stats_thread_.join();
    }

    void broadcast(const EncodedFramePtr& frame) override {
        if (client_count() == 0) return;
        std::lock_guard<std::mutex> lk(send_mtx_);
        send_frame(*frame);
    }

    const char* name() const override { return "udp"; }

private:
    struct ClientAddr {
        std::string ip;
        uint16_t port = 0;
    };

    size_t client_count() const {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        return clients_.size();
    }

    void raw_send(const uint8_t* d, size_t n) {
        // 重传/NACK 回包不经弱网模拟器，直接发
        std::lock_guard<std::mutex> lk(clients_mtx_);
        for (auto& c : clients_)
            udp_sendto(fd_, c.ip, c.port, d, n);
    }

    void send_frame(const EncodedFrame& f) {
        const size_t payload_size = cfg_.udp_mtu - UdpHeader::kSize;
        size_t cnt = (f.data.size() + payload_size - 1) / payload_size;
        if (cnt == 0) cnt = 1;

        std::vector<std::vector<uint8_t>> fragments(cnt);
        for (size_t i = 0; i < cnt; ++i) {
            size_t off = i * payload_size;
            size_t len = std::min(payload_size, f.data.size() - off);
            fragments[i].assign(f.data.begin() + off, f.data.begin() + off + len);
        }

        uint32_t gid = static_cast<uint32_t>(f.frame_id);
        std::vector<uint8_t> parity = fec_generate(fragments);

        auto emit = [&](uint16_t seq, const uint8_t* payload, size_t len, bool is_fec) {
            std::vector<uint8_t> pkt(UdpHeader::kSize + len);
            UdpHeader h;
            h.payload_type = static_cast<uint8_t>(PayloadType::Data);
            h.group_id = gid;
            h.fragment_seq = is_fec ? UdpHeader::kFecSeq : seq;
            h.fragment_cnt = static_cast<uint16_t>(cnt + 1);
            h.frame_id = static_cast<uint32_t>(f.frame_id);
            h.capture_us_lo = static_cast<uint32_t>(f.capture_us & 0xFFFFFFFFu);
            h.payload_len = static_cast<uint16_t>(len);
            h.set_keyframe(f.keyframe);
            h.write(pkt.data());
            std::memcpy(pkt.data() + UdpHeader::kSize, payload, len);
            if (is_fec) {
                h.payload_type = static_cast<uint8_t>(PayloadType::Fec);
                h.write(pkt.data());
                Metrics::instance().net_fec_sent++;
            }
            nack_.remember(gid, h.fragment_seq, pkt);
            sim_.inject(pkt.data(), pkt.size());
            Metrics::instance().net_packets_sent++;
            Metrics::instance().net_bytes_sent += pkt.size();
        };

        for (size_t i = 0; i < cnt; ++i)
            emit(static_cast<uint16_t>(i), fragments[i].data(), fragments[i].size(), false);
        emit(UdpHeader::kFecSeq, parity.data(), parity.size(), true);
    }

    // 每秒下发服务端指标（PayloadType::StatsJson），供客户端面板展示
    void stats_loop() {
        while (running_) {
            {
                std::unique_lock<std::mutex> lk(stats_mtx_);
                cv_stats_.wait_for(lk, std::chrono::milliseconds(1000), [this] { return !running_; });
            }
            if (!running_ || client_count() == 0) continue;
            std::string payload = Metrics::instance().to_json();
            if (payload.size() + UdpHeader::kSize > 65535) continue;
            std::vector<uint8_t> pkt(UdpHeader::kSize + payload.size());
            UdpHeader h;
            h.payload_type = static_cast<uint8_t>(PayloadType::StatsJson);
            h.payload_len = static_cast<uint16_t>(payload.size());
            h.write(pkt.data());
            std::memcpy(pkt.data() + UdpHeader::kSize, payload.data(), payload.size());
            raw_send(pkt.data(), pkt.size());
        }
    }

    void recv_loop() {
        uint8_t buf[65536];
        while (running_) {
            struct sockaddr_in peer;
            socklen_t plen = sizeof(peer);
            ssize_t n = ::recvfrom(fd_, buf, sizeof(buf), 0,
                                   reinterpret_cast<struct sockaddr*>(&peer), &plen);
            if (n < 0) {
                if (!running_) break;
                if (errno == EAGAIN || errno == EINTR) continue;
                break;
            }
            if (static_cast<size_t>(n) < UdpHeader::kSize) continue;
            if (!UdpHeader::magic_ok(buf)) continue;
            UdpHeader h = UdpHeader::read(buf);

            char ip[64];
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            handle_control(ip, ntohs(peer.sin_port), h, buf, static_cast<size_t>(n));
        }
    }

    void handle_control(const std::string& ip, uint16_t port, const UdpHeader& h,
                        const uint8_t* buf, size_t len) {
        switch (h.payload_type) {
            case static_cast<uint8_t>(PayloadType::StatsPing): {
                register_client(ip, port);
                // 回 Pong：echo 原包，仅改 payload_type
                std::vector<uint8_t> pong(buf, buf + len);
                pong[3] = static_cast<uint8_t>(PayloadType::StatsPong);
                raw_send(pong.data(), pong.size());
                break;
            }
            case static_cast<uint8_t>(PayloadType::Nack):
                nack_.on_nack(buf + UdpHeader::kSize, len - UdpHeader::kSize);
                break;
            default:
                break;
        }
    }

    void register_client(const std::string& ip, uint16_t port) {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        for (auto& c : clients_) {
            if (c.ip == ip && c.port == port) return;
        }
        clients_.push_back({ip, port});
        RTS_LOGI("udp", "client registered %s:%u total=%zu", ip.c_str(), port, clients_.size());
    }

    ServerConfig cfg_;
    NackManager nack_;
    LossSimulator sim_{cfg_.loss_rate, cfg_.delay_ms, cfg_.jitter_ms};
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread recv_thread_;
    std::thread stats_thread_;
    std::mutex stats_mtx_;
    std::condition_variable cv_stats_;
    mutable std::mutex clients_mtx_;
    std::vector<ClientAddr> clients_;
    std::mutex send_mtx_;
};

} // namespace rtstream
