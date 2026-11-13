# JMComic-qt 技术演进方案（2026-11 起）

依据：`docs/qt_cpp_migration_evaluation.md`（2026-11-01，收益/成本/构建路径评估）
与 `docs/pyside6_to_cpp_performance_assessment.md`（2026-10-31，性能实测）。

本方案把评估结论落成可执行计划：**先拿确定收益（P0~P2），把"是否整体迁移"推迟到一个有数据支撑的决策门**；
同时把工具链（JDK 25 LTS / NDK r30）作为独立工作流处理，因为它同时影响现状与迁移两条线。

---

## 0. 摘要

| 决策 | 结论 |
|---|---|
| 是否立刻整体迁移 C++ | **否**。30~45 人月，收益的 60~70% 可在 2~3 人月内以 P0~P2 拿到 |
| 现在做什么 | 阶段 A（4 周，确定收益）+ 阶段 B（10 周，架构改造）+ 工程保障（并行） |
| 什么时候再谈 C++ | 阶段 C0 决策门（3 周技术验证，与 A/B 并行），三项验证全过才进入 C1 |
| JDK 25 LTS | **采用**，但必须成对锁 Gradle ≥ 9.1（当前 9.8.2）；对 APK 产物零收益，收益在"构建可维护性" |
| NDK r30 | **建议采用**，且这是**修一个已存在的合规缺陷**：0.87 两个 APK 的 16KB 门禁实测不通过（见 §7.4） |
| 立即可做的三件事 | ① 16KB 不合规修复（否则 Android 17 fatal 模式装完即闪退）② `images.rcc`（-22 MB / 启动 -196 ms）③ 构建元数据落盘（现在无法从产物反查用了哪个 NDK） |

---

## 1. 目标与约束

### 1.1 目标

| 编号 | 目标 | 量化口径 |
|---|---|---|
| G1 | 设备端交互不变卡 | 列表首屏 ≤150 ms、开章 ≤60 ms、翻页 p95 ≥110 fps |
| G2 | APK 可分发 | 基础包 ≤120 MB（模型改按需下载），16KB 门禁 0 未对齐 |
| G3 | 桌面冷启与内存 | 冷启 ≤600 ms、空壳 RSS ≤70 MB |
| G4 | 构建可持续 | 单 ABI 增量构建 ≤10 min；产物可反查工具链版本 |
| G5 | 不牺牲迭代速度 | 设备端热更新链路保留（阶段 A/B 期间不动打包结构） |

### 1.2 非目标（本方案不做）

- 不做全量 C++ 重写（留给阶段 C，且需通过决策门）。
- 不改 `minSdk 34` / `targetSdk 34`（改 targetSdk 会引入边到边、权限等行为变化，另立项目）。
- 不动 QNN 超分链路的模型与推理逻辑。
- 不引入 QML（现有 274 个 py 文件全是 Widgets，改 QML 等于重写 UI，且与迁移无关）。

### 1.3 硬约束

1. 代码 70,825 行（真实迁移面 51,260 行），**单人可维护**是当前最大优势，方案不得破坏。
2. 迭代节奏：2026-09-30 ~ 10-28 完成 11 轮真机反馈修复，靠 `hotpush.sh`（10 秒）支撑。
3. 模型 178 MB 占 APK 41%，任何"瘦身"必须先处理它，否则语言层面的优化看不见。
4. 构建只能在 Linux/WSL（`pyside6-android-deploy` 仅 Unix 宿主），本机 40~60 min/ABI。

### 1.4 基线（当前值，作为验收对照）

| 指标 | 当前 | 测量方法 |
|---|---|---|
| 桌面冷启到首帧 | 998 ms | `bench_startup.py` |
| 桌面空壳 RSS | 138 MB | 同上（`/usr/bin/time -v` 复核） |
| 设备 PSS | 669 MB | `dumpsys meminfo`（连续翻 20 页后） |
| 设备首帧 | 204~261 ms | `am start -W` TotalTime，3 次取中位 |
| 列表首屏 60 条 | 250~500 ms | 埋点 `AddBooksDeferred` |
| 开章 60 页建场 | 154 ms | `bench_reader.py` |
| 单页解码显示 | 39.6 ms | `bench_pipeline.py` |
| 翻页帧率 | 110~123 fps | FrameTick p50/p95 |
| APK（arm64 / x86_64） | 433 MB / 372 MB | `ls -l` |
| 16KB 门禁 | **FAIL**（15 / 14 个库未对齐） | `check_elf_page_align.py` |

