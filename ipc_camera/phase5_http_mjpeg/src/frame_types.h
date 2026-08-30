#pragma once
/**
 * frame_types.h — Phase 5 共享数据结构
 *
 * 设计理念:
 *   单槽位 + 互斥锁 + 条件变量。MJPEG 每帧是独立图像, 积压旧帧无意义,
 *   保存最新一帧即可。采集线程永不阻塞 (lock 内只做 swap), 编码线程
 *   只在条件变量上阻塞 (零 sleep 轮询)。
 */

#include <cstdint>
#include <vector>
#include <atomic>
#include <pthread.h>

// ---- NV12 原始帧 (来自 V4L2 采集) ----
struct Nv12Frame {
    std::vector<uint8_t> data;   // W*H*3/2 字节
    int64_t pts_us = 0;          // 采集时刻 (微秒, 相对启动时间)

    void copy_from(const uint8_t* src, size_t len, int64_t pts) {
        data.assign(src, src + len);
        pts_us = pts;
    }
};

// ---- JPEG 编码帧 (产出给 HTTP 服务器) ----
struct JpegFrame {
    std::vector<uint8_t> data;   // 完整 JPEG 文件
    int64_t pts_us = 0;          // 来自源 Nv12Frame
    uint64_t seq    = 0;         // 单调递增序号 (用于检测新帧)

    void set_data(const uint8_t* src, size_t len, int64_t pts, uint64_t s) {
        data.assign(src, src + len);
        pts_us = pts;
        seq    = s;
    }
};

// ---- 全局共享状态 (所有线程通过指针访问) ----
struct SharedState {
    // --- NV12 槽位 (采集 → 编码) ---
    Nv12Frame       nv12;
    uint64_t        nv12_seq = 0;          // 每帧递增
    pthread_mutex_t nv12_mutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t  nv12_cond  = PTHREAD_COND_INITIALIZER;

    // --- JPEG 槽位 (编码 → HTTP) ---
    JpegFrame       jpeg;
    pthread_mutex_t jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;

    // --- 生命周期 ---
    std::atomic<bool> running{true};

    // --- 配置 ---
    int width  = 640;
    int height = 480;
};
