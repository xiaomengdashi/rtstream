// AVFoundation 采集实现（ObjC++，需 -fobjc-arc）：
// AVCaptureSession（640x480/720p/1080p 预设）→ VideoDataOutput(NV12)
// → 串行队列回调 → 锁定 CVPixelBuffer → stride 拷贝 + UV 解交织 → I420 帧回调。
// alwaysDiscardsLateVideoFrames=YES：实时流"追最新帧"策略，与发送端丢旧一致。

#include "avf_capture.h"

#if defined(__APPLE__)

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>

#include <atomic>

#include "common/logger.h"
#include "common/timer.h"
#include "common/media_types.h"

// ---------- ObjC 桥接 ----------

@interface AvfCaptureImpl : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property (strong) AVCaptureSession* session;
@property (strong) dispatch_queue_t videoQueue;
// C++ 闭包成员（ObjC++ 允许），帧回调出口
@property (nonatomic) rtstream::CaptureCallback onFrame;
- (BOOL)startWithWidth:(uint32_t)w height:(uint32_t)h fps:(uint32_t)fps error:(NSString**)err;
- (void)stopSession;
@end

@implementation AvfCaptureImpl

- (instancetype)init {
    if ((self = [super init])) {
        _session = [[AVCaptureSession alloc] init];
        _videoQueue = dispatch_queue_create("rtstream.avf.video", DISPATCH_QUEUE_SERIAL);
    }
    return self;
}

- (BOOL)startWithWidth:(uint32_t)w height:(uint32_t)h fps:(uint32_t)fps error:(NSString**)err {
    if (err) *err = nil;

    // TCC 相机授权：notDetermined 时发起系统弹窗并等待（弹窗归属宿主 App，
    // 如终端/iTerm；被拒或无 UI 宿主时给出可操作的错误信息）
    AVAuthorizationStatus auth =
        [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    if (auth == AVAuthorizationStatusNotDetermined) {
        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo
                                 completionHandler:^(BOOL __unused granted) {
            dispatch_semaphore_signal(sem);
        }];
        if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 30LL * NSEC_PER_SEC)) != 0) {
            if (err) *err = @"camera permission prompt timed out — 请在带 UI 的终端中运行一次以触发授权";
            return NO;
        }
        auth = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
    }
    if (auth != AVAuthorizationStatusAuthorized) {
        if (err) *err = @"camera permission denied — 请在 系统设置→隐私与安全性→相机 中放行宿主 App";
        return NO;
    }

    AVCaptureDevice* device = nil;
    AVCaptureDeviceDiscoverySession* ds = [AVCaptureDeviceDiscoverySession
        discoverySessionWithDeviceTypes:@[ AVCaptureDeviceTypeBuiltInWideAngleCamera ]
        mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified];
    device = ds.devices.firstObject;
    if (!device)  // 兼容旧 API 路径
        device = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
    if (!device) {
        if (err) *err = @"no camera device found";
        return NO;
    }

    // 会话预设按请求宽度就近选择；实际分辨率以首帧 CVPixelBuffer 为准
    NSString* preset = AVCaptureSessionPreset640x480;
    if (w >= 1920) preset = AVCaptureSessionPreset1920x1080;
    else if (w >= 1280) preset = AVCaptureSessionPreset1280x720;
    if ([self.session canSetSessionPreset:preset])
        self.session.sessionPreset = preset;

    NSError* e = nil;
    AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&e];
    if (!input) {
        if (err) *err = e.localizedDescription ?: @"camera input create failed (permission denied?)";
        return NO;
    }
    if (![self.session canAddInput:input]) {
        if (err) *err = @"cannot add capture input";
        return NO;
    }
    [self.session addInput:input];

    AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
    output.videoSettings = @{
        (id)kCVPixelBufferPixelFormatTypeKey :
            @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)  // NV12
    };
    output.alwaysDiscardsLateVideoFrames = YES;
    if (![self.session canAddOutput:output]) {
        if (err) *err = @"cannot add capture output";
        return NO;
    }
    [self.session addOutput:output];
    [output setSampleBufferDelegate:self queue:self.videoQueue];

    // 帧率锁定
    if ([device lockForConfiguration:&e]) {
        CMTime dur = CMTimeMake(1, fps ? fps : 30);
        device.activeVideoMinFrameDuration = dur;
        device.activeVideoMaxFrameDuration = dur;
        [device unlockForConfiguration];
    }

    [self.session startRunning];   // TCC 授权弹窗在此路径上触发
    return YES;
}

