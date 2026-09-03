#pragma once

// 弱网模拟器：在 UDP/RTP 发送路径注入丢包与延迟/抖动。
// 仅用于验证 FEC/NACK/Jitter Buffer 的效果，生产参数 loss=0 delay=0 即旁路。
// 延迟实现：延迟包进入 pending_ 堆，由 drain 线程到期后发出。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <random>
#include <thread>
#include <vector>

#include "common/logger.h"
#include "common/timer.h"
#include "stats/metrics.h"

namespace rtstream {

class LossSimulator {
public:
    using SendFn = std::function<void(const uint8_t*, size_t)>;

    LossSimulator(double loss_rate, uint32_t delay_ms, uint32_t jitter_ms)
        : loss_(loss_rate), delay_(delay_ms), jitter_(jitter_ms) {}

    void set_sender(SendFn fn) { send_ = std::move(fn); }

    void start() {
        if (delay_ == 0 && jitter_ == 0) return;  // 无延迟：同步直发
        running_ = true;
        worker_ = std::thread([this] { drain_loop(); });
    }

    void stop() {
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }

    // 返回 true=已发出（可能延迟），false=被模拟丢包
    bool inject(const uint8_t* data, size_t len) {
        if (loss_ > 0) {
            thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_real_distribution<double> uni(0.0, 1.0);
            if (uni(rng) < loss_) {
                Metrics::instance().sim_dropped++;
                return false;
            }
        }
        uint32_t d = delay_;
        if (jitter_ > 0) {
            thread_local std::mt19937 rng2{std::random_device{}()};
            std::uniform_int_distribution<uint32_t> j(0, jitter_);
            d += j(rng2);
        }
        if (d == 0) {
            if (send_) send_(data, len);
            return true;
        }
        {
            std::lock_guard<std::mutex> lk(mtx_);
            pending_.push({now_us() + static_cast<int64_t>(d) * 1000,
                           std::vector<uint8_t>(data, data + len)});
        }
        cv_.notify_one();
        return true;
    }

private:
    struct Pending {
        int64_t due_us;
        std::vector<uint8_t> data;
        bool operator<(const Pending& o) const { return due_us > o.due_us; } // 小顶堆
    };

    void drain_loop() {
        std::unique_lock<std::mutex> lk(mtx_);
        while (running_) {
            if (pending_.empty()) {
                cv_.wait(lk, [&] { return !running_ || !pending_.empty(); });
                continue;
            }
            int64_t now = now_us();
            if (pending_.top().due_us > now) {
                cv_.wait_for(lk, std::chrono::microseconds(pending_.top().due_us - now));
                continue;
            }
            auto pkt = pending_.top();
            pending_.pop();
            lk.unlock();
            if (send_) send_(pkt.data.data(), pkt.data.size());
            lk.lock();
        }
    }

    double loss_;
    uint32_t delay_;
    uint32_t jitter_;
    SendFn send_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::priority_queue<Pending> pending_;
    std::atomic<bool> running_{false};
    std::thread worker_;
};

} // namespace rtstream
