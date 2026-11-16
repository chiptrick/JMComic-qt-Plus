# 任意可交互区域附近的「滑动 vs 点击」误触修复

日期：2026-11-15
涉及文件：`cpp/shell/ui/touch.cpp`、`touch.h`、`section_strip.cpp`、`ui_kit.cpp`、
`ui_main_window.cpp/.h`、`main_desktop.cpp`

> **后续修订（2026-11-15 第二轮）**：弹窗/二级子菜单的 mouse replay 会重置手势
> 轨迹，导致选项框上方滑动、二级菜单、下拉选项框、悬浮窗仍误触；已补 `Overlay`
> 作用域、手势序列续接与浮层"按下即动作"改造。见
> `docs/tap_vs_swipe_popups_overlay_fix.md`，阈值与作用域以该文及 `touch.h` 为准。
>
> 2026-11-13 的 `docs/tap_vs_swipe_gesture_fix.md` 只处理了下拉框 / QMenu / 侧栏，
> 本文是它的扩展与修订：把保护范围扩到**所有可交互区域**，并补齐真机触摸链路。
> 但本文的自检字段列表与 `swallowed` 计数已过时。

## 1. 现象

在手机上滑动经过（或只是靠近）可点元素时，松手经常被当成点击：

* 首页卡片 → 直接进详情页；
* 设置页整行开关 → 开关被拨动；
* 列表行 / 文字链 / 模型选择栏 → 直接执行；
* 侧栏按钮、下拉框、菜单项 → 跳页 / 换选项 / 执行菜单项。

短距离滑动（约 20~80 物理像素，384dpi 上约 8~33dp）最容易复现：页面还没开始滚，
控件已经收到一次"合法"的抬起。要求同时保证：**正常点击照常、正常滚动照常、
惯性滚动中途点一下用来刹车不该点进内容**。

## 2. 根本原因（桌面 + MuMu + Qt 探针实测）

### 2.1 控件的点击语义只有一条规则

`QAbstractButton::mouseReleaseEvent` 只要 `hitButton(release)` 为真就发 `clicked()`；
自绘控件更宽松 —— `ComicCard` / `SettingRow` / `SrModelField` 的
`mouseReleaseEvent` **只看抬起点是否落在自己的 rect 内，完全不检查按下是否发生在
自己身上**。从空白处起手、划到卡片上松手同样会 `emit Activated()`。

### 2.2 QScroller 的"夹层"

`QScroller` 的 `DragStartDistance` 约 10mm（Android 上约 40dp）。位移小于它时
既不滚动、也不产生任何"这是滑动"的信号，于是抬手就是一次标准点击。
这正是误触最集中的区间。

### 2.3 QScroller 拿下手势后仍把合成抬起发给控件

Qt 6.12 实测（`qprobe/t6~t10`，`QTest::touchEvent` + `QWindowSystemInterface`）：

* `QScroller::grabGesture` 注册的是 `QGestureRecognizer`（由 `QGestureManager` 驱动），
  不是普通事件过滤器；
* 手势成立后会向手指下的控件发一条**假释放**来取消按下态；
* 控件最终看到的是 `MousePress(起点) + MouseRelease(终点)`，中间**没有任何 MouseMove**；
* 因此只看"鼠标位移峰值"的旧判定在真机上恒为 0，"划出去再划回来"必被判成点击。

### 2.4 触摸流根本没进守卫

真机/Android 上 `TouchBegin/Update/End` 投递给 `QWidgetWindow`（`QObject`，不是
`QWidget`）。旧 `TapGuard::eventFilter` 开头就 `qobject_cast<QWidget *>(obj)`，
非 QWidget 直接放行 —— 整条原始触摸轨迹被丢弃。这是旧修复在桌面上有效、
到 Android 上失效的另一半原因。

### 2.5 保护范围只覆盖三类控件

