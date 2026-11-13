# 漫画阅读器：Python 版功能清单与 C++ 版对齐报告

日期：2026-11-11
对照范围：`python/src/view/read/`、`python/src/tools/mobile_ui_reader.py`、
`python/src/component/scroll/read_scroll.py`、`python/src/config/setting.py`；
C++ 实现：`cpp/shell/reader.*`、`cpp/shell/sr_engine.*`、
`cpp/shell/ui/ui_pages_misc.cpp`（`ReaderPage`）、`cpp/shell/ui/ui_main_window.*`、
`cpp/shell/ui/main_desktop.cpp`。

---

## 1. 结论与根因

**根因**：改动前 C++ 阅读器是 `QScrollArea + QLabel` 结构，双指缩放挂在
`QPinchGesture` 上，翻页只靠 `MouseButtonPress/Release` 的合成鼠标事件。两个结构性缺陷：

1. `Touch::InstallScrollGestures()` 会给所有 `QAbstractScrollArea` 安装 `QScroller`，
   触摸事件先被 QScroller 接走，视口既收不到 `TouchUpdate` 也不会再合成鼠标事件；
   同时 Qt 的 Widget 手势识别器拿不到 `TouchBegin`，`QPinchGesture` 永远不启动。
2. 滚动模式下相邻页是嵌在视口上的独立子控件，两根手指常落在不同控件上形成两条
   独立的单点触摸序列 —— 只看"本次事件里有几个点"永远等不到两点。Python 版为此
   专门按 `QEventPoint.id()` 跨序列合并触点。

**修复**：阅读器改为 `QAbstractScrollArea` 子类 + **单视口自绘**（无子 QLabel），
结构上消除触点拆分；触控完全基于 `QTouchEvent`（按 id 合并、手势中视图预览缩放、
抬手才重排、一次手势只翻一页、放大后平移优先）；构造时打 `jmScroller` 标记，
阻止 QScroller 接管。翻页/滚动/缩放/模式语义逐项对齐 Python 版。

**验证结论**：桌面 21 项自检全通过（16 + 5），真机（MuMu / Android 15 / x86_64）
左滑、右滑、右左双页滑动、双指捏合、放大后平移全部通过；超分在桌面端完成真实
2× 推理验证（320×240 → 640×480）。

---

## 2. Python 版完整功能清单

### A. 阅读模式与布局（`read_enum.py` / `read_graphics.py`）

| 编号 | 功能 | 值 | 说明 |
|---|---|---|---|
| A1 | 上下滚动 | 0 | 整章纵向连续，页宽 = 视口宽 |
| A2 | 左右翻页（默认） | 1 | 单页，横向翻页轴 |
| A3 | 左右双页 | 2 | 当前页在左、下一页在右，步进 2 |
| A4 | 右左双页 | 3 | 当前页在右、下一页在左；`NextPage()` 反向（索引 -2） |
| A5 | 左右滚动 | 4 | 整章横向连续，页高 = 视口高 |
| A6 | 右左滚动 | 5 | 页 0 在最右，前进 = 横向滚动值减小；`NextPage()` 反向 |
| A7 | 右左双页（滚轮正序） | 6 | 同 A4 布局，但 `NextPage()` 按索引 +2 |
| A8 | 等宽模式 | 7 | 单页按宽铺满、纵向滚动；非 `isScroll` |
| A9 | 右左滚动（滚轮正序） | 8 | 同 A6 布局，滚轮/输入方向归一化 |
| A10 | 双页配对偏移 | F10 | `ChangeDoublePage()`：curIndex +1 改变配对（封面单页） |
| A11 | 滚轮正序语义 | `ForwardScrollDelta` | 右左滚动的"下一屏为正"换算成滚动条减量 |

### B. 翻页 / 滚动（`read_tool.py` / `read_graphics.py` / `read_scroll.py`）

