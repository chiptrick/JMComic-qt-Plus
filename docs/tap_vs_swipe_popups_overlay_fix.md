# 弹出类控件（二级菜单 / 下拉选项框 / 悬浮窗 / 悬浮菜单）残留误触修复

日期：2026-11-15
涉及文件：`cpp/shell/ui/touch.cpp`、`touch.h`、`ui_kit.cpp/.h`、`sr_model_sheet.cpp`、
`proxy_page.cpp/.h`、`ui_main_window.cpp`、`main_desktop.cpp`

> 本文是 `docs/tap_vs_swipe_any_interactive_fix.md` 的续篇。上一轮把保护范围扩到
> 「任意可交互区域」并补齐了 Android 触摸流，但测试仍发现**选项框上方滑动**、
> **二级菜单**、**下拉选项框**在弹窗出现后会误触。本文说明这次补的链路、覆盖范围
> 与验证方式；阈值与作用域命名以本文和 `touch.h` 注释为准。

## 1. 现象与复现

| 场景 | 现象 |
|---|---|
| 在下拉「选项框上方」起手，滑进已弹出的选项列表再松手 | 主题/筛选值被改成手指最后停的那一项 |
| 主菜单里按住带子菜单的项，子菜单弹出后划过去松手 | 子菜单项被 `triggered`（甚至触发两次） |
| 悬浮窗（模型选择浮层、模态框、高级抽屉）遮罩上滑一下 | 浮层被"点"关闭，或顺手选中一行/拨动开关 |

用独立 Qt 探针（Qt 6.12.0 / MinGW，`qprobe/popup_probe2.cpp`）逐行复刻修复前的
判定逻辑，在同一个真实 `QComboBox::showPopup()` / `QMenu` 二级菜单上注入完整
press → move → release 序列：

```
S1 combo popup: press above -> drag -> popup replay press -> release
  [G] PRESS  QWidget peak=0 (reset)        <- 起手在选项框上方
  [G] DRAG   peak=54 swallowMove=1          <- 已经判定为滑动
  [G] PRESS  QWidget peak=0 (reset)        <- popup 补发按下：轨迹被清零
  [G] RELEASE peak=6 end=6 drag=0 -> click
  S1 result: currentIndex=2 (expected 0), popup=0     ← 误选复现

S2 submenu: press parent -> hover opens sub -> replay press -> release
  [G] PRESS  QMenu peak=0 (reset)
  sub visible=1
  [G] DRAG   peak=30 swallowMove=1
  [G] PRESS  QMenu peak=0 (reset)          <- 子菜单补发按下
  [G] RELEASE peak=5 end=5 drag=0 -> click
  S2 result: fired=2 (expected 0), subVisible=0        ← 菜单项误执行复现
```

同时核对 Qt 源码（`qtbase/6.8` 的 `qcombobox.cpp` / `qmenu.cpp`，6.12 行为一致）：

- `QComboBoxPrivateContainer::setItemView()` 把容器自己的 `eventFilter` 装在
  `view` 与 `view->viewport()` 上；`MouseMove` 分支会 `view->setCurrentIndex(indexUnderMouse)`
  （高亮跟手），`MouseButtonRelease` 分支在条件满足时直接 `hidePopup()` +
  `emit itemSelected(view->currentIndex())` 并 `return true`。
- `QMenu::mousePressEvent()` 会 `setCurrentAction(action, 20)` —— 20ms 后 hover 定时器
  弹出二级子菜单；`QMenu::mouseReleaseEvent()` 直接 `activateAction(..., Trigger)`。
- 两者在 popup 拿到 grab / 子菜单弹出时都会**补发一条 `MouseButtonPress`**
  （mouse replay）。

## 2. 根本原因

1. **手势轨迹被补发按下清零**（主因）。旧实现把每条 `MouseButtonPress` 都当成
   「一次全新点击」，`pressGlobal / pressMs / maxMoved` 全部重置。滑动前半段的
   几十像素被洗掉，抬手只剩 5~6px，落在 8dp 阈值内 → 判成点击 → Qt 容器 filter
   / QMenu 直接提交。上一轮的「过程峰值」只覆盖了 `press → release` 之间，跨不过
   中途插入的按下。
2. **只吞抬起，拦不住「按下即动作」**。`QComboBox` 在 press 就 `showPopup()`，
   `QMenu` 在 press 就起子菜单定时器；项目自绘的 `ModalDialog` 遮罩在
   `mousePressEvent` 里直接 `Finish(false)`，`ProxyPage` 的「点面板外收起」也在
   press（且它的应用级过滤器**安装得比 TapGuard 晚、执行得比 TapGuard 早**）。
   这些副作用在按下那一刻已经发生，吞掉抬起为时已晚。
3. **作用域白名单漏了自绘浮层**。上一轮 `ScopeOf` 只认 Button / Item / Generic /
   Menu / Combo / PopupList；`SrModelSheetWidget`、`ModalDialog`、
   `ProxyAdvOverlay` 都是**嵌在主窗口里的普通 QWidget**（不是 `Qt::Popup` 顶层
   窗口），它们的遮罩、吸顶分组条、浮条、容器本身被判成 `None`，完全不参与仲裁。
4. **清空菜单只清了按下时那一个 QMenu**。抬手可能落在刚弹出的二级子菜单上，
   子菜单自己的 `activeAction` / hover 定时器仍然有效。

## 3. 修改范围

### 3.1 触摸层（`touch.cpp` / `touch.h`）

