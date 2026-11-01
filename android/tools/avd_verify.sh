#!/usr/bin/env bash
# android/tools/avd_verify.sh —— 模拟器/真机一键验收（装包 → 启动 → 取证 → 给结论）
#
# 为什么需要它：`pyside6-android-deploy` 打出 APK 之后，"这个包装上去到底能不能用"
# 以前是靠人手工跑 5~6 条 adb 命令再凭印象判断 —— 于是出现"校验通过但其实装的是旧包"
# 这类假成功。这里把判断固化成一张 PASS/FAIL 表，退出码 0/1 直接可用于脚本串联。
#
# 每项都给出 实测值 / 阈值 / 结论 三列，看表即可复述结论，不需要回看日志。
#
# 用法：
#   bash android/tools/avd_verify.sh --apk android/dist/JMComic-0.88-x86_64-debug.apk
#   bash android/tools/avd_verify.sh --apk ... --serial emulator-5554 --abi x86_64
#   bash android/tools/avd_verify.sh --apk ... --max-frame-ms 300 --require-16k
#
# 参数：
#   --apk <path>        要验收的 APK（必填）
#   --serial <id>       adb 串号（默认取 `adb devices` 里第一个 device）
#   --abi <abi>         目标 ABI，默认 x86_64（模拟器）；影响 16KB 检查与截图名
#   --pkg <name>        包名（默认从 APK 里用 aapt 读；读不到用 org.jmcomic.jmcomic）
#   --activity <name>   启动 Activity（默认从 aapt 读 launchable-activity）
#   --max-frame-ms <n>  `am start -W` 的 TotalTime 阈值，默认 300（方案 §1.1：首帧不回退）
#   --launch-wait <s>   启动后等待多久再判定进程存活/PSS，默认 6
#   --require-16k       设备不是 16KB 页时按 FAIL 处理（16KB 专项验收用）
#   --out-dir <dir>     截图与日志目录，默认 android/build_logs
#
# 退出码：0 全部 PASS（16KB 可能为 N/A，见下）/ 1 有 FAIL / 2 环境问题（无 adb/无设备）
#
# 关于 16KB 一栏：模拟器默认是 4KB 页，此时无法做 16KB 验收 —— 表里记 `N/A`，
# 不算 FAIL（否则 x86_64 冒烟永远红）；要卡这一项就加 `--require-16k`，
# 它会把 N/A 变成 FAIL，供"16KB 专项"那一轮使用。
#
# 判定口径（都是"退出码 + 产物 + 日志关键字"三者合判，不看单一信号）：
#   * 安装失败 → **立即收尾 exit 1**，后面的首帧/存活/PSS 一律不跑。原因：装不上时
#     这些项测的是设备上**上一次装的那个旧版本**，会产出一份"看起来在测本次产物、
#     实际在测旧包"的报告 —— 那是 MSI（误导性成功指标），比直接失败更糟。
#   * 首帧：`am start -W` 的 TotalTime（没有就用 WaitTime，实测值里标出来源）。
#     注意它是"adb 视角的 Activity 启动耗时"，不等于渲染首帧，也不等于设备端冷启体验。
#   * 16KB：一票否决。除了 check_16kb_device.sh 的设备页大小/兼容标记，
#     还单独扫两条关键字：`dlopen failed: .*program alignment` 与 `UnsatisfiedLinkError`
#     （0.87 的 x86_64 包就是靠这两条钉死的）。
#   * 退出码 134 / SIGABRT：进程仍存活 → 只记 INFO（已知的 PySide6 6.12 finalize GC
#     缺陷，跑完才死，产物齐全）；进程已死 → 由 pidof 一栏判 FAIL。
#   * PSS：只记录，不设阈值。
. "$(dirname "${BASH_SOURCE[0]}")/env.sh"
set -uo pipefail

APK=""
SERIAL="${JM_SERIAL:-}"
ABI="x86_64"
PKG=""
ACTIVITY=""
MAX_FRAME_MS=300
LAUNCH_WAIT=6
REQUIRE_16K=0
OUT_DIR=""

