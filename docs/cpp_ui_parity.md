# C++ 版 UI 实现与 Python 版对齐说明

> **2026-11-10 更新**：界面层的 `DemoData` 已整体移除——首页、搜索、详情、章节、
> 收藏、账号、评论、下载、历史等所有展示内容只来自服务端接口（或本地真实扫描），
> 取不到就显示加载态/空态，不再有任何本地样例兜底。本文档下方出现的
> `--demo-*` / `--paged` / `--items` / `--cover-dir` / `--cover-url` / `--real-data`
> 等参数属于当时的验证记录，现已从 CLI 删除。

日期：2026-11-03
代码：`cpp/shell/ui/`（8,853 行 C++ + 117 行 QML，Qt 6.12 Widgets/Quick / C++17）
产物：`cpp/shell/ui/build/jmcomic_ui.exe`（1.1 MB，MinGW-w64 13.1）
截图：`docs/shots/*.png`（`--shot` 自动生成，2560×1680，DPR 2）

---

## 1. 交付范围

| 项 | 内容 |
|---|---|
| 新增代码 | 主题层 `theme.*`、组件库 `ui_kit.*`、页面层 `ui_pages*`（3 个 TU）、窗口层 `ui_main_window.*`、桌面入口 `main_desktop.cpp` |
| 复用 | `res/theme/*.qss`（Python 版同一套 6 个主题，原文件不复制、不修改）<br>`cpp/shell/reader.cpp`（C1-W2/W3 的阅读器：双页 / RTL / 亮度 / 缩放）<br>`cpp/shell/net_client.*` `jm_api.*`（C1 已有的进程内网络层，接口保留） |
| 第二轮修复 | 自绘 `ModalDialog` 取代 QMessageBox；`ComicGrid` 改 `QListView + QAbstractListModel + QStyledItemDelegate` 虚拟化；新增 `CoverCache`（本地/HTTP 异步加载 + 内存/磁盘缓存 + 失败回落） |
| 页面 | **14 个页面全部可交互**，覆盖 Python 版主流程（首页→详情→阅读→下载/收藏/历史/设置） |
| 数据 | `DemoData`（离线样本，18 部漫画 / 36 话 / 5 个下载任务 / 4 条评论 / 41 模型中的 4 个），接口字段与 `jm_api.h` 对齐 |
| 构建 | `ui/build_win.bat`（配置 + 编译 + 可选 windeployqt 打包） |

代码分层：

```
main_desktop.cpp     入口：--page / --theme / --shot（截图回归）
ui_main_window.*     无边框窗口 + 自绘标题栏 + 左侧导航 + 页面栈 + 返回栈
ui_pages.h           14 个页面类声明（唯一页面头）
ui_pages_browse.cpp  首页 / 分类 / 搜索 / 详情
ui_pages_library.cpp 下载 / 本地 / 收藏 / 历史 / 评论
ui_pages_misc.cpp    阅读器 / 超分 / 设置 / 关于 / 登录
comic_view.*         虚拟化网格：模型 + delegate + ComicGrid（卡片绘制与独立控件共用 CardPaint）
cover_cache.*        封面加载：内存缓存 → 本地/HTTP → 生成图回落；按目标尺寸解码 + 居中裁剪
ui_kit.*             卡片绘制（CardPaint）、模态框（ModalDialog）、开关、胶囊、搜索框、设置行、空态、Toast、图标
theme.*              6 套主题的加载与 token 化（QSS 覆盖层）
models.h             界面契约（Comic/Chapter/DownloadItem/CommentItem/SrModel）
```

---

## 2. 页面覆盖矩阵

Python 版有 64 个 `python/src/view/**/*.py`（含实现细节类与 DB 类）。按"用户可进入的页面"归并后对照如下：

### 2.1 已实现（14 页）

| C++ 页面 | 对应 Python view | 主要交互 |
|---|---|---|
| `HomePage` | `index/index_view.py` | 主推位、分区胶囊、封面网格、卡片→详情、收藏切换、刷新提示 |
| `CategoryPage` | `category/category_view.py` | 三个筛选下拉 + 应用筛选 + 网格 |
| `SearchPage` | `search/search_view.py` | 搜索框回车/按钮、历史胶囊、热门标签、结果区切换、空关键词校验 |
| `DetailPage` | `info/book_info_view.py`、`info/book_eps_view.py` | 封面/元信息/简介、标签与作者名点击（按该词跳搜索页并立即搜第 1 页）、开始阅读（指定章）、下载本章、收藏、分卷胶囊、36 话网格 |
| `ReaderPage` | `read/read_view.py`、`read_graphics.py`、`read_tool.py` | 工具栏返回、适宽、双页、方向、亮度；页码联动；真实 `ReaderView` 渲染 |
| `DownloadPage` | `download/download_view.py`、`download_item.py` | 状态胶囊筛选、进度条、暂停/删除、全部暂停、队列统计 |
| `LocalPage` | `tool/local_read_view.py`、`local_read_eps_view.py` | 目录选择（QFileDialog）、目录列表、阅读入口 |
| `FavoritePage` | `user/favorite_view.py`、`favorite_fold_view.py` | 收藏夹胶囊、网格、管理入口 |
| `HistoryPage` | `history/history_view.py` | 今天/昨天/更早分组、继续阅读、清空 |
| `CommentPage` | `comment/all_comment_view.py`、`my_comment_view.py`、`sub_comment_view.py` | 评论输入与发送、点赞、回复、热排序提示 |
| `SrPage` | `tool/waifu2x_tool_view.py`、`setting/setting_sr_select_view.py` | 模型列表（NPU/CPU 标记）、切换提示、测速、预览区 |
| `SettingsView` | `setting/setting_view.py` | 五组设置、主题即时切换、开关/下拉、设置项搜索过滤 |
| `AboutPage` | `help/help_view.py`、`help_log_widget.py` | 版本/工具链信息、导出日志、检查更新 |
| `LoginPage` | `user/login_new_view.py`、`login_proxy_new_widget.py` | 账号密码校验、登录状态、代理入口跳转、注册/找回占位 |

### 2.2 未覆盖（按组归并，共 12 组）

| 组 | Python 文件 | 归并到哪个模式 | 预估 |
|---|---|---|---|
| NAS 管理 | `nas/nas_view.py`、`nas_add_view.py`、`nas_item.py`、`nas_status.py` | 列表 + 表单（同 Local/Login） | 5 人日 |
| 下载弹窗 | `download/download_all_view.py`、`download_some_view.py`、`download_some_edit_view.py`、`download_some_output_view.py`、`download_dir_view.py` | 模态对话框（需新增 Dialog 组件） | 4 人日 |
| 工具页 | `tool/batch_sr_tool_view.py`、`doh_dns_view.py`、`forbid_words_view.py` | 表单 + 列表 | 3 人日 |
| 用户中心 | `user/login_new_widget.py`、`user_manager_widget`、`local_favorite_view.py`、`local_favorite_fold_view.py` | 列表 + 表单 | 3 人日 |
| 远程历史 | `history/remote_history_view.py` | 同 HistoryPage | 1 人日 |
| 周历分区 | `index/week_view.py` | 首页内嵌视图 | 1 人日 |
| 阅读实现细节 | `read_pool.py`、`read_proxy.py`、`read_qgraphics_proxy_widget.py`、`read_vulkan.py` | 非页面（C1 用另一种实现替代） | — |

合计约 **17 人日**。这些页面不引入新的交互范式，都是"列表 / 表单 / 对话框"三类的组合。

---

## 3. 差异点与适配方式

以下差异都是**实现方式**不同，不是功能取舍。凡涉及视觉的，均已按同一套 token（`#31363b` 底、`#FF7B00` 强调色、`#232629` 描边）对齐。

### 3.1 布局：绝对坐标 → 布局管理器

- **Python**：`uic` 生成的代码是 `setGeometry(QRect(x, y, w, h))` 的绝对定位，窗口缩放靠手写 `resizeEvent` 重排。
- **C++**：全部用 `QVBoxLayout/QHBoxLayout/FlowLayout`，卡片网格按可用宽度自动折行（1280 宽 6 列、窄窗 4 列）。
- **影响**：同一窗口尺寸下像素位置不会与 Python 完全一致（间距按 18/16/12 三档栅格），但**缩放行为更好**——Python 在小窗口下会出现控件挤压，C++ 版会重排。
- **适配**：主内容左右留白 28px、卡片间距 18px、行高 52px，均与 Python 的常用值对齐；导航栏 260px 定宽、可整体收起（§13）。

### 3.2 样式：同一份 QSS + 覆盖层

- **Python**：主题 QSS（QtMaterial 派生）+ 145 处 `setStyleSheet` 内联样式（控件级）。
- **C++**：**原样加载** `res/theme/*.qss`（6 套，未修改文件），再追加一层 `BuildOverlay()`，用 `objectName` 选择器补齐 Python 内联那部分（卡片、胶囊、导航、设置行、Toast 等）。
- **影响**：基础控件（滚动条、输入框、下拉、表格）与 Python 完全同源；自绘组件是"同 token 重写"，不是逐条复制。
- **踩过的坑（已修）**：主题里有 `QWidget { background-color: #31363b; }`，会让每个裸 `QLabel`/`QWidget` 在父容器上盖一块底色（表现为文字背后一条深色带）。覆盖层统一 `background-color: transparent`，再由 `#Root`/`#ListRow`/`#TitleBar`/`#NavRail` 这些 id 规则上色。另外 `@accentSoft` 必须**先于** `@accent` 替换，否则会得到 `#1de9b6Soft` 这种非法颜色（曾导致导航选中态、标签胶囊全部失效）。

### 3.3 字体

- **Python**：QSS 里写死 `font-family: Roboto`，Linux/Android 上一般能命中；Windows 上通常没有 Roboto，会回退到系统默认。
- **C++**：按 `Microsoft YaHei UI → Microsoft YaHei → PingFang SC → Noto Sans CJK SC → Segoe UI` 顺序取第一个可用字体，字号 9.75pt。
- **影响**：Windows 上 C++ 版字形更规整（雅黑），行高比 Roboto 略高 1~2px；卡片标题与元信息的两行高度按 18px/14px 固定，不会因字体变化串行。

### 3.4 图标：位图资源 → 矢量绘制

- **Python**：导航/按钮图标来自 `res` 下的 png（`:/png/icon/*`），由 `pyside6-rcc` 打进 `images_rc.py`（22.3 MB）。
- **C++**：`Glyph::PathFor()` 用 `QPainterPath` 画 28 个图标，按目标 DPI 以 2× 渲染（`setDevicePixelRatio(2.0)`），颜色随主题传入。
- **影响**：形状是"同语义近似"，不是像素级复刻；收益是任意 DPI 清晰、可随主题着色、不依赖 22 MB 资源包。
- **适配**：图标线宽 1.7/24 视口，与 Python 24px 图标的视觉重量接近；导航图标 20px、工具栏 14~16px、空态 40px。

### 3.5 无边框窗口

- **Python**：自研 `QFramelessWindow`（拖拽、九宫格缩放、最大化、贴边）在 Python 里维护。
- **C++**：`Qt::FramelessWindowHint` + `QWindow::startSystemMove()` / `startSystemResize()`，交给 Windows 原生实现。
- **影响**：C++ 版支持 Aero Snap（拖到屏幕边缘分屏），Python 版是自绘逻辑、行为略有差别；标题栏高度 44px，按钮热区 38×30。
- **适配**：双击标题栏最大化；边缘 6px 触发缩放光标。

### 3.6 动画

- **Python**：33 个 `QPropertyAnimation`，换页用 6ms 精确定时器 + `QRhiWidget` GPU 覆盖层（真机 110~123 fps），着色器为预编译 `.qsb`，逐帧零 uniform。
- **C++**：换页改为 **QML 场景图**——`qml/PageSwipe.qml` 声明两个 `Image` 与位移规则，`page_transition.cpp` 负责快照投递与首帧/失败处理。逐帧只改两个 `Image` 的 `x`，纹理只在 Begin 时上传一次；时间轴用 QML `NumberAnimation`（250 ms / Linear，与 Python 版 `Easing` 一致）。
- **与 Python 的映射**：`setViewport(x=-off)` ↔ `prev.x = -off`、`setViewport(x=W-off)` ↔ `next.x = W-off`。两者都是"2 个四边形 + 零逐帧上传"，差别只是不再需要手写 QRhi 管线与 `.qsb`（省掉 `pyside6-qsb` 这条工具链）。
- **为什么是 QQuickWidget 而不是 QQuickView**：覆盖层不透明（两张整页快照铺满页面区），不需要透明合成；但必须是普通子控件，才能与 `QStackedWidget` 严格同几何、同 z 序。代价是每帧多一次全屏 FBO 合成，桌面实测可接受（见 §8.1）。
- **仍缺**：开关/悬停之外的 33 个细粒度动画（弹层淡入、点赞心跳等）没有逐个移植——那些是 Python 侧的内联实现，逐个搬性价比低。

### 3.7 列表与滚动

- **Python**：`setItemWidget` 的 widget-per-item（60 条 250~500 ms），靠分批加载掩盖。
- **C++**：`FlowLayout` + 自绘 `ComicCard`（一次 `paintEvent` 画封面/角标/进度/标题/元信息），60 条实测无卡顿。
- **C++（第二轮起）**：`ComicGrid` = `QListView(IconMode)` + `ComicListModel(QAbstractListModel)` + `ComicDelegate`，卡片绘制抽成 `CardPaint::Paint()` 与独立控件共用；支持 `canFetchMore/fetchMore` 分页。
- **实测**：5004 条数据下 delegate 只被调用 40 次（18 条时同样是 40 次），绘制次数不随行数增长（见 §6.2）。

### 3.8 鼠标 / 触屏

- **Python**：8,309 行 `mobile_*.py`（tap gate、长按、双指缩放、返回键、抽屉、分屏、输入法）。
- **C++**：桌面默认走鼠标语义，`TouchAdapter`（`touch.cpp`）在 Android 或 `--touch` 时接管：
  边缘右滑返回（带过程提示）、手势滚动（`QScroller`）、44dp 命中目标、输入法把焦点控件滚进可视区、安全区内边距、`Qt::Key_Back` 返回键；阅读器点按分区与 Python `_Tap` 的 3×2 语义一致。
- **与 Python 的差别**：手机端改为抽屉式侧栏（§15）；桌面/sidebar 模型为 260px 常驻，菜单按钮 85ms 收起/展开（§13）。分屏/悬浮窗交给系统。
- **验证方式**：桌面用 `--touch` 打开触摸模式后，可以用鼠标走完整条手势路径（同一份代码），见 §8.2。

### 3.9 数据层

- **Python**：`curl_cffi`（TLS 指纹 + ECH + H3）→ `JmCryptoTool.decode_resp_data` 解密。
- **C++**：`net_client.cpp` + `jm_api.cpp`（C1 已实现，进程内请求）；本 UI 默认走 `DemoData`。
- **影响**：页面与数据解耦，替换数据源只需把 `DemoData::Comics()` 换成 `JmApi::Search()` 的回调结果，页面代码不动。
- **适配**：`models.h` 的字段名与 `jm_api.h` 的 JSON 键一一对应（`id/title/author/tags/status/description/chapters`）。

### 3.10 阅读器

