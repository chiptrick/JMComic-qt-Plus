# Android QNN(NPU) 接入与排障记录

> 结论版本：cpp-ui 0.96.4（2026-11-15）  
> 设备：vivo V2463A / 骁龙 8 Elite（SM8750，soc_id=618）/ Adreno 830 / HTP V79

## 1. 现象

QNN HTP 在 `adb shell` 里能跑（`/data/local/tmp` 下同一份 `sr_qnn_proc` + 同一份 ORT + 同一套 QNN 2.42 库），
但在 App 子进程里恒定失败：

```
[ORT E] onnxruntime: QNN SetupBackend failed
        Failed to create device. Error: QNN_DEVICE_ERROR_INVALID_CONFIG: Invalid config values
```

四个 tier（3→0）全部失败，且**没有任何编译耗时**，说明 QNN 连 DSP 都没碰到。

## 2. 根因：app 的 linker namespace 不含 `/vendor/lib64`

失败时 linker 打出的 warning（关键证据）：

```
WARNING: linker: library "/vendor/lib64/libcdsprpc.so" ... is not accessible for the namespace:
  [name="(default)",
   ld_library_paths="<nativeLibraryDir>[:<filesDir>/qnn_libs]",
   default_library_paths="/system/lib64:/system_ext/lib64",
   permitted_paths="...:/vendor/framework:/vendor/app:/vendor/priv-app:...:/data:..."]
```

- `default_library_paths` 只有 `/system/lib64:/system_ext/lib64`
- `permitted_paths` 里有 `/vendor/framework`、`/vendor/app`、`/data`，**唯独没有 `/vendor/lib64`**
- `libQnnHtpV79Stub.so` 的 `NEEDED` 里有 **`libcdsprpc.so`**（FastRPC 客户端，跟 DSP 通信的唯一通道）
- 于是 `QnnDevice_create` 拿不到 FastRPC → 报 `QNN_DEVICE_ERROR_INVALID_CONFIG`

补充：`/vendor/etc/public.libraries.txt` **确实列了** `libcdsprpc.so`，但那份白名单只对 Java 层
`System.loadLibrary`（libnativeloader）生效；**C++ 里 `dlopen("libcdsprpc.so")` 裸名不走它**，
最终仍落到 namespace 的 search paths，而那里没有 `/vendor/lib64`。

### 为什么 shell 能跑

`shell` 域的 default namespace 的 `default_library_paths` 含 `/vendor/lib64`，绝对路径可直接 dlopen。

## 3. 修复：运行时把厂商库复制到 app 私有目录（不打包进 APK）

把库打进 `nativeLibraryDir` 可行（实测 31.9~35.8×），但那是 Qualcomm 私有库，分发有授权风险。
纯系统方案的关键是：**app namespace 的 `permitted_paths` 里有 `/data`**，而
`filesDir`（`/data/user/0/<pkg>/files`）就在它下面。

做法（`cpp/shell/sr_engine.cpp` 的 `QnnPrepareFastRpcLib` + `QnnProcStart`）：

1. 首次启动把 `/vendor/lib64/` 下的 3 个文件复制到 `filesDir/qnn_libs/`：
   - `libcdsprpc.so`（497KB，FastRPC 客户端）
   - `libvmmem.so`（69KB，libcdsprpc 的 NEEDED）
   - `vendor.qti.hardware.dsp-V1-ndk.so`（86KB，同上）
   三个缺一不可——少任何一个都会在依赖解析阶段失败
2. **在 exec 之前**把该目录写进 `LD_LIBRARY_PATH`（`posix_spawn` 的 envp）：
   `nativeLibraryDir:filesDir/qnn_libs`。namespace 的 `ld_library_paths` 在进程启动时
   从环境读取，**运行期 `setenv` 不会更新**
3. proc 里 `dlopen("libcdsprpc.so")`（裸名）即命中该目录：
   - 路径以 `/data` 开头 → 命中 `permitted_paths` → namespace 放行
   - SELinux 标签 `app_data_file` 对 `untrusted_app` 有 `execute` 权限 → 放行
