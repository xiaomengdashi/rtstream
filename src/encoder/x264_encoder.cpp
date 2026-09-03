#include "x264_encoder.h"

#include "common/logger.h"

namespace rtstream {

X264Encoder::~X264Encoder() { close(); }

bool X264Encoder::open(const EncoderConfig& cfg, std::string& error) {
    if ((cfg.width & 1) || (cfg.height & 1)) {
        error = "width/height must be even (x264 requirement): " +
                std::to_string(cfg.width) + "x" + std::to_string(cfg.height);
        return false;
    }
    cfg_ = cfg;
    x264_param_t p;
    x264_param_default(&p);
    if (x264_param_default_preset(&p, "veryfast", cfg.zerolatency ? "zerolatency" : "medium") < 0) {
        error = "x264 preset failed";
        return false;
    }
    p.i_width = static_cast<int>(cfg.width);
    p.i_height = static_cast<int>(cfg.height);
    p.i_fps_num = cfg.fps;
    p.i_fps_den = 1;
    p.i_keyint_max = static_cast<int>(cfg.gop);
    p.i_keyint_min = static_cast<int>(cfg.gop / 2);
    p.i_bframe = 0;                       // 实时链路禁用 B 帧
    p.b_intra_refresh = 0;
    if (cfg.rc_mode == EncoderConfig::RcMode::CRF) {
        p.rc.i_rc_method = X264_RC_CRF;
        p.rc.f_rf_constant = static_cast<float>(cfg.crf);
    } else {
        p.rc.i_rc_method = X264_RC_ABR;
        p.rc.i_bitrate = static_cast<int>(cfg.bitrate_kbps);
        if (cfg.rc_mode == EncoderConfig::RcMode::CBR) {
            p.rc.i_vbv_max_bitrate = static_cast<int>(cfg.bitrate_kbps);
            p.rc.i_vbv_buffer_size = static_cast<int>(cfg.bitrate_kbps / cfg.fps); // 1 帧 VBV
            p.rc.b_filler = 1;            // 填充保 CBR 恒定码率
            p.b_vfr_input = 0;
        }
    }
    p.i_threads = 1;                      // 单线程 + zerolatency：延时最优
    p.b_repeat_headers = 1;               // 每个关键帧前重复 SPS/PPS
    p.b_annexb = 1;
    p.i_log_level = X264_LOG_WARNING;

    enc_ = x264_encoder_open(&p);  // 使用宏展开版本，避免 x264_encoder_open_164 之类的符号问题
    if (!enc_) {
        error = "x264_encoder_open failed";
        return false;
    }
    x264_picture_alloc(&pic_in_, X264_CSP_I420, p.i_width, p.i_height);

    // 缓存 SPS/PPS（headers 归 encoder 内部所有，无需释放），
    // 供 UDP/RTP 链路在会话开始时先行发送
    x264_nal_t* nal = nullptr;
    int nnal = 0;
    if (x264_encoder_headers(enc_, &nal, &nnal) >= 0 && nnal > 0) {
        for (int i = 0; i < nnal; ++i)
            seq_hdr_.insert(seq_hdr_.end(), nal[i].p_payload, nal[i].p_payload + nal[i].i_payload);
    }
    RTS_LOGI("x264", "opened %ux%u@%u %s %ukbps gop=%u zerolatency=%d",
             cfg.width, cfg.height, cfg.fps,
             cfg.rc_mode == EncoderConfig::RcMode::CRF ? "CRF" :
             (cfg.rc_mode == EncoderConfig::RcMode::CBR ? "CBR" : "VBR"),
             cfg.bitrate_kbps, cfg.gop, cfg.zerolatency);
    return true;
}

void X264Encoder::close() {
    if (enc_) {
        x264_encoder_close(enc_);
        enc_ = nullptr;
    }
    x264_picture_clean(&pic_in_);
}

bool X264Encoder::encode(const Frame& in, EncodedFramePtr& out) {
    if (!enc_ || in.fmt != PixelFormat::I420 ||
        in.data.size() < static_cast<size_t>(in.width) * in.height * 3 / 2) {
        return false;
    }
    std::memcpy(pic_in_.img.plane[0], in.data.data(),
                static_cast<size_t>(in.width) * in.height * 3 / 2);
    pic_in_.i_pts = pts_++;

    x264_nal_t* nal = nullptr;
    int nnal = 0;
    int frame_size = x264_encoder_encode(enc_, &nal, &nnal, &pic_in_, &pic_out_);
    if (frame_size <= 0) return true;  // 缓冲中无输出，不算错误

    out = std::make_shared<EncodedFrame>();
    out->capture_us = in.capture_us;
    out->wall_us = in.wall_us;
    out->frame_id = in.frame_id;
    out->width = in.width;
    out->height = in.height;
    out->keyframe = (pic_out_.b_keyframe != 0);
    out->encode_done_us = now_us();
    out->data.reserve(static_cast<size_t>(frame_size));
    for (int i = 0; i < nnal; ++i)
        out->data.insert(out->data.end(), nal[i].p_payload, nal[i].p_payload + nal[i].i_payload);
    return !out->data.empty();
}

} // namespace rtstream
