#!/usr/bin/env bash
# 回环演示：服务端（测试源 + 弱网注入）→ UDP/RTP 客户端 headless + Web 面板
# 用法: ./scripts/run_loopback.sh [udp|rtp] [loss_rate]
set -euo pipefail
cd "$(dirname "$0")/.."

TRANSPORT="${1:-udp}"
LOSS="${2:-0.05}"
WEB_PORT=8080

if [ ! -x build/app/rtstream_server ]; then
    echo "先运行 scripts/build.sh"
    exit 1
fi

echo "==> 启动服务端: transport=$TRANSPORT loss=$LOSS web=http://127.0.0.1:$WEB_PORT"
build/app/rtstream_server \
    --source test --transport "$TRANSPORT" \
    --width 1280 --height 720 --fps 30 \
    --bitrate 4000 --rc cbr \
    --loss "$LOSS" --delay-ms 20 --jitter-ms 15 \
    --port-web "$WEB_PORT" &
SERVER_PID=$!
trap 'kill $SERVER_PID 2>/dev/null || true' EXIT

sleep 1
echo "==> 启动客户端 (headless, 与服务端同机回环)"
build/app/rtstream_client --server 127.0.0.1 --transport "$TRANSPORT" &
CLIENT_PID=$!
trap 'kill $SERVER_PID $CLIENT_PID 2>/dev/null || true' EXIT

echo
echo "==> 浏览器打开 http://127.0.0.1:$WEB_PORT/ 查看画面与指标面板"
echo "    客户端日志在上方滚动显示 e2e 延时统计；Ctrl-C 退出"
wait
