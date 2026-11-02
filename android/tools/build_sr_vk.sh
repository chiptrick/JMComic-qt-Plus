#!/usr/bin/env bash
# 构建 libsr_vk.so（Vulkan GPU 超分后端，非高通机型用）
#
# 前置：先跑 android/tools/build_ncnn_android.sh 产出 ncnn 静态库（含 glslang/SPIRV）
# 用法：bash android/tools/build_sr_vk.sh [arm64-v8a]
set -euo pipefail

ABI="${1:-arm64-v8a}"
REPO="/mnt/c/Users/WQ159/source/repos/JMComic-qt"
SRC="$REPO/android/sr_qnn/cpp_vk"
NCNN_SRC="/mnt/d/jm-ncnn/ncnn"
NCNN_BUILD="$HOME/jmcomic-build/ncnn-build-$ABI"
OUT="$HOME/jmcomic-build/sr_vk-$ABI"
SDK="$HOME/jmcomic-build/android-sdk"
NDK="$SDK/ndk/30.0.16248370"
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"

[ -f "$NCNN_BUILD/src/libncnn.a" ] || {
  echo "[fail] 缺 ncnn 静态库（先跑 build_ncnn_android.sh）"
  exit 1
}

mkdir -p "$OUT"
cmake -S "$SRC" -B "$OUT" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCNN_BUILD="$NCNN_BUILD" \
  -DNCNN_SRC="$NCNN_SRC"

cmake --build "$OUT" -j"$(nproc)"

SO="$OUT/libsr_vk.so"
echo "== 产物 =="
ls -lh "$SO" 2>/dev/null || { echo "[fail] 未产出 libsr_vk.so"; exit 1; }
echo "== 导出符号（应与 libsr_qnn 同名） =="
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm" -D --defined-only "$SO" \
  | grep -E " T srq_" | awk '{print "  "$3}' | sort
echo "== 页对齐 =="
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-readelf" -l "$SO" \
  | awk '/^ *LOAD/{print "  align="$NF}' | sort -u