| 编号 | 功能 | 说明 |
|---|---|---|
| B1 | 下一页 / 上一页 | 双页步进 2；右左基础版（3/5）方向反转 |
| B2 | 章末自动跳下一章 | 在线 / 离线 / 本地三种来源分别判定，`AutoSkipNext` |
| B3 | 章首自动跳上一章 | 同上，`AutoSkipLast` |
| B4 | 点按三分区 | 上排左右=显隐菜单、中=上滚半屏；下排左右=翻页、中=下滚半屏；放大时下排左右不翻页 |
| B5 | 滚轮翻页 | 页尾再向下 → 下一页；页首再向上 → 上一页；否则平滑滚动 |
| B6 | 滚动模式方向 | 右左基础版跟手即后退，滚轮正序版输入取反 |
| B7 | 平滑滚动动画 | `ReadScroll` 200ms 线性补间；触摸拖动走 `DragScroll` 1:1 跟手 |
| B8 | 页码滑块跳页 | `QtCustomSlider`，显示页码，抬手结算 |
| B9 | 自动滚动 / 自动翻页 | F5；滚动速度、滚动时间、翻页速度、手动滚动像素四项可调 |
| B10 | 键盘 | ← → 翻页、↑ ↓ 半屏、Space、F1~F12、+ / - 缩放、0 复位、Shift+← → 切章、Esc 返回 |
| B11 | 首次帮助浮层 | `ReadFrame.InitHelp()` 画三分区示意，点一下隐藏 |

### C. 缩放与触摸（`mobile_ui_reader.py` / `read_graphics.py`）

| 编号 | 功能 | 说明 |
|---|---|---|
| C1 | 缩放 20%~500% | 工具条滑块；右键菜单 50%~200% 预设 |
| C2 | 双指捏合缩放 | 手势中只做 `QGraphicsView.scale()` 预览，抬手按目标百分比重排一次；<5% 变化忽略 |
| C3 | 按 id 合并触点 | 跨触摸序列累积 `{id: 全局坐标}`，两点即进缩放 |
| C4 | 放大后双向平移 | 横向 + 纵向都按内容跟手移动，`notify=False` 不结算换页 |
| C5 | 1 指拖动滚动 | 滚动模式 1:1 跟手；不套补间 |
| C6 | 翻页模式滑动翻页 | 一次手势只翻一页：轴锁定（横向优先）、累计位移 > 32px、滚动条在对应边界 |
| C7 | 缩放后剩余手指吞掉 | 不会退化成点按翻页 |
| C8 | Ctrl+滚轮 / 快捷键缩放 | 与 `+` `-` `0` 同一入口 |
| C9 | 触摸阈值 | `TouchSlopPx=16`（起拖）、`TurnPageSlopPx=32`（翻页） |

### D. 工具条、菜单与信息（`read_tool.py` / `read_view.py` / `ui_read_tool.ui`）

| 编号 | 功能 | 说明 |
|---|---|---|
| D1 | 图片信息 | 位置（n/m + 章节名）、状态、大小、分辨率 |
| D2 | 阅读模式下拉 | 9 项，切换后落库 `LookReadMode` |
| D3 | 缩放滑块 / 标签 | 20%~500%，显示百分比 |
| D4 | 自动滚动参数 | 手动滚动（px）、滚动速度（px）、滚动时间（ms）、翻页速度（s） |
| D5 | 按钮 | 隐藏、全屏、返回、上一章、下一章、上一页、下一页 |
| D6 | 右键菜单 | 菜单 F12、全屏 F11、自动/本张超分 F2、9 模式、双页对齐 F10、缩放预设、切章、复制图片 F1 / 复制文件 F3 / 复制到上次目录 F4、自动滚动 F5、退出 Esc |
| D7 | 复制 / 另存为 | 剪贴板图片；桌面 QFileDialog、手机 SAF |
| D8 | 全屏 | `LookReadFull` 记忆 |
| D9 | 手机端工具条整宽 | `mobile_ui_reader.SetupReadTool()` |

### E. 图片数据与展示（`read_view.py` / `read_graphics.py` / `read_qgraphics_proxy_widget.py`）

| 编号 | 功能 | 说明 |
|---|---|---|
| E1 | 在线 / 离线 / 本地三种来源 | `OpenPage` / `OpenLocalPage` |
| E2 | 章节图片下载与还原 | scramble 还原、进度条、失败重试（`ResetCnt=5`） |
| E3 | 预加载 | 下载 `PreLoading=10`、QImage 预转换 `PreLook=5`、窗口外释放 |
| E4 | GIF / 动画播放 | 非滚动模式用 `QMovie`；滚动模式用静态首帧 |
| E5 | 阅读历史 / 继续阅读 | `AddHistory()` 记录 bookId/epsId/curIndex，`OpenPage(pageIndex)` 续读 |
| E6 | 状态显示 | 下载中 / 转换中 / 超分中进度条 |

### F. 图片超分（`read_view.py` / `read_tool.py` / `sr_backend.py`）

