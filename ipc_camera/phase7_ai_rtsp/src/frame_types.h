#pragma once
/**
 * frame_types.h — Phase 7.3 共享数据结构
 *
 * 四线程共享:
 *   采集 ──NV12单槽──▶ 编码 ──H264环──▶ RTSP        (主码流, 不变)
 *         └─NV12单槽──▶ AI检测 ──框──▶ 编码(画框)     (旁路, 不阻塞)
 *                        ↑ condvar          ↑ AiResult
 *
 * 在 Phase 6 基础上新增:
 *   - AI 单槽 (采集 → AI 检测线程, 最新帧覆盖, AI 慢时自动丢帧)
 *   - AiBox / AiResult (AI 检测结果 → 编码线程画框)
 */

#include <vector>
#include <string>
#include <cstdint>
#include <atomic>
#include <pthread.h>
#include "ring_buffer.h"

// ---- NV12 原始帧 (采集产出) ----
struct Nv12Frame {
    std::vector<uint8_t> data;
    int64_t pts_us = 0;

    void copy_from(const uint8_t* src, size_t len, int64_t pts) {
        data.assign(src, src + len);
        pts_us = pts;
    }
};

// ---- H.264 编码包 (编码产出, 一个包可能包含多个 NAL) ----
struct H264Packet {
    std::vector<uint8_t> data;
    int64_t pts_us = 0;
    bool    is_idr  = false;
};

// ---- AI 检测框 (已映射回摄像头坐标系 640x480) ----
struct AiBox {
    float x1, y1, x2, y2;
    float conf;
    int   cls;
    char  label[48]{};   // 预格式化 "类别 置信度", 编码线程直接画
};

// ---- AI 检测结果 (AI 线程产出 → 编码线程画框) ----
struct AiResult {
    std::vector<AiBox> boxes;
    int64_t            seq = 0;   // 每次检测自增
    pthread_mutex_t    mutex = PTHREAD_MUTEX_INITIALIZER;
};

// ---- 全局共享状态 ----
struct SharedState {
    // --- NV12 单槽 (采集 → 编码) ---
    Nv12Frame       nv12;
    uint64_t        nv12_seq = 0;
    pthread_mutex_t nv12_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  nv12_cond  = PTHREAD_COND_INITIALIZER;

    // --- H.264 环形缓冲 (编码 → RTSP) ---
    // 在 main 中用 new 分配, 因为容量需要在运行时确定
    RingBuffer<H264Packet>* h264_ring = nullptr;
    static const size_t     RING_SIZE = 64;

    // --- SPS/PPS 缓存 (编码提取, RTSP 读取) ---
    std::vector<uint8_t> sps;
    std::vector<uint8_t> pps;
    bool                 sps_pps_ready = false;
    pthread_mutex_t      sps_mutex = PTHREAD_MUTEX_INITIALIZER;

    // --- AI 检测单槽 (采集 → AI 线程, 最新帧覆盖) ---
    Nv12Frame       ai_nv12;
    uint64_t        ai_nv12_seq = 0;
    pthread_mutex_t ai_nv12_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  ai_nv12_cond  = PTHREAD_COND_INITIALIZER;

    // --- AI 检测结果 (AI 线程 → 编码线程) ---
    AiResult ai_result;

    // --- AI 配置 ---
    std::string ai_model_path   = "/yolov5s.rknn";
    float       ai_conf_thresh  = 0.25f;  // obj/class 阈值 (误检多可调 0.4~0.5)
    bool        ai_draw_boxes   = true;   // 检测框叠加到 H.264 流
    std::string ai_font_path    = "/root/projects/ipc_camera/jc_round_600.ttf";  // 标签字体
    int         ai_font_height  = 18;     // 标签字号(px)

    // --- 生命周期 ---
    std::atomic<bool> running{true};

    // --- 配置 ---
    int width  = 640;
    int height = 480;
};
