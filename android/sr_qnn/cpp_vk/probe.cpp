// sr_vk_probe：在设备上验证 libsr_vk.so（init / 模型表 / 单图推理）
//
// 用法：
//   ./sr_vk_probe <so>                                  # 只测 init
//   ./sr_vk_probe <so> <modelDir>                       # + 列模型
//   ./sr_vk_probe <so> <modelDir> <model> <in.raw> <out.raw>   # + 真推理（raw=RGB24，192x192）
//   ./sr_vk_probe <so> <modelDir> <model> <in.raw> <out.raw> <w> <h>
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

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

typedef int (*InitFn)(void);
typedef int (*InitSetFn)(int, int);
typedef int (*SetDirFn)(const char*);
typedef int (*CountFn)(void);
typedef const char* (*StrFn)(int);
typedef const char* (*NoArgStrFn)(void);
typedef int (*ModelIdFn)(const char*);
typedef int (*AddFn)(const unsigned char*, int, int, int, int, int, int, double, int, int,
                     const char*, int);
typedef int (*LoadFn)(int, SrqResult*);
typedef void (*FreeFn)(SrqResult*);

static void* Sym(void* h, const char* name) {
    void* p = dlsym(h, name);
    if (p == nullptr) printf("[warn] 缺符号 %s\n", name);
    return p;
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // 崩溃时也要看到已打印的进度
    setvbuf(stderr, nullptr, _IONBF, 0);
    const char* so = (argc > 1) ? argv[1] : "./libsr_vk.so";
    printf("== probe: %s ==\n", so);

    void* h = dlopen(so, RTLD_NOW);
    if (h == nullptr) {
        printf("[FAIL] dlopen: %s\n", dlerror());
        return 1;
    }
    auto init = (InitFn)Sym(h, "srq_init");
    auto initSet = (InitSetFn)Sym(h, "srq_init_set");
    auto version = (NoArgStrFn)Sym(h, "srq_get_version");
    auto backend = (NoArgStrFn)Sym(h, "srq_get_backend_info");
    auto lastErr = (NoArgStrFn)Sym(h, "srq_get_last_error");
    auto setDir = (SetDirFn)Sym(h, "srq_set_model_dir");
    auto count = (CountFn)Sym(h, "srq_model_count");
    auto nameAt = (StrFn)Sym(h, "srq_model_name");
    auto infoAt = (StrFn)Sym(h, "srq_model_info");
    auto modelId = (ModelIdFn)Sym(h, "srq_model_id");
    auto add = (AddFn)Sym(h, "srq_add");
    auto load = (LoadFn)Sym(h, "srq_load");
    auto freeRes = (FreeFn)Sym(h, "srq_free_result");

    printf("version: %s\n", version ? version() : "?");
    if (init == nullptr) return 1;
    int rc = init();
    printf("srq_init -> %d %s\n", rc, rc == 0 ? "(Vulkan 可用)" : "(失败)");
    printf("backend : %s\n", backend ? backend() : "?");
    if (rc != 0) {
        printf("err     : %s\n", lastErr ? lastErr() : "?");
        return 2;
    }
    if (argc > 2 && setDir != nullptr) {
        setDir(argv[2]);
        int n = count ? count() : -1;
        printf("models  : %d (dir=%s)\n", n, argv[2]);
        for (int i = 0; i < n && nameAt; ++i) printf("  [%d] %-16s %s\n", i, nameAt(i), infoAt ? infoAt(i) : "");
    }

    if (argc > 5 && add && load && modelId && freeRes) {
        const char* model = argv[3];
        const char* inPath = argv[4];
        const char* outPath = argv[5];
        int w = (argc > 6) ? atoi(argv[6]) : 192;
        int h = (argc > 7) ? atoi(argv[7]) : 192;
        if (initSet) initSet(0, 4);

        int id = modelId(model);
        printf("model id: %d (%s)\n", id, model);
        if (id < 0) return 3;

        size_t need = (size_t)w * h * 3;
        unsigned char* img = (unsigned char*)malloc(need);
        FILE* fp = fopen(inPath, "rb");
        if (fp == nullptr) { printf("[FAIL] 打不开输入 %s\n", inPath); return 4; }
        size_t got = fread(img, 1, need, fp);
        fclose(fp);
        if (got != need) { printf("[FAIL] 输入应为 %zu 字节，实际 %zu\n", need, got); return 4; }

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        printf("calling srq_add ...\n");
        int tid = add(img, w, h, w * 3, 3, id, 1, 0.0, 0, 0, nullptr, 0);
        printf("srq_add returned %d; calling srq_load ...\n", tid);
        SrqResult res;
        memset(&res, 0, sizeof(res));
        int ok = load(0, &res);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double wall = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        printf("add -> %d, load -> %d, status=%d, %dx%d stride=%d tick=%.3fs wall=%.3fs\n",
               tid, ok, res.status, res.width, res.height, res.stride, res.tick, wall);
        if (res.status != 0 || res.data == nullptr) {
            printf("[FAIL] 推理失败: %s\n", lastErr ? lastErr() : "?");
            free(img);
            return 5;
        }
        FILE* fo = fopen(outPath, "wb");
        if (fo == nullptr) { printf("[FAIL] 写不了输出 %s\n", outPath); return 6; }
        fwrite(res.data, 1, (size_t)res.stride * res.height, fo);
        fclose(fo);
        printf("[OK] 已写出 %s (%d bytes)\n", outPath, res.stride * res.height);
        freeRes(&res);
        free(img);
        return 0;
    }

    printf("[OK] init 通过\n");
    return 0;
}
