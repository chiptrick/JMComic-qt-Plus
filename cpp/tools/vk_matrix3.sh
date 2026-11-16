#!/bin/bash
# fp16 compare 补齐（logcat -s jmcomic:W 过滤版轮询）
export PATH="/c/Users/WQ159/AppData/Local/Android/Sdk/platform-tools:$PATH"
OUT=/tmp/vk_cmp16.log
: > $OUT

run_case() {
  local tag="$1"; local args="$2"
  adb shell am force-stop org.jmcomic.ui >/dev/null 2>&1
  sleep 2
  adb shell am start -n org.jmcomic.ui/org.qtproject.qt.android.bindings.QtActivity \
      --es jm.args "'$args'" >/dev/null 2>&1
  for i in $(seq 1 40); do
    sleep 4
    if adb logcat -d -s jmcomic:W 2>/dev/null | grep -aq "vk-report: done"; then
      echo "=== [$tag] ===" >> $OUT
      adb logcat -d -s jmcomic:W 2>/dev/null | grep -aE "vk-report: (compare|summary)" >> $OUT
      return 0
    fi
  done
  echo "=== [$tag] TIMEOUT ===" >> $OUT
}

# fp16+wino compare 剩余 5 个模型（waifu2x 已过）
for m in realcugan_se_up2x.onnx realcugan_pro_up2x_denoise3.onnx realesrgan_animevideo_v3_up2x.onnx srmd_x4.onnx; do
  run_case "CMP-fp16-$m" "--vk-compare --vk-sweep --vk-tile 192 --vk-only $m"
done

echo "ALL DONE" >> $OUT