---

## 2. 方案总览

```
                 ┌──────────── 阶段 0（1 周）────────────┐
                 │ 基线固化 · 门禁不可绕过 · build-info  │
                 └───────────────┬───────────────────────┘
        ┌────────────────────────┼────────────────────────┐
        ▼                        ▼                        ▼
┌──── 阶段 A（4 周）────┐  ┌──── 阶段 B（10 周）────┐  ┌── 阶段 C0（3 周）──┐
│ P0 快赢 + 模型分包    │  │ P1/P2 架构改造          │  │ C++ 技术验证（spike）│
│ · images.rcc         │  │ · 解码进程池            │  │ · curl-impersonate  │
│ · 桌面解码域内        │  │ · 列表 delegate         │  │ · Windows 出包       │
│ · 模型按需下载        │  │ · 阅读器去 proxy        │  │ · libsr_qnn 直链     │
└──────────┬───────────┘  └──────────┬──────────────┘  └──────────┬──────────┘
           │                          │                            │
           └──────────────┬───────────┘                    三项全过？
                          ▼                                        │
                 ┌── 阶段 C1~C4（可选，30~45 人月）────────────────┘
                 │ 壳 → 阅读器 → 业务 → 网络收编 → 去 Python
                 └──────────────────────────────────────────────
```

**并行工作流**：
- W1 工具链（JDK 25 / NDK r30，§7）：第 0 周完成，之后只做回归。
- W2 工程保障（§6）：CI、指标基线、发布策略，贯穿全程。

---

## 3. 阶段 0：基线与门禁（1 周，2026-11-02 ~ 11-08）

| # | 任务 | 改动点 | 完成标准 |
|---|---|---|---|
| 0.1 | **修 16KB 不合规** | 用 NDK ≥ r28 重编 `libsr_qnn`/`libsr_qnn_proc`/`libpython3.14`/`libc++_shared`；recipe 产物（openssl/libffi/sqlite/jpeg/png/freetype/turbojpeg）随之重编 | `check_elf_page_align.py` 两个 APK 都是 **0 未对齐**；`check_16kb_device.sh` 真机通过 |
| 0.2 | **门禁不可绕过** | `build_android.sh` 已有硬门禁（`ALLOW_UNALIGNED=1` 才放行），补一条：放行时必须往 `python/android/build_logs/` 写 `unaligned_override.json`（含原因/操作人/时间） | 任何 `ALLOW_UNALIGNED=1` 的构建都能被审计 |
| 0.3 | **build-info 落盘** | 打包末尾把 JDK/NDK/Gradle/AGP/Qt/p4a/CPython 版本 + git sha 写入 `assets/build-info.json`（或 `res/raw`），运行时"关于"页可读 | 从 APK 就能反查工具链（现在做不到，见 §7.1） |
| 0.4 | **修正过期文档** | `resolve_jdk.sh` 头注释仍写"默认 17~24"；`requirements_android_ci.txt` 的 PySide6 版本说明与 6.12 不一致 | 文档与 `versions.env` 一致 |
| 0.5 | **指标基线快照** | 用 §1.4 的口径各测一次，存 `docs/baseline_2026-11.md` | 后续每阶段有对照 |

**为什么 0.1 必须先做**：`0.87` 的 arm64 包实测 15 个库 `p_align=4096`（含 `libsr_qnn.so`、`libpython3.14.so`、`libc++_shared.so`）。
Android 17 若把兼容模式设为 fatal，表现为**装完即闪退**；Google Play 对 `targetSdk≥35` 的更新自 2025-11 起也强制要求。
现在 `targetSdk=34` 只是"暂时不触发"，不是"没问题"。

---

## 4. 阶段 A：P0 快赢（4 周，2026-11-09 ~ 12-06）

### A1 资源二进制化（0.5 天）

| 项 | 内容 |
|---|---|
| 改动 | `script/build_qrc.py`：`pyside6-rcc` 加 `--binary -o python/src/images.rcc`；`python/src/start.py:78` 的 `import images_rc` 换成 `QResource.registerResource(":/images.rcc")` + `addSearchPath` |
| 收益 | 启动 **-196 ms**、RSS **-75 MB**、APK **-22 MB**（`private.tar` 里这份是明文存储） |
| 风险 | 资源路径需要与 `:/png/...` 前缀对齐；Android 端 `private.tar` 里也要带上 `.rcc` |
| 验收 | `bench_startup.py` 冷启 ≤800 ms；`du` 对比 `images_rc.py` 消失；真机首帧不回退 |
| 回滚 | 保留 `images_rc.py` 生成开关，环境变量 `JM_USE_PYC_RES=1` 走旧路径 |

