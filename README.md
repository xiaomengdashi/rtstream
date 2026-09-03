# rtstream — 实时视频采集、编码、传输、解码与渲染系统

基于 **Linux / macOS 双平台**的实时视频端到端系统：服务端负责 **摄像头采集（Linux: V4L2 / macOS: AVFoundation）、H.264 编码（x264）与多协议分发（TCP / UDP / RTP）**；客户端负责 **网络接收、弱网恢复（FEC + NACK + Jitter Buffer）、视频解码（FFmpeg）与 OpenGL 渲染（GLFW 窗口 + GLAD 加载 + Dear ImGui 运行指标面板）**。服务端另内嵌 **Web 监控面板**，作为免安装的辅助查看端。

系统采用三层架构（入口启动 / 业务模块 / 公共组件），核心链路全异步流水线 + 线程安全队列解耦，内置可配置的丢包/延迟/抖动注入器用于弱网验证。

## 技术栈

C++17 · CMake · Linux(V4L2) · macOS(AVFoundation) · FFmpeg(libavcodec/swscale) · x264 · H.264 · TCP · UDP · RTP(RFC 6184) · OpenGL 3.3 · GLFW · GLAD · Dear ImGui · 多线程 · 线程安全队列 · FEC/NACK · Jitter Buffer · 延时统计(P50/P95/P99/Max + RTT/Jitter)

```
┌─────────────────────────── 服务端 ───────────────────────────┐
│ CaptureSource (Linux: V4L2 / macOS: AVFoundation / 测试源)    │
│   └─采集线程──> ThreadSafeQueue<Frame> ──> 编码线程 (x264)     │
│                                              │ EncodedFrame   │
│              ┌───────────────────────────────┘               │
│              ├──> TcpServer    (4B长度+帧头, 可靠)             │
│              ├──> UdpServer    (分片 + XOR FEC + NACK 重传)    │
│              │        └─> LossSimulator (丢包/延迟/抖动注入)  │
│              └──> RtpServer    (RFC 6184 FU-A + RTP seq NACK) │
│                                                                │
│  Web 监控: 采集帧 tap ──> JPEG ──> WebSocket + MJPEG ──┐       │
└──────────────────────────────────────────────────────┼────────┘
                                                       ▼
                                    浏览器面板（Canvas 渲染 + 指标/延时曲线）
┌─────────────────────────── 客户端（OpenGL 渲染 / headless）───┐
│ UdpReceiver / RtpReceiver / TcpClient                         │
│   ├─ NACK 请求 + FEC 恢复 + JitterBuffer(自适应去抖/重排)      │
│   └─out──> 解码线程 (FFmpeg) ──> 渲染线程 (GLFW+GLAD+GL YUV    │
│                                  shader + ImGui 运行指标面板)  │
└───────────────────────────────────────────────────────────────┘
```

## 目录结构（三层架构）

```
rtstream/
├── app/                      # 第一层：入口启动
│   ├── server_main.cpp       # 服务端 main：参数解析→模块装配→信号处理
│   └── client_main.cpp       # 客户端 main：OpenGL 渲染 / --headless 链路验证
├── src/                      # 第二层：业务模块 + 第三层：公共组件
│   ├── capture/              # V4L2（Linux）/ AVFoundation（macOS）/ 测试源
│   ├── encoder/              # x264 封装
│   ├── transport/            # TCP/UDP/RTP 分发 + FEC/NACK + 弱网注入
│   ├── receiver/             # 三链路接收端 + Jitter Buffer
│   ├── decoder/              # FFmpeg H.264 解码
│   ├── render/               # GLFW 窗口 + GL 渲染 + ImGui 面板
│   ├── stats/                # 指标聚合
│   ├── web/                  # WebSocket/HTTP 服务 + 浏览器面板
│   └── common/               # 第三层：公共组件（队列/日志/时钟/socket/CRC32/SHA1）
├── cmake/deps.cmake          # 第三方依赖统一 FetchContent 声明
├── scripts/                  # build.sh / run_loopback.sh / ws_diag.mjs
├── CMakeLists.txt
└── LICENSE · .gitignore · .clang-format · README.md
```

| 层 | 目录 | 内容 |
| --- | --- | --- |
| 入口启动 | `app/` | `server_main.cpp`（装配+信号处理）、`client_main.cpp`（OpenGL 渲染客户端 / --headless 链路验证） |
| 业务模块 | `src/` | capture / encoder / transport / receiver / decoder / render / stats / web |
| 公共组件 | `src/common/` | 线程安全队列、线程池、日志、单调时钟、socket 工具、CRC32、SHA1/Base64、协议常量 |
| 第三方 | `cmake/deps.cmake` | glad2 / CLI11 / nlohmann-json / Dear ImGui，全部 FetchContent 按需拉取 |

