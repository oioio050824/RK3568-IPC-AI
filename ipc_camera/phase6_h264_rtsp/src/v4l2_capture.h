#pragma once
/**
 * v4l2_capture.h — V4L2 摄像头采集模块
 *
 * 基于 Phase 1 已验证代码。特性:
 *   - Multiplanar API (RK3568 ISP 必需)
 *   - NV12 格式 (半平面 YUV 4:2:0)
 *   - MMAP 缓冲区
 *   - 3A 收敛后采集 (自动丢前 9 帧)
 *   - poll() 可中断等待 (解决 Ctrl+C 问题)
 */

#include "frame_types.h"

/**
 * 采集线程入口。
 * @param state  共享状态指针 (读取 width/height/running, 写入 nv12 slot)
 * @return       nullptr
 */
void* capture_thread(void* state);
