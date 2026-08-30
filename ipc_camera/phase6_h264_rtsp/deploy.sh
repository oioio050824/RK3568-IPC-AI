#!/bin/bash
# =============================================
# Phase 6: H.264 + RTSP 实时视频流
# 用法: ./deploy.sh
#
# 流程:
#   1. CMake 交叉编译 ipc_rtsp
#   2. scp 传板子
#   3. 启动 RTSP 服务器
#   4. PC 端: ffplay -rtsp_transport tcp rtsp://192.168.0.200:8554/stream
# =============================================
set -e

PROJECT="ipc_rtsp"
BOARD_IP="192.168.0.200"
BOARD_USER="root"
BOARD_DIR="/root/projects/ipc_camera"
CROSS=aarch64-rockchip-linux-gnu

SDK_ROOT="$HOME/RK3568/SDK/linux/rk3568_linux_sdk"
SYSROOT="$SDK_ROOT/buildroot/output/rockchip_rk3568/host/aarch64-buildroot-linux-gnu/sysroot"

echo "=== Phase 6: H.264 + RTSP Streaming ==="

# ---- 0. 检查板子 -------------------------------------------------------
echo "📡 [0/5] 检查板子连接..."
if ! ssh -o ConnectTimeout=5 ${BOARD_USER}@${BOARD_IP} "echo 板子在线" 2>/dev/null; then
    echo "❌ 无法连接到板子 ${BOARD_IP}"
    exit 1
fi

ssh ${BOARD_USER}@${BOARD_IP} "ls /dev/video0 /dev/mpp_service > /dev/null 2>&1" || {
    echo "❌ 板子上 /dev/video0 或 /dev/mpp_service 不可用"
    exit 1
}
echo "✅ 板子在线, 设备就绪"

# ---- 1. 编译 -----------------------------------------------------------
echo ""
echo "🔨 [1/5] 编译 ipc_rtsp..."

if command -v cmake &> /dev/null; then
    cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    cp build/${PROJECT} .
else
    echo "⚠️  cmake 不可用, 使用 g++ 直接编译"
    ${CROSS}-g++ --version > /dev/null 2>&1 || {
        echo "❌ 编译器 ${CROSS}-g++ 不可用, 请先 source ~/.bashrc"
        exit 1
    }
    SRC="src/main.cpp src/v4l2_capture.cpp src/h264_encoder.cpp src/rtsp_server.cpp"
    ${CROSS}-g++ -std=c++17 -O2 \
        --sysroot="${SYSROOT}" \
        -Isrc \
        -o ${PROJECT} ${SRC} \
        -lrockchip_mpp -lpthread
fi

echo "✅ 编译完成"
file ${PROJECT}

# ---- 2. 部署 -----------------------------------------------------------
echo ""
echo "📤 [2/5] 部署到板子..."
ssh ${BOARD_USER}@${BOARD_IP} "mkdir -p ${BOARD_DIR}"
scp ${PROJECT} ${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/
echo "✅ 部署完成"

# ---- 3. 检查 rkaiq -----------------------------------------------------
echo ""
echo "🔍 [3/5] 检查 ISP 3A 服务..."
if ssh ${BOARD_USER}@${BOARD_IP} "ps | grep rkaiq | grep -v grep" 2>/dev/null; then
    echo "✅ rkaiq_3A_server 在跑"
else
    echo "⚠️  rkaiq_3A_server 未运行, 画面可能全暗!"
fi

# ---- 4. 检查 mpp_service ------------------------------------------------
echo ""
echo "🔍 [4/5] 检查 MPP 服务..."
if ssh ${BOARD_USER}@${BOARD_IP} "ls /dev/mpp_service > /dev/null 2>&1"; then
    echo "✅ /dev/mpp_service 就绪"
else
    echo "❌ /dev/mpp_service 不可用, MPP 编码无法工作"
    exit 1
fi

# ---- 5. 启动 -----------------------------------------------------------
echo ""
echo "🚀 [5/5] 启动 RTSP 服务器..."
echo ""
echo "  ╔══════════════════════════════════════════╗"
echo "  ║  RTSP 实时视频流:                       ║"
echo "  ║  rtsp://${BOARD_IP}:8554/stream          ║"
echo "  ║                                          ║"
echo "  ║  📺 PC 播放:                             ║"
echo "  ║  ffplay -rtsp_transport tcp \\           ║"
echo "  ║    rtsp://${BOARD_IP}:8554/stream         ║"
echo "  ║                                          ║"
echo "  ║  🎬 VLC:                                 ║"
echo "  ║  媒体→打开网络串流→输入上述地址          ║"
echo "  ╚══════════════════════════════════════════╝"
echo ""
echo "  ⏎  按 Ctrl+C 停止服务器"
echo ""

ssh -t ${BOARD_USER}@${BOARD_IP} \
    "cd ${BOARD_DIR} && chmod +x ./${PROJECT} && ./${PROJECT} 640 480 8554"
