// libsr_vk.so —— 非高通机型 Vulkan GPU 超分后端（M1：骨架）
//
// 设计（见 docs §0C）
// ----------------
// 与 libsr_qnn 共用同一套 C ABI（sr_qnn.h），Python 层按能力选择后端：
//   有 QNN → libsr_qnn；无 QNN 有 Vulkan → libsr_vk；都没有 → 纯 CPU（libsr_qnn 的 CPU EP）。
// 这样上层 `sr_backend` 只需要换 so 名字，调用代码零改动。
//
// 与 ONNX 版的差异
// ---------------
// ncnn 模型的输入/输出 blob 名、缩放倍数都写在 param 里；本项目约定模型目录下有一个
// `models_vk.txt`（每行：NAME|param文件|bin文件|scale|tile），与 models.txt 并列、互不干扰。
//
// 当前阶段（M1）
// ------------
// - srq_init：初始化 ncnn GPU 实例，探测 Vulkan 设备数/名称；没有设备时返回 -191
// - 模型表：读 models_vk.txt
// - srq_add/srq_load：M2 实现（现在返回 -190 并给出明确错误，避免"看起来能用其实没算"）
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

// 源码树里 ncnn 头是平铺的（安装版才在 ncnn/ 子目录下）
#include "net.h"
#include "gpu.h"

namespace {

struct VkModel {
    std::string name;
    std::string param;
    std::string bin;
    int scale = 2;
    int tile = 192;
};

struct VkResultBlob {
    int width = 0;
    int height = 0;
    int channels = 3;
    std::vector<float> data;
};

std::vector<VkModel> g_models;
std::string g_modelDir;
std::string g_lastError;
bool g_inited = false;
bool g_gpuReady = false;
int g_gpuCount = 0;
std::string g_gpuName;
std::mutex g_mutex;

std::string Join(const std::string& dir, const std::string& file) {
    if (dir.empty()) {
        return file;
    }
    if (dir.back() == '/') {
        return dir + file;
    }
    return dir + "/" + file;
}

void SetErr(const std::string& err) {
    g_lastError = err;
}

/* models_vk.txt：NAME|param|bin|scale|tile（# 开头为注释） */
void LoadRegistry(const std::string& dir) {
    g_models.clear();
    FILE* fp = fopen(Join(dir, "models_vk.txt").c_str(), "r");
    if (fp == nullptr) {
        SetErr("models_vk.txt 不存在（Vulkan 模型清单未安装）");
        return;
    }
    char line[512];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        if (s.empty() || s[0] == '#') {
            continue;
        }
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); ++i) {
            if (i == s.size() || s[i] == '|') {
                parts.push_back(s.substr(start, i - start));
                start = i + 1;
            }
        }
        if (parts.size() < 3) {
            continue;
        }
        VkModel m;
        m.name = parts[0];
        m.param = parts[1];
        m.bin = parts[2];
        if (parts.size() > 3) m.scale = std::max(1, atoi(parts[3].c_str()));
        if (parts.size() > 4) m.tile = std::max(64, atoi(parts[4].c_str()));
        g_models.push_back(m);
    }
    fclose(fp);
}

}  // namespace

extern "C" {

int srq_init(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_inited) {
        return 0;
    }
    // ncnn Vulkan：没有可用设备时明确失败，别让上层以为在用 GPU
    ncnn::create_gpu_instance();
    g_gpuCount = ncnn::get_gpu_count();
    if (g_gpuCount <= 0) {
        SetErr("无可用 Vulkan 设备（libsr_vk 需要 GPU）");
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

int srq_set_model_dir(const char* dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_modelDir = dir != nullptr ? dir : "";
    if (!g_modelDir.empty()) {
        LoadRegistry(g_modelDir);
    }
    return 0;
}

int srq_model_count(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return static_cast<int>(g_models.size());
}

const char* srq_model_name(int index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index < 0 || index >= static_cast<int>(g_models.size())) {
        return "";
    }
    return g_models[index].name.c_str();
}

/* "file|scale|denoise|tile" —— 与 sr_qnn 同格式，便于上层复用同一份解析 */
const char* srq_model_info(int index) {
    static char buf[256];
    std::lock_guard<std::mutex> lock(g_mutex);
    if (index < 0 || index >= static_cast<int>(g_models.size())) {
        return "";
    }
    const VkModel& m = g_models[index];
    snprintf(buf, sizeof(buf), "%s|%d|0|%d", m.bin.c_str(), m.scale, m.tile);
    return buf;
}

int srq_model_id(const char* name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (name == nullptr) {
        return -2;
    }
    for (size_t i = 0; i < g_models.size(); ++i) {
        if (g_models[i].name == name) {
            return static_cast<int>(i);
        }
    }
    return -2;
}

const char* srq_get_version(void) {
    return "sr_vk 0.1.0 (ncnn Vulkan backend, M1 骨架)";
}

const char* srq_get_backend_info(void) {
    static char buf[256];
    snprintf(buf, sizeof(buf), "vulkan=%d device=%s", g_gpuReady ? 1 : 0,
             g_gpuName.empty() ? "(none)" : g_gpuName.c_str());
    return buf;
}

int srq_has_htp(void) {
    return 0;  // 这条链路只跑 GPU/Vulkan
}

int srq_get_last_error(void) {
    return 0;
}

const char* srq_get_error(void) {
    return g_lastError.c_str();
}

/* M2：tile 管线 + ncnn 推理。现在明确返回未实现，避免"静默返回原图"被误判为成功 */
int srq_add(const unsigned char* img, int width, int height, int stride, int channels,
            const char* modelName, int tile, int threads) {
    (void)img;
    (void)width;
    (void)height;
    (void)stride;
    (void)channels;
    (void)modelName;
    (void)tile;
    (void)threads;
    SetErr("libsr_vk M1 骨架：推理管线将在 M2 实现");
    return -190;
}

int srq_load(long long timeoutMs, void* out) {
    (void)timeoutMs;
    (void)out;
    return -190;
}

void srq_free_result(void* res) {
    (void)res;
}

}  // extern "C"