### A2 桌面解码链路去 PIL（0.5 天）

| 项 | 内容 |
|---|---|
| 改动 | `python/src/tools/tool.py:1271 SegmentationPicture` 桌面分支改调 `ToolUtil.SegmentationQImage`（`:1099`，Android 已在用）；`task_multi.py:31` 的进程池入参同步 |
| 收益 | 单页 39.6 → 24 ms（-40%） |
| 风险 | 进程池里 PIL 释放 GIL，换 Qt 后不释放 → 见 B1，本步只改单线程路径 |
| 验收 | `bench_pipeline.py` 单页 ≤26 ms；像素级与旧路径逐点比对（`mobile_ui_diag.py:1125` 已有对照工具） |

### A3 模型按需分发（2~3 周，最大单项收益）

| 项 | 内容 |
|---|---|
| 改动 | 基础包只带 2~3 个默认模型（waifu2x cunet up2x + realcugan se up2x）；其余 24 个改为首次使用时下载 → 落 `files/state/models/`；`python/android/tools/patch_buildozer_spec.py` 的 `source.include_exts` 收窄；`python/android/main.py:121` 的"从 assets 兜底复制"改为"校验 + 缺失提示" |
| 收益 | APK **-150 MB 量级**（433 → 约 280，再叠加 A1 约 260） |
| 风险 | ① 首次超分需联网（离线场景要回退到"仅默认模型"）；② 下载源与校验（sha256 + 断点续传）；③ `sr_model_table.py` 的 41 个模型条目要区分"已装/未装" |
| 验收 | 基础包 ≤120 MB（含 3 模型）；下载失败有明确提示；模型表与磁盘一致（`host_test_sr_model_list.py` 扩展） |
| 回滚 | `JM_BUNDLE_ALL_MODELS=1` 走旧打包 |

> **注**：A3 的收益大于"迁移到 C++"在体积上的全部收益（-112 MB），成本却是 1/20。这是本方案排序的核心依据。

### A4 清理死重量（0.5 天）

- `Qt6Multimedia` + FFmpeg（17.7 MB 原始）：确认无人使用后从 p4a `--qt-libs` 与 wheel 拷贝清单里剔除。
- `Qt6Designer/Pdf/Quick*/3D*/Charts/Graphs` 等 100 MB 死模块：**Python 链做不到**（p4a 的 PySide6 recipe 整包拷贝），
  仅在阶段 C 才可得。本阶段只做记录，不投入。

---

## 5. 阶段 B：架构改造（10 周，2026-12-07 ~ 2027-02-13）

### B1 解码并发（3 周，收益最高）

| 项 | 内容 |
|---|---|
| 问题 | `QImage.loadFromData()` 期间不释放 GIL → 线程池 4 线程仅 **0.91×**；PIL 对照 1.82× |
| 方案 | 复用 `python/src/task/task_multi.py` 的进程池模式，新增"解码 worker"（进程内用 PIL 解码 + 分块还原 + 转 QImage 共享内存回传）；或写 pybind11 扩展把 `QImageReader` 搬出 GIL |
| 判据 | 先做 2 天 spike：进程池 IPC 成本 vs 解码收益。若单页 IPC >5 ms，改走 pybind11 |
| 验收 | 10 页预加载 ≤80 ms（现 200 ms，≥2.5×）；内存峰值 +≤30 MB |
| 风险 | 进程池在 Android 上 fork 开销、共享内存生命周期、崩溃隔离 |

### B2 列表去 widget-per-item（4 周）

| 项 | 内容 |
|---|---|
| 问题 | `comic_list_widget.py:334 AddBookItem` → 每项 `ComicItemWidget`（约 15 个子控件 + 5 处 QSS），4~8 ms/条 |
| 方案 | 改 `QStyledItemDelegate` 自绘（封面 + 标题 + 标签），保留现有数据模型；交互（点击/长按/复选框）在 delegate 内做命中测试 |
| 验收 | 首屏 60 条 ≤150 ms；滚动 p95 ≥110 fps；视觉回归截图与旧版逐像素比对（允许字体渲染差异） |
| 风险 | 触摸/长按/多选的行为回归（项目已有 `host_test_table_touch.py` 可扩展）；QSS 主题切换要改为读调色板 |

