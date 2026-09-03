#pragma once

// 全项目共享的基础数据结构与协议常量。
// Frame      —— 原始/解码后视频帧
// EncodedFrame —— 一帧 H.264（NALU 列表 + 元数据）
// MediaPacket   —— 网络层传输单元（分片/RTP 包统一视图）

#include <arpa/inet.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <netinet/in.h>
#include <string>
#include <vector>

namespace rtstream {

// 大端序读写（glibc 的 htobe64/be64toh 在 macOS/BSD 不存在，自实现保证可移植）
inline uint64_t load_be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

inline void store_be64(uint8_t* p, uint64_t v) {
    for (int i = 7; i >= 0; --i) {
        p[i] = static_cast<uint8_t>(v & 0xFF);
        v >>= 8;
    }
}

// ---------------- 视频帧 ----------------

enum class PixelFormat : int {
    None = 0,
    I420,   // YUV420Planar：解码输出/渲染输入
    MJPEG,  // 压缩 JPEG（V4L2 常见输出，测试源也可输出）
    YUYV,   // V4L2 常见 4:2:2 packed
};

struct Frame {
    int64_t capture_us = 0;   // 单调时钟采集时间戳（延时统计基准）
    int64_t wall_us = 0;      // 墙钟时间戳（跨进程/浏览器对照，仅回环可信）
    int64_t frame_id = 0;     // 采集侧递增帧号
    uint32_t width = 0;
    uint32_t height = 0;
    PixelFormat fmt = PixelFormat::None;
    std::vector<uint8_t> data;
};

using FramePtr = std::shared_ptr<Frame>;

inline FramePtr make_frame() { return std::make_shared<Frame>(); }

// ---------------- 编码帧 ----------------

struct EncodedFrame {
    int64_t capture_us = 0;
    int64_t wall_us = 0;
    int64_t encode_done_us = 0;
    int64_t frame_id = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    bool keyframe = false;
    // 一帧完整 H.264 Annex-B 数据（含 startcode），分发层负责切 NALU/分片
    std::vector<uint8_t> data;
};

using EncodedFramePtr = std::shared_ptr<EncodedFrame>;

// ---------------- 网络包 ----------------

// 载荷类型
enum class PayloadType : uint8_t {
    Data = 0,   // 数据分片
    Fec  = 1,   // XOR 恢复包
    Nack = 2,   // 重传请求（接收端→发送端）
    StatsPing = 3, // RTT 测量（客户端→服务端回显）
    StatsPong = 4,
};

// 传输链路
enum class TransportType : int { TCP = 0, UDP = 1, RTP = 2 };

// 字节布局（24 字节，网络序，工具函数 read/write 见下）：
//  [0..1]  magic 0x5254 ("RT")
//  [2]     version
//  [3]     payload_type  (PayloadType)
//  [4..7]  group_id      （FEC 组号）
//  [8..9]  fragment_seq  （组内分片号；FEC 包固定 0xFFFF）
//  [10..11] fragment_cnt （组内分片总数，含 FEC 包）
//  [12..15] frame_id 低 32 位（帧号递增，32 位足够）
//  [16..19] capture_us 低 32 位（单调钟微秒，约 71 分钟回绕，回环演示足够；
//           回绕由接收端按相邻差值处理）
//  [20..21] payload_len
//  [22]    flags (bit0 = keyframe)
//  [23]    reserved
struct [[gnu::packed]] UdpWire {
    uint8_t magic_hi, magic_lo, version, payload_type;
    uint32_t group_id;
    uint16_t fragment_seq, fragment_cnt;
    uint32_t frame_id;
    uint32_t capture_us_lo;
    uint16_t payload_len;
    uint8_t flags, reserved;
};
static_assert(sizeof(UdpWire) == 24, "UdpWire must be 24 bytes");

struct UdpHeader {
    static constexpr uint16_t kMagic = 0x5254;  // "RT"
    static constexpr uint8_t kVersion = 1;
    static constexpr size_t kSize = 24;
    static constexpr uint16_t kFecSeq = 0xFFFF;

    uint8_t payload_type = 0;   // PayloadType
    uint32_t group_id = 0;
    uint16_t fragment_seq = 0;
    uint16_t fragment_cnt = 0;
    uint32_t frame_id = 0;
    uint32_t capture_us_lo = 0;
    uint16_t payload_len = 0;
    uint8_t flags = 0;          // bit0: keyframe

    void write(uint8_t* buf) const {
        buf[0] = static_cast<uint8_t>(kMagic >> 8);
        buf[1] = static_cast<uint8_t>(kMagic & 0xFF);
        buf[2] = kVersion;
        buf[3] = payload_type;
        uint32_t g = htonl(group_id);
        std::memcpy(buf + 4, &g, 4);
        uint16_t fs = htons(fragment_seq);
        std::memcpy(buf + 8, &fs, 2);
        uint16_t fc = htons(fragment_cnt);
        std::memcpy(buf + 10, &fc, 2);
        uint32_t fid = htonl(frame_id);
        std::memcpy(buf + 12, &fid, 4);
        uint32_t cap = htonl(capture_us_lo);
        std::memcpy(buf + 16, &cap, 4);
        uint16_t pl = htons(payload_len);
        std::memcpy(buf + 20, &pl, 2);
        buf[22] = flags;
        buf[23] = 0;
    }

    static UdpHeader read(const uint8_t* buf) {
        UdpHeader h;
        uint16_t magic;
        std::memcpy(&magic, buf, 2);
        h.payload_type = buf[3];
        uint32_t g;
        std::memcpy(&g, buf + 4, 4);
        h.group_id = ntohl(g);
        uint16_t fs, fc;
        std::memcpy(&fs, buf + 8, 2);
        std::memcpy(&fc, buf + 10, 2);
        h.fragment_seq = ntohs(fs);
        h.fragment_cnt = ntohs(fc);
        uint32_t fid, cap;
        std::memcpy(&fid, buf + 12, 4);
        std::memcpy(&cap, buf + 16, 4);
        h.frame_id = ntohl(fid);
        h.capture_us_lo = ntohl(cap);
        uint16_t pl;
        std::memcpy(&pl, buf + 20, 2);
        h.payload_len = ntohs(pl);
        h.flags = buf[22];
        (void)magic; // magic 校验由调用方完成
        return h;
    }

    static bool magic_ok(const uint8_t* buf) {
        return buf[0] == static_cast<uint8_t>(kMagic >> 8) &&
               buf[1] == static_cast<uint8_t>(kMagic & 0xFF);
    }

    bool keyframe() const { return flags & 0x1; }
    void set_keyframe(bool on) { flags = on ? (flags | 0x1) : (flags & ~0x1); }
};

} // namespace rtstream