关键模块：

- `src/capture` — `v4l2_capture`（Linux mmap + select 轮询，MJPEG/YUYV 自动协商）、`avf_capture`（macOS AVFoundation，NV12→I420，含 TCC 相机授权流程）、`test_source`（程序化彩条+运动方块+秒表，无摄像头可演示）
- `src/encoder` — `x264_encoder`：zerolatency 预设、CBR/CRF、单线程零延迟、SPS/PPS 缓存
- `src/transport` — 三链路统一 `TransportServer` 接口
  - `udp_server`：自研 24B 分片协议（帧号/组号/分片号/采集时间戳），每帧 K+1 组（K 数据片 + 1 XOR 恢复片），`NackManager` 重传缓存
  - `rtp_server`：RFC 6184（Single NALU / FU-A）、RTP 序号 NACK、组级 XOR FEC
  - `loss_simulator`：按概率丢包 + 延迟/抖动延迟队列（验证 FEC/NACK/JB）
- `src/receiver` — `jitter_buffer`（seq 重排、缺口检测→NACK 节流、RFC3550 风格抖动估计、自适应去抖深度 `3×jitter+RTT`、超时跳帧、下游饥饿直通）、`udp_receiver`（分片重组+FEC 恢复+分片级 NACK）、`rtp_receiver`（包级 JB+帧组装+帧内单包 FEC 恢复）、`tcp_client`
- `src/decoder` — `ffmpeg_decoder`：H.264 Annex-B → I420（LOW_DELAY 配合 zerolatency）
- `src/render` — `gl_window`（GLFW 窗口+3.3 Core 上下文）、`gl_renderer`（GL_RED 纹理 ×3 + shader BT.601 色度转换）、`imgui_panel`（运行指标面板）；GL 函数经 **GLAD**（glad2，FetchContent 拉取 + 配置期生成）加载
- `src/web` — 自实现 WebSocket（RFC 6455 握手/帧解析）、HTTP 静态 + MJPEG、JPEG 编码（swscale+mjpeg）、ImGui 风格浏览器面板（服务端辅助监控端）

## 依赖策略

| 依赖 | 引入方式 | 用途 |
| --- | --- | --- |
| x264 / FFmpeg(libavcodec·swscale) | 系统包管理器 | 编码 / 解码 / 缩放 / JPEG |
| GLFW | 系统包管理器 | 窗口与 GL 上下文（桌面客户端） |
| Dear ImGui v1.90.4 | FetchContent（git） | 桌面客户端指标面板 |
| glad2 v2.0.8 | FetchContent（git）+ 配置期代码生成 | GL 3.3 Core 函数加载 |
| CLI11 v2.4.2 | FetchContent（官方 release 单头工件） | 命令行解析（两个入口的参数/help/校验） |
| nlohmann/json v3.11.3 | FetchContent（git） | 指标 JSON 序列化 |

所有第三方库统一在 `cmake/deps.cmake` 中经 FetchContent 拉取，仓库不 vendor 任何第三方源码。首次 `cmake` 配置需联网；glad2 的代码生成需要 `python3`（及 `pip3 install jinja2`），生成产物缓存在 build 目录，之后增量编译不再需要。

**有意自研**（替换得不偿失）：WebSocket 握手/帧解析与 HTTP 静态服务（换 Boost.Beast/cpp-httplib 需引入 Boost 或放弃零构建）；SHA1/Base64（仅 WebSocket 握手使用，OpenSSL 过重）；CRC32（zlib 过重）；线程安全队列/线程池（std 原语足够，无锁队列库无必要）。

## 编译

### Ubuntu 22.04 / 24.04

```bash
sudo apt install build-essential cmake pkg-config \
     libx264-dev libavcodec-dev libavutil-dev libswscale-dev \
     libglfw3-dev          # 桌面 OpenGL/ImGui 客户端
./scripts/build.sh
# 无 GLFW/离线环境可关闭桌面客户端：cmake -B build -DRTSTREAM_ENABLE_GL_CLIENT=OFF
# （第三方库首次 cmake 配置时经 FetchContent 联网拉取；glad2 代码生成需 python3 + jinja2）
```

### macOS（Apple Silicon / Intel）

```bash
brew install cmake pkg-config x264 ffmpeg glfw
./scripts/build.sh        # AVFoundation 采集为系统框架，无需额外依赖
```

