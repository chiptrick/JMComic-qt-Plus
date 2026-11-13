# 首页横向滑动 · 独立 DEMO

验证「首页分区左右滑动 + 分区/模块统一整页横向滑动转场」的手感与一致性。
**本目录是独立 CMake 工程，不引用、不修改 `cpp/shell` 里的任何文件。**

## 构建 / 运行

```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/6.12.0/mingw_64/bin:/c/Qt/Tools/Ninja:$PATH"
cd cpp/demo_swipe
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH=C:/Qt/6.12.0/mingw_64 \
      -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
ninja -C build
./build/jm_swipe_demo.exe
```

## 交互

| 操作 | 结果 |
| --- | --- |
| 内容区按住左右拖 | 分区跟手位移；松手按阈值/速度决定提交或回弹 |
| 点顶部分区栏 | 整页滑到该分区（程序切换，恒定 240ms） |
| 点左侧导航 | 整页滑到该模块（与分区同一条时间轴） |
| 点卡片 / 开关 | 正常触发；拖拽超过 12px 后不再触发（防误触） |
| 底部按钮 | 自动演示 / 边界回弹 / A/B 现状淡入 / 边界说明 |

## 参数（`SlidePager` 内可改）

| 项 | 值 | 说明 |
| --- | --- | --- |
| 转场时长 | 240ms | 分区与模块共用；对应 `Motion::Slow` |
| 缓冲曲线 | OutCubic | 与 `PageSwipe.qml` 位移一致 |
| 跟手 | 1:1 无缓动 | 位移 = 手指位移 |
| 离手提交 | 28% 宽度 或 0.35px/ms | 两者满足其一 |
| 离手补间 | `240 × 剩余距离`，夹到 [120,240]ms | 曲线不变 |
| 轴锁 | 12px，`|dx| > 1.2·|dy|` | 否则整段手势判为纵向 |
| 边界 | 橡皮筋 ×0.32 | 首/尾分区只回弹，不切模块 |
| 单次手势 | 最多一页 | 位移夹在 ±1 屏 |

## 回归

```bash
./build/jm_swipe_demo.exe --selftest          # 阈值 / 边界 / 仲裁 / 防误触，13 项
./build/jm_swipe_demo.exe --auto              # 自动走一遍
./build/jm_swipe_demo.exe --freeze 0.45 --shot a.png
./build/jm_swipe_demo.exe --level module --from 0 --to 2 --freeze 0.6 --shot b.png
./build/jm_swipe_demo.exe --rules --shot c.png
./build/jm_swipe_demo.exe --fade              # 以现状交叉淡入启动，做 A/B
```

## 与正式接入的对应关系

- 位移原语：本 demo 的 `offset_` 就是 `PageSwipe.qml` 的 `progress × width`；
  QML 侧已经支持 `freeze(p)` 逐帧写进度，缺的只是"由手势写"。
- 分区：`HomePage` 现在用 `Motion::CrossFade(tabs_, …, Fast)`（120ms）；替换为
  一份两页快照 + signed progress 的覆盖层即可，其余逻辑不动。
- 模块：`MainWindow::SwitchTo` 里 `NavMotion::Lateral → Mode::Fade`（180ms）
  改成 `Mode::Lateral + Direction`，时长取 `Motion::Slow`。
- 手势：需要在 `Touch::InstallScrollGestures` / `QScroller` 抢到 TouchBegin **之前**
  完成判轴，否则 `QScroller` 拿到手势后不再放行（本 demo 用应用级过滤器 + 显式
  `grabMouse()` 复现了同样的顺序，并用合成 release 收掉子控件按下态）。