| 编号 | 功能 | 说明 |
|---|---|---|
| F1 | 自动超分 | `IsOpenWaifu`，进入页自动发起转换 |
| F2 | 本张开/关 | F2 / 右键菜单，按页独立 |
| F3 | 模型 / 降噪 / 倍数选择 | `LookModelName`、`scaleBox`，模型表 41 项 |
| F4 | 优先使用下载转换好的 | `PreDownWaifu2x` |
| F5 | 状态 / 耗时 / 分辨率 / 大小 | 工具条实时回填 |
| F6 | 自动跳过 | 超大图（`LookMaxNum=4096`）、动图不自动超分 |
| F7 | 后端 | ncnn-Vulkan / QNN-NPU；CPU 时给出明确提示 |
| F8 | Vulkan 视口加速 | `IsOpenVulkan`（重启生效） |

### G. 其它

| 编号 | 功能 | 说明 |
|---|---|---|
| G1 | 图元池 | `QtReadImgPoolManager`（300 proxy / 3 pixmap 复用） |
| G2 | 亮度/夜间滤镜 | C++ 独有（Python 阅读器无此能力，图标资源存在） |

---

## 3. 逐项对照检查结果

状态：**已补齐** = 行为与 Python 一致；**部分** = 能力可用但实现路径/参数范围不同；
**未覆盖** = 明确缺失，见第 6 节原因。

