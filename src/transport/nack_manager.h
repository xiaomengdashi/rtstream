#pragma once

// 服务端 NACK 管理：按 (group_id, fragment_seq) 维护最近发送包的重传缓存。
// 接收端检测到序号缺口后发 NACK（载荷为一组 6 字节记录：
// group_id(4, 网络序) + fragment_seq(2, 网络序)），本模块查缓存重发。

#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include "common/logger.h"
#include "stats/metrics.h"

namespace rtstream {

struct RetransPacket {
    uint32_t group_id;
    uint16_t fragment_seq;
    std::vector<uint8_t> raw;  // 完整 UDP 包（含头），直接重发
};

class NackManager {
public:
    using SendFn = std::function<void(const uint8_t*, size_t)>;

    explicit NackManager(size_t cache_packets = 2048) : cache_(cache_packets) {}

    void set_sender(SendFn fn) { send_ = std::move(fn); }

    // 数据包发出后登记缓存
    void remember(uint32_t group_id, uint16_t fragment_seq, std::vector<uint8_t> raw) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (cache_ == 0) return;
        while (items_.size() >= cache_) {
            items_.pop_front();
        }
        items_.push_back({group_id, fragment_seq, std::move(raw)});
    }

    // 处理 NACK：payload 为连续 6 字节记录，重发命中的包
    void on_nack(const uint8_t* payload, size_t len) {
        size_t n = len / 6;
        for (size_t i = 0; i < n; ++i) {
            uint32_t gid;
            uint16_t seq;
            std::memcpy(&gid, payload + i * 6, 4);
            std::memcpy(&seq, payload + i * 6 + 4, 2);
            gid = ntohl(gid);
            seq = ntohs(seq);
            Metrics::instance().nack_received++;
            RetransPacket pkt;
            if (find(gid, seq, pkt) && send_) {
                send_(pkt.raw.data(), pkt.raw.size());
                Metrics::instance().retransmit_sent++;
            }
        }
    }

private:
    bool find(uint32_t group_id, uint16_t fragment_seq, RetransPacket& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto it = items_.rbegin(); it != items_.rend(); ++it) {
            if (it->group_id == group_id && it->fragment_seq == fragment_seq) {
                out = *it;
                return true;
            }
        }
        return false;
    }

    std::mutex mtx_;
    std::deque<RetransPacket> items_;
    size_t cache_;
    SendFn send_;
};

} // namespace rtstream
