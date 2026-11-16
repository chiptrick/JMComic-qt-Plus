/* sr_qnn_proc.cpp —— sr_qnn 的"独立进程"入口
 *
 * 为什么需要它(真机 SM8750 上验证过的问题链)：
 *   QNN HTP 后端需要厂商库 libcdsprpc.so(FastRPC)。App 进程的链接器命名空间只允许
 *   加载 nativeLibraryDir + public.libraries.txt 里列出的库，vivo/部分机型的固件不把
 *   libcdsprpc.so 列进去 → 在 App 进程里 dlopen 必然失败 → HTP 起不来 → QNN 0 节点，
 *   超分退化成纯 CPU(实测 ~400ms/块)。
 *
 *   独立 native 进程没有这个限制：LD_LIBRARY_PATH 会被动态链接器读取，
 *   "/vendor/lib64/libcdsprpc.so" 直接可用 —— 这正是 local-dream(xororz) 等
 *   "真跑 NPU"项目的做法：推理引擎放子进程，QNN 库按需布置，厂商库用设备自带的那份。
 *
 * 协议(stdin/stdout, 锁步一问一答, 小端, 裸字段无整体长度帧)：
 *   请求  u32 cmd   (1=hello 2=add 3=stop)
 *   hello 回复: 引擎版本/后端/HTP/模型表/错误串
 *   add  请求: 任务参数 + 原始像素
 *   add  回复: 结果头 + 原始像素(stride*height 字节, RGB888)
 *
 * 由 android/sr_qnn/__init__.py 通过 subprocess 启动；stderr 接到 sr_native.log，
 * ORT 的自定义 logger(见 sr_qnn.cpp)会把 QNN 的报错也写进去。
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#if defined(__ANDROID__)
#include <android/dlext.h>
#endif
#include <unistd.h>

#include "sr_qnn.h"

namespace {

/* 协议输出 fd。启动时把 fd1 抢出来单独保存，再把 fd1 重定向到 stderr ——
 * sr_qnn.cpp 里有大量 printf 调试输出，如果它们落到 fd1，就会和二进制协议混在
 * 一起，父进程解析必然错位。重定向后：协议走 g_outFd，引擎日志走 stderr(日志文件)。 */
int g_outFd = 1;

