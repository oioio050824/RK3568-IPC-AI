#!/bin/bash
# =============================================
# Phase 7.1: RKNN 模型探测 + 首帧推理
# 用法: ./deploy.sh [--synthetic]
#
# 流程:
#   1. 交叉编译 rknn_demo (需 rknn_api.h + im2d.h + librknnrt/librknn_api + librga)
#   2. scp 传板子
#   3. 板子上运行 (默认摄像头模式, --synthetic 跳过摄像头/RGA 只测 NPU)
# =============================================
set -e

PROJECT="rknn_demo"
BOARD_IP="192.168.0.200"
BOARD_USER="root"
BOARD_DIR="/root/projects/ipc_camera"
CROSS=aarch64-rockchip-linux-gnu

SDK_ROOT="$HOME/RK3568/SDK/linux/rk3568_linux_sdk"
SYSROOT="$SDK_ROOT/buildroot/output/rockchip_rk3568/host/aarch64-buildroot-linux-gnu/sysroot"
RKNN_INC="$SDK_ROOT/external/rknpu2/runtime/RK356X/Linux/librknn_api/include"
RKNN_LIB="$SDK_ROOT/external/rknpu2/runtime/RK356X/Linux/librknn_api/aarch64"

echo "=== Phase 7.1: RKNN 探测 + 首帧推理 ==="

# ---- 0. 检查板子 -------------------------------------------------------
echo "📡 [0/4] 检查板子连接..."
if ! ssh -o ConnectTimeout=5 ${BOARD_USER}@${BOARD_IP} "echo 板子在线" 2>/dev/null; then
    echo "❌ 无法连接板子 ${BOARD_IP}"
    exit 1
fi
echo "✅ 板子在线"

# ---- 1. 编译 -----------------------------------------------------------
echo ""
echo "🔨 [1/4] 编译 ${PROJECT}..."
${CROSS}-g++ --version > /dev/null 2>&1 || {
    echo "❌ 编译器 ${CROSS}-g++ 不可用, 请先 source ~/.bashrc"
    exit 1
}

# 注: RGA 暂用 CPU 转换替代 (librga 2.x handle 问题 7.3 再解), 故不链 -lrga
${CROSS}-g++ -std=c++17 -O2 \
    --sysroot="${SYSROOT}" \
    -I"${RKNN_INC}" -Isrc \
    -o ${PROJECT} src/main.cpp \
    -L"${RKNN_LIB}" -lrknnrt -lrknn_api -lpthread

echo "✅ 编译完成"
file ${PROJECT}

# ---- 2. 部署 -----------------------------------------------------------
echo ""
echo "📤 [2/4] 部署到板子..."
ssh ${BOARD_USER}@${BOARD_IP} "mkdir -p ${BOARD_DIR}"
scp ${PROJECT} ${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/
echo "✅ 部署完成"

# ---- 3. 检查运行时库 ----------------------------------------------------
echo ""
echo "🔍 [3/4] 检查板子运行时库..."
ssh ${BOARD_USER}@${BOARD_IP} "ls /usr/lib/librknnrt.so /usr/lib/librknn_api.so /usr/lib/librga.so 2>&1"

# ---- 4. 运行 -----------------------------------------------------------
echo ""
echo "🚀 [4/4] 运行..."
SYNTH="${1:-}"
ssh -t ${BOARD_USER}@${BOARD_IP} \
    "cd ${BOARD_DIR} && chmod +x ./${PROJECT} && ./${PROJECT} /yolov5s.rknn ${SYNTH}"
