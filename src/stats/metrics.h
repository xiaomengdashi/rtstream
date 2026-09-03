#pragma once

// 全局指标聚合（单例，线程安全）：
// 服务端打点：采集/编码/发送/FEC/NACK/模拟丢包
// 客户端打点：接收/恢复/解码/渲染/E2E 延时
// Web 端通过 snapshot().to_json() 周期性推给浏览器。

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>

namespace rtstream {

class Metrics {
public:
    static Metrics& instance() {
        static Metrics m;
        return m;
    }

    // ---- 计数器 ----
    std::atomic<uint64_t> capture_frames{0};
    std::atomic<uint64_t> encoded_frames{0};
    std::atomic<uint64_t> encoded_bytes{0};
    std::atomic<uint64_t> net_packets_sent{0};
    std::atomic<uint64_t> net_bytes_sent{0};
    std::atomic<uint64_t> net_fec_sent{0};        // FEC 恢复包发送数
    std::atomic<uint64_t> sim_dropped{0};         // 丢包模拟器丢弃数
    std::atomic<uint64_t> nack_received{0};       // 收到 NACK 请求数（条目数）
    std::atomic<uint64_t> retransmit_sent{0};     // 重传包发送数
    std::atomic<uint64_t> net_packets_recv{0};
    std::atomic<uint64_t> net_bytes_recv{0};
    std::atomic<uint64_t> net_lost{0};            // 接收端判定丢失（未恢复）
    std::atomic<uint64_t> fec_recovered{0};       // FEC 恢复成功包数
    std::atomic<uint64_t> nack_requested{0};      // 客户端发出 NACK 条目数
    std::atomic<uint64_t> nack_hit{0};            // NACK 重传收到的包数
    std::atomic<uint64_t> decoded_frames{0};
    std::atomic<uint64_t> rendered_frames{0};
    std::atomic<uint64_t> queue_dropped{0};       // 内部队列丢帧

    // RTT（ms，EWMA），UDP/RTP 链路 ping/pong 测得
    std::atomic<double> rtt_ms{0};

    // 接收抖动（ms，RFC3550 风格估计）
    std::atomic<double> jitter_ms{0};

    void add_e2e_ms(double ms) {
        std::lock_guard<std::mutex> lk(lat_mtx_);
        latencies_[lat_head_] = ms;
        lat_head_ = (lat_head_ + 1) % kLatCapacity;
        if (lat_count_ < kLatCapacity) lat_count_++;
    }

    // ---- 快照 ----
    struct LatencyStats {
        double p50 = 0, p95 = 0, p99 = 0, max = 0;
        size_t samples = 0;
    };

    LatencyStats latency() const {
        std::vector<double> v;
        {
            std::lock_guard<std::mutex> lk(lat_mtx_);
            v.assign(latencies_, latencies_ + lat_count_);
        }
        LatencyStats s;
        s.samples = v.size();
        if (v.empty()) return s;
        s.p50 = [&] {
            auto w = v; size_t i = w.size() / 2;
            std::nth_element(w.begin(), w.begin() + i, w.end());
            return w[i];
        }();
        s.p95 = [&] {
            auto w = v; size_t i = static_cast<size_t>(0.95 * (w.size() - 1));
            std::nth_element(w.begin(), w.begin() + i, w.end());
            return w[i];
        }();
        s.p99 = [&] {
            auto w = v; size_t i = static_cast<size_t>(0.99 * (w.size() - 1));
            std::nth_element(w.begin(), w.begin() + i, w.end());
            return w[i];
        }();
        s.max = *std::max_element(v.begin(), v.end());
        return s;
    }

    std::string to_json() const {
        auto l = latency();
        nlohmann::json j = {
            {"capture_frames",   capture_frames.load()},
            {"encoded_frames",   encoded_frames.load()},
            {"net_packets_sent", net_packets_sent.load()},
            {"net_bytes_sent",   net_bytes_sent.load()},
            {"net_fec_sent",     net_fec_sent.load()},
            {"sim_dropped",      sim_dropped.load()},
            {"nack_received",    nack_received.load()},
            {"retransmit_sent",  retransmit_sent.load()},
            {"net_packets_recv", net_packets_recv.load()},
            {"net_bytes_recv",   net_bytes_recv.load()},
            {"net_lost",         net_lost.load()},
            {"fec_recovered",    fec_recovered.load()},
            {"nack_requested",   nack_requested.load()},
            {"nack_hit",         nack_hit.load()},
            {"decoded_frames",   decoded_frames.load()},
            {"rendered_frames",  rendered_frames.load()},
            {"queue_dropped",    queue_dropped.load()},
            {"rtt_ms",           rtt_ms.load()},
            {"jitter_ms",        jitter_ms.load()},
            {"lat_p50",          l.p50},
            {"lat_p95",          l.p95},
            {"lat_p99",          l.p99},
            {"lat_max",          l.max},
            {"lat_samples",      l.samples},
        };
        return j.dump();
    }

private:
    static constexpr size_t kLatCapacity = 1024;
    mutable std::mutex lat_mtx_;
    double latencies_[kLatCapacity] = {0};
    size_t lat_head_ = 0;
    size_t lat_count_ = 0;
};

} // namespace rtstream
