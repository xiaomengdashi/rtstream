#pragma once

// RTP 链路接收端：
// recv 线程：RTP 包（0x80 开头）→ 包级 JitterBuffer（按 seq 重排）；
//           自研控制包：FEC parity（按 capture_ms 暂存）、Pong→RTT。
// 组装线程：JB 出队（有序）→ 按帧聚合（ts 相同为一帧，marker 结束），
//           帧内恰缺 1 包且 parity 到达 → XOR 恢复该 RTP 包后组帧；
//           缺多包 → 已由 JB 的 on_missing 发 NACK，超时放弃整帧。
// NACK：6 字节记录（gid=0 + RTP seq），与服务端 NackManager 约定一致。

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

struct RtpPkt {
    uint16_t seq = 0;
    uint32_t ts = 0;
    bool marker = false;
    int64_t capture_ms = 0;        // 由 90kHz 时间戳回绕扩展还原（毫秒精度）
    std::vector<uint8_t> raw;      // 完整 RTP 包（含 12B 头）
};

struct RtpAssembled {
    int64_t capture_us = 0;
    bool keyframe = false;
    std::vector<uint8_t> annexb;
};

class RtpReceiver {
public:
    RtpReceiver() : jb_(1024) { jb_.set_nack_interval_us(10 * 1000); }
    ~RtpReceiver() { stop(); }

    ThreadSafeQueue<EncodedFramePtr> out{8};

    bool start(const std::string& server_ip, uint16_t server_port, std::string& error) {
        server_ip_ = server_ip;
        server_port_ = server_port;
        fd_ = create_udp_socket();
        if (fd_ < 0) { error = "rtp socket: " + errno_str(); return false; }
        set_recv_buffer(fd_, 4 * 1024 * 1024);
        if (!bind_socket(fd_, 0)) { error = "bind: " + errno_str(); return false; }

        jb_.set_on_missing([this](uint32_t seq) {
            send_nack(static_cast<uint16_t>(seq));
        });

        running_ = true;
        recv_thread_ = std::thread([this] { recv_loop(); });
        asm_thread_ = std::thread([this] { assemble_loop(); });
        ping_thread_ = std::thread([this] { ping_loop(); });
        send_ping();
        RTS_LOGI("rtp-rx", "started -> %s:%u", server_ip_.c_str(), server_port_);
        return true;
    }

    void stop() {
        running_ = false;
        close_fd(fd_);
        if (recv_thread_.joinable()) recv_thread_.join();
        if (asm_thread_.joinable()) asm_thread_.join();
        if (ping_thread_.joinable()) ping_thread_.join();
        out.close();
    }

private:
    // ---- RTP 解析 ----
    static bool parse_rtp(const uint8_t* d, size_t len, RtpPkt& p) {
        if (len < 12 || d[0] != 0x80) return false;
        if ((d[0] & 0x0F) != 0 || (d[0] & 0x10) != 0) return false;  // 不支持 CSRC/扩展
        p.marker = (d[1] & 0x80) != 0;
        uint16_t s;
        std::memcpy(&s, d + 2, 2);
        p.seq = ntohs(s);
        uint32_t t;
        std::memcpy(&t, d + 4, 4);
        p.ts = ntohl(t);
        p.raw.assign(d, d + len);
        return true;
    }

