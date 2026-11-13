# 仓库结构说明（2026-11 重构）

## 1. 目标与原则

- **两套实现并行**：Python 版（`python/`）与 C++ 版（`cpp/`）各有独立的源码、资源副本、构建脚本与产物目录。
- **互不干扰**：任一端的构建、清理、产物不会写到另一端目录；C++ 不再引用 `python/` 下的文件。
- **共享只限文档**：`docs/` 是两版共用的设计与测试记录；主题 QSS 在两端各留一份（`python/res/theme` 与
  `cpp/assets/theme`），由 `cpp/tools/check_theme_sync.py` 守卫一致性。

## 2. 顶层结构

```
JMComic-qt/
├── python/                          Python 版
│   ├── src/                         业务源码（start.py 入口、view/、task/、tools/ …）
│   ├── ui/                          45 个 .ui 源文件（生成物在 src/interface/ui_*.py）
│   ├── res/                         主题、图标、images.qrc、deb/AppImage 模板
│   ├── data/  lib/                  运行期数据与预编译依赖（linux/macos/win 子目录）
│   ├── script/                      资源生成与打包辅助（build_qrc.py 等）
│   ├── translate/                   翻译
│   ├── android/                     Python 版 Android 打包工程
│   │   ├── build_android.sh         一键构建（arm64-v8a / x86_64）
│   │   ├── main.py                  p4a 入口
│   │   ├── tools/                   128 个构建/回归脚本（host_test_*、wsl_*、门禁）
│   │   ├── sr_qnn/ recipes/ shims/ java/ wheels/ …
│   │   └── README.md                Android 移植的完整踩坑记录
│   ├── packaging/legacy/            旧 build_ubuntu/uos/appimage 脚本（仅存档，不再维护）
│   └── build_linux.sh               ★ Python 版唯一构建入口（Linux）
│
├── cpp/                             C++ 版（Qt 6.12）
│   ├── shell/                       原 android/c1_shell
│   │   ├── ui/                      C++ UI（桌面 + Android 同一个目标）
│   │   │   ├── build_win.bat        最底层的 Windows 单次编译
│   │   │   └── build_android.ps1    UI APK（x86_64 / arm64-v8a）
│   │   ├── host_tests/              jm_api / jm_crypto / jm_webp / net_client 的宿主测试
│   │   ├── 3rdparty/libwebp/        webp 解码
│   │   ├── build_windows.ps1        设备壳 APK（sr_qnn + OpenSSL）
│   │   └── build_wsl.sh             WSL 下的设备壳构建
│   ├── c0_skeleton/                 C0 直链验证骨架
│   ├── assets/                      C++ 自带的 theme/ 与 icon/
│   ├── tools/                       C++ 专用工具与回归
│   │   ├── c1_ui_desktop_suite.py   桌面 66 项回归
│   │   ├── c1_ui_android_smoke.py   AVD 20 项冒烟
│   │   ├── check_ui_apk_16kb.py     APK 16 KB 页对齐门禁
│   │   ├── host_test_soft_buttons.py 造型源码守卫
│   │   └── check_theme_sync.py      主题副本一致性
│   ├── build_windows.ps1            ★ C++ 桌面 Windows 构建
│   └── build_linux.sh               ★ C++ 桌面 Linux 构建
│
├── docs/                            共用文档与截图
├── .github/workflows/               CI（路径已改到新结构，尚未在 Actions 上复跑）
└── README.md / NOTICE.md / LICENSE / CHANGELOG
```

## 3. 构建矩阵

| 目标 | 脚本 | 宿主 | 产物 |
|---|---|---|---|
| Python 桌面 | `python/build_linux.sh` | **仅 Linux** | `python/dist/jmcomic_*_amd64.deb`、`*.AppImage` |
| Python Android | `python/android/build_android.sh` | Linux / macOS | `python/android/JMComic-<ver>-<abi>-debug.apk` |
| C++ 桌面 | `cpp/build_windows.ps1` | Windows | `cpp/shell/ui/build/jmcomic_ui.exe`（`-Deploy` → `cpp/dist/windows/`） |
| C++ 桌面 | `cpp/build_linux.sh` | Linux | `cpp/shell/ui/build-linux/jmcomic_ui` |
| C++ UI APK | `cpp/shell/ui/build_android.ps1` | Windows | `cpp/dist/JMComic-ui-<ver>-<abi>-debug.apk` |
| C++ 设备壳 APK | `cpp/shell/build_windows.ps1` | Windows | `cpp/shell/build-win/<abi>/...apk` |

> Python 版**不提供 Windows 构建脚本**（按要求）。Linux 构建脚本本次只做静态检查（`bash -n`），
> 实际编译与验证留待 Linux 真机。

## 4. 构建脚本用法

### 4.1 `python/build_linux.sh`

```bash
cd python
./build_linux.sh                          # 全量（含超分）+ deb
./build_linux.sh --mode nosr              # 无超分版本
./build_linux.sh --mode both              # 两个版本都出
./build_linux.sh --package appimage       # AppImage（需要 appimagetool）
./build_linux.sh --no-venv                # 用当前 Python 环境
./build_linux.sh --version v1.3.2 --clean
```

