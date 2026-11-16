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

---

## 5. 开源项目声明与感谢

本项目在实现与参考过程中使用了下列开源项目及其成果。正是这些项目让本仓库的
两套客户端成为可能，在此向所有作者与维护者表示诚挚感谢。许可证与归属信息
**以上游项目为准**，完整清单亦见 [NOTICE.md](NOTICE.md)。

### 5.1 上游与直接参考的项目

| 项目 | 在本项目中的角色 | 许可（以上游为准） |
|---|---|---|
|[JMComic-qt](https://github.com/tonquer/JMComic-qt)|本仓库的上游项目，Python 版桌面端代码与 Android 移植基础；C++ 版主题 QSS 亦复用其设计|LGPL-3.0|
|[JMComic-Crawler-Python](https://github.com/hect0x7/JMComic-Crawler-Python)|接口协议与图片分割/还原算法的参考实现|MIT|
|[sr-vulkan](https://github.com/tonquer/sr-vulkan)|Python 版桌面端超分后端参考|以上游为准|
|[waifu2x-ncnn-vulkan](https://github.com/nihui/waifu2x-ncnn-vulkan)、[Real-ESRGAN](https://github.com/xinntao/Real-ESRGAN)、[realcugan-ncnn-vulkan](https://github.com/nihui/realcugan-ncnn-vulkan)|超分算法与模型格式的参考实现|各项目上游许可|
|[local-dream](https://github.com/xororz/local-dream)|Android 端 QNN/HTP 超分实现（运行时库部署、FastRPC 依赖解析等）的参考|以上游为准|
|[RealSR-NCNN-Android](https://github.com/tumuyan/RealSR-NCNN-Android)|C++ 版 Vulkan 超分后端（tile 分档、性能与 DEVICE_LOST 处理）的参考实现|以上游为准|

### 5.2 界面、运行时与构建

| 项目 | 在本项目中的角色 | 许可（以上游为准） |
|---|---|---|
|[Qt / PySide6](https://www.qt.io/qt-for-python)|C++ 版 Qt 6 Widgets/Quick 与 Python 版界面框架（含 Android）|LGPL-3.0|
|[CPython](https://www.python.org/)|Python 版及打进 APK 的 Python 运行时|PSF-2.0|
|[python-for-android](https://github.com/kivy/python-for-android)、[buildozer](https://github.com/kivy/buildozer)|Python Android APK 打包链|MIT|
|[Android NDK](https://developer.android.com/ndk)、CMake、Ninja|C++ 版 Android 与本地构建|各项目上游许可|
|[Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers)|C++ Vulkan 超分后端所用头文件|Khronos 上游许可|

### 5.3 网络、安全与图像处理

| 项目 | 在本项目中的角色 | 许可（以上游为准） |
|---|---|---|
|[OpenSSL](https://www.openssl.org/)|TLS / ECH / QUIC-TLS 实现|Apache-2.0|
|[ngtcp2](https://github.com/ngtcp2/ngtcp2)、[nghttp3](https://github.com/nghttp3/nghttp3)|C++ 版 HTTP/3 协议栈|MIT|
|[curl_cffi](https://github.com/lexiforest/curl_cffi)（内含 [curl-impersonate](https://github.com/lwthiker/curl-impersonate) / curl）|Python 版 HTTP 客户端与浏览器指纹模拟|MIT|
|[ONNX Runtime](https://github.com/microsoft/onnxruntime)|C++ 版与 Python 版的超分推理运行时|MIT|
|[libwebp](https://github.com/webmproject/libwebp)|C++ 版内嵌的 WebP 图片解码|BSD-3-Clause|
|[Pillow](https://python-pillow.org/)|Python 版图片解码/编码|MIT-CMU|

### 5.4 Python 工具库

Python 版还使用了 [lxml](https://lxml.de/)、[beautifulsoup4](https://www.crummy.com/software/BeautifulSoup/)、
[pycryptodomex](https://www.pycryptodome.org/)、[PySocks](https://github.com/Anorov/PySocks)、
[natsort](https://github.com/SethMMorton/natsort)、[webdavclient3](https://github.com/designerror/webdav-client-python-3)、
[tqdm](https://github.com/tqdm/tqdm)、[pysmb](https://github.com/miketeo/pysmb)、
[smbprotocol](https://github.com/jborean93/smbprotocol)、[cryptography](https://github.com/pyca/cryptography)
等库，分别用于 HTML 解析、加解密、代理、自然排序、WebDAV/SMB 上传、进度显示等。
感谢这些项目的维护者。

> **说明**：[Qualcomm QNN SDK](https://www.qualcomm.com/developer/software/qualcomm-ai-engine-direct)
> 运行时属于 Qualcomm 专有组件，并非开源软件；本仓库仅提供接入脚本，不授予其任何权利，
> 分发含该运行时的 APK 前请自行确认符合 Qualcomm 许可条款。
> 超分 ONNX 权重文件不在本仓库中，许可证以来源仓库为准。
