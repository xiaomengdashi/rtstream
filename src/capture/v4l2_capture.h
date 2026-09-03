#pragma once

// V4L2 采集（仅 Linux 编译）：MJPEG 优先，YUYV 回退。
// 流程：open → querycap → 枚举格式 → S_FMT → REQBUFS → mmap →
//       QBUF × N → STREAMON → select 等帧 → DQBUF → 回调 → QBUF。
// 采集线程内不做任何拷贝以外的工作；失败时通过 error 通道上报。

#if defined(__linux__)

#include <atomic>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <unistd.h>

#include "capture_source.h"
#include "common/logger.h"
#include "common/net_utils.h"
#include "common/timer.h"

namespace rtstream {

class V4l2Capture : public CaptureSource {
public:
    V4l2Capture() = default;
    ~V4l2Capture() override { stop(); }

    void set_callback(CaptureCallback cb) { cb_ = std::move(cb); }

    bool start(const CaptureConfig& cfg, std::string& error) override {
        if (!open_device(cfg, error)) return false;
        if (!init_buffers(cfg, error)) { close_device(); return false; }
        if (!start_streaming(error)) { close_device(); return false; }
        cfg_ = cfg;
        running_ = true;
        thread_ = std::thread([this] { loop(); });
        RTS_LOGI("v4l2", "capture started: %s %ux%u@%u", cfg_.device.c_str(),
                 cfg_.width, cfg_.height, cfg_.fps);
        return true;
    }

    void stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        close_device();
    }

    const char* name() const override { return "v4l2"; }

private:
    struct Buffer {
        void* start = nullptr;
        size_t length = 0;
    };

    static int xioctl(int fd, unsigned long req, void* arg) {
        int r;
        do { r = ::ioctl(fd, req, arg); } while (r < 0 && errno == EINTR);
        return r;
    }

    bool open_device(const CaptureConfig& cfg, std::string& error) {
        fd_ = ::open(cfg.device.c_str(), O_RDWR | O_NONBLOCK, 0);
        if (fd_ < 0) {
            error = "open " + cfg.device + ": " + errno_str();
            return false;
        }
        v4l2_capability cap;
        if (xioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
            error = "VIDIOC_QUERYCAP: " + errno_str();
            return false;
        }
        if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
            error = "not a capture device";
            return false;
        }
        if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
            error = "streaming IO not supported";
            return false;
        }
        return true;
    }

    bool try_format(uint32_t pixfmt, const CaptureConfig& cfg, v4l2_format& fmt) {
        std::memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        fmt.fmt.pix.width = cfg.width;
        fmt.fmt.pix.height = cfg.height;
        fmt.fmt.pix.pixelformat = pixfmt;
        fmt.fmt.pix.field = V4L2_FIELD_NONE;
        return xioctl(fd_, VIDIOC_S_FMT, &fmt) >= 0 &&
               fmt.fmt.pix.pixelformat == pixfmt;
    }

    bool init_buffers(const CaptureConfig& cfg, std::string& error) {
        v4l2_format fmt;
        if (try_format(V4L2_PIX_FMT_MJPEG, cfg, fmt)) {
            fmt_ = PixelFormat::MJPEG;
        } else if (try_format(V4L2_PIX_FMT_YUYV, cfg, fmt)) {
            fmt_ = PixelFormat::YUYV;
        } else {
            error = "neither MJPEG nor YUYV supported";
            return false;
        }
        width_ = fmt.fmt.pix.width;
        height_ = fmt.fmt.pix.height;

        v4l2_requestbuffers req;
        std::memset(&req, 0, sizeof(req));
        req.count = kBufferCount;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd_, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
            error = "VIDIOC_REQBUFS: " + errno_str();
            return false;
        }
        buffers_.resize(req.count);
        for (uint32_t i = 0; i < req.count; ++i) {
            v4l2_buffer buf;
            std::memset(&buf, 0, sizeof(buf));
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (xioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                error = "VIDIOC_QUERYBUF: " + errno_str();
                return false;
            }
            buffers_[i].length = buf.length;
            buffers_[i].start = ::mmap(nullptr, buf.length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, fd_, buf.m.offset);
            if (buffers_[i].start == MAP_FAILED) {
                error = "mmap: " + errno_str();
                return false;
            }
        }
        return true;
    }

    bool start_streaming(std::string& error) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        for (uint32_t i = 0; i < buffers_.size(); ++i) {
            v4l2_buffer buf;
            std::memset(&buf, 0, sizeof(buf));
            buf.type = type;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            if (xioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
                error = "VIDIOC_QBUF: " + errno_str();
                return false;
            }
        }
        if (xioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
            error = "VIDIOC_STREAMON: " + errno_str();
            return false;
        }
        return true;
    }

    void loop() {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        int64_t fid = 0;
        while (running_) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(fd_, &fds);
            timeval tv{1, 0};
            int r = ::select(fd_ + 1, &fds, nullptr, nullptr, &tv);
            if (r < 0) {
                if (errno == EINTR) continue;
                RTS_LOGE("v4l2", "select: %s", errno_str().c_str());
                break;
            }
            if (r == 0) continue;  // 超时继续等

            v4l2_buffer buf;
            std::memset(&buf, 0, sizeof(buf));
            buf.type = type;
            buf.memory = V4L2_MEMORY_MMAP;
            if (xioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
                if (errno == EAGAIN) continue;
                RTS_LOGE("v4l2", "DQBUF: %s", errno_str().c_str());
                break;
            }

            if (cb_ && buf.bytesused > 0) {
                auto frame = make_frame();
                frame->width = width_;
                frame->height = height_;
                frame->fmt = fmt_;
                frame->frame_id = ++fid;
                frame->capture_us = now_us();
                frame->wall_us = wall_us();
                frame->data.assign(static_cast<uint8_t*>(buffers_[buf.index].start),
                                   static_cast<uint8_t*>(buffers_[buf.index].start) + buf.bytesused);
                cb_(frame);
            }

            xioctl(fd_, VIDIOC_QBUF, &buf);  // 归还缓冲
        }
        if (running_) running_ = false;
        xioctl(fd_, VIDIOC_STREAMOFF, &type);
    }

    void close_device() {
        for (auto& b : buffers_) {
            if (b.start) ::munmap(b.start, b.length);
        }
        buffers_.clear();
        close_fd(fd_);
    }

    static constexpr uint32_t kBufferCount = 4;

    CaptureConfig cfg_;
    CaptureCallback cb_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int fd_ = -1;
    PixelFormat fmt_ = PixelFormat::None;
    uint32_t width_ = 0, height_ = 0;
    std::vector<Buffer> buffers_;
};

} // namespace rtstream

#endif // __linux__
