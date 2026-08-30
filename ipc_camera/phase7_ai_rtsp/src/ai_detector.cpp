/**
 * ai_detector.cpp — RKNN 实时目标检测实现 (Phase 7.3)
 *
 * 从 Phase 7.2 (单帧 demo) 提取后处理, 封装为旁路线程:
 *   等待 ai_nv12 单槽新帧
 *   → NV12→RGB(640x480)      [RGA 硬件优先, CPU 兜底]
 *   → letterbox(640x640)     [CPU memcpy, 灰 114, 上下各 pad 80]
 *   → rknn_inputs_set → rknn_run → rknn_outputs_get(want_float=1)
 *   → YOLOv5 3 头解码 + NMS
 *   → 坐标映射 (模型 y - 80 → 摄像头 y) + 裁剪
 *   → 发布 AiResult (swap 到共享区)
 *
 * 关键踩坑 (见 Phase7_待解决问题.md):
 *   - 本 SDK librga 1.10 的 im2d 没有 importbuffer/handle 那套 API, 也没旧版 c_RkRgaBlit。
 *     NV12→RGB 用 wrapbuffer_virtualaddr 包 src/dst + imcvtcolor (两者都是宏, 4 参)。
 *     wrapbuffer_virtualaddr 是宏: (vir_addr, width, height, format[, wstride, hstride])
 *   - imcvtcolor 返回 IM_STATUS_SUCCESS(=1) 才是成功, IM_STATUS_FAILED(=0)
 */

#include "ai_detector.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cerrno>
#include <string>
#include <vector>
#include <algorithm>
#include <math.h>       // expf (踩坑#14: Buildroot <cmath> 不完整)

#include <sys/time.h>

#ifdef HAS_RGA
#include <rga/im2d.h>
#include <rga/rga.h>    // RK_FORMAT_* 枚举在 rga.h, 不在 im2d.h
#endif

#include "rknn_api.h"

// ============ 常量 ============
static const int MODEL_W = 640;   // YOLOv5s 标准输入
static const int MODEL_H = 640;

// ============ 小工具 ============
static int64_t now_us() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static bool load_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out.resize(sz);
    size_t rd = fread(out.data(), 1, sz, f);
    fclose(f);
    return rd == (size_t)sz;
}

// ============ YOLOv5 后处理 (从 Phase 7.2 复用) ============
static float sigmoid(float x) { return 1.0f / (1.0f + expf(-x)); }

// COCO 80 类
static const char* COCO_CLASSES[80] = {
    "person","bicycle","car","motorcycle","airplane","bus","train","truck","boat",
    "traffic light","fire hydrant","stop sign","parking meter","bench","bird","cat",
    "dog","horse","sheep","cow","elephant","bear","zebra","giraffe","backpack",
    "umbrella","handbag","tie","suitcase","frisbee","skis","snowboard","sports ball",
    "kite","baseball bat","baseball glove","skateboard","surfboard","tennis racket",
    "bottle","wine glass","cup","fork","knife","spoon","bowl","banana","apple",
    "sandwich","orange","broccoli","carrot","hot dog","pizza","donut","cake","chair",
    "couch","potted plant","bed","dining table","toilet","tv","laptop","mouse",
    "remote","keyboard","cell phone","microwave","oven","toaster","sink",
    "refrigerator","book","clock","vase","scissors","teddy bear","hair drier","toothbrush"
};

struct DetectBox { float x1, y1, x2, y2, conf; int cls; };

