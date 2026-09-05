#pragma once

// UDP 链路接收端：
// recv 线程 → 按组（group_id=frame_id）缓存分片 → 组内丢 1 片用 FEC 恢复，
// 多片丢失发 NACK（节流 10ms，250ms 放弃）→ 完整帧推入帧级 Jitter Buffer
// （按 frame_id 排序 + 自适应去抖）→ 出队输出 EncodedFrame。
// RTT：500ms 周期 StatsPing/Pong。

#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <thread>

#include "common/logger.h"
#include "common/net_utils.h"
#include "common/timer.h"
#include "common/thread_safe_queue.h"
#include "receiver/jitter_buffer.h"
#include "stats/metrics.h"
#include "transport/fec.h"

namespace rtstream {

struct UdpAssembled {
    uint32_t frame_id = 0;
    int64_t capture_us = 0;
    bool keyframe = false;
    std::vector<uint8_t> annexb;
};

class UdpReceiver {
public:
    UdpReceiver() : jb_(256) { jb_.set_nack_interval_us(10 * 1000); }
    ~UdpReceiver() { stop(); }

    ThreadSafeQueue<EncodedFramePtr> out{8};

    bool start(const std::string& server_ip, uint16_t server_port, std::string& error) {
        server_ip_ = server_ip;
        server_port_ = server_port;
        fd_ = create_udp_socket();
        if (fd_ < 0) { error = "udp socket: " + errno_str(); return false; }
        set_recv_buffer(fd_, 4 * 1024 * 1024);
        // 绑定任意端口；服务端通过首个 Ping 学习回包地址
        if (!bind_socket(fd_, 0)) { error = "bind: " + errno_str(); return false; }

        jb_.set_on_missing([](uint32_t) {
            // 帧级缺口：整组从未到达，无法指定分片，等待 JB 超时跳帧
        });

        running_ = true;
        recv_thread_ = std::thread([this] { recv_loop(); });
        jb_thread_ = std::thread([this] { jb_loop(); });
        ping_thread_ = std::thread([this] { ping_loop(); });
        // 首个 Ping 同时完成客户端注册
        send_ping();
        RTS_LOGI("udp-rx", "started -> %s:%u", server_ip_.c_str(), server_port_);
        return true;
    }

    void stop() {
        running_ = false;
        close_fd(fd_);
        if (recv_thread_.joinable()) recv_thread_.join();
        if (jb_thread_.joinable()) jb_thread_.join();
        if (ping_thread_.joinable()) ping_thread_.join();
        out.close();
    }

private:
    struct Group {
        uint16_t fragment_cnt = 0;      // 数据分片数（不含 FEC）
        uint32_t frame_id = 0;
        int64_t capture_us = 0;
        bool keyframe = false;
        int64_t first_recv_us = 0;
        int64_t last_nack_us = 0;
        bool fec_seen = false;
        std::vector<std::vector<uint8_t>> frags;  // empty 槽位=缺失
        std::vector<uint8_t> parity;
    };

    static bool complete(const Group& g) {
        for (auto& f : g.frags)
            if (f.empty()) return false;
        return true;
    }

    void recv_loop() {
        uint8_t buf[65536];
        while (running_) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n < 0) {
                if (!running_) break;
                if (errno == EAGAIN || errno == EINTR) continue;
                break;
            }
            if (static_cast<size_t>(n) < UdpHeader::kSize) continue;
            Metrics::instance().net_packets_recv++;
            Metrics::instance().net_bytes_recv += static_cast<size_t>(n);

