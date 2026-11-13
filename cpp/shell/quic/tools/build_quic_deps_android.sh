#!/usr/bin/env bash
# Build the Android QUIC dependencies (A1 + A2) from source.
#
#   A1: OpenSSL 4.1 -> libssl.so / libcrypto.so with ECH + QUIC-TLS symbols
#   A2: nghttp3 + ngtcp2 (static, crypto bound to the A1 build)
#
# This is the Linux counterpart of build_quic_deps.ps1. It exists because the
# Windows host cannot run OpenSSL's POSIX configure: `android-*` targets require
# a Unix perl and forward-slash NDK paths, while Win32 canonpath yields
# backslashes (see docs/cpp_ui_next_steps.md, A1 notes).
#
# Usage:
#   ./build_quic_deps_android.sh                     # both ABIs, default paths
#   ./build_quic_deps_android.sh arm64-v8a           # one ABI
#   NDK=/path/to/ndk OUT=/path/to/deps ./build_quic_deps_android.sh
#
# Output layout (mirrors the desktop deps tree so the APK build can just point
# JM_QUIC_ROOT / JM_OPENSSL41_ROOT at the per-ABI directory):
#   $OUT/openssl41-android/<abi>/{include,lib}
#   $OUT/quic-android/<abi>/{include,lib}
#
# Requirements: Linux host, perl, make, cmake >= 3.20, ninja, an NDK with
# linux-x86_64 prebuilt toolchain, and the OpenSSL 4.1 + nghttp3 + ngtcp2 sources.

set -euo pipefail

ABIS=("${@:-}")
if [ -z "${ABIS[0]:-}" ]; then
  ABIS=(arm64-v8a x86_64)
fi

NDK="${NDK:-$HOME/.android/ndk/30.0.16248370}"
OUT="${OUT:-/run/media/kokomi/Windows/Users/WQ159/jmcomic-build/deps}"
OSSL_SRC="${OSSL_SRC:-/run/media/kokomi/Windows/Users/WQ159/source/repos/openssl-4.1.0}"
NGHTTP3_SRC="${NGHTTP3_SRC:-/run/media/kokomi/Windows/Users/WQ159/nghttp3}"
NGTCP2_SRC="${NGTCP2_SRC:-/run/media/kokomi/Windows/Users/WQ159/ngtcp2}"
WORK="${WORK:-$HOME/jm-android-quic}"
API="${API:-34}"
JOBS="${JOBS:-$(nproc)}"

TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
[ -x "$TOOLCHAIN/bin/clang" ] || { echo "[x] NDK toolchain not found: $TOOLCHAIN" >&2; exit 2; }
[ -f "$OSSL_SRC/Configure" ] || { echo "[x] OpenSSL source not found: $OSSL_SRC" >&2; exit 2; }

# Map the Android ABI to the OpenSSL configure target name.
ossl_target() {
  case "$1" in
    arm64-v8a) echo android-arm64 ;;
    x86_64)    echo android-x86_64 ;;
    *) echo "[x] unsupported ABI: $1" >&2; exit 2 ;;
  esac
}

build_openssl() {
  local abi="$1" target; target="$(ossl_target "$abi")"
  local b="$WORK/openssl-$abi"
  local prefix="$OUT/openssl41-android/$abi"
  echo "==> [A1] OpenSSL 4.1 ($abi)"
  rm -rf "$b"; mkdir -p "$b"
  (
    cd "$b"
    export ANDROID_NDK_ROOT="$NDK"
    export PATH="$TOOLCHAIN/bin:$PATH"
    # NOTE: do not pass -D__ANDROID_API__ here. The NDK ships per-API wrappers
    # (aarch64-linux-android34-clang) which already define it; passing it again
    # triggers a redefinition warning in every translation unit.
    #
    # enable-ech / enable-quic are spelled out on purpose: both are default-on in
    # OpenSSL 4.x, but they cascade off with no-bulk/no-dgram/no-tls1_3 and some
    # packagers ship no-ech. A "successful" build without them links fine and
    # only fails at runtime (ech=0, no SSL_set_quic_tls_cbs for ngtcp2).
    perl "$OSSL_SRC/Configure" "$target" \
      --prefix="$prefix" \
      no-tests no-apps no-docs shared \
      enable-ech enable-quic \
      > configure.log 2>&1
    make -j"$JOBS" > build.log 2>&1
    make install_sw >> build.log 2>&1
  )
  # Android builds install an unversioned libssl.so (no SHLIB_VERSION suffix).
  # Keep a libX.so.4 alias as well: if a future OpenSSL adds the suffix, the
  # loader still resolves both spellings inside the APK.
  local libdir="$prefix/lib"
  for n in ssl crypto; do
    if [ -f "$libdir/lib$n.so" ] && [ ! -e "$libdir/lib$n.so.4" ]; then
      ln -sf "lib$n.so" "$libdir/lib$n.so.4"
    fi
    [ -f "$libdir/lib$n.so" ] || { echo "[x] $abi: lib$n.so missing" >&2; exit 1; }
  done
}

