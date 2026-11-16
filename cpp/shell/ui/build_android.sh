#!/usr/bin/env bash
# Build (and optionally deploy/test) the C++ UI APK on a Linux host.
#
# This is the Linux counterpart of build_android.ps1. It exists because the Qt
# Android kit + SDK + NDK can all live on Linux, and the QUIC dependencies for
# Android are built here too (cpp/shell/quic/tools/build_quic_deps_android.sh).
#
#   ./build_android.sh                       # x86_64; QUIC/ECH auto (deps present -> on)
#   ./build_android.sh --abi arm64-v8a       # phone build
#   ./build_android.sh --with-quic           # force HTTP/3 + ECH (fails if deps missing)
#   ./build_android.sh --no-quic             # force them off
#   ./build_android.sh --deploy --test       # build + install on AVD + smoke test
#   ./build_android.sh --clean               # wipe the build dir first
#   ./build_android.sh --abi arm64-v8a --with-quic --release   # signed release APK
#
# What the script checks before doing anything (fail fast, one line each):
#   1. Qt Android kit for the requested ABI + host Qt (androiddeployqt needs it)
#   2. ninja / cmake / qt-cmake
#   3. JDK (Gradle 8.x cannot run on JDK > 24 -- the script prefers a <=24 JDK)
#   4. Android SDK: platform android-34, build-tools (zipalign + apksigner)
#   5. Android NDK (r26+)
#   6. OpenSSL 3.x Android libs (Qt's TLS plugin dlopens libssl_3/libcrypto_3)
#   7. OpenSSL 4.x + ngtcp2/nghttp3 per-ABI deps -- auto-detected; --with-quic
#      turns a missing dep into an error instead of a silent "ech=0" build.
#
# After packaging the APK is inspected: with QUIC on, libssl.so must export
# SSL_set1_ech_config_list / SSL_set_quic_tls_cbs and the app library must
# contain the real H3 backend, otherwise the build fails.
#
# Output: cpp/dist/JMComic-ui-<version>-<abi>[-quic]-{debug,release}.apk
#
# Restricted networks (proxy with a private CA, or slow links):
#   JAVA_TOOL_OPTIONS="-Djavax.net.ssl.trustStore=/etc/pki/ca-trust/extracted/java/cacerts \
#                      -Djavax.net.ssl.trustStorePassword=changeit"
#   GRADLE_RO_DEP_CACHE=/path/to/another/.gradle/caches   # reuse jars read-only
# A Gradle distribution already present in ~/.gradle/wrapper/dists is reused as is.

set -euo pipefail

# ---------------------------------------------------------------------------
# defaults (override with env vars)
# ---------------------------------------------------------------------------
ABI="x86_64"
WITH_QUIC="auto"    # auto | 1 | 0
SIGN_RELEASE=0
CLEAN=0
DEPLOY=0
RUN_TEST=0
AVD_NAME=""
API_LEVEL="${API_LEVEL:-34}"
# Gradle 9.x can run on JDK 25 but not on 27 ("Unsupported class file major
# version 71" comes from Groovy parsing the JDK's own classes).
MAX_JAVA="${MAX_JAVA:-25}"

QT_ROOT="${QT_ROOT:-$HOME/Qt/6.12.0}"
SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/.android}}"
NDK_ROOT="${ANDROID_NDK_HOME:-}"
DEPS_ROOT="${JM_DEPS:-/run/media/kokomi/Windows/Users/WQ159/jmcomic-build/deps}"
BUILD_TOOLS_VERSION="${BUILD_TOOLS_VERSION:-37.0.0}"
APP_ID="org.jmcomic.ui"
ACTIVITY="$APP_ID/org.qtproject.qt.android.bindings.QtActivity"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
DIST="$REPO/cpp/dist"

usage() {
  awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "${BASH_SOURCE[0]}"
  exit "${1:-0}"
}

