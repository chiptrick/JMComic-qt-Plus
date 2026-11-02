#!/usr/bin/env bash
# C1-W1：Qt 6.12 C++ 壳工程的 WSL 一键构建（复用 C0 已踩通链路）（2026-11-01 踩通的全链路，坑都固化在注释里）。
#
# 前置（一次性）：
#   1. Qt 6.12.0 Android kit：官方在线仓库已从 linux_x64/android/ 搬到 all_os/android/
#      （aqt 3.3.0 查旧路径会报 "Failed to locate XML data"，需手工下 7z + py7zr 解包）：
#        https://download.qt.io/online/qtsdkrepository/all_os/android/qt6_6120/qt6_6120_x86_64/
#          qt.qt6.6120.android_x86_64/6.12.0-0-202609280346qtbase-Linux-RHEL_9_6-Clang-Android-Android_ANY-X86_64.7z
#          （qttools 同理）→ 解到 $QT_ANDROID（结构：<dir>/bin include lib mkspecs jar share ...）
#   2. host Qt（交叉编译要 host 的 moc）：linux_x64/desktop/qt6_6120/qt6_6120/…linux_gcc_64 的
#      qtbase+qttools 7z → 解到 $QT_HOST。**下载用清华镜像**（官方源经代理只有 ~3MB/min）。
#   3. platforms/android-36：sdkmanager(12.0) 不认识 API 37，且 androidx.core:1.17.0 要求
#      compileSdk>=36 → 从 dl.google.com/android/repository/platform-36_r02.zip 手工解到
#      $SDK/platforms/android-36（注意 zip 直接解出 android-36/，要落在 platforms/ 下）。
#
# 已踩过的坑（都已在脚本内处理）：
#   - host 的 androiddeployqt 链接 ICU 73（RHEL 构建），WSL 系统没有 → 借 PySide6 桌面轮子
#     venv 里的 libicu*.so.73（LD_LIBRARY_PATH）；
#   - JVM CA 不全 → JAVA_TOOL_OPTIONS 指向 /opt/java-ca.jks（tools/wsl_fix_java_ca.sh 生成）；
#   - Qt 生成的 gradle wrapper 默认从 services.gradle.org 拉 gradle-9.5.1，经代理 TLS 握手
#     PKIX 失败（即便 truststore 生效）→ 把 distributionUrl 改成腾讯镜像；
#   - gradle.properties 里 androidCompileSdkVersion 由 Qt 默认写成 android-34，低于 androidx
#     要求 → 配置后 sed 成 android-36。
set -euo pipefail

REPO_WSL="/mnt/c/Users/WQ159/source/repos/JMComic-qt"
QT_ANDROID="$HOME/jmcomic-build/qt-android-6.12/6.12.0"
QT_HOST="$HOME/jmcomic-build/qt-host-6.12/6.12.0"
SDK="$HOME/jmcomic-build/android-sdk"
NDK="$SDK/ndk/30.0.16248370"
GRADLE="$HOME/jmcomic-build/gradle-9.8.2"
SRC="$HOME/jmcomic-build/src/android/c1_shell"
BUILD="$HOME/jmcomic-build/build-c1"

# --- 环境 ---
source "$REPO_WSL/android/tools/env.sh" >/dev/null 2>&1 || true
export PATH="$GRADLE/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/jmcomic-build/venv311/lib/python3.11/site-packages/PySide6/Qt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export JAVA_TOOL_OPTIONS="-Djavax.net.ssl.trustStore=/opt/java-ca.jks -Djavax.net.ssl.trustStorePassword=changeit"

for f in "$QT_ANDROID/bin/qt-cmake" "$QT_HOST/libexec/moc" "$SDK/platforms/android-36" "$NDK/ndk-build"; do
  [ -e "$f" ] || { echo "[fail] 缺前置: $f（见脚本头注释）"; exit 1; }
done

# --- 源码同步（repo → ext4 构建树） ---
mkdir -p "$(dirname "$SRC")"
rsync -a --delete "$REPO_WSL/android/c1_shell/" "$SRC/"

# --- W6：Python 运行时打包（从现有 p4a dist 复制；没有就跳过，CMake 会明确告警） ---
# 这里不把 96MB bundle 提交进仓库：它属于构建产物，p4a 已经验证过一次。
ABI="${ABI:-x86_64}"
PY_DEST="$SRC/pylibs/$ABI"
PY_SRC_GLOB=$(find "$HOME/jmcomic-build/keep-"* -path "*/dists/*/libs/$ABI/libpythonbin.so" 2>/dev/null | head -1 || true)
if [ -n "$PY_SRC_GLOB" ] && [ -f "$PY_SRC_GLOB" ]; then
  PY_SRC_DIR=$(dirname "$PY_SRC_GLOB")
  mkdir -p "$PY_DEST"
  for lib in libpythonbin.so libpython3.14.so libsqlite3.so libffi.so libcrypto.so libssl.so; do
    cp "$PY_SRC_DIR/$lib" "$PY_DEST/" 2>/dev/null || true
  done
  # bundle 里目录 mode=644（无 x），应用用户解不开；重打包成 755
  if [ ! -f "$PY_DEST/libpybundle_fixed.so" ]; then
    python3 "$SRC/make_pybundle.py" "$PY_SRC_DIR/libpybundle.so" "$PY_DEST/libpybundle_fixed.so"
  fi
  echo "Python runtime libs -> $PY_DEST (from $PY_SRC_DIR)"