旧 `ScopeOf()` 只认 `QAbstractButton` / `QMenu` / `QComboBox`；
`tapGuardItem` 属性虽然写在代码里，**全仓没有任何控件设置过**（死属性）。
于是卡片、设置整行、模型栏、文字链、`QAbstractItemView` 行全在守卫之外。

### 2.6 按下与抬起可能落在不同控件 / 不同窗口

弹窗容器按下后 grab 鼠标；鼠标事件被 `ignore()` 后还会向父控件传播（自绘卡片
不 accept → 同一条按下又到达 `QFrame` / `MainWindow`）。任何"看见一次按下就重置
状态"的写法都会把正在进行的手势清掉。

## 3. 修改涉及的关键逻辑

| 位置 | 改动 |
| --- | --- |
| `touch.cpp` `TapGuard::eventFilter` | 非 QWidget 的触摸事件单独走 `OnTouch()`；输入事件之外的类型直接放行 |
| `touch.cpp` `TouchTrack` / `OnTouch()` | 独立维护**原始触摸轨迹**：过程峰值、多指、手势是否触摸合成。只记录，永不吞 touch（吞了会掐掉滚动/捏合） |
| `touch.cpp` `ScopeOf()` / `IsTappable()` / `TappableAncestor()` | 作用域扩到 `Generic`（自绘可点）/ `Item`（列表行）；`tapGuardItem` / `toggleRow` / `clickable` 属性 + `WA_SetCursor + PointingHandCursor` 自动识别；文本输入、滑块、滚动区、QML 宿主显式排除 |
| `touch.cpp` `OnPress()` | 记录整条手势（滚动指纹、按下时刻、来源）；**已有手势时不重置**，避免事件传播把守卫清掉；空白起手也记一条"空手势"（`scope=None`） |
| `touch.cpp` `OnRelease()` | 峰值取 `max(鼠标峰值, 触摸峰值)`；五条判据合判；判成滑动后 `Settle()` 复位按下态 / 菜单高亮 / 下拉选中项，再吞掉抬起 |
| `touch.cpp` `ScrollerEngaged()` | 与 QScroller 的冲突消解（见 §5） |
| `touch.cpp` `TouchAdapter` 边缘返回手势 | 记住起手控件 `pressWidget_`，判明是横滑返回后解除按钮按下态；`fromTouch_` 区分来源 |
| `section_strip.cpp` | 声明 `tapGuardSelfManaged=true`：分区栏自带 8dp 仲裁，守卫必须完全让路 |
| `ui_kit.cpp` `ComicCard` | 显式 `tapGuardItem=true`（自绘，Qt 不认识它的点击语义） |
| `ui_main_window.cpp` `TapGuardSelfTest()` | 自检扩到 22 个记录字段（21 项断言），含真机触摸序列、多指、划出再划回、空白起手→卡片抬手；结果可回传字符串 |
| `main_desktop.cpp` Android 分支 | 新增 `--tapguard-selftest`（结果落盘，绕开部分 ROM 丢 logcat）与 `--tapguard-watch <秒>`（每 400ms 落盘 `page` + `swallowed`，供注入真实手势验收） |

## 4. 滑动与点击的判定条件、阈值

一次「按下 → 抬起」**只有五条判据全不成立**才算有效点击；任一条成立即整条手势按滑动处理。

| 判据 | 阈值 / 来源 | 说明 |
| --- | --- | --- |
| 位移峰值 | `kTapSlopDp = 8dp` | 取 `max(鼠标过程峰值, 原始触摸过程峰值, 首末差)`。划出去再划回来（首末差 ≈ 0）仍判滑动。`Touch::Dp()` 返回逻辑像素（Android 上 1 逻辑像素 ≈ 1dp），384dpi 下 8dp ≈ 19 物理像素 |
| 长按漂移 | `kTapHoldDriftMs = 500ms` 且 `kTapDriftDp = 4dp` | 按得久又缓慢挪动（慢慢拖列表）不算点击；长按不动仍是点击 |
| 手势期间滚动 | `ScrollFingerprint` + `lastScrollMs_` | 本次手势期间任一滚动区发生过位移（含 QScroller 拖出来的、含惯性），内容动过就不是点击 |
| 滚动静默期 | `kTapIgnoreAfterScrollMs = 250ms` | 刚滚过 / 惯性未收尾时的点击一律忽略（"想停滚动"不该点进某一项） |
| 多指 | `TouchTrack::multi` | 手势期间出现第二根手指，一律按非点击（本应用无双指点按语义） |

