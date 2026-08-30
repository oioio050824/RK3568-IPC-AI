#pragma once
/**
 * frame_types.h — Phase 6 共享数据结构
 *
 * 三线程共享:
 *   采集 ──NV12单槽──▶ 编码 ──H264环──▶ RTSP
 *              ↑ condvar        ↑ RingBuffer(64)
 *
 * 比 Phase 5 多了:
 *   - RingBuffer (H.264 GOP 结构需要多帧缓冲)
 *   - SPS/PPS 缓存 (供 RTSP 的 SDP 和新客户端注入)
 */

#include <vector>
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

    // --- 生命周期 ---
    std::atomic<bool> running{true};

    // --- 配置 ---
    int width  = 640;
    int height = 480;
};
