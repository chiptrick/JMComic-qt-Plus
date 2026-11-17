# 超分模型包（按需下载/安装/卸载/删除）

日期：2026-11-17　版本：cpp-ui 0.97.x

## 目标与范围

桌面（Windows）与移动端（Android）同一套 `SrPlugin` 逻辑：用户只下载/安装自己用得上的
模型，不用就卸载删除。**本轮不做后端插件化**（Vulkan/OpenVINO/ORT/QNN 拆包）——见
`docs/sr_backend_plugin_analysis_2026-11-16.md`，留待后续。

## 下载入口 = 模型选择列表（已合并）

原来的「超分模型包」独立表格已合并进 **超分模型选择浮层**（`SrModelSheet`）：

- 列表按算法分组；**未下载的模型留在它所属的算法分组里**（不单独分区），只是右列
  显示「下载」按钮而不显示状态徽标；
- 点「下载」即从网络下载该模型，行内显示「下载中 N%」「校验中…」「重试（失败）」；
- 下载完成后自动整表重建，模型归位到对应算法分组；
- 「刷新清单」按钮移到列表上方的**功能区**（与「已安装 N · 未下载 M · 下载源 在线/离线」
  提示同一行）；
- 选择列表入口：设置页「默认模型」、图片超分页、批量超分页（都打开同一浮层）。

## 安装/卸载语义

- **安装**：下载到 `<file>.part` → sha256 + 尺寸校验 → 通过后才 `rename` 成正式文件。
  失败/中断删半成品；同名先删再改名（覆盖升级）。
- **卸载**：通过 CLI `--plugin-uninstall` 或（后续可加）管理入口删除受管目录文件；
  `--plugin-uninstall-bundled` 额外删其它可写目录里的同名文件（随包 assets 只读，删不掉如实报错）。

## 落盘路径（Android 用户可见）

Android 上把应用的 **数据/模型/下载/缓存** 统一放到**外部私有目录**，用户用文件管理器或
`adb pull` 能直接访问（无需任何存储权限）：

```
/storage/emulated/0/Android/data/<pkg>/files/            ← AppPaths::DataDir()
  ├─ sr_models/              ← 模型包安装目录（SrEngine::PluginsDir()）
  ├─ download/               ← 默认下载目录（DownloadManager::DefaultRoot()）
  ├─ cache/                  ← 默认缓存目录（CachePath::DefaultRoot() = AppPaths::CacheDir()）
  └─ sr_models_manifest.json ← 清单缓存（--plugin-refresh 写入）
```

实现：`AppPaths::DataDir()` 在 Android 上优先用
`getExternalFilesDir(null)`（= 上面的路径），拿不到才退回 `/data/user/0/<pkg>/files`
（后者普通用户不可见，只有 root 能进）。桌面行为不变。`--path-report` 会打印
`androidExternalFiles=... / dataDir=... / cacheDir=...` 便于核对。

> 说明：个别机型对 `/sdcard/Android/data` 的访问经 MTP 可能受限，但应用自身 + adb pull
> 一定可读写；比原 `/data/user/0/...`（普通用户完全进不去）已明显改善。

## 清单

- 内嵌：`python/src/config/sr_models_manifest.json` → `:/sr/sr_models_manifest.json`（离线可用）。
- 网络刷新：`--plugin-refresh` 或浮层「刷新清单」从清单 `base_url` 同目录拉
  `sr_models_manifest.json` 并缓存；`JM_SR_MANIFEST_URL` 可覆盖为任意清单 URL。
  发布目录必须真的托管这份清单，否则刷新 404（内嵌清单仍可用）。

## 入口

| 端 | 入口 |
|---|---|
| 桌面/移动 UI | 打开模型选择浮层 → 末尾「未下载」分区逐条「下载」；功能区「刷新清单」 |
| 桌面 CLI | `--plugin-list` `--plugin-selftest` `--plugin-refresh` `--plugin-install <file\|名>` `--plugin-uninstall <file\|名>` |
| 移动端 | 同一套 `--plugin-*` 经 `jm.args` 注入；结果写外部分目录 `plugin-run.txt` |

## 推理模块（ONNX Runtime）——桌面端应用内下载

桌面端 CPU/QNN 推理依赖 `onnxruntime.dll`（约 19MB，GPU 走自研 Vulkan 不需要它）。为不必让
安装包背这份运行时，新增应用内下载：

- 设置页「超分」组 →「超分推理模块（ONNX Runtime）」行：显示状态（随安装包提供 / 已下载 /
  未安装）与当前运行时路径，点「下载/更新」从网络拉取。
- 落盘：`SrEngine::RuntimeDir()` = `<data>/sr_runtime/onnxruntime.dll`；`SrEngine::FindRuntime()`
  优先加载它，找不到再退回随安装包目录里的 `onnxruntime.dll`。
- 清单：`python/src/config/sr_runtime_manifest.json` → 内嵌 `:/sr/sr_runtime_manifest.json`；
  `base_url` 指向托管 `onnxruntime.dll` 的目录（下载 = base_url + file）；`JM_SR_RUNTIME_URL`
  可覆盖为任意 dll 直链（推荐指向 GitHub Release 资源，避免 jsdelivr 20MB 单文件上限）。
- 下载走 sha256 + 尺寸校验 + `.part` 原子改名；安装后 `SrEngine::UnloadRuntime()` 卸载旧库，
  下次推理重建会话即用新文件。
- CLI：`--rt-list` / `--rt-install [id]` / `--rt-uninstall [id]` / `--rt-report`。
## 超分后端插件（桌面：OpenVINO / SR-Vulkan 等）

把桌面各超分后端做成可独立**安装/删除**的插件包（tar.gz）。设置页「超分」组 →「超分后端插件」：

| 后端 | 类型 | 默认 | 说明 |
|---|---|---|---|
| OpenVINO 推理插件 | binary | 按需下载（约 228MB） | ORT plugin EP + openvino/tbb runtime；装在 ovep/ 后 Intel NPU/XPU 档可用 |

> SR-Vulkan（GPU）着色器随安装包分发（打进 qrc），**不再作为可安装/删除的插件**。

- 安装流程：下载 tar.gz → 归档 sha256 校验 → 解压 → 逐文件 sha256 校验 → 原子落位到
  `<data>/sr_backend/<targetSubdir>/`（`ovep` 或 `vkshaders`）。安装/删除后需重启生效。
- 加载优先级：OpenVINO 的 `OvPluginPath()` 先找 `<data>/sr_backend/ovep`，找不到再用随包 `ovep/`。
- 清单：`tools/gen_sr_backend_manifest.py` 生成 `python/src/config/sr_backend_manifest.json`
  （内嵌 `:/sr/sr_backend_manifest.json`）并打包 `cpp/dist/sr_backend_packages/*.tar.gz`。
  `base_url` 指向托管目录；`JM_SR_BACKEND_URL` 可覆盖为本地/镜像目录。
- CLI：`--backend-list` / `--backend-install <id>` / `--backend-uninstall <id>`。

> ONNX Runtime 运行时本体（CPU/QNN 推理引擎）已在前一节做成「推理模块」按需下载；本节「后端插件」
> 是各加速后端（OpenVINO EP、Vulkan 着色器），两者独立安装。