    // ---- 接收 ----
    void recv_loop() {
        uint8_t buf[65536];
        while (running_) {
            ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n < 0) {
                if (!running_) break;
                if (errno == EAGAIN || errno == EINTR) continue;
                break;
            }
            if (static_cast<size_t>(n) < 12) continue;
            Metrics::instance().net_packets_recv++;
            Metrics::instance().net_bytes_recv += static_cast<size_t>(n);

            if (buf[0] == 0x80) {
                RtpPkt p;
                if (!parse_rtp(buf, static_cast<size_t>(n), p)) continue;
                p.capture_ms = extend_ts90(p.ts) / 90;
                jb_.push(p.seq, p.capture_ms * 1000, std::make_shared<RtpPkt>(std::move(p)));
            } else if (UdpHeader::magic_ok(buf)) {
                UdpHeader h = UdpHeader::read(buf);
                const uint8_t* payload = buf + UdpHeader::kSize;
                size_t plen = static_cast<size_t>(n) - UdpHeader::kSize;
                switch (h.payload_type) {
                    case static_cast<uint8_t>(PayloadType::StatsPong): {
                        if (plen >= 8) {
                            int64_t sent;
                            std::memcpy(&sent, payload, 8);
                            double rtt = (now_us() - sent) / 1000.0;
                            double& r = cur_rtt_;
                            r = (r == 0) ? rtt : (r * 0.875 + rtt * 0.125);
                            Metrics::instance().rtt_ms = r;
                            jb_.set_rtt_ms(r);
                        }
                        break;
                    }
                    case static_cast<uint8_t>(PayloadType::Fec): {
                        std::lock_guard<std::mutex> lk(fec_mtx_);
                        // 服务端约定：FEC 包头的 capture_us_lo 字段承载 capture_ms
                        fec_[h.capture_us_lo] = std::vector<uint8_t>(payload, payload + plen);
                        if (fec_.size() > 64) fec_.erase(fec_.begin());
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }

    // ---- 组装 ----
    struct FrameState {
        bool started = false;
        bool marker_seen = false;
        int64_t capture_ms = 0;
        uint32_t ts = 0;
        uint16_t first_seq = 0;
        std::map<uint16_t, std::vector<uint8_t>> raws;  // seq → 完整 RTP 包
        int64_t first_seen_us = 0;
    };

    void assemble_loop() {
        JitterBuffer<RtpPkt>::Unit u;
        while (running_) {
            if (!jb_.pop(u)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const RtpPkt& p = *u.data;
            auto& f = cur_;
            if (!f.started || p.ts != f.ts) {
                finalize_frame(f, false);      // 上一帧收尾（可能进入 zombie 等待）
                f = FrameState{};
                f.started = true;
                f.ts = p.ts;
                f.capture_ms = p.capture_ms;
                f.first_seq = p.seq;
                f.first_seen_us = now_us();
            }
            f.marker_seen = f.marker_seen || p.marker;
            f.raws.emplace(p.seq, p.raw);
            if (p.marker) finalize_frame(f, true);
            expire_zombies();
        }
    }

    void finalize_frame(FrameState& f, bool explicit_marker) {
        if (!f.started || f.raws.empty()) return;
        uint16_t last_seq = f.raws.rbegin()->first;
        uint32_t span = static_cast<uint32_t>(last_seq - f.first_seq) + 1;
        bool complete = (f.raws.size() == span) && f.marker_seen;

        if (complete) {
            emit(f, nullptr);
            return;
        }

        // 恰缺 1 包 + parity → XOR 恢复
        auto it = fec_.find(static_cast<uint32_t>(f.capture_ms));
        if (f.raws.size() == span - 1 && it != fec_.end() && f.marker_seen) {
            std::vector<std::vector<uint8_t>> known(span);
            size_t i = 0;
            int missing_idx = -1;
            for (uint32_t s = 0; s < span; ++s, ++i) {
                uint16_t seq = static_cast<uint16_t>(f.first_seq + s);
                auto r = f.raws.find(seq);
                if (r != f.raws.end()) known[i] = r->second;
                else missing_idx = static_cast<int>(i);
            }
            known.push_back(it->second);  // parity 作为最后一个元素参与恢复
            size_t max_len = it->second.size();
            for (auto& k : known) max_len = std::max(max_len, k.size());
            if (missing_idx >= 0 && fec_recover(known, max_len)) {
                emit(f, &known[static_cast<size_t>(missing_idx)]);
                std::lock_guard<std::mutex> lk(fec_mtx_);
                fec_.erase(it);
                Metrics::instance().fec_recovered++;
                return;
            }
        }

        // 不完整：marker 已到但还有缺口 → 进 zombie 等 NACK/重传；超时放弃
        int64_t now = now_us();
        if (explicit_marker && now - f.first_seen_us < 250 * 1000) {
            zombies_.push_back(std::move(f));
        } else {
            Metrics::instance().net_lost += span - f.raws.size();
        }
        f = FrameState{};
    }

    // raw_extra: FEC 恢复出的缺失 RTP 包（可能为空指针）
    void emit(FrameState& f, const std::vector<uint8_t>* raw_extra) {
        // 按 seq 序展开 annexb
        std::vector<std::vector<uint8_t>> ordered;
        uint32_t span = 0;
        if (!f.raws.empty()) {
            uint16_t last_seq = f.raws.rbegin()->first;
            span = static_cast<uint32_t>(last_seq - f.first_seq) + 1;
            ordered.resize(span);
            for (auto& kv : f.raws)
                ordered[static_cast<size_t>(kv.first - f.first_seq)] = kv.second;
        }
        if (raw_extra && !raw_extra->empty()) {
            uint16_t seq;
            std::memcpy(&seq, raw_extra->data() + 2, 2);
            seq = ntohs(seq);
            uint32_t idx = static_cast<uint32_t>(seq - f.first_seq);
            if (idx < span) ordered[idx] = *raw_extra;
        }

        RtpAssembled a;
        bool fu_active = false;
        for (auto& raw : ordered) {
            if (raw.size() < 13) continue;
            const uint8_t* pl = raw.data() + 12;
            size_t plen = raw.size() - 12;
            uint8_t nt = pl[0] & 0x1F;
            if (nt == 28) {  // FU-A
                if (plen < 2) continue;
                uint8_t fu = pl[1];
                if (fu & 0x80) {  // S
                    fu_active = true;
                    uint8_t nal_hdr = static_cast<uint8_t>((pl[0] & 0xE0) | (fu & 0x1F));
                    a.annexb.push_back(0x00);
                    a.annexb.push_back(0x00);
                    a.annexb.push_back(0x00);
                    a.annexb.push_back(0x01);
                    a.annexb.push_back(nal_hdr);
                    if ((nal_hdr & 0x1F) == 5) a.keyframe = true;
                    a.annexb.insert(a.annexb.end(), pl + 2, pl + plen);
                } else if (fu_active) {
                    a.annexb.insert(a.annexb.end(), pl + 2, pl + plen);
                }
                if (fu & 0x40) fu_active = false;  // E
            } else {           // Single NALU
                fu_active = false;
                a.annexb.push_back(0x00);
                a.annexb.push_back(0x00);
                a.annexb.push_back(0x00);
                a.annexb.push_back(0x01);
                a.annexb.insert(a.annexb.end(), pl, pl + plen);
                if (nt == 5) a.keyframe = true;
            }
        }
        if (a.annexb.empty()) return;

        auto ef = std::make_shared<EncodedFrame>();
        ef->capture_us = f.capture_ms * 1000;
        ef->frame_id = f.capture_ms;   // 以 capture_ms 作帧序（单调）
        ef->keyframe = a.keyframe;
        ef->data = std::move(a.annexb);
        ef->encode_done_us = now_us();
        out.push(ef);
    }

    void expire_zombies() {
        int64_t now = now_us();
        for (auto it = zombies_.begin(); it != zombies_.end();) {
            bool done = false;
            // parity 到位且恰缺 1 → 恢复
            auto fit = fec_.find(static_cast<uint32_t>(it->capture_ms));
            if (fit != fec_.end() && it->marker_seen) {
                uint16_t last_seq = it->raws.rbegin()->first;
                uint32_t span = static_cast<uint32_t>(last_seq - it->first_seq) + 1;
                if (it->raws.size() == span - 1) {
                    FrameState tmp = *it;
                    if (recover_zombie(tmp, fit->second)) {
                        fec_.erase(fit);
                        it = zombies_.erase(it);
                        done = true;
                    }
                }
            }
            if (done) continue;
            if (now - it->first_seen_us > 250 * 1000) {
                uint16_t last_seq = it->raws.rbegin()->first;
                uint32_t span = static_cast<uint32_t>(last_seq - it->first_seq) + 1;
                Metrics::instance().net_lost += span - it->raws.size();
                it = zombies_.erase(it);
                continue;
            }
            ++it;
        }
    }

    bool recover_zombie(FrameState& f, const std::vector<uint8_t>& parity) {
        uint16_t last_seq = f.raws.rbegin()->first;
        uint32_t span = static_cast<uint32_t>(last_seq - f.first_seq) + 1;
        std::vector<std::vector<uint8_t>> known(span);
        int missing_idx = -1;
        for (uint32_t s = 0; s < span; ++s) {
            uint16_t seq = static_cast<uint16_t>(f.first_seq + s);
            auto r = f.raws.find(seq);
            if (r != f.raws.end()) known[s] = r->second;
            else missing_idx = static_cast<int>(s);
        }
        if (missing_idx < 0) return false;
        known.push_back(parity);
        size_t max_len = parity.size();
        for (auto& k : known) max_len = std::max(max_len, k.size());
        if (!fec_recover(known, max_len)) return false;
        emit(f, &known[static_cast<size_t>(missing_idx)]);
        Metrics::instance().fec_recovered++;
        return true;
    }

    // ---- NACK / Ping ----
    void send_nack(uint16_t rtp_seq) {
        std::vector<uint8_t> pkt(UdpHeader::kSize + 6);
        UdpHeader h;
        h.payload_type = static_cast<uint8_t>(PayloadType::Nack);
        h.payload_len = 6;
        h.write(pkt.data());
        uint32_t gid = 0;
        uint16_t seq = htons(rtp_seq);
        std::memcpy(pkt.data() + UdpHeader::kSize, &gid, 4);
        std::memcpy(pkt.data() + UdpHeader::kSize + 4, &seq, 2);
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

    void ping_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (running_) send_ping();
        }
    }

    // RTP 时间戳 90kHz 模 2^32 回绕扩展；首包用本地单调钟折算的 90kHz 高位作基准
    int64_t extend_ts90(uint32_t ts90) {
        if (last_ts90_full_ == 0) {
            int64_t now_ticks = now_us() * 90 / 1000;
            int64_t cand = (now_ticks & ~0xFFFFFFFFLL) | ts90;
            if (cand > now_ticks + (1LL << 31)) cand -= (1LL << 32);
            if (cand < now_ticks - (1LL << 31)) cand += (1LL << 32);
            last_ts90_full_ = cand;
            return cand;
        }
        int64_t cand = (last_ts90_full_ & ~0xFFFFFFFFLL) | ts90;
        while (cand + (1LL << 31) < last_ts90_full_) cand += (1LL << 32);
        while (cand > last_ts90_full_ + (1LL << 31)) cand -= (1LL << 32);
        last_ts90_full_ = cand;
        return cand;
    }

    std::string server_ip_;
    uint16_t server_port_ = 0;
    int fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread recv_thread_, asm_thread_, ping_thread_;
    JitterBuffer<RtpPkt> jb_;
    FrameState cur_;
    std::mutex fec_mtx_;
    std::map<uint32_t, std::vector<uint8_t>> fec_;  // capture_ms → parity
    std::vector<FrameState> zombies_;
    double cur_rtt_ = 0;
    int64_t last_ts90_full_ = 0;
};

} // namespace rtstream
