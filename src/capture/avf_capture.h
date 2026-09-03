#pragma once

// macOS AVFoundation 采集源：AVCaptureSession 采集 → NV12 → I420 转换。
// 接口与 V4L2/TestSource 一致（CaptureSource）；本头文件为纯 C++ 声明，
// ObjC 实现在 avf_capture.mm（需 -fobjc-arc，仅 Apple 平台编译）。
// 注意：首次运行需要用户授予"相机"权限（TCC），被拒时 start() 返回 false。

#if defined(__APPLE__)

#include "capture_source.h"

namespace rtstream {

class AvfCapture : public CaptureSource {
public:
    AvfCapture();
    ~AvfCapture() override;

    void set_callback(CaptureCallback cb) { cb_ = std::move(cb); }

    bool start(const CaptureConfig& cfg, std::string& error) override;
    void stop() override;
    const char* name() const override { return "avf"; }

private:
    void* impl_ = nullptr;   // ObjC 实现句柄（AvfCaptureImpl*），ARC bridge 管理
    CaptureCallback cb_;
};

} // namespace rtstream

#endif // __APPLE__
