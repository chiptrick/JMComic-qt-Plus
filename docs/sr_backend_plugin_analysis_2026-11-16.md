# 超分后端插件化：可行性与工作量分析

日期：2026-11-16
范围：把超分实现拆为可独立**下载、安装、加载、卸载、删除**的插件包，按需要只装其中一种或多种。
目标实现：`onnxruntime`、`OpenVINO`、Vulkan 路径、`sr_qnn`（含用户所称 `ncnn_Vulkan` 的口径澄清）。

---

## 1. 结论摘要

**总体可行，但四个后端的"插件"语义不可能完全对齐，必须先做范围取舍。**

1. **桌面（Windows）适合做完整插件化**：ORT 与 OpenVINO 已经是运行期动态加载，Vulkan 后端边界较干净（tile 进 / tile 出），三者都具备"独立包 + 按目录加载 + 卸载删除"的条件。建议采用**薄插件方案**：宿主保留分块、补边、拼装、几何探针和回退策略，插件只承载推理运行时与着色器。
2. **Android 无法在不改变分发方式的前提下实现"下载原生代码并加载"**：`targetSdk=34`，实测主进程链接器命名空间不能 `dlopen` `filesDir` 下的 `.so`，仅 QNN 独立子进程通过 `LD_LIBRARY_PATH` 指向 `filesDir/qnn_libs` 成功；同时应用商店政策禁止自行下载可执行代码（Play Feature Delivery 除外）。因此 Android 的可落地形态是：**代码随 APK，按需下载的是模型/设备适配数据；真正的代码按需下发需要引入 Dynamic Feature 分发渠道**。
3. **`ncnn_Vulkan` 口径需要澄清**：当前 C++ 主力 Vulkan 后端 `cpp/shell/vk/`（约 5.2k 行）是**自研 SrVk**，不是 ncnn；ncnn/Vulkan 只存在于旧 Python/Android 路径 `python/android/sr_qnn/cpp_vk/sr_vk.cpp`（447 行包装 + 静态 ncnn）且已非活跃链路。若"插件"指现有 SrVk，成本按下文估算；若必须重新启用真正的 ncnn-Vulkan 实现，需另加编译链、模型转换与数值验收，追加约 **15~25 人日**。
4. **工作量**（1 名熟悉本仓 C++/Qt 的工程师，人日按 6 小时有效工作）：
   - 分发期裁剪出多个 SKU（不满足"按需下载"，仅作对照）：**2~4 人日**
   - 推荐方案，桌面完整 + Android 仅模型/数据按需：**约 36~48 人日**
   - 推荐方案再补齐 Android 真代码下发（Feature Delivery/重打包渠道）：**约 52~68 人日**
   - 全后端统一厚 ABI 插件（宿主只做调度）：**约 70~95 人日**
   以上为基于代码规模与改造面的工程估算，未含真机兼容性长尾验证，建议按 ±30% 看待。

---

## 2. 现状盘点：四个实现各自的"插件成熟度"

| 实现 | 当前集成方式 | 关键证据 | 插件化难度 |
|---|---|---|---|
| ONNX Runtime（CPU / ORT-QNN EP） | 运行期 `QLibrary` 裸名加载外部 DLL/so，未链接；桌面 `onnxruntime.dll` 19.3MB | `cpp/shell/sr_engine.cpp:243-265,510-566`；`cpp/shell/ui/CMakeLists.txt:224,253-265` | 低 |
| OpenVINO EP | 运行期 `RegisterExecutionProviderLibrary` 加载插件；整套 `ovep/` 实测 227MB，默认不随包，已有 `JM_OV_EP_DIR` / `<exe>/ovep` 目录约定 | `cpp/shell/sr_engine.cpp:688-711,954-1059`；`cpp/shell/ui/CMakeLists.txt:267-280` | 低（已有雏形） |
| Vulkan（自研 SrVk） | C++ 源码编译进主 exe；loader 运行期解析 Vulkan 符号；12 个 `.comp` 在构建期由 `qsb` 编成 24 个 `.qsb` 并打进 qrc | `cpp/shell/ui/CMakeLists.txt:137-192`；`cpp/shell/vk/sr_vk.h:1-12` | 中高（需抽出动态库 + 外部 shader） |
| QNN / HTP | Android 为独立子进程 `libsr_qnn_proc.so`（posix_spawn + 锁步协议）；桌面走 ORT 内置 QNN EP；厂商库设备专属，首次启动从 `/vendor/lib64` 复制到 `filesDir/qnn_libs` | `cpp/shell/sr_engine.cpp:1197-1212,1595-1628,2325-2409`；`python/android/sr_qnn/cpp/CMakeLists.txt:41-102` | 高（进程隔离 + 设备绑定） |

