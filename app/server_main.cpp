// rtstream 服务端：采集 → H.264 编码 → 多协议分发 + Web 监控。
// 三层架构入口：装配公共组件与业务模块，信号处理保证优雅退出。

#include <atomic>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <thread>

#include <cli11.hpp>

#include "common/logger.h"
#include "common/thread_safe_queue.h"
#include "capture/capture_factory.h"
#include "decoder/ffmpeg_decoder.h"
#include "encoder/x264_encoder.h"
#include "receiver/rtp_receiver.h"
#include "receiver/tcp_client.h"
#include "receiver/udp_receiver.h"
#include "stats/metrics.h"
#include "transport/transport_factory.h"
#include "web/http_server.h"
#include "web/web_previewer.h"
#include "web/ws_server.h"

using namespace rtstream;

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop = true; }

struct Args {
    std::string source = "auto";       // auto | test | v4l2 | avf
    std::string transport = "udp";     // tcp | udp | rtp
    std::string device = "/dev/video0";
    uint32_t width = 1280, height = 720, fps = 30;
    uint32_t bitrate_kbps = 4000;
    std::string rc = "crf";            // crf | cbr | vbr
    int crf = 23;
    uint16_t port_tcp = 9000, port_udp = 9001, port_rtp = 9002, port_web = 8080;
    std::string web_root = "src/web/web_client";
    double loss = 0.0;
    uint32_t delay_ms = 0, jitter_ms = 0;
    size_t udp_mtu = 1200;
    uint32_t fec_k = 8;
    bool loopback_monitor = true;   // 内置回环接收器：填充恢复端指标（fec_recovered/net_lost/jitter/e2e）
};

