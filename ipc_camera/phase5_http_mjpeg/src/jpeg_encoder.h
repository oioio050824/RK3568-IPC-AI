#pragma once
/**
 * jpeg_encoder.h — JPEG 编码模块
 *
 * 从 NV12 slot 读取帧, 转换为 RGB 后压缩为 JPEG, 写入 JPEG slot。
 *
 * 编码后端 (编译期选择):
 *   - 优先 libjpeg / libjpeg-turbo (HAS_LIBJPEG 宏)
 *   - 降级为内置 tiny JPEG 编码器
 *
 * 线程模型: 独立线程, 只在 pthread_cond_wait 阻塞 (零 sleep 轮询)
 */

#include "frame_types.h"

/**
 * 编码线程入口。
 * @param state  共享状态指针
 * @return       nullptr
 */
void* encoder_thread(void* state);
