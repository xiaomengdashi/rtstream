#pragma once

// 程序化测试源：生成 720p 动态画面（滚动的彩条 + 秒表/运动方块），
// 输出 I420，保证无摄像头环境也能演示端到端链路与弱网指标。

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

#include "capture_source.h"
#include "common/logger.h"
#include "common/timer.h"

namespace rtstream {

class TestSource : public CaptureSource {
public:
    TestSource() = default;
    ~TestSource() override { stop(); }

    bool start(const CaptureConfig& cfg, std::string& error) override {
        cfg_ = cfg;
        // x264 要求宽高为偶数；测试源直接取整，保证下游可用
        cfg_.width = std::max(64u, cfg_.width & ~1u);
        cfg_.height = std::max(64u, cfg_.height & ~1u);
        if (cfg_.width < 64 || cfg_.height < 64) {
            error = "invalid resolution";
            return false;
        }
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        RTS_LOGI("testsrc", "test source started: %ux%u@%u", cfg_.width, cfg_.height, cfg_.fps);
        return true;
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }

    const char* name() const override { return "test"; }

private:
    void loop() {
        const uint32_t w = cfg_.width, h = cfg_.height;
        const uint32_t ysz = static_cast<uint64_t>(w) * h;
        const uint32_t usz = ysz / 4;
        const auto interval_us = 1000000 / (cfg_.fps ? cfg_.fps : 30);

        int64_t next_t = now_us();
        int64_t fid = 0;
        while (running_) {
            // 先睡到节拍点再生成帧，保证 capture_us 贴近真实出帧时刻
            int64_t wait = next_t - now_us();
            if (wait > 0)
                std::this_thread::sleep_for(std::chrono::microseconds(wait));
            else if (wait < -interval_us)
                next_t = now_us();  // 落后过多则重置节拍
            next_t += interval_us;

            auto frame = make_frame();
            frame->width = w;
            frame->height = h;
            frame->fmt = PixelFormat::I420;
            frame->frame_id = ++fid;
            frame->capture_us = now_us();
            frame->wall_us = wall_us();
            frame->data.resize(ysz + usz * 2);
            uint8_t* Y = frame->data.data();
            uint8_t* U = Y + ysz;
            uint8_t* V = U + usz;

            double t = frame->capture_us / 1e6;

            // 彩条：8 个竖条随时间水平滚动
            const int bars = 8;
            const int shift = static_cast<int>(std::fmod(t * 60.0, static_cast<double>(w)));
            static const uint8_t bar_colors[bars][3] = {
                {235,128,128},{235,128,240},{160,166,128},{160,166,240},
                {107,52,128},{107,52,240},{41,240,110},{168,44,6}};

            // 运动方块参数（叠在彩条上，用于肉眼观察卡顿/丢帧）
            const int box = static_cast<int>(h) / 6;
            int bx = static_cast<int>(w / 2 + (w / 4) * std::sin(t * 2.0));
            int by = static_cast<int>(h / 2 + (h / 4) * std::sin(t * 3.1));

            for (uint32_t y = 0; y < h; ++y) {
                uint8_t* yrow = Y + y * w;
                for (uint32_t x = 0; x < w; ++x) {
                    int xx = static_cast<int>((x + shift) % w);
                    int bi = std::min(bars - 1, xx * bars / static_cast<int>(w));
                    uint8_t yv = bar_colors[bi][0];
                    int xi = static_cast<int>(x), yi = static_cast<int>(y);
                    if (xi >= bx && xi < bx + box && yi >= by && yi < by + box)
                        yv = 235;
                    yrow[x] = yv;
                }
            }
            for (uint32_t y = 0; y < h / 2; ++y) {
                for (uint32_t x = 0; x < w / 2; ++x) {
                    int xx = static_cast<int>(((x * 2) + shift) % w);
                    int bi = std::min(bars - 1, xx * bars / static_cast<int>(w));
                    U[y * (w / 2) + x] = bar_colors[bi][1];
                    V[y * (w / 2) + x] = bar_colors[bi][2];
                }
            }

            // 秒数字幕：用亮度图案在左上角画 3x5 点阵数字
            draw_second_digits(Y, w, h, static_cast<int>(std::fmod(t, 60.0)));

            if (cb_) cb_(frame);
        }
    }

    // 3x5 点阵数字，显示"SS.s"，占左上角区域
    void draw_second_digits(uint8_t* Y, uint32_t w, uint32_t, int seconds) {
        static const uint8_t font[10][5] = {
            {0b111,0b101,0b101,0b101,0b111}, // 0
            {0b010,0b110,0b010,0b010,0b111}, // 1
            {0b111,0b001,0b111,0b100,0b111}, // 2
            {0b111,0b001,0b111,0b001,0b111}, // 3
            {0b101,0b101,0b111,0b001,0b001}, // 4
            {0b111,0b100,0b111,0b001,0b111}, // 5
            {0b111,0b100,0b111,0b101,0b111}, // 6
            {0b111,0b001,0b010,0b010,0b010}, // 7
            {0b111,0b101,0b111,0b101,0b111}, // 8
            {0b111,0b101,0b111,0b001,0b111}, // 9
        };
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%02d", seconds % 100);
        const int scale = 4;
        int ox = 8, oy = 8;
        for (int ci = 0; ci < 2; ++ci) {
            int d = buf[ci] - '0';
            if (d < 0 || d > 9) continue;
            for (int r = 0; r < 5; ++r)
                for (int c = 0; c < 3; ++c) {
                    if (!(font[d][r] & (0b100 >> c))) continue;
                    for (int sy = 0; sy < scale; ++sy)
                        for (int sx = 0; sx < scale; ++sx) {
                            uint32_t px = ox + ci * 4 * scale + c * scale + sx;
                            uint32_t py = oy + r * scale + sy;
                            Y[py * w + px] = 16; // 深色
                        }
                }
        }
    }

    CaptureConfig cfg_;
    CaptureCallback cb_;
    std::atomic<bool> running_{false};
    std::thread thread_;

public:
    // 设置帧回调（start 前调用）
    void set_callback(CaptureCallback cb) { cb_ = std::move(cb); }
};

} // namespace rtstream
