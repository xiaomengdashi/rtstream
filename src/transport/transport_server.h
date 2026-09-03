#pragma once

// 传输服务器抽象：TCP/UDP/RTP 三种链路统一接口。
// 服务端流水线把编码帧交给 broadcast()，由具体实现负责打包/分发/弱网处理。

#include <cstdint>
#include <memory>
#include <string>

#include "common/media_types.h"

namespace rtstream {

struct ServerConfig {
    uint16_t tcp_port = 9000;
    uint16_t udp_port = 9001;
    uint16_t rtp_port = 9002;
    uint16_t web_port = 8080;

    size_t udp_mtu = 1200;         // UDP/RTP 单包载荷上限
    uint32_t fec_group_k = 8;      // 每 K 个数据分片 1 个 FEC 恢复包（UDP 链路按帧分组）
    size_t nack_cache = 4096;      // 发送端重传缓存包数

    // 弱网模拟（下行）
    double loss_rate = 0.0;
    uint32_t delay_ms = 0;
    uint32_t jitter_ms = 0;
};

class TransportServer {
public:
    virtual ~TransportServer() = default;
    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;
    virtual void broadcast(const EncodedFramePtr& frame) = 0;
    virtual const char* name() const = 0;
};

// TCP 帧协议头（20 字节，大端）：
//  [0..3]  magic "RTV1" (0x52 0x54 0x56 0x31)
//  [4]     flags (bit0 keyframe)
//  [5..7]  reserved
//  [8..11] frame_id
//  [12..19] capture_us (int64)
struct TcpWire {
    static constexpr size_t kHeaderSize = 20;
    static inline const uint8_t kMagic[4] = {0x52, 0x54, 0x56, 0x31};

    static void write_header(uint8_t* buf, const EncodedFrame& f) {
        std::memcpy(buf, kMagic, 4);
        buf[4] = f.keyframe ? 1 : 0;
        buf[5] = buf[6] = buf[7] = 0;
        uint32_t fid = htonl(static_cast<uint32_t>(f.frame_id));
        std::memcpy(buf + 8, &fid, 4);
        store_be64(buf + 12, static_cast<uint64_t>(f.capture_us));
    }

    static bool magic_ok(const uint8_t* buf) {
        return std::memcmp(buf, kMagic, 4) == 0;
    }
};

inline std::unique_ptr<TransportServer> make_transport_server(TransportType type,
                                                              const ServerConfig& cfg);

} // namespace rtstream
