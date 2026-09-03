# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概述

rtstream 是跨 Linux/macOS 的实时视频端到端系统：服务端采集（V4L2/AVFoundation）→ x264 编码 → TCP/UDP/RTP 分发；客户端接收 → 弱网恢复（FEC+NACK+JitterBuffer）→ FFmpeg 解码 → OpenGL 渲染。服务端内嵌 Web 监控面板（自实现 WebSocket/HTTP/JPEG）。

## 常用命令

```bash
# 编译（产物：build/app/rtstream_server、build/app/rtstream_client）
./scripts/build.sh

# 无 GLFW 环境关闭桌面客户端
cmake -B build -DRTSTREAM_ENABLE_GL_CLIENT=OFF && cmake --build build -j

# 一键回环验证（服务端测试源+弱网注入 → headless 客户端 + Web 面板）
./scripts/run_loopback.sh [udp|rtp] [loss_rate]   # 如 ./scripts/run_loopback.sh udp 0.05

# Web 面板诊断（需 playwright，验证 WS 握手/帧下发/指标推送）
node scripts/ws_diag.mjs http://127.0.0.1:8080/
```

没有单元测试——验证方式是跑回环脚本观察日志指标（e2e p50/p95、lost、fec、rtt、jitter）和浏览器面板。端口约定：TCP 9000 / UDP 9001 / RTP 9002 / Web 8080。

依赖：x264、FFmpeg(libavcodec/libavutil/libswscale) 经 pkg-config 必需；GLFW/ImGui（FetchContent 拉取 v1.90.4）仅桌面客户端；GLAD 生成代码已 vendor 在 `modules/render/glad/`。

## 架构

三层结构：`app/`（入口装配）、`modules/`（业务模块）、`common/`（公共组件）。

**header-heavy 设计**：核心链路只有 `x264_encoder.cpp` 和 `avf_capture.mm` 是独立编译单元，其余全部为头文件实现（归入 INTERFACE 库 `rtstream_web`）。改头文件要重编所有依赖者。

**核心抽象接口**（新增实现时遵守）：
- `CaptureSource`（modules/capture/capture_source.h）— `start(cfg, error)` 启动内部线程，帧经回调推送
- `TransportServer`（modules/transport/transport_server.h）— 编码帧统一走 `broadcast()`，由实现负责打包/弱网处理
- 三种链路（tcp/udp/rtp）各有对应的发送端（modules/transport）与接收端（modules/receiver），经 `transport_factory` / `capture_factory` 选择

**数据流与线程模型**：
- 服务端：采集线程 → `ThreadSafeQueue<FramePtr>`（有界，满丢旧帧"追最新"）→ 编码线程 → transport.broadcast → 弱网模拟器（LossSimulator，仅 UDP/RTP 下行）
- 客户端：接收 → JitterBuffer → 解码 → 渲染 四级流水，队列解耦
- 队列关停靠 `close()` 唤醒全部阻塞点；全局停止用 `std::atomic<bool> g_stop` + 信号处理

**平台隔离**：V4L2/epool 代码用 `#if __linux__`（宏 `RTS_LINUX`）隔离；AVFoundation 在 `avf_capture.mm`（`RTSTREAM_HAVE_AVF`，-fobjc-arc），修改采集相关代码需保持两个平台都能编译。

**关键约定**：
- 编码器/预览器延迟到首帧到达才按**实际分辨率**打开（相机协商结果可能与请求不同）
- 帧携带单调时钟 `capture_us` 贯穿全链路，E2E 延时 = 渲染时刻 − 采集时刻；`Metrics` 单例（modules/stats/metrics.h）聚合所有计数
- 心智模型见 README「设计说明」「已知边界」两节——尤其 FEC 与 NACK 的先后关系、`nack_hit` 恒为 0 的原因、客户端中途入会需等 IDR（最长 2s）
