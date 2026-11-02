// libsr_vk.so —— 非高通机型 Vulkan GPU 超分后端（M2：tile 管线 + ncnn 推理）
//
// ABI 与 sr_qnn.h **逐字段一致**（上层 ctypes 解析的是同一份 SrqResult/函数签名），
// 区别只在实现：这里用 ncnn 的 Vulkan 后端，模型是 ncnn param/bin（见 models_vk.txt）。
//
// 与 ONNX 版的语义对齐点（别自作聪明改）：
//   - 输入 RGB888/RGBA8888，stride 可大于 width*channels；
//   - tileSize<=0 时用模型的默认 tile；
//   - 输出 RGB888 紧凑缓冲，由 srq_free_result 释放；taskId 原样回传；
//   - tick 为实际耗时（秒）。
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "gpu.h"
#include "net.h"

extern "C" {
typedef struct SrqResult {
    unsigned char* data;
    int width;
    int height;
    int stride;
    int taskId;
    double tick;
    int status;
    char format[16];
} SrqResult;
}

namespace {

struct VkModel {
    std::string name;
    std::string param;
    std::string bin;
    int scale = 2;
    int tile = 192;
    int inCh = 3;      // 输入通道数：SRMD=19(3+15常量+noise) / SRMDNF=18 / 其它=3
};

struct Task {
    std::vector<unsigned char> pixels;
    int width = 0;
    int height = 0;
    int stride = 0;
    int channels = 3;
    int modelId = -1;
    int taskId = 0;
    int tileSize = 0;
    bool used = false;
    bool valid = false;
};

std::vector<VkModel> g_models;
std::string g_modelDir;
std::string g_lastError;
bool g_inited = false;
bool g_gpuReady = false;
std::string g_gpuName;
int g_device = 0;
int g_threads = 0;
Task g_pending;
std::mutex g_mutex;
ncnn::Net g_net;
std::string g_netModel;   // 当前已加载模型（切模型时重载）
bool g_netLoaded = false; // 编译时关了 NCNN_STRING，不能用 input_names() 判空

std::string Join(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    return (dir.back() == '/') ? dir + file : dir + "/" + file;
}

void SetErr(const std::string& err) {
    g_lastError = err;
}

void LoadRegistry(const std::string& dir) {
    g_models.clear();
    FILE* fp = fopen(Join(dir, "models_vk.txt").c_str(), "r");
    if (fp == nullptr) {
        SetErr("models_vk.txt 不存在");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '|') {
                parts.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        if (parts.size() < 3) continue;
        VkModel m;
        m.name = parts[0];
        m.param = parts[1];
        m.bin = parts[2];
        if (parts.size() > 3) m.scale = std::max(1, atoi(parts[3].c_str()));
        if (parts.size() > 4) m.tile = std::max(32, atoi(parts[4].c_str()));
        if (parts.size() > 5) m.inCh = std::max(3, atoi(parts[5].c_str()));
        g_models.push_back(m);
    }
    fclose(fp);
}

/* 加载（或切模型时重载）ncnn 网络：Vulkan + fp16 存储，与 ncnn-vulkan 发行版默认一致 */
bool EnsureNet(const VkModel& model) {
    if (g_netLoaded && g_netModel == model.name) {
        return true;
    }
    // 调试开关：JM_SR_VK_CPU=1 走 CPU（用来把"管线逻辑错误"和"Vulkan 驱动问题"分开）
    const bool useCpu = getenv("JM_SR_VK_CPU") != nullptr;
    fprintf(stderr, "[sr_vk] EnsureNet(%s) useCpu=%d\n", model.name.c_str(), (int)useCpu);
    g_net.clear();
    g_net.opt.use_vulkan_compute = !useCpu;
    // fp16 必须按设备能力开：x86 模拟器的虚拟 Adreno 报 support_fp16_storage=false，
    // 硬开会在 ncnn 内部直接崩（实测 MuMu 段错误）。
    const ncnn::GpuInfo& info = ncnn::get_gpu_info(g_device);
    const bool fp16 = info.support_fp16_storage() && info.support_fp16_packed();
    g_net.opt.use_fp16_packed = fp16;
    g_net.opt.use_fp16_storage = fp16;
    g_net.opt.use_fp16_arithmetic = fp16 && info.support_fp16_arithmetic();
    g_net.opt.use_int8_storage = false;
    g_net.opt.num_threads = g_threads > 0 ? g_threads : 4;
    g_net.set_vulkan_device(g_device);
    fprintf(stderr, "[sr_vk] load_param %s\n", model.param.c_str());
    if (g_net.load_param(Join(g_modelDir, model.param).c_str()) != 0) {
        SetErr("load_param 失败: " + model.param);
        return false;
    }
    fprintf(stderr, "[sr_vk] load_model %s\n", model.bin.c_str());
    if (g_net.load_model(Join(g_modelDir, model.bin).c_str()) != 0) {
        SetErr("load_model 失败: " + model.bin);
        return false;
    }
    g_netModel = model.name;
    g_netLoaded = true;
    return true;
}

/* 取 I/O 的 **blob 索引**（不是"第几个输入/输出"）：
 * ncnn 的 Extractor::input/extract(int) 收的是 blob_index，
 * 直接用 0 会拿到 blob 0（输入块）当输出 —— 实测表现为"输出跟输入一样大"。
 * input_indexes()/output_indexes() 在 NCNN_STRING=OFF 下也可用，用它们最稳。 */
int InputBlobIndex() {
    const std::vector<int>& idx = g_net.input_indexes();
    return idx.empty() ? 0 : idx[0];
}

int OutputBlobIndex() {
    const std::vector<int>& idx = g_net.output_indexes();
    return idx.empty() ? 0 : idx[0];
}

/* SRMD 的 15 个退化常量（原样抄自 srmd_preproc.comp，顺序不能改） */
const float kDegradation[15] = {
    -1.12360956e-08f, -1.36899159e-08f, 1.85637958e-02f, 2.86066886e-08f,
    3.35292965e-02f, 8.37272935e-08f, -2.54424009e-07f, -3.16234976e-02f,
    -1.35169253e-02f, -1.10466090e-08f, 3.84753793e-02f, 3.79465739e-08f,
    -2.44916752e-01f, -8.02213490e-01f, -5.40549755e-01f,
};
constexpr float kNoiseLevel = 0.0f;   // 统一按"干净图"烘焙（与 ONNX 版一致）

/* 单个 tile：RGB uint8 → ncnn::Mat（planar float 0..1）→ 推理 → 回写 RGB uint8 */
bool RunTile(const Task& task, int x0, int y0, int tw, int th, int scale, int inCh,
             unsigned char* dst, int dstStride, int dstW, int dstH) {
    ncnn::Mat in(tw, th, inCh);
    if (in.empty()) return false;
    for (int c = 3; c < inCh; ++c) {              // 条件通道：15 个退化常量 (+1 个 noise)
        float* plane = in.channel(c);
        float v = (c - 3 < 15) ? kDegradation[c - 3] : (kNoiseLevel / 255.0f);
        for (int i = 0; i < tw * th; ++i) plane[i] = v;
    }
    for (int c = 0; c < 3; ++c) {
        float* plane = in.channel(c);
        for (int y = 0; y < th; ++y) {
            const unsigned char* row = task.pixels.data() + (size_t)(y0 + y) * task.stride +
                                       (size_t)x0 * task.channels + c;
            for (int x = 0; x < tw; ++x) {
                plane[y * tw + x] = (float)row[(size_t)x * task.channels] / 255.0f;
            }
        }
    }

    static int g_tileLog = 0;
    if (g_tileLog < 2) fprintf(stderr, "[sr_vk] tile %dx%d @(%d,%d) in=%d/%d scale=%d\n",
                               tw, th, x0, y0, InputBlobIndex(), OutputBlobIndex(), scale);
    ncnn::Extractor ex = g_net.create_extractor();
    ex.input(InputBlobIndex(), in);
    if (g_tileLog < 2) fprintf(stderr, "[sr_vk] input fed\n");
    ncnn::Mat out;
    if (ex.extract(OutputBlobIndex(), out) != 0) {
        SetErr("extract 失败");
        return false;
    }
    if (g_tileLog < 2) { fprintf(stderr, "[sr_vk] extracted c=%d w=%d h=%d\n", out.c, out.w, out.h); ++g_tileLog; }
    if (out.c != 3 || out.w != tw * scale || out.h != th * scale) {
        char buf[160];
        snprintf(buf, sizeof(buf), "输出形状异常: c=%d w=%d h=%d，期望 3/%d/%d", out.c, out.w, out.h,
                 tw * scale, th * scale);
        SetErr(buf);
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        const float* plane = out.channel(c);
        for (int y = 0; y < out.h; ++y) {
            int dy = y0 * scale + y;
            if (dy < 0 || dy >= dstH) continue;
            unsigned char* row = dst + (size_t)dy * dstStride;
            for (int x = 0; x < out.w; ++x) {
                int dx = x0 * scale + x;
                if (dx < 0 || dx >= dstW) continue;
                float v = plane[(size_t)y * out.w + x] * 255.0f;
                v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
                row[(size_t)dx * 3 + c] = (unsigned char)(v + 0.5f);
            }
        }
    }
    return true;
}

bool Process(const Task& task, SrqResult* out) {
    if (task.modelId < 0 || task.modelId >= (int)g_models.size()) {
        SetErr("非法 modelId");
        return false;
    }
    const VkModel& model = g_models[task.modelId];
    if (!EnsureNet(model)) return false;

    const int scale = model.scale;
    const int dstW = task.width * scale;
    const int dstH = task.height * scale;
    const int dstStride = dstW * 3;
    unsigned char* dst = (unsigned char*)malloc((size_t)dstStride * dstH);
    if (dst == nullptr) {
        SetErr("输出缓冲分配失败");
        return false;
    }
    const int tile = task.tileSize > 0 ? task.tileSize : model.tile;

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int y0 = 0; y0 < task.height; y0 += tile) {
        for (int x0 = 0; x0 < task.width; x0 += tile) {
            int tw = std::min(tile, task.width - x0);
            int th = std::min(tile, task.height - y0);
            if (!RunTile(task, x0, y0, tw, th, scale, model.inCh, dst, dstStride, dstW, dstH)) {
                free(dst);
                return false;
            }
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    out->data = dst;
    out->width = dstW;
    out->height = dstH;
    out->stride = dstStride;
    out->taskId = task.taskId;
    out->tick = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    out->status = 0;
    snprintf(out->format, sizeof(out->format), "%s", "RGB888");
    return true;
}

}  // namespace

extern "C" {

int srq_init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_inited) return 0;
    ncnn::create_gpu_instance();
    if (ncnn::get_gpu_count() <= 0) {
        SetErr("无可用 Vulkan 设备");
        ncnn::destroy_gpu_instance();
        return -191;
    }
    const ncnn::GpuInfo& info = ncnn::get_gpu_info(0);
    char buf[128] = {0};
    snprintf(buf, sizeof(buf), "%s", info.device_name());
    g_gpuName = buf;
    g_gpuReady = true;
    g_inited = true;
    return 0;
}