while [ $# -gt 0 ]; do
  case "$1" in
    --abi) ABI="${2:-}"; shift 2 ;;
    --with-quic) WITH_QUIC=1; shift ;;
    --no-quic) WITH_QUIC=0; shift ;;
    --release) SIGN_RELEASE=1; shift ;;
    --clean) CLEAN=1; shift ;;
    --deploy) DEPLOY=1; shift ;;
    --test) RUN_TEST=1; DEPLOY=1; shift ;;
    --avd) AVD_NAME="${2:-}"; shift 2 ;;
    -h|--help) usage 0 ;;
    *) echo "[x] unknown option: $1" >&2; usage 2 ;;
  esac
done

case "$ABI" in
  x86_64)   QT_KIT="android_x86_64" ;;
  arm64-v8a) QT_KIT="android_arm64_v8a" ;;
  *) echo "[x] unsupported ABI: $ABI (x86_64 | arm64-v8a)" >&2; exit 2 ;;
esac

# Gradle 需要可写的 GRADLE_USER_HOME。本机可能以 root 运行但 /root 不可写
# （沙箱），而真正带缓存的 .gradle 在 /home/<user> 下——直接让 gradle 用 /root
# 会报 "Could not create parent directory for lock file"。
if [ -z "${GRADLE_USER_HOME:-}" ]; then
  if mkdir -p "${HOME:-/nonexistent}/.gradle" 2>/dev/null \
     && [ -w "${HOME:-/nonexistent}/.gradle" ]; then
    GRADLE_USER_HOME="$HOME/.gradle"
  else
    for d in /home/*/.gradle; do
      if [ -d "$d/wrapper/dists" ] && [ -w "$d" ]; then
        GRADLE_USER_HOME="$d"
        break
      fi
    done
  fi
fi
export GRADLE_USER_HOME="${GRADLE_USER_HOME:-$HERE/.gradle-local}"
mkdir -p "$GRADLE_USER_HOME" 2>/dev/null || die "cannot create GRADLE_USER_HOME: $GRADLE_USER_HOME"

BUILD="${JM_BUILD_DIR:-$HERE/build-android-$ABI}"   # NTFS is slow; point this at a local disk
if [ "$CLEAN" = "1" ] && [ -d "$BUILD" ]; then
  rm -rf "$BUILD"
fi
QT_ANDROID="$QT_ROOT/$QT_KIT"
QT_HOST="$QT_ROOT/gcc_64"
NINJA="$QT_ROOT/../Tools/Ninja/ninja"
[ -x "$NINJA" ] || NINJA="$(command -v ninja || true)"

C_OK=$'\033[32m'; C_BAD=$'\033[31m'; C_DIM=$'\033[2m'; C_END=$'\033[0m'
ok()   { printf "  ${C_OK}ok${C_END}   %s\n" "$1"; }
bad()  { printf "  ${C_BAD}FAIL${C_END} %s\n" "$1"; }
step() { printf "\n==> %s\n" "$1"; }
die()  { printf "${C_BAD}[x]${C_END} %s\n" "$1" >&2; exit 1; }
note() { printf "  ${C_DIM}%s${C_END}\n" "$1"; }

# ---------------------------------------------------------------------------
# 1. environment checks
# ---------------------------------------------------------------------------
step "environment"

[ -d "$QT_ANDROID" ] || die "Qt Android kit not found: $QT_ANDROID"
[ -x "$QT_ANDROID/bin/qt-cmake" ] || die "qt-cmake missing in $QT_ANDROID"
ok "Qt Android kit: $QT_ANDROID"

[ -d "$QT_HOST" ] || die "host Qt not found: $QT_HOST (androiddeployqt runs on the host)"
[ -x "$QT_HOST/bin/androiddeployqt" ] || die "androiddeployqt missing in $QT_HOST"
ok "host Qt: $QT_HOST"

[ -n "$NINJA" ] && [ -x "$NINJA" ] || die "ninja not found (set PATH or install ninja-build)"
ok "ninja: $NINJA"

# Prefer an explicit JAVA_HOME, then any JDK <= MAX_JAVA, and only fall back to
# the default java with a warning.
java_major() {
  "$1" -version 2>&1 | awk -F'"' '/version/{print $2}' | awk -F. '{print ($1=="1"?$2:$1)}'
}
JAVA_CANDIDATE="${JAVA_HOME:+$JAVA_HOME/bin/java}"
if [ -n "$JAVA_CANDIDATE" ] && { [ ! -x "$JAVA_CANDIDATE" ] || [ "$(java_major "$JAVA_CANDIDATE")" -gt "$MAX_JAVA" ]; }; then
  note "JAVA_HOME ($JAVA_HOME) is too new for Gradle, looking for a <=$MAX_JAVA JDK"
  JAVA_CANDIDATE=""
fi
if [ -z "$JAVA_CANDIDATE" ]; then
  for d in "$HOME"/.jdks/* /usr/lib/jvm/* "$HOME"/jdks/*; do
    [ -x "$d/bin/java" ] || continue
    if [ -x "$d/bin/javac" ] && [ "$(java_major "$d/bin/java")" -le "$MAX_JAVA" ]; then
      JAVA_CANDIDATE="$d/bin/java"; JAVA_HOME="$d"; break
    fi
  done
fi
if [ -z "$JAVA_CANDIDATE" ]; then
  JAVA_CANDIDATE="$(command -v java || true)"
  [ -n "$JAVA_CANDIDATE" ] || die "no JDK found (Gradle needs one; JDK 17-24 recommended)"
  note "WARNING: only JDK $(java_major "$JAVA_CANDIDATE") found; Gradle may refuse to start"
fi
export JAVA_HOME="${JAVA_HOME:-$(dirname "$(dirname "$JAVA_CANDIDATE")")}"
# A JRE-only install (Fedora's java-25-openjdk has no javac) fails later with
# "Toolchain installation ... does not provide [JAVA_COMPILER]"; catch it here.
[ -x "$JAVA_HOME/bin/javac" ] || die "JAVA_HOME has no javac: $JAVA_HOME (install a full JDK, 17-25)"
ok "java: $JAVA_CANDIDATE ($(java_major "$JAVA_CANDIDATE")) + javac"

[ -d "$SDK_ROOT/platforms/android-$API_LEVEL" ] || die "SDK platform missing: $SDK_ROOT/platforms/android-$API_LEVEL"
ok "SDK platform: android-$API_LEVEL"
BT="$SDK_ROOT/build-tools/$BUILD_TOOLS_VERSION"
[ -x "$BT/zipalign" ] && [ -x "$BT/apksigner" ] || die "build-tools $BUILD_TOOLS_VERSION incomplete (need zipalign + apksigner)"
ok "build-tools: $BUILD_TOOLS_VERSION"

if [ -z "$NDK_ROOT" ]; then
  NDK_ROOT="$(ls -d "$SDK_ROOT"/ndk/* 2>/dev/null | sort -V | tail -1 || true)"
fi
[ -n "$NDK_ROOT" ] && [ -d "$NDK_ROOT" ] || die "Android NDK not found under $SDK_ROOT/ndk"
ok "NDK: $NDK_ROOT"

# Qt's TLS backend on Android dlopens libssl_3.so / libcrypto_3.so.
OSSL3="$DEPS_ROOT/openssl/$ABI"
[ -f "$OSSL3/libssl.so" ] || die "OpenSSL 3.x for Android missing: $OSSL3/libssl.so"
ok "OpenSSL 3.x (Qt TLS): $OSSL3"

OSSL4="$DEPS_ROOT/openssl41-android/$ABI"
QUIC_DEPS="$DEPS_ROOT/quic-android/$ABI"
# ECH and HTTP/3 need the per-ABI OpenSSL 4 + ngtcp2/nghttp3 tree. Default is
# AUTO: use it when complete, otherwise build without (and say so loudly -- a
# silent fallback is exactly how the phone ended up with ech=0 / http3=0).
QUIC_READY=1
for f in "$OSSL4/lib/libssl.so" "$OSSL4/lib/libcrypto.so" \
         "$QUIC_DEPS/lib/libngtcp2.a" "$QUIC_DEPS/lib/libngtcp2_crypto_ossl.a" \
         "$QUIC_DEPS/lib/libnghttp3.a"; do
  [ -f "$f" ] || QUIC_READY=0
done
case "$WITH_QUIC" in
  auto)
    if [ "$QUIC_READY" = "1" ]; then
      WITH_QUIC=1
      ok "QUIC/ECH: auto ON ($OSSL4)"
    else
      WITH_QUIC=0
      note "QUIC/ECH: auto OFF -- per-ABI deps missing under $OSSL4 / $QUIC_DEPS"
      note "  HTTP/3 and ECH will NOT be available in this APK"
    fi
    ;;
  1)
    [ "$QUIC_READY" = "1" ] || die "QUIC dependency missing under $OSSL4 / $QUIC_DEPS (run cpp/shell/quic/tools/build_quic_deps_android.sh $ABI)"
    ok "QUIC/ECH: ON ($OSSL4)"
    ;;
  0)
    note "QUIC/ECH: OFF (--no-quic)"
    ;;
esac

VERSION="$(sed -n 's/^project(JmComicUi VERSION \([0-9.]*\).*/\1/p' "$HERE/CMakeLists.txt")"
[ -n "$VERSION" ] || die "cannot read project version from CMakeLists.txt"
ok "project version: $VERSION"

