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
SRC="$HOME/jmcomic-build/src/cpp/shell"
BUILD="$HOME/jmcomic-build/build-c1"

# --- 环境 ---
source "$REPO_WSL/python/android/tools/env.sh" >/dev/null 2>&1 || true
export PATH="$GRADLE/bin:$PATH"
export LD_LIBRARY_PATH="$HOME/jmcomic-build/venv311/lib/python3.11/site-packages/PySide6/Qt/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export JAVA_TOOL_OPTIONS="-Djavax.net.ssl.trustStore=/opt/java-ca.jks -Djavax.net.ssl.trustStorePassword=changeit"

for f in "$QT_ANDROID/bin/qt-cmake" "$QT_HOST/libexec/moc" "$SDK/platforms/android-36" "$NDK/ndk-build"; do
  [ -e "$f" ] || { echo "[fail] 缺前置: $f（见脚本头注释）"; exit 1; }
done

# --- 源码同步（repo → ext4 构建树） ---
mkdir -p "$(dirname "$SRC")"
rsync -a --delete "$REPO_WSL/cpp/shell/" "$SRC/"

# --- C1 网络：随包 OpenSSL（Qt TLS 后端运行时 dlopen 它） ---
ABI="${ABI:-x86_64}"
NET_SSL_DIR="$SRC/openssl/$ABI"
mkdir -p "$NET_SSL_DIR"

# 三个来源按优先级取：
#   1) 自编的 16KB 对齐版本（arm64 专用，见 /tmp/build_openssl_a64.sh）；
#   2) p4a dist（x86_64 有；arm64 的 dist 缺 openssl）；
#   3) 同 ABI 的成品 APK 里抽（arm64 走这条：0.88 包里带 libssl/libcrypto，4KB 对齐）。
SSL_DIR=""
if [ -f "$HOME/jmcomic-build/openssl-a64/libssl.so" ]; then
  SSL_DIR="$HOME/jmcomic-build/openssl-a64"
fi
if [ -z "$SSL_DIR" ]; then
  _p=$(find "$HOME/jmcomic-build/keep-"* -path "*/dists/*/libs/$ABI/libssl.so" 2>/dev/null | head -1 || true)
  [ -n "$_p" ] && SSL_DIR=$(dirname "$_p")
fi
if [ -z "$SSL_DIR" ]; then
  _apk=$(ls -t "$HOME"/jmcomic-build/src/android/*"$ABI"*.apk 2>/dev/null | head -1 || true)
  if [ -n "$_apk" ]; then
    unzip -o -q "$_apk" "lib/$ABI/libssl.so" "lib/$ABI/libcrypto.so" -d "$NET_SSL_DIR/extract" 2>/dev/null || true
    SSL_DIR="$NET_SSL_DIR/extract/lib/$ABI"
    echo "OpenSSL 取自 $_apk（注意：p4a 的 arm64 库是 4KB 对齐）"
  fi
fi

if [ -n "$SSL_DIR" ] && [ -f "$SSL_DIR/libssl.so" ] && [ -f "$SSL_DIR/libcrypto.so" ]; then
  cp "$SSL_DIR/libssl.so" "$NET_SSL_DIR/libssl.so"
  cp "$SSL_DIR/libcrypto.so" "$NET_SSL_DIR/libcrypto.so"
  # Qt 在 Android 上按它自己 android_openssl 包的命名（libssl_3.so / libcrypto_3.so）
  # 找库；只给 libssl.so 会得到 "Failed to load libssl/libcrypto."（插件能加载但后端不可用）。
  cp "$SSL_DIR/libssl.so" "$NET_SSL_DIR/libssl_3.so"
  cp "$SSL_DIR/libcrypto.so" "$NET_SSL_DIR/libcrypto_3.so"
  echo "OpenSSL libs -> $NET_SSL_DIR (from $SSL_DIR)"
else
  echo "[fail] 没找到 $ABI 的 libssl.so/libcrypto.so（三处来源都空）；HTTPS 会不可用"
  exit 1
fi

# --- 配置 + 编译 + 出包 ---
rm -rf "$BUILD"
"$QT_ANDROID/bin/qt-cmake" -S "$SRC" -B "$BUILD" \
  -DANDROID_ABI="${ABI:-x86_64}" \
  -DANDROID_PLATFORM=android-36 \
  -DANDROID_SDK_ROOT="$SDK" \
  -DANDROID_NDK_ROOT="$NDK" \
  -DCMAKE_BUILD_TYPE=Debug \
  -DQT_HOST_PATH="$QT_HOST" \
  -DC1_TLS_PLUGIN="$QT_ANDROID/plugins/tls/libplugins_tls_qopensslbackend_$ABI.so"

# gradle.properties / wrapper 由 make_apk 步骤按配置首次生成（已存在则不改）。
# 所以：先完整构建一次（若 Qt 默认 compileSdk=34 被 androidx 拒收会失败，但配置文件已生成），
# 再打补丁、再构建一次 → 确定性收敛。
cmake --build "$BUILD" || true

# Qt 默认 compileSdk=android-34，被 androidx.core:1.17.0 拒收（要求 >=36）
sed -i 's/androidCompileSdkVersion=.*/androidCompileSdkVersion=android-36/' "$BUILD/android-build/gradle.properties" 2>/dev/null || true

# C1 网络：CA 证书进 assets（Qt 的 OpenSSL 后端在 Android 上读不到系统证书库）
mkdir -p "$BUILD/android-build/assets"
if [ -f "$SRC/assets/cacert.pem" ]; then
  cp "$SRC/assets/cacert.pem" "$BUILD/android-build/assets/cacert.pem"
else
  echo "[fail] 缺 $SRC/assets/cacert.pem —— 没有它所有 HTTPS 都会 SSLErr"
  exit 1
fi
# wrapper 默认源经代理 TLS 失败 → 腾讯镜像
sed -i 's|distributionUrl=.*|distributionUrl=https\\://mirrors.cloud.tencent.com/gradle/gradle-9.5.1-bin.zip|' \
  "$BUILD/android-build/gradle/wrapper/gradle-wrapper.properties" 2>/dev/null || true

cmake --build "$BUILD"

APK=$(find "$BUILD" -name '*.apk' | head -1)
[ -n "$APK" ] || { echo "[fail] 未产出 APK"; exit 1; }
echo "APK: $APK  ($(du -m "$APK" | cut -f1) MB)"

# --- C1 网络门禁：CA 与 OpenSSL 必须真的在包里（否则 HTTPS 一定失败且难查） ---
# 注意：这里先把清单落盘再匹配。`unzip -l | grep -q` 在 `set -o pipefail` 下会因为
# grep 提前退出触发 SIGPIPE，把整条管道判成失败——库在包里也会报"没有"。
unzip -l "$APK" > "$BUILD/apk_list.txt" 2>/dev/null || true
for f in assets/cacert.pem "lib/$ABI/libssl.so" "lib/$ABI/libcrypto.so" \
         "lib/$ABI/libplugins_tls_qopensslbackend_$ABI.so"; do
  if grep -qF "$f" "$BUILD/apk_list.txt"; then
    echo "[OK] APK 含 $f"
  else
    echo "[FAIL] APK 里没有 $f —— Qt 的 TLS 后端会在运行时找不到 OpenSSL"
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
"$PY" "$REPO_WSL/python/android/tools/check_elf_page_align.py" "$APK"
# 已移除 Python 运行时，"bundle 门禁"不再适用；C1 的原生依赖只剩 libQt6* + sr_qnn/onnx + OpenSSL，
# 全部由上面的 check_elf_page_align.py（lib/ 布局）覆盖。