| 改动 | 说明 |
|---|---|
| `GuardScope::Overlay` | 弹出类顶层窗口与自绘浮层的兜底作用域 |
| `IsPopupWindow` / `OverlayAncestor` / `IsPopupContext` | 顶层窗口按 `windowType()` 判 `Popup/ToolTip/Tool`；自绘浮层靠 `tapGuardOverlay=true` 属性沿父链识别 |
| `ScopeOf` 兜底 | 具体作用域（Menu/PopupList/Button/Item/…）优先，剩下的只要落在浮层/popup 里一律给 `Overlay`，不再逐控件列白名单 |
| `GestureSequence` + `GestureState::contaminated` | 维护「一根手指的完整序列」累计峰值；补发按下不重置轨迹，整条强制按滑动算 |
| `OnPress` 续接分支 | 命中续接时：保留序列峰值、标 `contaminated`；目标在弹出类窗口则**连按下一起吞**（阻止 showPopup / 子菜单定时器 / 遮罩关闭）；普通页面控件的新按下无条件开新序列 |
| `OnShow` 扩展 | 手势进行中弹出的窗口：标污染、记 `pendingPopup_`、收起 QComboBox 延时弹窗、收起 hover 弹出的二级子菜单；手势结束后用户主动点开的弹窗不干预 |
| `ClearMenuChain()` | 遍历所有顶层 `QMenu`，清 `activeAction` 并 hide 子菜单链（主菜单保持可见） |
| `OnRelease` | 峰值并入序列峰值；`contaminated`/`swiping` 强制滑动；弹窗路径**不**把抬起交还 QScroller |
| 冲突消解 | 桌面鼠标 + QScroller 正在拖 + 非弹窗 → 交还抬起（起惯性）；触摸路径 / 弹窗路径 → 必须吞 |
| 可测性 | `Touch::TapGuardTrace()`（最近 24 条判定，watch 落盘）、`TapGuardScopeOf()`（覆盖清单）、`SetTapGuardLegacy()`（A/B 对照） |

### 3.2 控件侧

| 文件 | 改动 |
|---|---|
| `ui_kit.cpp/.h` | `ModalDialog` 声明 `tapGuardOverlay`；遮罩「按下即关闭」改成 `mousePressEvent` 记录 + `mouseReleaseEvent` 在位移 ≤8dp 时才 `Finish(false)`（两道保险：滑动的抬起已被吞，8dp 兜底） |
| `sr_model_sheet.cpp` | `SrModelSheetWidget` 声明 `tapGuardOverlay`（覆盖模型列表、吸顶分组条、「继续测试」浮条、遮罩） |
| `proxy_page.cpp/.h` | `ProxyAdvOverlay` 声明 `tapGuardOverlay`；「点面板外收起」由 press 改为 **release 且位移 ≤8dp**（同时处理 MouseButtonPress/Release 与 TouchBegin/TouchEnd/Cancel，保留真机上 QScroller 吃 touch 的兼容路径） |
| `ui_main_window.cpp` | 自检新增第 10~13 段（见 §6） |
| `main_desktop.cpp` | 新增 `--tapguard-legacy`（桌面 + Android `jm.args`）；`--tapguard-watch` 报告增加 `trace:` 段落盘 |

### 3.3 全项目可交互控件覆盖清单

| 类别 | 实例 | 作用域 / 处理方式 |
|---|---|---|
| 标准按钮 / 开关 | `QPushButton`、`QToolButton`、`QCheckBox`、`ToggleSwitch` | `Button`，额外解除按下态 |
| 一级菜单 | 侧栏/阅读器菜单、右键菜单、地区菜单 | `Menu` |
| 二级、三级子菜单 | 阅读器菜单的「章节 / 阅读模式 / 缩放 / 自动滚动设置 / 超分 / 图片」等 | `Menu` + `ClearMenuChain()` 整链清理 |
| 下拉框本体 | `QComboBox#Filter`、`#SettingCombo`、各页面筛选框 | `Combo`（滑动时收起延时弹窗、外层页面补滚） |
| 下拉弹窗列表 | `QComboBoxPrivateContainer` / `QComboBoxListView` / viewport | `PopupList`（吞 move 防高亮跟手、手动滚列表、回滚 `currentIndex`） |
| 列表行 | `QListWidget`、`ComicListView`、`SrModelList` 的 viewport | `Item` |
| 自绘可点控件 | `ComicCard`、`SettingRow`、`SrModelField`、文字链 | `Generic`（`tapGuardItem` / `toggleRow` / `WA_SetCursor` 手型光标） |
| 悬浮窗 | `SrModelSheetWidget`、`ModalDialog`、`ProxyAdvOverlay` | `Overlay`（属性声明 + 浮层内所有控件兜底） |
| 悬浮菜单 | 浮层之上弹出的 `QMenu` | `Menu` + `Overlay` 兜底 |
| 浮层内浮条/吸顶条 | `SrBenchResume`、`SrStickyBand` | `Overlay`；`SrStickyBand` 本身 `WA_TransparentForMouseEvents`，不接收事件 |
| 自管控件 | `SectionStrip` | `tapGuardSelfManaged=true`，内部已有 8dp 仲裁，守卫完全让路 |
| 明确排除 | `QLineEdit`/`QTextEdit`/`QAbstractSpinBox`/`QAbstractSlider`/`QScrollBar`/`QML` 宿主 | 这些控件自己要用完整的按下-移动-抬起（选词、拖光标、拖滑块、QML 手势），吞事件会破坏交互 |

覆盖清单由自检在**整棵窗口树**（含未显示的页面）上运行时扫描，逐个调用
`TapGuardScopeOf()` 断言不为 `None`。

## 4. 触发 / 滑动的判定条件与阈值

一次「按下 → 抬起」只有**全部不满足**下列滑动判据才算点击，否则整条手势按滑动处理：

