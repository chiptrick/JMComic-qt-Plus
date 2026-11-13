#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# C++ 版：Linux 桌面构建（Qt 6.12 Widgets + Quick）
#
# 产物：cpp/shell/ui/build-linux/jmcomic_ui
#
# 依赖（Ubuntu/Debian 名称示例）：
#   * Qt 6.12（Core Gui Widgets Svg Network Quick QuickWidgets）
#     可用在线安装器，或发行版包：qt6-base-dev qt6-declarative-dev libqt6svg6-dev
#   * cmake >= 3.22、ninja-build、g++（C++17）、libgl1-mesa-dev
#
# 用法：
#   ./build_linux.sh                         # 自动找 qt-cmake / cmake
#   ./build_linux.sh --qt ~/Qt/6.12.0/gcc_64 # 指定 Qt 前缀
#   ./build_linux.sh --clean --jobs 8
#
# 说明：本脚本只负责"编译出可执行文件"。分发所需的 linuxdeploy/AppImage
# 打包不在本轮范围内（Python 版的 AppImage 流程见 python/build_linux.sh）。
# ---------------------------------------------------------------------------
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UI_DIR="${HERE}/shell/ui"
BUILD_DIR="${HERE}/shell/ui/build-linux"
QT_ROOT="${QT_ROOT:-}"
JOBS="$(nproc 2>/dev/null || echo 4)"
CLEAN=0

usage() {
    sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --qt) QT_ROOT="${2:-}"; shift 2 ;;
        --build-dir) BUILD_DIR="${2:-}"; shift 2 ;;
        --jobs|-j) JOBS="${2:-4}"; shift 2 ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage ;;
        *) echo "[x] 未知参数: $1"; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "[x] 本脚本只支持 Linux（当前: $(uname -s)）"
    exit 2
fi
if [[ ! -f "${UI_DIR}/CMakeLists.txt" ]]; then
    echo "[x] 找不到 ${UI_DIR}/CMakeLists.txt"
    exit 2
fi

# ---- 找 Qt / CMake / Ninja -------------------------------------------------
QMAKE=""
if [[ -n "${QT_ROOT}" ]]; then
    for cand in "${QT_ROOT}/bin/qt-cmake" "${QT_ROOT}/bin/qmake6" "${QT_ROOT}/bin/qmake"; do
        [[ -x "${cand}" ]] && QMAKE="${cand}" && break
    done
    if [[ -z "${QMAKE}" ]]; then
        echo "[x] ${QT_ROOT} 下找不到 qt-cmake/qmake6"; exit 2
    fi
else
    QMAKE="$(command -v qt-cmake || command -v qt-cmake-qt6 || command -v qmake6 || true)"
    if [[ -z "${QMAKE}" ]]; then
        echo "[x] 找不到 Qt（qt-cmake/qmake6）。用 --qt <Qt前缀> 指定，例如 ~/Qt/6.12.0/gcc_64"
        exit 2
    fi
    QT_ROOT="$(cd "$(dirname "${QMAKE}")/.." && pwd)"
fi

CMAKE="${CMAKE:-$(command -v cmake || true)}"
[[ -n "${CMAKE}" ]] || { echo "[x] 缺少 cmake"; exit 2; }
NINJA="${NINJA:-$(command -v ninja || true)}"
[[ -n "${NINJA}" ]] || { echo "[x] 缺少 ninja-build"; exit 2; }

echo "==> Qt     : ${QT_ROOT}"
echo "==> CMake  : ${CMAKE}"
echo "==> Ninja  : ${NINJA}"
echo "==> Build  : ${BUILD_DIR}"

if [[ "${CLEAN}" == "1" && -d "${BUILD_DIR}" ]]; then
    echo "==> clean"
    rm -rf "${BUILD_DIR}"
fi

echo "==> configure"
"${CMAKE}" -S "${UI_DIR}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="${QT_ROOT}" \
    -DCMAKE_MAKE_PROGRAM="${NINJA}"

echo "==> build (-j ${JOBS})"
"${CMAKE}" --build "${BUILD_DIR}" --parallel "${JOBS}"

BIN="${BUILD_DIR}/jmcomic_ui"
if [[ ! -x "${BIN}" ]]; then
    echo "[x] 构建结束但没找到可执行文件: ${BIN}"
    exit 1
fi
echo "==> OK: ${BIN}"
echo "    运行前确保 Qt 的 lib 在 LD_LIBRARY_PATH，或执行:"
echo "    LD_LIBRARY_PATH=${QT_ROOT}/lib:\$LD_LIBRARY_PATH ${BIN}"