补充事实：

- `SrEngine` 是 `namespace` + 全局 `State` 单例，无虚接口/ABI；`RunTile` 用 `if (s.vkReady) / backend == ...` 硬分派，回退链与 `#ifdef Q_OS_ANDROID` 交织（`cpp/shell/sr_engine.cpp:153-202,2483-2512,2325-2409`）。
- 后端选择 UI 硬编码 6 档 `auto/qnn/ov-npu/xpu/vulkan/cpu`（`cpp/shell/ui/ui_pages_misc.cpp:2178-2215`），标签函数同样硬编码（`cpp/shell/sr_engine.cpp:2684-2707`），模型分组标签写死在 `cpp/shell/ui/sr_model_sheet.cpp:51-86`。
- 模型约 342MB（39 个 fp32 + q16），随包只拷 fp32 约 260MB；`models.txt` 已含 `base_url/sha256/size`，Python 侧 `model_store.py` 已有断点续传 + sha256 + 原子改名（`python/src/component/sr/model_store.py:98-118,243-310`），但 C++ UI 侧没有通用下载器、没有 zip 解压、没有签名校验。
- 全仓 26 个文件引用 `SrEngine`，`sr_engine.cpp` 单文件 3960 行；这是改造面的主体。

---

## 3. 建议架构：薄插件 + 清单驱动

```
UI（插件管理 / 模型选择 / 测速）
        ↓ 能力查询，不认识具体后端
SrEngine 宿主核心：分块·补边·拼装·几何探针·回退·配置
        ↓ 稳定 C ABI（jm_sr_backend_v1）
插件目录：<AppDir>/sr_plugins/<id>/ 或 Android <DataDir>/sr_plugins/<id>/
  ├─ manifest.json   id / version / abi / appApiRange / files[sha256,size] / deps / capabilities
  ├─ backend.dll|so  create_session / run_tile / destroy / probe / capabilities
  ├─ shaders/*.qsb   （Vulkan 插件自带，不再编进主程序 qrc）
  └─ models/         （可选，按模型包独立安装）
```

生命周期：`未安装 → 下载中 → 校验中 → 暂存安装 → 已安装 → 已加载/激活 → 禁用 → 卸载中 → 未安装`。
安装使用"暂存目录 + 原子改名 + 失败回滚"，进程被杀后靠 marker 恢复；卸载需要同时清理配置键、模型归属、测速缓存与 QNN 临时库。清单中必须声明 **ABI 版本、ORT/EP 版本矩阵、依赖插件、模型架构支持**，否则 OpenVINO EP 与 ORT 版本错配会表现为"会话建成但静默回退 CPU"这类难查故障。

---

## 4. 平台可行性差异（决定能否真正"下载并加载代码"）

### 桌面（Windows）
- ORT/OpenVINO：现成动态加载路径，改为绝对路径加载 + 会话全释放后 `unload` 即可；主要工作是依赖目录隔离与版本锁定。
- Vulkan：把 `cpp/shell/vk/` 5.2k 行做成独立动态库，导出 C ABI（避免 MinGW C++ ABI 漂移）；shader 由 qrc 改为插件目录内 `.qsb` 文件加载。Vulkan loader 已是运行期解析（`VK_NO_PROTOTYPES`），不阻碍该方案。
- 安全：下载可执行代码必须做**签名校验**（内嵌公钥），sha256 只保证传输完整，不能防恶意源；同时处理杀软误报与卸载后文件占用。
- 约束：同一进程内重复加载/卸载动态库要严格引用计数；ORT EP 插件的注册是进程级的，彻底卸载可能需要重启应用，这一点需在 UI 明示。

### Android
- 原生代码必须随 APK 进 `nativeLibraryDir`。实测 `app_data_file` 下的 `.so` 在 Android 10+ 无法 `dlopen`（`cpp/shell/sr_engine.cpp:1612-1619`）；QNN 子进程能加载 `filesDir/qnn_libs` 是特定 namespace/LD_LIBRARY_PATH 的窄路径，不构成通用插件加载能力。
- targetSdk=34，从应用私有目录执行下载的可执行文件受限；Play 分发政策也禁止绕过商店更新可执行代码。真正"下载插件代码"需改用 Play Dynamic Feature / App Bundle 或自有 sideload 渠道，且要重新过 16KB 页对齐门禁（`cpp/tools/check_ui_apk_16kb.py`：ZIP STORED + 偏移 mod 16384 + ELF PT_LOAD≥0x4000）。
- QNN 厂商库是**设备专属**，不能作为通用插件包分发；继续沿用首次启动复制机制，或按 SoC/驱动版本做设备适配包。
- 因此 Android 的现实分层：**后端开关/激活（随包代码） + 模型与设备数据包（可下载删除） + 可选的 Feature 模块（代码按需）**。若产品要求四端一致的"下载即装"，应把 Android 列为独立子项目。