| 判据 | 阈值 / 条件 | 说明 |
|---|---|---|
| 位移峰值 | `kTapSlopDp = 8dp` | 取 `max(鼠标峰值, 原始 TouchUpdate 峰值, 序列累计峰值)`，不是首末差 |
| 长按漂移 | `kTapHoldDriftMs = 500ms` 且 `kTapDriftDp = 4dp` | 按得久又缓慢挪动算滑动；长按不动仍是点击 |
| 滚动证据 | 按下→抬起期间滚动指纹变化，或任一滚动区动过 | 含 QScroller 拖出来的、含惯性未收尾 |
| 惯性静默期 | `kTapIgnoreAfterScrollMs = 250ms` | 「想停滚动」不该点进某一项 |
| 多指 | 手势期间出现第二根手指 | 本应用无双指点按语义 |
| **序列污染** | 手势期间弹窗出现 / 弹出类窗口补发按下 | **强制按滑动算**，本轮新增 |
| **序列续接窗口** | `kTapSequenceMs = 300ms` | 只作用于「滑动期间新出现的弹出类窗口」；普通控件上的新按下无条件开新序列，滑完立刻点按钮/点菜单项不受影响 |

`Touch::Dp()` 返回逻辑像素（按窗口 DPR 取整）；MuMu 384dpi 下 8dp ≈ 19 物理像素。

判成滑动后的动作：吞掉抬起 + **吞掉污染序列里的补发按下** + 菜单/下拉列表吞 move
（按钮/卡片/浮层不吞 move，保证滚动）+ `Settle()` 复位按钮按下态、菜单整链
`activeAction`/子菜单、下拉列表 `currentIndex`。

## 5. 冲突消解与边界情况

- **QScroller**：桌面鼠标路径且 QScroller 正在拖同一容器时交还抬起（它靠这条事件
  收尾并起惯性，且自己会发假释放取消点击）；触摸路径与所有弹窗路径绝不交还。
- **弹窗延时出现**：`QComboBox` 某些样式在按下后 50~150ms 才 `showPopup`，
  `BeginDrag()` 与 `OnShow()` 两处都收起；`pendingPopup_` 让随后补发的按下按同一
  序列处理。
- **二级菜单 hover 定时器**：它不依赖新的 move，`BeginDrag()` / `DragTo()` /
  `OnShow()` / `Settle()` 四处都调用 `ClearMenuChain()`。
- **按下即动作**：控件侧统一改成抬起才执行；TapGuard 同时在污染序列里吞掉弹窗
  的补发按下，双保险。
- **跨控件 / 跨窗口**：空白处起手也记「空手势」；已有手势时不重置（鼠标事件被
  ignore 后会向父控件传播）；抬手落在可点控件才吞，空白→空白的普通滚动放行。
- 已覆盖：快速滑动、短距离（6px 级）滑动、划出去再划回来、多点触控、手势起手
  位于可交互区域边缘内外、惯性未收尾时点击、滑完立刻正常点击、正常点开下拉框
  并选一项、正常点一级/悬浮菜单项、正常点遮罩关闭浮层、弹窗销毁/窗口切换。

## 6. 验证方式

### 6.1 桌面（本机后台）

```
# 全控件自检（注意自检含弹窗/动画等待，--shot-delay 需 ≥ 12000）
jmcomic_ui.exe --touch --tapguard-selftest --quiet --shot out.png --shot-delay 12000
# 修复前对照：同一套用例必须 fail
jmcomic_ui.exe --touch --tapguard-legacy --tapguard-selftest --quiet --shot out.png --shot-delay 12000
# 其它回归
jmcomic_ui.exe --touch --toggle-test "自动超分" --quiet --shot out.png --shot-delay 2500
jmcomic_ui.exe --touch --blank-report --quiet --shot out.png --shot-delay 2500
python cpp/tools/c1_ui_desktop_suite.py --only offline
```

`--tapguard-selftest` 现在输出 42 个字段（含 `swallowed` / `style`、两个
`*_skipped` 与清单字段），其中本轮新增：

| 字段 | 场景 | 预期 |
|---|---|---|
| `replay_popup` / `combo_replay` / `combo_replay_tap` | 选项框上方起手 → 滑入列表 → popup 补发按下 → 抬手；随后正常点选 | 1 / 1 / 1 |
| `sub_replay` / `sub_replay_tap` / `replay_menu_skipped` | 二级子菜单 hover 弹出 → 补发按下 → 抬手；随后正常点一级项 | 1 / 1 / 0 |
| `overlay_swipe` / `overlay_tap` | 模态浮层遮罩上滑动不关闭；正常点遮罩关闭 | 1 / 1 |
| `float_menu` | 悬浮窗之上的悬浮菜单滑动不 triggered、正常点选触发 | 1 |
| `inventory=A/B` / `inventory_miss` | 整棵窗口树的可交互控件覆盖清单 | `303/303` / `-` |

**实测结果**

| 项目 | 预期 | 实际 |
|---|---|---|
| 修复后自检 | pass | **pass**；所有布尔断言为 1（`menu_skipped=0`、`replay_menu_skipped=0`），`inventory=303/303`、`inventory_miss=-`，`swallowed=16` |
| `--tapguard-legacy` 对照 | fail | **fail**；`combo_replay=0`、`sub_replay=0`、`sub_replay_tap=0`、`overlay_swipe=0`，其余原有字段仍全 1 → 证明新用例精确测的是本轮补的链路 |
| `--toggle-test 自动超分` | 整行点击仍生效 | `before=0 after=1` |
| `--blank-report` | 开关行不散架 | `toggleRows=5 sameLine=5 maxCenterDy=1` |
| 桌面离线回归套件 | 全绿 | **PASS 76 / FAIL 0 / SKIP 0** |

