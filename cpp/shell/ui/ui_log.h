// 诊断日志出口。
//
// 为什么需要它：
//   * Windows 构建是 WIN32 子系统，没有控制台，qWarning/stderr 全部丢失；
//   * Android 上 stderr 也不进 logcat，但 Qt 默认的 qWarning 会被转到 logcat；
// 所以：Android 保持 Qt 默认（logcat 可见），桌面安装一个写文件的 handler，
// 同时保留 stderr（有控制台时能直接看）。
#pragma once

#include <QString>

namespace JmLog {

void Init();
QString Path();

}  // namespace JmLog