- **Python**：`read_view.py` + `read_graphics.py`（60 页 proxy+QLabel，154 ms 建场）+ 预加载池 + GPU 覆盖层。
- **C++**：直接复用 C1 的 `ReaderView`（`reader.cpp`，459 行）：适宽/缩放/双页/RTL/亮度/手势都有，**页项是单个 QLabel 而非 proxy**，建场成本低。
- **影响**：功能对齐，缺少 Python 的"预加载 10 页"与换页动画；翻页是立即渲染。
- **适配**：阅读器入口统一为 `OpenComic(comic, chapterIndex)`，后续接真实图片下载只替换 `BuildDemoPages()`。

### 3.11 主题切换

- **Python**：部分主题切换需要重启（RHI 后端等环境变量只在首次读取）。
- **C++**：`UiTheme::Apply()` 重新拼 QSS 并 `qApp->setStyleSheet()`，**立即生效**；代价是整窗重绘一次。
- **影响**：C++ 更顺手；但自绘组件（卡片封面、图标）需要重新取色，已在 `UiTheme::Changed` 里重建图标，封面配色与主题无关（按标题哈希）。

### 3.12 国际化

- **Python**：`translate/*.ts` + 运行时切换语言。
- **C++ 首版**：中文硬编码。
- **适配**：文案集中在各页面构造函数里，接 `QTranslator` 时把字面量换成 `tr()` 即可；预估 1 人日（机械替换 + 4 个 .ts）。

### 3.13 对话框

- **Python**：自定义 `MessageBox` / `Toast` / 输入框，皮肤跟随主题。
- **C++**：`Toast` 已按主题实现（同一时刻只保留最新一条，避免叠字）；确认框暂用 `QMessageBox`（系统皮肤）。
- **影响**：确认框视觉风格与主界面不一致，是当前最明显的"出戏点"。
- **适配**：需要补一个 `ModalDialog` 组件（约 120 行），供下载弹窗、删除确认、NAS 表单复用。

### 3.14 高 DPI

- **Python**：依赖 Qt 自动缩放，图标是位图，2× 屏上略糊。
- **C++**：图标按 2× 绘制、卡片尺寸用逻辑像素、截图用 `QWidget::grab()`（本例 2560×1680 即 1280×840 @2x）。
- **影响**：C++ 在高分屏更清晰；但 `FlowLayout` 的折行按逻辑宽度计算，125%/150% 缩放下列数会变化（属预期行为）。

---

## 4. 验收与复现

```bat
rem 1) 构建（Qt 6.12 mingw + Ninja，脚本内已写死本机路径）
cd android\c1_shell\ui
build_win.bat

rem 2) 启动（默认首页）
build\jmcomic_ui.exe

rem 3) 指定页面 / 主题
build\jmcomic_ui.exe --page settings --theme light_teal

rem 4) 截图回归（输出 2560×1680 PNG 后自动退出）
build\jmcomic_ui.exe --page home --shot ..\..\..\docs\shots\home.png

rem 5) 用真实图片目录当封面（联调/演示）；生产由 JmApi 填 Comic.coverUrl
build\jmcomic_ui.exe --page home --cover-dir D://covers --shot out.png

rem 6) 压测：生成 N 条数据，验证虚拟化
build\jmcomic_ui.exe --page home --items 5000 --shot out.png   rem stderr 会打印 bench 行

rem 7) 确认框外观（截图用）
build\jmcomic_ui.exe --page history --demo-dialog --shot dialog.png
```

验收口径：

| 项 | 标准 | 现状 |
|---|---|---|
| 启动 | 双击即出首帧，无控制台报错 | 通过（冷启 <300 ms） |
| 导航 | 12 个入口切换、选中态跟随、返回栈可回退 | 通过 |
| 主流程 | 首页卡片 → 详情 → 开始阅读 → 阅读器翻页/缩放/双页 | 通过（阅读器为真实实现） |
| 设置 | 主题即时切换、开关可点、设置项可搜索过滤 | 通过 |
| 截图 | 14 个页面均可 `--shot` 出图，无空白页/重叠 | 通过（`docs/shots/`） |
| 16 KB 对齐 | 本目标为桌面程序，不涉及；Android 侧见 `check_elf_page_align.py` | 不适用 |
| 列表虚拟化 | 5000 条数据下只绘制可见项（`painted` 不应随行数增长） | 通过：5004 行 / 40 次绘制（见 §6.2） |
| 确认框 | 跟随主题、不出现系统浅色对话框 | 通过（`docs/shots/dialog.png`） |
| 封面 | 本地图片可解码、按 3:4 裁剪、圆角、失败回落 | 通过（`docs/shots/home_covers.png`） |
| 封面 HTTP | 起本地服务 → 出图 → 落盘 → 停服务后仍出图 | 通过（`home_http.png` 与 `home_offline_cache.png` md5 相同） |
| 分页 | 首屏只取一页，滚到底追加 | 通过：60 → 240（3 次滚底） |
| 键盘 | Tab 进入、方向键移动、Enter 打开、Esc 返回 | 通过（焦点环可见） |
| 长按 | 520ms 弹菜单，移动即取消 | 通过（右键同路径） |

---

## 5. 已知问题（第三轮修复后）

已修复：确认框换皮、列表虚拟化、封面加载（含 HTTP 与磁盘缓存）、内存 LRU、分页、键盘可达性、长按菜单（§6/§7）、**QML 换页动画**、**触摸适配层**（§8）。
仍存在：

1. **国际化未接**：14 个页面的文案是硬编码中文。需要产品先定语言集合，再统一 `tr()` + `.ts`/`.qm`，属机械改造，但半做等于没做。
2. **安全区只覆盖左右/顶部，不再保留底部手势条**：`Touch::SafeArea()` 用 `QScreen::geometry` 与 `availableGeometry` 的差值推算左右/顶部；底部恒为 0（手势导航条按 edge-to-edge 处理，避免多出一条 78dp 空白带）。要拿刘海信息必须走 JNI `getRootWindowInsets()`，且需真机验证。已提供 `JM_SAFE_INSETS`/`--safe-insets` 作为兜底（底部仍会被 `ApplySafeArea()` 归零）。
3. **下拉框箭头是运行时位图**：本机 Qt 未加载 svg 图像插件，箭头由 `UiTheme` 在 `Apply()` 时画成 PNG（颜色随主题）。
4. **`--page detail/reader` 直接进入时有默认内容**：为了截图与冒烟不出现空页，真实运行时由卡片点击传入数据。
5. **换页快照是同步抓取**：一次切页要 `render()` 两页（1194×796 实测 21~46 ms）。页面越复杂越慢，后续可缓存目标页快照（Python 版有 `CacheEnabled`）或改用 backing store 局部拷贝。
6. **QML 覆盖层在设备侧未实测帧率**：桌面 QQuickWidget 每帧多一次全屏 FBO 合成，Android 上建议改 `QQuickView` + 原生窗口容器（覆盖层不透明，不需要 alpha 合成）。

---

## 6. 第二轮修复记录（2026-11-03）

### 6.1 确认框：QMessageBox → 自绘 ModalDialog

| 项 | 内容 |
|---|---|
| 问题 | 系统 QMessageBox 在深色主题里是一块浅色系统窗口，和主界面完全两个观感 |
| 实现 | `ui_kit.h/.cpp` 的 `ModalDialog`：父窗口内的遮罩层 + 居中卡片（`#DialogCard`，radius 14、`@surface` 底色、`@border` 描边），标题 15px/500、正文 13px/`@textDim`，按钮为 `Ghost` + `Primary`/`DangerBtn` |
| 交互 | 点遮罩或 Esc = 取消；Enter = 确定；`Confirm` 用嵌套 `QEventLoop` 同步返回结果；`Message` 单按钮、2.5 s 自动关闭；父窗口 resize 时遮罩跟随（event filter） |
| 已接入 | 历史页「清空历史」（危险样式）、下载页「移除记录/删除任务」、收藏页「管理收藏夹」提示 |
| 证据 | `docs/shots/dialog.png`：遮罩覆盖整窗、卡片居中、红色危险按钮，无系统浅色窗口 |
| 注意 | `Confirm` 是阻塞调用，不要在构造/绘制路径里用；`Show` 是不阻塞版本，仅供截图/演示 |

### 6.2 列表虚拟化：widget-per-item → QListView + delegate

| 项 | 内容 |
|---|---|
| 问题 | 每部漫画一个 QWidget（含封面 pixmap），1000 条就是 1000 个控件；滚动/建场成本随数据量线性增长 |
| 实现 | `comic_view.h/.cpp`：`ComicListModel`（QAbstractListModel，一次 `beginResetModel` 换数据）、`ComicDelegate`（QStyledItemDelegate，`IconMode` 网格）、`ComicListView`（IconMode + Adjust + uniformItemSizes + 无边框 + 逐像素滚动） |
| 外观一致 | 卡片绘制抽成 `CardPaint::Paint()`，`ComicCard`（独立控件）与 delegate 共用同一份代码，尺寸 `CardPaint::Size(w)` |
| hover | QListView 自带 `State_MouseOver`，delegate 直接读，不需要额外 hover 管理 |
| 布局调整 | 首页/分类/搜索/收藏改为「页头固定 + 网格自滚动」，不再套外层 `QScrollArea`（嵌套滚动会互相抢滚轮） |
| 压测 | `--items 5000` 生成 5004 条数据：`setComics=0ms`、`painted=40`、`grab=16ms`；对照组 18 条同样 `painted=40`、`grab=19ms` —— **绘制次数不随行数增长** |
| 残余 | 仍未做「键盘全量导航」（QListView 只滚到可见项）；数据源仍是内存 QVector，接分页接口时需补 `canFetchMore/fetchMore` |

### 6.3 封面：哈希占位图 → 可加载真实图片

| 项 | 内容 |
|---|---|
| 问题 | `CoverArt::Render()` 只会画渐变块，真实接口接上后没有落点 |
| 实现 | `cover_cache.h/.cpp`：`Get(key,title,url,size)` 同步返回（缓存命中 → 真实图；否则 → 生成图并异步加载）；本地文件走 `QImageReader` 按目标尺寸解码（JPEG 走 DCT 缩放）+ `KeepAspectRatioByExpanding` 居中裁剪；HTTP 走 `QNetworkAccessManager`（重定向策略、UA、失败标记）；成功后写内存缓存与磁盘缓存（`CacheLocation/<key>.png`） |
| 接入 | `Comic.coverUrl` 字段；`CardPaint` 统一从 `CoverCache` 取图；`CoverCache::Loaded` → 列表 `viewport()->update()`，封面到货自动刷新，不需要重建行 |
| 数据源 | `--cover-dir <dir>` 把目录里的 png/jpg/webp 依次分配给演示数据；生产由 `JmApi` 填 `coverUrl`（http(s) 直接可用） |
| 证据 | `docs/shots/home_covers.png`：真实图片按 3:4 居中裁剪 + 圆角 + 角标叠加，与生成图共用同一条绘制路径 |
| 残余 | 封面尺寸按卡片宽度缓存（同一 key 不同尺寸各存一份），大列表下内存占用需加 LRU 上限（当前未做） |

### 6.4 修复过程中踩到的两个坑

1. **`ui_pages.h` 被 AUTOUIC 当成 uic 产物**、**AUTOMOC 不处理没有同名 .cpp 的头** —— 已在第一轮解决，本轮新增 `comic_view.h`/`cover_cache.h` 时同样要把头文件列进 `qt_add_executable`。
2. **`ninja` 失败被 `grep` 过滤掉**：一次构建实际失败（只改了 `.h` 里被 moc 的类），但命令只打印了 python 的 "ok"，结果拿旧二进制反复截图，误判"修复无效"。之后统一用 `ninja; echo exit=$?` 判结果。


---

## 7. 第三轮修复记录（2026-11-03 晚）

### 7.1 封面 HTTP 分支：端到端验证

之前只验证了本地文件，HTTP 写着"未验证"。本轮补齐，并顺手补了**磁盘缓存读**（原来只写不读，等于离线就丢图）。

| 步骤 | 结果 |
|---|---|
| 起本地服务 `python -m http.server 8765 --bind 127.0.0.1` | `curl` 200 |
| 应用内探针 `--net-probe http://127.0.0.1:8765/001.png` | `finished=1 error=0 bytes=45201`（确认 Qt 网络栈本身可用） |
| `--cover-url http://127.0.0.1:8765/` + `JM_CACHE_DIR=build/covercache` | 18 张落盘（`100000_168x224.png` …），内存缓存 10,584 KB，`hits=61 misses=18` |
| **停掉服务器**后重跑同一条命令 | `curl` 返回 000，应用仍出图：截图 md5 与联网时**完全一致**，`grab=15ms` |

结论：`Get()` 的查找顺序（内存 → 磁盘 → 网络 → 生成图）确实生效；断网时命中磁盘缓存，不再退回占位图。

配套改动：

- `DiskPath` 里的 key 做字符清洗（`p0@168x224` → `p0_168x224.png`），避免 `https://` 变成路径分隔符；
- 磁盘文件读失败时删除并重下（防止 0 字节/半截文件永久占坑）；
- 新增 `JM_CACHE_DIR` 环境变量覆盖缓存目录——受限账户/只读沙箱下 `QStandardPaths::CacheLocation` 可能"写入被重定向但不报错"（表现为 `save()` 返回 true 却找不到文件），显式指定是唯一可靠做法。

### 7.2 内存 LRU

`CoverCache` 从无上限的 `QHash` 改为 **LRU（400 条 / 96 MB 双约束）**：命中刷新位置、插入后按先到者淘汰、统计 `hits/misses/bytes` 并打进 bench 行。
按 168×224@2x 单张约 1.2 MB 估算，400 条约 470 MB —— 因此实际由 96 MB 先触发，约 80 张；想看更大范围可调 `kMaxBytes`。

### 7.3 分页（fetchMore）

| 项 | 内容 |
|---|---|
| 模型 | `SetPagedSource(loader, total)` + 重写 `canFetchMore/fetchMore`，每页 60 条，滚动到底由 `QAbstractItemView` 自动触发 |
| 数据源 | `DemoData::Page(offset, limit)` 按需生成（不预先造几千条），`--paged --items 2000` 启用 |
| 实测 | 初始 `homeRows=60`、`painted=40`；`--scroll-end 3` 后 `homeRows=240`、`painted=70` —— 每次只多渲染新区可见项 |
| 用途 | 接真实搜索/分类接口时，把 loader 换成 `JmApi::Search(page)` 即可，页面代码不变 |

### 7.4 键盘与触屏

- **键盘**：`QListView` 用 `StrongFocus`，Tab 可进入、方向键移动、Enter 打开详情；delegate 在 `State_HasFocus` 时画强调色边框，避免"焦点在哪看不见"。主窗口 `Esc` 返回上一页。
- **长按**：`ComicListView` 按下 520 ms 未移动 8 px 即弹菜单（查看详情 / 加入收藏 / 下载），右键同样触发；移动超过阈值立即取消，不会和滚动打架。
- **触摸滚动**：仅当系统报告存在 `TouchScreen` 设备时才 `QScroller::grabGesture`，桌面鼠标拖拽不会变成"甩列表"。

### 7.5 仍不建议做的事

- **不要**在没有产品定语言的情况下半途接 i18n：14 个页面的字符串是散在构造函数里的，改一半会比不改更难维护。
- **不要**为了动画而动画：Python 的换页覆盖层是"整页位移 + 接缝对齐"的特定方案，直接照搬到 QWidget 栈上会得到两套观感；要么完整移植 QRhi 覆盖层，要么不做。