void srq_set_ort_api_base(void* fn) {
    (void)fn;  // Vulkan 后端不依赖 ONNX Runtime
}

int srq_init_set(int device, int threads) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_device = device < 0 ? 0 : device;
    g_threads = threads;
    g_netModel.clear();   // 参数变了，下次推理重新加载网络
    return 0;
}

void srq_set_debug(int enable) {
    (void)enable;
}

const char* srq_get_version(void) {
    return "sr_vk 0.2.0 (ncnn Vulkan backend, M2 tile pipeline)";
}

const char* srq_get_backend_info(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "vulkan=%d device=%s", g_gpuReady ? 1 : 0,
             g_gpuName.empty() ? "(none)" : g_gpuName.c_str());
    return buf;
}

int srq_has_htp(void) { return 0; }
int srq_get_cpu_core_num(void) { return 4; }
int srq_get_gpu_core_num(void) { return 1; }
const char* srq_get_last_error(void) { return g_lastError.c_str(); }
const char* srq_get_htp_error(void) { return ""; }

int srq_set_model_dir(const char* dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_modelDir = dir != nullptr ? dir : "";
    if (!g_modelDir.empty()) LoadRegistry(g_modelDir);
    return 0;
}

int srq_model_count(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return (int)g_models.size();
}

const char* srq_model_name(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index < 0 || index >= (int)g_models.size()) return "";
    return g_models[index].name.c_str();
}

