#pragma once

// JPEG 编码器（服务端预览用）：I420 输入 → 缩放（swscale）→ MJPEG 输出。

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

#include "common/logger.h"
#include "common/media_types.h"

namespace rtstream {

class JpegEncoder {
public:
    ~JpegEncoder() { close(); }

    bool open(uint32_t src_w, uint32_t src_h, uint32_t out_w, uint32_t out_h,
              int quality, std::string& error) {
        src_w_ = src_w; src_h_ = src_h;
        out_w_ = out_w; out_h_ = out_h;
        const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
        if (!codec) {
            error = "mjpeg encoder not found";
            return false;
        }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) { error = "alloc mjpeg ctx failed"; return false; }
        ctx_->width = static_cast<int>(out_w_);
        ctx_->height = static_cast<int>(out_h_);
        ctx_->pix_fmt = AV_PIX_FMT_YUVJ420P;
        ctx_->time_base = AVRational{1, 25};
        ctx_->qmin = 2;
        ctx_->qmax = std::clamp(31 - quality * 29 / 100, 2, 31);
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            error = "avcodec_open2 mjpeg failed";
            return false;
        }
        frame_ = av_frame_alloc();
        frame_->format = AV_PIX_FMT_YUVJ420P;
        frame_->width = ctx_->width;
        frame_->height = ctx_->height;
        if (av_frame_get_buffer(frame_, 32) < 0) {
            error = "av_frame_get_buffer failed";
            return false;
        }
        sws_ = sws_getContext(static_cast<int>(src_w_), static_cast<int>(src_h_),
                              AV_PIX_FMT_YUV420P,
                              static_cast<int>(out_w_), static_cast<int>(out_h_),
                              AV_PIX_FMT_YUVJ420P,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!sws_) {
            error = "sws_getContext failed";
            return false;
        }
        pkt_ = av_packet_alloc();
        scaled_.resize(static_cast<size_t>(out_w_) * out_h_ * 3 / 2);
        return true;
    }

    void close() {
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
        if (ctx_) avcodec_free_context(&ctx_);
    }

    // 输入必须是 I420；输出 JPEG 字节
    bool encode(const Frame& f, std::vector<uint8_t>& out) {
        if (!ctx_ || f.fmt != PixelFormat::I420) return false;
        const uint8_t* src[3] = {
            f.data.data(),
            f.data.data() + static_cast<size_t>(f.width) * f.height,
            f.data.data() + static_cast<size_t>(f.width) * f.height * 5 / 4};
        const int src_stride[3] = {static_cast<int>(f.width),
                                   static_cast<int>(f.width / 2),
                                   static_cast<int>(f.width / 2)};
        uint8_t* dst[3] = {scaled_.data(),
                           scaled_.data() + static_cast<size_t>(out_w_) * out_h_,
                           scaled_.data() + static_cast<size_t>(out_w_) * out_h_ * 5 / 4};
        const int dst_stride[3] = {static_cast<int>(out_w_),
                                   static_cast<int>(out_w_ / 2),
                                   static_cast<int>(out_w_ / 2)};
        sws_scale(sws_, src, src_stride, 0, static_cast<int>(f.height), dst, dst_stride);

        if (av_frame_make_writable(frame_) < 0) return false;
        std::memcpy(frame_->data[0], dst[0], static_cast<size_t>(out_w_) * out_h_);
        std::memcpy(frame_->data[1], dst[1], static_cast<size_t>(out_w_) * out_h_ / 4);
        std::memcpy(frame_->data[2], dst[2], static_cast<size_t>(out_w_) * out_h_ / 4);
        frame_->pts = pts_++;

        if (avcodec_send_frame(ctx_, frame_) < 0) return false;
        int ret = avcodec_receive_packet(ctx_, pkt_);
        if (ret < 0) return false;
        out.assign(pkt_->data, pkt_->data + pkt_->size);
        av_packet_unref(pkt_);
        return !out.empty();
    }

private:
    uint32_t src_w_ = 0, src_h_ = 0, out_w_ = 0, out_h_ = 0;
    AVCodecContext* ctx_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVPacket* pkt_ = nullptr;
    SwsContext* sws_ = nullptr;
    std::vector<uint8_t> scaled_;
    int64_t pts_ = 0;
};

} // namespace rtstream
