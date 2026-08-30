#!/bin/bash
# =============================================
# Phase 2: MPP H.264 硬编码
# 用法: ./deploy.sh
#
# 流程:
#   1. 交叉编译 (aarch64-rockchip-linux-gnu-g++)
#   2. scp 传到板子
#   3. 在板子上运行 (NV12 → H.264)
#   4. 拉回 H.264 文件用于验证
# =============================================
set -e

PROJECT="encoder"
BOARD_IP="192.168.0.200"
BOARD_USER="root"
BOARD_DIR="/root/projects/ipc_camera"
CROSS=aarch64-rockchip-linux-gnu

# ---- SDK 路径 (VM 上) -------------------------------------------------
SDK_ROOT="$HOME/RK3568/SDK/linux/rk3568_linux_sdk"
SYSROOT="$SDK_ROOT/buildroot/output/rockchip_rk3568/host/aarch64-buildroot-linux-gnu/sysroot"

echo "=== Phase 2: MPP H.264 硬编码 ==="

# ---- 0. 确认板子在线上 -------------------------------------------------
echo "📡 [0/5] 检查板子连接..."
if ! ssh -o ConnectTimeout=5 ${BOARD_USER}@${BOARD_IP} "echo 板子在线" 2>/dev/null; then
    echo "❌ 无法连接到板子 ${BOARD_IP}"
    exit 1
fi

# 确认 /dev/mpp_service 可用
ssh ${BOARD_USER}@${BOARD_IP} "ls /dev/mpp_service > /dev/null 2>&1" || {
    echo "❌ 板子上 /dev/mpp_service 不存在, MPP 硬件编码不可用"
    exit 1
}
echo "✅ 板子在线, /dev/mpp_service 就绪"

# ---- 0b. 确认 NV12 测试帧存在 ------------------------------------------
echo "🎬 [0b/5] 确认 NV12 测试帧..."
HAS_FRAME=$(ssh ${BOARD_USER}@${BOARD_IP} \
    "test -f /root/capture.yuv && wc -c < /root/capture.yuv" 2>/dev/null || echo "0")

if [ "$HAS_FRAME" = "0" ] || [ -z "$HAS_FRAME" ]; then
    echo "⚠️  板子上没有 /root/capture.yuv, 请先运行 Phase 1 采集一帧"
    echo "   cd ../phase1_v4l2_capture && ./deploy.sh"
    echo "   或者: 手动放置一个 640x480 NV12 文件到板子 /root/capture.yuv"
    exit 1
fi
echo "✅ NV12 测试帧: ${HAS_FRAME} 字节"

# ---- 1. 交叉编译 -------------------------------------------------------
echo ""
echo "🔨 [1/5] 交叉编译..."

# 验证编译器
${CROSS}-g++ --version > /dev/null 2>&1 || {
    echo "❌ 编译器 ${CROSS}-g++ 不可用, 请先 source ~/.bashrc"
    exit 1
}

# 验证 sysroot
if [ ! -d "$SYSROOT" ]; then
    echo "❌ sysroot 不存在: $SYSROOT"
    exit 1
fi

echo "  sysroot: $SYSROOT"

${CROSS}-g++ -std=c++17 -O2 \
    --sysroot="${SYSROOT}" \
    -o ${PROJECT} main.cpp \
    -lrockchip_mpp

echo "✅ 编译完成"
file ${PROJECT}

# ---- 2. 传到板子 -------------------------------------------------------
echo ""
echo "📤 [2/5] 传到板子..."
ssh ${BOARD_USER}@${BOARD_IP} "mkdir -p ${BOARD_DIR}"
scp ${PROJECT} ${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/
echo "✅ 传输完成"

# ---- 3. 运行编码器 -----------------------------------------------------
echo ""
echo "🚀 [3/5] 运行 MPP 编码器 (NV12 → H.264)..."
ssh -t ${BOARD_USER}@${BOARD_IP} \
    "cd ${BOARD_DIR} && chmod +x ./${PROJECT} && ./${PROJECT} /root/capture.yuv ${BOARD_DIR}/output.h264 640 480"

# ---- 4. 验证输出 -------------------------------------------------------
echo ""
echo "🔍 [4/5] 验证输出..."

# 检查输出文件大小
OUT_SIZE=$(ssh ${BOARD_USER}@${BOARD_IP} \
    "wc -c < ${BOARD_DIR}/output.h264" 2>/dev/null || echo "0")

if [ "$OUT_SIZE" = "0" ] || [ -z "$OUT_SIZE" ]; then
    echo "❌ 输出文件为空或不存在"
    exit 1
fi
echo "  板子上 output.h264: ${OUT_SIZE} 字节"

# ---- 5. 拉回 PC 验证 ---------------------------------------------------
echo ""
echo "📥 [5/5] 拉回 H.264 文件..."
scp ${BOARD_USER}@${BOARD_IP}:${BOARD_DIR}/output.h264 ./output.h264
echo "✅ H.264 文件已拉回: $(pwd)/output.h264 ($(stat --printf=%s output.h264 2>/dev/null || echo ${OUT_SIZE}) 字节)"

echo ""
echo "========== Phase 2 完成 =========="
echo ""
echo "📺 播放验证:"
echo "   ffplay output.h264"
echo ""
echo "📊 分析工具:"
echo "   ffprobe -v quiet -show_streams output.h264"
echo "   ffprobe -v quiet -show_frames output.h264 | grep -E 'pict_type|pkt_size'"
echo ""
echo "🔍 十六进制查看 (前128字节):"
echo "   xxd output.h264 | head -20"
echo ""
echo "💡 下一个 Phase: RTSP 推流 (Phase 3)"
