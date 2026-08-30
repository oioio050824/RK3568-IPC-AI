#pragma once
/**
 * h264_encoder.h — MPP H.264 硬件编码模块
 *
 * 从 NV12 单槽读取帧, MPP 硬件编码为 H.264, 推入环形缓冲。
 * 同时提取 SPS/PPS 写入 SharedState 供 RTSP 使用。
 *
 * 基于 Phase 2 已验证的 MPP 配置 + Phase 5 的条件变量等待模式。
 */

#include "frame_types.h"

void* encoder_thread(void* state);
