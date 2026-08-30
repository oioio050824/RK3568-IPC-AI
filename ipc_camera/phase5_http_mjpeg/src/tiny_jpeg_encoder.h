#pragma once
/**
 * tiny_jpeg_encoder.h — 最小 JPEG 基线编码器
 *
 * 仅编码 Y (灰度) 分量。这是有意的简化:
 *   - 浏览器完全支持灰度 JPEG
 *   - 编码逻辑减半 (无 Cb/Cr, 无 4:2:0 MCU 交织)
 *   - RGB 输入仅用于提取亮度
 *
 * 如果板子上有 libjpeg, jpeg_encoder.cpp 会自动优先使用它
 * (完整的 YCbCr 彩色 JPEG)。
 *
 * 公共领域。基于 JPEG 基线标准 (ISO/IEC 10918-1)。
 */

#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <math.h>

namespace tiny_jpeg {

// ═══════════════════════════════════════════════════════════════
// 标准亮度量化表 (quality 50)
// ═══════════════════════════════════════════════════════════════
static const uint8_t STD_LUMA_QT[64] = {
    16,11,10,16, 24, 40, 51, 61,
    12,12,14,19, 26, 58, 60, 55,
    14,13,16,24, 40, 57, 69, 56,
    14,17,22,29, 51, 87, 80, 62,
    18,22,37,56, 68,109,103, 77,
    24,35,55,64, 81,104,113, 92,
    49,64,78,87,103,121,120,101,
    72,92,95,98,112,100,103, 99
};

// Zigzag 顺序
static const uint8_t ZZ[64] = {
     0, 1, 8,16, 9, 2, 3,10, 17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34, 27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36, 29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46, 53,60,61,54,47,55,62,63
};

// ═══════════════════════════════════════════════════════════════
// 位写入器 (带 0xFF byte stuffing)
// ═══════════════════════════════════════════════════════════════
struct Bits {
    std::vector<uint8_t>& buf;
    uint32_t acc;
    int      n;

    Bits(std::vector<uint8_t>& b) : buf(b), acc(0), n(0) {}

    void emit(uint32_t code, int bits) {
        acc = (acc << bits) | (code & ((1u << bits) - 1));
        n  += bits;
        while (n >= 8) {
            n -= 8;
            uint8_t b = (uint8_t)(acc >> n);
            buf.push_back(b);
            if (b == 0xFF) buf.push_back(0x00);  // byte stuffing
        }
    }

    void flush() {
        if (n > 0) {
            acc <<= (8 - n);
            buf.push_back((uint8_t)acc);
            n = 0;
        }
    }
};

// ═══════════════════════════════════════════════════════════════
// 写入标记 (marker)
// ═══════════════════════════════════════════════════════════════
static void w16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back((uint8_t)(v >> 8));
    b.push_back((uint8_t)(v & 0xFF));
}

static void marker(std::vector<uint8_t>& b, uint8_t m) {
    b.push_back(0xFF);
    b.push_back(m);
}

// ═══════════════════════════════════════════════════════════════
// 标准 Huffman 表 (JPEG 标准 Annex K, 仅亮度)
// ═══════════════════════════════════════════════════════════════

// DC 亮度: 码长排列
static const uint8_t DC_BITS[]  = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const uint8_t DC_VALS[]  = {0,1,2,3,4,5,6,7,8,9,10,11};

// AC 亮度: 码长排列
static const uint8_t AC_BITS[]  = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7D};
static const uint8_t AC_VALS[]  = {
    0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,
    0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xA1,0x08,
    0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,0x24,0x33,0x62,0x72,
    0x82,0x09,0x0A,0x16,0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,
    0x29,0x2A,0x34,0x35,0x36,0x37,0x38,0x39,0x3A,0x43,0x44,0x45,
    0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
    0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,0x74,0x75,
    0x76,0x77,0x78,0x79,0x7A,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
    0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,0xA2,0xA3,
    0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,
    0xB7,0xB8,0xB9,0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,
    0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,0xD9,0xDA,0xE1,0xE2,
    0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF1,0xF2,0xF3,0xF4,
    0xF5,0xF6,0xF7,0xF8,0xF9,0xFA
};

