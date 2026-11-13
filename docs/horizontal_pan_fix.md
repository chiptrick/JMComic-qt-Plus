# 页面整体横向位移（左右滑动整页跑动）修复报告

日期：2026-11-12（二次修订）
现象：分流设置页左右滑动时整页跟着左右移动；排查后发现同类问题存在于多个页面。

---

## 1. 根因

共有三层原因，缺一不可：

1. **页面级 `QScrollArea` 的横向可滚区间没有清零。**
   `setHorizontalScrollBarPolicy(AlwaysOff)` 只隐藏滚动条，不改变
   `horizontalScrollBar()->maximum()`。当内容最小宽大于视口时（分流页 360 宽下
   `hmax=53`），QScroller 仍能改变滚动条 value，视觉上就是整页横移。
2. **横向平移锁只处理了鼠标事件。**
   `Touch::LockHorizontalPan()` 安装的 `HorizontalPanLock` 原先只拦
   `MouseButtonPress/MouseMove`。真机 QScroller 用的是 `TouchGesture`，
   走 `QTouchEvent`，这条锁在手机上从未生效。
3. **覆盖面不全。**
   - `ProxyPage` 自建 `QScrollArea`，没有调用 `LockHorizontalPan`；
   - `ui_pages_extra.cpp`、`ui_pages_library.cpp` 各有一份 `WrapScroll` 副本，
     都漏了锁（只有 `ui_pages_browse.cpp` / `ui_pages_misc.cpp` 的副本有）；
   - 侧栏 `NavScroll` 与 `ComicListView`（网格）也没有锁；
   - 部分页面内容本身超宽，给横移提供了空间：
     - 本地漫画页：目录绝对路径不换行（+59px）；
     - 超分页：左右两栏并排 + 300px 固定预览（+354px）；
     - 分流页：筛选按钮 1×4 排布（352px）、节点表固定列宽、高级卡片 25 字复选框。

## 2. 二次排查（真机复测仍横移的根因）

第一轮修复后真机仍横移，逐层排查确认了三件事：

1. **时间线错位**：真机上安装的 0.93 包构建于 2026-11-11 23:56，而横向锁的触摸
   分支是 11-12 00:10 之后完成的 —— 用户拿到的"最新版本"实际上不含修复。
   证据：旧包在分流页横滑位移 **dx=110**（可复现）。
2. **hbar range=0 是个陷阱**：为了"让 QScroller 没有可移动空间"而把
   `horizontalScrollBar()->setRange(0,0)` 会让 QScroller 判定整个
   QAbstractScrollArea 不可滚动 —— **纵向也一起失效**（真机实测设置页
   纵滑 dy=0）。所以 range 不能清零，只能钉 value。
3. **QScroller 的手势识别跑在 QGestureManager（应用级过滤器）里，先于对象级
   事件过滤器**：单靠"吞掉 TouchUpdate"挡不住已经启动的滚动，必须显式
   `QScroller::stop()`，并把横向滚动值钉回 0。

### 最终实现（`ui/touch.cpp`）

- **滑动方向判定**：以按下点为基准，累计位移满足 `|dx| > 6 且 |dx| > |dy|`
  才判定为横向；一旦判定为本手势保持横向语义，后续不再反复震荡。
- **事件拦截**：横向判定后吞掉后续 `TouchUpdate / MouseMove`；
  `TouchEnd / MouseButtonRelease` 也一并吞掉，防止 QScroller 用残留速度收尾。
- **默认行为阻止**：吞事件的同时调用 `QScroller::stop()`，并把
  `horizontalScrollBar()->setValue(0)`（`valueChanged` 连接持续钉 0）；
  `QScrollerProperties::HorizontalOvershootPolicy = OvershootAlwaysOff`，
  关掉边界回弹。
- **不清零 range**：`hmax` 保留真实值（内容适配已把主要页面压到 0），
  纵向滚动完全不受影响。

## 2.1 第一轮修改清单