4. 两个依赖与主库同目录，NEEDED 解析在同一 `ld_library_paths` 内完成

### 为什么不能直接 dlopen `/vendor/lib64`

| 方式 | 结果 |
|---|---|
| `dlopen("/vendor/lib64/libcdsprpc.so")` | `is not accessible for the namespace`（该目录不在 `permitted_paths`） |
| `dlopen("libcdsprpc.so")` 裸名 | 同样失败：search paths 里没有 `/vendor/lib64`；`public.libraries.txt` 只对 Java 层 `System.loadLibrary` 生效 |
| `android_dlopen_ext` + `ANDROID_DLEXT_USE_LIBRARY_FD` | 仍报同一错误——bionic 在 fd 模式下依然执行 `is_accessible(soname)` |
| `linker64 <proc>` 启动 | namespace 不变；且 `dladdr` 会返回 `/apex/...`，反而破坏同目录查找 |
| `android_create_namespace` | NDK 不导出，设备 `libdl.so` 里也没有该符号 |

### 实测（waifu2x_cunet_up2x_denoise3，tile 192）

| 轮次 | NPU | CPU | 加速比 |
|---|---:|---:|---:|
| 1 | 25 ms | 825 ms | 33.0× |
| 2 | 25 ms | 830 ms | 33.2× |

与"打包进 jniLibs"的方案（23~27ms）性能一致，且 **APK 内零厂商库**。

## 4. 复盘：此前 8 种尝试为什么都失败

它们全部作用在 **FastRPC 加载成功之后**的环节：

| 尝试 | 为什么无效 |
|---|---|
| `DSP_LIBRARY_PATH` / `ADSP_LIBRARY_PATH` 各种组合 | 那是 DSP 侧找 skel 的路径；FastRPC 客户端还没加载，走不到这步 |
| 把 skel 复制到 `filesDir` 并让 ADSP 指向它 | 同上；且 `app_data_file` 也不允许 execute |
| 让厂商签名 skel 优先 | 同上 |
| `chdir` 到可写目录 | QNN 在线编译确实需要可写 cwd，但失败发生在 device_create，比编译更早 |
| `clearenv` 清空继承环境 | 环境不是原因 |
| QNN SDK 2.42 → 2.51 | 库版本不是原因（两版 md5 一致时行为也一致） |
| `fork` → `posix_spawn` | 启动语义不是原因（exec 后等价） |
| `CloseFrom(3)` 关闭继承 fd | fd 数量已降到 4（与 shell 相同），不是原因 |

### 三个诊断盲点（这次才补上）

1. `PreloadFastRpc` 只打印**第一个**失败路径（`<nativeLibraryDir>/libcdsprpc.so -> not found`），
   看起来像“库不存在”，掩盖了后面 `/vendor/lib64` 的 namespace 错误
2. linker warning 在日志**中部**，被 QNN 的大量输出淹没；`QnnProcLogTail` 只取尾部 1500 字符时看不到
3. `htp=1` 来自 `HasQnnLib()`（只代表 `libQnnHtp.so` 存在），**不代表 FastRPC 可用**，此前被误读成“后端就绪”

## 5. local-dream（xororz）的对照推断

已核实的部署事实：

- `nativeLibraryDir` 只有 3 个 `.so`（`libandroidx.graphics.path.so`、`libdatastore_shared_counter.so`、
  **`libstable_diffusion_core.so` 11.6MB**），**不含任何 QNN 库**
- QNN 运行时在 APK 的 `assets/qnnlibs/`：`libQnnHtp.so`、`libQnnSystem.so`、V68/V69/V73/V75/V79/V81
  的 `Stub` 与 `Skel`，外加 12MB 级的 `libQnnHtpV79.so`（DSP 侧签名 skel）
