#!/usr/bin/env bash
# C0-②'：Qt 6.12 C++ 空壳的 WSL 一键构建（2026-11-01 踩通的全链路，坑都固化在注释里）。
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
SRC="$HOME/jmcomic-build/src/android/c0_skeleton"
BUILD="$HOME/jmcomic-build/build-c0"

# --- 环境 ---
source "$REPO_WSL/android/tools/env.sh" >/dev/null 2>&1 || true
export PATH="$GRADLE/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/jmcomic-build/venv311/lib/python3.11/site-packages/PySide6/Qt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export JAVA_TOOL_OPTIONS="-Djavax.net.ssl.trustStore=/opt/java-ca.jks -Djavax.net.ssl.trustStorePassword=changeit"

for f in "$QT_ANDROID/bin/qt-cmake" "$QT_HOST/bin/moc" "$SDK/platforms/android-36" "$NDK/ndk-build"; do
  [ -e "$f" ] || { echo "[fail] 缺前置: $f（见脚本头注释）"; exit 1; }
done

# --- 源码同步（repo → ext4 构建树） ---
mkdir -p "$(dirname "$SRC")"
rsync -a --delete "$REPO_WSL/android/c0_skeleton/" "$SRC/"

# --- 配置 + 编译 + 出包 ---
rm -rf "$BUILD"
"$QT_ANDROID/bin/qt-cmake" -S "$SRC" -B "$BUILD" \
  -DANDROID_ABI="${ABI:-x86_64}" \
  -DANDROID_PLATFORM=android-36 \
  -DANDROID_SDK_ROOT="$SDK" \
  -DANDROID_NDK_ROOT="$NDK" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DQT_HOST_PATH="$QT_HOST"

# Qt 默认 compileSdk=android-34，被 androidx.core:1.17.0 拒收（要求 >=36）
sed -i 's/androidCompileSdkVersion=.*/androidCompileSdkVersion=android-36/' "$BUILD/android-build/gradle.properties"
# wrapper 默认源经代理 TLS 失败 → 腾讯镜像
sed -i 's|distributionUrl=.*|distributionUrl=https\\://mirrors.cloud.tencent.com/gradle/gradle-9.5.1-bin.zip|' \
  "$BUILD/android-build/gradle/wrapper/gradle-wrapper.properties"

cmake --build "$BUILD"

APK=$(find "$BUILD" -name '*.apk' | head -1)
[ -n "$APK" ] || { echo "[fail] 未产出 APK"; exit 1; }
echo "APK: $APK  ($(du -m "$APK" | cut -f1) MB)"

# --- 门禁 ---
PY="$HOME/jmcomic-build/venv311/bin/python"
"$PY" "$REPO_WSL/android/tools/check_elf_page_align.py" "$APK"
echo "bundle 门禁 N/A（C++ 空壳无 Python 运行时）"