---

## 8. 第四轮：换页动画改 QML + Android 触摸适配层（2026-11-03 晚）

### 8.1 换页动画：QWidget/QRhi → QML 场景图

| 文件 | 职责 |
|---|---|
| `qml/PageSwipe.qml`（117 行） | 场景与时间轴：两个 `Image` + `offset` 只读属性 + `NumberAnimation`（250 ms / Linear）；对外 `start()/stop()/freeze(p)` 与 `finished()` |
| `page_transition.h/.cpp`（约 300 行） | `QQuickWidget` 宿主、`PageImageProvider`（`image://pagecache/<id>`）、首帧判定、看门狗与降级 |
| `ui_main_window.cpp` | 快照抓取（`CapturePage`）、切页时机（`SwitchTo/ApplyPage/LandPending`）、失败回退 |

**时序**（与 Python 版 `AnimationStackWidget` 一一对应，每一步都有原因）：

1. `SwitchTo()` 抓两张快照：`prev` 必抓、`next` 若未布局先 `resize(stack size)` + `layout()->activate()`（否则拍出来是"另一个布局的左上角"）；
2. `PageTransition::Begin()` 把两张图塞进 image provider，设置 `prevSource/nextSource/direction/dpr`，`view_->show()` 后调 QML `start()`；
3. **首帧到达前不动页面栈**：旧页继续显示在覆盖层下面。首帧（`frameSwapped`，或 120 ms 看门狗的 `grabFramebuffer()`）到达后 `FirstFrame` → 隐藏旧页（否则旧页每帧都要参与合成）；
4. 动画播完 → `Finished` → `LandPending()`：此刻才 `setCurrentWidget(target)` 并 `OnEnter()`（阅读页首次进入实测 200 ms+，正好藏在覆盖层底下）；
5. 再等 48 ms 让新页真的画出一帧，然后 `End()` 撤覆盖层。

**三条不变量**（都是 Python 版踩过的坑）：

- `off` 必须是**设备像素整数**，且两页共用同一个值 —— QML 里用 `Math.round(progress*width*dpr)/dpr` 实现；
- 逐帧不重传纹理，只改 `x`；`cache: false` + 每次新 id 防止 QML 内部缓存无限增长；
- 不能提前撤覆盖层，也不能在新页画出来之前撤。

**验收（可复现的数字，不是"看着挺好"）**：

| 检查 | 方法 | 结果 |
|---|---|---|
| 接缝无重叠无空隙 | 把 `progress=0.5` 的截图逐列与 `progress=0`（纯 prev）、`progress=1`（纯 next）比较 | **1194 列属 prev、1193 列属 next、0 列"两者都像"、0 列"都不像"**；首个 next 列 = 1366，与理论接缝 `172 + 1194` 完全一致 |
| 终点与目标页一致 | `--anim-progress 1` 的截图 vs 普通 `--page settings` 截图 | **md5 完全相同**（`db939155…`） |
| 真实播放能收尾 | `--anim-ms` 用默认 250 连续播放后截图 | 播放结束的截图与普通页面 **md5 完全相同**，无残留 |
| 后退方向 | `--anim-from settings --page home --anim-back --anim-progress 0.4` | 旧页右移、新页从左侧进入，接缝同样平直 |
| 耗时 | 内置计时日志 | 快照抓取 21~46 ms（冷/热）、动画 250 ms、从开始到落位 297~306 ms |

**失败降级**（不重复 Python 的 raster 兜底，理由见 `page_transition.h` 头注释）：

1. QML 场景加载失败（缺 Qt Quick 插件）→ `IsReady()` 为 false，调用方直接切页；
2. 120 ms 没出帧 → 先 `grabFramebuffer()` 强制渲染一次；仍失败则本次放弃，连续 2 次后本进程不再尝试；
3. 动画期间窗口尺寸变化超过 8 px（软键盘/旋转/分屏）→ 立刻 `LandPending()` 落位，避免半途换几何。

### 8.2 触摸适配层

`touch.h/.cpp`（约 420 行）+ `MainWindow` 接线。桌面上用 `--touch` 打开，**走的是和 Android 完全相同的代码路径**（触摸模式下鼠标事件同样进入手势状态机），所以能在没有设备的情况下验收。

| 能力 | 实现 | 触发条件 |
|---|---|---|
| 边缘右滑返回 | 应用级事件过滤器：左边缘 24dp 内按下 → 横向位移 > 72dp → 松手返回 | 仅触摸模式；竖向滑动不接管（`|dx| > |dy|` 且 `dx > 0`） |
| 手势过程提示 | `EdgeHint`：左边缘竖条 + 滑入的圆片箭头，透明度随进度 | 同上 |
| 返回键 | `MainWindow::keyPressEvent` 处理 `Qt::Key_Back`（Qt 6 不再默认 `finish()`） | Android |
| 手势滚动 | `QScroller::grabGesture`，递归装到所有 `QAbstractScrollArea`；有触摸屏用 `TouchGesture`，否则 `LeftMouseButtonGesture` | 触摸模式 |
| 44dp 命中目标 | `PolishHitTargets`：把小于 44dp 的**固定尺寸**按钮放大（不碰布局驱动的） | 触摸模式（本次实测修了 29 个） |
| 输入法避让 | `FocusIn` 时延时 320 ms，把焦点控件 `ensureWidgetVisible` 到最近的 `QScrollArea` | 触摸模式 + 文本输入 |
| 安全区 | `SafeArea()`：`QScreen::geometry` 与 `availableGeometry` 差值，只取左右/顶部；**底部手势条恒为 0**；`--safe-insets`/`JM_SAFE_INSETS` 可覆盖 | Android / 显式指定 |
| 阅读器点按分区 | 3×2 网格，与 Python `mobile_ui_reader._Tap` 同语义：上排左右=显隐工具条、中间=滚半屏；下排左右=上/下一页（放大时改滚动）、中间=滚半屏 | 阅读页 |

**验收**（合成真实事件，走 `QCoreApplication::notify` → 应用级过滤器，等于用户真的在拖）：

| 用例 | 命令 | 结果 |
|---|---|---|
| 手势返回 | `--page settings --demo-drag 120` | `page=0`（回到首页），日志 `dir=backward`，落位 306 ms |
| **反例**：不足阈值 | `--page settings --demo-drag 40` | `page=11`（仍在设置页），无动画——证明阈值真的生效而不是"碰一下就返回" |
| 点按下一屏 | `--page reader --demo-reader-tap 0.85,0.80` | `zone=right-bottom page=2/12` |
| 点按上一屏 | `--page reader --demo-reader-tap 0.10,0.80` | `zone=left-bottom page=1/12`（首页再上一页是空操作） |
| 点按显隐工具条 | `--page reader --demo-reader-tap 0.10,0.20` | `zone=left-top bar=0` |
| 安全区 | `--page home --touch --safe-insets 0,48,0,0` | 日志 `safeArea applied = 0,48,0,0`，截图中标题栏整体下移 48 px |
| 命中目标 | 任意 `--touch` 启动 | 日志 `hitTargets<=44dp fixed=29` |

### 8.3 本轮踩到的坑

1. **`Touch::Dp()` 第一版乘了 `devicePixelRatio`** —— Qt 的逻辑坐标系本来就与 DPR 无关（Android 上 1 逻辑像素 ≈ 1dp）。乘完之后 200% 缩放的桌面上 44dp 变成 88dp、72dp 的滑动阈值变成 144dp，表现为"手势怎么拖都不触发"和"按钮突然巨大"。改成恒等映射并写进注释。
2. **`QQuickWidget` 没有 `frameSwapped` 信号**（它继承 `QWidget` 而非 `QQuickWindow`）；`quickWindow()` 是 Qt 6 才有的。所以首帧判定做了三条路：`quickWindow()->frameSwapped`、`QEvent::Paint`（延后一拍再报）、看门狗里的 `grabFramebuffer()`。
3. **`QEvent::Paint` 过滤器会在 `paintEvent` 执行前触发** —— 直接在那里发 `FirstFrame` 会让调用方提前藏旧页（闪一帧背景）。必须 `QTimer::singleShot(0, ...)` 排到本轮事件之后再报。
4. **`QInputDevice::devices()` 在 Qt 6.12 返回 `QList<const QInputDevice*>`**，写成非 const 会编译失败。
5. **`pages_` 是 `QVector<PageBase*>`**，`QWidget *page = pages_.at(i)` 会丢掉 `OnEnter()`（编译期就报"QWidget has no member OnEnter"）——算好事，类型帮我们抓到了。

### 8.4 复现命令

```bat
rem 动画：冻结在 50%（确定性截图）
build\jmcomic_ui.exe --page settings --anim-from home --anim-progress 0.5 --shot anim50.png

rem 动画：真实播放（慢放到 1.2s，便于截中途帧）
build\jmcomic_ui.exe --page settings --anim-from home --anim-ms 1200 --shot-delay 500 --shot live.png

rem 动画：后退方向
build\jmcomic_ui.exe --page home --anim-from settings --anim-back --anim-progress 0.4 --shot back.png

rem 触摸：边缘返回（会真的翻页）+ 反例
build\jmcomic_ui.exe --page settings --demo-drag 120 --shot-delay 1200 --shot back.png
build\jmcomic_ui.exe --page settings --demo-drag 40  --shot neg.png

rem 触摸：阅读器点按三分区
build\jmcomic_ui.exe --page reader --demo-reader-tap 0.85,0.80 --shot tap.png

rem 触摸：安全区与命中目标
build\jmcomic_ui.exe --page home --touch --safe-insets 0,48,0,0 --shot safearea.png
```


---

## 9. 直角矩形风格改造（2026-11-03 晚）

### 9.1 目标与边界

- 全站圆角归零，去掉胶囊（chip / 搜索框 / Toast）、圆形气泡（边缘手势提示）、圆形头像裁剪；
- **布局、配色、功能逻辑一律不动**：只改"角"，不改尺寸、间距、层级与交互；
- 保留真正的圆：放大镜、时钟、太阳、心形曲线这些**图标本身的圆**不是"圆角造型"，改成方形会让图标语义变形；
- 描边端头仍用 `RoundCap`/`RoundJoin`：那是线条端头，不是容器造型，改方头会让 1.5px 细线图标在小尺寸下发虚。

### 9.2 唯一来源（改一个值就能整体切回圆角）

| 层 | 位置 | 说明 |
|---|---|---|
| C++ 自绘 | `theme.h` → `UiStyle::Radius = 0` + `UiStyle::DrawPanel/AddPanel` | 用 `if constexpr`：`Radius = 0` 时直接 `drawRect/addRect`，不生成圆角调用 |
| C++ QSS 覆盖层 | `theme.cpp` → `@radius` token | `css.replace("@radius", UiStyle::Radius + "px")`，17 条规则共用 |
| 基座 QSS | `res/theme/*.qss` | Python 版共用、**不支持 token**，所以写死 `0px`；文件头加了说明注释 |

恢复圆角需要改两处：`UiStyle::Radius` 与 6 个 qss 文件——这一点写进了 `theme.h` 的注释，避免下次只改一半。

### 9.3 改动清单

| 位置 | 改了什么 | 数量 |
|---|---|---|
| `res/theme/*.qss`（6 个） | 全部 `border-radius` → `0px`（含 `QSlider` 的 20px 胶囊、`QPushButton` 的 4px 等） | **164 条** |
| `theme.cpp` 覆盖层 | 全部圆角改为 `@radius` token：`#Primary`/`#Ghost`/`#Chip`/`#SearchBox`/`#Input`/`#ListRow`/`#SettingCombo`/`#RowProgress`/`QScrollBar::handle`/`#Toast`/`#Tag`/`#DialogCard`/`#DangerBtn`/`#WinBtn`/`#NavItem`/`#NavBadge` | **17 条** |
| `ui_kit.cpp` | 开关轨道（胶囊→直角）、滑块（圆→方）、卡片 hover 底、封面裁剪路径、封面描边、封面角标 | 6 处 |
| `ui_kit.cpp` 图标 | `Grid` 四宫格 2px、`Comment` 气泡 3px、`Pause` 两根 1.4px —— 图标里的圆角矩形也归零 | 7 个矩形 |
| `comic_view.cpp` | delegate 焦点框（键盘导航的高亮描边） | 1 处 |
| `touch.cpp` | 边缘返回提示：圆形气泡 → 方形提示块 | 1 处 |
| `ui_pages_browse.cpp` / `ui_pages_library.cpp` | `Rounded*()` 图片裁剪：`UiStyle::Radius <= 0` 时直接返回原图（省一次全尺寸拷贝） | 2 个 helper |
| **C++ 内联 `setStyleSheet`**（第二轮审计补漏） | 登录页圆形头像 28px、评论页圆角头像 18px、超分预览面板 10px、关闭按钮 6px —— 全部改为 `%px` 取 `UiStyle::Radius` | 4 处 |
| Python `.ui` + `ui_*.py` | 生成源码与源文件同步改（避免下次 uic 重新生成又变回圆角） | 10 处 / 4 个文件 |
| Python 视图内联样式 | 卡片、代理行、登录框、设置项等内联 `border-radius` | 32 处 / 12 个文件 |
| Python 自绘 | `switch_button`（胶囊→直角 + 方块滑块）、`head_label.radius`（100→0，头像/封面不再裁圆）、`msg_label`（气泡圆角→直角）、`frame_less_widget`（窗口本体 10px→0） | 4 个文件 |

> Python 侧共 **42 处样式 + 4 个自绘控件**，17 个改动文件全部通过 `py_compile`（报的 `SyntaxWarning` 是改动前就有的旧问题）。

### 9.4 验收

| 检查 | 方法 | 结果 |
|---|---|---|
| 全站无残留圆角 | `grep -rn -e drawRoundedRect -e addRoundedRect -e drawEllipse cpp/shell/ui/*.cpp`；再 `grep -rn border-radius` 查内联样式 | 绘制侧只剩两个 `Rounded*` helper 里的死路径（`Radius > 0` 才走）；内联样式全部改成取 `UiStyle::Radius`（第二轮才扫出来的 4 处） |
| 6 套主题都能解析 | 逐主题跑 `--page settings --theme X --shot` | `dark/light × teal/pink/orange` 全部出图（`docs/shots/theme_*.png`） |
| 布局未受影响 | 14 页 + 弹窗 + 浅色主题重拍 | 无错位、无贴边、无边框缺失（`docs/shots/*.png`） |
| 换页动画未受影响 | `--anim-progress 1` 截图 vs 普通页面截图 | **md5 仍然完全相同**（`7733cdbf…`）——风格改造没有破坏动画落位 |
| Python 未写坏 | 17 个改动文件 `py_compile` | 全部通过 |

### 9.5 取舍说明

1. **`QSlider` 的 groove/handle 也一起归零**：Python 的亮度/缩放用到 `WheelSlider`，改后手柄是方块。这是"一并更新以保证一致"的必然结果，不是遗漏。
2. **`#NavBadge`（导航角标）从 7px 改 0**：现在的角标是小的方形色块，与整体一致；若将来觉得太硬，只改这一条即可。
3. **没有引入"半圆角"过渡值**：需求是直角风格，保留 2~4px 的"微圆角"会让整套 UI 看起来像没改干净。


---

## 10. 强调色对齐 Python 版（2026-11-03 晚）

### 10.1 Python 的"黄色"是哪个值