- 源码侧：`CMakeLists` 用 `add_executable(libstable_diffusion_core.so)`（名为 .so、实为可执行文件），
  链接 QNN SDK 自带的 SampleApp（含 PAL 层）；Kotlin 用 `ProcessBuilder` 拉起，经 127.0.0.1 HTTP 通信
- **它不分发 `libcdsprpc.so`**（APK 内 grep 无匹配）

由此推断它的运行原理：

1. 首次启动把 `assets/qnnlibs` 解压到 `filesDir/runtime_libs`（DSP 侧需要能按路径找到 skel）
2. 用 `ProcessBuilder` 启动可执行文件，`DSP_LIBRARY_PATH` 指向该目录，cwd 设为可写目录
3. 走 **QNN SDK 原生 API**（`QnnBackend_create` → `QnnDevice_create` → `QnnContext_create`），
   **完全不经过 ORT**，因此不受 ORT 的 provider options / context 缓存影响
4. **它的 FastRPC 依赖同样来自系统 `/vendor/lib64/libcdsprpc.so`** —— 这一点与我们的限制**是同一个问题**

### 可验证结论

- **已证实**：Qt App 的 namespace 不含 `/vendor/lib64`；补齐 3 个 vendor 库后 NPU 立刻可用（23ms / 35.8×）
- **待验证（可一条命令定性）**：local-dream 若真能在 app 域跑 NPU，它的进程 maps 里必然出现
  `/vendor/lib64/libcdsprpc.so`。验证方法：
  ```bash
  # 起一次它的生成任务后
  adb shell ps -AZ | grep localdream              # 找到子进程 pid
  adb shell "run-as io.github.xororz.localdream cat /proc/<pid>/maps | grep cdsprpc"
  ```
  - 有 → 它的 namespace/启动方式与我们的不同（例如 PAL 用 `android_dlopen_ext` 带 fd，
    或它在 targetSdk=36 + minSdk=28 的组合下命中了另一套 `ld.config` 规则），可据此再优化
  - 没有 → 它同样加载不到 FastRPC，NPU 实际并未启用（或只在部分机型可用）

## 6. 硬约束（后续改动必须遵守）

1. **不要**把 `libcdsprpc.so` / `libvmmem.so` / `vendor.qti.hardware.dsp-V1-ndk.so` 打进 APK
   （授权风险）。由 `QnnPrepareFastRpcLib` 首次启动从 `/vendor/lib64` 复制到
   `filesDir/qnn_libs`，**三个缺一不可**
2. `LD_LIBRARY_PATH` **必须在 exec 前**经 envp 传入（`nativeLibraryDir:filesDir/qnn_libs`）；
   运行期 `setenv` 不更新 namespace 的 `ld_library_paths`
3. 不要把 `/vendor/lib64` 整体加进 `LD_LIBRARY_PATH`：ORT 的 `libc++_shared` 会解析到
   厂商版本，子进程连 ORT 都加载不了
4. `build.gradle` 的 `packaging.jniLibs.useLegacyPackaging=true` 不能去掉
   （`libsr_qnn_proc.so` 要被 exec，不是 dlopen）
5. x86_64 不接 QNN
6. 复制出来的库是 `app_data_file`，其 execute 依赖 `untrusted_app` 的 sepolicy；
   换 ROM 后若被拒会退到 CPU，proc 日志里会出现 `fastrpc preload failed`

## 7. 复测入口

```bash
# 真机：性能（含 NPU 档）
adb shell am start -n org.jmcomic.ui/org.qtproject.qt.android.bindings.QtActivity \
  --es jm.args "'--sr-perf-test --sr-model waifu2x_cunet_up2x_denoise3.onnx --qnn-debug 1'"
adb pull /storage/emulated/0/Android/data/org.jmcomic.ui/files/sr-perf.txt
adb pull /storage/emulated/0/Android/data/org.jmcomic.ui/files/qnn_proc.log

# 关键日志行
#   [sr_qnn_proc] fastrpc preloaded: libcdsprpc.so
#   [ORT I] onnxruntime: Create device.
#   [sr_qnn] try tier=3 ... （opts 明细）
```