# ---------------------------------------------------------------------------
# 2. dependency staging (Qt TLS plugin + extra .so)
# ---------------------------------------------------------------------------
step "stage dependencies"
STAGE="$BUILD/staging"
mkdir -p "$STAGE"

# Two OpenSSL builds cannot coexist in one APK even under different file names:
# libssl_3.so's DT_NEEDED is still "libcrypto.so", so the loader binds it to the
# 4.x libcrypto and dies on missing symbols (实测: cannot locate "X509_NAME_free").
# With QUIC enabled we therefore ship OpenSSL 4 only, and let it also serve as
# Qt's TLS backend (staged under the _3 aliases). Without QUIC we keep 3.x.
if [ "$WITH_QUIC" = "1" ]; then
  TLS_SRC="$OSSL4/lib"
else
  TLS_SRC="$OSSL3"
fi
cp -f "$TLS_SRC/libssl.so"    "$STAGE/libssl_3.so"
cp -f "$TLS_SRC/libcrypto.so" "$STAGE/libcrypto_3.so"
TLS_PLUGIN="$QT_ANDROID/plugins/tls/libplugins_tls_qopensslbackend_$ABI.so"
[ -f "$TLS_PLUGIN" ] || die "Qt TLS plugin missing: $TLS_PLUGIN"

