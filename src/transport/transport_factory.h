#pragma once

// 传输服务器工厂：按 TransportType 构造对应实现。

#include "transport_server.h"
#include "tcp_server.h"
#include "udp_server.h"
#include "rtp_server.h"

namespace rtstream {

inline std::unique_ptr<TransportServer> make_transport_server(TransportType type,
                                                              const ServerConfig& cfg) {
    switch (type) {
        case TransportType::TCP: return std::make_unique<TcpServer>(cfg);
        case TransportType::UDP: return std::make_unique<UdpServer>(cfg);
        case TransportType::RTP: return std::make_unique<RtpServer>(cfg);
    }
    return nullptr;
}

} // namespace rtstream