bool ReadExact(void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(0, p + got, len - got);
        if (n <= 0) {
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool WriteExact(const void* buf, size_t len) {
    const auto* p = static_cast<const uint8_t*>(buf);
    size_t put = 0;
    while (put < len) {
        ssize_t n = write(g_outFd, p + put, len - put);
        if (n <= 0) {
            return false;
        }
        put += static_cast<size_t>(n);
    }
    return true;
}

template <typename T>
bool Put(const T& v) {
    return WriteExact(&v, sizeof(v));
}

template <typename T>
bool Get(T& v) {
    return ReadExact(&v, sizeof(v));
}

bool PutStr(const std::string& s, size_t cap) {
    std::vector<char> buf(cap, 0);
    size_t n = s.size() < cap ? s.size() : cap - 1;
    if (n > 0) {
        memcpy(buf.data(), s.data(), n);
    }
    return WriteExact(buf.data(), buf.size());
}

bool PutBlob(const char* data, size_t len) {
    uint32_t n = static_cast<uint32_t>(len);
    if (!Put(n)) {
        return false;
    }
    return len == 0 || WriteExact(data, len);
}

bool GetBlob(std::vector<char>& out) {
    uint32_t n = 0;
    if (!Get(n)) {
        return false;
    }
    out.resize(n);
    return n == 0 || ReadExact(out.data(), n);
}

std::string TrimStr(const char* p) {
    return p != nullptr ? std::string(p) : std::string();
}

/* "file|scale|denoise|tile" -> 四段 */
bool SplitModelInfo(const char* raw, std::string& file, int32_t& scale, int32_t& denoise, int32_t& tile) {
    if (raw == nullptr) {
        return false;
    }
    std::vector<std::string> parts;
    const char* begin = raw;
    for (const char* p = raw;; ++p) {
        if (*p == '|' || *p == '\0') {
            parts.emplace_back(begin, p);
            if (*p == '\0') {
                break;
            }
            begin = p + 1;
        }
    }
    if (parts.size() < 4) {
        return false;
    }
    file = parts[0];
    scale = atoi(parts[1].c_str());
    denoise = atoi(parts[2].c_str());
    tile = atoi(parts[3].c_str());
    return !file.empty() && scale > 0 && tile > 0;
}

bool HandleHello() {
    if (getenv("JM_QNN_DEBUG") != nullptr) {
        srq_set_debug(1);
    }
    fprintf(stderr, "[sr_qnn_proc] hello: srq_init begin\n");
    fflush(stderr);
    int32_t status = srq_init();
    fprintf(stderr, "[sr_qnn_proc] hello: srq_init=%d\n", status);
    fflush(stderr);
    int32_t threads = 0;
    {
        const char* t = getenv("JM_QNN_PROC_THREADS");
        if (t != nullptr) {
            threads = atoi(t);
        }
    }
    if (status >= 0) {
        // 启动引擎内部 worker(HTP 单引擎流水 / CPU 多线程都由引擎自己决定)
        srq_init_set(0, threads);
    }
    const uint32_t cmd = 1;
    if (!Put(cmd) || !Put(status)) {
        return false;
    }
    if (!PutStr(TrimStr(srq_get_version()), 64)) {
        return false;
    }
    if (!PutStr(TrimStr(srq_get_backend_info()), 160)) {
        return false;
    }
    const int32_t htp = srq_has_htp();
    if (!Put(htp)) {
        return false;
    }
    if (!PutStr(TrimStr(srq_get_htp_error()), 256)) {
        return false;
    }
    const int32_t cores = srq_get_cpu_core_num();
    if (!Put(cores)) {
        return false;
    }
    const int32_t count = status >= 0 ? srq_model_count() : 0;
    if (!Put(count)) {
        return false;
    }
    for (int32_t i = 0; i < count; ++i) {
        const char* name = srq_model_name(i);
        const char* info = srq_model_info(i);
        std::string file;
        int32_t scale = 0, denoise = 0, tile = 0;
        SplitModelInfo(info, file, scale, denoise, tile);
        const std::string nameStr = TrimStr(name);
        const int32_t id = srq_model_id(name);
        if (!Put(id)) {
            return false;
        }
        if (!PutBlob(nameStr.data(), nameStr.size())) {
            return false;
        }
        if (!PutBlob(file.data(), file.size())) {
            return false;
        }
        if (!Put(scale) || !Put(denoise) || !Put(tile)) {
            return false;
        }
    }
    return PutBlob(TrimStr(srq_get_last_error()).data(), TrimStr(srq_get_last_error()).size());
}

bool HandleAdd() {
    int32_t taskId = 0, modelId = 0, targetW = 0, targetH = 0;
    int32_t width = 0, height = 0, stride = 0, channels = 0, tile = 0;
    double scale = 0;
    char format[16] = {0};
    if (!Get(taskId) || !Get(modelId) || !Get(scale) || !Get(targetW) || !Get(targetH) ||
        !Get(width) || !Get(height) || !Get(stride) || !Get(channels) ||
        !ReadExact(format, sizeof(format)) || !Get(tile)) {
        return false;
    }
    std::vector<char> pixels;
    if (!GetBlob(pixels)) {
        return false;
    }
    const uint32_t cmd = 2;
    SrqResult result;
    memset(&result, 0, sizeof(result));
    std::string err;
    int32_t status = -1;
    double tick = 0;
    if (width <= 0 || height <= 0 || stride < width * channels || channels < 3 ||
        pixels.size() < static_cast<size_t>(stride) * height) {
        err = "add 参数非法";
        status = -2;
    } else {
        status = srq_add(reinterpret_cast<const unsigned char*>(pixels.data()),
                         width, height, stride, channels, modelId, taskId, scale,
                         targetW, targetH, format, tile);
        if (status <= 0) {
            err = TrimStr(srq_get_last_error());
            status = -3;
        } else {
            const int got = srq_load(0, &result);   /* 0 = 一直等到结果 */
            if (got <= 0) {
                err = TrimStr(srq_get_last_error());
                status = -4;
        } else {
            status = result.status;
            tick = result.tick;
            if (status < 0) {
                // 任务级失败(如 -11 模型文件缺失)：必须把引擎的错误原因
                // 带回去，否则父进程只看到 status<0，不知道缺哪个文件、
                // 该换模型还是补文件，只能看到一个空结果
                err = TrimStr(srq_get_last_error());
            }
        }
        }
    }
    int32_t outW = 0, outH = 0, outStride = 0;
    char outFormat[16] = {0};
    std::vector<char> outPixels;
    if (status >= 0) {
        outW = result.width;
        outH = result.height;
        outStride = result.stride;
        memcpy(outFormat, result.format, sizeof(outFormat));
        outPixels.resize(static_cast<size_t>(outStride) * outH);
        if (!result.data || outPixels.empty()) {
            status = -5;
            err = "结果数据为空";
        } else {
            memcpy(outPixels.data(), result.data, outPixels.size());
        }
        srq_free_result(&result);
    }
    if (!Put(cmd) || !Put(taskId) || !Put(status) || !Put(tick) ||
        !Put(outW) || !Put(outH) || !Put(outStride) ||
        !WriteExact(outFormat, sizeof(outFormat))) {
        return false;
    }
    if (!PutBlob(err.data(), err.size())) {
        return false;
    }
    if (status < 0) {
        const uint32_t zero = 0;
        return Put(zero);
    }
    return PutBlob(outPixels.data(), outPixels.size());
}

std::string SelfDir() {
    char buf[512];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return std::string();
    }
    buf[n] = '\0';
    std::string path(buf);
    size_t pos = path.find_last_of('/');
    return pos == std::string::npos ? std::string() : path.substr(0, pos);
}

bool LoadOrtFrom(const std::string& path, std::string& err) {
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (handle == nullptr) {
        const char* msg = dlerror();
        err = path + " -> " + (msg != nullptr ? msg : "unknown");
        return false;
    }
    void* sym = dlsym(handle, "OrtGetApiBase");
    if (sym == nullptr) {
        err = path + " -> 缺少 OrtGetApiBase 符号";
        dlclose(handle);
        return false;
    }
    srq_set_ort_api_base(sym);
    fprintf(stderr, "[sr_qnn_proc] ort loaded: %s\n", path.c_str());
    return true;
}

/* 不能把 libonnxruntime.so 放进 DT_NEEDED：可执行文件在 App 的 lib 目录里，
 * exec 出来的进程里该目录不在动态链接器的搜索路径内，进程会在 main() 之前
 * 就被 "CANNOT LINK EXECUTABLE ... not found" 杀掉(真机实测)。这里改为运行时
 * 按绝对路径 dlopen，失败时把 dlerror 原文打到日志，父进程能直接看到原因。 */
bool LoadOrt(const std::string& dir) {
    std::vector<std::string> candidates;
    if (!dir.empty()) {
        candidates.push_back(dir + "/libonnxruntime.so");
    }
    candidates.push_back("libonnxruntime.so");
    std::string firstErr;
    for (const auto& path : candidates) {
        std::string err;
        if (LoadOrtFrom(path, err)) {
            return true;
        }
        if (firstErr.empty()) {
            firstErr = err;
        }
    }
    // 专项补救：报 "cannot locate symbol" 说明某个符号由别的库提供(真机上缺的是
    // libfmt v11 的 vformat，APK 里只有 libavformat.so 导出它)。把这些库以
    // RTLD_GLOBAL 预加载后再试一次，符号即可被解析。
    if (firstErr.find("cannot locate symbol") != std::string::npos && !dir.empty()) {
        const char* helpers[] = {"libc++_shared.so", "libavformat.so", "libavcodec.so", "libavutil.so"};
        for (const char* name : helpers) {
            std::string path = dir + "/" + name;
            void* h = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
            fprintf(stderr, "[sr_qnn_proc] preload %s: %s\n", name,
                    h != nullptr ? "ok" : (dlerror() != nullptr ? "failed" : "failed"));
        }
        std::string err;
        if (LoadOrtFrom(candidates.front(), err)) {
            return true;
        }
    }
    fprintf(stderr, "[sr_qnn_proc] ort load FAILED: %s\n", firstErr.c_str());
    return false;
}

/* 动态状态查询(cmd=4)：hello 的快照是"建会话之前"的，后端/HTP 状态会随之变化，
 * 自检需要在跑完推理后再问一次，否则面板永远显示 "QNN 未生效"。
 * 帧格式与 hello 完全一致，只是 modelCount 恒为 0。 */
bool HandleInfo() {
    const int32_t status = srq_init();
    const uint32_t cmd = 4;
    if (!Put(cmd) || !Put(status)) {
        return false;
    }
    if (!PutStr(TrimStr(srq_get_version()), 64)) {
        return false;
    }
    if (!PutStr(TrimStr(srq_get_backend_info()), 160)) {
        return false;
    }
    const int32_t htp = srq_has_htp();
    if (!Put(htp)) {
        return false;
    }
    if (!PutStr(TrimStr(srq_get_htp_error()), 256)) {
        return false;
    }
    const int32_t cores = srq_get_cpu_core_num();
    if (!Put(cores)) {
        return false;
    }
    const int32_t zero = 0;
    if (!Put(zero)) {
        return false;
    }
    const std::string err = TrimStr(srq_get_last_error());
    return PutBlob(err.data(), err.size());
}

/* 预加载 FastRPC 客户端库(libcdsprpc.so)
 *
 * QNN HTP 后端通过 FastRPC 与 DSP 通信，而它内部是 `dlopen("libcdsprpc.so")`（按 soname）。
 * 可执行文件所在目录与 /vendor 是否在搜索路径里，各机型表现不一。这里主动按**绝对路径**
 * dlopen 一次并 RTLD_GLOBAL，之后 QNN 的 soname 查找会命中这个已加载对象 ——
 * 相当于把"厂商库找不到"这件事绕开了，同时不需要把库打进 APK。
 */
// 从 fd 加载 .so：ANDROID_DLEXT_USE_LIBRARY_FD 让 linker 直接读 fd，
// 不再按路径比对 namespace 的 permitted_paths。/vendor/lib64 下这些库的
// SELinux 标签是 same_process_hal_file（允许 app 域 execute），
// 所以只要绕过 namespace 的**路径**白名单，就不再需要把厂商库复制进 APK。
void* DlopenByFd(const char* path, const char* soname) {
#if defined(__ANDROID__)
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[sr_qnn_proc] fd open failed: %s", path);
        perror(" ");
        return nullptr;
    }
    android_dlextinfo ext;
    memset(&ext, 0, sizeof(ext));
    ext.flags = ANDROID_DLEXT_USE_LIBRARY_FD;
    ext.library_fd = fd;
    void* h = android_dlopen_ext(soname, RTLD_NOW | RTLD_GLOBAL, &ext);
    if (h == nullptr) {
        fprintf(stderr, "[sr_qnn_proc] fd dlopen failed: %s", path);
        const char* e = dlerror();
        fprintf(stderr, " -> %s", e != nullptr ? e : "unknown");
        fputc(10, stderr);
    }
    close(fd);
    return h;
#else
    (void)path;
    (void)soname;
    return nullptr;
#endif
}