// 从 BITS/VALS 构建规范化码表
struct HuffTable {
    uint16_t code[256];
    uint8_t  size[256];

    void init(const uint8_t* bits, const uint8_t* vals, int nvals) {
        memset(code, 0, sizeof(code));
        memset(size, 0, sizeof(size));
        uint16_t c = 0;
        for (int s = 1; s <= 16; s++) {
            for (int i = 0; i < bits[s-1]; i++) {
                int vi = (int)vals[c + i];  // 注意: vals 中的索引可能不是顺序的
                // 实际需要找到匹配的 val... 简化: 直接映射
            }
            c += bits[s-1];
        }
        // 构建规范化码: 按 BITS 顺序分配递增码
        int ci = 0;
        uint16_t cc = 0;
        for (int s = 1; s <= 16; s++) {
            int n = bits[s-1];
            for (int j = 0; j < n; j++) {
                if (ci < nvals) {
                    uint8_t sym = vals[ci];
                    code[sym] = cc;
                    size[sym] = (uint8_t)s;
                    ci++;
                    cc++;
                }
            }
            cc <<= 1;
        }
    }
};

static HuffTable DC_HUFF, AC_HUFF;
static bool      tables_ready = false;

static void ensure_tables() {
    if (tables_ready) return;
    DC_HUFF.init(DC_BITS, DC_VALS, 12);
    int ac_count = 0;
    for (int i = 0; i < 16; i++) ac_count += AC_BITS[i];
    AC_HUFF.init(AC_BITS, AC_VALS, ac_count);
    tables_ready = true;
}

// ═══════════════════════════════════════════════════════════════
// 写入 DHT (Huffman Table) 段
// ═══════════════════════════════════════════════════════════════
static void write_dht(std::vector<uint8_t>& buf) {
    // DC 亮度 Huffman 表 (class=0, id=0)
    marker(buf, 0xC4);
    int dc_vals = 0; for (int i = 0; i < 16; i++) dc_vals += DC_BITS[i];
    uint16_t len = (uint16_t)(2 + 1 + 16 + dc_vals);
    w16(buf, len);
    buf.push_back(0x00);  // Tc=0 (DC), Th=0
    for (int i = 0; i < 16; i++) buf.push_back(DC_BITS[i]);
    for (int i = 0; i < dc_vals; i++) buf.push_back(DC_VALS[i]);

    // AC 亮度 Huffman 表 (class=1, id=0)
    marker(buf, 0xC4);
    int ac_vals = 0; for (int i = 0; i < 16; i++) ac_vals += AC_BITS[i];
    len = (uint16_t)(2 + 1 + 16 + ac_vals);
    w16(buf, len);
    buf.push_back(0x10);  // Tc=1 (AC), Th=0
    for (int i = 0; i < 16; i++) buf.push_back(AC_BITS[i]);
    for (int i = 0; i < ac_vals; i++) buf.push_back(AC_VALS[i]);
}

// ═══════════════════════════════════════════════════════════════
// 写入 DQT (Quantization Table) 段
// ═══════════════════════════════════════════════════════════════
static void write_dqt(std::vector<uint8_t>& buf, int quality) {
    marker(buf, 0xDB);
    w16(buf, 67); // 2 + 1 + 64
    buf.push_back(0x00); // Pq=0 (8-bit), Tq=0

    // 根据 quality 缩放量化表
    float scale = (quality < 50) ? (5000.0f / (float)quality)
                 : (200.0f - (float)quality * 2.0f);
    if (scale < 1.0f) scale = 1.0f;

    uint8_t qt[64];
    for (int i = 0; i < 64; i++) {
        int v = (int)((float)STD_LUMA_QT[ZZ[i]] * scale / 100.0f + 0.5f);
        if (v < 1) v = 1;
        if (v > 255) v = 255;
        qt[i] = (uint8_t)v;
    }
    for (int i = 0; i < 64; i++) buf.push_back(qt[i]);
}