## 8. 量化模型（w8a16 QDQ）接入（0.96.6）

### 8.1 现象与两个根因

现象：目录里的 `*_q16*.onnx` / `*lowq*.onnx` 在真机上恒走 `CPU（ONNX Runtime）`，
状态行写"QDQ 量化模型不支持 QNN HTP / Vulkan，使用 CPU"。

1. **代码层直接排除**：`EnsureModel()` 的 QNN 分支带 `&& !quantModel`，量化模型
   连子进程都不进。这条判据来自 0.96.3 之前的实测（`bench_full_report.md` 记录
   "QNN 建会话回退（cpu）"），但那时 HTP 整体起不来（FastRPC/namespace 问题），
   并不是 QDQ 格式的问题。ORT QNN EP 的**推荐**配置恰恰是
   `activation_type=QUInt16 + weight_type=QUInt8`（w8a16，HTP 上精度/速度折中）。
2. **I/O 量化策略与严格档冲突**：QDQ 图的图首/图尾就是
   `QuantizeLinear` / `DequantizeLinear`。QNN EP 默认把这两个方向
   `offload_graph_io_quantization=1`（交给 CPU EP），而严格档同时下发
   `session.disable_cpu_ep_fallback=1`。一边把节点推给 CPU、一边禁止 CPU 兜底，
   建会话必然失败。

### 8.2 修改点

| 位置 | 改动 |
|---|---|
| `sr_engine.cpp` `EnsureModel` | 去掉 `!quantModel`；`pref=vulkan` 时量化模型也试 NPU（Vulkan 必然拒绝） |
| `sr_engine.cpp` `QnnWriteModelsTxt` | models.txt 增加第 6 列 arch，`quant` 表示 QDQ 模型 |
| `sr_qnn.cpp` `LoadModelOverride` | 解析第 6 列，`ModelEntry::quant` |
| `sr_qnn.cpp` `CreateSession` | `entry.quant` → `offload_graph_io_quantization=0`（整图归 QNN） |
| `sr_engine.cpp` `QnnPickModel` | quant 模型禁用"同倍数兜底"（否则 float 模型顶上来，界面写着量化、跑的是别的图） |
| `sr_engine.cpp` `Dyn()` | 倍数推断补 `_x2/_x3/_x4`（`srmd_x3_q16` 以前被当成 2 倍） |
| `sr_engine.cpp` `RunPerfProbe` / `BenchModel` | `--sr-model` 对三档同时生效；quant/srmd 的 tile 钳回 192 |

### 8.3 不能只信"会话建成"

QNN EP 在 `GetCapability` 里可能给出 **0 分区**（模型不被 HTP 支持），此时会话
仍然创建成功，但每个节点都过 CPU EP —— 比纯 CPU 还慢，且界面上看起来像 NPU。
`sr_qnn.cpp` 的 `ineffective()` 会丢弃这种会话（`Number of partitions supported
by QNN EP: 0`），父进程侧则必须再确认一次：

```cpp
if (ProbeGeometry()) {                 // 真跑过一块 tile
    const QString info = QnnProcInfo();  // cmd=4：htp=1 backend=...
    if (info.contains("htp=1")) return true;   // 真在 Hexagon 上
    /* 否则落 CPU，并把原因写进 backendNote */
}
```

`srq_get_backend_info()` 在 HTP 会话生效后返回
`Qualcomm Hexagon NPU (QNN HTP, fp16)`，未生效时返回 `CPU (ONNX Runtime)`。

### 8.4 实测（V2463A / SM8750）