Python 版没有把强调色散在代码里，而是写在 `res/theme/*.qss`：**同一个 hex 在一个文件里出现 24 次**，就是按钮 `:checked/:pressed`、`QProgressBar::chunk`、列表选中行用的主色。

| 主题（qss 文件名） | 强调色 | 说明 |
|---|---|---|
| `dark_orange` / `light_orange` | **`#FF7B00`** | **Python 的默认档**：`Setting.ThemeColorIndex` 默认 0 → `QtOwner.GetThemeColor()` 返回它，设置里叫"橙色" |
| `dark_pink` / `light_pink` | `#FF4081` | 第二档 |
| `dark_teal` / `light_teal` | `#1DE9B6`（浅色 `#00897B`） | C++ 侧额外保留的一套 |

C++ 之前写的 `#ffab40`（orange）与 `#ea4c89`（pink）**和 qss 里的值并不一致**，注释却写着"与 Python 版主题一致"——属于事实错误。这次逐个改回 qss 里的真值。

### 10.2 改动清单

| 位置 | 之前 | 现在 |
|---|---|---|
| `theme.cpp` orange 家族 | `#FFAB40` | **`#FF7B00`**（Python 默认色） |
| `theme.cpp` pink 家族 | `#EA4C89` | `#FF4081` |
| 默认主题（`theme.h` `name_` / `--theme` 默认值 / 设置页选中项） | `dark_teal` | **`dark_orange`** |
| `t_.danger` | `#E05B5B`（自定义的软红） | `#DC3545`（Python 的 `.danger`） |
| `#DangerBtn:hover` | `#C94B4B` | `#B02A37`（跟随新 danger） |
| `UiTheme::ToggleDark()` | 硬写 `light_teal`/`dark_teal` | 保留强调色家族，只翻明暗 |
| 设置页主题下拉 | 4 项、固定选中第 0 项 | 6 项（明暗 × 三色）、按当前主题选中 |

### 10.3 顺带修掉的 bug

`ToggleDark()` 原来硬写 `light_teal` / `dark_teal`：在橙色主题下点标题栏的日月图标，会莫名其妙跳回青色。默认色换成橙色后这个 bug 会天天被看到，所以必须一起修——顺带让设置页的下拉不再永远显示"深色 · 青色"。

### 10.4 验收

| 检查 | 方法 | 结果 |
|---|---|---|
| 主色就是 `#FF7B00` | 首页主按钮 / 进度条 / 选中胶囊逐像素采样 | 落点均为主色（截图 `home.png` / `download.png`） |
| 危险色就是 `#DC3545` | 确认框按钮区域 140×80 像素直方图 | 主色 `(220,53,69)` 出现 **9,495** 次 |
| 6 套主题都能渲染 | 逐主题 `--page settings --theme X --shot` | 全部出图，下拉均显示对应名称 |
| 明暗切换不串色 | 代码审查 + 手动切换 | 橙色主题切明暗仍是橙色 |
| 换页动画未受影响 | `--anim-progress 1` vs 普通页面 | **md5 仍完全相同**（`99371a47…`） |


---

## 11. 字体改为系统显示字体（2026-11-03 晚）

### 11.1 之前为什么"看起来不像系统字体"

基础主题 `res/theme/*.qss` 第一段就是：

```css
*{ color: #ffffff; font-family: Roboto; ... }
```

`*` 选择器会把**应用字体整个顶掉**。而 Windows 上根本没装 Roboto，Qt 不会报错，只会静默挑一个替代字体——实测落到 **Tahoma**（诊断日志：`system=Roboto 9.00pt -> resolved=Tahoma 9.00pt`）。所以上一版虽然有 `PickFont()` 的优先级表（YaHei UI → PingFang → Noto…），实际渲染的却是 Tahoma，`PickFont()` 等于没生效。

### 11.2 做法

| 层 | 改动 |
|---|---|
| 字体来源 | `PickFont()`（手写优先级表）删除，换成 `QFontDatabase::systemFont(QFontDatabase::GeneralFont)`：Windows 取 `NONCLIENTMETRICS.lfMessageFont`（中文系统 = Microsoft YaHei UI，英文 = Segoe UI），Android 取 Roboto/Noto，macOS 取 `.AppleSystemUIFont` |
| 应用字体 | `app->setFont(系统字体)` + `PreferFullHinting`（小字号更清晰），**不再硬写 9.75pt** |
| QSS 覆盖 | 覆盖层顶部加 `* { font-family: @font; }`，`@font` 由主题层注入带引号的家族名——用来压掉基础 qss 里的 Roboto |
| 诊断 | 启动打一行 `theme/font: system=... -> resolved=...`，以后"字体没生效"一眼能看出来 |
| 覆盖入口 | `JM_UI_FONT=<family>`，用于 A/B 对比与缺字体机器兜底 |

### 11.3 验收

| 检查 | 方法 | 结果 |
|---|---|---|
| 用的确实是系统字体 | 诊断日志 | `system=Microsoft YaHei UI 9.00pt -> resolved=Microsoft YaHei UI 9.00pt` |
| 改动真的生效 | `JM_UI_FONT=Roboto` 复现旧行为，与默认各出一张图比 PSNR | **24.86 dB**（差异显著，不是"看起来差不多"） |
| 旧行为是什么 | 同上，看日志 | `system=Roboto 9.00pt -> resolved=Tahoma 9.00pt` ← 之前渲染的是 Tahoma |
| 排版未破 | 14 页 + 弹窗 + 浅色 + 关于页长段落 | 无截断/挤压/溢出（`docs/shots/*.png`） |
| 换页动画未受影响 | `--anim-progress 1` vs 普通页面 | **md5 仍完全相同**（`f348112d…`） |

对比图：`docs/shots/font_system.png`（现在）与 `docs/shots/font_tahoma_fallback.png`（复现旧行为）。

### 11.4 两点说明

1. **字号跟着系统走（9.75pt → 9pt）**：这是"系统显示字体"的一部分，Windows 原生对话框就是 9pt YaHei UI。QSS 里绝大多数标签本来就是 px 固定值，只有少数继承默认字号，实测没有排版问题。
2. **Python 版未改**：它自己也有一份 `font-family: Roboto`，但有 `Setting.FontName` 这个自定义入口（`SetTheme` 里替换 `/*replace*/`）。要让 Python 也跟着系统走，把那份 qss 的 `font-family` 换成系统字体、或在设置里填字体名即可——属于另一个决定，不在本次范围。


---

## 12. 字体跟随系统 + 自定义字体（2026-11-03 夜）

> §11 是"静态版"（启动时读一次系统字体）。本节把它升级为**运行期跟随**，并加了自定义字体。

### 12.1 取字体与监听变更：分平台方案

| 平台 | 取当前系统字体 | 监听变更 |
|---|---|---|
| **Windows** | `SystemParametersInfoW(SPI_GETNONCLIENTMETRICS).lfMessageFont`：`lfFaceName` + `lfHeight`（负值=字符高度像素，按 `GetDeviceCaps(LOGPIXELSY)` 换算成 pt）+ `lfWeight` | `QAbstractNativeEventFilter` 抓 `WM_SETTINGCHANGE`（`wParam == SPI_SETNONCLIENTMETRICS`）、`WM_FONTCHANGE`、`WM_THEMECHANGED` |
| **Android** | `QFontDatabase::systemFont(GeneralFont)`（Roboto / Noto） | `QGuiApplication::fontDatabaseChanged` + 轮询 |
| **macOS / Linux** | 同上 | 轮询（未在真机验证） |

两个实现要点：

1. **不直接用 Qt 的 `QFontDatabase::systemFont()` 读 Windows 系统字体**。Qt 平台插件自己也要处理 `WM_SETTINGCHANGE` 才会刷新缓存，而我们的原生过滤器跑在它前面——立刻读会拿到旧值。直接问 Win32 才是"改完就能读到"。
2. **必须有轮询兜底**：原生消息会被别的窗口吞掉（或干脆不广播），所以再加一个 2 秒定时器比对 `family + pt`，两次 `SystemParametersInfo` 是微秒级开销。两条路都进同一个 250ms 防抖，避免一次系统变更触发一串重设。

### 12.2 回退链（三级）

| 级别 | 触发条件 | 行为 |
|---|---|---|
| 1 | 自定义字体不在 `QFontDatabase::families()` 里 | 退回系统字体，设置页显示"字体不可用，已回退系统字体" |
| 2 | 自定义字体缺中文字形 | `QFont::setFamilies({自定义, 系统})` —— Qt 逐字回退，不会出现豆腐块 |
| 3 | 系统字体也读不到（极端） | `QFontDatabase::systemFont(GeneralFont)` 的族 + 9pt |

### 12.3 刷新与"布局不乱"

- **字体本身**：`qApp->setFont()` → Qt 发 `ApplicationFontChange` → 所有控件重排，不需要手动遍历。
- **qss 里的 px**：基础 qss + 覆盖层里所有 `font-size: Npx` 由 `ScaleFontPx()` 按 9pt 基准等比缩放。不缩放的话会出现"正文变大了、标签没变"的割裂感。
- **自绘文字**：卡片标题/角标/作者、详情标题、关于页产品名都走 `UiStyle::Fpx()`；卡片的文字区高度（`+40`）也一起缩放，否则放大后标题会被裁。
- **定高容器**：导航栏宽 260、行高 40、图标 32 跟着缩放——9pt 时"分类与排行"刚好一行，字号一大就会折行（第一版实测）。
- **只有缩放比变了才重建 qss**：单纯换家族/字重/字宽时只 `setFont`，拖动滑块不会每一格都重刷整棵树（`AppFont` 里还有 40ms 合并）。
- **字号夹在 8~12pt（0.89~1.33 倍）**：这是实测出来的上界。16pt 时标题栏、卡片文字区、主推位简介会开始挤压/重叠——所以滑块与"跟随系统"都夹到 12pt。要放开就改 `app_font.cpp` 的 `kMaxPoint`，并逐页复核定高容器。

### 12.4 自定义字体（设置 → 字体）

| 控件 | 范围 | 说明 |
|---|---|---|
| 字体来源 | 跟随系统 / 自定义 | 跟随系统时下面四行变灰（`:disabled` 样式） |
| 字体 | 本机已装字体列表 | 缺中文时自动落到系统字体（回退链第 2 级） |
| 字重 | 100–900，步进 25 | 同时写 `QFont::setWeight` 与可变轴 `wght` |
| 字宽 | 50%–200%，步进 5 | 同时写 `setStretch` 与可变轴 `wdth` |
| 字号 | 8–12pt | 联动整套 qss 缩放 |
| 恢复默认 | — | 回到跟随系统 |

- **可变字体**：`QFont::setVariableAxis(QFont::Tag("wght"/"wdth"), v)`（Qt 6.7+ API，6.12 可用）。字体不是可变字体时 Qt 会忽略轴、退回 `setWeight/setStretch` 的就近匹配，所以两套都写，普通字体也不会炸。
- **持久化**：`AppConfigLocation/ui.ini` 的 `[font]` 段（source / family / weight / width / size），重启后仍在。
- **状态行显示"实际解析值"**：Segoe UI Variable 的 `wght` 轴只到 700，滑块拉到 800 时状态行写"字重 700（请求 800：该字体 wght 轴范围只到 700）"——比笼统写"可变轴生效"有用。

### 12.5 验收

| 用例 | 命令 | 结果 |
|---|---|---|
| 启动读系统字体 | 默认启动 | `source=system family=Microsoft YaHei UI -> resolved=Microsoft YaHei UI 9.0pt` |
| **运行期系统字体变更** | `--demo-sysfont "Segoe UI,11"`（只伪造系统给的答案，防抖/比对/重设/刷新走真实路径） | 日志两行：`9.0pt` → `11.0pt`；截图西文变 Segoe、导航栏同步变宽、无错位（`docs/shots/font_syschange.png`） |
| 自定义 + 可变轴 | `--font-family "Segoe UI Variable" --font-weight 800 --font-width 120 --font-size 11` | `resolved=Segoe UI Variable weight=700 size=11.0pt`，界面即时生效（`font_settings_custom.png`） |
| 字体不可用回退 | `--font-family NoSuchFontXYZ` | `resolved=Microsoft YaHei UI`，状态行提示"字体不可用，已回退"（`font_fallback.png`） |
| 字号极值 | `--font-size 8` / `--font-size 12` | 无裁切/重叠；12pt 时导航仍单行、卡片标题完整（`font_size8.png` / `font_size12.png`） |
| 字号上限的依据 | `--font-size 16`（改造过程中） | 主推位简介与标签重叠、导航折行 → 因此把上限压到 12pt |
| 持久化 | 自定义后重启（不带参数） | 仍是自定义设置；`--font-reset` 可恢复 |
| 换页动画未受影响 | `--anim-progress 1` vs 普通页面 | **md5 仍完全相同**（`0390f3fe…`） |

### 12.6 已知边界

1. **系统字体变更在真机上未验证**：本环境不能改系统字体，用的是"只替换操作系统答案"的注入口（`AppFont::setSystemFontProviderForTest`）。原生消息 → 防抖 → 重读 → 重设 这条链路跑的是真实代码，只有"Win32 是否真的广播消息"这一步没在真实场景里跑过。
2. **Android 上没有"系统字体变更"这回事**（改字体通常要重启应用），监听器在那里主要是兜底。
3. **字体列表不做搜索**：本机 200+ 字体时，组合框靠滚动选择；要加搜索得先给 ComboBox 做个可过滤的弹出层。

---

## 13. 菜单排版与首页分区对齐 Python（2026-11-03 深夜）

> 这一节修的是"结构"，不是"皮肤"：侧栏从"84px 图标栏"改成 Python 的"260px 文字列表"，
> 首页从"页头 + 主推位 + 筛选胶囊 + 网格"改成 Python 的"分区标签页 + 网格 + 页码条"。

### 13.1 Python 侧的真实结构（照抄对象）

| 位置 | Python 实现 | 结构 |
|---|---|---|
| 侧栏 | `ui/component/ui_navigation.ui` + `python/src/interface/ui_navigation.py` | `QVBoxLayout` → `topWidget`(用户区) + `scrollArea`(三段导航) + `tailWidget`(帮助/设置) |
| 导航项 | 同上 | `QToolButton` + `ToolButtonTextBesideIcon` + `iconSize 32` + `minimumSize 150×40`，`layout spacing 6`、`contentsMargins(0,9,0,9)` |
| 分段 | 同上 | `label 用户` → 5 项 → `HLine` → `label 导航` → 5 项 → `HLine` → `label 其他` → 5 项 |
| 面包屑 | `ui/ui_main.ui` 的 `menuLayout` | `menuButton` + 若干 `QToolButton`（页面标题）+ `QLabel(">")` 分隔 |
| 首页 | `ui/ui_index.ui` + `python/src/view/index/index_view.py` | 单个 `QTabWidget`，每个 `/promote` 分区一个 tab，tab 内一个 `ComicListWidget`；tab 0 底部有页码条（`categoryBox` / `weekBox` / `pages` / `spinBox` / `jumpButton`） |

### 13.2 改成了什么

**侧栏**（`ui_main_window.cpp: BuildNavRail`）