build_h3() {
  local abi="$1"
  local b="$WORK/nghttp3-$abi"
  local prefix="$OUT/quic-android/$abi"
  echo "==> [A2] nghttp3 ($abi)"
  rm -rf "$b"
  cmake -S "$NGHTTP3_SRC" -B "$b" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DENABLE_LIB_ONLY=ON -DBUILD_SHARED_LIBS=OFF \
    > "$WORK/nghttp3-$abi-configure.log" 2>&1
  cmake --build "$b" --target install >> "$WORK/nghttp3-$abi-configure.log" 2>&1
  [ -f "$prefix/lib/libnghttp3.a" ] || { echo "[x] $abi: libnghttp3.a missing" >&2; exit 1; }
}

build_tcp2() {
  local abi="$1"
  local b="$WORK/ngtcp2-$abi"
  local prefix="$OUT/quic-android/$abi"
  local ossl="$OUT/openssl41-android/$abi"
  echo "==> [A2] ngtcp2 ($abi)"
  rm -rf "$b"
  # -mavx2 is deliberately NOT passed: the x86_64 emulator does not guarantee
  # AVX2 and arm64 has no such flag (see T1 notes).
  cmake -S "$NGTCP2_SRC" -B "$b" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$abi" -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DENABLE_LIB_ONLY=ON -DBUILD_SHARED_LIBS=OFF \
    -DENABLE_OPENSSL=ON \
    -DOPENSSL_ROOT_DIR="$ossl" \
    -DOPENSSL_INCLUDE_DIR="$ossl/include" \
    -DOPENSSL_SSL_LIBRARY="$ossl/lib/libssl.so" \
    -DOPENSSL_CRYPTO_LIBRARY="$ossl/lib/libcrypto.so" \
    > "$WORK/ngtcp2-$abi-configure.log" 2>&1
  # Validation: the OpenSSL build must expose the QUIC-TLS callback API.
  grep -q "Looking for SSL_set_quic_tls_cbs - found" "$WORK/ngtcp2-$abi-configure.log" || {
    echo "[x] $abi: OpenSSL has no SSL_set_quic_tls_cbs (not a QUIC-TLS build)" >&2
    exit 1
  }
  cmake --build "$b" --target install >> "$WORK/ngtcp2-$abi-configure.log" 2>&1
  for lib in libngtcp2.a libngtcp2_crypto_ossl.a; do
    [ -f "$prefix/lib/$lib" ] || { echo "[x] $abi: $lib missing" >&2; exit 1; }
  done
}

# The deps tree lives on an NTFS mount (fuse). Freshly installed archives can be
# visible a moment later than the install command returns, so every check below
# retries instead of failing on the first look.
retry_grep() {
  local pattern="$1" file="$2" tries="${3:-5}"
  local i
  for i in $(seq 1 "$tries"); do
    grep -q -- "$pattern" "$file" && return 0
    sleep 1
  done
  return 1
}

retry_has_output() {
  local cmd_out="$1" tries="${2:-5}"
  local i n
  for i in $(seq 1 "$tries"); do
    n=$(printf '%s' "$cmd_out" | grep -c . || true)
    [ "$n" -gt 0 ] && return 0
    sleep 1
  done
  return 1
}

