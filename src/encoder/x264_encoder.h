#pragma once

// x264 编码封装：I420 输入 → H.264 Annex-B 输出。
// 零延迟配置（tune=zerolatency + sliced-threads off），保证最低端到端延时；
// CBR/VBR 可配，每帧输出 NALU 边界列表供 RTP 打包使用。

#include <cstdint>
#include <string>
#include <vector>

#include <x264.h>

#include "common/media_types.h"

namespace rtstream {

struct EncoderConfig {
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 30;
    uint32_t bitrate_kbps = 4000;
    // x264 公共 API 只有 CRF/CQP/ABR 三种方法：
    // CBR 用 ABR + 1 帧 VBV + b_filler 实现，VBR 用纯 ABR
    enum class RcMode : int { CRF = 0, CBR = 1, VBR = 2 };
    RcMode rc_mode = RcMode::CRF;
    int crf = 23;
    int gop = 60;               // IDR 间隔
    bool zerolatency = true;
};

class X264Encoder {
public:
    ~X264Encoder();

    bool open(const EncoderConfig& cfg, std::string& error);
    void close();
    bool is_open() const { return enc_ != nullptr; }

    // 编码一帧 I420；输出追加到 out（可复用调用方 vector）
    bool encode(const Frame& in, EncodedFramePtr& out);

private:
    EncoderConfig cfg_;
    x264_t* enc_ = nullptr;
    x264_picture_t pic_in_ {};
    x264_picture_t pic_out_ {};
    int64_t pts_ = 0;
    std::vector<uint8_t> seq_hdr_;  // SPS/PPS（用于 UDP/RTP 客户端入会即解码）
};

} // namespace rtstream