- 宽度 86 → **260**；按钮从 `TextUnderIcon`(70×54) → **`TextBesideIcon`(≥150×40，图标 32)**；
- 顺序与文案逐字对齐（`navigation_widget.retranslateUi`）：
  - 用户：我的收藏 / 我的评论 / 观看记录 / 本地收藏 / 本地记录
  - 导航：首页 / 搜索 / 分类与排行 / 每周必看 / 评论
  - 其他：下载 / 网络存储 / 本地漫画 / 图片超分 / 批量超分
  - 底部（tailWidget）：帮助 / 设置
- 顶部补 Python 的 `topWidget`：88px 方形头像（Python 是 100px 位图，这里用主色方块 + "JM"）、登录/打卡按钮、用户名+称号、`J Coins / 等级 / 收藏数 / 分流×2 / 屏蔽词` 六行键值；
- 选中态 = `@surfaceHover` 底 + **左侧 4px 主色边框**（对应 qss 的 `QToolButton:checked{border-left:5px solid #FF7B00}`）；
- 三段之间补 `HLine`，分组标题 11px `@textFaint`，左内边距 13px（与 Python 的 label 位置一致）。

**面包屑**（`BuildCrumbBar` / `RebuildCrumbs`）

- 新增 44px 高的条，左侧 `menuButton`（三横线，点击收起/展开侧栏，85ms `InOutQuad`），右侧按 `history_` 生成 `首页 > 作品详情 > 阅读`，分隔符 `>`，当前级主色；
- 点击任意一级 = 截断返回栈并切回去（等价于连按返回）；
- 页面标题统一改成导航文案（`分类与排行` / `本地记录` / `我的收藏` / `本地漫画` / `图片超分`），避免"面包屑写 A、页头写 B"。

**首页**（`ui_pages_browse.cpp: HomePage`）

- 删除页头（"发现 / 今日更新…"）、主推位（hero）、筛选胶囊——这三个都是 C++ 首版自创的，Python 首页没有；
- 改成 **`QTabWidget` + 每分区一个网格**，标题与顺序照抄线上 `/promote` 的返回（tab 0 是 `.ui` 里写死的「最新上传」）：
  `最新上传 / 连载更新 / →右滑看更多→ / C108推荐本本 / 禁漫去码全彩化 / 禁漫汉化组 / 韩漫更新 / 其他更新`；
  三个分区类型（`HomeSectionKind`）：`Latest`（/latest 分页）、`Weekly`（连载更新，支持 类别×星期 重新请求）、`Promo`（推荐区块，一次性给全）；
  Python 里 `filterTypes = ['novels','library']` 会跳过的两类区块，C++ 侧留了同一开关位；
- 底部页码条按 `ui_index.ui` 的顺序：`类别下拉 → 星期下拉 → 页码文字 → 数字框 → 跳转`；
- `SwitchCheck` 的规则照搬：第 0 个分区只显示页码，第 1 个分区（连载更新）多显示 `全部/日漫/韩漫` + `周一~周日` 两个下拉，第 3 个起整条隐藏；
- 两个下拉改值即重新请求该区（对应 Python 的 `LoadSerializationInfo`），实测三组筛选的条目与内容都变了（stderr `home: weekly cat=… day=… items=6`）。

**补齐的 7 个页面**（`ui_pages_extra.cpp`，此前导航里没有对应页面）

| 导航项 | 页面 | 内容 |
|---|---|---|
| 我的评论 | `MyCommentPage` | 评论行（作品/章节/正文/赞与回复/删除，删除走 `ModalDialog` 确认） |
| 观看记录 | `RemoteHistoryPage` | 云端记录列表 + 同步按钮 + 继续阅读 |
| 本地收藏 | `LocalFavoritePage` | 本地归档网格 + 重新扫描 |
| 每周必看 | `WeekPage` | 星期下拉 + 漫画/汉漫/其他三个 tab（对应 `ui_week.ui`） |
| 网络存储 | `NasPage` | SMB/WebDAV 节点列表 + 状态徽标 + 连接 |
| 批量超分 | `BatchSrPage` | 目录选择 + 模型/格式 + 进度条 + 开始 |
| 帮助 | `HelpPage` | 四条常见问题 + 版本号 |

### 13.3 验收

| 检查 | 方法 | 结果 |
|---|---|---|
| 菜单顺序/分段/缩进 | 截图 `home.png` / `nav_bottom.png` | 三段标题、15 个导航项 + 2 个 tail 项齐备；图标在左、文字左对齐、行距 38px |
| 侧栏可滚动 | `--demo-nav-end` | 滚到底能看到「其他」段与「批量超分」，tail 固定底部（滚动条 8px，`@textFaint` 手柄） |
| 面包屑层级 | `--demo-crumb`（首页→详情→阅读） | 显示 `首页 > 作品详情 > 阅读`，末级主色；侧栏仍高亮入口「首页」 |
| 侧栏收起 | `--demo-collapse` | 侧栏宽度 260→0、内容区占满、菜单按钮保留 |
| 首页分区 | `--home-section 1` | 「连载更新」选中时出现 `全部/日漫/韩漫` + `周一~周日` 两个下拉，页码条仍在 |
| 连载筛选 | `--home-filter 2,3` | 下拉显示 韩漫/周四，`items=6` 且内容随筛选变化（`home_weekly_hanman.png`） |
| 分页未回归 | `--paged --items 2000` | 首屏 `homeRows=60`、`painted=75` |
| 换页动画未回归 | `--anim-progress 1` vs 直接打开设置页 | **内容区（去掉面包屑）md5 相同**，整窗 PSNR 34.5dB 的差异只来自多出的「首页 >」两字 |

### 13.4 与 Python 仍存在的差异（其中 1~4、6 已在 §14 收敛）

1. **页码条位置**：Python 把它放在 tab 0 的布局里，切到第 2 个分区时整条页码跟着 tab 一起消失（`SwitchCheck` 里还在 `setVisible`，自相矛盾）。C++ 把条移到 tab 下方，规则改为"第 0/1 分区显示、其余隐藏"，既保留语义又保证可用。
2. **头像**：Python 用 `placeholder_avatar.png` 位图，C++ 是 `accentSoft` 方块 + "JM"（直角风格，无圆角裁剪）。
3. **用户区数值**：Python 登录后填 `J Coins / 等级 / 收藏数 / 分流 / 屏蔽词`；C++ 目前是未登录态占位（`0 / Lv.0 / 0 / 自动 / 已选 0 个`），接 `JmApi` 用户信息后替换。
4. **顶部分流/屏蔽词不可点**：Python 是 `QCommandLinkButton`（点击打开代理/屏蔽词面板），C++ 目前只显示值。
5. **分区数据是演示值**：标题已经对齐线上 `/promote` 的真实返回，但每个区块的作品仍是 `DemoData` 生成的；接网络后用 `SetHomeSections()` 覆盖（标题 + bookList + kind 一起给）。
6. **没有 `QTabBar` 的滚动按钮**：Python 分区多到溢出时会出现左右箭头（基座 qss 里有 `QTabBar QToolButton` 规则），C++ 目前分区固定 4 个，未验证溢出场景。
7. **侧栏不自动滚动到选中项**：点「批量超分」时该项在可视区外，需要手动滚一下——Python 同样如此（它没有 `ensureWidgetVisible`），保持一致。

---

## 14. 收敛 §13.4 的遗留差异（2026-11-04）

### 14.1 逐项结论

| # | 差异 | 处理 | 证据 |
|---|---|---|---|
| 1 | 页码条位置 | **移回 tab 内部**：`最新上传` 与 `连载更新` 各自的 tab 里各建一条（`HomePager`），`SyncPagers()` 统一页码/下拉/文本。位置与 Python 的 `widget` 完全一致，同时让 `SwitchCheck` 里"weekIndex 也 setVisible"的意图真正生效 | `home.png` / `home_weekly_hanman.png`：页码条紧贴网格下方，在标签页内容区之内 |
| 2 | 头像 | 换成 **Python 同一张资源** `res/icon/placeholder_avatar.png`（qrc 别名 `:/ui/assets/placeholder_avatar.png`），尺寸回到 100×100；资源缺失时才回退到方块 + "JM" | `home.png` / `nav_logged_in.png`：插画头像 |
| 3 | 用户区数值 | 新增 `DemoData::UserState`（姓名/称号/coins/等级/收藏数/分流×2/屏蔽词）；登录页点"登录"→ `SetLoggedIn(true)` → 侧栏刷新。未登录显示 `—`（Python 是空串，空值会被误读成加载失败） | `nav_logged_in.png`：南野 / Lv.7 · 收藏家 / 1280 / 42 |
| 4 | 分流/屏蔽词不可点 | 两个"分流"值装了事件过滤器：点击弹 `QMenu`（自动/直连/节点1/节点2）；"屏蔽词"点击弹 `ModalDialog::Prompt`（新增：带单行输入框的模态框），确认后过滤空值/去重并更新计数 | `nav_hidewords.png`：输入框预填"猎奇,重口"，侧栏同步"已选 2 个" |
| 5 | 分区数据是演示值 | **未完成**：仍需接 `/promote` + `/latest`（见 §14.3） | — |
| 6 | QTabBar 溢出无滚动按钮 | `setUsesScrollButtons(true)` + `ElideNone`，并补 `QTabBar QToolButton::left/right-arrow` 样式（箭头是运行时画的 PNG，基座 qss 指向的 svg 未打包） | `home_narrow.png`（980×660）：标签栏两端出现 ◀ ▶，末尾分区可点到 |
| 7 | 侧栏不自动滚到选中项 | 主动补上：`UpdateNavState()` 里对选中项 `ensureWidgetVisible`（下一个事件循环执行，避免布局未完成）。**这是有意优于 Python 的一处** | `nav_autoscroll.png`：进入"批量超分"后侧栏已滚到该项并高亮 |

### 14.2 过程中修掉的三个真问题

1. **`qt_add_resources(... FILES assets.qrc)` 不会编译 .qrc**：它把 `.qrc` 当成普通文件塞进资源，二进制里能直接看到 qrc 的 XML 原文，里面的条目一条都没注册 —— 表现是 `QPixmap(":/ui/assets/...")` 永远为空、头像回退成文字。改成 `PREFIX + BASE + FILES <png>` 后正常。
2. **`BuildPager()` 只造控件、没 `addWidget`**：页码条停在 (0,0) 被网格盖住，只在左上角露出"第 1 …"一角。修法是在 `BuildSection()` 里显式加入 layout。
3. **`setViewportMargins` 是 protected**：侧栏滚动区想留 4px 呼吸位时编译不过，改为用带边距的容器包住 `QScrollArea`。

### 14.3 仍未完成的一项：真实数据

现状：分区标题已对齐线上 `/promote`，但 bookList 仍来自 `DemoData`。落地路径（`cpp/shell/jm_api.*` 已具备前置条件）：

- `JmApi` 已有 `Request(path, query, handler)`、`Unwrap()`（AES + ts）、`JItem()`，以及 `kTokenSecret`；
- 需要新增 `Promote(page)` / `Latest(page)` 两个方法 + `PromoteReady/LatestReady` 信号，解析 `/promote` 的 `[{title, type, content[]}]`；
- UI 侧把 `HomeSection{title, kind, comics}` 用 `JmApi` 结果填进 `DemoData::SetHomeSections()`，失败时保留当前演示数据；
- 未做的原因：本机沙箱无法访问 JM 接口，无法对解密与分页做真实回归；贸然合入等于交付未验证的网络代码。

### 14.4 验收命令

```bat
rem 页码条位置（最新上传 / 连载更新）
build\jmcomic_ui.exe --page home --shot home.png
build\jmcomic_ui.exe --page home --home-filter 2,3 --shot weekly.png

rem 头像与登录态
build\jmcomic_ui.exe --page home --demo-login --shot logged.png

rem 屏蔽词输入框
build\jmcomic_ui.exe --page home --demo-hidewords --shot-delay 700 --shot words.png

rem 标签栏溢出（980 宽出现左右箭头）
build\jmcomic_ui.exe --page home --win-size 980x660 --shot narrow.png

rem 侧栏自动滚到选中项
build\jmcomic_ui.exe --page batchsr --shot autoscroll.png

rem 分页未回归
build\jmcomic_ui.exe --page home --paged --items 2000 --shot out.png
```

---

## 15. 手机端抽屉式导航（2026-11-04）

### 15.1 对照 Python 的实现

对照物：`python/src/tools/mobile_ui_drawer.py` + `mobile_ui_base.py` + `mobile_ui_window.py`（返回键优先级）。

| 参数/行为 | Python | C++ |
|---|---|---|
| 启用条件 | `mobile_ui.IsEnabled()` = `platform_mobile.IsAndroid()` | Android 侧同；桌面用 `--touch`（模拟 Android）或 `--drawer` 强制 |
| 宽度 | `int(min(300, max(220, win.width()*0.82)))` | 同公式（`UpdateDrawerGeometry`） |
| 顶部 | 菜单栏下沿（`_TopOffset`：menuButton/label 的底边） | `crumbBar_->geometry().bottom()+1` |
| 遮罩 | 子控件 `rgba(0,0,0,110)`，点空白关闭 | 同（`#DrawerScrim`，事件过滤器关闭） |
| 动画 | 只动 `pos`（x: -width → 0），120ms，开 OutCubic / 关 InCubic | 同（`QPropertyAnimation(navRail_, "pos")`） |
| 关闭兜底 | `singleShot(260)` 强制 hide | 同 |
| 菜单按钮 | `CheckShowMenu` → 先 `ToggleDrawer`，桌面才走 aniShow/aniHide | 同一分支 |
| 换页 | 先 `CloseDrawer`，`DrawerAniMs+40` 后再切 | 先关，160ms 后落页；且这次换页**不播**覆盖层动画（覆盖层几何刚变，容易停在半截） |
| 返回键 | 关抽屉优先于页面回退 | 同（`keyPressEvent` 里判断 `drawerOpen_`） |
| 展开时滚动位置 | 不滚动（从"用户"分组开始） | 同：打开后把侧栏滚回 0（桌面侧栏才做 `ensureWidgetVisible`） |

### 15.2 落地细节

- 抽屉模式下把 `navRail_` 从 `bodyLayout_` 中摘出来、`setParent(this)`，成为浮层；退出抽屉再 `insertWidget(0)` 放回，宽度恢复 260。
- **不能对抽屉用 `setFixedWidth`**（Python 注释里也写了这条）：会把宽度锁死、并与几何/动画互相打架。抽屉宽度只由 `setGeometry` 决定。
- 抽屉模式把窗口最小尺寸降到 320×480，否则 `--win-size 420x860` 会被桌面下限 980 夹住；退出抽屉恢复 980×660。
- 手机竖屏没有"边缘右滑打开抽屉"——Python 也没有（边缘手势只用于返回），保持一致。

### 15.3 验收

| 用例 | 命令 | 结果 |
|---|---|---|
| 开关 / Esc / 点遮罩 | `--page home --win-size 420x860 --drawer-selftest` | `open=1 / esc_closed=1 / scrim_closed=1`，退出码 0 |
| 抽屉外观 | `--win-size 420x860 --demo-drawer --shot` | 宽 300（=min(300, max(220, 420×0.82))），顶部从 y=88 开始，右侧内容被压暗（像素 114→65） |
| 收起态 | `--win-size 420x860 --drawer --shot` | 无固定侧栏，内容占满 420，菜单按钮仍在 |
| 触摸模式自动接管 | `--page settings --touch --win-size 420x860 --drawer-selftest` | 退出码 0（`--touch` 现在等同 Android，默认抽屉） |
| 抽屉 + 换页 | `--page batchsr --win-size 420x860 --demo-drawer --shot` | 抽屉展开在批量超分页之上，无覆盖层残留/拼接 |