static float iou(const DetectBox& a, const DetectBox& b) {
    float ix1 = std::max(a.x1, b.x1), iy1 = std::max(a.y1, b.y1);
    float ix2 = std::min(a.x2, b.x2), iy2 = std::min(a.y2, b.y2);
    float iw = std::max(0.0f, ix2 - ix1), ih = std::max(0.0f, iy2 - iy1);
    float inter = iw * ih;
    float uni = (a.x2 - a.x1) * (a.y2 - a.y1) + (b.x2 - b.x1) * (b.y2 - b.y1) - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

static void nms(std::vector<DetectBox>& boxes, float thresh) {
    std::sort(boxes.begin(), boxes.end(),
              [](const DetectBox& a, const DetectBox& b) { return a.conf > b.conf; });
    std::vector<DetectBox> keep;
    while (!boxes.empty()) {
        keep.push_back(boxes[0]);
        std::vector<DetectBox> rest;
        for (size_t i = 1; i < boxes.size(); i++)
            if (iou(boxes[0], boxes[i]) < thresh) rest.push_back(boxes[i]);
        boxes.swap(rest);
    }
    boxes.swap(keep);
}

// 解一个头: data 是 float* (NCHW [1,255,H,W]), 已反量化
static int decode_one_head(float* data, int grid_h, int grid_w, int stride,
                           const int anchor[6], float obj_thresh,
                           std::vector<DetectBox>& out) {
    int grid_len = grid_h * grid_w;
    int added = 0;
    for (int a = 0; a < 3; a++) {
        for (int gy = 0; gy < grid_h; gy++) {
            for (int gx = 0; gx < grid_w; gx++) {
                int base = (a * 85) * grid_len + gy * grid_w + gx;   // 通道 (a*85) 在 (gy,gx)
                float obj = sigmoid(data[base + 4 * grid_len]);
                if (obj < obj_thresh) continue;

                float tx = sigmoid(data[base + 0 * grid_len]);
                float ty = sigmoid(data[base + 1 * grid_len]);
                float tw = sigmoid(data[base + 2 * grid_len]);
                float th = sigmoid(data[base + 3 * grid_len]);

                float max_cls = 0.0f; int max_id = 0;
                for (int c = 0; c < 80; c++) {
                    float s = sigmoid(data[base + (5 + c) * grid_len]);
                    if (s > max_cls) { max_cls = s; max_id = c; }
                }
                if (max_cls < obj_thresh) continue;

                float conf = obj * max_cls;
                float cx = (2.0f * tx - 0.5f + gx) * stride;
                float cy = (2.0f * ty - 0.5f + gy) * stride;
                float bw = (2.0f * tw) * (2.0f * tw) * anchor[a * 2];
                float bh = (2.0f * th) * (2.0f * th) * anchor[a * 2 + 1];

                DetectBox b;
                b.x1 = cx - bw / 2.0f; b.y1 = cy - bh / 2.0f;
                b.x2 = cx + bw / 2.0f; b.y2 = cy + bh / 2.0f;
                b.conf = conf; b.cls = max_id;
                out.push_back(b);
                added++;
            }
        }
    }
    return added;
}

// ============ NV12 → RGB ============
// CPU 兜底转换 (BT.601 系数, 与 Phase 7.2 相同)
static void nv12_to_rgb_cpu(const uint8_t* nv12, int w, int h, uint8_t* rgb) {
    const uint8_t* y  = nv12;
    const uint8_t* uv = nv12 + (size_t)w * h;
    int half_w = w / 2;
    for (int j = 0; j < h; j++) {
        int uv_row = (j / 2) * half_w;
        for (int i = 0; i < w; i++) {
            int yv = y[j * w + i];
            int u  = uv[(uv_row + i / 2) * 2] - 128;
            int v  = uv[(uv_row + i / 2) * 2 + 1] - 128;
            int r = yv + ((1436 * v) >> 10);
            int g = yv - ((352 * u + 731 * v) >> 10);
            int b = yv + ((1815 * u) >> 10);
            size_t o = ((size_t)j * w + i) * 3;
            rgb[o + 0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
            rgb[o + 1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
            rgb[o + 2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
        }
    }
}

// ---- RGA 硬件转换 (librga 1.10 im2d API) ----
// 本 SDK 的 im2d 没有 importbuffer/handle 那套 API, 也没旧版 c_RkRgaBlit。
// 正确做法: wrapbuffer_virtualaddr 包 src/dst (都无 handle, 统一模式), 再 imcvtcolor。
static bool g_rga_warned = false;

// NV12 → RGB888, RGA 硬件转换。失败返回 false, 由调用方回退 CPU。
static bool rga_nv12_to_rgb(const uint8_t* nv12, int w, int h, uint8_t* rgb) {
#ifdef HAS_RGA
    // 宏签名: wrapbuffer_virtualaddr(vir_addr, width, height, format[, wstride, hstride])
    rga_buffer_t src = wrapbuffer_virtualaddr((void*)nv12, w, h, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_virtualaddr((void*)rgb,  w, h, RK_FORMAT_RGB_888);
    IM_STATUS st = imcvtcolor(src, dst, RK_FORMAT_YCbCr_420_SP, RK_FORMAT_RGB_888);

    if (st != IM_STATUS_SUCCESS) {   // 踩坑#20: SUCCESS=1, FAILED=0
        if (!g_rga_warned) {
            printf("[AI] ⚠️ RGA imcvtcolor 失败(status=%d), 回退 CPU\n", (int)st);
            g_rga_warned = true;
        }
        return false;
    }
    return true;
#else
    (void)nv12; (void)w; (void)h; (void)rgb;
    return false;
#endif
}

// letterbox: 640x480 → 640x640, 上下各 pad 80px, 灰 114 (纯 memcpy, 无缩放)
static void letterbox_rgb(const uint8_t* rgb480, int w, int h, uint8_t* rgb640) {
    memset(rgb640, 114, (size_t)MODEL_W * MODEL_H * 3);
    int pad_y = (MODEL_H - h) / 2;   // 80
    for (int y = 0; y < h; y++)
        memcpy(rgb640 + ((size_t)(y + pad_y) * MODEL_W) * 3,
               rgb480 + (size_t)y * w * 3, (size_t)w * 3);
}

// ============ AI 检测线程 ============
void* ai_thread(void* arg) {
    SharedState* s = static_cast<SharedState*>(arg);
    int W = s->width, H = s->height;
    std::string model_path = s->ai_model_path;
    float conf_thresh = s->ai_conf_thresh;

    // ---- 1. 加载模型 ----
    std::vector<uint8_t> model_buf;
    if (!load_file(model_path, model_buf)) {
        fprintf(stderr, "[AI] ❌ 打不开模型 %s: %s\n", model_path.c_str(), strerror(errno));
        return nullptr;
    }
    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model_buf.data(), (uint32_t)model_buf.size(), 0, nullptr);
    if (ret != 0) {
        fprintf(stderr, "[AI] ❌ rknn_init 失败 ret=%d\n", ret);
        return nullptr;
    }
    printf("[AI] 模型加载完成 (%zu 字节)\n", model_buf.size());

    // ---- 2. 查询输入/输出数量 (只需一次) ----
    rknn_input_output_num num{};
    if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &num, sizeof(num)) != 0) {
        fprintf(stderr, "[AI] ❌ 查询 IN_OUT_NUM 失败\n");
        rknn_destroy(ctx);
        return nullptr;
    }

    // ---- 3. 持久缓冲 ----
    std::vector<uint8_t> rgb480((size_t)W * H * 3);
    std::vector<uint8_t> rgb640((size_t)MODEL_W * MODEL_H * 3);
    bool rga_ok = true;   // 先假设 RGA 可用, 首次失败后锁死回退 CPU
    printf("[AI] 转换方式: RGA 硬件 (失败自动回退 CPU)\n");

    // ---- 4. 后处理常量 ----
    const int anchors[3][6] = {
        {10, 13, 16, 30, 33, 23},
        {30, 61, 62, 45, 59, 119},
        {116, 90, 156, 198, 373, 326}
    };
    const int strides[3] = {8, 16, 32};
    const int gh[3] = {80, 40, 20};
    const int gw[3] = {80, 40, 20};
    const int pad_y = (MODEL_H - H) / 2;   // 80

    uint64_t last_seq = 0;
    int det_cnt = 0, last_cnt = 0;
    int64_t last_log = now_us();

    // ---- 5. 主循环 ----
    while (s->running) {
        // 5a. 等待新帧 (条件变量, 零 CPU; 最新帧覆盖, AI 慢时自动丢帧)
        pthread_mutex_lock(&s->ai_nv12_mutex);
        while (s->ai_nv12_seq == last_seq && s->running)
            pthread_cond_wait(&s->ai_nv12_cond, &s->ai_nv12_mutex);
        if (!s->running) {
            pthread_mutex_unlock(&s->ai_nv12_mutex);
            break;
        }
        Nv12Frame local;
        local.data.swap(s->ai_nv12.data);   // O(1) 拿最新帧
        local.pts_us = s->ai_nv12.pts_us;
        last_seq = s->ai_nv12_seq;
        pthread_mutex_unlock(&s->ai_nv12_mutex);

        // 5b. NV12 → RGB480 (RGA 优先, 首次失败后锁死回退 CPU)
        if (rga_ok) {
            if (!rga_nv12_to_rgb(local.data.data(), W, H, rgb480.data()))
                rga_ok = false;
        }
        if (!rga_ok)
            nv12_to_rgb_cpu(local.data.data(), W, H, rgb480.data());

        // 5c. letterbox → RGB640
        letterbox_rgb(rgb480.data(), W, H, rgb640.data());

        // 5d. 喂入 + 推理
        rknn_input in{};
        in.index = 0;
        in.type  = RKNN_TENSOR_UINT8;
        in.fmt   = RKNN_TENSOR_NHWC;
        in.buf   = rgb640.data();
        in.size  = (uint32_t)rgb640.size();
        in.pass_through = 0;
        if (rknn_inputs_set(ctx, 1, &in) < 0) {
            fprintf(stderr, "[AI] ❌ rknn_inputs_set 失败\n");
            continue;
        }
        if (rknn_run(ctx, nullptr) < 0) {
            fprintf(stderr, "[AI] ❌ rknn_run 失败\n");
            continue;
        }

        std::vector<rknn_output> outs(num.n_output);
        for (uint32_t i = 0; i < num.n_output; i++) {
            outs[i].index = i;
            outs[i].want_float = 1;      // 让 runtime 反量化成 float
            outs[i].is_prealloc = 0;
        }
        if (rknn_outputs_get(ctx, num.n_output, outs.data(), nullptr) < 0) {
            fprintf(stderr, "[AI] ❌ rknn_outputs_get 失败\n");
            continue;
        }

        // 5e. 解码 + NMS
        std::vector<DetectBox> boxes;
        for (int o = 0; o < 3; o++)
            decode_one_head((float*)outs[o].buf, gh[o], gw[o], strides[o],
                            anchors[o], conf_thresh, boxes);
        nms(boxes, 0.45f);
        rknn_outputs_release(ctx, num.n_output, outs.data());

        // 5f. 坐标映射回摄像头 640x480 (模型 y → 摄像头 y-80) + 裁剪
        std::vector<AiBox> ai_boxes;
        ai_boxes.reserve(boxes.size());
        for (auto& b : boxes) {
            AiBox ab;
            ab.x1 = std::max(0.0f, std::min((float)W, b.x1));
            ab.y1 = std::max(0.0f, std::min((float)H, b.y1 - pad_y));
            ab.x2 = std::max(0.0f, std::min((float)W, b.x2));
            ab.y2 = std::max(0.0f, std::min((float)H, b.y2 - pad_y));
            ab.conf = b.conf;
            ab.cls  = b.cls;
            snprintf(ab.label, sizeof(ab.label), "%s %.2f",
                     COCO_CLASSES[b.cls], b.conf);
            ai_boxes.push_back(ab);
        }

        // 5g. 发布结果 (swap 到共享区, 编码线程读取)
        pthread_mutex_lock(&s->ai_result.mutex);
        s->ai_result.boxes.swap(ai_boxes);
        s->ai_result.seq++;
        pthread_mutex_unlock(&s->ai_result.mutex);

        // 5h. 日志
        det_cnt++;
        if ((det_cnt % 30) == 0) {
            int64_t now = now_us();
            double fps = (double)(det_cnt - last_cnt) * 1e6 / (double)(now - last_log);
            printf("[AI] %d 次检测 (%.1ffps) 本帧 %zu 框", det_cnt, fps, ai_boxes.size());
            for (size_t i = 0; i < ai_boxes.size() && i < 3; i++)
                printf(" | %s %.2f", COCO_CLASSES[ai_boxes[i].cls], ai_boxes[i].conf);
            printf("\n");
            last_log = now;
            last_cnt = det_cnt;
        }
    }

    // ---- 6. 清理 ----
    rknn_destroy(ctx);
    printf("[AI] 退出 (%d 次检测)\n", det_cnt);
    return nullptr;
}