const char* srq_model_info(int index) {
    static char buf[256];
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index < 0 || index >= (int)g_models.size()) return "";
    const VkModel& m = g_models[index];
    snprintf(buf, sizeof(buf), "%s|%d|0|%d", m.bin.c_str(), m.scale, m.tile);
    return buf;
}

int srq_model_id(const char* name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (name == nullptr) return -2;
    for (size_t i = 0; i < g_models.size(); ++i) {
        if (g_models[i].name == name) return (int)i;
    }
    return -2;
}

int srq_add(const unsigned char* img, int width, int height, int stride, int channels,
            int modelId, int taskId, double scale, int targetW, int targetH,
            const char* format, int tileSize) {
    (void)scale;
    (void)targetW;
    (void)targetH;
    (void)format;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (img == nullptr || width <= 0 || height <= 0) {
        SetErr("非法图像参数");
        return -1;
    }
    if (channels != 3 && channels != 4) {
        SetErr("channels 只支持 3/4");
        return -1;
    }
    if (stride <= 0) stride = width * channels;
    if (stride < width * channels) {
        SetErr("stride 小于 width*channels");
        return -1;
    }
    g_pending.pixels.assign(img, img + (size_t)stride * height);
    g_pending.width = width;
    g_pending.height = height;
    g_pending.stride = stride;
    g_pending.channels = channels;
    g_pending.modelId = modelId;
    g_pending.taskId = taskId;
    g_pending.tileSize = tileSize;
    g_pending.used = true;
    g_pending.valid = true;
    return taskId > 0 ? taskId : 1;
}

int srq_load(int timeoutMs, SrqResult* out) {
    (void)timeoutMs;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (out == nullptr) return 0;
    memset(out, 0, sizeof(*out));
    if (!g_pending.used) return 0;
    Task task = g_pending;      // 拷贝：处理期间允许上层继续 add
    g_pending.used = false;
    if (!Process(task, out)) {
        out->status = -1;
        out->taskId = task.taskId;
    }
    return 1;
}

void srq_free_result(SrqResult* res) {
    if (res != nullptr && res->data != nullptr) {
        free(res->data);
        res->data = nullptr;
    }
}

void srq_stop(void) {}

void srq_remove(const int* taskIds, int count) {
    (void)taskIds;
    (void)count;
}

}  // extern "C"