### 6.2 MuMu 模拟器

APK 用 `ninja -C build-android-x86_64 → llvm-strip → apk_repack.py → apksigner.jar`
重打包，47 个 `.so` 全部 STORED + 16KB 对齐，`install -r` 覆盖安装（签名一致、
保留数据）。MuMu 每次 `am start` 新建虚拟 display，手势注入用
`dumpsys window windows` 的逻辑 display id，截图用 `dumpsys display` 的
`uniqueId=local:<物理id>` 映射后 `screencap -p -d <物理id>`。

```
am start -n org.jmcomic.ui/...QtActivity --es jm.args "--tapguard-selftest"
# 注意：带多个参数的 jm.args 在 PowerShell 里必须写成
#   --es jm.args '\"--tapguard-selftest --tapguard-legacy\"'
# 否则 am 会把 --tapguard-legacy 当成自己的选项直接报 Unknown option。
am start -n org.jmcomic.ui/...QtActivity --es jm.args "--tapguard-legacy --tapguard-selftest"
cat /storage/emulated/0/Android/data/org.jmcomic.ui/files/tapguard-selftest.txt
# 真实手势：page + swallowed + trace 每 400ms 落盘
am start ... --es jm.args "--page settings --tapguard-watch 150"
input -d <disp> swipe ...
```

| 场景 | 操作步骤 | 预期 | 实际 |
|---|---|---|---|
| 应用内自检（修复后） | 启动 `--tapguard-selftest`，读落盘报告 | `result=pass` | **pass**，`inventory=287/287`，全部断言为 1，`swallowed=17` |
| 应用内自检（修复前对照） | 启动 `--tapguard-legacy --tapguard-selftest` | `result=fail` | **fail**，`combo_replay=0`、`sub_replay=0`、`sub_replay_tap=0`、`overlay_swipe=0`、`inventory=286/286`、`swallowed=13` |

**MuMu 真实 `input` 手势**（`--tapguard-watch 150` 落盘 `page/swallowed/trace`，
坐标均为 1080×2400 物理像素）：

| 场景 | 操作步骤 | 预期 | 实际 |
|---|---|---|---|
| A1 选项框上方滑动 | 设置页 `swipe 400,300 → 815,495 / 280ms`（起手在「外观」分组区，终点落在「主题」下拉框） | 主题值不变、不弹窗 | **PASS**；下拉框区域像素差 170（仅渲染微差），主题文字未变，`swallowed=0→0`（未产生需要拦截的点击） |
| A2 滑完立刻正常点击 | 紧接着 `tap 150,770`（「开机自启」整行） | 开关照常翻转，序列续接不误伤 | **PASS**；开关区域像素差 5456，`page=11` |
| B1 悬浮窗长滑 | `--page sr --sr-sheet` 打开模型浮层，`swipe 540,900 → 540,1600 / 300ms` | 不选中模型、吞掉抬起 | **PASS**；`swallowed=0→1`，`trace: release QLabel scope=overlay drag=1 peak=292 contam=0 -> swallow` |
| B2 悬浮窗短滑 | `swipe 540,1000 → 540,1080 / 90ms`（80px 短滑） | 短滑同样判滑动 | **PASS**；`swallowed=1→2`，`trace: ... peak=33 ... -> swallow` |
| B3 浮层内正常点击 | 滑动后 `tap 215,1090`（「waifu2x 9」筛选 chip） | 筛选生效、列表变化 | **PASS**；列表区域像素差 6315，`page=10`，`swallowed` 保持 2 |

（`--tapguard-watch` 报告新增 `trace:` 段，落盘 `press+ / show / release` 的
scope、峰值、是否污染，供真机排障；该 ROM 上 logcat 会整段丢，验收只信落盘文件。）

## 7. 第三轮：阅读器菜单「按下不触发、一开始滑动就触发」

> 用户实测：阅读器一级、二级、三级菜单，在选项框上方按下时不会触发动作，
> 但只要随后开始滑动，就会被判定为触发该菜单项。

### 7.1 根因（Qt 6.12 `QMenu` 源码 + 事件链核对）

1. **Qt 6.12 放宽了 release 激活条件**。`QMenu::mouseReleaseEvent` 在 6.12 里把
   `if (QMenuPrivate::mouseDown != this)` 改成 `if (QMenuPrivate::mouseDown && mouseDown != this)`，
   源码注释明说「mouseDown 为空时这可能是打开菜单的那次 release 重放」—— 放行后继续走
   `actionAt(release.pos) == d->currentAction` 的激活判断。也就是说抬起只要有一条路径
   到达菜单，就会激活**当前高亮项**。
2. **死区内（<8dp）放行的 move 会改写 currentAction**。`QMenu::mouseMoveEvent` 里
   `if (e->buttons()) QMenuPrivate::mouseDown = this;` 且 `setCurrentAction(手指下的项,
   20ms)` —— 按下之后任何放行的 move 都会把高亮改成手指下的项、续起子菜单定时器。
   滑动开始（死区 move）+ 抬起（激活 currentAction）组合起来就是「按下不触发、
   一开始滑动就触发」。
3. **Qt 内部转发/重放可绕过单次拦截**。`QMenuPrivate::mouseEventTaken` 会把事件转给
   caused popup，popup 打开时会补发按下；应用级过滤器只能拦住事件第一次进入应用，
   转发后的第二遍不再经过它。

### 7.2 修复（`touch.cpp`）