## 16. 六类主操作按钮改"近似直角"（2026-11-04）

### 16.1 范围

全站仍然保持 §9 的直角风格（`UiStyle::Radius = 0`），只把这六类**主操作按钮**放软一点：

| 按钮 | 位置 | objectName | 改前 | 改后 |
|---|---|---|---|---|
| 登录 | 侧栏用户区 | `#Primary` | 0px | 3px |
| 打卡 | 侧栏用户区 | `#Ghost` | 0px | 3px |
| 登录 | 登录页卡片 | `#Primary` | 0px | 3px |
| 开始阅读 | 作品详情 | `#Primary` | 0px | 3px |
| 收藏 / 已收藏 | 作品详情 | `#Ghost` | 0px | 3px |
| 确定 / 取消 | 所有模态框 | `#Primary`/`#DangerBtn` / `#Ghost` | 0px | 3px |

其余按钮（下载本章、返回、跳转、注册、忘记密码、代理与节点设置、恢复默认、各种行内动作）
保持 0px；输入框、卡片、列表行、Toast、进度条也都不动。

### 16.2 实现

- `theme.h`：新增 `UiStyle::RadiusSm = 3` 与 `UiStyle::MarkSoftButton(QWidget*)`；
  `Radius` 保持 0，自绘代码（`DrawPanel`/`AddPanel`）完全不受影响。
- `theme.cpp`：`@radiusSm` token 注入 `RadiusSm`px，QSS 里新增

  ```qss
  QPushButton#Primary[btnShape="soft"],
  QPushButton#Ghost[btnShape="soft"],
  QPushButton#DangerBtn[btnShape="soft"] { border-radius: @radiusSm; }
  ```

  规则**只声明 border-radius**：padding、配色、`:hover`、`:pressed`、`:disabled` 全部沿用原规则，
  所以尺寸与交互状态零变化。用 `id + 属性` 组合选择器（特异性高于纯 `#Primary`），才压得住上面
  那行 `border-radius: @radius`；反过来只写 `QPushButton[btnShape="soft"]` 是压不过 id 选择器的。
- 七个打点：`ui_main_window.cpp`（登录 / 打卡）、`ui_pages_misc.cpp`（登录页）、
  `ui_pages_browse.cpp`（开始阅读 / 收藏）、`ui_kit.cpp`（确认框的 ok / cancel）。
- **token 替换顺序**：`@radius` 是 `@radiusSm` 的前缀，必须先替换 `@radiusSm`，
  否则会拼出 `0pxSm`，Qt 静默丢弃整条声明（表现为"改了没生效"）。这一条已写成源码守卫。

### 16.3 两个取舍

1. **确认框成组**：`确定 / 取消` 是同一行的一组按钮，所以危险操作的同位按钮（`删除 / 清空 / 移除`）
   也跟随 3px —— 否则一个对话框里会出现"一个圆一个方"。若后续要求严格按文字区分，只需在
   `ModalDialog` 构造里按 `okText` 判断是否打标记。
2. **Python 版未同步**：Python 侧这几个按钮由 QtMaterial 自绘（不是 qss 的 border-radius），
   按文字逐个改需要动 Material 的绘制参数、且当前环境跑不起完整 GUI 做像素回归，这轮只在
   C++ 版落地（视觉基线仍以 C++ 版为准）。

### 16.4 验收

| 用例 | 命令 | 结果 |
|---|---|---|
| 侧栏 + 首页 | `--page home --shot btn_home.png` | 登录 / 打卡 左上角内缩 5→3→2→1→0（设备像素），逻辑半径 ≈3px；两按钮观感一致 |
| 详情页三连 | `--page detail --shot btn_detail.png` | 开始阅读、已收藏 有弧度；下载本章仍是直角（`docs/shots/btn_detail_zoom.png`） |
| 登录页 | `--page login --shot btn_login.png` | 登录按钮 3px；同卡片输入框仍是直角 |
| 浅色主题 | `--page detail --theme light_orange --shot btn_detail_light.png` | 半径与深色一致，浅底上边缘干净 |
| 确定 / 取消 | `--page home --demo-hidewords --shot-delay 800 --shot btn_prompt.png` | 两个按钮同为 3px，间距/尺寸不变 |
| 危险确认框 | `--page history --demo-dialog --shot btn_dialog.png` | 取消 / 清空 成组 3px（见 16.3-1） |
| 源码守卫 | `python cpp/tools/host_test_soft_buttons.py` | 29 项全过（半径来源、token 顺序、规则只改 radius、7 处打点、5 个同位反例） |

## 17. 桌面 / Android 双端构建与测试（2026-11-04）

完整报告：`docs/c1_ui_test_report_2026-11-04.md`（含环境、用例表、证据与未覆盖项）。

- **桌面**：`cpp/shell/ui/build_win.bat` → `build/jmcomic_ui.exe`；55 项自动化全过
  （21 页面 + 分页 + 11 交互 + 5 移动布局 + 主题/字体 + 性能 + 4 边界）。
- **Android**：新增 `cpp/shell/ui/build_android.ps1`（Qt android_x86_64 + NDK r30 + Gradle，
  `androiddeployqt` 出包 → zipalign/apksigner），产物 `cpp/dist/JMComic-ui-0.88-x86_64-debug.apk`
  （`org.jmcomic.ui`，minSdk 34 / target 34，仅 x86_64，随包 Qt + TLS 插件 + libssl/libcrypto）；
  AVD（Pixel_8a / API 36 / 1080×2400）20 项交互全过，logcat 无 FATAL/ANR。
- **上机才暴露、已修的问题**（详见报告 §6）：
  1. 手机启动仍是桌面布局（常驻侧栏 + 980 最小宽）→ `Touch::IsMobile()` 上机自动手机形态；
  2. 详情页横向溢出、收藏按钮被裁 → 手机上封面/信息上下堆叠；
  3. 首页单列 → 按视口两列（扣除滚动条宽度）；
  4. 阅读器首屏按 98px 视口适宽 → 布局完成补渲染；
  5. Android 一次 back 投递两次（间隔 322ms）→ 600ms 去重；首页放行给系统 finish
     （**不要**用 `QCoreApplication::quit()`，会 ANR）；
  6. 横屏 411dp 高 < 480 最小高 → 手机最小尺寸 320×320。
- 回归入口：`cpp/tools/c1_ui_desktop_suite.py`、`cpp/tools/c1_ui_android_smoke.py`、
  `cpp/tools/host_test_soft_buttons.py`；日志在 `docs/test_logs/`。

## 18. 未覆盖项收敛（2026-11-04 晚）

清单/优先级：`docs/c1_ui_remaining_work.md`；测试结果：`docs/c1_ui_test_report_2026-11-04.md` 第二轮。

- **图标**：`theme.cpp::RewriteLegacyIcons()` 把 QSS 里 6 类 `:/icon/theme/...` 箭头替换成运行时
  生成的 PNG（`EnsureChevron` 增加向上方向）；`qt.svg` 警告清零。
- **WebP**：UI 目标启用 C 语言、编译 `3rdparty/libwebp`（dec/dsp/utils）为 `jmwebp`；
  `CoverCache` 在 Qt 解码失败时走 `JmWebp::Decode`（HTTP + 本地文件）。
- **会话**：`session.h/.cpp`（QSettings INI，`JM_SESSION_FILE` 可覆盖）；登录页新增「退出登录」。
- **真实首页**：`JmApi::FetchPromote/FetchLatestPage` + `MainWindow::LoadRealData` + `--real-data`；
  实测 80 条 / 8 区块 → 7 个 tab，真实封面（`media/albums/<id>_3x4.jpg`）。
- **平板**：触摸模式 600dp 断点（抽屉 ↔ 常驻侧栏）；网格按视口 2~6 列；详情 ≥600dp 保持并排。
- **横屏阅读**：`ReaderView::RenderImage()` 宽>高时按高度适屏（`JM_READER_PROBE` 可打印 target/viewport）。
- **16 KB**：`cpp/tools/check_ui_apk_16kb.py`，当前 APK 46/46 个 .so 的 PT_LOAD 均 ≥0x4000。
- 未完成：真实分页/详情/登录落库（业务闭环）、QNN 超分（需 arm64 真机）、真机性能。

## 19. 业务闭环（2026-11-05）

- `PageLoader`（`ui/page_loader.*`）：/chapter → 并发下载 → WebP 解码 → `SegmentationNum/Descramble`
  还原 → JPG 落盘（章节缓存）；与 JmApi 分层：API 走单通道 NetClient，页图走独立 NAM 并发 6。
- `DetailPage::SetAlbum`：/album 回填（含章节，`Chapter::id` = photo id，阅读据此拉真实图）。
  **坑：SetAlbum 里重建章节前必须先删掉 chapterHost_ 的旧 layout，否则 `new QVBoxLayout` 静默失败，
  界面停留在 ShowComic 的演示章节（日志 80 章、界面"初见"）。**
- `CoverCache` 重写：内存 LRU + 磁盘（md5(resolved key)）+ 规格回退 jpg→webp→png；
  `ResolvedKey` = key(+URL)，`Loaded` 信号回传原始 key。
- 会话：`session.ini` 增 `session/avs`；`JmApi::RestoreSession` 启动恢复，之后请求自动带 Cookie。

## 20. 分流/节点与登录优先级（2026-11-05）

- `JmApi::EnqueueFront()`：登录插队，避免排在首页请求后 20~40s（真机"卡在登录中"的根因）。
- `ProxyConfig`：6 条线路（域名与 Python GlobalConfig 一致）+ 选中项/节点结果持久化到 `ui.ini`。
- `NodeTester`：线路 ping（/latest，3s，//4）→ 线路图片速度；节点 ping/封面（3s，//4）→
  排序 `0.8*delay + 0.2*download`；并发 4、失败换 CDN 重试。
- 差异：Qt 无 SNI 覆盖能力，节点只能"测+排序+记录"，无法像 curl --resolve 那样作用到真实流量。

## 21. 节点覆盖的实现（QSslSocket + SNI）与分流页（2026-11-06）

上一轮记录"Qt 下无法把节点作用到真实流量"，这个结论是错的：

- `QSslConfiguration` 确实没有 `setPeerVerifyName`，`QNetworkAccessManager` 也无法对
  单请求改 SNI；但 **`QSslSocket::connectToHostEncrypted(hostName, port, sslPeerName)`**
  的第三个参数就是 SNI + 证书校验名 —— TCP 连 `hostName`（传节点 IP），
  TLS 用 `sslPeerName`（传域名），与 `curl --resolve` 完全等价。探针实测 TLS1.3 + 200。
- `cpp/shell/node_http.*` 是据此写的 HTTP/1.1 客户端：GET/POST、三种响应定界、
  最多 3 次重定向、超时/maxBytes。每请求一个 socket，并发由调用方限制。
- `NetRequest` 新增 `connectIp`（连哪个 IP）与既有 `peerName`（SNI/证书名）；
  `NetClient` 在有 `connectIp` 时走 NodeHttp，否则仍走 QNAM。
- 收口必须按"响应体是否完整"判成功（`NodeHttp::BodySettled`）：只看响应头会把
  "慢节点收到 5KB 就超时"误判成 `200 OK`，之后在解密层失败且不触发重试。
- `NodeTester` 的节点探测因此回到 https（Python 的 curl --resolve 语义），
  旧的 "http + Host、301 视为可达" 降级路径已删除。

排序/选优（`ProxyConfig::SortResults/PickBest`）：

| 级别 | 规则 |
|---|---|
| 1 | 已选中项优先 |
| 2 | ping 失败沉底 |
| 3 | 图片失败再次之（Python 把两种失败并列；这里拆开，避免"分数 0 的失败项"排到最前） |
| 4 | `0.8*delay + 0.2*download` 升序 |

选优：已选节点 ping 正常 → 保持；ping 失败 → 换最优可用；全部 ping 失败 → 保持空，
不自动选一个不通的 IP（页面提示"没有可达节点"）。

分流 UI：`ProxyPage`（`PageId::Proxy`）取代原模态 `ProxyDialog`。原因是真机
V2463A 上对话框能显示但按钮点不动；页面化后走与其它页一致的导航/触摸事件链。
侧栏"分流"两行与设置页入口都进同一页。

测试隔离：Qt 在 Windows 的 `AppConfigLocation` 走 Known Folder API，改 `APPDATA`
环境变量无效，所以 `ui.ini` 需要用 `JM_UI_CONFIG` 指定目录才能真正隔离。

## 22. 线路/IP 分流整体对齐 Python（2026-11-07）

原型：`config/global_config.py`、`config/setting.py`、`server/req.py` + `server/server.py`、
`view/user/login_proxy_new_widget.py`、`ui/ui_login_new.ui`（tab_4 / speedGrid）。

### 线路表与默认行为

| 项 | Python | C++ (`ProxyConfig`) |
|---|---|---|
| 6 条线路 | `Url2List/PicUrlList[4]` + `CdnApiUrl/CdnImgUrl(5)` + `ProxyApiUrl/ProxyImgUrl(6)` | `Lines()` 同编号同域名 |
| UI 行顺序 | speedGrid：1,2,3,4,6(US反代),5(IP分流) | `DisplayOrder()` = `{1,2,3,4,6,5}` |
| 索引异常 | `GetApiUrl2(>=7)` 回落列表第 1 项；`<1` 无保护 | `LineAt/GetApiUrl2/GetImgUrl2` 对 `<1 / >=7` 都回落到线路 1 |
| 默认 | `ProxySelectIndex=ProxyImgSelectIndex=1`、`ProxyIpValue=""`、`IsHttpProxy=0` | `apiLine=imgLine=1`、`ip=""`、`mode=None` |
| 反代线路 6 | `proxyUrl` 改写成 `<jm2-api/img.jpacg.cc>/<原host>/...`，且 `ipList` 强制为空 | `ResolveRoute()` 同一改写，且直接返回不带 `connectIp` |

### 三类 IP 节点（配置 / 分类 / 顺序 / 匹配）

| 分类 | Python 来源 | C++ |
|---|---|---|
| ProxyIP分流N | `Setting.ProxyIpLastList`，空则内置 `ProxyIpList`(26) | `ProxyIpLastList()` / `ProxyIpList()` |
| 自定义分流N | `Setting.PreferCDNList`，`re.split(r'[、，；;,\s]\s*')` | `CustomIpList()` 同正则 |
| CDN分流N（优选） | `GlobalConfig.BestCfIpList`(24) | `BestCfIpList()` |

`AllNodes()` 与 `ShowAllItem` 一致：顺序 ProxyIP → 自定义 → 优选；前两类用 `allips`
去重，第三类**不去重也不写入 allips**；组内序号取 enumerate 原始序号（被去重项仍占位）；
`KindOf()` 按名字前缀判定（TagKind）；`MatchFilter("proxy")` = ProxyIP + 自定义，
`MatchFilter("cf")` = 优选；`LoadHistory` 按 IP 回填上次结果。

### 排序 / 选优 / 测速

- `SortResults(Smart)` = 选中优先 → 非失败 → `0.8*delay + 0.2*download`；
  `Latency/Download` 两种模式失败沉底、选中不置顶、未测速记 1e9；全部用 `std::stable_sort`。
