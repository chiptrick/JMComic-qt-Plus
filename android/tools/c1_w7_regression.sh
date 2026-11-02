#!/usr/bin/env bash
# C1-W7 真机回归取证（方案 P1 的自动化部分）
#
# 它做什么：
#   1. 设备体检（型号/SDK/ABI/是否 debug 包可装）
#   2. 安装 APK → 冷启动 3 次取中位 → PSS → gfxinfo 帧耗时 → logcat 关键行
#   3. 跑 16KB / bundle 两条门禁（对 APK 本身，不依赖设备）
#   4. 输出一份 markdown 表格到 android/build_logs/c1_w7_<时间戳>/report.md
#
# 它不做什么（必须人工，脚本在报告里标 MANUAL，不假装 PASS）：
#   F2 标题栏/返回栈、F3~F8 的视觉与交互、S1/S2 的连续翻页手感、S3 桥崩溃重启的人工确认。
#   原因：这些的判据是"看起来对/操作起来对"，用输入坐标去猜只会产出无法复核的假绿。
#
# 用法：
#   bash android/tools/c1_w7_regression.sh --apk <path.apk>
#   bash android/tools/c1_w7_regression.sh --apk <apk> --images <本地漫画目录> --runs 3
set -uo pipefail
export MSYS_NO_PATHCONV=1   # Git Bash 下别把 /data/... 变成 C:/...

PKG=${PKG:-org.jmcomic.c1shell}
ACT=${ACT:-org.qtproject.qt.android.bindings.QtActivity}
APK=""
IMAGES=""
RUNS=3
SKIP_INSTALL=0

while [ $# -gt 0 ]; do
  case "$1" in
    --apk) APK=${2:-}; shift 2 ;;
    --images) IMAGES=${2:-}; shift 2 ;;
    --runs) RUNS=${2:-3}; shift 2 ;;
    --no-install) SKIP_INSTALL=1; shift ;;
    -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
    *) echo "[fail] 未知参数: $1"; exit 2 ;;
  esac
done

# ---- adb 定位 ----
ADB=${ADB:-}
if [ -z "$ADB" ]; then
  for cand in "$(command -v adb 2>/dev/null)" \
              "${ANDROID_SDK_ROOT:-}/platform-tools/adb" \
              "${ANDROID_HOME:-}/platform-tools/adb" \
              "$HOME/AppData/Local/Android/Sdk/platform-tools/adb.exe" \
              "$HOME/Android/Sdk/platform-tools/adb"; do
    if [ -n "$cand" ] && [ -x "$cand" ]; then ADB=$cand; break; fi
  done
fi
[ -n "$ADB" ] || { echo "[fail] 找不到 adb；用 ADB=/path/to/adb 指定"; exit 1; }

STAMP=$(date +%Y%m%d-%H%M%S)
OUT="$PWD/android/build_logs/c1_w7_$STAMP"
mkdir -p "$OUT"
REPORT="$OUT/report.md"

say() { printf '%s\n' "$*"; printf '%s\n' "$*" >> "$OUT/run.log"; }
# 启动应用：vivo 上 am start 紧跟 force-stop 偶尔不生效（日志里连启动痕迹都没有），
# 这里用 -W 等待并对 pid 做一次校验重试，避免"静默没起来"被当成"功能 MISS"。
start_app() {
  "$ADB" -s "$DEV" shell am start -W -n "$PKG/$ACT" >/dev/null 2>&1 || true
  sleep 2
  if [ -z "$("$ADB" -s "$DEV" shell pidof "$PKG" 2>/dev/null | tr -d '\r')" ]; then
    "$ADB" -s "$DEV" shell am start -n "$PKG/$ACT" >/dev/null 2>&1 || true
    sleep 2
  fi
}

row() { printf '| %s | %s | %s | %s |\n' "$1" "$2" "$3" "$4" >> "$REPORT"; }