1. **`PopupFreeze` 对象级冻结**：按下即给 Menu/PopupList/Overlay 的弹出窗口及其全部
   子控件装对象级过滤器（`installEventFilter` 是 prepend，能挡在
   `QComboBoxPrivateContainer::eventFilter`、`QMenu` 内部转发与 `QWidget::event()` 之前），
   吞掉后续的 move 与补发按下 —— `currentAction` 停在按下时的项上，Qt 的激活条件不再
   成立。release 不在冻结层吞，由应用级守卫统一判定；OnPress 触发的冻结放行触发它的
   那条按下本身（否则正常点击直接失效），吞掉抬起后冻结保持 250ms（防同轮转发），
   真点击立即解冻，新序列开始/4s 超时兜底解冻。
2. **跨项/跨行判据**：Menu 用 `actionAt(press) != actionAt(release)`、PopupList 用
   `indexAt` 比较 —— 按下与抬起落在不同项上就是滑动（哪怕位移在峰值死区内）；
   同一项内 1~3px 手指抖动不算，正常点击手感不变。
3. **行为变化**：滑动中菜单高亮不再跟手、子菜单不再随滑动弹出（滑动=想关闭/滚动，
   不是选择）；「按住滑进子菜单抬手选子项」在触摸端上一轮就已被吞，现彻底不可能；
   点开子菜单后再点子项不受影响。

### 7.3 自检新增（`--tapguard-selftest` 菜单段扩成贴近阅读器菜单的结构：
QWidgetAction 头部 + 一/二/三级子菜单）

| 字段 | 场景 | 预期 |
|---|---|---|
| `menu_touch_slide` | 触摸合成路径：按住一级入口 → hover 弹出二级 → 滑进二级 → 抬手在子项 | 不触发 |
| `menu_touch_l3` | 二级 → hover 弹出三级 → 滑进三级 → 抬手 | 不触发 |
| `menu_jitter_tap` | 按在项上微动 3px（死区内）停住抬手 | 仍算一次点击 |
| `menu_hover_release` | 二级弹出后抬手仍停在一级入口上 | 不触发 |
| `menu_head_slide` | 按在 QWidgetAction 头部 → 滑到普通项 → 抬手 | 不触发 |
| `menu_freeze_replay` | 滑动建立后模拟 Qt 内部重放按下+抬起 | 冻结层拦住，不触发 |
| `menu_freeze_tap` | 吞掉后正常点选 | 静默期结束恢复，恰好触发一次 |

### 7.4 实测

| 项目 | 预期 | 实际 |
|---|---|---|
| 桌面修复后 | pass | **pass**，`inventory=303/303`、`swallowed=18`，菜单矩阵 7 字段全 1 |
| 桌面 `--tapguard-legacy` | fail | **fail**：`menu_freeze_replay=0`、`menu_freeze_tap=0`（其余仍 1）→ 证明新用例精确测的是冻结层 |
| 桌面离线套件 | 全绿 | **PASS 76 / FAIL 0** |
| `--toggle-test` / `--blank-report` | 正常 | `before/after` 翻转、`toggleRows=5 sameLine=5` |
| MuMu 应用内自检（修复后） | pass | **pass**，`inventory=287/287`，菜单矩阵全 1 |
| MuMu `--tapguard-legacy` | fail | **fail**：`menu_freeze_replay=0`、`menu_freeze_tap=0`，`inventory=288/288` |

## 8. 第四轮：菜单/选项框「按下就触发」（必现 + 概率）

> 用户实测：阅读器一/二/三级菜单滑动**必定**出现动作且表现为按下就触发；
> 设置等地区的选项框与二级菜单滑动**有概率**误触，同样按下就触发。

### 8.1 三条量化根因

1. **守卫吞掉按下之后，"无按下抬起"在 Qt 6.12 的 QMenu 里仍会激活残留高亮项**
   （`mouseDown && mouseDown != this` 放宽 + `currentAction` 在按下时已被
   `setCurrentAction` 设为按下项）。这是"按下就触发"的直接通路，守卫此前对
   "没有对应按下的抬起"一律放行。
2. **`hasMouseMoved` 激活窗口**：QMenu 用 `QApplication::startDragDistance()`
   （默认 10px 逻辑）判定"鼠标动过"，动过则激活。真机 DPR>=1.25 时 8dp 阈值
   **大于** startDragDistance —— 滑动 10~8dp 之间的抬起越过 QMenu 的激活窗口却
   低于守卫的峰值阈值，直接放行 → 必现/高概率误触。桌面 DPR=1 时两阈值几乎重合，
   无法离线构造。
3. **QComboBox 弹窗的 `blockMouseReleaseTimer`**：弹窗打开后的防误提交窗口一过期，
   死区内 move 让容器过滤器把 currentIndex 跟手改写，抬起就直接提交 —— 选项框
   "概率性误触"的来源。

### 8.2 修复（`touch.cpp`）

1. **无按下抬起落点在弹出类窗口 → 吞**（`OnRelease` 的 `!g_.active` 分支）；
   普通控件照常放行。
2. **弹出类作用域（Menu/PopupList/Combo/Overlay）的位移阈值对齐激活窗口**：
   `effSlop = min(8dp, max(startDragDistance, 4dp))` —— OnMove 的滑动判定与
   OnRelease 的抬起判定同用；页面控件保持 8dp。
3. **Combo 弹窗打开后的任何 move 立即按滑动处理**（死区 move 不再让 currentIndex
   跟手）+ **抬起落在弹窗里无条件吞**（按下在框上、抬在弹窗里必然经过滑动）+
   **弹窗无论怎么打开都装冻结层**；用户点列表项的按下由 PopupList 作用域的
   `armSkipFirstPress` 升级放行。
