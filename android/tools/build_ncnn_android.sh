#!/usr/bin/env bash
# 为非高通机型准备 Vulkan GPU 超分后端：构建 ncnn（Android arm64 + Vulkan）
#
# 为什么用 ncnn：
#   1. ncnn 的 Vulkan 后端在 Adreno/Mali 上成熟（nihui 维护，覆盖广）；
#   2. 我们需要的模型族（Real-CUGAN / SRMD / waifu2x）**官方就有 ncnn 格式**
#      （realcugan-ncnn-vulkan、srmd-ncnn-vulkan 的 models-*），不用重新转换；
#   3. 纯 C++ 静态库，能塞进 libsr_vk.so，与现有 sr_qnn 的 C ABI 对齐。
#
# 前置：ncnn 源码需先在**宿主** clone（WSL 到 github 域名不通）：
#   git clone --depth 1 -b 20240820 https://github.com/Tencent/ncnn.git D:/jm-ncnn/ncnn
#   （版本可换，建议锁定 tag 避免上游漂移）
#
# 用法：
#   bash android/tools/build_ncnn_android.sh /mnt/d/jm-ncnn/ncnn [abi]
set -euo pipefail

SRC="${1:?用法: build_ncnn_android.sh <ncnn 源码目录> [arm64-v8a|arm64-v8a]}"
ABI="${2:-arm64-v8a}"
REPO="/mnt/c/Users/WQ159/source/repos/JMComic-qt"
OUT="$HOME/jmcomic-build/ncnn-build-$ABI"
SDK="$HOME/jmcomic-build/android-sdk"
NDK="$SDK/ndk/30.0.16248370"
TOOLCHAIN="$NDK/build/cmake/android.toolchain.cmake"

[ -f "$SRC/CMakeLists.txt" ] || { echo "[fail] $SRC 不是 ncnn 源码目录（见脚本头注释）"; exit 1; }
[ -f "$TOOLCHAIN" ] || { echo "[fail] 缺 NDK toolchain: $TOOLCHAIN"; exit 1; }

mkdir -p "$OUT"
cmake -S "$SRC" -B "$OUT" \
  -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
  -DANDROID_ABI="$ABI" \
  -DANDROID_PLATFORM=android-24 \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCNN_VULKAN=ON \
  -DNCNN_SHARED_LIB=OFF \
  -DNCNN_BUILD_TOOLS=OFF \
  -DNCNN_BUILD_EXAMPLES=OFF \
  -DNCNN_BUILD_BENCHMARK=OFF \
  -DNCNN_OPENMP=ON \
  -DNCNN_THREADS=ON \
  -DNCNN_PIXEL=ON \
  -DNCNN_PIXEL_DRAWING=OFF \
  -DNCNN_STRING=OFF \
  -DNCNN_SYSTEM_GLSLANG=OFF \
  -DNCNN_VULKAN_ONLINE_SPIRV=OFF \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5   # ncnn 2024 版的 CMakeLists 最低策略版本较老

cmake --build "$OUT" -j"$(nproc)"

echo "== 产物 =="
ls -lh "$OUT"/src/libncnn.a "$OUT"/src/libncnn.a 2>/dev/null | tail -2
find "$OUT" -name "libncnn*.a" -o -name "libglslang*.a" -o -name "libSPIRV*.a" | head -8
du -sh "$OUT"
echo
echo "下一步（见 docs §0C）：把 ncnn + glslang/SPIRV 静态链进 libsr_vk.so，"
echo "实现与 sr_qnn.h 相同的 C ABI（srq_init/set_model_dir/model_*/add/load/free_result）。"