```
sr: qnn proc up ver=sr_qnn 1.4.1 (ONNX Runtime QNN EP / HEXAGON HTP) htp=1 models=81
sr: ready model=量化 · realcugan_pro_up2x_q16_lowq tile=192 out=384x384 scale=2
sr-perf NPU（QNN HTP，tile 192） ok=1 ms=271 note=htp=1 backend=Qualcomm Hexagon NPU (QNN HTP, fp16)
sr-perf CPU（tile 192）          ok=1 ms=671
recommend 推荐：NPU（QNN HTP） · 约为 CPU（ONNX Runtime） 的 2.5×
```

### 8.5 数值验收（必须做）

QDQ 图在 HTP 上"跑通"不等于"算对"：量化参数没被解析或 tile 尺寸不匹配时，
耗时正常但画面是错的。真机回归用：

```bash
adb shell am start -n org.jmcomic.ui/org.qtproject.qt.android.bindings.QtActivity \
  --es jm.args "'--sr-npu-compare realcugan_pro_up2x_q16_lowq.onnx'"
# 报告落 /storage/emulated/0/Android/data/<pkg>/files/sr-compare.txt（adb 可直接 cat）
```

实测（192×192 平滑渐变 + 低频纹理，NPU vs CPU 同一块 tile）：

| 模型 | maxAbs | PSNR | NPU | CPU | 加速 |
|---|---|---|---|---|---|
| `realcugan_pro_up2x_q16_lowq`（w8a16） | 0.0486 | **49.40 dB** | 269ms | 632ms | 2.3× |
| `realcugan_pro_up2x`（fp32） | 0.0580 | 32.18 dB | 32ms | 506ms | 15.8× |

两个结论：

1. **量化模型的一致性反而比 float 模型好**（49.4 vs 32.2dB）。HTP 的原生格式就是
   w8a16，CPU 侧同样是 QDQ 定点路径；float 模型在 HTP 上走 fp16、CPU 侧是 fp32，
   差异来源更多（`enable_htp_fp16_precision`）。
2. **输入图案决定 PSNR**：用逐像素伪随机（高频噪声）测同一对模型只有 13.09 和
   20.55dB —— 8-bit I/O 往返把高频细节的误差放大成整数级差异。真实照片介于两者
   之间，别拿最坏情况的数字下结论。

已知限制：父子进程之间用 `rgb888` 传 tile（输入/输出各 8-bit 量化往返），这是
Python 版既有协议。对 fp32 模型它抹掉了输入的 24 位精度，是 NPU 画质的上限来源；
要再上一层需要改成 float32/uint16 直通（协议两端同改，未做）。

### 8.6 踩坑：`SetBackendPreference(pref, false)` 曾经什么都不做

数值对比第一次跑出 `maxAbs=0 psnr=99.00dB npu=270ms cpu=271ms` —— 两次输出
**逐位相同**、耗时也相同，说明"CPU 档"其实还是 NPU。

根因：`SetBackendPreference(pref, persist=false)` 只释放了会话，偏好本身没有落到
任何地方；`PrefBackend()` 仍然按 `JM_SR_BACKEND` → `ui.ini [sr] backend` → `auto`
的顺序读。`auto` 在 Android 上就是"QNN 优先"，于是 CPU 档又被 QNN 接管。

修复：偏好必须有进程内承载。现在 `PrefBackend()` 的优先级是
`g_forceCpuOnce`（一次性回退）→ **`g_prefOverride`（`persist=false` 的覆盖）** →
`JM_SR_BACKEND` → `ui.ini [sr] backend` → `auto`；`SetBackendPreference(true)`
写盘并清覆盖，`false` 只写覆盖。性能测试/数值对比的临时切换统一 `persist=false`，
不再为了测一次而改写用户的永久偏好。

（早期版本的 `CompareNpuCpu` 用 `qputenv("JM_SR_BACKEND", ...)` 绕过同一个问题，
那段逻辑保留着，两条路都通；新代码用 `g_prefOverride`，不用再动环境变量。）

判据：修复后 CPU 档的耗时必须与 NPU 档**明显不同**（同模型通常 2~3 倍差），
且 `maxAbs` 不是 0 —— 两条同时成立才说明真的换了后端。
