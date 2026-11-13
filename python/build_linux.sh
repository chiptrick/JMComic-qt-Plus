#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Python 版：Linux 桌面构建（PyInstaller + deb / AppImage）
#
# 本仓库**只提供 Python 版的 Linux 构建脚本**；Windows 桌面构建不提供。
# （Android 打包是另一条链路：python/android/build_android.sh，同样只支持 Linux/macOS 宿主）
#
# 用法：
#   ./build_linux.sh                      # 全量构建（含超分）+ deb 包
#   ./build_linux.sh --mode nosr          # 无超分版本
#   ./build_linux.sh --mode both          # 两个版本都出
#   ./build_linux.sh --package appimage   # 出 AppImage（需要 appimagetool）
#   ./build_linux.sh --no-venv            # 用当前 Python 环境（需自行装依赖）
#   ./build_linux.sh --version v1.3.2 --clean
#
# 依赖（Ubuntu/Debian 名称示例）：
#   * python3 + venv（默认会建 python/.venv-build）
#   * dpkg-deb（deb 包） / appimagetool-x86_64.AppImage 在 PATH（AppImage）
#   * 系统库：libgl1、libxkbcommon0、libegl1（PySide6 运行期需要）
#
# 产物：python/dist/
#   jmcomic_<版本>_ubuntu_amd64.deb / _nosr_amd64.deb
#   JMComic-x86_64.AppImage（--package appimage）
# ---------------------------------------------------------------------------
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="${HERE}/src"
DIST="${HERE}/dist"
VERSION="v1.3.2"
MODE="full"          # full | nosr | both
PACKAGE="deb"        # deb | appimage | none
USE_VENV=1
VENV="${HERE}/.venv-build"
CLEAN=0

usage() { sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="${2:-}"; shift 2 ;;
        --package) PACKAGE="${2:-}"; shift 2 ;;
        --version) VERSION="${2:-}"; shift 2 ;;
        --venv) VENV="${2:-}"; shift 2 ;;
        --no-venv) USE_VENV=0; shift ;;
        --clean) CLEAN=1; shift ;;
        -h|--help) usage ;;
        *) echo "[x] 未知参数: $1"; exit 2 ;;
    esac
done

if [[ "$(uname -s)" != "Linux" ]]; then
    echo "[x] 本脚本只支持 Linux（当前: $(uname -s)）。Python 版不提供 Windows 构建脚本。"
    exit 2
fi
case "${MODE}" in full|nosr|both) ;; *) echo "[x] --mode 只能是 full/nosr/both"; exit 2 ;; esac
case "${PACKAGE}" in deb|appimage|none) ;; *) echo "[x] --package 只能是 deb/appimage/none"; exit 2 ;; esac
if [[ ! -f "${SRC}/start.py" ]]; then
    echo "[x] 找不到 ${SRC}/start.py（仓库结构异常）"; exit 2
fi

PYTHON_BIN="${PYTHON_BIN:-python3}"
if [[ "${USE_VENV}" == "1" ]]; then
    if [[ ! -x "${VENV}/bin/python" ]]; then
        echo "==> 创建 venv: ${VENV}"
        "${PYTHON_BIN}" -m venv "${VENV}"
    fi
    PY="${VENV}/bin/python"
    PIP="${VENV}/bin/pip"
else
    PY="${PYTHON_BIN}"
    PIP="${PYTHON_BIN} -m pip"
fi

echo "==> Python : $("${PY}" -V 2>&1)"
echo "==> mode   : ${MODE}, package=${PACKAGE}, version=${VERSION}"

if [[ "${CLEAN}" == "1" ]]; then
    echo "==> clean dist/build"
    rm -rf "${DIST}" "${HERE}/build/pyinstaller"
fi
mkdir -p "${DIST}" "${HERE}/build"

# 生成资源模块（res/images.qrc -> src/images_rc.py）
echo "==> 生成 images_rc.py"
"${PY}" "${HERE}/script/build_qrc.py"

if [[ "${USE_VENV}" == "1" ]]; then
    echo "==> 安装构建依赖"
    ${PIP} install --upgrade pip >/dev/null
    ${PIP} install pyinstaller
fi

build_variant() {
    local variant="$1" req="$2" suffix="$3"
    echo "==> [${variant}] 安装运行依赖: ${req}"
    ${PIP} install -r "${req}"

    local out="${DIST}/${variant}"
    local work="${HERE}/build/pyinstaller-${variant}"
    rm -rf "${out}" "${work}" "${SRC}/build" "${SRC}/dist"

    echo "==> [${variant}] PyInstaller"
    (
        cd "${SRC}"
        "${PY}" -m PyInstaller --noconfirm --clean --windowed \
            --hidden-import=_cffi_backend \
            --collect-data curl_cffi \
            --add-data "${HERE}/lib/linux/*:." \
            --distpath "${out}" \
            --workpath "${work}" \
            start.py
    )
    local appdir="${out}/start"
    [[ -d "${appdir}" ]] || { echo "[x] PyInstaller 产物缺失: ${appdir}"; exit 1; }

    case "${PACKAGE}" in
        none) ;;
        deb)
            command -v dpkg-deb >/dev/null || { echo "[x] 缺少 dpkg-deb"; exit 2; }
            local pkg="${HERE}/build/deb-${variant}"
            rm -rf "${pkg}"
            cp -r "${HERE}/res/ubuntu" "${pkg}"
            mkdir -p "${pkg}/opt/JMComic"
            cp -r "${appdir}/." "${pkg}/opt/JMComic/"
            chmod -R 755 "${pkg}"
            local deb="${DIST}/jmcomic_${VERSION}_ubuntu_${suffix}amd64.deb"
            dpkg-deb --root-owner-group -b "${pkg}" "${deb}"
            echo "==> [${variant}] deb: ${deb}"
            ;;
        appimage)
            local tool
            tool="$(command -v appimagetool-x86_64.AppImage || command -v appimagetool || true)"
            [[ -n "${tool}" ]] || { echo "[x] 缺少 appimagetool"; exit 2; }
            local app="${HERE}/build/appimage-${variant}"
            rm -rf "${app}"
            cp -r "${HERE}/res/appimage" "${app}"
            mkdir -p "${app}/usr/bin"
            cp -r "${appdir}/." "${app}/usr/bin/"
            cp "${HERE}/res/icon/logo_round.png" "${app}/JMComic.png"
            chmod +x "${app}/AppRun" "${app}/usr/bin/start"
            local img="${DIST}/JMComic-${variant}-x86_64.AppImage"
            ( cd "${HERE}/build" && "${tool}" "${app}" "${img}" )
            echo "==> [${variant}] AppImage: ${img}"
            ;;
    esac
}

case "${MODE}" in
    full) build_variant full "${SRC}/requirements.txt" "" ;;
    nosr) build_variant nosr "${SRC}/requirements_nosr.txt" "nosr_" ;;
    both)
        build_variant nosr "${SRC}/requirements_nosr.txt" "nosr_"
        build_variant full "${SRC}/requirements.txt" ""
        ;;
esac

echo "==> 完成，产物在 ${DIST}"
ls -lh "${DIST}" 2>/dev/null || true