### B3 阅读器去 proxy（3 周）

| 项 | 内容 |
|---|---|
| 问题 | `read_graphics.py:459 InitAllQLabel` 每页一套 `QGraphicsProxyWidget + QLabel`，60 页 154 ms、+8 MB |
| 方案 | 自绘 `QGraphicsItem`（直接 `drawImage`，缩放用 `QPainter` 变换）；保留现有 `SetPixIem` 对外接口 |
| 验收 | 开章 ≤60 ms；单页内存 -8 MB/章；双指缩放/平移不回退（`host_test_pinch.py`） |

---

## 6. 工程保障（贯穿）

### 6.1 指标回归口径

| 指标 | 工具 | 阈值 | 频率 |
|---|---|---|---|
| 冷启/RSS | `bench_startup.py` | 不回退 >10% | 每 PR（宿主） |
| 单页解码 | `bench_pipeline.py` | 不回退 >10% | 每 PR |
| 列表/开章 | `bench_reader.py`、埋点 | 达标见 §5 | 每阶段 |
| 设备 PSS/首帧/fps | `am start -W`、`dumpsys meminfo`、FrameTick | 见 §1.1 | 每阶段一次真机 |
| APK 体积 | `ls -l` + 组成快照 | 见 G2 | 每次发包 |
| 16KB | `check_elf_page_align.py` | **0 未对齐** | 每次构建（硬门禁） |

### 6.2 CI

现状：`.github/workflows/android.yml` 只跑宿主回归（9,100 行），不打包（合理）。
本方案追加两条：

1. **PR 门禁**：宿主回归 + `check_elf_page_align` 干跑（对仓库内预置 `.so`）+ 体积预算检查。
2. **夜间构建**（可选）：Linux runner 打 x86_64 包，产出体积/baseline 报告；不打 arm64（QNN 库体积大、耗时长）。

### 6.3 双轨期策略（仅当进入阶段 C）

- 分支：`main` 保持 Python 版可发；C++ 版在 `next` 分支，按模块合并。
- 灰度：先出"只读本地"的 C++ 预览包，与 Python 版并存两个版本号。
- 冻结：进入 C2 后，`main` 只接受 P0 级修复。

### 6.4 可观测性

`build-info.json`（任务 0.3）同时写入：`git describe`、构建机、构建时刻、是否 `ALLOW_UNALIGNED`。
运行时可从"设置 → 关于 → 构建信息"导出，便于用户反馈时定位。

---

## 7. 工具链专项：JDK 25 LTS + NDK r30

### 7.1 现状核对（先纠正三个认知偏差）

| 项 | 仓库口径 | 实际 |
|---|---|---|
| JDK | `versions.env`：`JM_JDK_MIN=17`、`JM_JDK_MAX=25`，注释写"基线（0.88 起）：JDK 25 LTS + Gradle 9.x" | 本机 `local.env` 已钉 `jdk-25.0.5+8`；`build_android.sh` 会 `jm_apply_jdk` 并打印版本 |
| Gradle | `JM_GRADLE_MIN=9.1.0`、`JM_GRADLE_VERSION=9.8.2`，打包前改写 p4a 模板（步骤 4.5） | 已实现，且注释解释了"JDK 25 跑不了 8.14.x 的 wrapper" |
| NDK | `JM_NDK_MIN_MAJOR=28`，默认取 SDK 下最高版本 | 本机 `local.env` 指向 `30.0.16248370`；但 `setup_sdk.log` 显示初始化脚本装的是 **r26d**，最近一次 p4a 运行也报 `Found NDK version 26d` + `Maximum recommended NDK version is 28c` |
| 文档 | `resolve_jdk.sh` 头注释"区间：默认 17~24" | 与 `versions.env` 的 25 不一致（过期注释） |

**结论**：JDK 25 已经是"事实基线"，缺的是**文档、CI 与可追溯性**；NDK r30 只在部分机器上生效，
"实际用了哪个 NDK"无法从产物反查——这正是 0.87 出现 16KB 不合规却仍被发出来的原因。

### 7.2 JDK 25 LTS 的提升