void PreloadFastRpc(const std::string& dir) {
    // 先让 libcdsprpc 的两个 vendor 依赖就位：它们是 libcdsprpc 的 NEEDED，
    // 用 fd 方式加载（RTLD_GLOBAL）后 soname 会注册进全局，主库解析依赖时
    // 直接复用，不会再被 namespace 的路径白名单拦住。
    struct Dep {
        const char* path;
        const char* soname;
    };
    const Dep deps[] = {
        {"/vendor/lib64/libvmmem.so", "libvmmem.so"},
        {"/odm/lib64/libvmmem.so", "libvmmem.so"},
        {"/vendor/lib64/vendor.qti.hardware.dsp-V1-ndk.so",
         "vendor.qti.hardware.dsp-V1-ndk.so"},
        {"/odm/lib64/vendor.qti.hardware.dsp-V1-ndk.so",
         "vendor.qti.hardware.dsp-V1-ndk.so"},
    };
    for (const Dep& d : deps) {
        if (dlopen(d.soname, RTLD_NOW | RTLD_GLOBAL) != nullptr) {
            continue;   // 已在（打过包或已被其它库带进来）
        }
        if (DlopenByFd(d.path, d.soname) != nullptr) {
            fprintf(stderr, "[sr_qnn_proc] dep preloaded(fd): %s", d.path);
            fputc(10, stderr);
        }
    }

    std::vector<std::string> paths;
    if (!dir.empty()) {
        paths.push_back(dir + "/libcdsprpc.so");     // 若曾打进 APK，优先用同目录那份
    }
    const char* systemPaths[] = {"/vendor/lib64/libcdsprpc.so",
                                 "/odm/lib64/libcdsprpc.so",
                                 "/system/lib64/libcdsprpc.so",
                                 "libcdsprpc.so"};
    for (const char* p : systemPaths) {
        paths.emplace_back(p);
    }
    std::string firstErr;
    for (const auto& p : paths) {
        // 1) 普通 dlopen：同目录 / 已注册 soname / namespace 放行时最快
        void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_GLOBAL);
        if (h != nullptr) {
            fprintf(stderr, "[sr_qnn_proc] fastrpc preloaded: %s", p.c_str());
            fputc(10, stderr);
            return;
        }
        const char* msg = dlerror();
        if (firstErr.empty()) {
            firstErr = std::string(p) + " -> " + (msg != nullptr ? msg : "unknown");
        }
        // 2) fd 方式：绝对路径时绕过 namespace 的路径白名单
        if (!p.empty() && p[0] == '/') {
            const std::string base = p.substr(p.find_last_of('/') + 1);
            h = DlopenByFd(p.c_str(), base.c_str());
            if (h != nullptr) {
                fprintf(stderr, "[sr_qnn_proc] fastrpc preloaded(fd): %s", p.c_str());
                fputc(10, stderr);
                return;
            }
        }
    }
    fprintf(stderr, "[sr_qnn_proc] fastrpc preload failed: %s", firstErr.c_str());
    fputc(10, stderr);
}

}  // namespace