while [ $# -gt 0 ]; do
    case "$1" in
        --apk)          APK="${2:-}"; shift ;;
        --apk=*)        APK="${1#--apk=}" ;;
        --serial)       SERIAL="${2:-}"; shift ;;
        --serial=*)     SERIAL="${1#--serial=}" ;;
        --abi)          ABI="${2:-}"; shift ;;
        --abi=*)        ABI="${1#--abi=}" ;;
        --pkg)          PKG="${2:-}"; shift ;;
        --pkg=*)        PKG="${1#--pkg=}" ;;
        --activity)     ACTIVITY="${2:-}"; shift ;;
        --activity=*)   ACTIVITY="${1#--activity=}" ;;
        --max-frame-ms) MAX_FRAME_MS="${2:-}"; shift ;;
        --max-frame-ms=*) MAX_FRAME_MS="${1#--max-frame-ms=}" ;;
        --launch-wait)  LAUNCH_WAIT="${2:-}"; shift ;;
        --launch-wait=*) LAUNCH_WAIT="${1#--launch-wait=}" ;;
        --require-16k)  REQUIRE_16K=1 ;;
        --out-dir)      OUT_DIR="${2:-}"; shift ;;
        --out-dir=*)    OUT_DIR="${1#--out-dir=}" ;;
        -h|--help)      awk 'NR > 1 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print; next } exit }' \
                            "${BASH_SOURCE[0]}"; exit 0 ;;
        *) echo "未知参数: $1（--help 看用法）" >&2; exit 2 ;;
    esac
    shift
done

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${OUT_DIR:-${HERE}/build_logs}"
STAMP="$(date +%Y%m%d-%H%M%S)"

die() { echo "[FATAL] $*" >&2; exit 2; }

[ -n "${APK}" ] || { echo "缺少 --apk（--help 看用法）" >&2; exit 2; }
[ -f "${APK}" ] || die "APK 不存在: ${APK}"
APK="$(cd "$(dirname "${APK}")" && pwd)/$(basename "${APK}")"
mkdir -p "${OUT_DIR}" || die "创建输出目录失败: ${OUT_DIR}"

[ -x "${JM_ADB}" ] || die "找不到 adb: ${JM_ADB}（设 ANDROID_HOME 或在 PATH 里提供 adb）"
if [ -z "${SERIAL}" ]; then
    SERIAL="$("${JM_ADB}" devices 2>/dev/null | awk 'NR>1 && $2=="device" {print $1; exit}')"
fi
[ -n "${SERIAL}" ] || die "没有已授权的设备（adb devices 为空）。先启动 AVD 或插上真机并允许调试。"
export JM_SERIAL="${SERIAL}"      # check_16kb_device.sh 通过 env.sh 读它
export JM_ABI="${ABI}"

adbs() { "${JM_ADB}" -s "${SERIAL}" "$@"; }

# ---------------------------------------------------------------- 读取包信息
AAPT_BIN="$(ls "${JM_SDK}"/build-tools/*/aapt 2>/dev/null | sort -V | tail -1)"
if [ -z "${PKG}" ] && [ -n "${AAPT_BIN}" ] && [ -x "${AAPT_BIN}" ]; then
    BADGING="$("${AAPT_BIN}" dump badging "${APK}" 2>/dev/null)"
    PKG="$(printf '%s' "${BADGING}" | sed -n "s/^package: name='\([^']*\)'.*/\1/p" | head -1)"
    [ -n "${ACTIVITY}" ] || ACTIVITY="$(printf '%s' "${BADGING}" \
        | sed -n "s/^launchable-activity: name='\([^']*\)'.*/\1/p" | head -1)"
fi
PKG="${PKG:-org.jmcomic.jmcomic}"
ACTIVITY="${ACTIVITY:-org.kivy.android.PythonActivity}"

echo "=========================================================="
echo " AVD 验收: $(basename "${APK}")"
echo " 设备: ${SERIAL}   包名: ${PKG}   Activity: ${ACTIVITY}"
echo " 产物: $(date -r "${APK}" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo '?')  $(stat -c %s "${APK}" 2>/dev/null || echo '?') 字节"
echo "=========================================================="

ROWS=()          # 每行: 项目|实测值|阈值|结论
FAILED=0
add_row() { ROWS+=("$1|$2|$3|$4"); [ "$4" = "FAIL" ] && FAILED=$((FAILED + 1)); return 0; }

# ---------------------------------------------------------------- 1. 安装
echo "-> adb install -r（覆盖安装，保留数据）"
INSTALL_OUT="$(adbs install -r "${APK}" 2>&1)"
INSTALL_RC=$?
INSTALL_OK=1
if [ ${INSTALL_RC} -ne 0 ] || printf '%s' "${INSTALL_OUT}" | grep -qi "Failure"; then
    echo "${INSTALL_OUT}" | tail -5 | sed 's/^/   /'
    add_row "安装 (adb install -r)" "失败" "Success" "FAIL"
    INSTALL_OK=0
else
    echo "${INSTALL_OUT}" | tail -2 | sed 's/^/   /'
    add_row "安装 (adb install -r)" "Success" "Success" "PASS"
fi

