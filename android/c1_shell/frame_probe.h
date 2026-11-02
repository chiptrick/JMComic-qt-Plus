// C1 性能探针：翻页耗时（解码 + 双页合成 + 缩放 + 提交）。
//
// 为什么不是 fps：
//   * `dumpsys gfxinfo` 只统计 HWUI 渲染的帧，C1 是 Qt 自绘，实测只报 1 帧；
//   * `SurfaceFlinger --latency` 在本项目真机上只回刷新周期，不给帧时间戳；
//   * 更关键的是——C1 的翻页是"一次性重绘"，没有连续动画，静态页的 fps 本身无定义。
//   所以测真正决定体感、且可测的量：一次翻页的端到端耗时。
//
// 开关：应用私有目录存在 `fps_log` 标记，或 JM_C1_FPS=1。未开启时 Enabled() 直接返回，
// 调用点只多一次静态布尔判断，零分配。
//
// 输出（tag C1Fps，每 3 秒汇总一次，避免刷屏）：
//   C1Fps page-turns n=20 p50=6.2ms p95=9.8ms max=14.1ms
#pragma once

namespace FrameProbe {

// 标记/环境变量命中即启用（结果缓存，热路径无文件系统访问）
bool Enabled();

// 记录一次翻页耗时（毫秒）。未启用时为空操作。
void LogPageTurn(double ms);

}  // namespace FrameProbe