else
  echo "[warn] 没找到 p4a dist 里的 Python 运行时（$ABI），本次 APK 不会带 Python"
fi

# --- 配置 + 编译 + 出包 ---
rm -rf "$BUILD"
"$QT_ANDROID/bin/qt-cmake" -S "$SRC" -B "$BUILD" \
  -DANDROID_ABI="${ABI:-x86_64}" \
  -DANDROID_PLATFORM=android-36 \
  -DANDROID_SDK_ROOT="$SDK" \
  -DANDROID_NDK_ROOT="$NDK" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DQT_HOST_PATH="$QT_HOST"

# gradle.properties / wrapper 由 make_apk 步骤按配置首次生成（已存在则不改）。
# 所以：先完整构建一次（若 Qt 默认 compileSdk=34 被 androidx 拒收会失败，但配置文件已生成），
# 再打补丁、再构建一次 → 确定性收敛。
cmake --build "$BUILD" || true

# Qt 默认 compileSdk=android-34，被 androidx.core:1.17.0 拒收（要求 >=36）
sed -i 's/androidCompileSdkVersion=.*/androidCompileSdkVersion=android-36/' "$BUILD/android-build/gradle.properties" 2>/dev/null || true

# W6：Python 运行时不靠 nativeLibraryDir（extractNativeLibs=false 时 .so 不落盘、没法 exec）。
# 统一打进 assets/，运行时复制到应用私有目录再启动（见 pyruntime.cpp）。
if [ -f "$SRC/pylibs/$ABI/libpybundle_fixed.so" ]; then
  mkdir -p "$BUILD/android-build/assets"
  for lib in libpythonbin.so libpython3.14.so libsqlite3.so libffi.so libcrypto.so libssl.so; do
    cp "$SRC/pylibs/$ABI/$lib" "$BUILD/android-build/assets/$lib" 2>/dev/null || true
  done
  cp "$SRC/pylibs/$ABI/libpybundle_fixed.so" "$BUILD/android-build/assets/libpybundle.so" 2>/dev/null || true
fi
# P4：桥脚本一起进 assets（main.cpp 会复制到私有目录再执行）。
# 少了这一步，C1 只能跑 main.cpp 里那段内联 echo 桩——"Android 端桥能跑"就变成假绿。
mkdir -p "$BUILD/android-build/assets"
cp "$SRC/bridge.py" "$SRC/net_client.py" "$BUILD/android-build/assets/"
# wrapper 默认源经代理 TLS 失败 → 腾讯镜像
sed -i 's|distributionUrl=.*|distributionUrl=https\\://mirrors.cloud.tencent.com/gradle/gradle-9.5.1-bin.zip|' \
  "$BUILD/android-build/gradle/wrapper/gradle-wrapper.properties" 2>/dev/null || true

cmake --build "$BUILD"

APK=$(find "$BUILD" -name '*.apk' | head -1)
[ -n "$APK" ] || { echo "[fail] 未产出 APK"; exit 1; }
echo "APK: $APK  ($(du -m "$APK" | cut -f1) MB)"

# --- P4 门禁：桥脚本必须真的在包里（否则设备上只能跑内联桩） ---
for f in bridge.py net_client.py; do
  if unzip -l "$APK" 2>/dev/null | grep -q "assets/$f"; then
    echo "[OK] APK assets 含 $f"
  else
    echo "[FAIL] APK 里没有 assets/$f —— 设备侧 net.* 一定不可用"
    exit 1
  fi
done

# --- C0-③ 的"直链"判据（linux 侧，比运行时更硬）：主程序自己的 .so 必须 NEEDED libsr_qnn.so ---
APP_SO=$(find "$BUILD" -maxdepth 1 -name "libc1shell_*.so" | head -1)
if [ -n "$APP_SO" ]; then
  NEEDED=$("$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -d "$APP_SO" 2>/dev/null | grep NEEDED)
  if echo "$NEEDED" | grep -q "libsr_qnn.so"; then
    echo "[OK] 直链生效：$(basename "$APP_SO") DT_NEEDED 含 libsr_qnn.so"
    echo "$NEEDED" | grep -E "sr_qnn|onnxruntime" | sed 's/^/     /'
  else
    echo "[FAIL] $(basename "$APP_SO") 的 DT_NEEDED 里没有 libsr_qnn.so —— 直链没生效，别把门禁绿当结论"
    exit 1
  fi
else
  echo "[FAIL] 找不到 libc1shell_*.so，无法验证直链"
  exit 1
fi

# --- 门禁 ---
PY="$HOME/jmcomic-build/venv311/bin/python"
"$PY" "$REPO_WSL/android/tools/check_elf_page_align.py" "$APK"
# 以前这里无条件写 "N/A（C++ 空壳无 Python 运行时）"，但 W6 之后包里确实带 bundle：
# 无条件 N/A 等于给了一条永远不会红的门禁。有 bundle 就必须真跑。
if unzip -l "$APK" 2>/dev/null | grep -q "assets/libpybundle.so"; then
  # shiboken6 的 0x1000 是上游缺陷（文档 §0A 更新 15），且 C1 的 bridge.py 只 import stdlib，
  # 从不 import PySide6 → 该 .so 不会被 dlopen。把它写成显式例外，而不是把整条门禁关掉。
  "$PY" "$REPO_WSL/android/tools/check_pybundle_align.py" "$APK" "$ABI" \
    --member assets/libpybundle.so --allow shiboken6
else
  echo "bundle 门禁 N/A（本包未含 Python runtime 的 libpybundle.so）"
fi