# 安装失败必须**立刻收尾**：后面每一项（首帧/进程存活/PSS/16KB/截图）都会去测
# 设备上**上一次装的那个旧版本** —— 于是产出一份"看起来在测本次产物、其实在测旧包"
# 的报告。这比直接失败更糟：它是 MSI（误导性成功指标）。
# 所以：失败 → 打一张只有"安装 FAIL + 未执行"的表，然后 exit 1。
if [ "${INSTALL_OK}" != "1" ]; then
    echo
    echo "=========================================================="
    echo " 验收结果（安装失败，后续项一律未执行）"
    echo "=========================================================="
    printf '%s\n' "${ROWS[@]}" | python3 -c '
import sys, unicodedata

def width(text):
    total = 0
    for ch in text:
        total += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return total

def pad(text, target):
    return text + " " * max(0, target - width(text))

rows = [line.rstrip("\n").split("|") for line in sys.stdin if line.strip()]
header = ["项目", "实测值", "阈值", "结论"]
body = [header] + rows
cols = [max(width(r[i]) if i < len(r) else 0 for r in body) for i in range(4)]
print(" | ".join(pad(header[i], cols[i]) for i in range(4)))
print("-+-".join("-" * c for c in cols))
for row in rows:
    cells = [(row[i] if i < len(row) else "") for i in range(4)]
    print(" | ".join(pad(cells[i], cols[i]) for i in range(4)))
' || echo "  （表格渲染失败）"
    echo
    echo " 未执行: 首帧 / 进程存活 / 崩溃日志 / PSS / 16KB 兼容 / 截图"
    echo " 原因: APK 没装上，这些项只会测到设备上已有的旧版本，结论无意义。"
    echo " 排查: ①签名/降级冲突 → 先 adb uninstall ${PKG} 再试"
    echo "       ②空间不足 → adb shell df -h /data"
    echo "       ③ABI 不符 → aapt dump badging ${APK} | head -1"
    echo " 结论: FAIL（安装失败）"
    exit 1
fi

# ---------------------------------------------------------------- 2. 冷启动 + TotalTime
# 先 force-stop 再 am start -W：否则拿到的是"已在运行"的假 TotalTime。
adbs shell am force-stop "${PKG}" >/dev/null 2>&1 || true
adbs logcat -c >/dev/null 2>&1 || true
START_OUT="$(adbs shell am start -W -n "${PKG}/${ACTIVITY}" 2>&1)"
TOTAL_MS="$(printf '%s' "${START_OUT}" | tr -d '\r' | sed -n 's/^TotalTime: *\([0-9]\+\).*/\1/p' | head -1)"
# 部分 ROM/系统版本只给 WaitTime（TotalTime 在"启动未完成/LaunchState: UNKNOWN"时不打印）。
# 两者都是"adb 视角的启动耗时"，所以缺 TotalTime 时用 WaitTime 顶，并在实测值里标出来源。
FRAME_SRC="TotalTime"
if [ -z "${TOTAL_MS}" ]; then
    TOTAL_MS="$(printf '%s' "${START_OUT}" | tr -d '\r' | sed -n 's/^WaitTime: *\([0-9]\+\).*/\1/p' | head -1)"
    FRAME_SRC="WaitTime"
fi
echo "${START_OUT}" | tr -d '\r' | sed 's/^/   /' | head -6
if [ -z "${TOTAL_MS}" ]; then
    add_row "首帧 (am start -W ${FRAME_SRC})" "解析失败" "<= ${MAX_FRAME_MS} ms" "FAIL"
elif [ "${TOTAL_MS}" -le "${MAX_FRAME_MS}" ]; then
    add_row "首帧 (am start -W ${FRAME_SRC})" "${TOTAL_MS} ms" "<= ${MAX_FRAME_MS} ms" "PASS"
else
    add_row "首帧 (am start -W ${FRAME_SRC})" "${TOTAL_MS} ms" "<= ${MAX_FRAME_MS} ms" "FAIL"
fi

sleep "${LAUNCH_WAIT}"

# ---------------------------------------------------------------- 3. 进程存活
PID="$(adbs shell pidof "${PKG}" 2>/dev/null | tr -d '\r' | awk '{print $1}')"
if [ -n "${PID}" ]; then
    add_row "进程存活 (pidof)" "pid=${PID}" "非空" "PASS"
else
    add_row "进程存活 (pidof)" "无进程" "非空" "FAIL"
fi