产物：`build/app/rtstream_server`、`build/app/rtstream_client`。

## 运行

服务端（测试源 + UDP + 5% 丢包 + 20ms 延迟 + 15ms 抖动注入）：

```bash
build/app/rtstream_server --source test --transport udp \
    --width 1280 --height 720 --fps 30 --rc cbr --bitrate 4000 \
    --loss 0.05 --delay-ms 20 --jitter-ms 15
```

- `--transport tcp|udp|rtp` 切换链路
- `--source auto`（默认）：Linux 优先 V4L2，macOS 优先 AVFoundation，失败回落测试源；`--source v4l2` / `--source avf` 为对应平台的强制模式
- **macOS 摄像头**：首次运行系统会弹"相机访问"授权窗（归属运行它的终端 App，如 Terminal/iTerm），允许后即可出画面；被拒时去 系统设置→隐私与安全性→相机 放行
- 编码器/预览器在首帧到达时按**实际分辨率**打开（相机实际输出可能与请求不同）
- Web 面板：浏览器打开 `http://127.0.0.1:8080/`（`--port-web` 可改）

客户端（OpenGL 渲染 + ImGui 指标面板；`--headless` 为无窗口链路验证模式）：

```bash
build/app/rtstream_client --server 127.0.0.1 --transport udp
build/app/rtstream_client --server 127.0.0.1 --transport rtp --headless
```

一键回环演示：`./scripts/run_loopback.sh udp 0.05`

> 端口约定：TCP 9000 / UDP 9001 / RTP 9002 / Web 8080（均可用 `--port-*` 修改）。
> 客户端通过首个 Ping 包向 UDP/RTP 服务器注册地址，NACK/Pong 走同一 socket。

## 指标与弱网验证

服务端每秒向 Web 面板推送指标 JSON（`src/stats/metrics.h` 聚合）：

| 指标 | 含义 |
| --- | --- |
| capture/encoded_frames | 采集、编码帧计数（→帧率） |
| net_packets_sent / bytes | 发送包数/字节（→码率） |
| net_fec_sent | FEC 恢复包发送数（约每帧 +1） |
| sim_dropped | 弱网模拟器注入的丢包数 |
| nack_received / retransmit_sent | NACK 请求条目数 / 重传包数 |
| net_packets_recv / net_lost | 接收包数 / 判定丢失未恢复数 |
| fec_recovered / nack_hit | FEC 恢复成功 / NACK 重传命中 |
| rtt_ms / jitter_ms | RTT（Ping-Pong EWMA）/ RFC3550 风格抖动估计 |
| lat_p50/p95/p99/max | 端到端延时（渲染时刻 − 采集时刻，单调钟；同机回环有效） |

弱网实验建议：

```bash
# 5% 丢包：观察 fec_recovered 与 nack_hit 上升、net_lost 接近 0
./scripts/run_loopback.sh udp 0.05
# 15% 突发丢包：FEC 单包恢复不足，主要依赖 NACK 重传
./scripts/run_loopback.sh udp 0.15
# RTP 链路：RFC6184 打包 + 包级 Jitter Buffer
./scripts/run_loopback.sh rtp 0.1
# TCP 链路对照：无 FEC/NACK，观察队头阻塞下的延时上升
build/app/rtstream_server --transport tcp && build/app/rtstream_client --transport tcp
```

浏览器面板中"端到端延时"由浏览器墙钟与服务端帧内嵌墙钟差值得出（同机回环可信）。

## 设计说明

- **线程模型**：采集 → 编码 → 分发全部经有界 `ThreadSafeQueue` 解耦，满时丢旧帧（实时流"追最新"策略），关停用 `close()` 唤醒全部阻塞点；客户端为 接收 → JitterBuffer → 解码 → 渲染 四级流水。
- **UDP 自研协议**（24B 头）：magic/版本/载荷类型、FEC 组号=帧号、分片号、采集时间戳（32 位微秒低段，接收端做回绕扩展）、关键帧标志。
- **FEC**：XOR 方案（K 数据 + 1 恢复），组内丢 1 可恢复，丢 ≥2 由 NACK 兜底；UDP 链路按帧分组，RTP 链路对完整 RTP 包异或（可还原整个 RTP 包）。
- **NACK**：接收端按序号缺口触发（10ms 节流、序号大跳直接对齐防风暴）；发送端维护最近 4096 包重传缓存；重传包与数据包走同一弱网路径。
- **Jitter Buffer**：出队条件 = 头部就绪且等待 ≥ 目标深度，或下游饥饿直通（压延迟）；头部缺失超时（2×目标+50ms）跳帧；目标深度 = clamp(3×抖动 + RTT, 15, 250) ms。
- **延时测量**：帧携带采集单调时间戳贯穿全链路；E2E = 渲染时刻 − 采集时刻，环形缓冲 1024 样本计算 P50/P95/P99/Max。
- **桌面渲染**：GL 3.3 Core，解码输出的 I420 直传 GPU——三张 GL_RED 纹理 + shader BT.601 色度转换，无二次压缩；GL 函数经 **GLAD**（`gladLoadGLLoader` + `glfwGetProcAddress`）加载；ImGui 面板与视频画面在同一 GL 上下文逐帧叠加。
- **Web 渲染**：服务端把首帧后按实际分辨率缩放的 JPEG 经 WebSocket 二进制帧下发，浏览器 Canvas 绘制；指标 JSON 每秒推送，E2E 延时由浏览器墙钟与帧内嵌墙钟差值实时计算（同机回环可信），另有 MJPEG 通道兜底。