| 维度 | 内容 | 量级 |
|---|---|---|
| 支持窗口 | JDK 25 是 LTS（2025-09 发布，Oracle 首席支持到 2030-09） | 5 年 |
| 前置条件 | Gradle ≥ 9.1 才能运行在 Java 25 上（官方矩阵：Java 24→8.14、25→9.1、26→9.4）；仓库已用 9.8.2 | 已满足 |
| 构建稳定性 | Gradle 9 的 configuration cache / daemon 在 JDK 25 上更稳；不再受"系统 java 是 27 导致 `Unsupported class file major version 71`"这类环境漂移影响（`resolve_jdk.sh` 注释里就记录了这个坑） | 中 |
| 安全性 | TLS/证书链、CVE 修复随 LTS 更新；Gradle 9.8.1 起修掉 3 个 high 级公告 | 中 |
| **对产物** | **0**。Java 侧只有 p4a bootstrap 的几十 KB dex，JDK 版本不进包、不影响运行 | 0 |
| 对 C++ 轨道 | Qt 6.12 的 Android 支持矩阵是 JDK 21 + Gradle 9.5.1 + AGP 9.2.1；JDK 25 需 Gradle ≥9.4.1，满足 | 前置条件 |

### 7.3 NDK r30（30.0.16248370）的提升

| 维度 | 内容 |
|---|---|
| 定位 | Google 当前**最新 LTS** 版本（下载页原文：Latest LTS (r30)） |
| 编译器 | LLVM/clang 更新到 `clang-r574158c`，修复多处误编译与编译器崩溃（Issue 2215 ARM -O3 挂起、2225 引用/指针误编译、2226 默认相等运算符崩溃、2229 等） |
| 平台 | NDK sysroot 最大 API 升到 **37**；simpleperf 预编译支持 Android 10 |
| 16 KB | r28 起默认按 16 KB 对齐（r30 延续），无需手工 `-Wl,-z,max-page-size=16384` |
| 对本项目 | ① `libsr_qnn.so` / `libsr_qnn_proc.so` 自动合规（现在是 4096，见 §7.4）；② 超分 C++ 侧受益于 clang 修复；③ 为阶段 C 的 C++ 代码提供与 Qt 官方库同代的工具链 |

### 7.4 关键实测：0.87 的 16KB 门禁不通过

对仓库内两个 APK 直接跑仓库自带门禁：

```
$ python python/android/tools/check_elf_page_align.py python/android/JMComic-0.87-arm64-v8a-debug.apk
[FAIL] 15 个 64 位库的 PT_LOAD 段不满足 16384 字节对齐
  libc++_shared.so / libc++_shared-d523468d.so / libcrypto.so / libffi.so / libfreetype.so
  / libjpeg.so / libmain_arm64-v8a.so / libpng16.so / libpython3.14.so / libpythonbin.so
  / libsqlite3.so / libsr_qnn.so / libsr_qnn_proc.so / libssl.so / libturbojpeg.so
小结: 对齐 195 / 未对齐 15 / 跳过 8

$ ... -x86_64-debug.apk → 对齐 183 / 未对齐 14
```

解读：

1. **Qt 官方库与 QNN 预编译库都是对齐的**（195 个通过），未对齐的全是 **p4a/自编产物** → 说明这次打包走的是旧 NDK（r26d），
   与 `setup_sdk.log` 和日志里的 `Found NDK version 26d` 完全吻合。
2. 仓库默认硬门禁（`ALLOW_UNALIGNED=1` 才放行）说明**这次构建被人工放行过**，但没有任何留痕。
3. 影响：Android 17 若把 16KB 兼容模式设为 fatal → **装完即闪退**；Play 对 `targetSdk≥35` 强制要求（当前 34，暂不触发）。
4. 这条不是"升级 NDK 的收益"，是**欠账**。升级到 r28+ 是顺带修掉它。

### 7.5 风险清单

