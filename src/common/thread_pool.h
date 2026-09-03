#pragma once

// 固定大小线程池：任务队列 + N 工作线程。服务端仅用于偶发的
// 并行工作（如 JPEG 预览编码），核心流水线不经过线程池以保证低延迟。

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace rtstream {

class ThreadPool {
public:
    explicit ThreadPool(size_t n_threads) {
        for (size_t i = 0; i < n_threads; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() { shutdown(); }

    void submit(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stopped_) return;
            tasks_.push(std::move(task));
        }
        cv_.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (stopped_) return;
            stopped_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_) {
            if (t.joinable()) t.join();
        }
        workers_.clear();
    }

private:
    void worker_loop() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                cv_.wait(lk, [&] { return stopped_ || !tasks_.empty(); });
                if (stopped_ && tasks_.empty()) return;
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex mtx_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

} // namespace rtstream