# ---------------------------------------------------------------- 4. 崩溃/加载失败日志
# 只匹配"必须为 0 条"的模式：16KB 兼容警告、未对齐、dlopen 失败、链接失败、FATAL。
LOGCAT_FILE="${OUT_DIR}/avd-${STAMP}-${ABI}-logcat.txt"
adbs logcat -d > "${LOGCAT_FILE}" 2>/dev/null || true
BAD_LOG="$(tr -d '\r' < "${LOGCAT_FILE}" 2>/dev/null \
    | grep -icE "16 ?kb|not aligned|dlopen failed|CANNOT LINK|FATAL" || true)"
if [ "${BAD_LOG:-0}" -eq 0 ]; then
    add_row "崩溃/加载失败日志" "0 条" "0 条" "PASS"
else
    add_row "崩溃/加载失败日志" "${BAD_LOG} 条" "0 条" "FAIL"
    echo "   匹配到的日志（前 5 条，全文: ${LOGCAT_FILE}）:"
    tr -d '\r' < "${LOGCAT_FILE}" \
        | grep -iE "16 ?kb|not aligned|dlopen failed|CANNOT LINK|FATAL" | head -5 | sed 's/^/     /'
fi

# 4.1 16KB 库加载失败：**必须单独一行**，不能只混在"崩溃日志"里。
# 0.87 那次就是这两条把问题钉死的：
#   dlopen failed: ".../libc++_shared.so" program alignment (4096) cannot be smaller than system page size (16384)
#   E QtLoader: java.lang.UnsatisfiedLinkError: dlopen failed: ...
# 单独成行是因为它是**一票否决项**，而且判定依据是关键字而不是"有没有崩"。
ALIGN_HITS="$(tr -d '\r' < "${LOGCAT_FILE}" 2>/dev/null \
    | grep -icE "dlopen failed:.*program alignment|UnsatisfiedLinkError" || true)"
if [ "${ALIGN_HITS:-0}" -eq 0 ]; then
    add_row "16KB 库加载 (对齐/UnsatisfiedLink)" "0 条" "0 条" "PASS"
else
    add_row "16KB 库加载 (对齐/UnsatisfiedLink)" "${ALIGN_HITS} 条" "0 条" "FAIL"
    tr -d '\r' < "${LOGCAT_FILE}" \
        | grep -iE "dlopen failed:.*program alignment|UnsatisfiedLinkError" | head -2 | sed 's/^/     /'
fi

# 4.2 退出码 134 / SIGABRT：要区分"真失败"和"已知的上游 finalize GC 缺陷"。
# PySide6 6.12.0 + Py3.11 在进程退出阶段会因 QColor/QRect 临时对象的引用计数
# 在 finalize GC 里崩(SIGABRT=134)，与业务无关；这种情况下进程是"跑完才死"，
# 内存/截图都拿得到。所以：进程仍存活 = INFO（不影响结论），进程已死 = 由第 3 项判 FAIL。
SIG_HITS="$(tr -d '\r' < "${LOGCAT_FILE}" 2>/dev/null \
    | grep -icE "exit code 134|Fatal signal 6|SIGABRT" || true)"
if [ "${SIG_HITS:-0}" -gt 0 ]; then
    if [ -n "${PID}" ]; then
        add_row "退出码 134/SIGABRT" "${SIG_HITS} 条，进程仍存活" "仅提示" "INFO"
        echo "   注: 进程存活时按已知的 PySide6 6.12 finalize GC 缺陷处理，不判失败；"
        echo "       若进程已死则该 134 会被第 3 项(pidof)判 FAIL。"
    else
        add_row "退出码 134/SIGABRT" "${SIG_HITS} 条，进程已死" "0 条" "FAIL"
        tr -d '\r' < "${LOGCAT_FILE}" \
            | grep -iE "exit code 134|Fatal signal 6|SIGABRT" | head -2 | sed 's/^/     /'
    fi
fi

# ---------------------------------------------------------------- 5. PSS（只记录，不设阈值）
# TOTAL PSS 的取值行随 ROM 版本略有差异（有的还有 TOTAL RSS/SWAP），
# 所以只认 "TOTAL PSS:" 后面的第一个整数（单位 kB）。
PSS_KB="$(adbs shell dumpsys meminfo "${PKG}" 2>/dev/null | tr -d '\r' \
    | sed -n 's/.*TOTAL PSS: *\([0-9]\+\).*/\1/p' | head -1)"
if [ -n "${PSS_KB}" ]; then
    PSS_MB="$(awk -v k="${PSS_KB}" 'BEGIN {printf "%.1f", k/1024}')"
    add_row "内存 PSS (dumpsys meminfo)" "${PSS_MB} MB" "仅记录" "INFO"
else
    add_row "内存 PSS (dumpsys meminfo)" "取不到" "仅记录" "INFO"
fi

