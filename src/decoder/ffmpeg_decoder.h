#pragma once

// FFmpeg H.264 解码封装：Annex-B 输入 → I420 Frame 输出。

#include <cstdint>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
}

#include "common/media_types.h"

namespace rtstream {

class FFmpegDecoder {
public:
    FFmpegDecoder() = default;
    ~FFmpegDecoder() { close(); }

    bool open(std::string& error) {
        const AVCodec* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) {
            error = "h264 decoder not found";
            return false;
        }
        ctx_ = avcodec_alloc_context3(codec);
        if (!ctx_) {
            error = "avcodec_alloc_context3 failed";
            return false;
        }
        ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;  // 配合 zerolatency，到帧即出
        if (avcodec_open2(ctx_, codec, nullptr) < 0) {
            error = "avcodec_open2 failed";
            return false;
        }
        pkt_ = av_packet_alloc();
        frame_ = av_frame_alloc();
        if (!pkt_ || !frame_) {
            error = "av_packet/av_frame alloc failed";
            return false;
        }
        return true;
    }

    void close() {
        if (pkt_) av_packet_free(&pkt_);
        if (frame_) av_frame_free(&frame_);
        if (ctx_) avcodec_free_context(&ctx_);
    }

    // 解码一帧 Annex-B；有输出时 out 非空。flush=false 正常送包。
    bool decode(const uint8_t* annexb, size_t len, int64_t capture_us,
                int64_t wall_us, int64_t frame_id, FramePtr& out) {
        if (!ctx_) return false;
        pkt_->data = const_cast<uint8_t*>(annexb);
        pkt_->size = static_cast<int>(len);
        pkt_->pts = frame_id;
        pkt_->dts = frame_id;
        int ret = avcodec_send_packet(ctx_, pkt_);
        pkt_->data = nullptr;  // 所有权仍归调用方
        pkt_->size = 0;
        if (ret < 0 && ret != AVERROR(EAGAIN)) return false;
        return receive(capture_us, wall_us, frame_id, out);
    }

private:
    bool receive(int64_t capture_us, int64_t wall_us, int64_t frame_id, FramePtr& out) {
        int ret = avcodec_receive_frame(ctx_, frame_);
        if (ret < 0) return false;
        out = make_frame();
        out->width = static_cast<uint32_t>(frame_->width);
        out->height = static_cast<uint32_t>(frame_->height);
        out->fmt = PixelFormat::I420;
        out->capture_us = capture_us;
        out->wall_us = wall_us;
        out->frame_id = frame_id;
        int ysize = frame_->width * frame_->height;
        out->data.resize(static_cast<size_t>(ysize) * 3 / 2);
        // 处理行对齐（linesize >= width）
        for (int i = 0; i < frame_->height; ++i)
            std::memcpy(out->data.data() + i * frame_->width,
                        frame_->data[0] + i * frame_->linesize[0], frame_->width);
        uint8_t* dst_u = out->data.data() + ysize;
        uint8_t* dst_v = dst_u + ysize / 4;
        for (int i = 0; i < frame_->height / 2; ++i) {
            std::memcpy(dst_u + i * (frame_->width / 2),
                        frame_->data[1] + i * frame_->linesize[1], frame_->width / 2);
            std::memcpy(dst_v + i * (frame_->width / 2),
                        frame_->data[2] + i * frame_->linesize[2], frame_->width / 2);
        }
        av_frame_unref(frame_);
        return true;
    }

    AVCodecContext* ctx_ = nullptr;
    AVPacket* pkt_ = nullptr;
    AVFrame* frame_ = nullptr;
};

} // namespace rtstream
