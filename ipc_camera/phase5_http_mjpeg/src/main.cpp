/**
 * main.cpp — Phase 5: HTTP MJPEG 实时视频流
 *
 * 三线程架构:
 *   采集线程 (V4L2) → NV12 slot → 编码线程 (NV12→RGB→JPEG) → JPEG slot → HTTP 线程
 *
 * 编译 (CMake):
 *   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain.cmake -DCMAKE_BUILD_TYPE=Release
 *   cmake --build build
 *
 * 编译 (直接 g++, 备选):
 *   aarch64-rockchip-linux-gnu-g++ -std=c++17 -O2 \
 *       --sysroot=<buildroot_sysroot> -Isrc \
 *       -o ipc_mjpeg src/main.cpp src/v4l2_capture.cpp \
 *       src/jpeg_encoder.cpp src/http_server.cpp -lpthread [-ljpeg]
 *
 * 用法:
 *   ./ipc_mjpeg [width] [height] [port]
 *   浏览器打开: http://<board_ip>:8080/
 */

#include "frame_types.h"
#include "v4l2_capture.h"
#include "jpeg_encoder.h"
#include "http_server.h"

#include <iostream>
#include <csignal>
#include <pthread.h>

// ---- 全局指针 (信号处理器访问) ----
static SharedState* g_state = nullptr;

// ---- 信号处理器 ----
static void sig_handler(int) {
    std::cout << "\n[信号] 收到退出信号, 停止中..." << std::endl;
    if (g_state) {
        g_state->running = false;
        // 唤醒可能阻塞的编码线程
        pthread_cond_broadcast(&g_state->nv12_cond);
    }
}

// ═══════════════════════════════════════════════════════════════════════
int main(int argc, char* argv[]) {
    int width  = (argc > 1) ? std::stoi(argv[1]) : 640;
    int height = (argc > 2) ? std::stoi(argv[2]) : 480;
    int port   = (argc > 3) ? std::stoi(argv[3]) : 8080;

    std::cout << "╔══════════════════════════════════════════════╗" << std::endl;
    std::cout << "║  RK3568 HTTP MJPEG 实时视频流 (Phase 5)     ║" << std::endl;
    std::cout << "╚══════════════════════════════════════════════╝" << std::endl;
    std::cout << "分辨率: " << width << "x" << height << std::endl;
    std::cout << "端口:   " << port << std::endl;
    std::cout << std::endl;

    // ---- 1. 信号处理 ----
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    // ---- 2. 创建共享状态 ----
    SharedState state;
    state.width  = width;
    state.height = height;
    g_state = &state;

    // ---- 3. 启动线程 ----
    pthread_t ct, et, ht;

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

    std::cout << "启动 HTTP 服务器..." << std::endl;
    if (pthread_create(&ht, nullptr, http_server_thread, &state) != 0) {
        std::cerr << "❌ 无法创建 HTTP 线程" << std::endl;
        state.running = false;
        pthread_cond_broadcast(&state.nv12_cond);
        pthread_join(ct, nullptr);
        pthread_join(et, nullptr);
        return 1;
    }

    // ---- 4. 打印访问信息 ----
    std::cout << std::endl;
    std::cout << "  ╔══════════════════════════════════════════╗" << std::endl;
    std::cout << "  ║  🌐 浏览器打开:                          ║" << std::endl;
    std::cout << "  ║  http://192.168.0.200:" << port << "/              ║" << std::endl;
    std::cout << "  ║                                          ║" << std::endl;
    std::cout << "  ║  📸 单帧测试:                            ║" << std::endl;
    std::cout << "  ║  curl http://192.168.0.200:" << port << "/snapshot  ║" << std::endl;
    std::cout << "  ║     > test.jpg                           ║" << std::endl;
    std::cout << "  ╚══════════════════════════════════════════╝" << std::endl;
    std::cout << std::endl;
    std::cout << "  按 Ctrl+C 停止" << std::endl;
    std::cout << std::endl;

    // ---- 5. 等待线程退出 ----
    pthread_join(ct, nullptr);
    pthread_join(et, nullptr);
    pthread_join(ht, nullptr);

    std::cout << std::endl;
    std::cout << "=== Phase 5 正常退出 ===" << std::endl;
    return 0;
}