| 文件 | 修改 |
|---|---|
| `ui/touch.cpp` | `HorizontalPanLock` 新增 `TouchBegin/TouchUpdate/TouchEnd/TouchCancel` 分支：横向为主的单指触摸位移整段吞掉，纵向放行；多指时停止拦截，避免误伤缩放。`Wheel` 分支同时看 `angleDelta` 与 `pixelDelta`（触控板）。`LockHorizontalPan` 不再依赖 `g_touchMode`，桌面同样生效 |
| `ui/proxy_page.cpp` | 自建 `QScrollArea` 调用 `LockHorizontalPan`；筛选按钮窄屏改 2×2 网格；窄屏列宽压缩（name 62 / ip 108 / delay 48 / score 34）；线路表窄屏列宽与间距收紧；高级卡片复选框与保存按钮窄屏短文案（完整文案进 tooltip） |
| `ui/ui_pages_extra.cpp` | `WrapScroll` 补 `LockHorizontalPan` |
| `ui/ui_pages_library.cpp` | `WrapScroll` 补 `LockHorizontalPan`；目录路径 `wordWrap + minimumWidth(0)`；文件夹名折行 |
| `ui/ui_pages_misc.cpp` | 超分页窄屏改为纵向堆叠（模型在上、预览在下）；预览不再固定 300px；模型行窄屏隐藏长说明（收进 tooltip） |
| `ui/ui_main_window.cpp` | 侧栏 `NavScroll` 调用 `LockHorizontalPan` |
| `ui/comic_view.cpp` | `ComicListView` 调用 `LockHorizontalPan`（网格按视口宽换行，本无横向内容） |
| `ui/main_desktop.cpp` | `--scroll-report` 扩展：除 `QScrollArea` 外，还打印所有非 `QScrollArea` 的 `QAbstractScrollArea`（QListView/自绘视图）的 `hmax` |

## 3. 保留的正常横向操作

修复只作用于**页面级滚动容器的 viewport**，不改变子控件和专用视图：

| 功能 | 是否保留 | 原因 |
|---|---|---|
| 阅读器左右/右左滑动翻页、双指缩放、放大后平移 | 保留 | `ReaderView` 自己处理 `QTouchEvent`，未安装页面锁；`--reader-touch-selftest` 5/5 通过 |
| 缩放滑块、开关、下拉框 | 保留 | 事件先到子控件，不经过 viewport 的过滤器 |
| 首页分区栏横向切换 | 保留 | `SectionStrip` 是自绘控件，非 `QAbstractScrollArea` |
| 侧栏、列表纵向滚动 | 保留 | 锁只吞"横向为主"的位移，纵向事件原样放行 |
| 触控板纵向两指滚动 | 保留 | `Wheel` 只在横向分量占优时拦截 |

## 4. 验证

### 4.1 横向可滚区间（360×800，`--touch --scroll-report`）

| 页面 | 修复前 hmax | 修复后 hmax |
|---|---|---|
| 分流设置 `proxy` | **53** | **0** |
| 本地漫画 `local` | **59** | **0** |
| 图片超分 `sr` | **354** | **0** |
| 设置 `settings` | 0 | 0 |
| 下载 / 本地记录 / 评论 / 关于 / 登录 / 帮助 | 0 | 0 |

其余页面（首页/搜索/分类/收藏等）没有可见的 `QScrollArea`；`ComicListView` 已单独加锁，
扩展后的 `--scroll-report: area=` 也未报出横向可滚容器。

### 4.2 阅读器回归（确认未误伤）

```
reader-touch done pass=5 fail=0 zoom=100%
reader-parity done pass=16 fail=0
```

### 4.3 布局检查

360×800 窄屏截图：分流页筛选按钮 2×2、节点表与高级面板完整；超分页模型列表与预览
纵向堆叠；本地漫画页路径折行。均无右侧裁切。

### 4.4 真机触摸（vivo V2463A / Android 17 / arm64-v8a / 1440×3168）

用 `input swipe` 注入单指手势，对前后截图做互相关式扫描取最佳位移（dx ∈ ±300、
dy ∈ ±1200），|dx|≈0 即整页不再横移：

| 用例 | 旧包 0.93（无修复） | 新包（含完整修复） |
|---|---|---|
| 分流页横向拖动 `1200→200` | **dx=110**（整页横移，复现） | **dx=0**，mean diff 2.5 |
| 分流页纵向拖动 `2600→1400` | — | **dy=-1180**（内容上移，纵向正常） |
| 设置页横向拖动 `1200→1800` | — | **dx=0**，mean diff 1.7 |
| 桌面回归（touch/parity） | — | 5/5、16/16 |

补充：AVD Pixel 8a（Android 16 / x86_64）上同样测得分流页横滑 dx=0、纵滑 -520px。

结论：横移由"横向手势 + QScroller 可滚区间"共同造成；修复后横向锁生效、
纵向滚动与阅读器等横向交互不受影响。

## 5. 复现命令

```bash
cd cpp/shell/ui/build
# 逐页检查 hmax（0 为正确）
for p in proxy local sr settings; do
  ./jmcomic_ui.exe --page $p --touch --win-size 360x800 \
    --scroll-report --shot /tmp/$p.png --shot-delay 900 --quiet 2>&1 | grep scroll-report
done

# 阅读器回归
./jmcomic_ui.exe --reader-dir build/testpages --reader-touch-selftest \
  --shot /tmp/t.png --shot-delay 1800 --quiet
```