// NDK 的 unistd.h 不导出 bionic 的 closefrom()，按 /proc/self/fd 自己实现。
static void CloseFrom(int lowfd) {
    DIR* d = opendir("/proc/self/fd");
    if (d == nullptr) {
        struct rlimit rl{};
        if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
            const int maxFd = rl.rlim_cur > 4096 ? 4096 : static_cast<int>(rl.rlim_cur);
            for (int fd = lowfd; fd < maxFd; ++fd) {
                close(fd);
            }
        }
        return;
    }
    const int dfd = dirfd(d);
    std::vector<int> fds;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        const int fd = atoi(ent->d_name);
        if (fd >= lowfd && fd != dfd) {
            fds.push_back(fd);
        }
    }
    closedir(d);
    for (int fd : fds) {
        close(fd);
    }
}

int main() {
    // 第一件事：关掉所有继承的 fd（3 以上）。
    // 宿主若是 Qt 应用，父进程带着数百个 Vulkan/GL/dmabuf/eventfd 进来；Android 的
    // 这些 fd 多数没有 O_CLOEXEC，exec 后会一路留在子进程里。QNN 的 HTP 后端在
    // QnnDevice_create 阶段要自己开 /dev/adsprpc-smd 并做 mmap，残留 fd 会干扰它的
    // 设备探测（Python 宿主走 posix_spawn + pass_fds，天生只有 0/1/2 三个 fd，
    // 所以同一份二进制在那边一直是好的）。协议通道用的 0/1 不受影响。
    CloseFrom(3);
    // 第一行必须无条件打到 stderr：父进程靠它区分"exec/动态链接就失败"和"崩在引擎里"。
    fprintf(stderr, "[sr_qnn_proc] entry (build " __DATE__ " " __TIME__ ")\n");
    fflush(stderr);
    {
        // 残留 fd 计数：排障时和 Python 宿主（应为 0）对照
        int openFds = 0;
        DIR* fdDir = opendir("/proc/self/fd");
        if (fdDir != nullptr) {
            while (readdir(fdDir) != nullptr) {
                ++openFds;
            }
            closedir(fdDir);
        }
        fprintf(stderr, "[sr_qnn_proc] openFds(>2)=%d\n", openFds > 2 ? openFds - 2 : 0);
        const char* adsp = getenv("ADSP_LIBRARY_PATH");
        fprintf(stderr, "[sr_qnn_proc] ADSP_LIBRARY_PATH=%s\n", adsp != nullptr ? adsp : "(unset)");
        const char* dsp = getenv("DSP_LIBRARY_PATH");
        fprintf(stderr, "[sr_qnn_proc] DSP_LIBRARY_PATH=%s\n", dsp != nullptr ? dsp : "(unset)");
        fflush(stderr);
    }
    // 进程属性诊断：QNN 的 device_create 对 SELinux 域 / seccomp / cgroup 敏感。
    // 能跑通 NPU 的 Python 版和失败的 C++ 版都是 untrusted_app，差在哪只能逐项比。
    {
        char buf[1024];
        FILE *pf = fopen("/proc/self/attr/current", "r");
        if (pf != nullptr) {
            if (fgets(buf, sizeof(buf), pf) != nullptr) {
                buf[strcspn(buf, "\n")] = 0;
                fprintf(stderr, "[sr_qnn_proc] selinux=%s\n", buf);
            }
            fclose(pf);
        }
        pf = fopen("/proc/self/cgroup", "r");
        if (pf != nullptr) {
            int ci = 0;
            while (fgets(buf, sizeof(buf), pf) != nullptr && ci < 4) {
                buf[strcspn(buf, "\n")] = 0;
                fprintf(stderr, "[sr_qnn_proc] cgroup[%d]=%s\n", ci++, buf);
            }
            fclose(pf);
        }
        pf = fopen("/proc/self/status", "r");
        if (pf != nullptr) {
            while (fgets(buf, sizeof(buf), pf) != nullptr) {
                if (strncmp(buf, "Seccomp", 7) == 0 || strncmp(buf, "CapEff", 6) == 0
                    || strncmp(buf, "NoNewPrivs", 10) == 0
                    || strncmp(buf, "Cpus_allowed_list", 17) == 0) {
                    buf[strcspn(buf, "\n")] = 0;
                    fprintf(stderr, "[sr_qnn_proc] %s\n", buf);
                }
            }
            fclose(pf);
        }
        fflush(stderr);
    }
    // 抢走协议 fd，再把 fd1 指向 stderr：引擎里的 printf 不能污染协议流。
    int protoFd = dup(1);
    if (protoFd >= 0) {
        g_outFd = protoFd;
        if (dup2(2, 1) < 0) {
            g_outFd = 1;   // 重定向失败也要保证协议能用
        }
    }
    fprintf(stderr, "[sr_qnn_proc] start pid=%d protoFd=%d\n", getpid(), g_outFd);
    fflush(stderr);

    const std::string selfDir = SelfDir();
    fprintf(stderr, "[sr_qnn_proc] self dir=%s\n", selfDir.empty() ? "(unknown)" : selfDir.c_str());
    if (!LoadOrt(selfDir)) {
        // 让父进程拿到可读的原因，而不是只看到 "proc exited"
        const uint32_t cmd = 1;
        const int32_t status = -100;
        (void) Put(cmd);
        (void) Put(status);
        (void) PutStr("load ort failed", 64);
        (void) PutStr("", 160);
        const int32_t zero = 0;
        (void) Put(zero);
        (void) PutStr("", 256);
        (void) Put(zero);
        (void) Put(zero);
        const std::string err = "无法加载 libonnxruntime.so(见 sr_native.log 的 ort load FAILED 行)";
        (void) PutBlob(err.data(), err.size());
        fflush(stderr);
        return 4;
    }
    // ORT 就绪后立刻尝试把 FastRPC 拉进来，QNN 初始化时就能直接用上
    PreloadFastRpc(selfDir);

    while (true) {
        uint32_t cmd = 0;
        if (!Get(cmd)) {
            fprintf(stderr, "[sr_qnn_proc] stdin closed, exit\n");
            break;
        }
        bool ok = true;
        if (cmd == 1) {
            ok = HandleHello();
        } else if (cmd == 2) {
            ok = HandleAdd();
        } else if (cmd == 4) {
            ok = HandleInfo();
        } else {
            srq_stop();
            const uint32_t bye = 3;
            (void) Put(bye);
            break;
        }
        if (!ok) {
            fprintf(stderr, "[sr_qnn_proc] protocol error at cmd=%u, exit\n", cmd);
            break;
        }
    }
    fflush(stdout);
    return 0;
}