- `PickBest()` 与 `CheckSelectBox` 一致：选中项还在列表里就保持，否则取排序第一。
- 线路测速：Python `SpeedTest` 跳过 IP分流(5)；C++ **补齐为 6 条全覆盖**，
  无节点时给确定性状态「未选节点」。ping = `<api>/latest?page=0`（3s，耗时//4），
  speed = `<img>/media/photos/292840/00002.webp`（字节/秒，按 bytes/kb/mb 格式化）；
  每线 2 次采样，取最小 ping / 最大吞吐，抵消单次抖动，口径仍可横向比较。
- 节点测速：先 `check.jpacg.cc/resolve?proxyip=proxyip[.地区].cmliussss.net`，
  失败换 `check2.jpacg.cc`，再失败清空列表回落内置 26；并发 8、ping 3s、
  图片 5s；全部回来后排序 + 自动选中 + 存 `LastIpResult`。
- 失败列显示 Python 的短状态：延迟「连接超时」、速度「下载失败」、IP分流无节点
  「未选节点」；完整错误只放 tooltip，避免 Qt 的英文长串撑破窄列。

### 请求路由优先级（唯一入口 `ResolveRoute`）

1. 线路 6 反代 → 改写 URL，**不做 IP 覆盖**；
2. `IsHttpProxy != 0`（无代理/HTTP/Socks5/系统）→ 关闭 IP 覆盖；
3. 线路 5 且选中 IP 合法、无代理、节点 ping 通过 → `connectIp` + `sniHost`（NodeHttp）；
4. 其余 → 域名直连；索引异常 → 线路 1。

`RecommendLine()` 结合测速结果与 IP 配置：只取 ping 成功线路（线路 5 额外要求 `ipReady`），
先比延迟、同延迟比吞吐、再同则保留当前线路；全部无效 → 默认线路 1。
`SetProxyRoute()`（JmApi）、`cover_cache.cpp`、`page_loader.cpp` 都改用 `RoutePlan`，
保证 API / 封面 / 章节页图三条链路遵循同一优先级。

### 页面（`ProxyPage`）

Python tab_4 的分区/文案/顺序全部落到 `ProxyPage`：分流设置（无代理/HTTP/Socks5/系统）、
线路测速 4 列网格（Api分流 / 延迟 / 图片分流 / 速度）+ 6 行两组独立单选、测速、
节点列表（全部 / ProxyIP / 优选 + 智能排序 / 按延迟 / 按下载）、自定义 IP / 地区码 / 应用、
节点测速、共 N 项 · 可用 M、当前生效路由提示。C++ 额外增加「自动选择」按钮
（按 `RecommendLine` 同时选接口与图床），不改变其它交互语义。

## 23. DOH / ECH / HTTP3（2026-11-07）

原型：`config/setting.py`（`DohAddress` / `EnableEch=1` / `IsOpenHTTP3=0`）、
`config/global_config.py`（`DohUrlList` 4 个、`EchDomain=cloudflare-ech.com`）、
`server/req.py`（`DnsOverHttpsReq`、`GetEchConfigReq`、`Http3EchFallback`、
`IsAdvancedTlsSupported`）、`view/user/login_new_view.py`（ECH 15min/1min 刷新）。

### 先核对 Qt 能力（结论：只能自己实现 DoH）

| 能力 | Qt 6.12 实际情况 |
|---|---|
| DoH | `QDnsLookup::Protocol` 只有 `Standard / DnsOverTls`，无 DoH；`QNetworkAccessManager` 无 DNS 接口 |
| HTTP/3 | `QNetworkRequest` 只有 `Http2Allowed/WasUsed/Direct/Cleartext`，无 `Http3Allowed`；`Qt6Network.dll` 无 nghttp3/ngtcp2/QUIC 符号 |
| ECH | `QSslConfiguration` 无 ECH 接口，二进制里也没有 ech_config / ECH_REJECTED 符号 |

因此 ECH 与 HTTP/3 的**传输层在 Qt 后端上无法开启**（与 Python 的 Android
`curl_cffi` 垫片 `HTTP3_ECH_SUPPORTED=False` 是同一类限制）。C++ 侧不放假开关：
配置、ECH 配置获取、能力探测、降级状态全部真实可用，页面明确写出「后端不支持，已回退」。

### 已落地的能力

| 项 | 实现 |
|---|---|
| DoH JSON | `GET <DohAddress>?name=<host>&type=A`，`Accept: application/dns-json`，读 `Answer[].data`（Python `DnsOverHttpsReq`） |
| DoH wire | `POST <DohAddress>`，`application/dns-message`，自拼 DNS query；解析 A 与 HTTPS(65) 记录，SvcParam key=5 提取 ECHConfigList base64（Python `GetEchConfigReq`） |
| 地址轮换 | 配置地址优先 + `DohUrlList` 其余 3 个；单次 5s，失败换下一个；全部失败回落 `QHostInfo` 系统 DNS，不阻断请求 |
| 缓存 | 按 host 缓存 A 记录，TTL 取应答 TTL（30~3600s），换 DoH 地址即清空 |
| 真实接管流量 | `DohEnabled` 开启时 `ResolveRoute` 把 DoH IP 放进 `RoutePlan.connectIp`（`viaDoh`），由 `NodeHttp` 连该 IP、域名做 SNI/Host/证书名；`JmApi` 按 `Domain()` 每次查缓存。等价于 `curl --doh-url + --resolve` |
| ECH | 通过 DoH 的 HTTPS RR 获取配置（桌面与 MuMu 实测 `len=96`），持久化 `proxy/echConfig`；成功 15 分钟刷新、失败 1 分钟重试；`EchEnabled` 默认 1 |
| HTTP3 | 开关默认 0；开启后所有 QNAM 请求显式 `Http2AllowedAttribute=true`，`NetClient` 首次成功打印 `transport: HTTP/2 / HTTP/1.1`；状态显示「后端不支持，已回退 HTTP/2」 |
| 降级 | `AdvancedTlsSupported()=false`、`EchOff/Http3Off`、`DowngradeAdvanced()`、`ResetAdvancedFallback()` 与 Python `Http3EchFallback` 同语义；不支持时不做无意义的降级重试 |
| 页面 | `ProxyPage` 高级区：DOH 地址 + 检查按钮 + 接管开关 + 启用 ECH / HTTP3 + 状态 + 保存/重试降级项；`--scroll-bottom` 可截长页底部 |

### 路由优先级（在 §22 基础上插入 DoH）

线路 6 反代改写 URL（禁止 IP 覆盖）> 非无代理模式（禁止 IP 覆盖）>
线路 5 + 合法可用节点 > **DoH 解析 IP（开启时）** > 系统 DNS 直连。
`ApplyProxySettings` 不会把某个域名的 DoH 结果固定成全局节点（`viaDoh` 单独标记），
`JmApi` 按当前域名取缓存，避免域名轮换后连错 IP。

### 配置键与入口

`proxy/dohAddress`、`proxy/dohEnabled`、`proxy/enableEch`、`proxy/http3`、`proxy/echConfig`。
CLI：`--doh-test [host]`、`--doh-on`（开启并同步预热）、`--advanced-status`、
`--scroll-bottom`。自检输出 `proxy-align: advanced ...` 与
`proxy-align: dohquery len=36 a=1.2.3.4 ech=1 wire=1`（手造 DNS 应答，不联网）。

### 与 Python 的差异

1. Python `CurlOpt.DOH_URL` 那段是注释，默认不接管每个请求；C++ 给一个显式
   `DohEnabled`（默认关）让 DoH 真正作用于流量，语义上等价「默认不启用」。
2. Python 用 curl_cffi 的 `CurlOpt.ECH` / `HTTP_VERSION=V3`；Qt 后端没有这两个能力，
   C++ 只保留配置、ECH 配置获取与降级状态，不做假传输。
3. ECH 配置获取失败时 Python 用下一次定时重试；C++ 同样 1 分钟重试，并可由
   「保存高级设置 / 重试降级项」立即重新尝试。

## 24. TLS/QUIC 后端替换的边界（2026-11-07）

完整方案见 `docs/cpp_tls_quic_ech_design.md`。要点：

- 新增 `cpp/shell/net_backend.{h,cpp}`：`NetBackend::Tls()/Quic()/Summary()/
  SetEchConfig()`，能力由运行时探测，不再写死。
- 实测：Qt 实际后端 `openssl/OpenSSL 3.5.5`，但该构建是 **no-ech**
  （`SSL_set1_ech_config_list` 未导出），Qt 插件也不引用 ECH 符号；QUIC/HTTP3 无栈。
- `DohClient` 拿到 ECHConfigList 后真实调用 `SetEchConfig()`，日志
  `ech config ok len=96 injected=0 (...)`；页面/自检显示
  `tls=openssl/OpenSSL 3.5.5 ech=0 echLib=0 quic=0 http3=0`。
- 替换路径：`TlsChannel`（OpenSSL 3.2+，ECH/0-RTT/QUIC-TLS 回调）与
  `QuicTransport`（ngtcp2+nghttp3）两个接口；`NetClient` 的 QNAM 路径作为 HTTP/2 回退。
- 本机无法完成实际替换的原因与依赖清单（OpenSSL `enable-ech`、ngtcp2/nghttp3、
  GitHub 502）见设计文档 §0/§7。

## 25. P1：OpenSSL 4.1 ECH 通道（2026-11-07 下午）

用户已在本机安装带 ECH 的 OpenSSL 4.1，P1 落地：

- 新增 `cpp/shell/tls/tls_channel.h`、`cpp/shell/tls/openssl_tls.cpp`：
  `QTcpSocket` + memory BIO 驱动 TLS 状态机，`QLibrary` 动态加载
  `libssl-4-x64.dll`（先 `libcrypto-4-x64.dll`），不链接、不改 Qt。
- 关键顺序：锁 TLS1.3（`SSL_CTX_ctrl(123, 0x0304)`）→
  `SSL_set1_ech_config_list` → `SSL_ech_set1_server_names(inner, outer)` →
  内层 ALPN + `SSL_ech_set1_outer_alpn_protos` → `SSL_do_handshake`。
  少任何一项都会被 Cloudflare 回 `illegal_parameter`。
- `NodeHttp` 在 `DohClient::CachedEch()` 非空且通道可用时走 ECH 通道
  （ALPN 只给 `http/1.1`），否则回退 `QSslSocket`；收包统一走 `Consume()`。
- 修复：`SSL_read` 关闭时先 `ReadyRead` 再 `Disconnected`，否则最后一批
  chunked 响应会丢，表现为 `closed mid-body`。
- `NetBackend` 探测更新：`tls=openssl-ech/4.1.0 ech=1 echLib=1`；
  `AdvancedTlsSupported()=true`，页面 ECH 显示"开"。
- CLI：`--ech-probe <host>`；环境变量 `JM_OPENSSL_ECH_DIR`、`JM_CA_BUNDLE`。

实测（桌面）：
`ech-probe: host=www.cdnhjk.net ok ech=SUCCESS(1) version=TLSv1.3 alpn=http/1.1 verify=1`
`node-http ech-tls host=172.67.199.12 sni=www.cdnhjk.net ech=71B` →
`GET /album -> 200 7710B` → `detail-real: chapters=80`。

Android 仍无带 ECH 的 OpenSSL 4.x，`CreateEchChannel()` 返回空 → 回退 QSslSocket；
QUIC/HTTP-3（P2+）仍缺 ngtcp2/nghttp3。

## 26. 版本 0.89：QUIC 连接池 / 0-RTT / 回归分层（2026-11-07 晚）

**已支持：DoH / ECH(TCP) / HTTP-3(Windows + Android arm64/x86_64)。** Android 侧的 QUIC
依赖（OpenSSL 4.1 + ngtcp2 + nghttp3）在 Linux 宿主交叉编译，Linux 脚本
`cpp/shell/ui/build_android.sh --with-quic` 直接出包；Windows 侧仍是
`build_android.ps1 -WithQuic`。

Android 的 ECH 自 0.90 起可用：`EchChannelAvailable()` 的库探测按平台分派，
Android 从 native lib 目录加载无版本号的 `libssl.so`（原实现只认
`libssl-4-x64.dll`，所以 Android 上恒为 0）。真机 V2463A 实测
`ech-probe: handshake ok version=TLSv1.3 alpn=h2 ech=SUCCESS verify=0/1`。
QUIC 侧当时仍与桌面一致地不注入 ECH（`--h3-probe --h3-ech` 可强制验证）。
**0.90 补录（§28）已改变这一结论**：H3 默认开且业务路径重新尝试 H3+ECH 组合，
服务端拒绝时按「隐藏 SNI 优先」回退 TCP+ECH；下面这段是当时（0.89→0.90 上半场）
的历史状态。

真机实测（V2463A / Android 17）：`--h3-probe` → `HTTP/3 / h3 / TLSv1.3`，258~885ms；
`--h3-bench 5` → `p50=329ms p95=3669ms`。

- 版本号三处统一为 **0.89**：`CMakeLists.txt` 的 `project(VERSION)`、
  `QT_ANDROID_VERSION_CODE/NAME`、`build_android.ps1` 的 `-VersionName`；
  APK 产物名 `JMComic-ui-0.89-<abi>-debug.apk`。
- **HTTP/3 连接池（Q2）**：`quic::Client` 按 `(host, port, echConfig 指纹)` 复用连接，
  一条连接并发多流（单连接 32 流、同域 2 连接、全局 24 连接，超限排队）。
  验收：`--h3-burst 10` → `ok=10 handshakes=1 reused=10 conns=1 queued=0`。
- **0-RTT（Q1）**：会话票据按 host 落盘（`<state>/h3/<host>.pem` + `.meta`），
  `SSL_set_session` + `SSL_set_quic_tls_early_data_enabled`；每连接只让首个 GET 走 0-RTT，
  被拒时由 ngtcp2 在 1-RTT 重传（实测 Cloudflare `0rtt=rejected`，请求仍 301 成功）。
  票据与 ECH 指纹、证书指纹、信任锚指纹绑定，任一变化即作废。
- **首包计时（Q3）**：每请求一行 `h3-timing`（resolve / udp / hs / ttfb / total / 0rtt / reused）；
  `JM_H3_QLOG=1` 写 `h3-*.sqlog`；Alt-Svc 为进程内学习，H3 失败的域名 10 分钟内不再尝试。
- **回退计数（Q5）**：`ProxyConfig::Counters()` 统计 H3 成功/失败/回退与 ECH SUCCESS/FAILED；
  页面「高级」区与 `--proxy-selftest`（`proxy-align: counters h3ok=..`）共用同一份数据源。
- **回归分层（D2）**：`c1_ui_desktop_suite.py --only offline|net`，汇总分两行
  （`offline PASS x/y`、`net PASS x/y / SKIP n`）；联网用例区分"没拿到输出 → SKIP"与
  "断言不成立 → FAIL"。
- **安全复核（T2）**：见 `docs/cpp_tls_quic_security_review.md`。修掉两个真实缺陷：
  H3 证书失败被 TLS 回退掩盖、会话复用跳过证书链重校验（票据未绑定信任锚）。

## 27. 版本 0.90：直连 H3 / 清单修补 / 返回键定性（2026-11-08）

0.90 不铺新功能，把 0.89 收尾时登记的三条"环境阻塞"逐条解掉。

- **版本号统一 0.90**：`cpp/shell/ui/CMakeLists.txt` 的 `project(VERSION)`、
  `QT_ANDROID_VERSION_CODE/NAME` 与 APK 的 `versionName/versionCode` 四处一致；
  最终包 `cpp/dist/JMComic-ui-0.90-arm64-v8a-quic-release.apk`（arm64-v8a、release 签名）。
