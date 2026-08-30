#!/bin/bash
# =============================================
# Phase 1: V4L2 采集 → 保存 NV12 帧
# 用法: ./deploy.sh
# =============================================
set -e

PROJECT="capture"
BOARD_IP="192.168.0.200"
BOARD_USER="root"
BOARD_DIR="/root/projects/ipc_camera"
CROSS=aarch64-rockchip-linux-gnu

echo "🔨 [1/3] 交叉编译..."
${CROSS}-g++ -std=c++17 -O2 -o ${PROJECT} main.cpp
echo "✅ 编译完成"

echo "📤 [2/3] 传到板子..."
ssh ${BOARD_USER}@${BOARD_IP} "mkdir -p ${BOARD_DIR}"
scp ${PROJECT} ${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/
echo "✅ 传输完成"

echo "🚀 [3/3] 运行..."
ssh -t ${BOARD_USER}@${BOARD_IP} "cd ${BOARD_DIR} && ./${PROJECT} /dev/video0 /root/capture.yuv 640 480"

echo ""
echo "📥 拉回 PC 查看:"
echo "   scp ${BOARD_USER}@${BOARD_IP}:/root/capture.yuv ."
echo "   ffplay -f rawvideo -pixel_format nv12 -video_size 640x480 capture.yuv"