Args parse_args(int argc, char** argv) {
    Args a;
    CLI::App app{"rtstream server —— 实时视频采集 / H.264 编码 / 多协议分发 + Web 监控"};

    app.add_option("--source", a.source, "采集源：auto=Linux 试 V4L2、macOS 试 AVFoundation，失败回落测试源")
        ->capture_default_str()
        ->check(CLI::IsMember({"auto", "test", "v4l2", "avf"}));
    app.add_option("--transport", a.transport, "传输链路")
        ->capture_default_str()
        ->check(CLI::IsMember({"tcp", "udp", "rtp"}));
    app.add_option("--device", a.device, "V4L2 设备路径")->capture_default_str();
    app.add_option("--width", a.width, "采集宽度（实际以首帧为准）")->capture_default_str();
    app.add_option("--height", a.height, "采集高度")->capture_default_str();
    app.add_option("--fps", a.fps, "帧率")->capture_default_str();
    app.add_option("--bitrate", a.bitrate_kbps, "目标码率 kbps（CBR/VBR）")->capture_default_str();
    app.add_option("--rc", a.rc, "码率控制")->capture_default_str()
        ->check(CLI::IsMember({"crf", "cbr", "vbr"}));
    app.add_option("--crf", a.crf, "CRF 质量（rc=crf 时）")->capture_default_str();
    app.add_option("--port-tcp", a.port_tcp, "TCP 端口")->capture_default_str();
    app.add_option("--port-udp", a.port_udp, "UDP 端口")->capture_default_str();
    app.add_option("--port-rtp", a.port_rtp, "RTP 端口")->capture_default_str();
    app.add_option("--port-web", a.port_web, "Web 面板端口")->capture_default_str();
    app.add_option("--web-root", a.web_root, "Web 面板静态目录")->capture_default_str();
    app.add_option("--loss", a.loss, "弱网模拟：丢包率 0~1（UDP/RTP）")
        ->capture_default_str()->check(CLI::Range(0.0, 1.0));
    app.add_option("--delay-ms", a.delay_ms, "弱网模拟：固定延迟 ms")->capture_default_str();
    app.add_option("--jitter-ms", a.jitter_ms, "弱网模拟：抖动 ms")->capture_default_str();
    app.add_option("--mtu", a.udp_mtu, "UDP/RTP 单包载荷上限")->capture_default_str();
    app.add_option("--fec-k", a.fec_k, "FEC 组大小（K 数据片 + 1 恢复片）")->capture_default_str();
    app.add_option("--loopback-monitor", a.loopback_monitor,
                   "内置回环接收器（true/false，填充 fec/nack/jitter/e2e 指标）")
        ->capture_default_str();

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        std::exit(app.exit(e));   // --help / 参数错误：打印并按 CLI11 语义退出
    }
    return a;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    log_level() = LogLevel::Info;
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    std::signal(SIGPIPE, SIG_IGN);

    ServerConfig scfg;
    scfg.tcp_port = args.port_tcp;
    scfg.udp_port = args.port_udp;
    scfg.rtp_port = args.port_rtp;
    scfg.web_port = args.port_web;
    scfg.udp_mtu = args.udp_mtu;
    scfg.fec_group_k = args.fec_k;
    scfg.loss_rate = args.loss;
    scfg.delay_ms = args.delay_ms;
    scfg.jitter_ms = args.jitter_ms;
    TransportType ttype = args.transport == "tcp" ? TransportType::TCP :
                          args.transport == "rtp" ? TransportType::RTP : TransportType::UDP;

    std::string error;

    // ---- 传输层 ----
    auto transport = make_transport_server(ttype, scfg);
    if (!transport->start(error)) {
        RTS_LOGE("server", "transport start failed: %s", error.c_str());
        return 1;
    }

    // ---- Web 监控：HTTP（静态页 + MJPEG）与 WebSocket 同端口 ----
    WsServer ws;
    SharedJpeg shared_jpeg;
    HttpServer http(shared_jpeg, args.web_root);
    http.set_ws(&ws);
    if (!http.start(args.port_web, error)) {
        RTS_LOGE("server", "http/ws start failed: %s", error.c_str());
        return 1;
    }
    ws.set_on_message([&ws](int, const std::string& text) {
        // 浏览器 RTT 测量：echo
        if (text.rfind("ping ", 0) == 0)
            ws.broadcast_text("pong " + text.substr(5));
    });

    // ---- 采集 + 编码 ----
    ThreadSafeQueue<FramePtr> enc_q{8};          // 采集 → 编码
    ThreadSafeQueue<FramePtr> tap_q{2};          // 采集 → Web 预览（低延迟旁路）

    CaptureConfig ccfg{args.width, args.height, args.fps, args.device};
    auto capture = create_capture(args.source, ccfg, [&](FramePtr f) {
        if (f) {
            tap_q.try_push(f);
            Metrics::instance().capture_frames++;
            enc_q.try_push(f);
        }
    }, error);
    if (!capture) {
        RTS_LOGE("server", "capture start failed: %s", error.c_str());
        return 1;
    }

    WebPreviewer previewer(shared_jpeg);
    previewer.set_ws(&ws);

    // 编码线程：I420 → H.264 → transport 广播。
    // 编码器与预览器延后到首帧打开：相机实际输出分辨率可能与请求不同
    //（AVFoundation 会话预设/V4L2 S_FMT 协商），必须以首帧为准。
    std::thread encode_thread([&] {
        EncoderConfig ecfg;
        ecfg.fps = args.fps;
        ecfg.bitrate_kbps = args.bitrate_kbps;
        ecfg.crf = args.crf;
        ecfg.rc_mode = args.rc == "cbr" ? EncoderConfig::RcMode::CBR :
                       args.rc == "vbr" ? EncoderConfig::RcMode::VBR :
                                          EncoderConfig::RcMode::CRF;
        X264Encoder enc;
        bool opened = false;
        FramePtr f;
        while (!g_stop && enc_q.pop(f)) {
            if (!opened) {
                ecfg.width = f->width;
                ecfg.height = f->height;
                std::string perr;
                if (!previewer.start(tap_q, f->width, f->height, perr))
                    RTS_LOGE("server", "previewer start failed: %s (web 预览不可用，链路不受影响)",
                             perr.c_str());
                if (!enc.open(ecfg, error)) {
                    RTS_LOGE("server", "encoder open failed: %s", error.c_str());
                    g_stop = true;
                    return;
                }
                opened = true;
            }
            EncodedFramePtr ef;
            if (enc.encode(*f, ef) && ef) {
                Metrics::instance().encoded_frames++;
                Metrics::instance().encoded_bytes += ef->data.size();
                transport->broadcast(ef);
            }
        }
        enc.close();
    });

    // ---- 内置回环接收器：走完整"接收→FEC/NACK→JB→解码"路径，填充恢复端指标 ----
    std::thread monitor_thread;
    if (args.loopback_monitor) {
        auto monitor_body = [ttype, &args]() {
            std::string err;
            std::string server = "127.0.0.1";
            UdpReceiver udp_rx;
            RtpReceiver rtp_rx;
            TcpClient tcp_rx;
            bool ok = false;
            if (ttype == TransportType::UDP) ok = udp_rx.start(server, args.port_udp, err);
            else if (ttype == TransportType::RTP) ok = rtp_rx.start(server, args.port_rtp, err);
            else ok = tcp_rx.start(server, args.port_tcp, err);
            if (!ok) {
                RTS_LOGW("monitor", "loopback receiver failed: %s", err.c_str());
                return;
            }
            FFmpegDecoder dec;
            if (!dec.open(err)) {
                RTS_LOGW("monitor", "loopback decoder failed: %s", err.c_str());
                return;
            }
            Metrics& m = Metrics::instance();
            ThreadSafeQueue<EncodedFramePtr>* in =
                ttype == TransportType::UDP ? &udp_rx.out :
                ttype == TransportType::RTP ? &rtp_rx.out : &tcp_rx.out;
            EncodedFramePtr ef;
            while (!g_stop) {
                if (!in->pop_for(ef, std::chrono::milliseconds(200))) {
                    if (in->closed()) break;
                    continue;
                }
                FramePtr f;
                if (dec.decode(ef->data.data(), ef->data.size(), ef->capture_us,
                               ef->wall_us, ef->frame_id, f)) {
                    m.decoded_frames++;
                    m.rendered_frames++;
                    m.add_e2e_ms((now_us() - ef->capture_us) / 1000.0);
                }
            }
        };
        monitor_thread = std::thread(monitor_body);
        RTS_LOGI("server", "loopback monitor enabled (fec/nack/jitter/e2e metrics)");
    }

    RTS_LOGI("server", "running: source=%s transport=%s %ux%u@%u web=http://0.0.0.0:%u",
             capture->name(), transport->name(), args.width, args.height, args.fps,
             args.port_web);
    RTS_LOGI("server", "web dashboard: http://127.0.0.1:%u/  (ws://127.0.0.1:%u/ws)",
             args.port_web, args.port_web);

    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(200));

    RTS_LOGI("server", "shutting down...");
    g_stop = true;
    enc_q.close();
    tap_q.close();
    // 唤醒回环接收器（接收队列在 receiver.stop() 中关闭）
    monitor_thread.join();
    encode_thread.join();
    capture->stop();
    previewer.stop();
    ws.stop();
    http.stop();
    transport->stop();

    auto lat = Metrics::instance().latency();
    RTS_LOGI("server", "summary: captured=%llu encoded=%llu sent=%llu fec=%llu rtx=%llu",
             (unsigned long long)Metrics::instance().capture_frames.load(),
             (unsigned long long)Metrics::instance().encoded_frames.load(),
             (unsigned long long)Metrics::instance().net_packets_sent.load(),
             (unsigned long long)Metrics::instance().net_fec_sent.load(),
             (unsigned long long)Metrics::instance().retransmit_sent.load());
    (void)lat;
    return 0;
}
