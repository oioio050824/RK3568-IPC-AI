#!/bin/bash
# =============================================
# Phase 4: 多线程实时 IPC 流水线
# 用法: ./deploy.sh
#
# 流程:
#   1. 交叉编译 ipc_server (采集+编码+RTSP 三合一)
#   2. scp 传板子
#   3. 启动实时推流服务器
#   4. 用 VLC/ffplay 连接 rtsp://192.168.0.200:8554/stream
# =============================================
set -e

PROJECT="ipc_server"
BOARD_IP="192.168.0.200"
BOARD_USER="root"
BOARD_DIR="/root/projects/ipc_camera"
CROSS=aarch64-rockchip-linux-gnu

SDK_ROOT="$HOME/RK3568/SDK/linux/rk3568_linux_sdk"
SYSROOT="$SDK_ROOT/buildroot/output/rockchip_rk3568/host/aarch64-buildroot-linux-gnu/sysroot"

echo "=== Phase 4: 实时 IPC 流水线 ==="

# ---- 0. 检查板子 -------------------------------------------------------
echo "📡 [0/5] 检查板子连接..."
if ! ssh -o ConnectTimeout=5 ${BOARD_USER}@${BOARD_IP} "echo 板子在线" 2>/dev/null; then
    echo "❌ 无法连接到板子 ${BOARD_IP}"
    exit 1
fi

# 检查必要设备
ssh ${BOARD_USER}@${BOARD_IP} "ls /dev/video0 /dev/mpp_service > /dev/null 2>&1" || {
    echo "❌ 板子上 /dev/video0 或 /dev/mpp_service 不可用"
    exit 1
}
echo "✅ 板子在线, 设备就绪"

# ---- 1. 交叉编译 -------------------------------------------------------
echo ""
echo "🔨 [1/5] 交叉编译 ipc_server..."

${CROSS}-g++ --version > /dev/null 2>&1 || {
    echo "❌ 编译器 ${CROSS}-g++ 不可用, 请先 source ~/.bashrc"
    exit 1
}

${CROSS}-g++ -std=c++17 -O2 \
    --sysroot="${SYSROOT}" \
    -o ${PROJECT} main.cpp \
    -lrockchip_mpp -lpthread

echo "✅ 编译完成"
file ${PROJECT}

# ---- 2. 部署 -----------------------------------------------------------
echo ""
echo "📤 [2/5] 部署到板子..."
ssh ${BOARD_USER}@${BOARD_IP} "mkdir -p ${BOARD_DIR}"
scp ${PROJECT} ${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/
echo "✅ 部署完成"

# ---- 3. 确认 rkaiq 3A 服务在跑 ----------------------------------------
echo ""
echo "🔍 [3/5] 检查 ISP 3A 服务..."
if ssh ${BOARD_USER}@${BOARD_IP} "ps | grep rkaiq | grep -v grep" 2>/dev/null; then
    echo "✅ rkaiq_3A_server 在跑"
else
    echo "⚠️  rkaiq_3A_server 未运行, 画面可能全暗!"
    echo "   尝试启动: /etc/init.d/S99rkaiq start"
fi

# ---- 4. 启动实时推流 ---------------------------------------------------
echo ""
echo "🚀 [4/5] 启动实时推流服务器..."
echo ""
echo "  ╔══════════════════════════════════════════╗"
echo "  ║  实时 RTSP 推流地址:                    ║"
echo "  ║  rtsp://${BOARD_IP}:8554/stream          ║"
echo "  ╚══════════════════════════════════════════╝"
echo ""
echo "  📺 PC 播放 (新开终端):"
echo "     ffplay -rtsp_transport tcp rtsp://${BOARD_IP}:8554/stream"
echo "     vlc    rtsp://${BOARD_IP}:8554/stream"
echo ""
echo "  ⏎  按 Ctrl+C 停止服务器"
echo ""

# ---- 5. 前台运行 -------------------------------------------------------
ssh -t ${BOARD_USER}@${BOARD_IP} \
    "cd ${BOARD_DIR} && chmod +x ./${PROJECT} && ./${PROJECT}"
