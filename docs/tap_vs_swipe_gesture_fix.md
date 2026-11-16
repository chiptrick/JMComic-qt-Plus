# 滑动 vs 点击：下拉选项框与二级菜单的误触修复

> **后续修订（2026-11-15，共两轮）**：保护范围已扩展到所有可交互区域，补齐
> Android 触摸链路，并修复了弹窗/二级子菜单 mouse replay 造成的残留误触。
> 最新阈值与作用域以 `docs/tap_vs_swipe_popups_overlay_fix.md` 和
> `cpp/shell/ui/touch.h` 的注释为准；本文仅作历史记录。

日期：2026-11-13
涉及文件：`cpp/shell/ui/touch.cpp`、`cpp/shell/ui/touch.h`、`cpp/shell/ui/ui_kit.cpp`、
`cpp/shell/ui/ui_kit.h`、`cpp/shell/ui/ui_main_window.cpp`

## 1. 现象

手指在下列位置滑动时，会误触发点击语义：

1. **带下拉选项框的按钮附近**：想滚动页面，一按在 `QComboBox` 上就弹列表；手指继续
   滑动，松手时落在某一项上 → `currentIndex` 被改掉。
2. **下拉选项框内部**：在弹窗列表里滑动，列表高亮跟着手指走，松手提交并关闭弹窗。
3. **二级菜单内部**：在 `QMenu`（阅读器菜单）里滑动，滑过带子菜单的项后子菜单自己
   弹出来；松手落在哪一项就执行哪一项。

同时要求：**正常点击必须照旧**（点按钮跳页、点下拉框弹列表、点菜单项执行、列表内点选生效）。

## 2. 根本原因

### 2.1 判定维度不足：只看"首末差"，不看过程

旧 `TapGuard` 用 `release − press` 的曼哈顿距离判断是否滑动。手指**划出去再划回来**
时首末差≈0；缓慢拖列表时单帧位移也一直小于 8dp。两类真实滑动都被判成点击。

### 2.2 守卫范围不含 `QComboBox` 本体与弹窗列表

`TapGuard` 只认 `QAbstractButton`、`QMenu` 和属性 `tapGuardItem`（**从未有任何代码设置
过**，是死属性）。而 `QComboBox` 不是 `QAbstractButton`：
`ui_pages_extra.cpp`（模型下拉）、`ui_pages_browse.cpp`（筛选下拉）用的是裸
`QComboBox`，完全在守卫之外。按下即弹窗（Windows 11 样式实测弹窗还会延迟 ~150ms
出现，正好落在手指已经开始滑动之后），随后的 move/release 被弹窗容器 grab，一松手
就是"选中手指下面那一项"。

### 2.3 菜单的 move 没有被拦

旧实现只在 move 里 `setActiveAction(nullptr)` 然后**继续放行**，`QMenu::mouseMoveEvent`
立刻把高亮设回来，并启动 20ms 的子菜单弹出定时器。于是"滑动过程中二级菜单被展开"，
且展开状态在滑动结束后残留。

### 2.4 按下与抬起不在同一个控件上

弹窗容器按下后会 grab 鼠标，按下发生在 `QComboBox`/`QMenu`，抬起却发生在弹窗容器或
子菜单上。旧的单点记录没有"整条手势"的概念，跨控件时判据全丢。

### 2.5 判定写了两份，行为不一致

`TouchCombo` + `ComboPopupGuard`（`ui_kit.cpp`）与 `TapGuard`（`touch.cpp`）各有一套阈值
和滚动实现，只有字体/主题两处下拉走了前者，其余下拉没有任何防护。

## 3. 修复思路

把判定收敛成**一处**（`touch.cpp` 的应用级 `TapGuard`），并把"一次手势"当成整体处理。

### 3.1 三档阈值（缺一不可）

| 阈值 | 值 | 作用 |
| --- | --- | --- |
| `kTapSlopDp` | 8dp | 取**过程峰值位移**判滑动；划出去再划回来同样算滑动 |
| `kTapHoldDriftMs` / `kTapDriftDp` | 500ms / 4dp | 长按且缓慢挪动不算点击；长按不动仍是点击 |
| `kTapIgnoreAfterScrollMs` | 250ms | 内容刚滚动过（含惯性未收尾）时的点击忽略，避免"想停滚动"点进某一项 |

### 3.2 作用域分类（`GuardScope`）

`Button` / `Menu` / `Combo` / `PopupList`。分类顺序有意义：**弹窗列表必须排在 `QComboBox`
之前**，否则弹窗内的按下会被当成"点在下拉框本体"，滑动时把整个弹窗藏掉。
判定用 `windowType()` 而不是 `windowFlags() & Qt::Popup` —— `Qt::Popup` 的枚举值是
`0x8 | Window`，与普通窗口按位与也成立，实测会把主窗口里所有按钮误判成弹窗列表。

### 3.3 判成滑动后的三件事

1. **吞掉抬起**：按钮不 `clicked`、菜单项不 `triggered`、下拉列表不提交。
2. **过程中不制造选中态**：
   * 菜单 / 弹窗列表的 `move` 直接吞掉（高亮不跟手，子菜单的 hover 定时器不会被续上）；
   * 弹窗列表需要滚动就自己改 `verticalScrollBar()`（无惯性，且给滚动值加
     `scrolling_` 标记，不参与 250ms 静默期，避免紧接着的正常点选被误伤）；
   * 下拉框本体：判定滑动的瞬间 `hidePopup()`；若弹窗是延时出现的，由 `QEvent::Show`
     钩子在它出现时立刻收回（否则它 grab 鼠标后抬手就会选中）；
   * 外层页面滚动：下拉框吃掉了 press，`QScroller` 拿不到，由守卫手动补；若
     `QScroller` 已在拖同一容器则不叠加（避免双倍速度）。
