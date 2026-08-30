#pragma once
/**
 * text_renderer.h — FreeType 文字渲染 (OSD 叠加)
 *
 * 用 FreeType 把 UTF-8 字符串渲染成灰度位图, 直接写进 NV12 的 Y 平面(亮度),
 * 抗锯齿文字天然平滑。支持中文(板上已有 noto-sans-sc / source-han-sans-cn,
 * 或自带的江城圆体)。
 *
 * 用法 (仅在编码线程内调用, 单线程):
 *   text_renderer_init(font_path, px_height)   // 启动时
 *   draw_text_y(y, W, H, x, ytop, utf8)        // 每帧画标签
 *   text_renderer_deinit()                     // 退出时
 */

#include <cstdint>

// 加载字体并设字号(px)。失败返回 false, 后续 draw_text_y 变空操作。
bool text_renderer_init(const char* font_path, int px_height);
void text_renderer_deinit();

// 一行文本的高度(px), 未就绪返回 0
int  text_line_height();

// 在 (x, ytop) 处画文本(ytop=顶端), 实心黑底+白字(灰度抗锯齿), 返回文本像素宽
int  draw_text_y(uint8_t* y, int W, int H, int x, int ytop, const char* utf8);
