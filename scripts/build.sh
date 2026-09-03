#!/usr/bin/env bash
# rtstream 编译脚本（Linux / macOS 通用）
set -euo pipefail
cd "$(dirname "$0")/.."

# 依赖（Ubuntu 22.04/24.04）：
#   sudo apt install build-essential cmake pkg-config \
#        libx264-dev libavcodec-dev libavutil-dev libswscale-dev
# macOS：brew install cmake pkg-config x264 ffmpeg
# （AVFoundation 采集为 macOS 系统框架，无需额外安装）

JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo "$@"
cmake --build build -j"$JOBS"

echo
echo "build ok:"
ls -la build/app/rtstream_server build/app/rtstream_client