// ═══════════════════════════════════════════════════════════════
// 前向 DCT (行列分解, 浮点, 可读性优先)
// ═══════════════════════════════════════════════════════════════

// 预先计算的 8x8 DCT 矩阵 (缩放因子已乘入)
static float dct_coeff(int k, int n) {
    // coeff(k,n) = cos((2n+1)*k*pi/16)
    static const float PI = 3.141592653589793f;
    float v = (float)k * (float)(2 * n + 1) * PI / 16.0f;
    return cos(v);
}

static void fdct_8x8(int16_t block[64]) {
    float tmp[64];
    float scale_row0 = 1.0f / sqrt(2.0f);  // 1/sqrt(2) for k=0

    // 1D DCT 在行上
    for (int y = 0; y < 8; y++) {
        for (int k = 0; k < 8; k++) {
            float sum = 0.0f;
            for (int x = 0; x < 8; x++)
                sum += (float)block[y * 8 + x] * dct_coeff(k, x);
            tmp[y * 8 + k] = sum * (k == 0 ? scale_row0 : 1.0f) * 0.5f;
        }
    }

    // 1D DCT 在列上 (转置后)
    float s0 = 1.0f / sqrt(2.0f);
    for (int x = 0; x < 8; x++) {
        for (int k = 0; k < 8; k++) {
            float sum = 0.0f;
            for (int y = 0; y < 8; y++)
                sum += tmp[y * 8 + x] * dct_coeff(k, y);
            block[k * 8 + x] = (int16_t)(sum * (k == 0 ? s0 : 1.0f) * 0.5f);
        }
    }
}

// ═══════════════════════════════════════════════════════════════
// Huffman 编码辅助
// ═══════════════════════════════════════════════════════════════

// 获取值的 magnitude category
static int category(int val) {
    int a = val < 0 ? -val : val;
    int c = 0;
    while (a) { a >>= 1; c++; }
    return c;
}

static void encode_dc(Bits& w, int diff) {
    int cat = category(diff);
    // Huffman 编码 category
    if (cat < (int)sizeof(DC_HUFF.size)) {
        w.emit(DC_HUFF.code[cat], DC_HUFF.size[cat]);
    }
    // 附加位: diff 的值 (two's complement offset)
    if (cat > 0) {
        if (diff < 0) diff--;
        w.emit((uint32_t)diff, cat);
    }
}

static void encode_ac(Bits& w, int16_t* block) {
    int run = 0;
    for (int i = 1; i < 64; i++) {
        int val = block[ZZ[i]];
        if (val == 0) {
            run++;
            if (i == 63) {
                // EOB
                w.emit(AC_HUFF.code[0x00], AC_HUFF.size[0x00]);
            }
            continue;
        }
        // 输出前面的零
        while (run >= 16) {
            // ZRL (run=15, size=0)
            int sym = (15 << 4) | 0;  // 0xF0
            w.emit(AC_HUFF.code[sym], AC_HUFF.size[sym]);
            run -= 16;
        }
        int cat = category(val);
        int sym = (run << 4) | cat;
        w.emit(AC_HUFF.code[sym], AC_HUFF.size[sym]);
        if (cat > 0) {
            if (val < 0) val--;
            w.emit((uint32_t)val, cat);
        }
        run = 0;
    }
}

// ═══════════════════════════════════════════════════════════════
// 编码一个 8x8 块
// ═══════════════════════════════════════════════════════════════
static void encode_block(Bits& w, int16_t block[64],
                         int& prev_dc, const uint8_t* qt) {
    // DCT
    fdct_8x8(block);

    // 量化 (zigzag 序内做)
    int16_t q[64];
    for (int i = 0; i < 64; i++)
        q[i] = block[ZZ[i]] / (int)qt[i];

    // DC 差值编码
    int diff = q[0] - prev_dc;
    prev_dc = q[0];
    encode_dc(w, diff);

    // AC 编码
    encode_ac(w, q);
}

} // namespace tiny_jpeg

// ═══════════════════════════════════════════════════════════════
// 公开接口
// ═══════════════════════════════════════════════════════════════

