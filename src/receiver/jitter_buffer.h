#pragma once

// Jitter Buffer（模板类，上下文类型参数化）：
// - 按 seq 重排乱序到达的单元（RTP 链路=包级，UDP 链路=帧级）
// - 检测缺口 → on_missing 回调（内部按 seq 节流，默认 10ms）触发 NACK
// - 自适应目标去抖深度：target = clamp(3*jitter + rtt, min, max)
// - 出队策略：
//     a) 头部就绪且已等待 >= target → 出队
//     b) 头部缺失超过 max_wait → 跳过缺口（计丢包）继续
//     c) 下游饥饿（上次出队距今 > target）→ 立即出队，压低延迟
// - 抖动估计：RFC 3550 风格，D(i) = (到达间隔) - (产生间隔)，EWMA

#include <functional>
#include <map>
#include <memory>
#include <mutex>

#include "common/timer.h"
#include "stats/metrics.h"

namespace rtstream {

template <typename Ctx>
class JitterBuffer {
public:
    struct Unit {
        uint32_t seq = 0;
        int64_t gen_us = 0;          // 产生时间（RTP timestamp 还原 / 帧号对应 capture_us）
        int64_t first_recv_us = 0;
        std::shared_ptr<Ctx> data;
    };

    using MissFn = std::function<void(uint32_t seq)>;

    explicit JitterBuffer(size_t max_units = 512)
        : max_units_(max_units) {
        target_delay_us_ = 40 * 1000;
    }

    void set_on_missing(MissFn fn) { on_missing_ = std::move(fn); }
    void set_nack_interval_us(int64_t us) { nack_interval_us_ = us; }
    void set_rtt_ms(double ms) { rtt_ms_ = ms; retune_target(); }

    // 乱序安全插入；过旧（已被消费越过）返回 false
    bool push(uint32_t seq, int64_t gen_us, std::shared_ptr<Ctx> data) {
        std::lock_guard<std::mutex> lk(mtx_);
        int64_t now = now_us();
        if (started_ && seq_lt(seq, next_out_)) return false;

        if (!started_) {
            started_ = true;
            next_out_ = seq;
            first_arrival_us_ = now;
        }
        // 抖动估计（RFC 3550 简化）只在"按序到达"时更新：
        // 乱序/重传到达（NACK 恢复）会人为抬高 D，污染自适应深度
        bool in_order = (seq == next_out_);
        if (in_order && last_arrival_us_ != 0) {
            int64_t d_arr = now - last_arrival_us_;
            int64_t d_gen = gen_us - last_gen_us_;
            if (d_gen > 0 && d_arr >= 0) {
                double D = static_cast<double>(d_arr - d_gen);
                if (D < 0) D = -D;
                if (D > 200 * 1000.0) D = 200 * 1000.0;   // 安全上限 200ms
                jitter_us_ += (D - jitter_us_) / 16.0;    // EWMA 1/16
                Metrics::instance().jitter_ms = jitter_us_ / 1000.0;
                retune_target_locked();
            }
        }
        if (in_order) {
            last_arrival_us_ = now;
            last_gen_us_ = gen_us;
        }

        auto& slot = buf_[seq];
        if (!slot.data) {   // 新单元；重复到达（重传后）不覆盖
            slot.seq = seq;
            slot.gen_us = gen_us;
            slot.first_recv_us = now;
            slot.data = std::move(data);
            total_buffered_++;
        }
        detect_missing_locked(seq, now);
        evict_overflow_locked();
        return true;
    }

    // 尝试出队一个有序单元
    bool pop(Unit& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (!started_ || buf_.empty()) return false;
        int64_t now = now_us();
        bool starved = (now - last_pop_us_) > target_delay_us_;

        auto it = buf_.find(next_out_);
        if (it == buf_.end()) {
            // 头部缺失：从"首次发现缺口"起等待，超过 max_wait 则跳过
            if (head_missing_since_us_ == 0) head_missing_since_us_ = now;
            if (now - head_missing_since_us_ < max_wait_us_ && !starved) {
                if (on_missing_) {
                    MissFn fn_copy;
                    {
                        std::lock_guard<std::mutex> cb_lk(cb_mtx_);
                        fn_copy = on_missing_;
                    }
                    fn_copy(next_out_);
                }
                Metrics::instance().nack_requested++;
                return false;
            }
            // 跳过缺口
            auto first = buf_.begin();
            uint32_t head = first->first;
            Metrics::instance().net_lost += seq_delta(head, next_out_);
            next_out_ = head;
            head_missing_since_us_ = 0;
            it = first;
        } else {
            head_missing_since_us_ = 0;
            int64_t held_us = now - it->second.first_recv_us;
            if (held_us < target_delay_us_ && !starved) return false;
        }

        out = it->second;
        buf_.erase(it);
        next_out_ = out.seq + 1;
        last_pop_us_ = now;
        total_buffered_--;
        return true;
    }