EXTRA_LIST=("$STAGE/libssl_3.so" "$STAGE/libcrypto_3.so" "$TLS_PLUGIN")
if [ "$WITH_QUIC" = "1" ]; then
  # 4.x takes the unversioned names; Qt keeps its _3 aliases. Both sets coexist.
  EXTRA_LIST+=("$OSSL4/lib/libssl.so" "$OSSL4/lib/libcrypto.so")
else
  cp -f "$TLS_SRC/libssl.so"    "$STAGE/libssl.so"
  cp -f "$TLS_SRC/libcrypto.so" "$STAGE/libcrypto.so"
  EXTRA_LIST+=("$STAGE/libssl.so" "$STAGE/libcrypto.so")
fi
EXTRA="$(IFS=';'; echo "${EXTRA_LIST[*]}")"
ok "extra libs: $(echo "$EXTRA" | tr ';' '\n' | wc -l) files"

# ---------------------------------------------------------------------------
# 3. configure + build
# ---------------------------------------------------------------------------
step "prepare CA bundle"
# QUIC 走自建 OpenSSL，不认 Qt 的证书加载；Android 也没有 OpenSSL 默认路径。
# 把宿主的 CA bundle 放进构建目录，CMake 会把它编成 :/tls/cacert.pem。
CA_PEM=""
for c in /etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem \
         /etc/ssl/certs/ca-certificates.crt /etc/ssl/cert.pem; do
  [ -f "$c" ] && { CA_PEM="$c"; break; }