# ---------------------------------------------------------------- 6. 16KB 页兼容
# 复用 check_16kb_device.sh（页大小判断/兼容模式标记都在那边，别在这里另写一套）
KC_OUT="$(bash "${HERE}/tools/check_16kb_device.sh" --pkg "${PKG}" 2>&1)"
KC_RC=$?
KC_PAGE="$(printf '%s' "${KC_OUT}" | tr -d '\r' | sed -n 's/^页大小: *\([0-9]*\).*/\1/p' | head -1)"
# check_16kb_device.sh 只做"设备是不是 16KB"，被标记兼容模式时它给的是 [WARN]（rc 仍为 0）。
# 对**验收这个包**来说，"应用被标记为 16KB 兼容模式"= 包内还有 4KB 对齐的库 = 必须 FAIL，
# 所以在这一层把它提升为失败（否则会出现"设备 16KB -> 全绿"但其实根本没合规的假结论）。
if [ ${KC_RC} -eq 0 ] && printf '%s' "${KC_OUT}" | tr -d '\r' | grep -q "被标记为 16KB 兼容模式"; then
    add_row "16KB 页兼容 (check_16kb_device)" "PAGE_SIZE=${KC_PAGE:-?}，包被标记兼容" "PASS" "FAIL"
elif [ ${KC_RC} -eq 0 ]; then
    add_row "16KB 页兼容 (check_16kb_device)" "PAGE_SIZE=${KC_PAGE:-?}" "PASS" "PASS"
elif printf '%s' "${KC_OUT}" | grep -q "不是 16KB 页"; then
    if [ "${REQUIRE_16K}" -eq 1 ]; then
        add_row "16KB 页兼容 (check_16kb_device)" "PAGE_SIZE=${KC_PAGE:-?}（非 16KB）" "PASS" "FAIL"
    else
        add_row "16KB 页兼容 (check_16kb_device)" "PAGE_SIZE=${KC_PAGE:-?}（非 16KB 设备）" "PASS" "N/A"
    fi
else
    add_row "16KB 页兼容 (check_16kb_device)" "rc=${KC_RC}" "rc=0" "FAIL"
fi
printf '%s\n' "${KC_OUT}" | tail -8 | sed 's/^/   /'
echo "   完整 16KB 结论: bash android/tools/check_16kb_device.sh --pkg ${PKG}"

# ---------------------------------------------------------------- 7. 截图
SHOT="${OUT_DIR}/avd-${STAMP}-${ABI}.png"
if adbs exec-out screencap -p > "${SHOT}" 2>/dev/null && [ -s "${SHOT}" ]; then
    SHOT_BYTES="$(stat -c %s "${SHOT}")"
    if [ "${SHOT_BYTES}" -gt 1000 ]; then
        add_row "截图" "$(basename "${SHOT}") (${SHOT_BYTES} B)" "非空 PNG" "PASS"
    else
        add_row "截图" "${SHOT_BYTES} B（疑似空图）" "非空 PNG" "FAIL"
    fi
else
    add_row "截图" "失败" "非空 PNG" "FAIL"
fi

# ---------------------------------------------------------------- 汇总表
# 表格用 python 渲染：中文字符宽度是 2，bash 的 printf %-Ns 按"字节/字符数"补齐会错位，
# 表看不整齐就没人愿意看，结论也就传不下去。
echo
echo "=========================================================="
echo " 验收结果"
echo "=========================================================="
printf '%s\n' "${ROWS[@]}" | python3 -c '
import sys, unicodedata

def width(text):
    total = 0
    for ch in text:
        total += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return total

def pad(text, target):
    return text + " " * max(0, target - width(text))

rows = [line.rstrip("\n").split("|") for line in sys.stdin if line.strip()]
header = ["项目", "实测值", "阈值", "结论"]
body = [header] + rows
cols = [max(width(r[i]) if i < len(r) else 0 for r in body) for i in range(4)]
line = "-+-".join("-" * c for c in cols)
print(" | ".join(pad(header[i], cols[i]) for i in range(4)))
print(line)
for row in rows:
    cells = [(row[i] if i < len(row) else "") for i in range(4)]
    print(" | ".join(pad(cells[i], cols[i]) for i in range(4)))
' || echo "  （表格渲染失败，原始结果见上方各行）"
echo
echo " 截图: ${SHOT}"
echo " 日志: ${LOGCAT_FILE}"
if [ "${FAILED}" -eq 0 ]; then
    echo " 结论: PASS（16KB 一栏为 N/A 表示当前设备是 4KB 页，需 16KB AVD/真机另验）"
    exit 0
fi
echo " 结论: FAIL（${FAILED} 项不达标，见上表）"
exit 1
