#!/usr/bin/env bash
# C1-P4 桌面闭环一键跑：协议级（纯 Python）+ 进程级（真实 PythonBridge）。
#
# 用法：
#   bash android/c1_shell/host_tests/run_host_tests.sh
#   QT_PREFIX=/opt/Qt/6.12.0/gcc_64 PYTHON=python3 bash .../run_host_tests.sh
#
# 退出码 0 = 两组测试全 PASS。
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
C1DIR=$(cd "$HERE/.." && pwd)
BUILD_DIR=${BUILD_DIR:-$HERE/build}

# ---- Python 解释器 ----
PYTHON=${PYTHON:-}
if [ -z "$PYTHON" ]; then
  for cand in python3 python; do
    if command -v "$cand" >/dev/null 2>&1; then PYTHON=$cand; break; fi
  done
fi
[ -n "$PYTHON" ] || { echo "[fail] 找不到 python3/python"; exit 1; }

echo "== 1/2 协议级测试（不需要 Qt） =="
"$PYTHON" "$C1DIR/host_test_net_client.py"

# ---- 宿主 Qt（需要 Core + Network；Android kit 不可用于宿主构建） ----
echo "== 2/2 进程级测试（链接真实 python_bridge.cpp） =="
QT_PREFIX=${QT_PREFIX:-}
if [ -z "$QT_PREFIX" ]; then
  for cand in "${QTDIR:-}" "$HOME/Qt/6.12.0/gcc_64" "/c/Qt/6.12.0/mingw_64" \
              "C:/Qt/6.12.0/mingw_64" "/opt/Qt/6.12.0/gcc_64"; do
    if [ -n "$cand" ] && [ -f "$cand/lib/cmake/Qt6/Qt6Config.cmake" ]; then
      QT_PREFIX=$cand
      break
    fi
  done
fi
[ -n "$QT_PREFIX" ] || {
  echo "[fail] 找不到宿主 Qt6（含 Qt6Config.cmake）；请用 QT_PREFIX=... 指定"
  exit 1
}

# Ninja 若不在 PATH，尝试 Qt 自带的那份（Windows 安装器常见布局）
CMAKE_EXTRA=()
if ! command -v ninja >/dev/null 2>&1 && [ -x "/c/Qt/Tools/Ninja/ninja.exe" ]; then
  CMAKE_EXTRA+=(-G Ninja -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe)
fi
if [ -x "/c/Qt/Tools/mingw1310_64/bin/g++.exe" ] && [ ! -x /usr/bin/g++ ]; then
  CMAKE_EXTRA+=(-DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe)
fi

cmake -S "$HERE" -B "$BUILD_DIR" -DCMAKE_PREFIX_PATH="$QT_PREFIX" ${CMAKE_EXTRA[@]+"${CMAKE_EXTRA[@]}"}
cmake --build "$BUILD_DIR"

EXE="$BUILD_DIR/host_test_python_bridge"
[ -x "$EXE" ] || EXE="$BUILD_DIR/host_test_python_bridge.exe"

# 运行时需要 Qt 的 dll/so 在搜索路径里
export PATH="$QT_PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$QT_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$EXE" "$PYTHON" "$C1DIR/bridge.py"