done
if [ -n "$CA_PEM" ]; then
  mkdir -p "$BUILD/generated"
  cp -f "$CA_PEM" "$BUILD/generated/cacert.pem"
  ok "CA bundle: $CA_PEM -> $BUILD/generated/cacert.pem ($(grep -c 'BEGIN CERTIFICATE' "$CA_PEM") certs)"
else
  note "no host CA bundle found; QUIC on Android will rely on system paths"
fi

step "configure ($ABI)"
"$QT_ANDROID/bin/qt-cmake" -S "$HERE" -B "$BUILD" -G Ninja \
  "-DCMAKE_MAKE_PROGRAM=$NINJA" \
  "-DANDROID_ABI=$ABI" \
  "-DANDROID_PLATFORM=android-$API_LEVEL" \
  "-DANDROID_SDK_ROOT=$SDK_ROOT" \
  "-DANDROID_NDK_ROOT=$NDK_ROOT" \
  "-DCMAKE_BUILD_TYPE=Release" \
  "-DQT_HOST_PATH=$QT_HOST" \
  "-DUI_ANDROID_EXTRA_LIBS=$EXTRA" \
  "-DJM_WITH_QUIC=$WITH_QUIC" \
  "-DJM_QUIC_ROOT=$QUIC_DEPS" \
  "-DJM_OPENSSL41_ROOT=$OSSL4"
ok "configured"

step "build + package"
LOG="$BUILD/build.log"
# gradle keeps stdout open; redirect and show the tail on failure
if ! "$NINJA" -C "$BUILD" > "$LOG" 2>&1; then
  tail -40 "$LOG" >&2
  die "build failed (full log: $LOG)"
fi
tail -3 "$LOG" | sed 's/^/    /'

UNSIGNED="$BUILD/android-build/build/outputs/apk/release/android-build-release-unsigned.apk"
[ -f "$UNSIGNED" ] || die "no unsigned APK: $UNSIGNED"

# ---------------------------------------------------------------------------
# 4. align + sign
# ---------------------------------------------------------------------------
step "sign"
mkdir -p "$DIST"
# 带 QUIC 的包单独命名，否则两次构建会互相覆盖
QUIC_TAG=""
[ "$WITH_QUIC" = "1" ] && QUIC_TAG="-quic"
ALIGNED="$BUILD/android-build-release-aligned.apk"

if [ "$SIGN_RELEASE" = "1" ]; then
  SIGN_TAG="release"
  KS="${JM_KS_FILE:-$HOME/.android/jmcomic-release.keystore}"
  KS_ALIAS="${JM_KS_ALIAS:-jmcomic}"
  KS_PASS="${JM_KS_PASS:-}"
  if [ -z "$KS_PASS" ] && [ -f "$HOME/.android/jmcomic-release.pass" ]; then
    KS_PASS="$(cat "$HOME/.android/jmcomic-release.pass")"
  fi
  [ -f "$KS" ] || die "release keystore not found: $KS (set JM_KS_FILE)"
  [ -n "$KS_PASS" ] || die "release password missing: set JM_KS_PASS or ~/.android/jmcomic-release.pass"
