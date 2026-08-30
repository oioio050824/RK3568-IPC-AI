/**
 * Phase 7.1 — RKNN 模型探测 + 首帧推理（独立 demo）
 *
 * 目的（验证地基，不是最终产品）:
 *   [1/6] 加载 /yolov5s.rknn，打印模型真实输入/输出格式（关键未知量）
 *   [2/6] 查询 tensor 数量 + 每个 tensor 的 dims/type/fmt/size
 *   [3/6] 抓一帧 NV12 (640x480) → RGA 转 RGB (640x640 letterbox)
 *   [4/6] rknn_inputs_set 喂入
 *   [5/6] rknn_run 推理
 *   [6/6] 打印每个输出 tensor 的 shape + 前 20 个 float 值
 *
 * 检测框的 decode + NMS 放到 7.2 —— 要先看到模型真实输出格式（3 头 / 1 头 /
 * 已解码等好几种约定），用这份输出确认后再写，否则必踩坑。
 *
 * 用法:
 *   ./rknn_demo                                  # 真机: 摄像头 + RGA + NPU
 *   ./rknn_demo /yolov5s.rknn --synthetic   # 跳过摄像头/RGA, 喂灰图只测 NPU
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#include <math.h>     // expf (踩坑#14: Buildroot <cmath> 不完整)

#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "rknn_api.h"

// ============ 常量 ============
static const int MODEL_W = 640;   // YOLOv5s 标准输入
static const int MODEL_H = 640;
static const int CAM_W   = 640;   // 摄像头输出 (RK3568 ISP 上限 800x600)
static const int CAM_H   = 480;

// ============ 小工具 ============
static int xioctl(int fd, unsigned long req, void* arg) {
    int r;
    do { r = ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

static const char* tensor_type_str(rknn_tensor_type t) {
    switch (t) {
        case RKNN_TENSOR_FLOAT32: return "FLOAT32";
        case RKNN_TENSOR_FLOAT16: return "FLOAT16";
        case RKNN_TENSOR_INT8:    return "INT8";
        case RKNN_TENSOR_UINT8:   return "UINT8";
        case RKNN_TENSOR_INT16:   return "INT16";
        case RKNN_TENSOR_INT32:   return "INT32";
        default:                  return "UNKNOWN";
    }
}

static const char* tensor_fmt_str(rknn_tensor_format f) {
    switch (f) {
        case RKNN_TENSOR_NCHW:      return "NCHW";
        case RKNN_TENSOR_NHWC:      return "NHWC";
        case RKNN_TENSOR_NC1HWC2:   return "NC1HWC2";
        case RKNN_TENSOR_UNDEFINED: return "UNDEFINED";
        default:                    return "?";
    }
}

static bool load_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        fprintf(stderr, "❌ 打不开模型 %s: %s\n", path.c_str(), strerror(errno));
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    out.resize(sz);
    size_t rd = fread(out.data(), 1, sz, f);
    fclose(f);
    return rd == (size_t)sz;
}

// ============ [2/6] 打印模型 spec ============
static void print_model_spec(rknn_context ctx) {
    rknn_sdk_version ver{};
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == 0)
        printf("  rknn_api 版本: %s\n", ver.api_version);

    rknn_input_output_num num{};
    if (rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &num, sizeof(num)) != 0) return;
    printf("  输入 tensor: %u 个, 输出 tensor: %u 个\n", num.n_input, num.n_output);

    for (uint32_t i = 0; i < num.n_input; i++) {
        rknn_tensor_attr a{};
        a.index = i;
        if (rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &a, sizeof(a)) != 0) continue;
        printf("  [输入 %u] dims=[", i);
        for (uint32_t d = 0; d < a.n_dims; d++) printf("%s%u", d ? "," : "", a.dims[d]);
        printf("] type=%s fmt=%s size=%u\n", tensor_type_str(a.type), tensor_fmt_str(a.fmt), a.size);
    }
    for (uint32_t i = 0; i < num.n_output; i++) {
        rknn_tensor_attr a{};
        a.index = i;
        if (rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a)) != 0) continue;
        printf("  [输出 %u] dims=[", i);
        for (uint32_t d = 0; d < a.n_dims; d++) printf("%s%u", d ? "," : "", a.dims[d]);
        printf("] type=%s fmt=%s size=%u scale=%.6f zp=%d\n",
               tensor_type_str(a.type), tensor_fmt_str(a.fmt), a.size, a.scale, a.zp);
    }
}

// ============ [3/6] 抓一帧 NV12 (Phase 1 逻辑, 单帧版) ============
static bool capture_one_nv12(std::vector<uint8_t>& out, int& w, int& h) {
    int fd = open("/dev/video0", O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "❌ 打不开 /dev/video0: %s\n", strerror(errno));
        return false;
    }

    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width       = CAM_W;
    fmt.fmt.pix_mp.height      = CAM_H;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field       = V4L2_FIELD_ANY;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0) {
        fprintf(stderr, "❌ VIDIOC_S_FMT 失败: %s\n", strerror(errno));
        close(fd); return false;
    }
    w = (int)fmt.fmt.pix_mp.width;
    h = (int)fmt.fmt.pix_mp.height;

    v4l2_requestbuffers req{};
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count  = 4;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "❌ VIDIOC_REQBUFS 失败: %s\n", strerror(errno));
        close(fd); return false;
    }

    struct MmapBuf { void* ptr; size_t len; };
    std::vector<MmapBuf> bufs(req.count);
    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_buffer b{}; v4l2_plane planes[8]{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.m.planes = planes; b.length = 8;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) { close(fd); return false; }
        bufs[i].len = b.m.planes[0].length;
        bufs[i].ptr = mmap(nullptr, b.m.planes[0].length,
                           PROT_READ | PROT_WRITE, MAP_SHARED,
                           fd, b.m.planes[0].m.mem_offset);
        if (bufs[i].ptr == MAP_FAILED) { close(fd); return false; }
    }

    for (uint32_t i = 0; i < req.count; i++) {
        v4l2_buffer b{}; v4l2_plane planes[8]{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP; b.index = i;
        b.m.planes = planes; b.length = 8;
        xioctl(fd, VIDIOC_QBUF, &b);
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "❌ VIDIOC_STREAMON 失败: %s\n", strerror(errno));
        close(fd); return false;
    }

    // 丢 9 帧让 ISP 3A 收敛
    for (int i = 0; i < 9; i++) {
        v4l2_buffer b{}; v4l2_plane planes[8]{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP; b.m.planes = planes; b.length = 8;
        if (xioctl(fd, VIDIOC_DQBUF, &b) == 0) xioctl(fd, VIDIOC_QBUF, &b);
    }

    // 取第 10 帧
    v4l2_buffer b{}; v4l2_plane planes[8]{};
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP; b.m.planes = planes; b.length = 8;
    if (xioctl(fd, VIDIOC_DQBUF, &b) < 0) {
        fprintf(stderr, "❌ VIDIOC_DQBUF 失败: %s\n", strerror(errno));
        close(fd); return false;
    }
    size_t bytes = b.m.planes[0].bytesused;
    out.assign((uint8_t*)bufs[b.index].ptr, (uint8_t*)bufs[b.index].ptr + bytes);
    xioctl(fd, VIDIOC_QBUF, &b);

    xioctl(fd, VIDIOC_STREAMOFF, &type);
    for (auto& x : bufs) if (x.ptr && x.ptr != MAP_FAILED) munmap(x.ptr, x.len);
    close(fd);
    printf("  NV12: %dx%d, %zu 字节\n", w, h, out.size());
    return true;
}

// ============ [3/6] NV12 -> RGB 640x640 (letterbox) ============
// 注: 先用 CPU 转换验证链路 (librga 2.x handle 混用问题 7.3 再解, 换回 RGA 零拷贝)
static void nv12_to_rgb_cpu(const uint8_t* nv12, int w, int h, uint8_t* rgb) {
    const uint8_t* y  = nv12;
    const uint8_t* uv = nv12 + (size_t)w * h;   // NV12: Y 平面后接交织 UV
    int half_w = w / 2;
    for (int j = 0; j < h; j++) {
        int uv_row = (j / 2) * half_w;
        for (int i = 0; i < w; i++) {
            int yv = y[j * w + i];
            int u  = uv[(uv_row + i / 2) * 2] - 128;
            int v  = uv[(uv_row + i / 2) * 2 + 1] - 128;
            // BT.601 全范围, libyuv 系数 (×1024)
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

static bool nv12_to_rgb640(const std::vector<uint8_t>& nv12, int w, int h,
                           std::vector<uint8_t>& rgb640) {
    // 1) NV12(640x480) -> RGB(640x480)  [CPU]
    std::vector<uint8_t> rgb480((size_t)w * h * 3);
    nv12_to_rgb_cpu(nv12.data(), w, h, rgb480.data());

    // 2) letterbox: 640x480 -> 640x640, 上下各 pad 80px, 灰 114
    //    (scale = min(640/640, 640/480) = 1.0, 所以不缩放只 pad)
    rgb640.assign((size_t)MODEL_W * MODEL_H * 3, 114);
    int pad_y = (MODEL_H - h) / 2;   // 80
    for (int y = 0; y < h; y++)
        memcpy(&rgb640[((size_t)(y + pad_y) * MODEL_W) * 3], &rgb480[(size_t)y * w * 3], (size_t)w * 3);
    printf("  RGB: %dx%d -> %dx%d (letterbox pad_y=%d, CPU 转换)\n", w, h, MODEL_W, MODEL_H, pad_y);
    return true;
}

// ============ YOLOv5 后处理 (7.2: 解码 + NMS) ============
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

// ============ 主流程 ============
int main(int argc, char** argv) {
    std::string model_path = "/yolov5s.rknn";
    bool synthetic = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--synthetic") synthetic = true;
        else model_path = a;
    }

    printf("=== Phase 7.1: RKNN 模型探测 + 首帧推理 ===\n");
    printf("模型: %s, 模式: %s\n", model_path.c_str(), synthetic ? "synthetic(灰图)" : "摄像头");

    // ---- [1/6] 加载模型 ----
    printf("\n[1/6] 加载模型...\n");
    std::vector<uint8_t> model_buf;
    if (!load_file(model_path, model_buf)) return 1;
    printf("  模型 %zu 字节\n", model_buf.size());

    rknn_context ctx = 0;
    int ret = rknn_init(&ctx, model_buf.data(), (uint32_t)model_buf.size(), 0, nullptr);
    if (ret != 0) {
        fprintf(stderr, "❌ rknn_init 失败, ret=%d\n", ret);
        return 1;
    }

    // ---- [2/6] 打印模型 spec ----
    printf("\n[2/6] 模型 spec:\n");
    print_model_spec(ctx);

    // ---- [3/6] 准备输入 ----
    printf("\n[3/6] 准备输入...\n");
    std::vector<uint8_t> rgb640;
    if (synthetic) {
        rgb640.assign((size_t)MODEL_W * MODEL_H * 3, 114);   // 纯灰图
        printf("  合成灰图 %dx%dx3\n", MODEL_W, MODEL_H);
    } else {
        std::vector<uint8_t> nv12;
        int w = CAM_W, h = CAM_H;
        if (!capture_one_nv12(nv12, w, h)) return 1;
        if (!nv12_to_rgb640(nv12, w, h, rgb640)) return 1;
    }

    // ---- [4/6] 设置输入 ----
    printf("\n[4/6] 设置输入...\n");
    rknn_input in{};
    in.index = 0;
    in.type  = RKNN_TENSOR_UINT8;
    in.fmt   = RKNN_TENSOR_NHWC;
    in.buf   = rgb640.data();
    in.size  = (uint32_t)rgb640.size();
    in.pass_through = 0;
    ret = rknn_inputs_set(ctx, 1, &in);
    if (ret < 0) {
        fprintf(stderr, "❌ rknn_inputs_set 失败, ret=%d\n", ret);
        return 1;
    }

    // ---- [5/6] 推理 ----
    printf("\n[5/6] 推理...\n");
    ret = rknn_run(ctx, nullptr);
    if (ret < 0) {
        fprintf(stderr, "❌ rknn_run 失败, ret=%d\n", ret);
        return 1;
    }
    printf("  rknn_run 成功\n");

    // ---- [6/6] 取输出 + 打印 ----
    printf("\n[6/6] 输出 tensor:\n");
    rknn_input_output_num num{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &num, sizeof(num));

    std::vector<rknn_output> outs(num.n_output);
    for (uint32_t i = 0; i < num.n_output; i++) {
        outs[i].index = i;
        outs[i].want_float = 1;      // 让 runtime 反量化成 float
        outs[i].is_prealloc = 0;
    }
    ret = rknn_outputs_get(ctx, num.n_output, outs.data(), nullptr);
    if (ret < 0) {
        fprintf(stderr, "❌ rknn_outputs_get 失败, ret=%d\n", ret);
        return 1;
    }

    // ---- 解码 + NMS (7.2) ----
    const int anchors[3][6] = {
        {10, 13, 16, 30, 33, 23},
        {30, 61, 62, 45, 59, 119},
        {116, 90, 156, 198, 373, 326}
    };
    const int strides[3] = {8, 16, 32};
    const int gh[3] = {80, 40, 20};
    const int gw[3] = {80, 40, 20};

    std::vector<DetectBox> boxes;
    for (int o = 0; o < 3; o++) {
        float* data = (float*)outs[o].buf;
        decode_one_head(data, gh[o], gw[o], strides[o], anchors[o], 0.25f, boxes);
    }
    nms(boxes, 0.45f);

    printf("  检测到 %zu 个目标:\n", boxes.size());
    for (auto& b : boxes) {
        printf("    %-14s conf=%.2f  box=[%.0f, %.0f, %.0f, %.0f]\n",
               COCO_CLASSES[b.cls], b.conf, b.x1, b.y1, b.x2, b.y2);
    }

    rknn_outputs_release(ctx, num.n_output, outs.data());
    rknn_destroy(ctx);
    printf("\n=== 完成 ===\n");
    return 0;
}