# ---- 0. 设备 ----
DEV=$("$ADB" devices | awk 'NR>1 && $2=="device" {print $1; exit}')
if [ -z "$DEV" ]; then
  cat > "$REPORT" <<EOF
# C1-W7 真机回归（$STAMP）

**未执行：没有可见设备。**

\`\`\`
$("$ADB" devices)
\`\`\`

按方案 R1，P1/P2 需要真机（V2463A）。请开启 USB 调试并在设备上确认授权后重跑：

\`\`\`bash
bash android/tools/c1_w7_regression.sh --apk <apk路径>
\`\`\`
EOF
  echo "[blocked] 没有可见设备；已写入 $REPORT"
  exit 3
fi

MODEL=$("$ADB" -s "$DEV" shell getprop ro.product.model | tr -d '\r')
SDK=$("$ADB" -s "$DEV" shell getprop ro.build.version.sdk | tr -d '\r')
REL=$("$ADB" -s "$DEV" shell getprop ro.build.version.release | tr -d '\r')
ABI=$("$ADB" -s "$DEV" shell getprop ro.product.cpu.abi | tr -d '\r')

cat > "$REPORT" <<EOF
# C1-W7 真机回归（$STAMP）

- 设备：\`$DEV\` / $MODEL / Android $REL (SDK $SDK) / $ABI
- 包名：\`$PKG\`
- APK：\`${APK:-（未提供，跳过安装）}\`
- 采样次数：$RUNS

## 1. 性能

| 项 | 数值 | 目标 | 判定 |
|---|---|---|---|
EOF

# ---- 1. 安装 ----
if [ -n "$APK" ] && [ "$SKIP_INSTALL" -eq 0 ]; then
  say "安装 APK: $APK"
  INSTALL=$("$ADB" -s "$DEV" install -r "$APK" 2>&1 | tr -d '\r')
  echo "$INSTALL" > "$OUT/install.txt"
  if ! echo "$INSTALL" | grep -q "Success"; then
    say "[fail] 安装失败：$INSTALL"
    echo "## 0. 安装" >> "$REPORT"
    echo "" >> "$REPORT"
    echo "**FAIL**：$INSTALL" >> "$REPORT"
  else
    say "[ok] 安装成功"
  fi
fi

# 投图（阅读器目录：应用私有 files/reader）
if [ -n "$IMAGES" ] && [ -d "$IMAGES" ]; then
  say "投图: $IMAGES"
  "$ADB" -s "$DEV" shell "run-as $PKG mkdir -p files/reader" >/dev/null 2>&1
  for img in "$IMAGES"/*; do
    [ -f "$img" ] || continue
    case "${img,,}" in
      *.jpg|*.jpeg|*.png|*.webp|*.bmp|*.gif)
        # 不能走 /data/local/tmp 中转（应用 uid 无法遍历该目录），也不能用 stdin
        # （adb shell 的 PTY 会截断二进制）。base64 走命令行是唯一稳定的路径，
        # 已用 md5 对拍验证过。
        b64=$(base64 -w0 "$img")
        "$ADB" -s "$DEV" shell "run-as $PKG sh -c 'echo $b64 | base64 -d > files/reader/$(basename "$img")'" >/dev/null 2>&1
        ;;
    esac
  done
  COUNT=$("$ADB" -s "$DEV" shell run-as "$PKG" sh -c 'ls files/reader | wc -l' | tr -d '\r')
  say "已投图 $COUNT 张"
fi

# ---- 2. 冷启动（3 次取中位） ----
START_TIMES=""
for i in $(seq 1 "$RUNS"); do
  "$ADB" -s "$DEV" shell am force-stop "$PKG" >/dev/null 2>&1
  sleep 1
  T=$("$ADB" -s "$DEV" shell am start -W -n "$PKG/$ACT" 2>/dev/null \
      | tr -d '\r' | awk -F': ' '/^TotalTime/ {print $2}' | head -1)
  [ -n "${T:-}" ] && START_TIMES="$START_TIMES $T"
  echo "run $i: TotalTime=${T:-NA}ms" >> "$OUT/start.txt"
done
MEDIAN=$(printf '%s\n' $START_TIMES | sort -n | awk '{a[NR]=$0} END{ if(NR==0){print "NA"} else if(NR%2){print a[(NR+1)/2]} else {print int((a[NR/2]+a[NR/2+1])/2)} }')
FAILED_START=$("$ADB" -s "$DEV" shell pidof "$PKG" | tr -d '\r')
row "冷启动 TotalTime" "${MEDIAN}ms（${START_TIMES# }）" "≤ Python 版同机中位" "$([ "$MEDIAN" != "NA" ] && [ -n "$FAILED_START" ] && echo "待对照" || echo "FAIL")"
say "冷启动中位: ${MEDIAN}ms"

# ---- 3. 自检钩子（功能侧取证） ----
"$ADB" -s "$DEV" logcat -c >/dev/null 2>&1
"$ADB" -s "$DEV" shell am force-stop "$PKG" >/dev/null 2>&1
sleep 1
"$ADB" -s "$DEV" shell "run-as $PKG sh -c 'touch files/selftest files/net_selftest'" >/dev/null 2>&1
start_app
say "已触发 selftest / net_selftest，等待 30s 采集日志"
sleep 30
"$ADB" -s "$DEV" logcat -d > "$OUT/logcat.txt" 2>/dev/null
grep -E "C1Shell|C1Reader|C1Net" "$OUT/logcat.txt" > "$OUT/c1_log.txt" 2>/dev/null || true

grep -q "selftest done pages=" "$OUT/c1_log.txt" && SE=OK || SE=MISS
grep -q "settings selftest OK" "$OUT/c1_log.txt" && SS=OK || SS=MISS
grep -q "net OK 2[0-9][0-9]" "$OUT/c1_log.txt" && NET=OK || NET=MISS
grep -q "CA ready" "$OUT/c1_log.txt" && CA=OK || CA=MISS
grep -q "sr_qnn in link map OK" "$OUT/c1_log.txt" && SRQ=OK || SRQ=MISS
CRASH=$(grep -cE "FATAL EXCEPTION|AndroidRuntime.*FATAL" "$OUT/logcat.txt" 2>/dev/null | head -1 || true)
CRASH=${CRASH:-0}

# ---- 4. PSS ----
PSSLINE=$("$ADB" -s "$DEV" shell dumpsys meminfo "$PKG" 2>/dev/null | tr -d '\r' | awk '/TOTAL/ {print; exit}')
PSS=$(printf '%s\n' "$PSSLINE" | awk '{for(i=1;i<=NF;i++) if($i ~ /^[0-9]+$/){print $i; exit}}')
echo "$PSSLINE" > "$OUT/meminfo_total.txt"
[ -n "$PSS" ] && PSS_MB=$((PSS / 1024)) || PSS_MB=0
row "内存 PSS" "${PSS_MB}MB（${PSS:-NA}KB）" "≤450MB" "$([ "${PSS_MB:-0}" -gt 0 ] && [ "$PSS_MB" -le 450 ] && echo PASS || echo FAIL)"

# ---- 5. 帧耗时（gfxinfo p95） ----
"$ADB" -s "$DEV" shell dumpsys gfxinfo "$PKG" reset >/dev/null 2>&1
# 不等人工：用 selftest 钩子让阅读器自己连续翻页/缩放（间隔 300ms），
# 这样 gfxinfo 采到的是真实渲染帧，而不是空窗口下的 0 帧。
"$ADB" -s "$DEV" shell "run-as $PKG sh -c 'touch files/selftest'" >/dev/null 2>&1
"$ADB" -s "$DEV" shell am force-stop "$PKG" >/dev/null 2>&1
sleep 1
start_app
say "翻页采样：selftest 自动翻页中，等待 12s"
sleep 12
"$ADB" -s "$DEV" shell dumpsys gfxinfo "$PKG" > "$OUT/gfxinfo.txt" 2>/dev/null
P95=$("$ADB" -s "$DEV" shell dumpsys gfxinfo "$PKG" 2>/dev/null | tr -d '\r' \
      | awk -F': ' '/95th percentile/ {print $2+0; exit}')
if [ -n "${P95:-}" ] && [ "$P95" != "0" ]; then
  FPS=$(awk -v ms="$P95" 'BEGIN{ if(ms>0) printf "%.0f", 1000/ms; else print "NA" }')
  row "翻页帧率 p95" "${FPS}fps（p95=${P95}ms）" "≥110fps" "$([ "$FPS" -ge 110 ] 2>/dev/null && echo PASS || echo FAIL)"
else
  row "翻页帧率 p95" "NA（gfxinfo 无帧数据）" "≥110fps" "UNKNOWN"
fi

# ---- 6. 功能/稳定性汇总 ----
cat >> "$REPORT" <<EOF

## 2. 自动化可判定项（来自 logcat）

| 项 | 期望 | 实测 | 判定 |
|---|---|---|---|
| 阅读器自检 | \`selftest done pages=N\` | $SE | $([ "$SE" = OK ] && echo PASS || echo FAIL) |
| 设置页自检 | \`settings selftest OK\` | $SS | $([ "$SS" = OK ] && echo PASS || echo FAIL) |
| CA 证书就绪 | \`CA ready\` | $CA | $([ "$CA" = OK ] && echo PASS || echo FAIL) |
| 真实请求 | \`net OK 2xx\` | $NET | $([ "$NET" = OK ] && echo PASS || echo FAIL) |
| 直链探针 | \`sr_qnn in link map OK\` | $SRQ | $([ "$SRQ" = OK ] && echo PASS || echo FAIL) |
| 崩溃行数 | 0 | $CRASH | $([ "$CRASH" -eq 0 ] && echo PASS || echo FAIL) |

## 3. 必须人工确认（脚本不代判）

| 用例 | 判据 | 结果 |
|---|---|---|
| F2 三页切换 | 标题栏跟随、Back 依次回退 | MANUAL |
| F3 目录加载 | 页数与实际图片数一致 | MANUAL |
| F4 翻页方向 | RTL 时左右键语义反转 | MANUAL |
| F5 缩放 | +/-/0 与捏合，适宽复位 | MANUAL |
| F6 双页 | 第二页不足时退化单页 | MANUAL |
| F7 亮度 | 1.0→0.75→0.5→0.25→1.0 | MANUAL |
| F8 设置读写 | config.ini 字段一致 | MANUAL |
| S1 连续翻 20 页 | 无 crash/OOM | MANUAL |
| S2 连续切页 10 次 | 无 crash | MANUAL |
| S3 网络层异常 | 断网/超时后能继续请求 | MANUAL |

## 4. 原始产物

- \`logcat.txt\` / \`c1_log.txt\`（关键行）
- \`start.txt\`（冷启动逐次）、\`meminfo_total.txt\`、\`gfxinfo.txt\`
- \`install.txt\`

> 判定口径：本脚本只把"能从日志/系统计数里读出数字"的项标 PASS，其余一律 MANUAL。
> 人工项未填完之前，W7 不得宣布通过（方案 §6.1：「不接受基本可用」）。
EOF

# ---- 7. APK 门禁（不依赖设备） ----
if [ -n "$APK" ] && [ -f "$APK" ]; then
  {
    echo ""
    echo "## 5. APK 门禁"
    echo ""
    echo '```'
    python "$PWD/android/tools/check_elf_page_align.py" "$APK" 2>&1 || true
    python "$PWD/android/tools/check_pybundle_align.py" "$APK" 2>&1 || true
    echo '```'
  } >> "$REPORT"
fi

say ""
say "报告: $REPORT"
[ "$CRASH" -eq 0 ] || exit 1
exit 0
