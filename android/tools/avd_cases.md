# AVD / 真机验收用例（`avd_verify.sh` 的配套清单）

本文件回答两个问题：**每批次要验哪几条**、**哪些 logcat 噪音是无害的**。
执行入口只有一条命令（详见 `bash android/tools/avd_verify.sh --help`）：

```bash
bash android/tools/avd_verify.sh --apk android/dist/JMComic-<ver>-<abi>-debug.apk --abi <abi>
# 16KB 专项（设备不是 16KB 时把 N/A 变 FAIL）：
bash android/tools/avd_verify.sh --apk <apk> --abi <abi> --require-16k
```

退出码：`0` 全 PASS（16KB 一栏可能是 N/A）/ `1` 有 FAIL / `2` 环境问题（无 adb / 无设备）。

## 1. 已实测的设备与本机结论（2026-11-01）

| 项 | 实测值 | 结论 |
| --- | --- | --- |
| AVD 镜像 | `google/sdk_gphone16k_x86_64/emu64xa16k:17/CP41.260831.015/...`（userdebug） | **Android 17 / SDK 37 的 16KB 镜像**（`ro.build.version.sdk=37`，`getconf PAGE_SIZE=16384`） |
| 设备页大小 | `16384` | 可用于 16KB 验收（不需要真机） |
| 装 targetSdk=34 的包 | `adb install -r` → `Success`；`dumpsys package` 显示 `minSdk=34 targetSdk=34 versionName=0.87 primaryCpuAbi=x86_64` | **能装**。Android 17 只拦"targetSdk 过低（<24）"的包，34 不受限 —— 之前文档里挂的"待确认"可以关掉 |
| 0.87 x86_64 包启动 | `dlopen failed: ".../lib/x86_64/libc++_shared.so" program alignment (4096) cannot be smaller than system page size (16384)` → `QtLoader: UnsatisfiedLinkError` → 进程未存活 | **一票否决**：装得上 ≠ 跑得起来。16KB 设备上必须靠本脚本判定 |

> 结论写法约定：能安装、能启动、能跑业务是**三件事**，必须分开验；历史事故就是把
> "装上了/校验通过"当成了"能用"。

## 2. 每批次必须验的 6 条（`avd_verify.sh` 已自动化）

| 项目 | 判据 | 说明 |
| --- | --- | --- |
| 安装 | `adb install -r` 返回 `Success` | 覆盖安装，保留数据；失败时看 `Failure [INSTALL_...]` |
| 首帧 | `am start -W` 的 `TotalTime`（缺失时用 `WaitTime`）≤ `--max-frame-ms`（默认 300） | **这是 adb 视角的启动耗时，不等于渲染首帧**，只做同机不回退判定 |
| 进程存活 | `pidof <pkg>` 非空（启动后等 `--launch-wait` 秒） | 启动即崩最常见就是 16KB/dlopen |
| 16KB 库加载 | logcat 中 `dlopen failed: .*program alignment` 与 `UnsatisfiedLinkError` **各 0 条** | 0.87 就是靠这两条钉死的；单独成行、一票否决 |
| 16KB 页兼容 | 设备 `PAGE_SIZE=16384` 且应用**未被**标记兼容模式 | 设备不是 16KB → `N/A`（加 `--require-16k` 则 `FAIL`） |
| PSS | `dumpsys meminfo` 的 `TOTAL PSS` | **只记录不设阈值**（趋势用 bench/回归看） |
| 截图 | `adb exec-out screencap -p` 非空且 >1KB | 人眼复核用，存 `android/build_logs/avd-<时间戳>-<abi>.png` |

补充（暂不自动化，人工过一遍）：超分开关跑一张图（QNN/HTP 与 CPU 各一次）、
webp/gif/长图解码、连续翻 30 页、下载+解密入库、字体渲染。

## 3. 已知无害的 logcat 噪音（不要当成失败）

| 关键字 | 为什么无害 |
| --- | --- |
| `Fatal signal 6 (SIGABRT) ... in tid ... (android.hardwar)` | 模拟器的 **硬件抽象层**服务在 16KB 镜像上自己崩，与应用无关；判据是**进程名不是我们的包** |
| `This plugin does not support propagateSizeHints()` | Qt offscreen/模拟器窗口插件的正常提示 |
| `exit code 134`（`SIGABRT`）且**应用进程仍存活** | 已知 PySide6 6.12.0 + Py3.11 的 finalize GC 缺陷，跑完才死；`avd_verify.sh` 记 `INFO` 不判 FAIL，但进程已死时会被 pidof 判 FAIL |
| `nativeloader: ... using class loader ns` | 正常加载日志（只有带 `dlopen failed:` 才是失败） |
| `gralloc` / `EGL` / `OpenGLRenderer` 的 W 级提示 | 模拟器软件渲染常见，不影响功能判定 |

## 4. 判定顺序建议（节省时间）

1. 先跑 `check_elf_page_align.py <apk> --abi <abi>`：静态页对齐不过，就别浪费时间装包；
2. 再跑 `avd_verify.sh`：装 → 起 → 活 → 日志 → PSS → 截图；
3. 两项都 PASS 才做人工业务回归（第 2 节末尾那几项）。