/**
 * 编码 RGB 图像为 JPEG (灰度)。
 *
 * @param rgb     RGB 交错像素 (R,G,B,R,G,B,...), W*H*3 字节
 * @param w       宽度
 * @param h       高度
 * @param quality 1-100 (默认75)
 * @return        完整 JFIF JPEG 文件
 */
inline std::vector<uint8_t> encode_jpeg(const uint8_t* rgb,
                                         int w, int h, int quality) {
    using namespace tiny_jpeg;
    ensure_tables();

    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;

    std::vector<uint8_t> buf;
    buf.reserve((size_t)w * h / 2);  // 预分配 ~0.5 bpp

    // ---- SOI ----
    marker(buf, 0xD8);

    // ---- APP0 (JFIF) ----
    marker(buf, 0xE0);
    w16(buf, 16);
    buf.push_back('J'); buf.push_back('F'); buf.push_back('I'); buf.push_back('F');
    buf.push_back(0x00);
    buf.push_back(1); buf.push_back(1);  // version 1.1
    buf.push_back(0x00);                 // density unit: no aspect ratio
    w16(buf, 1); w16(buf, 1);           // 1x1
    buf.push_back(0x00); buf.push_back(0x00); // no thumbnail

    // ---- DQT ----
    write_dqt(buf, quality);

    // ---- SOF0 (灰度, 1 分量) ----
    marker(buf, 0xC0);
    w16(buf, 11);    // Lf = 8 + 3*1
    buf.push_back(8); // P=8 bit
    w16(buf, (uint16_t)h);
    w16(buf, (uint16_t)w);
    buf.push_back(1); // Nf=1 (灰度)
    buf.push_back(1); // component 1
    buf.push_back(0x11); // H=1, V=1, 无子采样
    buf.push_back(0x00); // Tq=0

    // ---- DHT ----
    write_dht(buf);

    // ---- SOS ----
    marker(buf, 0xDA);
    w16(buf, 8);     // Ls = 6 + 2*1
    buf.push_back(1); // Ns=1
    buf.push_back(1); // Cs=1
    buf.push_back(0x00); // Td=0, Ta=0
    buf.push_back(0x00); // Ss=0
    buf.push_back(0x3F); // Se=63
    buf.push_back(0x00); // Ah=0, Al=0

    // ---- 编码图像数据 ----
    Bits bits(buf);

    int mcu_w = (w + 7) / 8;
    int mcu_h = (h + 7) / 8;
    int prev_dc = 0;

    // 量化表 (zigzag 序)
    float scale = (quality < 50) ? (5000.0f / (float)quality)
                 : (200.0f - (float)quality * 2.0f);
    if (scale < 1.0f) scale = 1.0f;
    uint8_t qt[64];
    for (int i = 0; i < 64; i++) {
        int v = (int)((float)STD_LUMA_QT[i] * scale / 100.0f + 0.5f);
        if (v < 1) v = 1; if (v > 255) v = 255;
        qt[i] = (uint8_t)v;
    }

    for (int my = 0; my < mcu_h; my++) {
        for (int mx = 0; mx < mcu_w; mx++) {
            // 提取 8x8 亮度块
            int16_t block[64] = {0};
            for (int by = 0; by < 8; by++) {
                int py = my * 8 + by;
                if (py >= h) break;
                for (int bx = 0; bx < 8; bx++) {
                    int px = mx * 8 + bx;
                    if (px >= w) break;
                    const uint8_t* p = rgb + ((size_t)py * w + px) * 3;
                    // RGB → Y (BT.601)
                    block[by * 8 + bx] = (int16_t)(
                        (66 * (int)p[0] + 129 * (int)p[1] + 25 * (int)p[2] + 128) >> 8
                    );
                }
            }
            // Level shift
            for (int i = 0; i < 64; i++) block[i] -= 128;

            encode_block(bits, block, prev_dc, qt);
        }
    }

    bits.flush();

    // ---- EOI ----
    marker(buf, 0xD9);

    return buf;
}