else
  # 签名稳定性（升级丢数据根因之一，见 docs/data_persistence_upgrade_analysis.md
  # §2.2/§3.4）：~/.android/debug.keystore 每台机器都不一样，Windows 构建的包
  # 与 Linux 沙箱构建的包互相覆盖安装必然 INSTALL_FAILED_UPDATE_INCOMPATIBLE，
  # 现场被迫卸载重装 -> /data/data/<pkg> 连同登录态一起蒸发。
  # 改用随仓 dev keystore：任何机器出的 debug 包签名一致，`install -r` 永远可覆盖。
  # 注意：签名变更只发生一次——从旧的机器级 debug key 换到 jm-dev 时，已装设备
  # 需要卸载一次（先 --export-data！），此后 debug<->debug 升级不再丢数据。
  SIGN_TAG="debug"
  KS="$HERE/keys/jm-dev.keystore"
  KS_ALIAS="jmdev"
  KS_PASS="android"
  [ -f "$KS" ] || die "committed dev keystore missing: $KS"
fi
APK="$DIST/JMComic-ui-$VERSION-$ABI$QUIC_TAG-$SIGN_TAG.apk"

# -P 16：.so 条目按 16 KB 页对齐（Android 15+）。历史默认 4 KB（zipalign -p /
# AGP）在 16 KB 设备上触发"应用兼容性提醒"。裸 4 仍对齐其它 STORED 条目。
"$BT/zipalign" -f -P 16 4 "$UNSIGNED" "$ALIGNED"
# PKCS12 的 storepass 与 keypass 必须相同，所以两处都用 $KS_PASS
"$BT/apksigner" sign --ks "$KS" --ks-pass "pass:$KS_PASS" --key-pass "pass:$KS_PASS" \
  --ks-key-alias "$KS_ALIAS" --out "$APK" "$ALIGNED"
"$BT/apksigner" verify "$APK" > /dev/null || die "signature verify failed"
# 签名后的正式门禁：zip 偏移必须 16 KB 对齐（-c 检查模式，失败即出不了包）
"$BT/zipalign" -c -P 16 4 "$APK" || die "APK zip offsets not 16 KB aligned"
ok "signed with $SIGN_TAG key (alias $KS_ALIAS): $KS"
ok "APK: $APK ($(du -h "$APK" | cut -f1))"

echo ""
echo "    badging:"
AAPT="$BT/aapt2"
if [ -x "$AAPT" ]; then
  "$AAPT" dump badging "$APK" 2>/dev/null | grep -E "^(package|sdkVersion|targetSdkVersion|native-code)" | sed 's/^/      /'
fi

