// rtstream 客户端：网络接收 → H.264 解码 → OpenGL 渲染 + ImGui 运行指标面板。
// 链路：--transport udp|rtp|tcp（对应 UdpReceiver / RtpReceiver / TcpClient）。
// --headless：无窗口模式（仅解码 + 指标打印，用于无显示环境验证链路）。
// 桌面渲染需要 GLFW/OpenGL/GLAD/ImGui（CMake 选项 RTSTREAM_ENABLE_GL_CLIENT）。

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <cli11.hpp>
// 注：CLI11 内部 std::wstring_convert 的弃用告警在 app CMake 目标上抑制
//（模板实例化点在本文件内，包含处 pragma 无法覆盖）

#include "common/logger.h"
#include "common/thread_safe_queue.h"
#include "decoder/ffmpeg_decoder.h"
#include "receiver/jitter_buffer.h"
#include "receiver/rtp_receiver.h"
#include "receiver/tcp_client.h"
#include "receiver/udp_receiver.h"
#include "stats/metrics.h"

#if RTSTREAM_ENABLE_GL_CLIENT
#include "render/gl_renderer.h"
#include "render/gl_window.h"
#include "render/imgui_panel.h"
#endif

using namespace rtstream;

namespace {

struct Args {
    std::string server = "127.0.0.1";
    std::string transport = "udp";
    uint16_t port = 0;       // 0 = 按链路默认
    bool headless = false;
};

Args parse_args(int argc, char** argv) {
    Args a;
    CLI::App app{"rtstream client —— 网络接收 / 弱网恢复 / H.264 解码 / OpenGL 渲染 + ImGui 指标"};

    app.add_option("--server", a.server, "服务端地址")->capture_default_str();
    app.add_option("--transport", a.transport, "传输链路（需与服务端一致）")
        ->capture_default_str()
        ->check(CLI::IsMember({"tcp", "udp", "rtp"}));
    app.add_option("--port", a.port, "端口（0=按链路默认：udp 9001 / rtp 9002 / tcp 9000）")
        ->capture_default_str();
    app.add_flag("--headless", a.headless, "无窗口模式（仅解码 + 指标打印）；默认 OpenGL+ImGui 窗口渲染");

    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        std::exit(app.exit(e));
    }
    return a;
}