判成滑动后：吞掉 `MouseButtonRelease`（按钮不 `clicked`、菜单项不 `triggered`、
下拉列表不提交、自绘控件不发 `Activated`），并复位中途产生的视觉态。
`scope=None` 的空手势**只在抬手落点是可点控件时才吞**：空白→空白的普通滚动要放行，
否则会掐掉 QScroller 的收尾抬起，也不会污染 `swallowed` 统计。

## 5. 冲突消解

1. **QScroller 收尾（最关键）**：桌面鼠标路径若 QScroller 正在 `Dragging/Scrolling`，
   这条抬起是它结束手势、起惯性的唯一输入 —— 实测吞掉会停在 `Dragging`，页面只走到
   手指位移的一半且不再回弹。此时交还给它；它自己发的假释放已经取消按下态，点击本来
   不会发生。触摸路径相反：`QScroller` 只吃 touch，合成抬起与它无关，**必须吞**。
   按下时记 `touchOrigin`，抬起时用它区分两条路径。
2. **按钮 / 卡片 / 列表行不吞 move**：`QScroller` 靠转发的位移滚动，吞一条 move 就从
   "滑动"退化成"点了不动"。只有菜单与下拉弹窗列表吞 move（否则高亮跟手、二级菜单自弹）。
3. **下拉框**：某些样式在按下就 `showPopup()`，Windows 11 样式还会延迟 ~150ms 才出现，
   正好落在手指开始滑之后。`BeginDrag()` / `OnShow()` 两处都收起弹窗，并让外层滚动区
   自己补滚；`Settle()` 回滚 `currentIndex`。
4. **菜单**：`QMenu::mouseMoveEvent` 会立刻把 `activeAction` 设回来并启动 20ms 子菜单
   定时器，所以从判定滑动那刻起吞掉 move，且每次 `DragTo()` 都 `ClearMenu()` 一遍。
5. **自带仲裁的控件**：`SectionStrip` 声明 `tapGuardSelfManaged=true`，守卫完全让路 ——
   吞掉它的抬起会让 `EmitDragState(false)` 与惯性都不执行，停在"拖动中"。
6. **自己要用整条手势的控件**：`QLineEdit` / `QTextEdit` / `QSpinBox` / 滑块 /
   滚动区 / `QQuickWidget` 一律不参与仲裁（选词、拖光标、QML Flickable 都靠按下-移动-抬起）。
7. **边缘返回手势**：`TouchAdapter` 在起手点位于左边缘时接管横滑；判明方向后主动
   `setDown(false)`，避免按钮停在按下高亮。竖滑仍交还页面滚动。

## 6. 边界情况覆盖