4. **删除"按时间窗续接补发按下"**：它会把用户滑动结束后的正常按下误吞，而那条
   按下被吞后跟着的无按下抬起正是"按下就触发"；迟到 replay 由冻结层（吞抬起后
   保持 250ms）与无按下抬起拦截兜住。

### 8.3 自检新增（46 字段）

| 字段 | 场景 | 预期 |
|---|---|---|
| `menu_orphan_release` | 按住缓慢漂移（长按漂移吞抬起）后，菜单仍开着、残留高亮，再发一条无按下抬起 | 不激活 |
| `menu_tap_after_swipe` | 滑动吞抬起后立刻正常点另一项 | 照常触发（防误吞回归） |
| `combo_dead_swipe` | 按下框上（弹窗随按下打开）→ 防误提交窗口过期后小幅滑动 → 抬手落在弹窗行上 | index 保持不动 |
| `combo_dead_tap` | 随后正常点选弹窗项 | 照常提交 |

### 8.4 实测

| 项目 | 预期 | 实际 |
|---|---|---|
| 桌面修复后 | pass | **pass**，46 字段全 1，`inventory=303/303`、`swallowed=25` |
| 桌面 `--tapguard-legacy` | fail | **fail**：`menu_freeze_replay=0`、`combo_replay=0`（新增字段在 legacy 也受益于多层防护，作为回归保护） |
| 桌面离线套件 | 全绿 | **PASS 76 / FAIL 0** |
| MuMu 自检（修复后） | pass | **pass**，`inventory=287/287`，46 字段全 1 |
| MuMu `--tapguard-legacy` | fail | **fail**：`menu_freeze_replay=0` |

## 9. 第五轮：菜单/选项框的按压反馈、松手才响应与边界提示

> 用户实测截图（阅读器菜单 + 弹出的二级子菜单）：滑动仍会触发二级菜单弹出
> （按下弹出、滑动开始立刻收起）；二级菜单内滑动会让选项高亮后立刻关闭。
> 要求：滑动不应弹子菜单/不应高亮后关闭；按住显示半透明按压效果、**松手才响应**；
> 按住滑动按方向在底部/顶部播放"已到顶部/已到底部"提示、按压效果随滑动渐隐；
> 可滚动的菜单滚到边界也用同样的提示；统一应用到页面/菜单/选项框。

### 9.1 实现与新文件

**`cpp/shell/ui/touch_menu.h/.cpp`（新增，CMakeLists 已加入）**：

| 组件 | 职责 |
|---|---|
| `PressOverlay` | 按下项上的半透明圆角按压层（accent 28% 透明度），滑动开始时 `FadeAway` |
| `BoundaryBar` + `ShowBoundaryHint(host, top)` | "已到顶部 / 已到底部"渐隐提示条（半透明深色圆角 + 白字，~1s 自动销毁，同宿主同方向 800ms 节流） |
| `InstallBoundaryHints(area)` | 给滚动容器挂滚动条监听：QScroller 拖动中越过顶/底时弹同款提示（**页面滚动容器与下拉弹窗列表统一生效**） |
| `MenuPressController` | 菜单/弹窗按压器官：`BeginMenuPress / BeginListPress / FeedMove / FeedRelease / DismissOutside / Cancel` |

`touch.cpp` 接线：Menu 作用域按下 → `BeginMenuPress` 并标记 `menuTaken`；
move/release 全部转交控制器；`Settle` 对接管手势不再复位菜单状态；弹出类作用域的
滚动区在 `ScrollFingerprint` 挂钩时统一 `InstallBoundaryHints`。

### 9.2 关键交互语义

1. **按下不弹子菜单**：QMenu 原生是"按下起 20ms 定时器弹子菜单"。接管后**按下照常
   放行**给 QMenu（**吞掉会触发 Qt popup 的按下重放循环直到栈溢出，实测**），定时器
   弹出的子菜单由 `OnShow` 在接管手势中立即收起 —— 按下不再触发子菜单弹出；
   `setActiveAction(nullptr)` 的视觉清理只在按压仍在进行时执行（手势结束后清高亮会把
   刚弹出的子菜单一起收掉）。
2. **按压视觉**：手指下的项显示半透明按压层（accent 色），随按住持续。
3. **滑动**：越过 `startDragDistance` 后进入滑动模式 —— 按压层渐隐消失；QMenu 用
   **合成滚轮跟手滚动**（QMenu::wheelEvent → 内部 scrollMenu），滚动到第一项完整可见/
   末项完整可见时按滑动方向弹"已到顶部 / 已到底部"；下拉弹窗列表与页面容器由
   `InstallBoundaryHints` 同机制提示。滑动期间不高亮跟手、抬起不触发任何项、
   **菜单保持打开**（不再"高亮后立刻关闭"）。
4. **松手才响应**：抬起无滑动且落在按下项上 → 子菜单项弹出子菜单（用 Enter 键事件走
   Qt 内部 `popupAction/showSubMenu`，causedPopup 链一致；手动 `sub->popup` 在
   "收起后立即重弹"时会崩，实测）、普通项 `trigger()` 并关闭菜单链；抬起落在菜单
   矩形外 → 关闭菜单链（接管后 Qt 的"点外面关闭"失效，由 `DismissOutside` 补齐）。
5. **长按漂移**：按住 >500ms 且漂移 >4dp 与 OnRelease 的 holdDrift 判据同源，
   不触发、菜单保持。

### 9.3 自检新增（菜单段扩成 54 字段）