| # | 风险 | 概率 | 影响 | 缓解 / 回退 |
|---|---|---|---|---|
| J1 | Gradle 版本与 JDK 不配对（p4a 模板自带 8.14.x） | 高（若绕过步骤 4.5） | 构建直接失败，报错位置与原因无关 | 已由 `gradle_wrapper.py` 在首次 gradle 调用前改写；任何新入口必须复用该步骤 |
| J2 | AGP 8.x 跑在 JDK 25 属"非官方测试组合" | 中 | Gradle 插件告警/个别任务失败 | 保持 AGP 8.x + Gradle 9.8.2（已验证可用）；回退 JDK 21 |
| J3 | `javac -source/-target 8` 在 JDK 25 已 obsolete | 高（仅告警） | JDK 26+ 可能移除，p4a 模板若仍用 8 会硬失败 | 钉 `JM_JDK_MAX=25`，不要越过；给 p4a 模板加 `-Xlint:-options` 静音 |
| J4 | keytool/apksigner 默认签名算法变化 | 低 | debug 安装失败或签名校验异常 | 删除并重建 `debug.keystore` 验证一次；release 走固定 keystore |
| J5 | 团队机器 JDK 漂移（如系统 java 27） | 高 | 报 `Unsupported class file major version`，难定位 | `resolve_jdk.sh` 已强制区间；建议仓库自带 `tools/jdk-25` 或 CI 显式 `setup-java: 25` |
| N1 | p4a 官方只保证 NDK ≤ 28c（日志原文 "Maximum recommended ... 28c, but newer versions may work"） | 高 | recipe（openssl/libffi/sqlite/CPython）在新 clang 下可能冒新告警/错误 | 双 NDK 矩阵：`JM_NDK_VERSION=28.2.13676358` 作为回退开关；阶段 0 先用 r30 冷构建两个 ABI 验证 |
| N2 | Qt 6.12 官方 Android 库用 r27.2 编，与 r30 混用 | 中 | 极端情况下 libc++ 符号缺失/运行期异常 | `llvm-nm -D --undefined-only` 检查 Qt 库未定义符号；libc++ 前向兼容，APK 内只保留**一份** r30 的 `libc++_shared.so` |
| N3 | APK 内存在两份 libc++（`libc++_shared.so` 与 `libc++_shared-d523468d.so`） | 中（现状即存在） | 两个 libc++ 运行时共存，异常/RTTI 跨边界可能异常 | 用 `/proc/<pid>/maps` 确认实际加载；长期方案是给 curl_cffi wheel 打 SONAME 补丁或重编 wrapper |
| N4 | 新 clang 改变默认告警等级 | 中 | 自编 C++（sr_qnn）编译失败 | `-Wno-error` 兜底；CI 加 `-Werror` 反向前置检查 |
| N5 | 用户设备（Android 15/16）在 16KB 页上跑 | 低~中 | 兼容模式下性能下降/首启警告 | 任务 0.1 修复后实测 `check_16kb_device.sh` |

### 7.6 推荐组合与回退

| 场景 | JDK | Gradle | AGP | NDK | 说明 |
|---|---|---|---|---|---|
| **主推（0.88 基线）** | 25 LTS (25.0.5+8) | 9.8.2 | 8.13.x（p4a 模板） | **30.0.16248370** | 需通过 §7.7 全部检查 |
| 回退 1（保守） | 21 | 8.14.6 | 8.13.x | 28.2.13676358 | 出现 N1/N2 类问题时整体回退 |
| 回退 2（只退 NDK） | 25 | 9.8.2 | 8.13.x | 28.2.13676358 | recipe 编译失败但 JDK 侧正常时使用 |
| 阶段 C 轨道 | 25 | ≥9.5.1 | 9.2.1（Qt 生成） | 27.2 或 30（二选一验证） | Qt 6.12 矩阵以 27.2 为准，r30 需实测 |

### 7.7 验证清单（可执行）

```bash
# 1) 版本解析（不构建）
bash python/android/tools/resolve_jdk.sh --check          # 期望 Java 25.x，区间 17~25
bash python/android/tools/resolve_ndk.sh --sdk "$ANDROID_HOME" --check   # 期望 30.x + 16KB 结论

# 2) 两个 ABI 冷构建（清 .work，强制重编 recipe）
rm -rf python/android/.work && cd android && ./build_android.sh --abi arm64-v8a,x86_64

# 3) 产物验收（三件套，缺一不可）
python python/android/tools/check_elf_page_align.py cpp/dist/JMComic-*-arm64-v8a-debug.apk
python python/android/tools/check_apk_qt_libs.py    cpp/dist/JMComic-*-arm64-v8a-debug.apk
adb install -r cpp/dist/JMComic-*-arm64-v8a-debug.apk && \
  adb shell am start -W -n org.jmcomic.jmcomic/org.kivy.android.PythonActivity | grep TotalTime

# 4) 真机 16KB（Android 15+ 设备）
bash python/android/tools/check_16kb_device.sh
```

