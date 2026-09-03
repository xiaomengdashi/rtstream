#pragma once

// 有界阻塞队列：连接采集→编码→分发/解码→渲染各线程的线程安全队列。
// - 满时丢弃最旧元素并计数（实时流"追最新帧"策略，可配置为阻塞）
// - close() 后所有阻塞操作立即返回 false，用于优雅关停

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <utility>

namespace rtstream {

template <typename T>
class ThreadSafeQueue {
public:
    explicit ThreadSafeQueue(size_t capacity = 8, bool drop_old_when_full = true)
        : capacity_(capacity), drop_old_(drop_old_when_full) {}

    // 压入元素；队列已满时：drop_old=true 丢弃队头再压入（返回 true），
    // 否则阻塞等待。closed 后返回 false。
    bool push(T v) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_not_full_.wait(lk, [&] { return closed_ || drop_old_ || q_.size() < capacity_; });
        if (closed_) return false;
        if (q_.size() >= capacity_) {
            dropped_++;
            q_.pop_front();
        }
        q_.push_back(std::move(v));
        cv_not_empty_.notify_one();
        return true;
    }

    // 非阻塞尝试压入，失败返回 false
    bool try_push(T v) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (closed_) return false;
            if (q_.size() >= capacity_) {
                dropped_++;
                q_.pop_front();
            }
            q_.push_back(std::move(v));
        }
        cv_not_empty_.notify_one();
        return true;
    }

    // 取出队头；closed 且为空时返回 false（关停哨兵语义）
    bool pop(T& out) {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_not_empty_.wait(lk, [&] { return closed_ || !q_.empty(); });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        cv_not_full_.notify_one();
        return true;
    }

    // 带超时 pop（毫秒），超时返回 false
    template <typename Rep, typename Period>
    bool pop_for(T& out, const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (!cv_not_empty_.wait_for(lk, timeout, [&] { return closed_ || !q_.empty(); }))
            return false;
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop_front();
        cv_not_full_.notify_one();
        return true;
    }

    void close() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            closed_ = true;
        }
        cv_not_empty_.notify_all();
        cv_not_full_.notify_all();
    }

    bool closed() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return closed_;
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.size();
    }

    uint64_t dropped() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return dropped_;
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_not_empty_;
    std::condition_variable cv_not_full_;
    std::deque<T> q_;
    size_t capacity_;
    bool drop_old_;
    bool closed_ = false;
    uint64_t dropped_ = 0;
};

} // namespace rtstream
