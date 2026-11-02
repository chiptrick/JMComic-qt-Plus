#!/usr/bin/env bash
# C1 网络层桌面闭环：编译并运行 host_test_net_client（链接真实 net_client.cpp）。
#
# 用法：
#   bash android/c1_shell/host_tests/run_host_tests.sh
#   QT_PREFIX=/opt/Qt/6.12.0/gcc_64 bash .../run_host_tests.sh
#
# 退出码 0 = 全 PASS。
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
BUILD_DIR=${BUILD_DIR:-$HERE/build}

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

# Ninja / 编译器若不在 PATH，尝试 Qt 自带的那份（Windows 安装器常见布局）
CMAKE_EXTRA=()
if ! command -v ninja >/dev/null 2>&1 && [ -x "/c/Qt/Tools/Ninja/ninja.exe" ]; then
  CMAKE_EXTRA+=(-G Ninja -DCMAKE_MAKE_PROGRAM=C:/Qt/Tools/Ninja/ninja.exe)
fi
if [ -x "/c/Qt/Tools/mingw1310_64/bin/g++.exe" ] && [ ! -x /usr/bin/g++ ]; then
  CMAKE_EXTRA+=(-DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe)
fi

cmake -S "$HERE" -B "$BUILD_DIR" -DCMAKE_PREFIX_PATH="$QT_PREFIX" ${CMAKE_EXTRA[@]+"${CMAKE_EXTRA[@]}"}
cmake --build "$BUILD_DIR"

EXE="$BUILD_DIR/host_test_net_client"
[ -x "$EXE" ] || EXE="$BUILD_DIR/host_test_net_client.exe"

export PATH="$QT_PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$QT_PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$EXE"
