#!/usr/bin/env bash
# Repack an existing APK with a freshly built libjmcomic_ui_<abi>.so.
#
# Why this exists: the normal path is `build_android.sh` (qt-cmake + ninja +
# androiddeployqt + Gradle). Gradle copies JNI libs with Files.copy(), which
# calls setLastModifiedTime(); in the current sandbox utimensat is denied, so
# `mergeReleaseJniLibFolders` fails with EPERM ("不允许的操作") no matter which
# directory the build uses. Compiling and packaging are independent, so this
# script keeps the packaging step usable: take the last good APK, replace one
# .so, re-align, re-sign.
#
# It is NOT a replacement for a real build — Qt libs and other resources come
# from the base APK. Use it only when the base APK already matches the current
# resources and the only changes are C++ code and/or a few manifest flags.
#
#   ./repack_apk.sh --abi arm64-v8a \
#       --so /home/kokomi/jm-rel-arm64/libjmcomic_ui_arm64-v8a.so \
#       --manifest /tmp/AndroidManifest.xml \
#       --base ../../dist/JMComic-ui-0.89-arm64-v8a-quic-debug.apk \
#       --out  ../../dist/JMComic-ui-0.89-arm64-v8a-quic-release.apk
#
# --manifest accepts a patched binary AXML produced by patch_apk_manifest.py.
# Either --so or --manifest (or both) is required.
#
# Env: JM_KS_FILE / JM_KS_PASS / JM_KS_ALIAS (defaults to the release keystore
# in ~/.android), ANDROID_SDK_ROOT, BUILD_TOOLS_VERSION.
set -euo pipefail

ABI=""
SO=""
MANIFEST=""
BASE=""
OUT=""
SIGN="release"
SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$HOME/.android}}"
BT_VERSION="${BUILD_TOOLS_VERSION:-37.0.0}"
NDK_ROOT="${ANDROID_NDK_HOME:-$HOME/.android/ndk/30.0.16248370}"

usage() { sed -n '2,32p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit "${1:-0}"; }

while [ $# -gt 0 ]; do
  case "$1" in
    --abi) ABI="${2:-}"; shift 2 ;;
    --so) SO="${2:-}"; shift 2 ;;
    --manifest) MANIFEST="${2:-}"; shift 2 ;;
    --base) BASE="${2:-}"; shift 2 ;;
    --out) OUT="${2:-}"; shift 2 ;;
    --debug) SIGN="debug"; shift ;;
    --release) SIGN="release"; shift ;;
    -h|--help) usage 0 ;;
    *) echo "[x] unknown option: $1" >&2; usage 2 ;;
  esac
done

[ -n "$ABI" ] || { echo "[x] --abi is required" >&2; exit 2; }
[ -n "$SO" ] || [ -n "$MANIFEST" ] || { echo "[x] --so or --manifest is required" >&2; exit 2; }
[ -z "$SO" ] || [ -f "$SO" ] || { echo "[x] no such .so: $SO" >&2; exit 2; }
[ -z "$MANIFEST" ] || [ -f "$MANIFEST" ] || { echo "[x] no such manifest: $MANIFEST" >&2; exit 2; }

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
DIST="$REPO/cpp/dist"

if [ -z "$BASE" ]; then
  # newest APK for this ABI that is not itself a repack result
  BASE="$(ls -t "$DIST"/JMComic-ui-*-"$ABI"*.apk 2>/dev/null | head -1 || true)"
fi
[ -f "${BASE:-}" ] || { echo "[x] no base APK for $ABI (pass --base)" >&2; exit 2; }

BT="$SDK_ROOT/build-tools/$BT_VERSION"
[ -x "$BT/zipalign" ] && [ -x "$BT/apksigner" ] || { echo "[x] build-tools $BT_VERSION incomplete" >&2; exit 2; }
STRIP=""
if [ -n "$SO" ]; then
  STRIP="$(ls "$NDK_ROOT"/toolchains/llvm/prebuilt/*/bin/llvm-strip 2>/dev/null | head -1)"
  [ -n "$STRIP" ] || { echo "[x] llvm-strip not found under $NDK_ROOT" >&2; exit 2; }
fi