- 默认创建 `python/.venv-build` 并安装 `PyInstaller` 与 `src/requirements*.txt`。
- 先跑 `script/build_qrc.py` 生成 `src/images_rc.py`，再在 `src/` 下执行 PyInstaller。
- deb 用 `dpkg-deb --root-owner-group`（不需要 sudo）；输出统一到 `python/dist/`。
- 旧的 `packaging/legacy/*.sh` 仅作对照，不再维护。

### 4.2 `cpp/build_windows.ps1`

```powershell
powershell -ExecutionPolicy Bypass -File cpp\build_windows.ps1
powershell -ExecutionPolicy Bypass -File cpp\build_windows.ps1 -Clean -Jobs 8
powershell -ExecutionPolicy Bypass -File cpp\build_windows.ps1 -Deploy   # windeployqt 到 cpp\dist\windows
```

- 默认 Qt `C:\Qt\6.12.0`，可用 `-QtRoot` 覆盖；CMake/Ninja/MinGW 从同级的 `C:\Qt\Tools` 取。
- 构建目录默认 `cpp\shell\ui\build`（与既有回归脚本一致），`-Clean` 会删掉它。

### 4.3 `cpp/build_linux.sh`

```bash
cd cpp
./build_linux.sh                          # 自动找 qt-cmake / qmake6
./build_linux.sh --qt ~/Qt/6.12.0/gcc_64
./build_linux.sh --clean -j 8
```

- 依赖 Qt 6.12 的 Core/Gui/Widgets/Svg/Network/Quick/QuickWidgets、CMake ≥3.22、Ninja、g++。
- 产物 `cpp/shell/ui/build-linux/jmcomic_ui`；运行前需把 `$QT_ROOT/lib` 加进 `LD_LIBRARY_PATH`。
- 按本轮要求**未在 Linux 上实际运行**，仅做了 `bash -n` 语法检查。

## 5. 路径不变式（改动时注意）

1. `python/` 完整镜像了重构前的仓库根：`src/ui/res/data/lib/script/translate/android` 的相对位置不变，
   因此 Python 侧的 128 个工具脚本里 `parents[2]`、`../..` 这类根路径推导无需修改。
2. `cpp/shell/` 与原来的 `android/c1_shell/` 同深度（`cpp/shell/ui` ↔ `android/c1_shell/ui`），
   CMake 里的 `../../../` 与构建脚本里的 `..\..\..` 语义不变；只有主题资源改到 `cpp/assets/`。
3. C++ 主题副本与 `python/res/theme` 必须同步：
   `python cpp/tools/check_theme_sync.py`（漂移返回非 0，`--fix` 覆盖）。
4. Python Android 的 build-info / 放行审计默认写到**仓库根**的 `docs/`（`SRC_REPO` 默认取 `python/..`）。

## 6. 旧路径映射

| 旧路径 | 新路径 |
|---|---|
| `src/`、`ui/`、`res/`、`data/`、`lib/`、`script/`、`translate/` | `python/` 下同名目录 |
| `build_ubuntu.sh`、`build_uos.sh`、`build_appimage.sh` | `python/packaging/legacy/`（入口改为 `python/build_linux.sh`） |
| `android/**`（除 c1_shell） | `python/android/**` |
| `android/c1_shell/**` | `cpp/shell/**` |
| `android/c0_skeleton/**` | `cpp/c0_skeleton/**` |
| `android/tools/c1_ui_*.py`、`check_ui_apk_16kb.py`、`host_test_soft_buttons.py`、`c1_w7_*` | `cpp/tools/` |
| `android/dist/`（C++ APK 产物） | `cpp/dist/` |
| `res/theme/*.qss`（C++ 侧引用） | `cpp/assets/theme/*.qss`（副本，由检查脚本守卫） |

## 7. 验证状态

| 项 | 状态 |
|---|---|
| `cpp` 桌面 Windows 构建（`build_win.bat`） | ✅ 清理后重建通过（100 个编译目标） |
| `cpp/build_windows.ps1` | ✅ 复跑通过，输出 `cpp/shell/ui/build/jmcomic_ui.exe` |
| `cpp` UI APK（`build_android.ps1 -Clean`） | ✅ 出包到 `cpp/dist/JMComic-ui-0.88-x86_64-debug.apk`（61.6 MB） |
| APK 16 KB 页对齐 | ✅ 46/46 `.so` 的 `PT_LOAD` ≥ 0x4000 |
| `cpp` 桌面回归 | ✅ 完整套件 66/66 PASS（`docs/test_logs/desktop_2026-11-04c.log`） |
| `cpp/tools/check_theme_sync.py` | ✅ 6 个 QSS 一致 |
| `python/build_linux.sh` / `cpp/build_linux.sh` 语法 | ✅ `bash -n` |
| `python` 源码编译检查 | ✅ `compileall`（见验证日志） |
| Python Linux 构建、Android（Python/C++）打包 | ⏸ 按本轮要求留待 Linux 真机 |
| CI workflows | ⚠ 路径已改，未在 Actions 上复跑 |