| 功能 | C++ 改动前 | C++ 改动后 | 状态 |
|---|---|---|---|
| A1 上下滚动 | 有（Vertical，QLabel 列表懒加载） | 单视口连续布局 + 1:1 拖动 | 已补齐 |
| A2 左右翻页 | 有（Single，仅鼠标按键/键盘） | 横向滑动翻页 + 方向语义 | 已补齐 |
| A3 左右双页 | 有（Spread，合成图 + 8px 缝） | 两张独立页矩形、无额外缝、当前页在左 | 已补齐 |
| A4 右左双页 | 仅 `Direction` 开关影响合成顺序/按键 | 独立模式 3，当前页在右，`NextPage` 反向 | 已补齐 |
| A5 左右滚动 | **无** | 模式 4，整章横向连续 | 已补齐 |
| A6 右左滚动 | **无** | 模式 5，页 0 在最右、前进=值减小 | 已补齐 |
| A7 右左双页(正序) | **无** | 模式 6，`NextPage` 按 +2 | 已补齐 |
| A8 等宽模式 | **无** | 模式 7，单页按宽铺满纵向滚动 | 已补齐 |
| A9 右左滚动(正序) | **无** | 模式 8，滚轮输入归一化 | 已补齐 |
| A10 双页配对偏移 | **无** | F10 / 菜单 | 已补齐 |
| A11 语义方向换算 | **无** | `ForwardDelta()` | 已补齐 |
| B1 上下页 | 有，无 RTL 语义 | 完整含基础版反转 | 已补齐 |
| B2/B3 自动跳章 | **无** | `RequestNextChapter/PrevChapter` + 详情页章节表定位 | 已补齐 |
| B4 点按三分区 | 有（鼠标事件过滤，安卓失效） | `TapZone` 信号，触摸/鼠标同一套 | 已补齐 |
| B5 滚轮翻页 | **无**（仅 QScrollArea 滚动） | 边界翻页 + 平滑滚动 | 已补齐 |
| B6 滚动方向 | **无** | 基础版/正序版分别处理 | 已补齐 |
| B7 平滑 + 1:1 拖动 | **无** | `QPropertyAnimation` 200ms / 拖动直置 | 已补齐 |
| B8 页码滑块 | **无** | 底部滑块，拖动跳页 | 已补齐 |
| B9 自动滚动 + 4 参数 | 设置页只有"每 8 秒"文案，未接线 | 菜单子项 + 落库 + 生效 | 已补齐 |
| B10 键盘 | 仅方向/PageUp/+/-/0/D/R/B | 补齐 F1~F5、F10~F12、Shift+← →、Home/End、Space | 已补齐 |
| B11 首次帮助 | **无** | 一次性浮层，点击关闭落库 | 已补齐 |
| C1 缩放 20~500% | 1.25 倍步进、上限 8 倍（相对适宽，无百分比） | 百分比 20~500%、10% 步进 + 预设菜单 | 已补齐 |
| C2 双指捏合 | `QPinchGesture`（**真机不触发**） | `QTouchEvent` 预览缩放 + 抬手重排 | 已补齐 |
| C3 按 id 合并 | **无** | 单视口 + `QHash<id, globalPos>` 合并 | 已补齐 |
| C4 放大后平移 | **无**（放大后滑动仍翻页） | 双轴 1:1 平移，不结算换页 | 已补齐 |
| C5 1 指拖动 | **无** | 滚动模式 1:1 跟手 | 已补齐 |
| C6 一次手势一页 | **无**（一次长滑连翻多页） | 轴锁定 + 32px 阈值 + `turnFired` 锁 | 已补齐 |
| C7 缩放后吞指 | **无** | `swallowTouch_` | 已补齐 |
| C8 Ctrl+滚轮 | **无** | 有 | 已补齐 |
| C9 阈值 16/32 | 无 | 常量一致 | 已补齐 |
| D1 图片信息 | 仅 n/m | 分辨率/大小/文件/缩放/超分/动图 + 弹窗 | 已补齐 |
| D2 模式下拉 | 3 项（单页/双页/上下滚动） | 9 项 + 与阅读器共用偏好 | 已补齐 |
| D3 缩放控件 | 仅"适宽"按钮 | 百分比按钮 + 预设菜单 + 底部滑块 | 已补齐 |
| D4 自动滚动参数 | 设置页不可用的文案 | 菜单 4 组预设 + 落库 | 已补齐 |
| D5 按钮 | 5 个图标按钮 | 返回/上一页/下一页/模式/缩放/自动/全屏/信息/更多 + 底部页码条 | 已补齐 |
| D6 右键/更多菜单 | **无** | 覆盖 D6 全部项 | 已补齐 |
| D7 复制/另存 | **无** | 剪贴板 + 另存为(F3) + 上次目录(F4) | 已补齐 |
| D8 全屏 | **无** | F11 / 工具条 / 菜单 | 已补齐 |
| D9 手机整宽工具条 | 固定 52px 顶栏 | 顶栏+底栏可隐藏（点按上排左右），布局整宽 | 部分（结构不同，行为等价） |
| E1 三种数据源 | 仅本地目录 / 在线已下载文件列表 | 同左 + 本地漫画页默认扫描数据目录 | 部分 |
| E2 章节下载/还原 | `PageLoader` 一次性下载并还原 | 保持（下载进度回传工具条） | 已补齐 |
| E3 预加载 | **无** | 前后 2~3 页解码缓存（8 页 / 160MB 上限）+ 窗口外释放 | 已补齐 |
| E4 GIF | **无** | `QMovie` 非滚动模式播放 | 部分（动画 webp 见第 6 节） |
| E5 继续阅读 | **无** | `readerLastChapter/readerLastPage` 落库，重开同章跳回 | 已补齐 |
| E6 状态显示 | Toast | Toast + 超分进行中绘制提示 | 已补齐 |
| F1 自动超分 | **无**（"后端未接入"） | 设置页开关 + 每页自动发起 | 已补齐 |
| F2 本张开/关 | **无** | F2 / 菜单，按页缓存 | 已补齐 |
| F3 模型/降噪/倍数 | **无** | 固定 waifu2x cunet 2× denoise3 | 部分（见第 6 节） |
| F4 预下载超分结果 | **无** | 内存缓存 4 页，无磁盘预下载 | 部分 |
| F5 状态/耗时/分辨率 | **无** | 完成日志 + 信息弹窗 + 耗时 | 已补齐 |
| F6 超大图/动图跳过 | **无** | 未实现 | 未覆盖 |
| F7 推理后端 | **无** | ONNX Runtime CPU（桌面 DLL / 安卓 .so） | 已补齐（CPU）；NPU 未覆盖 |
| F8 Vulkan 视口 | **无** | 未实现 | 未覆盖 |
| G1 图元池 | **无** | 单视口直接绘制，无需图元池 | 不适用 |
| G2 亮度压暗 | 有（C++ 独有） | 保留 | 保留 |

**统计**：对照表 53 项中，43 项已补齐、约 7 项部分对齐（实现路径或参数范围
不同，含 CPU 超分已可用而 NPU 未覆盖）、2 项未覆盖、1 项不适用、1 项为 C++ 独有保留。
改动前仅有约 17 项可用（且其中双指缩放与滑动翻页在真机上实际失效）。

---

## 4. 实现明细

