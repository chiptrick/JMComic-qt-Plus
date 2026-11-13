# JMComic-qt · Android 移植分支（Python 版 + C++ 版）

本仓库是 [tonquer/JMComic-qt](https://github.com/tonquer/JMComic-qt) 的**修改版**，包含两套互不依赖的实现：

* **Python 版**（`python/`）：上游桌面客户端代码 + Android 移植层，PySide6 / python-for-android 技术栈。
* **C++ 版**（`cpp/`）：独立重写的 Qt 6 Widgets/Quick 客户端，不依赖 Python 运行时。

两套实现各自构建、各自出包，产物分别落在 `python/dist/`、`cpp/dist/`。2026-11 做过一次目录重构：原先位于仓库顶层的 `src/`、`res/`、`ui/`、`lib/`、`script/`、`translate/`、`android/` 已整体移入 `python/`。当前目录树：

```
python/     Python 版：src/ res/ ui/ android/ packaging/ + build_linux.sh
cpp/        C++ 版：shell/(ui/ tls/ quic/ 3rdparty/ host_tests/) assets/ tools/ demo_swipe/
            + build_windows.ps1 / build_linux.sh
docs/       设计、修复、验收文档与截图证据
.github/    CI 工作流
```

> 本项目仅供技术研究学习。这是修改版而非上游官方版本；上游桌面端代码归属见 [NOTICE.md](NOTICE.md) 与 [LICENSE](LICENSE)（LGPL v3）。

---

## 1. 两版差异（以下均以仓库内现有文件为依据）

### 1.1 功能范围

| 维度 | Python 版 | C++ 版 | 可验证依据 |
|---|---|---|---|
| UI 框架 | PySide6（Qt Designer `.ui` + QWidget） | Qt 6 Widgets 自绘 + QQuickWidget 卡片墙，无 `.ui` 文件 | `python/ui/`、`python/src/view/`；`cpp/shell/ui/CMakeLists.txt`（`AUTOUIC OFF`） |
| 页面/视图 | `python/src/view/` 下有 category、comment、download、help、history、index、info、main、nas、read、search、setting、tool、user 共 14 个业务目录 | `main_desktop.cpp` 的 `PageFromName` 注册 21 个页面名（home/category/search/detail/reader/download/local/favorite/history/comment/sr/settings/about/login/mycomment/remote/localfav/week/nas/batchsr/help/proxy） | 目录清单；`cpp/shell/ui/main_desktop.cpp:113-134` |
| 接口层 | 依赖 `jmcomic` 包 + `python/src/server/{req,server,user_handler}.py` | 自实现 `cpp/shell/jm_api.cpp`，端点字面量为 `/promote`、`/latest`、`/search`、`/album`、`/chapter`、`/favorite`、`/login`、`/daily`、`/setting`、`/daily_chk`、`/chapter_view_template` | `python/src/requirements.txt`；`cpp/shell/jm_api.cpp` |
| 登录态 | `python/src/tools/user.py`、`python/src/qt_owner.py` | `cpp/shell/session.cpp`、`cpp/shell/ui/ui_prefs.cpp`（`AppPaths` 统一落盘） | 同名源码文件 |
| 下载 | `python/src/task/task_download.py`、多个 `upload_*.py`（WebDAV / SMB / SMBv3） | `cpp/shell/ui/download_manager.cpp`：章节级任务队列、3 路并发页图、WebP 解码 + scramble 还原、`.jm-download.json` 断点恢复 | 源码文件与 `docs/` 修复记录 |
| 超分 | 桌面走 `sr_vulkan*` 模块，Android 走 QNN/HTP（`python/src/tools/sr_backend.py`、`python/android/sr_qnn/`） | `cpp/shell/sr_engine.cpp`：39 个 ONNX 模型目录、运行时 `QLibrary` 加载 ONNX Runtime；`cpp/shell/ui/sr_model_sheet.cpp` 提供选择浮层 | `python/src/start.py:InitSrEngine()`；`cpp/shell/ui/CMakeLists.txt` |
| 网络进阶 | `curl_cffi`（TLS 指纹 / HTTP3 / ECH），`python/src/server/req.py` 的 `SetProxy` | `cpp/shell/tls/openssl_tls.cpp` + `quic/quic_http.cpp` + `doh_client.cpp`，OpenSSL ECH 与 ngtcp2/nghttp3 HTTP/3；`cpp/shell/ui/proxy_page.cpp`、`node_tester.cpp`、`region_code.cpp` 分流测速 | 源码文件、`docs/quic_deps_versions.md` |
| 阅读器 | `python/src/view/read/` | `cpp/shell/reader.cpp` 单视口自绘 `ReaderView`，9 种阅读模式 | `docs/reader_parity_python_vs_cpp.md` |
| **C++ 版明确未接入的部分** | — | 代码内以 toast/空态标注「尚未接入」：云端记录接口、节点管理、日志导出、更新检查、注册页、找回密码、QNN/HTP 执行后端 | `cpp/shell/ui/ui_pages_extra.cpp:122,128,212`、`ui_pages_misc.cpp:2099,2238,2239,2390,2391` |

> 上表只列仓库里能直接查到的代码事实；未列出的能力（如某页面是否端到端可用）不作断言。C++ 版 `NasPage`、`CommentPage` 等类存在声明与占位实现（`cpp/shell/ui/ui_pages.h:455-648`），不等同于对应业务已接通。

### 1.2 依赖

| | Python 版 | C++ 版 |
|---|---|---|
| 语言/运行时 | Python（`python/src/requirements.txt` 钉 `PySide6==6.5.3`、`jmcomic==2.6.20`、`curl_cffi>=0.13,<=0.16.1`、`pycryptodomex`、`pillow`、`lxml`、`bs4`、`Pysocks`、`natsort`、`webdavclient3`、`tqdm`、`pysmb`、`smbprotocol`、`sr-vulkan` 系列） | C++17，Qt 6.12（Core / Gui / Widgets / Svg / Network / Quick / QuickWidgets / Concurrent），CMake ≥ 3.22，Ninja |
| Android 附加 | PySide6 / shiboken6 Android wheel、`pyside6-android-deploy`、p4a/buildozer、JDK 17、Android SDK、NDK r28+；CI 侧钉 `PySide6-Essentials==6.12.0`、`jmcomic==2.7.7`、`pycryptodome` | Qt `android_<abi>` kit、Android SDK、NDK `30.0.16248370`、`JAVA_HOME`；可选 OpenSSL 4.x（ECH + QUIC-TLS）、ngtcp2 / nghttp3；ONNX Runtime 运行时动态加载；libwebp 解码端源码内嵌 |
| 版本口径差异 | 桌面 requirements 钉 `PySide6==6.5.3` / `jmcomic==2.6.20`，Android APK 实际用 `6.12.0` / `2.7.7` | 由 `JM_WITH_QUIC` 三态 AUTO/ON/OFF 控制 HTTP/3 依赖是否编入 |

### 1.3 运行前提

| | Python 版 | C++ 版 |
|---|---|---|
| 桌面 | 需 Python 环境与对应 requirements；**本仓库只提供 Linux 桌面构建脚本** `python/build_linux.sh`（PyInstaller → deb / AppImage），脚本头明确「Windows 桌面构建不提供」 | Windows 需 `C:\Qt\6.12.0\mingw_64`、`Tools\Ninja`、`Tools\mingw1310_64` 在 PATH；Linux 需 Qt 6.12 gcc_64 + cmake/ninja/g++。产物 `cpp/shell/ui/build/jmcomic_ui(.exe)`，运行时 Qt DLL 需在 PATH |
| Android | 宿主**必须 Linux 或 macOS**（`python/android/build_android.sh` 注明 pyside6-android-deploy 不支持 Windows）；minSdk 34；`arm64-v8a`（真机）/ `x86_64`（模拟器）两种 ABI，一次只装一种；debug 签名包 | Windows 上用 `cpp/shell/ui/build_android.ps1 -Abi arm64-v8a|x86_64` 出包；`cpp/shell/build_windows.ps1` 出设备壳 APK。需要 `android:debuggable=true` 才能通过 `applicationArguments` 传回归参数，真机 V2463A 要求清单 `enableOnBackInvokedCallback=false` |
| 已知构建限制 | 首次冷构建需 NDK + QNN SDK，CI 不跑真打包 | 本机沙箱内 Gradle 打包会因 `utimensat` 受限挂起，出包改用 Python 重打包 + `apksigner.jar` 签名链路（见 `docs/mobile_ech_h3_enable.md`） |

### 1.4 平台适配对照

| 平台 | Python 版 | C++ 版 |
|---|---|---|
| Windows 桌面 | 无本仓库构建脚本（上游 CI 有） | 有 `cpp/build_windows.ps1` |
| Linux 桌面 | 有 `python/build_linux.sh`（deb / AppImage） | 有 `cpp/build_linux.sh`（仅可执行文件，脚本注明不含 AppImage 打包） |
| macOS 桌面 | 上游 CI 覆盖；本仓库无独立构建脚本 | 未提供 |
| Android 真机 arm64 | 支持（minSdk 34，QNN 超分） | 支持（真机验证记录见 `docs/shots/device_094_*.png`） |
| Android 模拟器 x86_64 | 支持 | 支持 |
| Android 打包宿主 OS | Linux / macOS | Windows（PowerShell 脚本） |

---

## 2. 上游 tonquer/JMComic-qt 提交对本仓库 Python 版的适用性

评估范围：上游 `main` 最近 **30 个提交**（2026-09-14 ~ 2026-11-02，最新 `63af1a5`）。方法：把上游 diff 中 `src/`、`res/`、`ui/`、`lib/`、`script/` 路径映射到 `python/` 同名路径，检查文件是否存在，并用每个提交 diff 的**新增代码行**作为特征串比对本地文件内容。所有 30 个提交涉及的文件在 `python/` 下均存在（缺失 0）。

三类结论定义：

* **可应用**：目标文件存在、本地尚未包含该改动，语义不依赖上游独有流程；cherry-pick 时统一把路径前缀改为 `python/` 即可。
* **需调整**：本地已有部分改动、文件被 Android 适配层改过，或涉及本仓库已改写的 CI / 打包流程，直接套补丁会冲突，需手工合并。
* **不适用**：合并提交（无独立补丁）、本地已包含该改动、或上游专属的版本号 / 发布流程改动。

| # | 提交 | 日期 | 主题 | 结论 | 依据 |
|---|---|---|---|---|---|
| 1 | `63af1a5` | 11-02 | Merge #247 | 不适用 | 合并提交，改动由 `9fda0d3` 承载 |
| 2 | `9fda0d3` | 10-31 | 注销清实时会话 + 配置导入原子化 | 可应用 | 4 文件均在 `python/src/`；新增行特征 0/14 命中，本地 `navigation_widget.py` 仍以新 `User()` 对象注销 |
| 3 | `9c2ae07` | 10-30 | Merge #246 | 不适用 | 合并提交 |
| 4 | `612d533` | 10-29 | 账号按钮真正注销 | 需调整 | 特征 5/12 部分命中；`navigation_widget.py` 含本分支移动端改动 |
| 5 | `fd496a4` | 10-29 | 删除历史后不复用选中项 | 可应用 | `search_line_edit.py` 存在，0/4 命中 |
| 6 | `f5579da` | 10-29 | 删除历史不触发搜索 | 可应用 | 同上，0/4 命中 |
| 7 | `769632d` | 10-29 | 设置 zip 备份/恢复 | 可应用 | `setting_view.py` 存在且无 backup/zipfile 相关代码，0/4 命中 |
| 8 | `a454021` | 10-29 | 历史逐行删除按钮 | 可应用 | `search_line_edit.py` 0/4 命中 |
| 9 | `c126295` | 10-22 | Merge #244 | 不适用 | 合并提交 |
| 10 | `60c827e` | 10-20 | 图片 CDN 失败回退 | 需调整 | 1/4 命中；本地 `server.py:306` 的 `resetUrlHost` 仍注释，实现方式不同 |
| 11 | `d01343b` | 10-20 | API 线路失败回退 | 需调整 | 1/4 命中，同上 |
| 12 | `9cadee8` | 10-20 | 历史可删除 + 不遮挡 IME 候选 | 可应用 | 0/4 命中，本地无 `preedit` 相关代码 |
| 13 | `d06f1f8` | 10-20 | `Defalut` 拼写修正 | 可应用 | 0/1 命中；`python/src/qt_owner.py:400` 仍为 `"Defalut"` |
| 14 | `76d79d6` | 10-20 | 系统代理无 http 项崩溃 | 需调整 | 0/4 命中；`python/src/server/req.py` 的 `SetProxy` 有本分支真机适配注释 |
| 15 | `6870736` | 10-20 | macOS/Linux 旧配置目录迁移 | 可应用 | 0/4 命中；`setting.py` 有 `_xdgDir` 但无 legacy 目录迁移 |
| 16 | `c9ed2ae` | 10-07 | navigation_widget.py | 不适用 | 已应用（1/1 命中） |
| 17 | `2dc5e97` | 10-07 | 更新（13 文件） | 不适用 | 已应用（31/31 命中） |
| 18 | `5882879` | 10-07 | json 解析修正 | 不适用 | 已应用（7/7） |
| 19 | `7198e34` | 09-29 | 错误值修正 | 不适用 | 已应用（1/1） |
| 20 | `687a0f1` | 09-28 | jm3 接口 | 不适用 | 已应用（3/4） |
| 21 | `1bc51be` | 09-21 | update（打包元数据） | 需调整 | 0/3 命中；涉及 `res/ubuntu`、`res/uos` 控制文件，本仓库构建已改为 `python/build_linux.sh` |
| 22 | `2046db1` | 09-21 | update（登录 + CI） | 需调整 | 16/24 命中；含 `.github/workflows`，本仓库 CI 已改为 Android 分支流程 |
| 23 | `5cf0e8b` | 09-20 | 新版登录逻辑 | 不适用 | 已应用（3/4） |
| 24 | `77c9c11` | 09-20 | 新接口 | 不适用 | 已应用（15/15） |
| 25 | `824d708` | 09-20 | 本地漫画翻页修正 | 不适用 | 已应用（3/3） |
| 26 | `85f5863` | 09-20 | 复用 Session / 测速 H3 | 不适用 | 已应用（8/8） |
| 27 | `1212ad4` | 09-15 | 版本号 | 不适用 | 已应用（2/2），且版本号由本仓库独立维护 |
| 28 | `5e6a538` | 09-14 | release.yml | 不适用 | 0/2 命中；本仓库 `.github/workflows/release.yml` 是另一套流程且默认不运行 |
| 29 | `d78d38b` | 09-14 | 优化（8 文件） | 不适用 | 已应用（18/23） |
| 30 | `2dd9c19` | 09-14 | 连载更新分类筛选 | 不适用 | 已应用（30/32） |

汇总：**可应用 8、需调整 6、不适用 16**（其中合并提交 3、已应用 12、上游专属流程 1）。共同前提：任何落地的上游补丁都必须做 `src/ → python/src/` 的路径映射，且 C++ 版不受这些补丁影响（两套实现无共享源码）。

---

## 3. 构建与回归入口

| 目标 | 入口 | 宿主 |
|---|---|---|
| Python 桌面（deb / AppImage） | `python/build_linux.sh` | Linux |
| Python Android APK | `python/android/build_android.sh`（`--abi arm64-v8a,x86_64`） | Linux / macOS |
| C++ 桌面 | `cpp/build_windows.ps1` / `cpp/build_linux.sh` | Windows / Linux |
| C++ Android UI APK | `cpp/shell/ui/build_android.ps1 -Abi arm64-v8a`（`-NoQuic` / `-WithQuic`） | Windows |
| C++ UI 回归 CLI | `jmcomic_ui.exe --quiet --shot <png> --shot-delay <ms>` + 各 `--*-selftest` / `--*-report` 钩子 | Windows 桌面；参数表见 `cpp/shell/ui/main_desktop.cpp:350-605` |
| 回归工具层 | `cpp/tools/quiet_runner.py`（静默窗口 + 低优先级 + 结果落盘）、`cpp/tools/c1_ui_desktop_suite.py` | Windows |

CI：`.github/workflows/android.yml` 跑宿主回归（字节码、`*.sh` 语法、C++↔ctypes 接口一致性、竖屏冒烟等）；`CI.yml` / `release.yml` 源自上游桌面流程，在本仓库默认不运行（工作流内有 `if: github.repository == 'tonquer/JMComic-qt'` 条件）。

---

## 4. 已知限制（仅列仓库内可查证项）

* C++ 版有若干页面仍为占位实现，见 §1.1 末行；`ui_pages_misc.cpp:2099` 明确说明 QNN/HTP 后端未接入。
* Python 版本仓库不提供 Windows 桌面构建脚本；Android 打包宿主不支持 Windows。
* 上游 2026-10-20 之后的 8 个可应用修复尚未同步到 `python/`，详见 §2。
* `python/src/requirements.txt` 与 Android APK 实际依赖版本不一致（PySide6 6.5.3 vs 6.12.0、jmcomic 2.6.20 vs 2.7.7），以 `python/src/requirements_android_ci.txt` 的说明为准。

---

## 5. 许可与署名

* 沿用上游 **GNU LGPL v3**，见 [LICENSE](LICENSE)；修改部分同样以 LGPL v3 提供。
* 上游项目：[tonquer/JMComic-qt](https://github.com/tonquer/JMComic-qt)；第三方组件与归属见 [NOTICE.md](NOTICE.md)。
* 上游桌面端代码版权归原作者所有，本分支不主张其版权。