| 边界 | 处理 / 覆盖方式 |
| --- | --- |
| 快速滑动（60ms 完成 360px） | 原始 TouchUpdate 峰值即时累计，不依赖鼠标 move；MuMu `fast_swipe` |
| 短距离滑动（8~25dp，未触发 QScroller） | 8dp 死区判据独立于 QScroller；MuMu `short_swipe` |
| 划出去再划回来（首末差 0） | 过程峰值；自检 `touch_back`、MuMu `out_and_back`（`motionevent DOWN/MOVE/MOVE/UP`） |
| 多点触控 / 捏合 | `TouchTrack::multi`；自检注入双指 `TouchUpdate` |
| 起手贴着可交互区域边缘 | 边缘内侧起手仍按可点手势处理、外侧起手记空手势；自检 `blank_to_card`、MuMu `edge_*` |
| 手势起手在空白处、抬手落在自绘控件 | 空手势 + 落点作用域复判；自检 `blank_to_card` / `tap_after_blank` |
| 惯性滚动未收尾时点一下 | 250ms 静默期 + 手势内滚动证据 |
| 长按不动 / 长按缓慢漂移 | 500ms + 4dp 双条件，长按不动仍算点击 |
| 按下与抬起不在同一控件 / 弹窗被销毁 | 应用级过滤器看整条序列；`QPointer` 防悬空；未收尾手势在下一次按下前 `Settle()` |
| 弹窗延时出现在滑动过程中 | `OnShow()` 里 `singleShot(0)` 收起 |
| QScroller 同时拖同一个容器 | 见 §5.1；`DragTo()` 里也检查 `sc->state()` 避免双倍滚动 |
| 守卫状态不卡住 | `tap_after_blank`、`click_after_swipe` 两项验证后续正常点击仍生效 |

## 7. 验证

### 7.1 桌面（本机后台，Windows / MinGW / Qt 6.12）

```bash
ninja -C cpp/shell/ui/build jmcomic_ui
jmcomic_ui.exe --touch --tapguard-selftest --quiet --shot out.png --shot-delay 6000
```

结果：`tapguard-selftest: pass`，21 项断言全为 1、`menu_skipped=0`（菜单段正常执行），`swallowed=11`。
关键项：`card_slide/card_tap/card_touch_tap`、`row_slide/row_tap`、`item_slide/item_tap`、
`touch_back`、`multi_touch`、`blank_to_card`、`tap_after_blank`。

其它回归：`--toggle-test 自动超分`（`before=1 after=0`，整行点击仍能拨开关）、
`--blank-report`（`toggleRows=5 sameLine=5 maxCenterDy=1`，布局未受影响）、
`cpp/tools/c1_ui_desktop_suite.py --only offline` → **PASS 76/76 / FAIL 0**。

### 7.2 MuMu 模拟器（x86_64 / Android 15 / 1080×2400 @384dpi）

打包：增量 `ninja -C build-android-x86_64 libjmcomic_ui_x86_64.so` →
`llvm-strip` → `cpp/tools/apk_repack.py` 替换主库（47 个 .so 全部 STORED + 16KB 对齐）→
`apksigner.jar` 用与设备上已装包一致的调试密钥签名 → `adb install -r`（保留数据）。

**应用内自检**（`am start ... --es jm.args "--tapguard-selftest"`）：
`result=pass`，21 项断言全为 1，`swallowed=11`；结果同时落盘
`/storage/emulated/0/Android/data/org.jmcomic.ui/files/tapguard-selftest.txt`
（MuMu 上 logcat 也完整，两条链路互相印证）。

**真实触摸手势**：每个场景重启应用，用 `--tapguard-watch 60` 每 400ms 把
`page` 与 `swallowed` 落盘；用 `adb shell input -d <应用所在虚拟 display> ...` 注入。
MuMu Nx 每次 `am start` 新建虚拟 display，必须由 `dumpsys window windows` 取逻辑
displayId、再由 `dumpsys display` 的 `mViewports` 换成 `uniqueId=local:<物理id>` 才能
`screencap -d`，`input -d` 则用逻辑 id。
目标：收藏页「去登录」按钮（约 `(540,1540)`，点击后 `page 7 → 13`）。