            if (UdpHeader::magic_ok(buf)) {
                UdpHeader h = UdpHeader::read(buf);
                if (h.payload_type == static_cast<uint8_t>(PayloadType::StatsJson)) {
                    size_t plen = static_cast<size_t>(n) - UdpHeader::kSize;
                    if (h.payload_len > 0 && h.payload_len < plen) plen = h.payload_len;
                    Metrics::instance().update_remote_from_json(
                        std::string(reinterpret_cast<const char*>(buf) + UdpHeader::kSize,
                                    plen));
                    continue;
                }
                if (h.payload_type == static_cast<uint8_t>(PayloadType::StatsPong)) {
                    if (n - UdpHeader::kSize >= 8) {
                        int64_t sent;
                        std::memcpy(&sent, buf + UdpHeader::kSize, 8);
                        double rtt = (now_us() - sent) / 1000.0;
                        update_rtt(rtt);
                    }
                    continue;
                }
                on_media(h, buf + UdpHeader::kSize, static_cast<size_t>(n) - UdpHeader::kSize);
            }
            // RTP 首字节 0x80 不会出现在 UDP 链路
        }
    }

    void on_media(const UdpHeader& h, const uint8_t* payload, size_t len) {
        int64_t capture = extend_capture_us(h.capture_us_lo);
        auto& g = groups_[h.group_id];
        if (g.frags.empty()) {
            g.first_recv_us = now_us();
            g.frame_id = h.group_id;
            g.capture_us = capture;
            g.keyframe = h.keyframe();
            g.fragment_cnt = static_cast<uint16_t>(h.fragment_cnt - (h.fragment_cnt > 0 ? 1 : 0));
            g.frags.resize(g.fragment_cnt);
        }
        if (h.fragment_seq == UdpHeader::kFecSeq) {
            g.fec_seen = true;
            g.parity.assign(payload, payload + len);
        } else if (h.fragment_seq < g.fragment_cnt) {
            if (g.frags[h.fragment_seq].empty())
                g.frags[h.fragment_seq].assign(payload, payload + len);
        } else {
            return;  // 越界，协议错位
        }

        try_finalize(g);
    }

    void try_finalize(Group& g) {
        if (complete(g)) {
            finish(g);
            return;
        }
        // FEC 恢复：恰好缺 1 片且 FEC 已到
        if (g.fec_seen) {
            size_t expect = 0;
            for (auto& f : g.frags) expect = std::max(expect, f.size());
            expect = std::max(expect, g.parity.size());
            std::vector<std::vector<uint8_t>> known = g.frags;
            known.push_back(g.parity);
            if (fec_recover(known, expect)) {
                g.frags.assign(known.begin(), known.begin() + g.fragment_cnt);
                Metrics::instance().fec_recovered++;
                finish(g);
                return;
            }
        }
        // NACK：缺多片，节流请求
        int64_t now = now_us();
        if (now - g.last_nack_us >= 10 * 1000) {
            g.last_nack_us = now;
            send_nack(g);
        }
        if (now - g.first_recv_us > 250 * 1000) {
            // 放弃该组
            Metrics::instance().net_lost += missing_count(g);
            groups_.erase(g.frame_id);
        }
    }

    static size_t missing_count(const Group& g) {
        size_t m = 0;
        for (auto& f : g.frags)
            if (f.empty()) m++;
        return m;
    }

    void finish(Group& g) {
        size_t total = 0;
        for (auto& f : g.frags) total += f.size();
        auto a = std::make_shared<UdpAssembled>();
        a->frame_id = g.frame_id;
        a->capture_us = g.capture_us;
        a->keyframe = g.keyframe;
        a->annexb.reserve(total);
        for (auto& f : g.frags) a->annexb.insert(a->annexb.end(), f.begin(), f.end());
        uint32_t fid = g.frame_id;
        int64_t cap = g.capture_us;
        groups_.erase(fid);
        jb_.push(fid, cap, std::move(a));
    }

    void send_nack(const Group& g) {
        std::vector<uint8_t> recs;
        for (uint16_t i = 0; i < g.fragment_cnt; ++i) {
            if (g.frags[i].empty()) {
                uint32_t gid = htonl(g.frame_id);
                uint16_t seq = htons(i);
                recs.insert(recs.end(), reinterpret_cast<const uint8_t*>(&gid),
                            reinterpret_cast<const uint8_t*>(&gid) + 4);
                recs.insert(recs.end(), reinterpret_cast<const uint8_t*>(&seq),
                            reinterpret_cast<const uint8_t*>(&seq) + 2);
            }
        }
        if (recs.empty()) return;
        std::vector<uint8_t> pkt(UdpHeader::kSize + recs.size());
        UdpHeader h;
        h.payload_type = static_cast<uint8_t>(PayloadType::Nack);
        h.payload_len = static_cast<uint16_t>(recs.size());
        h.write(pkt.data());
        std::memcpy(pkt.data() + UdpHeader::kSize, recs.data(), recs.size());
        udp_sendto(fd_, server_ip_, server_port_, pkt.data(), pkt.size());
    }

    void send_ping() {
        std::vector<uint8_t> pkt(UdpHeader::kSize + 8);
        UdpHeader h;
        h.payload_type = static_cast<uint8_t>(PayloadType::StatsPing);
        h.payload_len = 8;
        h.write(pkt.data());
        int64_t t = now_us();
        std::memcpy(pkt.data() + UdpHeader::kSize, &t, 8);
        udp_sendto(fd_, server_ip_, server_port_, pkt.data(), pkt.size());
    }

    void update_rtt(double ms) {
        double& r = cur_rtt_;
        r = (r == 0) ? ms : (r * 0.875 + ms * 0.125);
        Metrics::instance().rtt_ms = r;
        jb_.set_rtt_ms(r);
    }

    void ping_loop() {
        // 20ms 小片轮询：保持 500ms 心跳周期，同时 stop() 时 join 至多等 20ms
        int64_t next_send = now_us();
        while (running_) {
            if (now_us() >= next_send) {
                send_ping();
                next_send += 500000;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }

    void jb_loop() {
        JitterBuffer<UdpAssembled>::Unit u;
        while (running_) {
            if (!jb_.pop(u)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            auto ef = std::make_shared<EncodedFrame>();
            ef->capture_us = u.data->capture_us;
            ef->wall_us = 0;
            ef->frame_id = u.data->frame_id;
            ef->keyframe = u.data->keyframe;
            ef->data = std::move(u.data->annexb);
            ef->encode_done_us = now_us();
            out.push(ef);
        }
    }

    // 32 位 capture_us 低段回绕扩展；首包用本地单调钟高 32 位作基准
    // （链路单向延迟远小于 2^31 us≈35.8 分钟，回环/实时场景成立）
    int64_t extend_capture_us(uint32_t lo) {
        if (last_capture_us_ == 0) {
            int64_t now = now_us();
            int64_t candidate = (now & ~0xFFFFFFFFLL) | lo;
            if (candidate > now + (1LL << 31)) candidate -= (1LL << 32);
            if (candidate < now - (1LL << 31)) candidate += (1LL << 32);
            last_capture_us_ = candidate;
            return candidate;
        }
        int64_t candidate = (last_capture_us_ & ~0xFFFFFFFFLL) | lo;
        while (candidate + (1LL << 31) < last_capture_us_) candidate += (1LL << 32);
        while (candidate > last_capture_us_ + (1LL << 31)) candidate -= (1LL << 32);
        last_capture_us_ = candidate;
        return candidate;
    }

    std::string server_ip_;
    uint16_t server_port_ = 0;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread recv_thread_, jb_thread_, ping_thread_;
    std::mutex groups_mtx_;
    std::map<uint32_t, Group> groups_;
    JitterBuffer<UdpAssembled> jb_;
    double cur_rtt_ = 0;
    int64_t last_capture_us_ = 0;
};

} // namespace rtstream