# 16 KB page alignment gate (Android 15+): every LOAD segment must be 0x4000
# or larger (0x10000 is fine). ELF p_align 检查只覆盖 ELF 维度；zip 偏移由上面
# 的 `zipalign -c -P 16` 门禁负责——两个维度都过才算 16 KB 兼容。
READELF="$(ls "$NDK_ROOT"/toolchains/llvm/prebuilt/*/bin/llvm-readelf 2>/dev/null | head -1)"
if [ -n "$READELF" ] && command -v unzip > /dev/null; then
  TMPD="$(mktemp -d)"
  unzip -q "$APK" "lib/*/*.so" -d "$TMPD" || true
  BAD_ALIGN=0
  for so in "$TMPD"/lib/*/*.so; do
    [ -f "$so" ] || continue
    # 可移植的 hex->dec：BUSY/gawk 通用（strtonum 不是 POSIX awk 的）
    MISALIGNED="$("$READELF" -lW "$so" 2>/dev/null | awk '
      /LOAD/ { h=$NF; sub(/^0x/,"",h); d=0
               for (i=1; i<=length(h); i++) { c=tolower(substr(h,i,1)); d=d*16+index("0123456789abcdef",c)-1 }
               if (d < 16384) print d }')"
    if [ -n "$MISALIGNED" ]; then
      bad "not 16 KB aligned (LOAD <$MISALIGNED): $(basename "$so")"; BAD_ALIGN=1
    fi
  done
  rm -rf "$TMPD"
  [ "$BAD_ALIGN" = "0" ] || die "16 KB page alignment failed; rebuild with -Wl,-z,max-page-size=16384 (NDK r27+) or repackage"
  ok "16 KB page alignment: all packaged .so use >= 0x4000"
fi

# ---------------------------------------------------------------------------
# 4b. ECH / HTTP3 gate: symbols must be inside the APK, not only in deps/
# ---------------------------------------------------------------------------
step "verify ECH/HTTP3 in APK"
if ! command -v unzip > /dev/null; then
  note "unzip not available -- skipping APK content check"
else
  SSL_OK=0; QUICTLS_OK=0; H3_OK=0
  if unzip -p "$APK" "lib/$ABI/libssl.so" 2>/dev/null | grep -qa "SSL_set1_ech_config_list"; then
    SSL_OK=1; ok "libssl.so exports SSL_set1_ech_config_list"
  else
    bad "libssl.so has no ECH export"
  fi
  if unzip -p "$APK" "lib/$ABI/libssl.so" 2>/dev/null | grep -qa "SSL_set_quic_tls_cbs"; then
    QUICTLS_OK=1; ok "libssl.so exports SSL_set_quic_tls_cbs"
  else
    bad "libssl.so has no QUIC-TLS callback API"
  fi
  # QStringLiteral lives as UTF-16LE: drop NULs, then the ASCII text is greppable.
  UI_SO="$(unzip -Z1 "$APK" | grep -m1 "^lib/$ABI/libjmcomic_ui.*\.so$" || true)"
  if [ -n "$UI_SO" ] && unzip -p "$APK" "$UI_SO" | tr -d '\000' \
       | grep -qa "ngtcp2 %1 / nghttp3 %2 / %3"; then
    H3_OK=1; ok "app library contains the ngtcp2/nghttp3 backend"
  else
    bad "app library was built with the QUIC stub"
  fi

  if [ "$WITH_QUIC" = "1" ] && [ "$SSL_OK$QUICTLS_OK$H3_OK" != "111" ]; then
    die "APK would report ech=0 / http3=0; rebuild after cpp/shell/quic/tools/build_quic_deps_android.sh $ABI"
  fi
  if [ "$WITH_QUIC" = "0" ]; then
    note "QUIC/ECH intentionally disabled for this build"
  fi
fi

# ---------------------------------------------------------------------------
# 5. deploy + smoke test (optional)
# ---------------------------------------------------------------------------
if [ "$DEPLOY" = "1" ]; then
  step "deploy"
  ADB="${ADB:-$(command -v adb || echo "$SDK_ROOT/platform-tools/adb")}"
  [ -x "$ADB" ] || die "adb not found"
  if [ -n "$AVD_NAME" ]; then
    export ANDROID_AVD_HOME="${ANDROID_AVD_HOME:-$HOME/.android/avd}"
    if ! "$ADB" devices | grep -q "emulator.*device$"; then
      note "starting AVD: $AVD_NAME"
      nohup "$SDK_ROOT/emulator/emulator" -avd "$AVD_NAME" -no-snapshot-save \
        -no-audio -gpu swiftshader_indirect > "$BUILD/emulator.log" 2>&1 &
    fi
  fi
  "$ADB" wait-for-device
  # wait for full boot
  for i in $(seq 1 90); do
    [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] && break
    sleep 2
  done
  [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ] || die "emulator did not boot"
  ok "device ready: $("$ADB" shell getprop ro.product.model | tr -d '\r') ($("$ADB" shell getprop ro.product.cpu.abi | tr -d '\r'))"

  "$ADB" install -r -t "$APK" | sed 's/^/    /'
  "$ADB" shell am force-stop "$APP_ID" || true
  "$ADB" logcat -c || true
  "$ADB" shell am start -W -n "$ACTIVITY" | sed 's/^/    /'
  ok "launched"
fi

if [ "$RUN_TEST" = "1" ]; then
  step "smoke test"
  PY="${PYTHON:-python3}"
  "$PY" "$REPO/cpp/tools/c1_ui_android_smoke.py" --apk "$APK"
fi

echo ""
echo "[ok] done: $APK"