| 字段 | 场景 | 预期 |
|---|---|---|
| `menu_no_sub_on_press` | 按下子菜单入口 220ms | 子菜单未弹（原 20ms 定时器会弹） |
| `menu_press_overlay` | 按下项上 | 半透明按压层存在 |
| `menu_overlay_gone` | 滑动开始 400ms 后 | 按压层已渐隐销毁 |
| `menu_swipe_no_sub` | 滑动抬起 | 不触发、子菜单未弹、菜单保持打开 |
| `menu_sub_on_release` | 按住松手（同项） | 子菜单弹出 |
| `menu_tap_triggered` | 普通项按住松手 | 恰好触发一次 |
| `menu_bottom_hint` | 长菜单 | "已到底部"提示条弹出 |

### 9.4 实测

| 项目 | 预期 | 实际 |
|---|---|---|
| 桌面修复后 | pass | **pass**，54 字段全 1，`inventory=303/303` |
| 桌面 `--tapguard-legacy` | fail | **fail**：`menu_no_sub_on_press=0`、`menu_press_overlay=0`、`menu_freeze_replay=0` |
| 桌面离线套件 | 全绿 | **PASS 76 / FAIL 0** |
| `--toggle-test` / `--blank-report` | 正常 | 翻转正常 / `toggleRows=5 sameLine=5` |
| MuMu 自检（修复后） | pass | **pass**，`inventory=287/287`，接管矩阵全 1 |
| MuMu `--tapguard-legacy` | fail | **fail**：`menu_no_sub_on_press=0`、`menu_press_overlay=0` |

## 10. 第六轮：「按下即弹」彻底移除（按下被守卫吞掉、松手才真正弹出）

> 用户复测：上一轮的"弹-收"实现里，子菜单/选项框仍在**按下瞬间弹出**、滑动开始
> 立刻消失（闪现肉眼可见）。根因：放行按下 + 收起弹出的方案里，QMenu 的 20ms
> 定时器与 QComboBox 的同步 showPopup 都在按下时已经执行，收起只是补救。
> 本轮改为**按下根本不触发弹出**：按下被守卫吞掉（QMenu 收不到 press，20ms
> 定时器不会启动；QComboBox::mousePressEvent 不执行，showPopup 不会发生），
> 按压层由控制器自绘，**松手才真正 `showPopup()` / 弹子菜单 / trigger**。

### 10.1 关键改动（`touch_menu.cpp` / `touch.cpp`）

1. **`BeginMenuPress` 恢复吞下按下**，同时清掉上一次手势残留的
   `activeAction`（QMenu::event(Show) 会对残留 currentAction 执行
   `popupAction`，导致菜单打开 250ms 后凭空弹出子菜单）。
   早先判定的"吞按下会触发 Qt popup 按下重放死循环"实为自检里
   `overlaySeen` 裸指针的 use-after-free（`FadeOut` 的 `deleteLater` 之后
   仍调 `isVisible()`），已改 QPointer —— 重放循环并不存在。
2. **`BeginComboPress`（新）**：选项框按下被吞，按压层画在框上；
   `FeedRelease` 松手（无滑动）才 `showPopup()`，滑动则不弹（按压层已渐隐）。
3. **`PopupFreeze` 只吞 move**：吞抬起后 250ms 冻结静默期内的任何新按下必须
   先过应用级守卫（OnPress 建立新手势并解冻）—— 静默期吞按下会让快速连续
   操作的按下永远到不了守卫（实测自检与真机全部按下消失）。按下/抬起交给
   应用级守卫统一判定。
4. **接管手势保护**：接管进行中出现的新按下（Qt popup replay、触摸合成的
   重复按下）不属于新的用户点击，吞掉以保证 `menuTaken` 手势完整；
   move/release 用 `HasState()`（控制器按压状态）判定，双保险。
5. **`CloseMenuChain` 清 `activeAction`**：残留会让下一次菜单打开时凭空弹出。

### 10.2 自检扩展（54 → 58 字段）

| 字段 | 场景 | 预期 |
|---|---|---|
| `combo_no_popup_press` | 按下选项框 320ms | 弹窗未打开（原实现按下即弹） |
| `combo_press_overlay` | 按下选项框 | 框上出现按压层 |
| `combo_popup_release` | 松手（无滑动） | 此时才真正弹出 |
| `combo_swipe_no_popup` | 按住滑动抬起 | 不弹（放弃打开） |

### 10.3 实测

| 项目 | 预期 | 实际 |
|---|---|---|
| 桌面修复后 | pass | **pass**，58 字段全 1，`inventory=303/303` |
| 桌面 `--tapguard-legacy` | fail | **fail**：`combo_no_popup_press=0`、`menu_no_sub_on_press=0`、`menu_press_overlay=0`、`menu_freeze_replay=0` |
| 桌面离线套件 | 全绿 | **PASS 76 / FAIL 0** |
| MuMu 自检（修复后） | pass | **pass**，`inventory=287/287`，接管矩阵全 1 |
| MuMu `--tapguard-legacy` | fail | **fail**：`combo_no_popup_press=0`、`menu_no_sub_on_press=0`、`menu_press_overlay=0` |

## 11. 第七轮：阅读器菜单滑动仍闪现 + 按压层在非默认主题下糊字

> 用户复测两处：(a) 阅读器菜单里滑动仍会看到子菜单“闪一下”；(b) 按住时的
> 半透明效果在其他主题色上异常，黑色文字被黑色背景遮住看不清。

### 11.1 子菜单闪现的真正根因（`touch.cpp` OnPress 的 Menu 分支）

上一轮把 `BeginMenuPress` 改成“返回 true = 按下已被控制器接管”，但 `OnPress`
里的调用点**把返回值丢了**：

```cpp
// 修复前（touch.cpp:1024-1033）
if (scope == GuardScope::Menu && Touch::IsTouchMode()) {
    auto *m = qobject_cast<QMenu *>(scopeWidget);
    if (m && !Touch::TapGuardLegacy()) {
        Touch::MenuPressController::BeginMenuPress(m, global);   // 返回值被丢弃
        g_.menuTaken = true;
    }
}
// …继续往下走到 return false;  ← 按下照样放行给 QMenu
```

