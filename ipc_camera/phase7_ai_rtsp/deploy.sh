#!/bin/bash
# =============================================
# Phase 7.3: H.264 + RTSP + RKNN 实时目标检测
# 用法: ./deploy.sh
#
# 流程:
#   1. 交叉编译 ipc_ai (直连 g++, 比 cmake 更省事, 与 phase7 一致)
#   2. scp 传板子
#   3. 启动 RTSP 服务器 (带 AI 检测框)
#   4. PC 端: ffplay -rtsp_transport tcp rtsp://192.168.0.200:8554/stream
# =============================================
set -e

PROJECT="ipc_ai"
BOARD_IP="192.168.0.200"
BOARD_USER="root"
BOARD_DIR="/root/projects/ipc_camera"
CROSS=aarch64-rockchip-linux-gnu

SDK_ROOT="$HOME/RK3568/SDK/linux/rk3568_linux_sdk"
SYSROOT="$SDK_ROOT/buildroot/output/rockchip_rk3568/host/aarch64-buildroot-linux-gnu/sysroot"
RKNN_INC="$SDK_ROOT/external/rknpu2/runtime/RK356X/Linux/librknn_api/include"
RKNN_LIB="$SDK_ROOT/external/rknpu2/runtime/RK356X/Linux/librknn_api/aarch64"
FT_INC="$SYSROOT/usr/include/freetype2"     # FreeType 头 (标签文字)

FONT="jc_round_600.ttf"                    # 标签字体 (江城圆体 600W, 放本目录)

SRC="src/main.cpp src/v4l2_capture.cpp src/h264_encoder.cpp src/rtsp_server.cpp src/ai_detector.cpp src/text_renderer.cpp"

echo "=== Phase 7.3: H.264 + RTSP + RKNN AI ==="

# ---- 0. 检查板子 -------------------------------------------------------
echo "📡 [0/4] 检查板子连接..."
if ! ssh -o ConnectTimeout=5 ${BOARD_USER}@${BOARD_IP} "echo 板子在线" 2>/dev/null; then
    echo "❌ 无法连接板子 ${BOARD_IP}"
    exit 1
fi
ssh ${BOARD_USER}@${BOARD_IP} "ls /dev/video0 /dev/mpp_service /yolov5s.rknn > /dev/null 2>&1" || {
    echo "❌ 板子上 /dev/video0 或 /dev/mpp_service 或 /yolov5s.rknn 不可用"
    exit 1
}
echo "✅ 板子在线, 设备/模型就绪"

# ---- 1. 编译 -----------------------------------------------------------
echo ""
echo "🔨 [1/4] 编译 ${PROJECT}..."
${CROSS}-g++ --version > /dev/null 2>&1 || {
    echo "❌ 编译器 ${CROSS}-g++ 不可用, 请先 source ~/.bashrc"
    exit 1
}

# RGA 检测: sysroot 有 librga.so 才启用硬件转换, 否则纯 CPU
RGA_DEFINE=""; RGA_LIB=""
if ls "$SYSROOT/usr/lib/librga.so"* >/dev/null 2>&1; then
    RGA_DEFINE="-DHAS_RGA"; RGA_LIB="-lrga"
    echo "✅ sysroot 有 librga → 启用 RGA 硬件转换"
else
    echo "⚠️  sysroot 无 librga.so → 用 CPU 转换 (可跑, 但 30fps 会吃满 CPU)"
fi

${CROSS}-g++ -std=c++17 -O2 \
    --sysroot="${SYSROOT}" \
    -I"${RKNN_INC}" -I"${FT_INC}" -Isrc \
    ${RGA_DEFINE} \
    -o ${PROJECT} ${SRC} \
    -L"${RKNN_LIB}" -lrknnrt -lrknn_api \
    ${RGA_LIB} -lrockchip_mpp -lfreetype -lpthread

echo "✅ 编译完成"
file ${PROJECT}

# ---- 2. 部署 -----------------------------------------------------------
echo ""
echo "📤 [2/4] 部署到板子..."
ssh ${BOARD_USER}@${BOARD_IP} "mkdir -p ${BOARD_DIR}"
scp ${PROJECT} ${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/
echo "✅ 部署完成"

# 字体 (可选): 本地有才传, 板上已有则跳过
if [ -f "${FONT}" ]; then
    if ssh ${BOARD_USER}@${BOARD_IP} "ls ${BOARD_DIR}/${FONT} >/dev/null 2>&1"; then
        echo "✅ 板上已有字体, 跳过上传"
    else
        echo "📤 上传字体 ${FONT} (约15MB)..."
        scp "${FONT}" ${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/
    fi
else
    echo "⚠️  本地无 ${FONT}, 标签文字不显示 (把江城圆体600W重命名成 ${FONT} 放本目录即可)"
fi

# ---- 3. 检查运行时库 ----------------------------------------------------
echo ""
echo "🔍 [3/4] 检查板子运行时库..."
ssh ${BOARD_USER}@${BOARD_IP} "ls /usr/lib/librknnrt.so /usr/lib/librknn_api.so /usr/lib/librga.so 2>&1"

# ---- 4. 运行 -----------------------------------------------------------
echo ""
echo "🚀 [4/4] 启动 (Ctrl+C 停止)..."
echo ""
echo "  ╔══════════════════════════════════════════╗"
echo "  ║  RTSP 实时视频流 (带 AI 检测框):         ║"
echo "  ║  rtsp://${BOARD_IP}:8554/stream          ║"
echo "  ║                                          ║"
echo "  ║  📺 PC 播放:                             ║"
echo "  ║  ffplay -rtsp_transport tcp \\           ║"
echo "  ║    rtsp://${BOARD_IP}:8554/stream         ║"
echo "  ╚══════════════════════════════════════════╝"
echo ""

ssh -t ${BOARD_USER}@${BOARD_IP} \
    "cd ${BOARD_DIR} && chmod +x ./${PROJECT} && ./${PROJECT} 640 480 8554"