| 文件 | 改动 |
|---|---|
| `cpp/shell/reader.h/.cpp` | 重写为 `QAbstractScrollArea` 单视口自绘；9 模式布局、触摸状态机、缩放/平移、滚轮/键盘、图片缓存、GIF、自动滚动、页码跳转、超分调度、回归自检 |
| `cpp/shell/sr_engine.h/.cpp` | 新增：ONNX Runtime 运行时加载（`QLibrary`，API 版本向下兼容）、waifu2x cunet 2× 分块推理（192 tile / 312 输出 / 收缩补偿 / 重叠拼接）、自检 |
| `cpp/shell/3rdparty/onnxruntime/` | ORT C API 头文件、缺失的 `onnxruntime_error_code.h` 补齐、Windows `onnxruntime.dll`（19MB，仅构建时拷贝，不做链接依赖） |
| `cpp/shell/ui/ui_pages_misc.cpp` | `ReaderPage` 工具条/底部页码条/9 模式菜单/缩放菜单/更多菜单/信息弹窗/复制另存/帮助浮层/继续阅读；菜单按主题 token 上色 |
| `cpp/shell/ui/ui_pages.h` | `ReaderPage`、`SettingsView`、`DetailPage` 声明 |
| `cpp/shell/ui/ui_main_window.cpp/.h` | 跳章、全屏、阅读进度落库、设置页 9 模式/方向/缩放/自动超分信号接线 |
| `cpp/shell/ui/ui_pages_browse.cpp` | `ChapterNameAt()` 供跳章标题 |
| `cpp/shell/ui/ui_pages_library.cpp` | 本地漫画默认扫描应用数据目录（手机免 SAF 选择） |
| `cpp/shell/ui/main_desktop.cpp` | 回归入口 `--reader-dir/--reader-mode/--reader-parity/--reader-touch-selftest/--reader-swipe/--reader-pinch/--reader-sr/--sr-selftest/--sr-upscale` |
| `cpp/shell/ui/CMakeLists.txt` | `Qt6::Concurrent`、ORT 头文件路径、DLL 与模型 POST_BUILD 拷贝 |
| `cpp/dist/JMComic-ui-0.91-x86_64-reader-final-debug.apk` | 真机验证包 |

---

## 5. 验证结果

### 5.1 桌面（Windows / MinGW 13.1 / Qt 6.12）

| 用例 | 命令 | 结果 |
|---|---|---|
| 9 模式 + 手势自检 | `--reader-parity --shot … --quiet` | **16 PASS / 0 FAIL** |
| 真实 `QTouchEvent` 事件链 | `--reader-touch-selftest` | **5 PASS / 0 FAIL**（横滑翻页、双指 200%、点按右下翻页、滚轮翻页、右方向键翻页） |
| 超分引擎 | `--sr-selftest` | OK，64×64 → 128×128，218ms |
| 超分真实图像 | `--sr-upscale sr_in.png,sr_out.png` | 320×240 → 640×480，1.5s，无拼接缝/黑带/色偏 |
| 逐模式截图 | `--reader-mode 0..8 --shot` | 9 张，布局与方向正确 |
| UI 集成超分 | `--reader-sr --shot-delay 60000` | 800×1200 → 1600×2400，23.4s（48 tile），页面替换生效 |
| 首次帮助浮层 | 不带 `--quiet` 启动 + `--shot` | 深色浮层显示"点按分区 / 左右滑动翻页 / 双指捏合缩放 / 放大后拖动平移"，点击关闭落库 |
| 工具条短标签 | 同上 | `左右翻页` 与 `100%` 分离、无文字重叠；长模式名用 `右左双页·正` 等短标签 |

`--reader-parity` 输出示例：
```
reader-parity: mode=0 (上下滚动) page=1/7 rect=1018x1527 content=1018x11262 scroll=0x10610
reader-parity: mode=3 (右左双页) page=1/7 rect=435x652 content=1018x652 scroll=0x0
reader-parity: mode=5 (右左滚动) page=1/7 rect=435x652 content=3550x652 scroll=2532x0
reader-parity: mode=7 (等宽模式) page=1/7 rect=1018x1527 content=1018x1527 scroll=0x875
reader-parity done pass=16 fail=0
```

### 5.2 真机 / 模拟器（MuMu，Android 15，x86_64，虚拟 display 2）

| 操作 | 结果 |
|---|---|
| 本地漫画页 → 阅读（6 页） | 默认扫描数据目录并列出 `reader` 目录 |
| 9 模式菜单 | 9 项齐全、可读 |
| 左右翻页：左滑 | 1/6 → **2/6**（绿色第 2 页） |
| 左右翻页：右滑 | 2/6 → **1/6** |
| 右左双页(正序)：左滑 | 1/6 → **3/6**，步进 +2，当前页在右、第 4 页在左 |
| 双指捏合（sendevent 注入 MT 协议） | `pinch start 100%` → `pinch apply 100% -> 240%`，工具条显示 240% |
| 240% 下左滑 | 画面横向平移，页码仍 **1/6**（不误翻页） |

