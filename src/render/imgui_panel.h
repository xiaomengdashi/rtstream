#pragma once

// ImGui 指标面板（桌面客户端）：叠在 GL 画面上显示运行指标。
//Dear ImGui 经后端（glfw+opengl3）复用客户端的窗口与 GL 上下文。

#include <GLFW/glfw3.h>
#include <glad/gl.h>

// ImGui 自身头文件的 memset 告警（第三方代码内部用法），局部屏蔽
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnontrivial-memcall"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#pragma clang diagnostic pop

#include <unistd.h>

#include <string>

#include "common/media_types.h"
#include "stats/metrics.h"

namespace rtstream {

class ImGuiPanel {
public:
    bool init(GLFWwindow* win, std::string& error) {
        IMGUI_CHECKVERSION();
        if (!ImGui::CreateContext()) {
            error = "ImGui::CreateContext failed";
            return false;
        }
        // 默认内嵌字体只含 ASCII，中文会渲染成 '?'——加载系统中文字体
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;  // 禁用布局持久化：保证每次启动都是新默认布局
        static const char* cjk_fonts[] = {
            "/System/Library/Fonts/PingFang.ttc",                          // macOS 苹方
            "/System/Library/Fonts/Hiragino Sans GB.ttc",                  // macOS 冬青黑体
            "/System/Library/Fonts/STHeiti Light.ttc",                     // macOS 华文黑体
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",      // Linux Noto CJK
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",              // Linux 文泉驿
        };
        const char* cjk = nullptr;
        for (const char* p : cjk_fonts) {
            if (::access(p, R_OK) == 0) { cjk = p; break; }
        }
        if (cjk) {
            // Full 字形集（GB2312 全表）：Common 的 2500 常用字表缺"帧/拟/染"等字
            ImFont* font = io.Fonts->AddFontFromFileTTF(
                cjk, 16.0f, nullptr, io.Fonts->GetGlyphRangesChineseFull());
            if (!font)
                RTS_LOGW("imgui", "中文字体加载失败: %s（界面中文将显示为 ?）", cjk);
        } else {
            RTS_LOGW("imgui", "未找到系统中文字体，界面中文将显示为 ?");
        }
        ImGui::StyleColorsDark();
        ImGuiStyle& st = ImGui::GetStyle();
        st.WindowRounding = 4.f;
        st.FrameRounding = 3.f;

        if (!ImGui_ImplGlfw_InitForOpenGL(win, true)) {
            error = "ImGui glfw backend init failed";
            return false;
        }
        if (!ImGui_ImplOpenGL3_Init("#version 330 core")) {
            error = "ImGui opengl3 backend init failed";
            return false;
        }
        return true;
    }

    void shutdown() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    void begin_frame() {
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void draw(TransportType transport, double jb_target_ms) {
        Metrics& m = Metrics::instance();
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(400, 390), ImGuiCond_FirstUseEver);
        ImGui::Begin("rtstream 监控", nullptr);

        const char* link = transport == TransportType::TCP ? "TCP" :
                           transport == TransportType::UDP ? "UDP" : "RTP";
        ImGui::Text("链路: %s   JitterBuffer 目标: %.0f ms", link, jb_target_ms);
        ImGui::Separator();

        if (ImGui::CollapsingHeader("采集 / 编码（服务端同步）", ImGuiTreeNodeFlags_DefaultOpen)) {
            int64_t sync_us = m.remote.last_sync_us.load();
            bool synced = (sync_us > 0 && (now_us() - sync_us) < 3000000);
            ImGui::Text("状态: %s   采集帧: %llu   编码帧: %llu",
                        synced ? "同步中" : "等待数据/离线",
                        (unsigned long long)m.remote.capture_frames.load(),
                        (unsigned long long)m.remote.encoded_frames.load());
        }
        if (ImGui::CollapsingHeader("解码 / 渲染", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("解码: %llu   渲染: %llu   队列丢帧: %llu",
                        (unsigned long long)m.decoded_frames.load(),
                        (unsigned long long)m.rendered_frames.load(),
                        (unsigned long long)m.queue_dropped.load());
            auto lat = m.latency();
            if (lat.samples > 0)
                ImGui::Text("E2E  P50 %.1f | P95 %.1f | Max %.1f ms",
                            lat.p50, lat.p95, lat.max);
            else
                ImGui::Text("E2E  （等待数据，回环链路有效）");
        }
        if (ImGui::CollapsingHeader("网络 · 发送（服务端同步）", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("发送包: %llu (FEC %llu, 重传 %llu)",
                        (unsigned long long)m.remote.net_packets_sent.load(),
                        (unsigned long long)m.remote.net_fec_sent.load(),
                        (unsigned long long)m.remote.retransmit_sent.load());
            ImGui::Text("发送字节: %llu   模拟丢包: %llu",
                        (unsigned long long)m.remote.net_bytes_sent.load(),
                        (unsigned long long)m.remote.sim_dropped.load());
            ImGui::Text("NACK 请求收到: %llu", (unsigned long long)m.remote.nack_received.load());
        }
        if (ImGui::CollapsingHeader("网络 · 接收（本机）", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("接收包: %llu   判定丢失: %llu",
                        (unsigned long long)m.net_packets_recv.load(),
                        (unsigned long long)m.net_lost.load());
            ImGui::Text("FEC 恢复: %llu   NACK 命中: %llu",
                        (unsigned long long)m.fec_recovered.load(),
                        (unsigned long long)m.nack_hit.load());
            ImGui::Text("RTT: %.1f ms   Jitter: %.1f ms",
                        m.rtt_ms.load(), m.jitter_ms.load());
        }
        ImGui::Separator();
        ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
        ImGui::End();
    }

    void end_frame() {
        ImGui::Render();
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
};

} // namespace rtstream
