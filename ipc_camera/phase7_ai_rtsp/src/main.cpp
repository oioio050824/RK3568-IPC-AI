/**
 * main.cpp — Phase 7.3: H.264 + RTSP + RKNN 实时目标检测
 *
 * 四线程架构:
 *   采集(V4L2) ──NV12──▶ 编码(MPP H.264) ──H264环──▶ RTSP    (主码流)
 *         └─NV12──▶ AI(RKNN 检测) ──框──▶ 编码(画框)          (旁路)
 *
 * AI 是旁路: 慢时自动丢帧, 永不阻塞采集/编码/推流。检测框叠加到 H.264 流。
 *
 * 复用:
 *   - v4l2_capture / h264_encoder / rtsp_server  — Phase 6 (含画框改动)
 *   - ai_detector                                — Phase 7.2 后处理 + RGA
 *
 * 用法: ./ipc_ai [width] [height] [port] [conf阈值] [模型路径]
 * 播放: ffplay -rtsp_transport tcp rtsp://192.168.0.200:8554/stream
 */

#include "frame_types.h"
#include "v4l2_capture.h"
#include "h264_encoder.h"
#include "rtsp_server.h"
#include "ai_detector.h"

#include <iostream>
#include <csignal>
#include <pthread.h>

static SharedState* g_state = nullptr;

static void sig_handler(int) {
    std::cout << "\n[信号] 收到退出信号, 停止中..." << std::endl;
    if (g_state) {
        g_state->running = false;
        pthread_cond_broadcast(&g_state->nv12_cond);
        pthread_cond_broadcast(&g_state->ai_nv12_cond);
        if (g_state->h264_ring)
            g_state->h264_ring->wake_all();
    }
}

// ═══════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    int width  = (argc > 1) ? std::stoi(argv[1]) : 640;
    int height = (argc > 2) ? std::stoi(argv[2]) : 480;
    int port   = (argc > 3) ? std::stoi(argv[3]) : 8554;

    std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  RK3568 H.264 RTSP + RKNN 目标检测 (7.3)  ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
    std::cout << "分辨率: " << width << "x" << height << std::endl;
    std::cout << "编码:   MPP H.264 CBR 2Mbps GOP=30" << std::endl;
    std::cout << "协议:   RTSP/RTP over TCP" << std::endl;
    std::cout << std::endl;

    // ---- 1. 信号处理 ----
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);  // 客户端断开时 writev 返回错误, 不杀进程

    // ---- 2. 创建共享状态 ----
    SharedState state;
    state.width  = width;
    state.height = height;

    // 可选参数: 置信度阈值 / 模型路径 / 字体路径
    if (argc > 4) state.ai_conf_thresh = std::stof(argv[4]);
    if (argc > 5) state.ai_model_path   = argv[5];
    if (argc > 6) state.ai_font_path    = argv[6];
    std::cout << "AI 检测: 模型=" << state.ai_model_path
              << " 阈值=" << state.ai_conf_thresh
              << " 画框=" << (state.ai_draw_boxes ? "开" : "关") << std::endl;
    std::cout << "标签字体: " << state.ai_font_path
              << " (" << state.ai_font_height << "px)" << std::endl;
    std::cout << std::endl;

    RingBuffer<H264Packet> h264_ring(SharedState::RING_SIZE);
    state.h264_ring = &h264_ring;

    g_state = &state;

    // ---- 3. 启动线程 ----
    pthread_t ct, et, rt, ait;
    bool ai_started = false;

    std::cout << "启动采集线程..." << std::endl;
    if (pthread_create(&ct, nullptr, capture_thread, &state) != 0) {
        std::cerr << "❌ 无法创建采集线程" << std::endl;
        return 1;
    }

    std::cout << "启动编码线程..." << std::endl;
    if (pthread_create(&et, nullptr, encoder_thread, &state) != 0) {
        std::cerr << "❌ 无法创建编码线程" << std::endl;
        state.running = false;
        pthread_cond_broadcast(&state.nv12_cond);
        pthread_join(ct, nullptr);
        return 1;
    }

    std::cout << "启动 RTSP 服务器..." << std::endl;
    if (pthread_create(&rt, nullptr, rtsp_thread, &state) != 0) {
        std::cerr << "❌ 无法创建 RTSP 线程" << std::endl;
        state.running = false;
        pthread_cond_broadcast(&state.nv12_cond);
        pthread_cond_broadcast(&state.ai_nv12_cond);
        h264_ring.wake_all();
        pthread_join(ct, nullptr);
        pthread_join(et, nullptr);
        return 1;
    }

    std::cout << "启动 AI 检测线程..." << std::endl;
    if (pthread_create(&ait, nullptr, ai_thread, &state) != 0) {
        std::cerr << "⚠️ 无法创建 AI 线程, 继续无 AI 运行" << std::endl;
    } else {
        ai_started = true;
    }

    // ---- 4. 打印访问信息 ----
    std::cout << std::endl;
    std::cout << "  ╔══════════════════════════════════════════╗" << std::endl;
    std::cout << "  ║  📺 RTSP 实时视频流(带 AI 检测框):       ║" << std::endl;
    std::cout << "  ║  rtsp://192.168.0.200:" << port << "/stream         ║" << std::endl;
    std::cout << "  ║                                          ║" << std::endl;
    std::cout << "  ║  🎬 播放命令:                            ║" << std::endl;
    std::cout << "  ║  ffplay -rtsp_transport tcp \\          ║"  << std::endl;
    std::cout << "  ║    rtsp://192.168.0.200:" << port << "/stream     ║" << std::endl;
    std::cout << "  ║  vlc rtsp://192.168.0.200:" << port << "/stream  ║" << std::endl;
    std::cout << "  ╚══════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    std::cout << "  按 Ctrl+C 停止" << std::endl;
    std::cout << std::endl;

    // ---- 5. 等待线程退出 ----
    pthread_join(ct, nullptr);
    pthread_join(et, nullptr);
    pthread_join(rt, nullptr);
    if (ai_started) pthread_join(ait, nullptr);

    std::cout << std::endl;
    std::cout << "=== Phase 7.3 正常退出 ===" << std::endl;
    return 0;
}