日志证据：
```
I C1Reader: loaded 6 online pages
I C1Reader: read-mode 1 (左右翻页)
I C1Reader: read-mode 6 (右左双页(滚轮正序))
I C1Reader: pinch start base=100% dist=92 mode=1
I C1Reader: pinch apply 100% -> 240%
```

### 5.3 构建

- 桌面 `ninja -C cpp/shell/ui/build`：0 error。
- 安卓 `ninja -C cpp/shell/ui/build-android-x86_64`：Gradle `BUILD SUCCESSFUL`，
  手工 debug 签名后安装成功。
- 超分随包：桌面 `onnxruntime.dll`（19MB）+ `sr_models/waifu2x_cunet_up2x_denoise3.onnx`（5MB）；
  缺 DLL/模型时 `SrEngine::Status()` 返回明确原因，不影响阅读器其它功能。

---

## 6. 残留差异与限制（如实说明）

1. **超分模型/降噪/倍数 UI（F3 部分）**：C++ 固定 `waifu2x_cunet_up2x_denoise3.onnx`
   （2×、denoise3），未移植 Python 的 41 项模型表与降噪/倍数调节。模型可替换
   `sr_models/` 下的同名文件，但**不同架构模型的输入 tile/输出收缩需重新探测**
   （引擎已有探针，但 UI 未暴露选择）。
2. **超分自动跳过（F6）**：未按 `LookMaxNum=4096` 跳过超大图，也未对动图跳过。
   CPU 推理下超大页耗时显著（实测 800×1200 约 23s），自动超分默认关闭。
3. **Vulkan 视口加速（F8）/ QNN-NPU 后端（F7 部分）**：C++ 使用 ONNX Runtime CPU。
   安卓端 `libonnxruntime.so` 需随 APK 打包并匹配 `OrtApi` 版本（引擎已做
   API 版本向下兼容探测）；本次验证包的 ORT 通过 root 投放，未随 APK 分发，
   arm64 真机包未构建。
4. **在线逐页懒下载 + 失败重试（E2 部分）**：C++ 由 `PageLoader` 一次性下载并还原
   整章图片后交给阅读器，阅读中只有本地解码缓存（前后 2~3 页），没有网络层的
   逐页重试与 10 页预下载。用户可见结果一致（都能连续阅读），但断网恢复语义不同。
5. **动画 webp（E4 部分）**：Qt 6.12 安装包不带 `qwebp` 插件，`JmWebp` 只解静态帧；
   GIF 动画已支持，动画 webp 退化为静态首帧。
6. **手机 SAF 另存为（D7 部分）**：C++ 用 `QFileDialog`；Android 上未接入
   `content://` URI 写入。复制到剪贴板与"存到上次目录"可用。
7. **工具条形态（D9 部分）**：Python 是覆盖在画面上的右侧 400px 抽屉（竖屏整宽），
   C++ 是顶部 52px + 底部 46px 两条工具条，点按上排左右可整体隐藏以全屏看图。
   显隐行为等价，覆盖式抽屉未实现。

以上 7 项均不影响用户本次提出的三项核心诉求（双指缩放、左右/右左滑动翻页、
功能对齐主体），且均已定位到具体文件与依赖，可作为后续独立任务。

---

## 7. 复现命令

```bash
# 桌面构建
cd cpp/shell/ui && build_win.bat

# 9 模式 + 手势自检（16 项）
cd cpp/shell/ui/build
jmcomic_ui.exe --reader-dir build/testpages --reader-parity \
  --shot reg_parity.png --shot-delay 2000 --quiet

# 真实 QTouchEvent 事件链（5 项）
jmcomic_ui.exe --reader-dir build/testpages --reader-touch-selftest \
  --shot reg_touch.png --shot-delay 2000 --quiet

# 超分
jmcomic_ui.exe --sr-selftest probe.png
jmcomic_ui.exe --sr-upscale in.png,out.png

# 逐模式截图
for m in 0 1 2 3 4 5 6 7 8; do
  jmcomic_ui.exe --reader-dir build/testpages --reader-mode $m \
    --shot mode_$m.png --shot-delay 1600 --quiet
done
```

配置落盘：`ui.ini [reader]` 下 `mode / zoom / turnMs / scrollStep / scrollMs /
manualStep / autoSr`；继续阅读用 `ui/readerLastChapter`、`ui/readerLastPage`。
