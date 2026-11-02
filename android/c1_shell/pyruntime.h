// C1-W6：Python 运行时准备 —— 找到打包在 APK 里的 libpythonbin/libpybundle，
// 解出 _python_bundle 并设置子进程所需的环境变量。
//
// 约定（与 p4a 的 start.c/PythonUtil 一致）：
//   * libpythonbin.so 是真正的 CPython 可执行入口（PIE executable）；
//   * libpybundle.so 是 tar.gz 容器，内容是 _python_bundle/{stdlib.zip,modules,site-packages}；
//   * PYTHONPATH 指向 stdlib.zip + modules + site-packages；
//   * 子进程的 LD_LIBRARY_PATH 指向 APK 的 nativeLibraryDir。
#pragma once

#include <QString>

namespace PyRuntime {

// 成功返回 true，并把可执行的 libpythonbin 路径写到 pyBin；失败时 err 说明原因。
bool EnsureReady(QString *pyBin, QString *err);

}  // namespace PyRuntime
