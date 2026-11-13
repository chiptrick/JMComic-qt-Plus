# 首页分区横滑 + 同级整页转场（2026-11-13）

范围：首页分区切换（手势 / 点分区栏）、左侧导航的模块切换。
层级页（详情、阅读）仍是原来的 Push / Pop，不动。

## 结构

```
HomePage
└─ SectionSwipe（分区容器）
   └─ Pane[i] = [SectionStrip(本页选中项) + 分区内容]
```

- 每个分区是**一整页**，分区栏属于页：切换时内容与分区栏一起横向滑走。
  QTabWidget 只能同屏一页，所以被替换掉（`#HomeTabs` 的 QSS 成了无引用样式）。
- 模块切换仍是 `PageTransition`（两张整页快照 + QML 覆盖层），只把
  `NavMotion::Lateral` 从 `Mode::Fade` 改成按方向选 `Push` / `Pop`。

## 参数（分区与模块共用）

| 项 | 值 |
| --- | --- |
| 时长 | 240ms（`Motion::Slow`） |
| 曲线 | OutCubic（与 `PageSwipe.qml` 位移一致） |
| 跟手 | 位移 = 手指位移 |
| 提交阈值 | 28% 宽度 或 0.35px/ms |
| 离手补间 | `240 × 剩余/W`，夹到 [120,240]ms |
| 轴锁 | 12px，`|dx| > 1.2·|dy|` |
| 边界 | 橡皮筋 ×0.32，只回弹不换页 |
| 单次手势 | 最多一页（位移夹在 ±一屏） |
| 方向（模块） | 目标在导航列表更下方 → 从右滑入（`MainWindow::NavRank`） |

## 边界

1. 首/尾分区：橡皮筋，**不**级联到模块切换。
2. 分区栏区域：横滑仍是滚动标签（`SectionStrip` 原逻辑），点按 = 整页滑到该分区。
3. 纵向手势：12px 内判轴，`|dy|` 占优即整段放弃横滑，交给卡片墙滚动。
4. 触摸模式贴左缘 24dp 留给系统返回手势，不从那条窄带起手切分区。
5. 非相邻分区/模块：按方向整页滑一屏，不做多屏连滑。
6. 动画中再次按下：从当前进度接管（跨多屏的跳转直接落定后重开）。
7. 关动效（`--no-anim`）：时长归 0，手势与落点不变。

## 防误触（关键）

卡片墙是 `QQuickWidget`（QML MouseArea + GridView），抢走它的 press/release
会让 MouseArea 停在按下态，所以这里**只观察、不抢事件**：

- 判定横滑后调用 `ComicGrid::SetSwipeLock(true)`，`cardClicked / cardLongPress`
  直接丢弃这次 release；松手后 `QTimer::singleShot(0)` 解锁。
- 落在普通控件（"加载更多"按钮等）上的拖动，补发一次界外 release 收掉按下态。
  ⚠ 这条补发会经过应用级过滤器，必须用 `sendingCancel_` 屏蔽，否则会被自己
  当成"手势结束"提前结算（实测 offset 停在 0，不再位移）。

## 回归

```bash
jmcomic_ui.exe --home-freeze 0.45 --home-freeze-from 0 --home-freeze-to 1 \
               --shot frozen.png --shot-delay 20000
jmcomic_ui.exe --home-swipe -420 --shot landed.png --shot-delay 25000
jmcomic_ui.exe --no-anim --home-swipe -420 --shot noanim.png --shot-delay 25000
jmcomic_ui.exe --strip-tap 300 --shot tap.png --shot-delay 25000   # 打印 section
jmcomic_ui.exe --strip-drag 80 --shot drag.png --shot-delay 20000
jmcomic_ui.exe --anim-from home --page search --anim-fade --anim-progress 0.5 \
               --shot module.png --shot-delay 2200                  # mode=lateral
```

等待预算跟着 `--shot-delay` 走（接口慢时 H3 超时→TCP 回退实测 14s+）。

## 已知限制

- 每个分区页各有一份分区栏：标签溢出需要滚动时，两页的滚动偏移可能不同，
  滑动过程中"两组标签"的位置不会完全对齐（内容与选中项始终正确）。
- 分区内容里的 QML 手势不参与判轴：斜向拖动时卡片墙可能同时纵向滚动一点。
- 模块切换仍是快照方案：`capture` 实测 13~46ms（随首页卡片数变化），
  这是原有成本，本次没有新增。