3. **结束后复位**：菜单 `activeAction` 归空并 `hide()` 残留子菜单、按钮 `setDown(false)`、
   弹窗列表 `currentIndex` 回到按下时的值。

### 3.4 触摸路径

`QComboBox` / `QMenu` 自身不接受触摸事件，Qt 会合成鼠标事件走同一套逻辑；对自行
接受触摸的控件，同一份判定由 `TouchBegin/Update/End` 分支接管。

## 4. 关键代码位置

| 位置 | 内容 |
| --- | --- |
| `touch.cpp:428-477` | 判定规则注释、`GuardScope` / `GestureState` |
| `touch.cpp:495` `PopupViewOf()` | 弹窗列表识别（`windowType()` 判据） |
| `touch.cpp:612` `ScopeOf()` | 作用域分类与优先级 |
| `touch.cpp:640` `OnPress()` | 手势开始、滚动指纹 |
| `touch.cpp:668` `OnMove()` | 峰值位移、转滑动 |
| `touch.cpp:689` `BeginDrag()` | 分类收尾动作（菜单清高亮 / 弹窗列表自滚 / 下拉框收弹窗） |
| `touch.cpp:729` `DragTo()` | 列表与页面手动滚动 |
| `touch.cpp:776` `OnRelease()` | 三档阈值合判、吞抬起、复位 |
| `touch.cpp:824` `OnTouch()` | 触摸事件路径 |
| `touch.cpp:842` `OnShow()` | 延时弹窗在滑动中出现的补救 |
| `touch.cpp:860` `ClearMenu()` / `877` `Settle()` | 菜单与列表状态复位 |
| `touch.h:72-96` | 三档阈值常量与说明 |
| `ui_kit.cpp` `TouchCombo` | 只保留指针样式；事件处理统一交给 `TapGuard` |
| `ui_main_window.cpp:3038` `TapGuardSelfTest()` | 回归自检（见下） |

## 5. 验证

### 5.1 自动自检

```bash
# 桌面（Windows / MinGW）
ninja -C cpp/shell/ui/build jmcomic_ui

# 有焦点跑一次（覆盖 QMenu：菜单是独立 popup 窗口，窗口失活会被系统收回）
cpp/shell/ui/build/jmcomic_ui.exe --tapguard-selftest --shot out.png --shot-delay 4500

# 常规回归（--quiet 时若菜单被系统收回，该段自动跳过：menu_skipped=1）
cpp/shell/ui/build/jmcomic_ui.exe --tapguard-selftest --quiet --shot out.png --shot-delay 4500
```

期望输出（全部为 1 即通过）：

```
tapguard-selftest: nav_drag=1 nav_release=1 back_swipe=1 nav_click=1
                   combo_swipe=1 combo_open=1 popup_swipe=1 popup_tap=1
                   menu_drag=1 menu_tap=1 menu_skipped=0 swallowed=5
tapguard-selftest: pass
```

自检逐项含义：

| 字段 | 断言 |
| --- | --- |
| `nav_drag` | 侧栏按钮上滑 24px 后抬手，页面不得跳转 |
| `nav_release` | 被吞掉抬手后按钮不得停在"按下"态 |
| `back_swipe` | 滑出 40px 再滑回原点（首末差 0）仍不得跳转 |
| `nav_click` | 位移为 0 的点击必须照常跳页 |
| `combo_swipe` | 在下拉框上（含延时弹出的弹窗）滑动后 `currentIndex` 不变、弹窗关闭 |
| `combo_open` | 正常点一下下拉框必须弹出列表 |
| `popup_swipe` | 列表内滑 72px 后不提交、列表不关闭、`currentIndex` 不变 |
| `popup_tap` | 列表内点选必须提交并关闭 |
| `menu_drag` | 按住子菜单项 280ms 再滑走：不 `triggered`、`activeAction` 为空、子菜单不可见 |
| `menu_tap` | 菜单项正常点击恰好 `triggered` 一次 |

### 5.2 手工验收（真机 / 桌面 `--touch`）

1. 设置页把"主题 / 字体"下拉滑一下：页面应滚动或不动，**不得**弹出列表、不得换值。
2. 点开字体下拉，在长列表里上下滑：只滚列表；松手不选中、不关闭；再点某一项才生效。
3. 阅读器打开菜单（工具条菜单按钮），在"阅读模式 / 缩放 / 超分"上滑过：不得展开
   子菜单、不得执行任何项；点空白关闭菜单后重开，菜单不应停留在被滑过的高亮项上。
4. 正常点击逐项验证：侧栏跳页、下拉展开、菜单项执行、列表点选均与修复前一致。

### 5.3 负向回归

* `--blank-report`、`--layout-dump`、`--page settings` 截图应与修复前一致（判定层不改布局）。
* 页面滚动（从按钮上起手拖动）不得失效：按钮作用域**不吞 move**，否则 `QScroller` 拿不到位移。
