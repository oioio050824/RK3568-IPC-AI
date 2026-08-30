#!/bin/bash
# =============================================
# Phase 5: HTTP MJPEG 实时视频流
# 用法: ./deploy.sh
#
# 流程:
#   1. CMake 交叉编译 ipc_mjpeg
#   2. scp 传板子
#   3. 启动 HTTP MJPEG 服务器
#   4. 浏览器打开 http://192.168.0.200:8080/
# =============================================
set -e

PROJECT="ipc_mjpeg"
BOARD_IP="192.168.0.200"
BOARD_USER="root"
BOARD_DIR="/root/projects/ipc_camera"
CROSS=aarch64-rockchip-linux-gnu

SDK_ROOT="$HOME/RK3568/SDK/linux/rk3568_linux_sdk"
SYSROOT="$SDK_ROOT/buildroot/output/rockchip_rk3568/host/aarch64-buildroot-linux-gnu/sysroot"

echo "=== Phase 5: HTTP MJPEG Streaming ==="

# ---- 0. 检查板子 -------------------------------------------------------
echo "📡 [0/5] 检查板子连接..."
if ! ssh -o ConnectTimeout=5 ${BOARD_USER}@${BOARD_IP} "echo 板子在线" 2>/dev/null; then
    echo "❌ 无法连接到板子 ${BOARD_IP}"
    exit 1
fi

ssh ${BOARD_USER}@${BOARD_IP} "ls /dev/video0 > /dev/null 2>&1" || {
    echo "❌ 板子上 /dev/video0 不可用"
    exit 1
}
echo "✅ 板子在线, 设备就绪"

# ---- 1. 编译 -----------------------------------------------------------
echo ""
echo "🔨 [1/5] 编译 ipc_mjpeg..."

# 优先 CMake，fallback 到直接 g++
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
    SRC="src/main.cpp src/v4l2_capture.cpp src/jpeg_encoder.cpp src/http_server.cpp"
    ${CROSS}-g++ -std=c++17 -O2 \
        --sysroot="${SYSROOT}" \
        -Isrc \
        -o ${PROJECT} ${SRC} \
        -lpthread
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
    echo "   尝试启动: /etc/init.d/S99rkaiq start"
fi

# ---- 4. 检查 libjpeg (可选) --------------------------------------------
echo ""
echo "📚 [4/5] 检查板子 libjpeg..."
if ssh ${BOARD_USER}@${BOARD_IP} "ldconfig -p 2>/dev/null | grep libjpeg" 2>/dev/null; then
    echo "✅ libjpeg 可用, 会使用硬件加速的 JPEG 编码"
else
    echo "ℹ️  未检测到 libjpeg, 使用内置 tiny JPEG 编码器"
fi

# ---- 5. 启动 -----------------------------------------------------------
echo ""
echo "🚀 [5/5] 启动 HTTP MJPEG 服务器..."
echo ""
echo "  ╔══════════════════════════════════════════╗"
echo "  ║  HTTP MJPEG 实时视频流:                 ║"
echo "  ║  🌐 http://${BOARD_IP}:8080/             ║"
echo "  ║                                          ║"
echo "  ║  📸 单帧:                                ║"
echo "  ║  curl http://${BOARD_IP}:8080/snapshot    ║"
echo "  ║     > test.jpg                           ║"
echo "  ╚══════════════════════════════════════════╝"
echo ""
echo "  ⏎  按 Ctrl+C 停止服务器"
echo ""

ssh -t ${BOARD_USER}@${BOARD_IP} \
    "cd ${BOARD_DIR} && chmod +x ./${PROJECT} && ./${PROJECT} 640 480 8080"