于是 `QMenu::mousePressEvent` 仍然执行：`setCurrentAction(action, 20)` 起动 20ms
子菜单定时器 → 手指还没抬起，二级菜单已经弹出；紧接着滑动被判定接管，
`DragTo` 每帧调 `ClearMenu` 把子菜单收掉 —— 用户看到的就是“闪一下”。
同一文件里的 Combo 分支写法是对的（`if (… && BeginComboPress(…)) { …; return true; }`），
Menu 分支漏了 `return true`。修复：

```cpp
if (m && !Touch::TapGuardLegacy()
    && Touch::MenuPressController::BeginMenuPress(m, global)) {
    g_.menuTaken = true;
    return true;   // 按下必须吞：QMenu 收不到 press 就不会起子菜单定时器
}
```

配套把自检 N4 的断言从“220ms 后子菜单不可见”改成“从按下起 300ms 内**从未**
出现过”（每 20ms 采一次，300ms 共 15 次）。旧断言在 220ms 单点采样，闪现恰好
发生在 20~60ms、220ms 时已被 `ClearMenu` 收起，所以永远报 1 —— 这正是“自检全绿
但用户仍能看到闪现”的原因。

### 11.2 按压层为什么会糊字（`touch_menu.cpp` PressOverlay）

两个独立原因叠加：

1. **底色不透明**：`PressOverlay` 是菜单的子 QWidget。六套基础 QSS 里都有
   `QWidget { background-color: #31363b; }`（`theme.cpp:321` 只把它覆盖成
   transparent，但**阅读器菜单用 `StyleReaderMenu` 自己 setStyleSheet 过**，
   子树会退回 qApp 全局 QSS）—— 按压层先被刷成不透明深灰，深色主题的
   `#000000` 文字直接被盖住。修复：`WA_NoSystemBackground` +
   `setStyleSheet("background: transparent;")` 双保险。
2. **强调色叠色**：旧实现固定画 `accent @ alpha 56`。深色主题普通项是
   `#ffffff` 字 / `#232629` 底（叠一层橙没问题），但选中项是 `#000000` 字 /
   `#ff79b0`（或 `#FFA94D` / `#6effe8`）亮底 —— 再叠深色强调色等于把亮底压暗，
   黑字与背景的对比度被吃掉；浅色主题（`#3c3c3c` 字 / `#f5f5f5` 底）则是
   亮橙叠亮底，几乎看不见。

新实现三条通道：

| 通道 | 做法 | 作用 |
|---|---|---|
| 底 | 构造时 `host->grab(rc)` 存下按下瞬间的真实像素，paintEvent 里 `drawPixmap(rect(), base_, base_.rect())` | 文字像素原样保留；`grab()` 是设备像素（DPR 2 时两倍大），必须按控件矩形缩放，否则只画出左上角四分之一 |
| 雾 | 黑色薄雾，深底 alpha 64 / 亮底 alpha 30（`PreferDarken()` 用 16 桶亮度直方图众数判断底色明暗） | 只加深不提亮：白字更清楚，黑字只被轻微压暗，不会出现“白雾糊在白底上”的反向翻车 |
| 描边 | 强调色 1px 细边（alpha 170 / 130） | 品牌色与按压存在感，不碰文字像素 |

### 11.3 六套主题实测（`docs/shots/tapfix/`）

`JM_READER_MENU_SHOT=<png> JM_READER_MENU_PRESS=<n>` 可以让阅读器菜单离屏
布局后在指定项上模拟按住再抓图（见 `ReaderPage::OpenMenuForTest`）。
六套主题（dark/light × orange/pink/teal）逐一抓图核对：按住项只剩一层很淡的
加深与强调色描边，**所有文字在六种配色下都清晰可读**（`zoom_sheet.png` 为
2×3 拼图，`menu_<theme>.png` 是单主题整图）。

### 11.4 实测

| 项目 | 预期 | 实际 |
|---|---|---|
| 桌面修复后 | pass | **pass**，58 字段全 1，`inventory=303/303`，`swallowed=14` |
| 桌面 `--tapguard-legacy` | fail | **fail**：`combo_replay=0`、`sub_replay=0`、`menu_freeze_replay=0`、`combo_no_popup_press=0`、`combo_press_overlay=0`、`menu_no_sub_on_press=0`、`menu_press_overlay=0`、`overlay_swipe=0` |
| 桌面离线套件 | 全绿 | **PASS 76 / FAIL 0** |
| MuMu 自检（修复后） | pass | **pass**，`inventory=267/267`，接管矩阵全 1，`swallowed=15` |
| MuMu `--tapguard-legacy` | fail | **fail**：`combo_no_popup_press=0`、`combo_press_overlay=0`、`menu_no_sub_on_press=0`、`menu_press_overlay=0`、`menu_freeze_replay=0` |

### 6.3 已知限制

- MuMu 的虚拟 display 上 `Qt::Popup` 顶层窗口无法稳定截屏（应用内自检里
  `combo_open=1` 说明对象可见，但系统 `screencap` 抓不到弹窗层），因此
  「下拉弹窗 / 二级菜单」的验证以**应用内真实 QMenu / QComboBox 对象 + 完整事件
  序列**为准（含修复前后 A/B），真实 `input` 手势主要用于页面内与自绘浮层。
- 多指用应用内合成 `QTouchEvent` 验收（`input motionevent` 只能单指），逻辑与
  真机双指流一致，但无真机双指实测。
