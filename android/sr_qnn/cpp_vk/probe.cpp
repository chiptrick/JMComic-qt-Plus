// sr_vk_probe：在 Android 设备上真跑一次 libsr_vk.so，证明 Vulkan 后端可用
//
// 为什么不只看 `cmd gpu vkjson`：
//   系统报"有 Vulkan 设备"不等于我们的库能起来（驱动/加载器/so 缺符号都可能失败）。
//   这个探针用 dlopen + 函数指针调用真实 ABI，输出 init 结果、设备名、模型表数量。
//
// 用法（推送到 /data/local/tmp 后）：
//   adb push libsr_vk.so sr_vk_probe /data/local/tmp/
//   adb shell "cd /data/local/tmp && chmod 755 sr_vk_probe && LD_LIBRARY_PATH=. ./sr_vk_probe ./libsr_vk.so"
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

typedef int (*InitFn)(void);
typedef int (*SetDirFn)(const char*);
typedef int (*CountFn)(void);
typedef const char* (*StrFn)(int);
typedef const char* (*NoArgStrFn)(void);
typedef int (*IntFn)(void);

int main(int argc, char** argv) {
    const char* so = (argc > 1) ? argv[1] : "./libsr_vk.so";
    printf("== probe: %s ==\n", so);

    void* h = dlopen(so, RTLD_NOW);
    if (h == nullptr) {
        printf("[FAIL] dlopen: %s\n", dlerror());
        return 1;
    }
    auto init = reinterpret_cast<InitFn>(dlsym(h, "srq_init"));
    auto version = reinterpret_cast<NoArgStrFn>(dlsym(h, "srq_get_version"));
    auto backend = reinterpret_cast<NoArgStrFn>(dlsym(h, "srq_get_backend_info"));
    auto setDir = reinterpret_cast<SetDirFn>(dlsym(h, "srq_set_model_dir"));
    auto count = reinterpret_cast<CountFn>(dlsym(h, "srq_model_count"));
    auto nameAt = reinterpret_cast<StrFn>(dlsym(h, "srq_model_name"));
    if (init == nullptr || version == nullptr || backend == nullptr) {
        printf("[FAIL] 缺关键符号（不是 libsr_vk？）\n");
        return 1;
    }
    printf("version: %s\n", version());

    int rc = init();
    printf("srq_init -> %d %s\n", rc, rc == 0 ? "(Vulkan 可用)" : "(失败：无 Vulkan 设备或初始化错误)");
    printf("backend : %s\n", backend());
    if (rc != 0) {
        return 2;
    }

    if (argc > 2 && setDir != nullptr) {
        setDir(argv[2]);
        int n = (count != nullptr) ? count() : -1;
        printf("model dir: %s  models=%d\n", argv[2], n);
        for (int i = 0; i < n && nameAt != nullptr; ++i) {
            printf("  [%d] %s\n", i, nameAt(i));
        }
    }
    printf("[OK] Vulkan 后端可用\n");
    return 0;
}