- (void)stopSession {
    [self.session stopRunning];
}

- (void)captureOutput:(AVCaptureOutput*)output
didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    rtstream::CaptureCallback cb = self.onFrame;
    if (!cb) return;

    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pb) return;
    if (CVPixelBufferGetPixelFormatType(pb) != kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange)
        return;

    static std::atomic<int64_t> s_fid{0};
    uint32_t w = (uint32_t)CVPixelBufferGetWidth(pb);
    uint32_t h = (uint32_t)CVPixelBufferGetHeight(pb);

    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    auto frame = rtstream::make_frame();
    frame->width = w;
    frame->height = h;
    frame->fmt = rtstream::PixelFormat::I420;
    frame->frame_id = ++s_fid;
    frame->capture_us = rtstream::now_us();
    frame->wall_us = rtstream::wall_us();
    frame->data.resize((size_t)w * h * 3 / 2);

    const uint8_t* ysrc = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 0);
    const uint8_t* uvsrc = (const uint8_t*)CVPixelBufferGetBaseAddressOfPlane(pb, 1);
    size_t ystride = CVPixelBufferGetBytesPerRowOfPlane(pb, 0);
    size_t uvstride = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
    uint8_t* ydst = frame->data.data();
    uint8_t* udst = ydst + (size_t)w * h;
    uint8_t* vdst = udst + (size_t)w * h / 4;

    // Y：按 stride 逐行拷贝（相机缓冲通常有行对齐填充）
    for (uint32_t r = 0; r < h; ++r)
        std::memcpy(ydst + (size_t)r * w, ysrc + r * ystride, w);
    // UV：NV12 解交织为 I420 的 U/V 平面
    for (uint32_t r = 0; r < h / 2; ++r) {
        const uint8_t* uv = uvsrc + (size_t)r * uvstride;
        uint8_t* u = udst + (size_t)r * (w / 2);
        uint8_t* v = vdst + (size_t)r * (w / 2);
        for (uint32_t c = 0; c < w / 2; ++c) {
            u[c] = uv[2 * c];
            v[c] = uv[2 * c + 1];
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);

    cb(frame);
}

@end

// ---------- C++ 封装 ----------

namespace rtstream {

AvfCapture::AvfCapture() = default;

AvfCapture::~AvfCapture() { stop(); }

bool AvfCapture::start(const CaptureConfig& cfg, std::string& error) {
    @autoreleasepool {
        AvfCaptureImpl* impl = [[AvfCaptureImpl alloc] init];
        CaptureCallback cb = cb_;
        impl.onFrame = [cb](FramePtr f) { cb(f); };   // 拷贝闭包进 block

        NSString* nserr = nil;
        if (![impl startWithWidth:cfg.width height:cfg.height fps:cfg.fps error:&nserr]) {
            error = nserr ? std::string(nserr.UTF8String) : std::string("avf start failed");
            return false;
        }
        impl_ = (__bridge_retained void*)impl;
        RTS_LOGI("avf", "capture started: %ux%u@%u (session preset by width)", cfg.width,
                 cfg.height, cfg.fps);
        return true;
    }
}

void AvfCapture::stop() {
    if (!impl_) return;
    @autoreleasepool {
        AvfCaptureImpl* impl = (__bridge_transfer AvfCaptureImpl*)impl_;
        impl.onFrame = nullptr;
        [impl stopSession];
        impl_ = nullptr;   // __bridge_transfer 已接管释放
    }
}

} // namespace rtstream

#endif // __APPLE__
