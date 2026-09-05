#pragma once

// RTP 链路服务器：RFC 6184 H.264 RTP 打包（Single NALU + FU-A），PT=96。
// - RTP 数据包：首字节 0x80（V=2），marker 置于帧尾包
// - FEC：帧内所有 RTP 包（含 12B 头）整体异或，包为自研头格式（首字节 'R'）
// - NACK：接收端回自研头包，载荷为 6 字节记录（group_id 忽略 + RTP seq），
//         服务端按 rtp_seq 查重传缓存
// - RTT：StatsPing/Pong 同 UDP 链路
// 区分数据/控制：首字节 0x80 = RTP，其余（magic 'RT'）= 自研控制。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "common/logger.h"
#include "common/net_utils.h"
#include "common/timer.h"
#include "stats/metrics.h"
#include "fec.h"
#include "loss_simulator.h"
#include "nack_manager.h"
#include "transport_server.h"

namespace rtstream {

class RtpServer : public TransportServer {
public:
    explicit RtpServer(const ServerConfig& cfg) : cfg_(cfg), nack_(cfg.nack_cache) {}
    ~RtpServer() override { stop(); }

    bool start(std::string& error) override {
        fd_ = create_udp_socket();
        if (fd_ < 0) { error = "rtp socket: " + errno_str(); return false; }
        set_reuseaddr(fd_);
        set_recv_buffer(fd_, 4 * 1024 * 1024);
        if (!bind_socket(fd_, cfg_.rtp_port)) {
            error = "bind rtp " + std::to_string(cfg_.rtp_port) + ": " + errno_str();
            return false;
        }
        sim_.set_sender([this](const uint8_t* d, size_t n) {
            std::lock_guard<std::mutex> lk(clients_mtx_);
            for (auto& c : clients_)
                udp_sendto(fd_, c.ip, c.port, d, n);
        });
        nack_.set_sender([this](const uint8_t* d, size_t n) { sim_.inject(d, n); });
        ssrc_ = static_cast<uint32_t>(now_us()) | 1u;
        sim_.start();
        running_ = true;
        recv_thread_ = std::thread([this] { recv_loop(); });
        stats_thread_ = std::thread([this] { stats_loop(); });
        RTS_LOGI("rtp", "listening on %u (mtu=%zu loss=%.2f delay=%ums jitter=%ums)",
                 cfg_.rtp_port, cfg_.udp_mtu, cfg_.loss_rate,
                 cfg_.delay_ms, cfg_.jitter_ms);
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

    const char* name() const override { return "rtp"; }

private:
    struct ClientAddr {
        std::string ip;
        uint16_t port = 0;
    };

    static constexpr size_t kRtpHeader = 12;
    static constexpr uint8_t kPayloadType = 96;

    size_t client_count() const {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        return clients_.size();
    }

    void raw_send(const uint8_t* d, size_t n) {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        for (auto& c : clients_)
            udp_sendto(fd_, c.ip, c.port, d, n);
    }

    // ---- RTP 打包 ----

    static void write_rtp_header(uint8_t* p, bool marker, uint16_t seq,
                                 uint32_t timestamp, uint32_t ssrc) {
        p[0] = 0x80;
        p[1] = static_cast<uint8_t>((marker ? 0x80 : 0x00) | kPayloadType);
        uint16_t s = htons(seq);
        std::memcpy(p + 2, &s, 2);
        uint32_t t = htonl(timestamp);
        std::memcpy(p + 4, &t, 4);
        uint32_t ss = htonl(ssrc);
        std::memcpy(p + 8, &ss, 4);
    }

    void send_frame(const EncodedFrame& f) {
        std::vector<std::vector<uint8_t>> packets;
        uint32_t timestamp = static_cast<uint32_t>((f.capture_us / 1000) * 90); // 90kHz

        size_t pos = 0;
        while (pos < f.data.size()) {
            size_t nal_len = 0;
            if (!next_nalu(f.data.data(), f.data.size(), pos, nal_len)) break;
            const uint8_t* nal = f.data.data() + pos;
            pos += nal_len;

            if (nal_len <= cfg_.udp_mtu - kRtpHeader) {
                // Single NALU
                std::vector<uint8_t> pkt(kRtpHeader + nal_len);
                write_rtp_header(pkt.data(), false, seq_++, timestamp, ssrc_);
                std::memcpy(pkt.data() + kRtpHeader, nal, nal_len);
                packets.push_back(std::move(pkt));
            } else {
                // FU-A 分片
                const size_t max_payload = cfg_.udp_mtu - kRtpHeader - 2;
                size_t payload_off = 1;  // 跳过 NALU 头
                size_t remain = nal_len - 1;
                while (remain > 0) {
                    size_t chunk = std::min<size_t>(max_payload, remain);
                    bool first = (payload_off == 1);
                    bool last = (chunk == remain);
                    std::vector<uint8_t> pkt(kRtpHeader + 2 + chunk);
                    write_rtp_header(pkt.data(), false, seq_++, timestamp, ssrc_);
                    pkt[kRtpHeader] = static_cast<uint8_t>((nal[0] & 0xE0) | 28); // FU indicator
                    pkt[kRtpHeader + 1] = static_cast<uint8_t>(
                        (first ? 0x80 : 0) | (last ? 0x40 : 0) | (nal[0] & 0x1F));
                    std::memcpy(pkt.data() + kRtpHeader + 2, nal + payload_off, chunk);
                    payload_off += chunk;
                    remain -= chunk;
                    packets.push_back(std::move(pkt));
                }
            }
        }
        if (packets.empty()) return;
        // 帧尾 marker
        packets.back()[1] |= 0x80;

        // FEC：帧内全部 RTP 包整体异或
        uint32_t parity_len = 0;
        for (auto& p : packets) parity_len = std::max<uint32_t>(parity_len, static_cast<uint32_t>(p.size()));
        std::vector<uint8_t> parity(parity_len, 0);
        for (auto& p : packets)
            for (size_t i = 0; i < p.size(); ++i)
                parity[i] ^= p[i];

        uint32_t gid = static_cast<uint32_t>(f.frame_id);
        for (auto& p : packets) {
            nack_.remember(0, read_seq(p.data()), p);
            sim_.inject(p.data(), p.size());
            Metrics::instance().net_packets_sent++;
            Metrics::instance().net_bytes_sent += p.size();
        }
        // FEC 包：自研头。capture_us_lo 字段在控制/FEC 包中承载 capture_ms
        //（32 位毫秒约 49 天回绕，远超会话时长），与 RTP 90kHz 时间戳可互相印证
        {
            std::vector<uint8_t> pkt(UdpHeader::kSize + parity.size());
            UdpHeader h;
            h.payload_type = static_cast<uint8_t>(PayloadType::Fec);
            h.group_id = gid;
            h.fragment_seq = UdpHeader::kFecSeq;
            h.fragment_cnt = static_cast<uint16_t>(packets.size());
            h.frame_id = static_cast<uint32_t>(f.frame_id);
            h.capture_us_lo = static_cast<uint32_t>(f.capture_us / 1000);  // capture_ms
            h.payload_len = static_cast<uint16_t>(parity.size());
            h.set_keyframe(f.keyframe);
            h.write(pkt.data());
            std::memcpy(pkt.data() + UdpHeader::kSize, parity.data(), parity.size());
            sim_.inject(pkt.data(), pkt.size());
            Metrics::instance().net_fec_sent++;
        }
    }

    static uint16_t read_seq(const uint8_t* p) {
        uint16_t s;
        std::memcpy(&s, p + 2, 2);
        return ntohs(s);
    }

    // 跳过 startcode，返回下一个 NALU 长度；pos 前移到 NALU 起点
    static bool next_nalu(const uint8_t* d, size_t len, size_t& pos, size_t& nal_len) {
        // 找 startcode
        size_t sc_start = SIZE_MAX, sc_len = 0;
        size_t i = pos;
        while (i + 2 < len + 1 && i < len) {
            if (d[i] == 0 && d[i + 1] == 0) {
                if (i + 2 < len && d[i + 2] == 1) { sc_start = i; sc_len = 3; break; }
                if (i + 3 < len && d[i + 2] == 0 && d[i + 3] == 1) { sc_start = i; sc_len = 4; break; }
                i += 1;
            } else {
                i += 1;
            }
        }
        if (sc_start == SIZE_MAX) return false;
        size_t nal_start = sc_start + sc_len;
        // 找下一个 startcode 作为结束
        size_t end = len;
        for (size_t j = nal_start; j + 2 < len; ++j) {
            if (d[j] == 0 && d[j + 1] == 0 && (d[j + 2] == 1 || (j + 3 < len && d[j + 2] == 0 && d[j + 3] == 1))) {
                end = j;
                break;
            }
        }
        if (nal_start >= end) return false;
        nal_len = end - nal_start;
        pos = nal_start;
        // pos 前移后，下一轮从 end 开始扫描 startcode（跳过当前 NALU 内容）
        // 处理：把 pos 暂存，调用方 pos += nal_len 后到达 end —— 正确
        return true;
    }

    // ---- 控制/接收 ----

    // 每秒下发服务端指标（PayloadType::StatsJson 控制包，与 UDP 链路同一格式）
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
            if (buf[0] == 0x80) continue;  // 自身不收 RTP
            if (!UdpHeader::magic_ok(buf)) continue;
            UdpHeader h = UdpHeader::read(buf);

            char ip[64];
            inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
            std::string sip = ip;
            uint16_t port = ntohs(peer.sin_port);

            switch (h.payload_type) {
                case static_cast<uint8_t>(PayloadType::StatsPing): {
                    register_client(sip, port);
                    std::vector<uint8_t> pong(buf, buf + n);
                    pong[3] = static_cast<uint8_t>(PayloadType::StatsPong);
                    raw_send(pong.data(), pong.size());
                    break;
                }
                case static_cast<uint8_t>(PayloadType::Nack):
                    nack_.on_nack(buf + UdpHeader::kSize, static_cast<size_t>(n) - UdpHeader::kSize);
                    break;
                default:
                    break;
            }
        }
    }

    void register_client(const std::string& ip, uint16_t port) {
        std::lock_guard<std::mutex> lk(clients_mtx_);
        for (auto& c : clients_) {
            if (c.ip == ip && c.port == port) return;
        }
        clients_.push_back({ip, port});
        RTS_LOGI("rtp", "client registered %s:%u total=%zu", ip.c_str(), port, clients_.size());
    }

    ServerConfig cfg_;
    NackManager nack_;
    LossSimulator sim_{cfg_.loss_rate, cfg_.delay_ms, cfg_.jitter_ms};
    int fd_ = -1;
    uint16_t seq_ = 0;
    uint32_t ssrc_ = 0;
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