---

## 5. 工作量拆解（推荐方案：薄插件）

| 阶段 | 内容 | 人日 |
|---|---|---|
| P0 契约与清单 | ABI v1、manifest schema、版本兼容矩阵、依赖声明 | 1.5~2.5 |
| P1 下载与安装 | C++ 断点续传下载器（复用 `model_store.py` 逻辑）、sha256+签名校验、zip 解压、暂存/原子安装/回滚、进度 UI | 4~6 |
| P2 插件管理 | 发现、枚举、状态机、安装/卸载/删除、失败清理、配置迁移与缓存失效 | 4~5.5 |
| P3 后端适配 | ORT 1~1.5；OpenVINO 1.5~2；SrVk 抽库+C ABI+shader 外置 5~8；桌面 QNN EP 2~3 | 9.5~14.5 |
| P4 宿主重构 | `SrEngine` 注册表化、能力查询、回退链改为能力驱动，去掉 UI 硬编码枚举 | 4~6 |
| P5 UI | 插件管理页 + 模型/后端联动 + 安装态/失败态文案 + 重启提示 | 3~5 |
| P6 构建与分发 | 插件包构建脚本、签名、发布清单、CI、桌面 SKU 裁剪 | 2.5~4 |
| P7 回归与文档 | `--plugin-list/install/uninstall/selftest/report`、现有 sr 回归补测、文档 | 4~6 |
| **合计（桌面）** | | **32.5~49.5；建议预算 36~48（已含少量缓冲）** |
| Android 增量 | 模型/数据下载 + 偏好迁移 + 真机验收 | +4~7 |
| Android 真代码按需 | Feature Delivery / 重打包渠道 + 16KB 门禁 + 政策合规 | +14~20 |

口径说明：以上不含"ncnn-Vulkan 从旧代码复活"的 15~25 人日，也不含跨机型 QNN 兼容性长尾；若要求"厚 ABI"（连分块/拼装/探针都进插件），P3+P4 再增加约 20~30 人日，且回退与数值基线回归成本显著上升。

---

## 6. 主要风险与缓释

| 风险 | 等级 | 缓释 |
|---|---|---|
| Android 下载原生代码受技术与商店政策双重限制 | 高 | 分平台定义语义；Android 先做数据按需，代码按需走 Feature Delivery |
| ORT / OpenVINO EP 版本不匹配，出现"假 NPU/假加速" | 高 | manifest 锁定版本矩阵；加载成功后再用 `BackendReport()` 断言实际后端 |
| 无稳定 ABI：全局 `State`、硬分派、几何探针跨后端共享 | 高 | 先做 C ABI 边界；探针留在宿主，插件只实现 tile 推理 |
| QNN 厂商库设备绑定、子进程协议 | 高 | 不随通用包分发；保留设备适配包与现有成功判据（`htp=1` + backend 含 Hexagon） |
| 卸载后 `srModelName`/`backend`/`srBenchData` 悬空 | 中 | 卸载时归一化偏好、按插件 id 隔离模型缓存、把插件版本纳入测速 `sig` |
| 下载可执行代码的安全与信任链 | 中高 | 内嵌公钥验签，拒绝无签名包；暂存目录不可执行直至校验通过 |
| 16KB 对齐与签名一致性（Android） | 中 | 插件内 `.so` 复用现有门禁脚本校验；debug/release 只使用既有 keystore |
| 回归覆盖不足、桌面与真机行为漂移 | 中 | 新增插件自检 CLI；沿用 `--quiet --shot`；真机只信落盘报告 |

---

## 7. 实施建议

1. **先分离"模型包"与"后端包"**：模型已有清单与下载范式，先把它做成可独立安装/删除，能在 1~2 周内交付可见价值，并为后端包验证下载/校验/安装链路。
2. **后端先做 OpenVINO、再做 ORT**：二者改动最小，可用来验证 ABI、版本矩阵、状态机和卸载语义；Vulkan 抽库单独排期。
3. **QNN 与 Android 单列**：先明确"代码随包"口径，再决定是否投入 Feature Delivery；在口径确定前不要承诺与桌面一致的"下载即装"。
4. **每个阶段都补自检钩子**：`--plugin-list` / `--plugin-selftest` / `--plugin-report`，并让现有 `--sr-bench`、`--sr-sheet-*`、`--sr-page-image` 在插件缺失时走可见的降级态，而非静默回退。
5. **验收判据**：插件安装前后 `BackendReport()` 与 `BackendName()` 一致反映实际执行后端；卸载后无残留文件、无悬空偏好、其它超分路径与阅读器主流程不受影响。