uint16_t default_port(const std::string& transport) {
    if (transport == "tcp") return 9000;
    if (transport == "rtp") return 9002;
    return 9001;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parse_args(argc, argv);
    log_level() = LogLevel::Info;
    ignore_sigpipe();
    TransportType ttype = args.transport == "tcp" ? TransportType::TCP :
                          args.transport == "rtp" ? TransportType::RTP : TransportType::UDP;
    uint16_t port = args.port ? args.port : default_port(args.transport);

    // ---- 接收端 ----
    std::string error;
    UdpReceiver udp_rx;
    RtpReceiver rtp_rx;
    TcpClient tcp_rx;
    bool started = false;
    if (ttype == TransportType::UDP) {
        started = udp_rx.start(args.server, port, error);
    } else if (ttype == TransportType::RTP) {
        started = rtp_rx.start(args.server, port, error);
    } else {
        started = tcp_rx.start(args.server, port, error);
    }
    if (!started) {
        RTS_LOGE("client", "receiver start failed: %s", error.c_str());
        return 1;
    }

    // 解码线程：EncodedFrame → I420 Frame
    ThreadSafeQueue<FramePtr> decoded_q{4};
    std::atomic<bool> stop_flag{false};
    std::thread decode_thread([&] {
        FFmpegDecoder decoder;
        if (!decoder.open(error)) {
            RTS_LOGE("decode", "%s", error.c_str());
            stop_flag = true;
            return;
        }
        EncodedFramePtr ef;
        while (!stop_flag) {
            bool got;
            if (ttype == TransportType::UDP) got = udp_rx.out.pop(ef);
            else if (ttype == TransportType::RTP) got = rtp_rx.out.pop(ef);
            else got = tcp_rx.out.pop(ef);
            if (!got) {
                if ((ttype == TransportType::UDP && udp_rx.out.closed()) ||
                    (ttype == TransportType::RTP && rtp_rx.out.closed()) ||
                    (ttype == TransportType::TCP && tcp_rx.out.closed()))
                    break;
                continue;
            }
            FramePtr f;
            if (decoder.decode(ef->data.data(), ef->data.size(), ef->capture_us,
                               ef->wall_us, ef->frame_id, f)) {
                Metrics::instance().decoded_frames++;
                decoded_q.push(f);
            }
        }
        decoded_q.close();
    });

#if RTSTREAM_ENABLE_GL_CLIENT
    if (!args.headless) {
        // ---- OpenGL 渲染 + ImGui 面板 ----
        GlWindow win;
        if (!win.open(1280, 760, "rtstream client", error)) {
            RTS_LOGE("client", "%s", error.c_str());
            return 1;
        }
        if (!win.load_gl(error)) {
            RTS_LOGE("client", "%s", error.c_str());
            return 1;
        }
        GlRenderer renderer;
        if (!renderer.init(error)) {
            RTS_LOGE("client", "%s", error.c_str());
            return 1;
        }
        ImGuiPanel panel;
        if (!panel.init(win.handle(), error)) {
            RTS_LOGE("client", "%s", error.c_str());
            return 1;
        }

        Metrics& m = Metrics::instance();
        while (!win.should_close() && !stop_flag) {
            win.poll_events();
            int w = 0, h = 0;
            win.get_size(w, h);
            glViewport(0, 0, w, h);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);

            FramePtr f;
            if (decoded_q.pop_for(f, std::chrono::milliseconds(33))) {
                renderer.draw(*f);
                m.rendered_frames++;
                m.add_e2e_ms((now_us() - f->capture_us) / 1000.0);
            }

            panel.begin_frame();
            panel.draw(ttype, 0);
            panel.end_frame();

            win.swap_buffers();
        }
        stop_flag = true;
        panel.shutdown();
        udp_rx.stop(); rtp_rx.stop(); tcp_rx.stop();
        decode_thread.join();
        return 0;
    }
#endif

    // ---- headless 模式：仅解码 + 指标输出（无显示环境验证链路） ----
    RTS_LOGI("client", "headless client running (%s -> %s:%u)",
             args.transport.c_str(), args.server.c_str(), port);
    FramePtr f;
    int64_t last_report = now_us();
    for (;;) {
        if (!decoded_q.pop_for(f, std::chrono::milliseconds(200))) {
            if (stop_flag) break;
            continue;
        }
        Metrics& m = Metrics::instance();
        m.rendered_frames++;
        m.add_e2e_ms((now_us() - f->capture_us) / 1000.0);

        int64_t now = now_us();
        if (now - last_report > 2000000) {
            last_report = now;
            auto lat = m.latency();
            RTS_LOGI("client",
                     "decoded=%llu e2e p50=%.1f p95=%.1f ms | recv=%llu lost=%llu "
                     "fec=%llu rtt=%.1f jitter=%.1f | srv captured=%llu encoded=%llu sent=%llu",
                     (unsigned long long)m.decoded_frames.load(),
                     lat.p50, lat.p95,
                     (unsigned long long)m.net_packets_recv.load(),
                     (unsigned long long)m.net_lost.load(),
                     (unsigned long long)m.fec_recovered.load(),
                     m.rtt_ms.load(), m.jitter_ms.load(),
                     (unsigned long long)m.remote.capture_frames.load(),
                     (unsigned long long)m.remote.encoded_frames.load(),
                     (unsigned long long)m.remote.net_packets_sent.load());
        }
    }

    udp_rx.stop(); rtp_rx.stop(); tcp_rx.stop();
    decode_thread.join();

    auto lat = Metrics::instance().latency();
    RTS_LOGI("client", "summary: decoded=%llu e2e p50=%.1f p95=%.1f max=%.1f ms",
             (unsigned long long)Metrics::instance().decoded_frames.load(),
             lat.p50, lat.p95, lat.max);
    return 0;
}
