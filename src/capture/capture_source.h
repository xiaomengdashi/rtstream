#pragma once

// 采集源抽象接口：V4L2 与程序化测试源共用。
// start() 启动内部采集线程，帧经回调推送（调用方保证回调线程安全）。

#include <functional>

#include "common/media_types.h"

namespace rtstream {

struct CaptureConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 30;
    std::string device = "/dev/video0";  // 仅 V4L2 用
};

class CaptureSource {
public:
    virtual ~CaptureSource() = default;

    // 打开设备并启动采集线程；失败返回 false 并填充 error
    virtual bool start(const CaptureConfig& cfg, std::string& error) = 0;
    virtual void stop() = 0;
    virtual const char* name() const = 0;
};

using CaptureCallback = std::function<void(FramePtr)>;

} // namespace rtstream