    double jitter_ms() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return jitter_us_ / 1000.0;
    }

    double target_delay_ms() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return target_delay_us_ / 1000.0;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return buf_.size();
    }

private:
    static bool seq_lt(uint32_t a, uint32_t b) {
        return static_cast<int32_t>(a - b) < 0;   // 回绕安全比较
    }
    static uint32_t seq_delta(uint32_t a, uint32_t b) {
        return a - b;  // b→a 前进量
    }

    void retune_target() {
        std::lock_guard<std::mutex> lk(mtx_);
        retune_target_locked();
    }

    void retune_target_locked() {
        int64_t target = static_cast<int64_t>(3 * jitter_us_ + rtt_ms_ * 1000.0);
        target = std::max<int64_t>(target, min_delay_us_);
        target = std::min<int64_t>(target, max_delay_us_);
        target_delay_us_ = target;
        max_wait_us_ = 2 * target_delay_us_ + 50 * 1000;
    }

    // 缺口检测：next_out_..arrived_seq-1 视为缺失（仅对新增的"前沿"缺口触发）
    void detect_missing_locked(uint32_t arrived_seq, int64_t now) {
        if (seq_lt(arrived_seq, next_out_) || arrived_seq == next_out_) return;
        uint32_t gap = seq_delta(arrived_seq, next_out_);
        if (gap > 1024) {
            // 序号大跳（新会话/严重中断）：直接对齐，避免 NACK 风暴
            next_out_ = arrived_seq;
            first_arrival_us_ = now;
            return;
        }
        for (uint32_t s = next_out_; s != arrived_seq; ++s) {
            if (buf_.find(s) == buf_.end()) maybe_nack_locked(s, now);
        }
    }

    void maybe_nack_locked(uint32_t seq, int64_t now) {
        auto it = last_nack_us_.find(seq);
        if (it != last_nack_us_.end() && now - it->second < nack_interval_us_) return;
        last_nack_us_[seq] = now;
        // 清理过旧记录，防膨胀
        if (last_nack_us_.size() > 4096) last_nack_us_.clear();
        if (on_missing_) {
            MissFn fn_copy;
            {
                std::lock_guard<std::mutex> cb_lk(cb_mtx_);
                fn_copy = on_missing_;
            }
            fn_copy(seq);   // 回调不得重入本对象
        }
        Metrics::instance().nack_requested++;
    }

    void evict_overflow_locked() {
        while (buf_.size() > max_units_) {
            buf_.erase(buf_.begin());   // 丢最老
            Metrics::instance().net_lost++;
        }
    }

    mutable std::mutex mtx_;
    std::mutex cb_mtx_;   // 回调不持 mtx_ 调用（防死锁）
    std::map<uint32_t, Unit> buf_;
    uint32_t next_out_ = 0;
    bool started_ = false;
    int64_t head_missing_since_us_ = 0;
    int64_t first_arrival_us_ = 0;
    int64_t last_arrival_us_ = 0;
    int64_t last_gen_us_ = 0;
    int64_t last_pop_us_ = 0;
    double jitter_us_ = 0;
    double rtt_ms_ = 0;
    int64_t target_delay_us_ = 40 * 1000;
    int64_t max_wait_us_ = 130 * 1000;
    int64_t min_delay_us_ = 15 * 1000;
    int64_t max_delay_us_ = 250 * 1000;
    int64_t nack_interval_us_ = 10 * 1000;
    std::map<uint32_t, int64_t> last_nack_us_;
    MissFn on_missing_;
    size_t max_units_;
    size_t total_buffered_ = 0;
};

} // namespace rtstream