verify() {
  local abi="$1"
  echo "==> verify ($abi)"
  local ossl="$OUT/openssl41-android/$abi/lib"
  local quic="$OUT/quic-android/$abi/lib"
  local nm="$TOOLCHAIN/bin/llvm-nm"
  local so="$ossl/libssl.so"
  [ -f "$so" ] || so="$ossl/libssl.so.4"

  for sym in SSL_set_quic_tls_cbs SSL_set1_ech_config_list SSL_ech_set1_server_names \
             SSL_set_quic_tls_early_data_enabled; do
    local out i
    for i in 1 2 3 4 5; do
      out=$("$nm" -D --defined-only "$so" 2>/dev/null | grep -w "$sym" || true)
      [ -n "$out" ] && break
      sleep 1
    done
    if [ -z "$out" ]; then
      echo "[x] $abi: $(basename "$so") does not export $sym" >&2; exit 1
    fi
  done

  for lib in libngtcp2.a libngtcp2_crypto_ossl.a libnghttp3.a; do
    local n i
    n=0
    for i in 1 2 3 4 5; do
      n=$("$nm" -g --defined-only "$quic/$lib" 2>/dev/null | grep -c . || true)
      [ "$n" -gt 0 ] && break
      sleep 1
    done
    if [ "$n" -eq 0 ]; then
      echo "[x] $abi: $lib has no symbols (installed?)" >&2; exit 1
    fi
    echo "    $lib: $n symbols"
  done

  # ngtcp2 must have been configured with its OpenSSL backend; without this
  # symbol the app links but QUIC handshakes fail at runtime.
  if ! "$nm" -g --defined-only "$quic/libngtcp2_crypto_ossl.a" 2>/dev/null \
       | grep -q "ngtcp2_crypto_ossl_configure_client_session"; then
    echo "[x] $abi: libngtcp2_crypto_ossl.a lacks ngtcp2_crypto_ossl_configure_client_session" >&2
    echo "    rebuild ngtcp2 with -DENABLE_OPENSSL=ON against the $abi OpenSSL 4 tree" >&2
    exit 1
  fi

  # Packaging relies on the unversioned SONAME (libssl.so / libcrypto.so inside
  # the APK) and on 16 KB LOAD alignment (Android 15+). Both are easy to lose
  # when the OpenSSL configuration changes, so check them here.
  local readelf="$TOOLCHAIN/bin/llvm-readelf"
  if [ -x "$readelf" ]; then
    local soname
    soname=$("$readelf" -d "$so" 2>/dev/null | sed -n 's/.*SONAME.*\[\(.*\)\].*/\1/p' | head -1)
    [ "$soname" = "libssl.so" ] || {
      echo "[x] $abi: libssl.so SONAME is '$soname' (expected unversioned libssl.so)" >&2; exit 1; }
    if "$readelf" -lW "$so" 2>/dev/null | awk '/LOAD/{print $NF}' | grep -qv "0x4000"; then
      echo "[x] $abi: libssl.so LOAD segments are not 16 KB aligned" >&2; exit 1
    fi
    echo "    soname=$soname, LOAD=0x4000"
  fi

  echo "    openssl: $(stat -c%s "$so")/$(stat -c%s "$ossl/libcrypto.so") bytes"
}

for abi in "${ABIS[@]}"; do
  # SKIP_OPENSSL=1 reuses an existing install (useful when only A2 needs a retry).
  if [ "${SKIP_OPENSSL:-0}" != "1" ]; then
    build_openssl "$abi"
  fi
  build_h3 "$abi"
  build_tcp2 "$abi"
  verify "$abi"
done

echo ""
echo "[ok] Android QUIC deps installed under $OUT"
echo "     Pass per-ABI dirs to the APK build:"
echo "     -DJM_WITH_QUIC=ON -DJM_QUIC_ROOT=$OUT/quic-android/<abi>"
echo "     -DJM_OPENSSL41_ROOT=$OUT/openssl41-android/<abi>"
