#pragma once

// 采集源工厂：按平台与配置创建 V4L2 / AVFoundation / 测试源。
// source = "v4l2" 强制 V4L2（仅 Linux，失败即报错）
// source = "avf"  强制 AVFoundation（仅 macOS，失败即报错）
// source = "test" 强制测试源
// source = "auto" 优先真实摄像头（Linux→V4L2，macOS→AVF），失败回落测试源

#include <memory>

#include "capture_source.h"

#if defined(__linux__)
#include "v4l2_capture.h"
#endif
#if defined(__APPLE__) && defined(RTSTREAM_HAVE_AVF)
#include "avf_capture.h"
#endif
#include "test_source.h"

namespace rtstream {

inline std::unique_ptr<CaptureSource> create_capture(const std::string& source,
                                                     const CaptureConfig& cfg,
                                                     CaptureCallback cb,
                                                     std::string& error) {
#if defined(__linux__)
    if (source == "avf") {
        error = "avf requires macOS";
        return nullptr;
    }
    if (source == "v4l2" || source == "auto") {
        auto v = std::make_unique<V4l2Capture>();
        v->set_callback(cb);   // 拷贝：失败回落测试源时回调仍有效
        if (v->start(cfg, error)) return std::unique_ptr<CaptureSource>(v.release());
        RTS_LOGW("capture", "v4l2 failed: %s", error.c_str());
        if (source == "v4l2") return nullptr;
        RTS_LOGW("capture", "falling back to test source");
    }
#elif defined(__APPLE__)
    if (source == "v4l2") {
        error = "v4l2 requires Linux";
        return nullptr;
    }
#if defined(RTSTREAM_HAVE_AVF)
    if (source == "avf" || source == "auto") {
        auto a = std::make_unique<AvfCapture>();
        a->set_callback(cb);
        if (a->start(cfg, error)) return std::unique_ptr<CaptureSource>(a.release());
        RTS_LOGW("capture", "avfoundation failed: %s", error.c_str());
        if (source == "avf") return nullptr;
        RTS_LOGW("capture", "falling back to test source");
    }
#else
    if (source == "avf") {
        error = "avf requires RTSTREAM_ENABLE_AVF=ON (Apple platform build)";
        return nullptr;
    }
#endif
#else
    (void)cfg;
    if (source == "v4l2") {
        error = "v4l2 requires Linux";
        return nullptr;
    }
    if (source == "avf") {
        error = "avf requires macOS";
        return nullptr;
    }
#endif
    auto t = std::make_unique<TestSource>();
    t->set_callback(std::move(cb));
    if (!t->start(cfg, error)) return nullptr;
    return std::unique_ptr<CaptureSource>(t.release());
}

} // namespace rtstream