判定标准：

- `check_elf_page_align` → **未对齐 0**（当前 15）；
- `check_apk_qt_libs` → 加载清单与 APK 内 Qt 库一致；
- 首帧 TotalTime ≤ 300 ms（不回退）；
- 冷构建总耗时 ≤ 90 min（两 ABI；r30 的 clang 更慢属正常，超过则回退 28.2）。

### 7.8 结论

1. **JDK 25 LTS：采用，无需犹豫。** 收益是"构建可维护性 + 支持窗口"，对 APK 产物和运行时零影响；
   唯一硬约束是 Gradle ≥9.1（已锁 9.8.2），以及"不要越过 25 去追 26/27"（`-source 8` 与 Qt/p4a 矩阵都跟不上）。
2. **NDK r30：建议采用。** 它是官方 LTS，且顺带修掉 0.87 已经存在的 16KB 合规缺陷；
   代价是 p4a 未官方支持（>28c 属"may work"），必须用回退开关兜底。
3. **优先级高于一切的是任务 0.1**：现在发出去的包里 15 个库不合规，这不是"升级带来的风险"，是"不升级的现状"。

---

## 8. 资源与排期

| 阶段 | 周 | 起止 | 人力 | 交付物 |
|---|---|---|---|---|
| 0 基线与门禁 | 1 | 11-02 ~ 11-08 | 1 人 | 16KB 合规包、build-info、基线快照 |
| A P0 快赢 | 4 | 11-09 ~ 12-06 | 1 人 | `images.rcc`、桌面解码、模型分包（基础包 ≤120 MB） |
| B1 解码并发 | 3 | 12-07 ~ 12-27 | 1 人 | 预加载 ≥2.5× |
| B2 列表 delegate | 4 | 12-28 ~ 01-24 | 1 人 | 首屏 ≤150 ms |
| B3 阅读器去 proxy | 3 | 01-25 ~ 02-13 | 1 人 | 开章 ≤60 ms |
| C0 C++ 技术验证 | 3 | 12-15 ~ 01-04（并行） | 1 人 | 三项验证报告 + Go/No-Go |
| C1~C4（条件触发） | — | 2027-03 起 | 3~4 人 | 见 §9 |

阶段 A+B 合计 **11 周 / 约 3 人月**，完成后按 §1.1 复测，预期拿到评估中 60~70% 的收益。

---

## 9. 阶段 C（条件触发，仅在 C0 三项全过时启动）

| 阶段 | 范围 | 工期 | 决策门 |
|---|---|---|---|
| C0 | 技术验证：① curl-impersonate 在 Android 自编并跑通真实请求（TLS 指纹 + ECH + H3）；② Qt C++ 空壳在 **Windows** 上 `androiddeployqt` 出包并装真机；③ `libsr_qnn.so` 被主程序直接链接（去 ctypes） | 3 周 | 任一项失败 → 停在 B 阶段，转 P3（渲染层 pybind11） |
| C1 | 壳 + 导航 + 设置 + 本地阅读器（保留 Python 网络子进程） | 8 周 | 首帧 ≤150 ms、PSS ≤450 MB |
| C2 | 列表/下载/登录/收藏逐模块替换 | 12 周 | 功能对齐率 100%，PSS ≤400 MB |
| C3 | 网络收编（curl-impersonate + AES 解密重写） | 6 周 | 真机连续请求 1 小时零失败 |
| C4 | 删除 Python 运行时 | 3 周 | APK ≤280 MB、启动与 C1 持平 |

止损规则（每阶段末执行）：**PSS 未降 25% 或首帧未降 50% → 停止后续阶段**，已交付模块保留。

---

## 10. 需要拍板的五件事

1. **16KB 修复是否本周做**（建议：是，它是当前唯一"会闪退"的确定缺陷）。
2. **模型是否接受"按需下载"**（影响离线体验，是体积目标 G2 的前提）。
3. **是否接受"11 周内不做 C++"**（即 A+B 优先，C 只做 3 周验证）。
4. **NDK r30 是否作为 0.88 唯一基线**（建议：是，但保留 `JM_NDK_VERSION=28.2` 回退开关）。
5. **是否投入 3 周做 C0**（即使最终不迁移，验证结论也能让后续决策不再靠猜）。
