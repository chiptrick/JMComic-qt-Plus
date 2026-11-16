#!/bin/bash
# 补跑：M3 realcugan_se（TIMEOUT 复测）+ 精度 compare 组（fp16/fp32）
export PATH="/c/Users/WQ159/AppData/Local/Android/Sdk/platform-tools:$PATH"
OUT=/tmp/vk_matrix2.log
: > $OUT

run_case() {
  local tag="$1"; local args="$2"; local timeout_s="${3:-45}"
  adb shell am force-stop org.jmcomic.ui >/dev/null 2>&1
  adb logcat -c 2>/dev/null
  adb shell am start -n org.jmcomic.ui/org.qtproject.qt.android.bindings.QtActivity \
      --es jm.args "'$args'" >/dev/null 2>&1
  for i in $(seq 1 40); do
    sleep 3
    if adb logcat -d 2>/dev/null | grep -aq "vk-report: done"; then
      echo "=== [$tag] $args ===" >> $OUT
      adb logcat -d 2>/dev/null | grep -aE "vk-report: (vk-bench|model|compare|summary|available)" >> $OUT
      return 0
    fi
  done
  echo "=== [$tag] $args TIMEOUT ===" >> $OUT
}

# M3 se 复测（fp32+wino）
run_case "M3-re-se" "--vk-bench 5 --vk-tile 128 --vk-fp16 0 --vk-only realcugan_se_up2x.onnx" 60

# 精度组：compare（tile192，ORT CPU 逐像素 diff）
CMPS="waifu2x_cunet_up2x.onnx realcugan_se_up2x.onnx realcugan_pro_up2x_denoise3.onnx realesrgan_animevideo_v3_up2x.onnx srmd_x4.onnx realesrgan_general_up4x.onnx"

# A1 fp16+wino compare
for m in $CMPS; do
  run_case "CMP-fp16" "--vk-compare --vk-sweep --vk-tile 192 --vk-only $m"
done

# A2 fp32+wino compare
for m in $CMPS; do
  run_case "CMP-fp32" "--vk-compare --vk-sweep --vk-tile 192 --vk-fp16 0 --vk-only $m"
done

echo "ALL DONE" >> $OUT
