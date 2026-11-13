#!/usr/bin/env bash
# android/tools/verify_sr_on_device.sh —— 真机验收"超分到底跑没跑在 NPU 上"
#
# 背景：QNN 起不来时引擎会**静默**降级到 CPU EP —— 功能照常出图，只是慢两个数量级
# (真机 5ms/块 → 318ms/块)。只看"有没有崩"或"有没有出图"永远发现不了，
# 所以这里直接读 main.py 的一次性自检结论(引擎模式 / 实际后端 / ms per tile)。
#
# 用法：
#     bash android/tools/verify_sr_on_device.sh            # 取 adb devices 里第一台
#     bash android/tools/verify_sr_on_device.sh <serial>   # 多设备时指定串号
#     JM_PKG=org.jmcomic.jmcomic bash ...                  # 换包名
#
# 退出码：0 = 走的是 NPU(或该机型本就没有 QNN 运行库，判 SKIP)；1 = 退化到 CPU
#
# 说明：本脚本会 force-stop 目标应用再冷启动一次(自检必须发生在启动阶段)。
set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
# Git Bash(MSYS) 会把 /data/... 当成 Windows 路径改写，导致 adb shell 全部报
# "No such file or directory"；Linux/WSL 上这个变量无副作用。
export MSYS_NO_PATHCONV=1

ADB="${JM_ADB:-adb}"
PKG="${JM_PKG:-org.jmcomic.jmcomic}"
SERIAL="${1:-${JM_SERIAL:-}}"
if [ -z "$SERIAL" ]; then
    SERIAL="$("$ADB" devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
fi
if [ -z "$SERIAL" ]; then
    echo "[FAIL] 没有已授权的设备(adb devices 为空)。先开 USB 调试并授权。"
    exit 1
fi

APP_DATA=files
STARTUP_LOG="$APP_DATA/android_startup.log"

sh_device() {  # sh_device <shell 命令>
    "$ADB" -s "$SERIAL" shell "$@"
}

echo "== 设备 $SERIAL =="
sh_device "pm path $PKG" | grep -q "package:" || {
    echo "[FAIL] 设备上没有安装 $PKG"
    exit 1
}
echo "  版本: $(sh_device "dumpsys package $PKG | grep -m1 versionName" | tr -d '\r' | awk '{print $1}')"
echo "  ABI : $(sh_device "getprop ro.product.cpu.abilist" | tr -d '\r')"
echo "  SoC : $(sh_device "getprop ro.soc.model" | tr -d '\r') ($(sh_device "getprop ro.soc.manufacturer" | tr -d '\r'))"
echo "  系统: Android $(sh_device "getprop ro.build.version.release" | tr -d '\r') (API $(sh_device "getprop ro.build.version.sdk" | tr -d '\r'))"

# 自检是一次性开关：main.py 的 DumpDiagnostics 读到就删，所以每次要先重新 touch。
before="$(sh_device "run-as $PKG grep -c 'sr selftest: ' $STARTUP_LOG 2>/dev/null" | tr -d '\r')"
before="${before:-0}"

echo "== 触发自检(打标记 + 冷启动) =="
sh_device "run-as $PKG touch $APP_DATA/sr_selftest" || {
    echo "[FAIL] 打标记失败(run-as 不可用？debug 包才允许)"
    exit 1
}
sh_device "am force-stop $PKG" >/dev/null 2>&1
sh_device "monkey -p $PKG -c android.intent.category.LAUNCHER 1" >/dev/null 2>&1

# 自检在首帧之后跑：一轮 192x192 + 一轮 480x640。HTP 上几秒内完成，
# CPU 上约 8~10s，所以给 90s 上限。
echo -n "  等待自检写入日志"
found=0
for _ in $(seq 1 30); do
    sleep 3
    now="$(sh_device "run-as $PKG grep -c 'sr selftest: ' $STARTUP_LOG 2>/dev/null" | tr -d '\r')"
    now="${now:-0}"
    echo -n "."
    if [ "$now" -gt "$before" ]; then
        found=1
        break
    fi
done
echo
if [ "$found" -ne 1 ]; then
    echo "[FAIL] 90s 内没有产生新的自检块。检查:"
    echo "   adb -s $SERIAL shell run-as $PKG cat $STARTUP_LOG | tail -30"
    exit 1
fi

# 只取最后一块(上一次自检的内容)
block="$(sh_device "run-as $PKG grep -A 26 'sr selftest: ' $STARTUP_LOG" | tr -d '\r' \
        | awk '/sr selftest: /{buf=""} {buf=buf $0 "\n"} END{printf "%s", buf}')"

echo "== 自检结论 =="
echo "$block" | sed 's/^/  /'

mode="$(echo "$block" | sed -n 's/.*引擎模式: //p' | head -1)"
backend="$(echo "$block" | sed -n 's/.*实际后端: //p' | head -1)"
summary="$(echo "$block" | sed -n 's/.*SELFTEST_SUMMARY //p' | head -1)"
tilems="$(echo "$summary" | sed -n 's/.*tile_ms=\([0-9]*\).*/\1/p')"

echo "== 判定 =="
fail=0
case "$mode" in
    *独立进程*) echo "  [ok] 引擎模式: $mode" ;;
    "")         echo "  [FAIL] 没读到『引擎模式』行(自检被中断？)"; fail=1 ;;
    *)          echo "  [FAIL] 引擎模式: $mode —— 进程模式没起来，QNN 拿不到 /vendor 的 FastRPC"; fail=1 ;;
esac
case "$backend" in
    *QNN*HTP*|*Hexagon*) echo "  [ok] 实际后端: $backend" ;;
    "")                  echo "  [FAIL] 没读到『实际后端』行"; fail=1 ;;
    *CPU*)               echo "  [FAIL] 实际后端: $backend —— 整图退到 CPU EP，超分不可用(慢两个数量级)"; fail=1 ;;
    *)                   echo "  [warn] 实际后端: $backend"; fail=1 ;;
esac
if [ -n "$tilems" ]; then
    if [ "$tilems" -le 60 ]; then
        echo "  [ok] 单块耗时: ${tilems}ms/块"
    else
        echo "  [FAIL] 单块耗时: ${tilems}ms/块(CPU 量级，NPU 上应为个位数~几十 ms)"; fail=1
    fi
else
    echo "  [warn] 没读到 tile_ms"
fi

if [ "$fail" -ne 0 ]; then
    echo
    echo "== 定位下一步 =="
    echo "  子进程日志(进程模式失败原因只在这里):"
    echo "    adb -s $SERIAL shell run-as $PKG cat $APP_DATA/sr_proc.log | tail -40"
    echo "  若出现 cannot locate symbol \"_ZNSt3__113__hash_memoryEPKvm\" referenced by"
    echo "  \"/system/lib64/liblog.so\" -> LD_LIBRARY_PATH 里 /vendor/lib64 排到了"
    echo "  /system/lib64 前面(见 README §4.2 第 7 条)。"
    echo "  若是『找不到任何 ONNX 模型』-> 先让 EnsureModels() 把模型解包到私有目录。"
    exit 1
fi

if echo "$block" | grep -q "未打包 —— 进程模式下由子进程从 /vendor/lib64 加载"; then
    echo "  [note] FastRPC 未打进 APK，由子进程从 /vendor/lib64 加载(正常)"
fi
echo "== PASS =="
exit 0
