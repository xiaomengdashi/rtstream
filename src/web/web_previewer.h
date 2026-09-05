#pragma once

// 预览/指标推流器：消费采集帧 tap → JPEG → WebSocket 广播；1Hz 推指标 JSON。
// 二进制 WS 帧布局（大端）：
//   magic 'RTVJ'(4) | wall_ms double(8) | width u32(4) | height u32(4) | jpeg...
// wall_ms 为服务端墙钟毫秒（同机回环时浏览器 Date.now() 可与之对照测 E2E 延时）。

#include <atomic>
#include <condition_variable>
#include <thread>

#include "common/logger.h"
#include "common/timer.h"
#include "common/thread_safe_queue.h"
#include "stats/metrics.h"
#include "web/jpeg_encoder.h"
#include "web/ws_server.h"

namespace rtstream {

// HTTP 服务端与预览器共享的"最新 JPEG"槽（MJPEG 通道用）
struct SharedJpeg {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> data;
    uint32_t width = 0, height = 0;
    int64_t wall_us = 0;
    uint64_t version = 0;
};

class WebPreviewer {
public:
    explicit WebPreviewer(SharedJpeg& sink) : sink_(sink) {}
    ~WebPreviewer() { stop(); }

    void set_ws(WsServer* ws) { ws_ = ws; }

    // tap：采集侧投递的原始帧队列（低容量，满则丢旧）
    bool start(ThreadSafeQueue<FramePtr>& tap, uint32_t width, uint32_t height,
               std::string& error) {
        if (width < 64 || height < 64) {
            error = "invalid preview size";
            return false;
        }
        uint32_t out_h = 640 * height / width;
        if (!enc_.open(width, height, 640, out_h, 85, error))
            return false;
        tap_ = &tap;
        running_ = true;
        thread_ = std::thread([this] { preview_loop(); });
        stats_thread_ = std::thread([this] { stats_loop(); });
        RTS_LOGI("preview", "previewer started (%ux%u -> 640x%u)", width, height, out_h);
        return true;
    }

    void stop() {
        running_ = false;
        cv_stats_.notify_all();
        if (thread_.joinable()) thread_.join();
        if (stats_thread_.joinable()) stats_thread_.join();
    }

private:
    void preview_loop() {
        FramePtr f;
        std::vector<uint8_t> jpeg;
        while (running_) {
            if (!tap_->pop_for(f, std::chrono::milliseconds(50))) continue;
            if (!enc_.encode(*f, jpeg)) continue;

            // 共享槽（MJPEG）
            {
                std::lock_guard<std::mutex> lk(sink_.mtx);
                sink_.data.assign(jpeg.begin(), jpeg.end());
                sink_.width = 640;
                sink_.height = 640 * f->height / f->width;
                sink_.wall_us = f->wall_us;
                sink_.version++;
            }
            sink_.cv.notify_all();

            // WS 二进制：头 + jpeg
            if (ws_) {
                std::vector<uint8_t> pkt(20 + jpeg.size());
                pkt[0] = 'R'; pkt[1] = 'T'; pkt[2] = 'V'; pkt[3] = 'J';
                double wall_ms = static_cast<double>(f->wall_us) / 1000.0;
                uint64_t wbits;
                std::memcpy(&wbits, &wall_ms, 8);
                for (int i = 0; i < 8; ++i)
                    pkt[4 + i] = static_cast<uint8_t>(wbits >> ((7 - i) * 8));
                put_be32(pkt.data() + 12, 640);
                put_be32(pkt.data() + 16, 640 * f->height / f->width);
                std::memcpy(pkt.data() + 20, jpeg.data(), jpeg.size());
                ws_->broadcast_binary(pkt.data(), pkt.size());
            }
            Metrics::instance().rendered_frames++;
        }
    }

    void stats_loop() {
        while (running_) {
            {
                std::unique_lock<std::mutex> lk(stats_mtx_);
                cv_stats_.wait_for(lk, std::chrono::milliseconds(1000), [this] { return !running_; });
            }
            if (running_ && ws_) ws_->broadcast_text(Metrics::instance().to_json());
        }
    }

    static void put_be32(uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v >> 24);
        p[1] = static_cast<uint8_t>(v >> 16);
        p[2] = static_cast<uint8_t>(v >> 8);
        p[3] = static_cast<uint8_t>(v);
    }

    SharedJpeg& sink_;
    WsServer* ws_ = nullptr;
    ThreadSafeQueue<FramePtr>* tap_ = nullptr;
    JpegEncoder enc_;
    std::atomic<bool> running_{false};
    std::mutex stats_mtx_;
    std::condition_variable cv_stats_;
    std::thread thread_, stats_thread_;
};

} // namespace rtstream
