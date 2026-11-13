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

## 2. 构建与回归入口

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

## 3. 已知限制（仅列仓库内可查证项）

* C++ 版有若干页面仍为占位实现，见 §1.1 末行；`ui_pages_misc.cpp:2099` 明确说明 QNN/HTP 后端未接入。
* Python 版本仓库不提供 Windows 桌面构建脚本；Android 打包宿主不支持 Windows。
* `python/src/requirements.txt` 与 Android APK 实际依赖版本不一致（PySide6 6.5.3 vs 6.12.0、jmcomic 2.6.20 vs 2.7.7），以 `python/src/requirements_android_ci.txt` 的说明为准。

---

## 4. 许可与署名

* 沿用上游 **GNU LGPL v3**，见 [LICENSE](LICENSE)；修改部分同样以 LGPL v3 提供。
* 上游项目：[tonquer/JMComic-qt](https://github.com/tonquer/JMComic-qt)；第三方组件与归属见 [NOTICE.md](NOTICE.md)。
* 上游桌面端代码版权归原作者所有，本分支不主张其版权。
