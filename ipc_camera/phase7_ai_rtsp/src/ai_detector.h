#pragma once
/**
 * ai_detector.h — RKNN 目标检测线程 (Phase 7.3)
 *
 * 旁路检测, 不阻塞主码流:
 *   采集 → ai_nv12 单槽 → 本线程: NV12→RGB → letterbox(640x640) → RKNN
 *        → YOLOv5 解码 + NMS → 坐标映射(摄像头 640x480) → 发布 AiResult
 *
 * 设计要点:
 *   - 最新帧覆盖 (AI 慢时自动丢帧, 不积压不反压)
 *   - RGA 硬件转换 (librga 2.x handle API), CPU 兜底 (RGA 失败自动回退)
 *   - 检测框坐标从模型空间映射回摄像头空间 (y - pad_y)
 */

#include "frame_types.h"

/**
 * AI 检测线程入口。
 * @param state  共享状态指针 (读取 ai_nv12/配置, 写入 ai_result)
 * @return       nullptr
 */
void* ai_thread(void* state);
