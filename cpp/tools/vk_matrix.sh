#!/bin/bash
# 真机 Vulkan 复测矩阵：force-stop → am start(嵌套引号) → 轮询 logcat 到 done
export PATH="/c/Users/WQ159/AppData/Local/Android/Sdk/platform-tools:$PATH"
OUT=/tmp/vk_matrix.log
: > $OUT

run_case() {
  local tag="$1"; local args="$2"
  adb shell am force-stop org.jmcomic.ui >/dev/null 2>&1
  adb logcat -c 2>/dev/null
  adb shell am start -n org.jmcomic.ui/org.qtproject.qt.android.bindings.QtActivity \
      --es jm.args "'$args'" >/dev/null 2>&1
  # 轮询等 done（最长 150s）
  for i in $(seq 1 50); do
    sleep 3
    if adb logcat -d 2>/dev/null | grep -aq "vk-report: done"; then
      echo "=== [$tag] $args ===" >> $OUT
      adb logcat -d 2>/dev/null | grep -aE "vk-report: (vk-bench|model|compare|summary|available)" >> $OUT
      return 0
    fi
  done
  echo "=== [$tag] $args TIMEOUT ===" >> $OUT
}

# 性能矩阵：4 组开关 × 4 代表模型（bench 5 次，tile128）
MODELS="waifu2x_cunet_up2x.onnx realcugan_se_up2x.onnx realcugan_pro_up2x_denoise3.onnx realesrgan_x4plus_up4x.onnx"

# M1 fp16+wino（默认）
for m in $MODELS; do
  run_case "M1-fp16-wino" "--vk-bench 5 --vk-tile 128 --vk-only $m"
done

# M2 fp16 无wino
for m in $MODELS; do
  run_case "M2-fp16-nowino" "--vk-bench 5 --vk-tile 128 --vk-wino 0 --vk-only $m"
done

# M3 fp32+wino
for m in $MODELS; do
  run_case "M3-fp32-wino" "--vk-bench 5 --vk-tile 128 --vk-fp16 0 --vk-only $m"
done

# M4 fp32 无wino（第一轮基线）
for m in $MODELS; do
  run_case "M4-fp32-nowino" "--vk-bench 5 --vk-tile 128 --vk-fp16 0 --vk-wino 0 --vk-only $m"
done

echo "ALL DONE" >> $OUT
grep -c "===" $OUT