entry=""
[ -z "$SO" ] || entry="lib/$ABI/$(basename "$SO")"
VER="$(sed -n "s/^project(.*VERSION \([0-9.]*\).*/\1/p" "$HERE/CMakeLists.txt" | head -1)"
[ -n "$VER" ] || VER="0.0"
[ -n "$OUT" ] || OUT="$DIST/JMComic-ui-$VER-$ABI-repack-$SIGN.apk"

# /tmp is tiny here and the base APK is ~62 MB, so stage next to dist instead.
# Kept between runs (no trap cleanup): bulk deletion needs extra confirmation
# in this sandbox and the next run simply overwrites.
WORK="${JM_REPACK_WORK:-$DIST/.repack-$ABI}"
mkdir -p "$WORK"

echo "==> base   $BASE"
[ -z "$SO" ] || echo "==> so     $SO ($(stat -c%s "$SO") bytes) -> $entry"
[ -z "$MANIFEST" ] || echo "==> axml   $MANIFEST ($(stat -c%s "$MANIFEST") bytes)"

cp "$BASE" "$WORK/work.apk"

if [ -n "$SO" ]; then
  mkdir -p "$WORK/lib/$ABI"
  cp "$SO" "$WORK/$entry"
  "$STRIP" --strip-unneeded "$WORK/$entry"
  echo "    stripped -> $(stat -c%s "$WORK/$entry") bytes"
  # -0 (store) is mandatory: the manifest declares extractNativeLibs=false, so
  # the loader mmaps the .so straight out of the APK; a deflated entry fails
  # install with INSTALL_FAILED_INVALID_APK "Failed to extract native libraries".
  ( cd "$WORK" && zip -u -X -0 -q work.apk "$entry" )
  # 不要用 `unzip -v ... | grep -q " 0%"`：set -o pipefail 下 grep -q 命中第一行
  # 就退出，unzip 收到 SIGPIPE 会把整个管道判成失败 —— .so 明明是 Stored
  # 却误报 "deflated"（0.90 实测踩过）。改成 shell 模式匹配，无管道。
  zipinfo="$(unzip -v "$WORK/work.apk" "$entry")"
  case "$zipinfo" in
    *" 0%"*) echo "    zip method: stored (0%)" ;;
    *) echo "[x] $entry is deflated; the loader would fail at install" >&2; exit 1 ;;
  esac
fi

if [ -n "$MANIFEST" ]; then
  cp "$MANIFEST" "$WORK/AndroidManifest.xml"
  # The original entry is deflated; keep it that way (only .so must be STORED).
  ( cd "$WORK" && zip -u -X -q work.apk AndroidManifest.xml )
  echo "    AndroidManifest.xml replaced ($(unzip -p "$WORK/work.apk" AndroidManifest.xml | wc -c) bytes)"
fi

# -P 16：.so 条目按 16 KB 对齐（Android 15+ 16 KB 页设备要求；4 KB 是历史默认，
# 会触发"应用兼容性提醒"）。裸 4 仍对齐其它 STORED 条目（resources.arsc 等）。
"$BT/zipalign" -f -P 16 4 "$WORK/work.apk" "$WORK/aligned.apk" || { echo "[x] zipalign failed" >&2; exit 1; }
"$BT/zipalign" -c -P 16 4 "$WORK/aligned.apk" || { echo "[x] zipalign check failed (not 16 KB aligned)" >&2; exit 1; }

if [ "$SIGN" = "release" ]; then
  KS="${JM_KS_FILE:-$HOME/.android/jmcomic-release.keystore}"
  ALIAS="${JM_KS_ALIAS:-jmcomic}"
  PASS="${JM_KS_PASS:-}"
  [ -n "$PASS" ] || PASS="$(cat "$HOME/.android/jmcomic-release.pass" 2>/dev/null || true)"
  [ -f "$KS" ] || { echo "[x] release keystore missing: $KS" >&2; exit 2; }
  [ -n "$PASS" ] || { echo "[x] release password missing (JM_KS_PASS)" >&2; exit 2; }
else
  # 与 build_android.sh 一致：随仓 dev keystore，跨机器签名一致（升级不丢数据）。
  KS="$HERE/keys/jm-dev.keystore"; ALIAS="jmdev"; PASS="android"
  [ -f "$KS" ] || { echo "[x] committed dev keystore missing: $KS" >&2; exit 2; }
fi

"$BT/apksigner" sign --ks "$KS" --ks-pass "pass:$PASS" --key-pass "pass:$PASS" \
  --ks-key-alias "$ALIAS" --out "$OUT" "$WORK/aligned.apk" 2>/dev/null
"$BT/apksigner" verify "$OUT" 2>/dev/null

echo "==> signed ($SIGN, alias $ALIAS)"
echo "    $OUT ($(du -h "$OUT" | cut -f1))"
"$BT/aapt2" dump badging "$OUT" 2>/dev/null | grep -E "^(package|native-code)" | sed 's/^/    /'