| 场景 | 操作 | 预期 | 实际 |
| --- | --- | --- | --- |
| 正常点击 | `tap 540 1540` | 切到登录页、不拦截 | `page 7→13`，`swallowed 0→0` ✅ |
| 长滑动 | `swipe 540 1540 → 540 1700 / 300ms` | 不切页、吞掉抬起 | `page=7`，`swallowed=1` ✅ |
| 快速滑动 | `swipe 540 1540 → 540 1900 / 60ms` | 不切页、吞掉抬起 | `page=7`，`swallowed=1` ✅ |
| 短距离滑动 | `swipe 540 1540 → 540 1600 / 200ms` | 不切页、吞掉抬起 | `page=7`，`swallowed=1` ✅ |
| 划出再划回 | `motionevent DOWN/MOVE/MOVE/UP` 首末同点 | 不切页、吞掉抬起 | `page=7`，`swallowed=1` ✅ |
| 滑动后正常点击 | 先长滑，再 `tap` | 守卫不卡住 | 滑动 `page=7 swallowed=1`，点击 `page=13` ✅ |
| 正常滚动 | 设置页空白区 `swipe 700 1900 → 700 900` | 内容滚动、不计入误触 | `page=11`，`swallowed=0`，画面差异面积 2428861 px² ✅ |
| 起手在按钮外侧（远离边缘） | `swipe 540 1450 → 540 1540` | 不切页 | `page=7`，`swallowed=0` ✅（Android 隐式鼠标抓取把抬起交回起手控件，本就没有点击可拦；跨控件抬手的场景由自检 `blank_to_card` 覆盖） |
| 起手紧贴按钮上边缘外侧 | `swipe 540 1478 → 540 1540` | 不切页、吞掉抬起 | `page=7`，`swallowed=1` ✅ |
| 起手贴按钮上边缘内侧 | `swipe 540 1492 → 540 1552 / 200ms` | 不切页、吞掉抬起 | `page=7`，`swallowed=1` ✅ |
| 起手贴按钮左边缘内侧 | `swipe 498 1540 → 560 1600 / 200ms` | 不切页、吞掉抬起 | `page=7`，`swallowed=1` ✅ |

**修复前 / 修复后 A/B（同一手势、同一设备）**：

先用收藏页「去登录」`QPushButton` 做 A/B，发现基线包也不会误触 —— 旧守卫本来就覆盖
`QAbstractButton`，且 QScroller 的假释放会让按钮的按下态失效，控件自己就不会发 clicked。
这恰好印证了「本次真正要补的是**不检查按下态的自绘控件**」。于是把 A/B 换到设置页
「开机自启」整行（`SettingRow`，自绘、只看 `mouseReleaseEvent`）：

| 版本 | 操作 | 开关粉色像素（ON ≈ 4157 / OFF ≈ 0） | `page` / `swallowed` | 结论 |
| --- | --- | --- | --- | --- |
| 基线 0.93 | `swipe 150 770 → 150 830 / 200ms`（60 物理像素 ≈ 25dp） | `4157 → 0`（被误拨），差异区域恰为开关 `5456 px²` | 无监视通道 | ❌ 复现误触 |
| 修复后 | 同上 | `4157 → 4157`（纹丝不动），画面差异 `0` | `page=11` / `swallowed=1` | ✅ 滑动被吞 |
| 修复后追加 | 紧接着 `tap 150 770` | `4157 → 0` | — | ✅ 正常点击照常、守卫不卡住 |

两次安装都用设备上原有的同一调试密钥签名（`install -r`，不清数据）；验收结束后把
开关恢复为初始 ON，并重新装回修复后包。

## 8. 已知限制

* 多指验收用应用内合成的 `QTouchEvent`（`input motionevent` 只能发单指），
  判定逻辑与真机双指流一致，但没有真机双指实测。
* 桌面鼠标路径在 QScroller 已接管时交还抬起（见 §5.1），此时依赖 QScroller 自己的
  假释放取消按下态；自绘控件不检查按下态，但 Qt 的隐式抓取会把抬起送回起手控件。
* `tapGuardSelfManaged` 需要控件自己保证仲裁正确，新增自带仲裁的自绘控件时必须显式声明。
