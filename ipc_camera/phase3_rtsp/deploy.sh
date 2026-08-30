#!/bin/bash
# =============================================
# Phase 3: RTSP 推流
# 用法: ./deploy.sh
#
# 完整流程:
#   1. 交叉编译 capture_multi (多帧采集) + rtsp_server (推流)
#   2. 部署到板子
#   3. 采集30帧 NV12 → MPP硬编码 → 多帧 .h264
#   4. 在板子上启动 RTSP 服务器
#   5. 用 VLC/ffplay 播放 rtsp://192.168.0.200:8554/stream
# =============================================
set -e

PROJECT="rtsp_server"
BOARD_IP="192.168.0.200"
BOARD_USER="root"
BOARD_DIR="/root/projects/ipc_camera"
CROSS=aarch64-rockchip-linux-gnu

# ---- SDK 路径 (VM 上) -------------------------------------------------
SDK_ROOT="$HOME/RK3568/SDK/linux/rk3568_linux_sdk"
SYSROOT="$SDK_ROOT/buildroot/output/rockchip_rk3568/host/aarch64-buildroot-linux-gnu/sysroot"

echo "=== Phase 3: RTSP 推流 ==="

# ---- 0. 检查板子 -------------------------------------------------------
echo "📡 [0/8] 检查板子连接..."
if ! ssh -o ConnectTimeout=5 ${BOARD_USER}@${BOARD_IP} "echo 板子在线" 2>/dev/null; then
    echo "❌ 无法连接到板子 ${BOARD_IP}"
    exit 1
fi
echo "✅ 板子在线"

# ---- 1. 交叉编译 capture_multi -----------------------------------------
echo ""
echo "🔨 [1/8] 编译 capture_multi (连续采集)..."
${CROSS}-g++ -std=c++17 -O2 -o capture_multi capture_multi.cpp
echo "✅ capture_multi 编译完成"
file capture_multi

# ---- 2. 交叉编译 rtsp_server -------------------------------------------
echo ""
echo "🔨 [2/8] 编译 rtsp_server (RTSP推流)..."
${CROSS}-g++ -std=c++17 -O2 -o ${PROJECT} main.cpp
echo "✅ rtsp_server 编译完成"
file ${PROJECT}

# ---- 3. 部署到板子 -----------------------------------------------------
echo ""
echo "📤 [3/8] 部署到板子..."
ssh ${BOARD_USER}@${BOARD_IP} "mkdir -p ${BOARD_DIR}"
scp capture_multi ${PROJECT} ${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/
echo "✅ 部署完成"

# ---- 4. 采集 30 帧 (跳过如果已存在) ------------------------------------
echo ""
echo "🎬 [4/8] 采集 30 帧 NV12..."

NEED_CAPTURE=true
if ssh ${BOARD_USER}@${BOARD_IP} "test -f /root/capture_30.yuv" 2>/dev/null; then
    EXISTING=$(ssh ${BOARD_USER}@${BOARD_IP} "wc -c < /root/capture_30.yuv" 2>/dev/null || echo "0")
    EXPECTED=$((640 * 480 * 3 / 2 * 30))   # 13,824,000
    if [ "$EXISTING" -ge "$EXPECTED" ] 2>/dev/null; then
        echo "  已有 capture_30.yuv (${EXISTING} 字节), 跳过采集"
        NEED_CAPTURE=false
    fi
fi

if $NEED_CAPTURE; then
    ssh -t ${BOARD_USER}@${BOARD_IP} \
        "cd ${BOARD_DIR} && chmod +x ./capture_multi && ./capture_multi /dev/video0 /root/capture_30.yuv 640 480 30"
else
    echo "  ✅ 使用已有文件"
fi

# ---- 5. MPP 编码 30 帧 (跳过如果已存在) --------------------------------
echo ""
echo "🔨 [5/8] MPP 编码 (NV12 → H.264)..."

NEED_ENCODE=true
if ssh ${BOARD_USER}@${BOARD_IP} "test -f /root/capture_30.h264" 2>/dev/null; then
    H264_SIZE=$(ssh ${BOARD_USER}@${BOARD_IP} "wc -c < /root/capture_30.h264" 2>/dev/null || echo "0")
    if [ "$H264_SIZE" -gt 10000 ] 2>/dev/null; then
        echo "  已有 capture_30.h264 (${H264_SIZE} 字节), 跳过编码"
        NEED_ENCODE=false
    fi
fi

if $NEED_ENCODE; then
    # 确保编码器在板子上
    if ! ssh ${BOARD_USER}@${BOARD_IP} "test -f ${BOARD_DIR}/encoder" 2>/dev/null; then
        echo "  编译 Phase 2 encoder..."
        ${CROSS}-g++ -std=c++17 -O2 \
            --sysroot="${SYSROOT}" \
            -o encoder ../phase2_mpp_encode/main.cpp \
            -lrockchip_mpp
        scp encoder ${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/
    fi
    ssh -t ${BOARD_USER}@${BOARD_IP} \
        "cd ${BOARD_DIR} && chmod +x ./encoder && ./encoder /root/capture_30.yuv /root/capture_30.h264 640 480"
else
    echo "  ✅ 使用已有文件"
fi

# ---- 6. 拉回 PC (可选) -------------------------------------------------
echo ""
echo "📥 [6/8] 拉回 H.264 文件..."
scp ${BOARD_USER}@${BOARD_IP}:/root/capture_30.h264 ./capture_30.h264 2>/dev/null || true
echo "✅ 已拉回: $(pwd)/capture_30.h264"

# ---- 7. 验证多帧 H.264 -------------------------------------------------
echo ""
echo "🔍 [7/8] 验证多帧 H.264..."
H264_LOCAL_SIZE=$(wc -c < capture_30.h264 2>/dev/null || echo "0")
echo "  文件大小: ${H264_LOCAL_SIZE} 字节"

# 统计帧类型
echo "  帧类型:"
ffprobe -v quiet -show_frames capture_30.h264 2>/dev/null | grep "pict_type" | sort | uniq -c || \
    echo "    (ffprobe 不可用, 跳过)"

# ---- 8. 启动 RTSP 服务器 -----------------------------------------------
echo ""
echo "🚀 [8/8] 启动 RTSP 推流服务器..."
echo ""
echo "  ╔══════════════════════════════════════════╗"
echo "  ║  RTSP 推流地址:                         ║"
echo "  ║  rtsp://${BOARD_IP}:8554/stream          ║"
echo "  ╚══════════════════════════════════════════╝"
echo ""
echo "  📺 PC 播放 (新开终端):"
echo "     ffplay rtsp://${BOARD_IP}:8554/stream"
echo "     vlc    rtsp://${BOARD_IP}:8554/stream"
echo ""
echo "  ⏎  按 Ctrl+C 停止服务器"
echo ""

# 前台运行 RTSP 服务器
ssh -t ${BOARD_USER}@${BOARD_IP} \
    "cd ${BOARD_DIR} && chmod +x ./${PROJECT} && ./${PROJECT} /root/capture_30.h264 8554"