- **改清单不再依赖 Gradle**：Qt 的 Android 模板把 `enableOnBackInvokedCallback`
  写死为 `true`，而本机 Gradle 因沙箱禁 `utimensat` 无法重新打包。新增
  `cpp/shell/ui/patch_apk_manifest.py`，直接解析 APK 里的二进制 AXML，长度稳定地
  改布尔/整型属性与等长字符串（`versionName`），改完重新解析自检；
  `repack_apk.sh` 增加 `--manifest`，`.so` 保持 STORED、清单保持 deflate，
  再走 `zipalign -p 4` + `apksigner`。**前提**：资源与 dex 不变，只改清单属性和 C++ `.so`。
- **直连也走 H3**：此前 `NetClient` 只有 `connectIp` 非空（节点/DoH）才走 `NodeHttp`，
  直连固定走 `QNetworkAccessManager`，所以"默认开 H3"对直连流量无效。新增
  `NodeHttpRequest::quicOnly`：`NetClient` 在 `--http3-on` 且 https GET 时先借
  `NodeHttp` 的 QUIC 通道试一次，失败以 `kH3Unavailable` 把控制权还给 `NetClient`，
  由 `StartQnamAttempt()` 回落 HTTP/2；证书校验失败以 `kH3CertFailure` 直接判
  `SSLErr`，**不换一套 CA 重试**（延续 T2 的"证书错误不回退"）。当时 H3 默认关，
  该分支完全不进、行为与 0.89 一致；**§28 起 H3 默认开启**，这条分支默认就会进。
- **可复跑的验收入口**：`--direct-h3-probe <url>`（桌面/Android 同义）建一个干净的
  `NetClient`，不受节点/DoH 配置影响；`cpp/tools/c1_ui_android_h3.py` 增第 6 组用例
  （直连 H3 成功、注入失败回退 QNAM 且业务成功）。刻意不复用既有的
  `--net-probe`（同步 `QEventLoop`，桌面回归依赖它）。
- **返回键重新定性**：0.89 记的"Android 17 预测性返回导致直接退应用"只说对一半。
  Qt 6.12 的 `QtActivityBase` 只有 `dispatchKeyEvent`，没有 `onBackPressed`，也没有
  `OnBackInvokedCallback` 注册；模板的 `true` 让系统默认回调直接 `finish`，并造成
  Qt 侧 back 双发。把清单改成 `false` 后，logcat 实测每次物理返回**只投递一发**
  `Qt::Key_Back(0x1000061)`，非首页返回由 `MainWindow::keyPressEvent` 的 `Back()`
  消费，应用留在前台。冒烟脚本剩余的两条 FAIL 来自脚本本身：抽屉坐标
  `(153,1953)` 在真机 1440x3168 上命中的是「首页」项（`03_search.png` 仍是首页），
  于是在首页按返回——按设计就该退出应用。详见
  `docs/cpp_ui_next_steps.md` §14。

常用命令：

```
# 连接池（10 并发 / 期望 1 次握手）
build\jmcomic_ui.exe --page home --h3-burst 10 --h3-probe cloudflare.com --h3-path /

# 分层回归
py cpp/tools/c1_ui_desktop_suite.py --only offline
py cpp/tools/c1_ui_desktop_suite.py --only net
```

## 28. 版本 0.90 补录：H3 默认开与「隐藏 SNI 优先」（2026-11-08）

产品决策（用户拍板）：**HTTP/3 默认开启**；服务端策略拒绝 ECH-over-QUIC 时
**暂时回退 TCP+ECH**（隐藏 SNI 优先于 H3 收益）；**下一次启动或更换节点时
重新尝试 HTTP/3 + ECH 组合**。

- **默认开启**：`ProxyConfig::State::http3Enabled` 改为 `true`，ini 读新键
  `proxy/http3On`（旧键 `proxy/http3` 仍写，供旧版回读）。0.89 曾把默认
  `false` 写进 ini，若继续读旧键，老安装升级后仍是关 —— 换键是必要的迁移。
  UI 复选框跟随 `Http3Enabled()`，无需改动。
  **刻意与 Python 端分歧**：Python `ProxySetting.IsOpenHTTP3` 仍默认 0；
  这是 C++ 壳的产品决策，不是功能对齐差异。
- **组合尝试**：0.89 因 Cloudflare 前端对 ECH-over-QUIC 回 `FAILED`、首包
  2s → 7~16s，彻底关掉了 QUIC 侧 ECH 注入。现在在 `NodeHttp::StartQuic()` 里
  重新注入 `CachedEch()`，前提是 ECH 开关开、ECH 通道可用且该 `(host, 节点)`
  未被抑制。`quic::Response` 新增 `echRejected`：注入过 ECH 但
  `SSL_ech_get1_status() != 1(SUCCESS)`（含握手未完成）即为真，
  **即使 HTTP/3 返回 200 也必须丢弃** —— 那是一个暴露了真实 SNI 的成功。
- **回退 TCP+ECH**：`NodeHttpRequest::echConfigList` 把同一份 ECHConfigList
  带进 TCP 分支，不依赖 DoH 缓存是否还在（缓存过期不会悄悄退化成明文 SNI）。
  真机日志 `tls handshake ok ... ech=SUCCESS verify=0/1` 证明回退后仍隐藏 SNI。
  证书校验失败仍按 T2 判 `SSLErr`、不回退。
- **抑制与重试**：`ProxyConfig::EchH3Blocked/NoteEchH3Rejected/ResetEchH3Blocks`
  维护**纯内存**的 `(host, 节点 IP)` 集合；`h3Eligible` 命中即直接走 TCP+ECH。
  冷启动集合为空 → 自动重试；`SetSelectedIp()` 换节点清空集合，
  `NodeHttp::Send()` 检测到节点变化还会清掉 Alt-Svc 的持久化 `Denied` 并立即落盘。
  Q4 组合失败**刻意不调 `DenyH3()`**：那是写进 `alt-svc.json` 的「该域名不支持
  H3」结论，与「本进程/该节点上组合被拒、下次再试」的语义冲突。
- **已知边界**：`quicOnly` 直连探测（NetClient → QNAM/HTTP2）不注入 ECH ——
  Qt 6.12 的 QNAM 没有 ECH 能力，注入后被拒也无处「回退到 TCP+ECH」，
  伪装成组合只会让 SNI 暴露得更隐蔽。这条路径只受抑制集合约束。
- **可复跑验收**：`--h3-fail-inject ech-reject` 模拟服务端策略拒绝（跳过真实
  QUIC 往返，其余抑制/回退链路共用）；`--ech-h3-probe <url>` 走 NodeHttp 业务
  路径连发两次；`--ech-h3-selftest` 纯逻辑断言抑制与换节点/重置；
  `--advanced-status` 输出 `echH3Blocked=N`（新进程恒为 0）。
  `cpp/tools/c1_ui_android_h3.py` 增第 7 组用例。
  详见 `docs/cpp_ui_next_steps.md` §14.7。

## 29. 分流页：高级设置抽屉 + 筛选条单行（2026-11-12）

原型：`ui/ui_login_new.ui` 的 tab_4（`proxyAdvPanel` / `proxyAdvFrame` / `proxyFoot` /
`proxyListHead` / `proxyFilterBar`）+ `view/user/login_proxy_new_widget.py`
（`OnAdvClicked` / `InitAdvPanel` / `CompactButton` / `SyncFilterText`）。

### 页面层级（对齐 Python）

| Python | C++（`ProxyPage`） |
|---|---|
| `proxyListHead`：标题 + 计数 + 排序 + 密度 | 计数 `countLabel_`（左）+ `sortBtn_`（右）；「节点列表」标题是页级 `SectionTitle`，不重复 |
| `proxyFilterBar`：全部 / ProxyIP / 优选，spacing 5 | `filterRow_` 单行 3 个 chip，spacing 5，尾部 stretch |
| `proxyFoot`：状态 + 高级 + 测试 IP | 操作行：`节点测速` + `nodeStatus_` + `advBtn_`（右端） |
| `proxyAdvPanel`（默认 `maxHeight=0` 隐藏） | `advPanel_`（collapsed）+ `advFrame_`（描边容器） |
| `proxyAdvFrame`：DOH / ECH+HTTP3 / 地区 / 自定义 IP / 备注 | 同序，另加 C++ 侧的「检查 / 保存高级设置」与状态行 |

`自定义 IP` 与 `地区码` 原先常驻在筛选条下方，现按 Python 归入抽屉；配置项、存储键、
应用语义都没变。

### 展开/收起

- 入口文案 `高级` ↔ `收起`（与 Python 一致）；`advBtn_` 用紧凑 chip 度量
  （30px，`proxyFoot` 的 demo 也是 30px），与 50px 的「节点测速」同排居中。
- 高度动画：先放开 `maximumHeight` 量出目标高度（含 `heightForWidth`，抽屉里有
  wordWrap 状态文本），再从 0 长到目标；结束时 `min=0 / max=QWIDGETSIZE_MAX`
  交回布局。曲线/时长取 `ui/motion.*`（`Slow` 240ms + `Enter`/`Exit`），
  **不用** Python 的 OutBack 320ms —— 全站动效只能来自 motion 表。
- 展开时 `advFrame_` 打 `open=true` 属性 → 1px accent 描边（`QWidget#AdvFrame[open="true"]`），
  收起动画结束后撤掉并 `hide()`。
- 桌面窗口不高时抽屉会落在折线以下，展开后 `ensureWidgetVisible(advFrame_)`
  把它滚进视口（Python 的抽屉是底部固定区，天然可见）。

### 筛选条度量（Python `CompactButton`）

| 控件 | Python | C++（`theme.cpp` 覆盖层） |
|---|---|---|
| 筛选 chip | `height 24 / padding 0 8 / 字体 -2.2` | `#Ghost[chip="filter"]` 24+2 = 26px；`#Primary` 态补 `padding 1px` 对齐同高 |
| 排序 | `height 22 / padding 0 6` | `#Ghost[chip="sort"]` 22+2 = 24px |
| 高级 | 脚行紧凑按钮 30px | `#Ghost[chip="adv"]` 28+2 = 30px |

基座 qss 把 `QPushButton` 内容盒钉在 34px 且 `#Ghost` 还有 7px 上下 padding（实高 50），
所以只能靠属性选择器解锁尺寸；这类 chip 必须 `hitGrow=false`，否则
`Touch::PolishHitTargets` 会把它们冻成 44dp。按钮文案去掉计数后缀（Python 的 chip
只有分组名），计数只由头行 `共 N 项 · 可用 M` 承担 —— 320dp 也排得下。

### 验收

- 几何：`jmcomic_ui --page proxy --layout-dump 6 --quiet --shot x.png`
  → 三个 chip 同为 `26px`、`y` 相同；排序 `24px` 在头行；高级 `30px` 在操作行右端。
- 视觉：`docs/shots/proxy_before_{phone,desktop}.png`（改前）对照
  `proxy_after_{phone,desktop}.png`（改后，收起）、`proxy_adv_{phone,desktop}.png`
  与 `proxy_adv_full.png`（展开）。
- 抽屉：`--proxy-test adv`（新增分支，等价于点一次「高级」，无动画；
  `--no-anim` 下同样收敛）。
- 逻辑未动：`--proxy-selftest` 仍输出 `filter proxy=32 cf=24`。

## 30. 「自定义ProxyIP地区」改为下拉选择（2026-11-12）

原型：`view/user/login_proxy_new_widget.py` 的 `RegionNames()` / `RegionParam()` /
`OnRegionPicked()` + `tools/region_code.py`，以及 `server/req.py` 的 `GetIpInfoReq`。

### 为什么之前只能手打代码

`RegionCode`（Python `Setting.ProxyIpCountry`）会被拼进节点池子域：
`https://check.jpacg.cc/resolve?proxyip=proxyip[.<code>].cmliussss.net`。C++ 侧原来
是一个自由文本框，用户填「日本」会拼成 `proxyip.日本.cmliussss.net`，取不到节点 ——
而且 `NodeResult.country` 从来没被填过（C++ 缺 `ipinfo` 链路），下拉也就没有候选来源。

### 数据链路（新增）

| 环节 | 实现 |
|---|---|
| 取地区 | `NodeTester::FetchIpInfo()`：`https://parse.jpacg.cc/ipinfo?ip=<ip>`（失败换 `parse2`），`code==200` 时写 `country = "国家·省份"`、`asn = isp` |
| 何时查 | 节点测速的第 4 相位（ping/speed 之后），只查 **ping 通且 country 为空** 的节点；4 并发、5s 超时（Python 是全部并发发，这里按可用节点裁剪，56 节点全查会多等十几秒） |
| 落盘 | `FinishNodes()` 的 `SetResults + Save()` 已带 country/asn（`proxy_config.cpp` 第 7/8 字段），下次启动 `AllNodes()` 自动回填 |
| 候选 | `ProxyConfig::RegionNames(nodes)`：取 `country` 的 `·` 前部分、去重、字典序（Python `RegionNames`） |
| 名称↔代码 | 新增 `ui/region_code.{h,cpp}`，逐条搬 Python `CountryCode` 表；`ToCode()` 认不出返回空（按原文发） |

### 交互

- 标签改成 **「自定义ProxyIP地区」**；右侧是只读框（`NoFocus` + 手型光标），
  空值显示灰色占位「全部地区」。
- 点框弹 `QMenu`：第一项「全部地区」（= 清空），其后是当前已获取到的全部地区
  （显示中文名，当前项带勾）；还没测过节点时给一条禁用项
  「（暂无地区，请先「节点测速」）」。
- 选中即应用：`SetRegionCode(ToCode(name))` + `Save()` + 回显中文名 + 状态行
  「地区 → 香港（重新测速后生效）」；不自动重跑测速（避免一次点击触发整轮网络请求）。
- 防误触两条：框上按下→抬起位移 > 8dp 不算点击（`IsRegionTap()`，与
  `Touch::TapGuard` 同阈值，`MouseMove` 一旦超阈值即作废）；菜单项由 TapGuard 的
  `isMenu` 分支守，滑动不会 `triggered`。弹菜单前 `QScroller::stop()`，避免惯性滚动
  带着菜单飘。

### 验收（离网可复跑）

| 命令 | 断言 |
|---|---|
| `--proxy-selftest` | `proxy-align: region code(us/jp)=us/jp name(us/hk/cn)=美国/香港/中国大陆 unknown=1 names=2 codes=jp,us` |
| `--proxy-test advmenu` | `items=2 [全部地区 \|（暂无地区，请先「节点测速」）]`（无数据时的空态） |
| `--proxy-test advmenudemo` | 注入 3 个地区（只改内存、不落盘）→ `items=4 [全部地区 \| 中国香港 \| 日本 \| 美国]` |
| `--proxy-test advapply` | `applied=hk\|香港`（走 `QMenu::triggered` 真实链路，跑完把配置写回原值） |
| `--proxy-test advclick` | `tap=1 drag=0`（同点算点击，位移 20px 不算） |

截图：`docs/shots/proxy_adv_full.png`（抽屉全貌，含新标签与占位框）、
`proxy_adv_phone.png`、`proxy_adv_desktop.png`。QMenu 是独立顶层窗口，
`MainWindow::grab()` 拍不到，菜单本身只能靠日志断言 + 真机目视。