## 已知边界

- 弱网参数只作用于 UDP/RTP 下行（重传包也走同一路径，贴近真实网络）；TCP 链路依赖内核重传。
- E2E 延时统计在跨机部署时需时钟对齐（回环/同机即精确）。
- 客户端中途入会需等待下一个 IDR（默认 GOP=60 → 最长 2s）才能出画面，此前解码器打印 `non-existing PPS` 属预期行为。
- `nack_hit` 在接收端无法与首传包区分，恒为 0；重传工作量看服务端的 `nack_received`/`retransmit_sent`。
- FEC 与 NACK 的先后：UDP 链路在组帧时立即尝试 FEC（缺 1 片即恢复），缺多片才走 NACK；RTP 链路按帧收尾时若恰缺 1 包且 parity 已到则恢复，回环下 NACK 重传极快（<1ms），FEC 大多被"抢先"，真实网络（RTT 高）时 FEC 生效——可用 `--delay-ms 45` 复现 FEC 恢复。
- Jitter Buffer 的抖动估计（RFC 3550 风格）只在按序到达时更新，避免 NACK 重传的迟到到达污染自适应深度。
- V4L2 采集当前将 MJPEG/YUYV 帧原样透传，而编码器要求 I420——接真实 Linux 摄像头时需在采集层补一步 swscale/libavcodec 格式转换（AVFoundation 路径已在采集层完成 NV12→I420，无此问题）。

## 验证记录

本工程核心路径已在 macOS + 本机回环完成真实运行验证（`clang++ -std=c++17`，链接 Homebrew 的 x264/FFmpeg，AVFoundation 为系统框架）：

| 场景 | 结果 |
| --- | --- |
| UDP + 10% 丢包 + 15ms 延迟 + 10ms 抖动 | 稳定 30fps 解码，E2E P50≈27ms / P95≈41ms，`lost=0`，FEC 恢复 ≈ 每帧 1 次，Jitter 估计 ≈5ms |
| RTP（RFC 6184）同上 | 稳定 30fps，`lost≤2` 包；加大下行延迟后 FEC 恢复路径生效（fec>0） |
| TCP 对照 | 稳定 30fps，E2E≈4ms（回环） |
| Web 面板 | HTTP 200；WS 握手 101，`Sec-WebSocket-Accept` 与 RFC 6455 规范测试向量一致；MJPEG 通道持续出流；无头浏览器（Playwright/Chromium）端到端验证：WS 帧下发、指标 JSON 推送、RTT echo、画面渲染全部正常（`scripts/ws_diag.mjs` 可复检） |
| AVFoundation 摄像头路径 | 会话启动、设备枚举（FaceTime HD / iPhone 连续互通相机）、TCC 授权状态机（notDetermined→弹窗等待→denied 报错）均验证；帧回调因 agent 无 UI 宿主无法弹窗授权而未出帧——从用户终端运行触发授权后即走通（代码路径与探针一致） |
| `--source auto` 回落 + 奇数分辨率适配 | 请求 945x531 自动取整 944x530，编码器/预览器按首帧实际分辨率打开，回环 30fps 零丢失 |
| 桌面 GL 客户端（GLFW 3.5 + GLAD + ImGui 1.90） | macOS 本机编译零告警；窗口客户端实机运行：GL 上下文/glad 加载/ImGui 初始化/渲染循环全部正常（等待 IDR 期间的 `non-existing PPS` 为预期行为）；headless 模式回环 30fps、10% 丢包零丢失 |
| V4L2 路径 | 仅 Linux 可编译运行（`#if __linux__` 隔离），流程遵循 V4L2 mmap 流式采集标准步骤 |
