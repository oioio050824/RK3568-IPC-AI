/**
 * text_renderer.cpp — FreeType 文字渲染实现
 *
 * 关键点:
 *   - 字形缓存(std::map<码点, 灰度位图>): 每个字符只栅格化一次, 后续直接查表。
 *     检测标签每帧置信度都变, 按「字符」缓存才有效(按整串缓存会全 miss)。
 *   - 灰度直接写 Y 平面: FreeType 输出 8bit 灰度, 亮度平面上灰=亮度,
 *     抗锯齿文字不用二值化, 观感平滑。
 *   - UTF-8 解码: 中文是多字节, 解码成码点再 FT_Load_Char。
 */

#include "text_renderer.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <vector>
#include <map>
#include <cstring>
#include <cstdio>

static FT_Library g_ft = nullptr;
static FT_Face    g_face = nullptr;
static bool       g_ready = false;
static int        g_ascent = 0, g_descent = 0, g_line_h = 0;

bool text_renderer_init(const char* path, int px) {
    if (FT_Init_FreeType(&g_ft)) {
        fprintf(stderr, "[OSD] ❌ FT_Init_FreeType 失败\n");
        return false;
    }
    if (FT_New_Face(g_ft, path, 0, &g_face)) {
        fprintf(stderr, "[OSD] ❌ 打不开字体 %s (标签不显示)\n", path);
        FT_Done_FreeType(g_ft);
        g_ft = nullptr;
        return false;
    }
    if (FT_Set_Pixel_Sizes(g_face, 0, px)) {
        fprintf(stderr, "[OSD] ❌ FT_Set_Pixel_Sizes 失败\n");
        FT_Done_Face(g_face); FT_Done_FreeType(g_ft);
        g_face = nullptr; g_ft = nullptr;
        return false;
    }
    g_ascent  = (int)(g_face->size->metrics.ascender >> 6);
    g_descent = (int)(-(g_face->size->metrics.descender) >> 6);
    g_line_h  = g_ascent + g_descent;
    g_ready   = true;
    printf("[OSD] 字体加载: %s (%dpx, 行高 %d)\n", path, px, g_line_h);
    return true;
}

void text_renderer_deinit() {
    if (g_face) { FT_Done_Face(g_face); g_face = nullptr; }
    if (g_ft)   { FT_Done_FreeType(g_ft); g_ft = nullptr; }
    g_ready = false;
}

int text_line_height() { return g_ready ? g_line_h : 0; }

// ---- 字形缓存 ----
struct Glyph {
    int w = 0, h = 0, left = 0, top = 0, adv = 0;
    std::vector<uint8_t> gray;   // 8bit 灰度, 行主序
};
static std::map<uint32_t, Glyph> g_cache;

static const Glyph* get_glyph(uint32_t cp) {
    auto it = g_cache.find(cp);
    if (it != g_cache.end()) return &it->second;

    if (FT_Load_Char(g_face, cp, FT_LOAD_RENDER))
        return nullptr;   // 缺字形, 跳过

    FT_GlyphSlot slot = g_face->glyph;
    Glyph g;
    g.w    = (int)slot->bitmap.width;
    g.h    = (int)slot->bitmap.rows;
    g.left = slot->bitmap_left;
    g.top  = slot->bitmap_top;
    g.adv  = (int)(slot->advance.x >> 6);
    g.gray.resize((size_t)g.w * g.h);
    for (int r = 0; r < g.h; r++)
        memcpy(&g.gray[(size_t)r * g.w],
               slot->bitmap.buffer + (size_t)r * slot->bitmap.pitch, g.w);

    auto res = g_cache.emplace(cp, std::move(g));
    return &res.first->second;
}

// ---- UTF-8 解码: 返回码点并推进指针, 非法返回 0 ----
static uint32_t utf8_next(const char*& p) {
    const unsigned char* s = (const unsigned char*)p;
    if (s[0] < 0x80) { p++; return s[0]; }
    int n; uint32_t cp;
    if      ((s[0] & 0xE0) == 0xC0) { n = 1; cp = s[0] & 0x1F; }
    else if ((s[0] & 0xF0) == 0xE0) { n = 2; cp = s[0] & 0x0F; }
    else if ((s[0] & 0xF8) == 0xF0) { n = 3; cp = s[0] & 0x07; }
    else { p++; return 0; }
    for (int i = 1; i <= n; i++) {
        if ((s[i] & 0xC0) != 0x80) { p += i; return 0; }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    p += n + 1;
    return cp;
}

int draw_text_y(uint8_t* y, int W, int H, int x, int ytop, const char* utf8) {
    if (!g_ready || !utf8) return 0;

    // 1) 解码 + 度量总宽 (同时预热字形缓存)
    std::vector<uint32_t> cps;
    std::vector<const Glyph*> gs;
    int total_w = 0;
    for (const char* p = utf8; *p; ) {
        uint32_t cp = utf8_next(p);
        if (!cp) continue;
        const Glyph* g = get_glyph(cp);
        cps.push_back(cp);
        gs.push_back(g);
        total_w += g ? g->adv : 0;
    }
    if (cps.empty()) return 0;

    // 2) 实心黑底
    for (int py = ytop; py < ytop + g_line_h; py++) {
        if (py < 0 || py >= H) continue;
        for (int px = x; px < x + total_w; px++) {
            if (px < 0 || px >= W) continue;
            y[py * W + px] = 0;
        }
    }

    // 3) 画字: 灰度 → Y (亮度), 抗锯齿天然
    int pen_x = x;
    for (size_t i = 0; i < gs.size(); i++) {
        const Glyph* g = gs[i];
        if (g) {
            int ox = pen_x + g->left;
            int oy = ytop + g_ascent - g->top;   // 基线 = ytop + ascent
            for (int r = 0; r < g->h; r++) {
                int py = oy + r;
                if (py < 0 || py >= H) continue;
                for (int c = 0; c < g->w; c++) {
                    int px = ox + c;
                    if (px < 0 || px >= W) continue;
                    uint8_t a = g->gray[(size_t)r * g->w + c];
                    if (a) y[py * W + px] = a;
                }
            }
        }
        pen_x += g ? g->adv : 0;
    }
    return total_w;
}
