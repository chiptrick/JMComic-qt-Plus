# C++ 壳 0.91 修复轮（2026-11-08）

本轮针对移动端反馈的 9 个问题逐项修复，并在**桌面（本机，后台静默）**与
**AVD（Pixel_9a / 1080×2424 / android-37.2 x86_64）**两端验收。
构建：`cpp/shell/ui`（桌面 MinGW+Qt 6.12 / Android x86_64，同一份源码）。

## 结论速览

| # | 问题 | 根因 | 结果 |
|---|------|------|------|
| 1 | 首页分区栏滑不动、滑动误触别的标签 | `QTabBar` 在触摸屏上不支持拖动滚动，press/release 被当点击 | 换成自绘 `SectionStrip`（横向拖动 + 惯性 + 边界回弹，位移 <8dp 才算点击） |
| 2 | 登录后无法打卡 | 侧栏"打卡"是 `Toast("打卡成功（演示）")` 假实现 | 接真实 `/daily` + `/daily_chk`（uid 随会话落库），按钮反映"已打卡/未登录" |
| 3 | 分流 / 屏蔽词按钮没有边框区分 | 三个可交互行与只读 KV 行外观完全相同，命中区只有几个字 | 整行做成带边框的 `#NavAction` 按钮块（hover/pressed），整行可点 |
| 4 | 首页底部、搜索页顶部大量空白 | 首页底部页码条 60px（含无用 spin/jump）；搜索页顶部"标题+副标题"~120px；收尾反馈里页码条 30px、分组标题行 34px 仍比文字高一倍 | 页码条按字高收（桌面 18px / 文字 14px，AVD 18px）；搜索页分组标题行按字高收（桌面 20px / 文字 18px，AVD 18px）；单页时隐藏 spin/jump，手机端搜索页去掉大标题 |
| 5 | 设置页能左右滑、选项框附近滑动误触 | 内容最小宽被长文案撑到 900px（横向滚动范围 >0）；`QSlider` 按下即跳值、`QComboBox` 松手弹菜单 | 文案换行+不撑宽、下拉框限宽、锁定横向平移；`TouchSlider`（只有抓手柄才改值）/`TouchCombo`（拖动改为滚页面且不弹菜单） |
| 6 | 设置页开关点击区域太窄 | `ToggleSwitch` 固定 44×24，且 `hitButton` 只认细轨道 | 控件 52×32 + 命中区=整个控件 + **整行可点**（点标题/说明都能切换） |
| 7 | 缺少上下滚动阅读 | 阅读器只有单页/双页 | 新增 `ReadMode::Vertical`：整章纵向连续、按视口懒加载（±1 屏）、切页/工具条/偏好落库 |
| 8 | 移动端字体/字重/字宽不生效、改字号页面被拉伸 | QML 卡片墙写死 `pixelSize 13/11` 与 `Font.Medium`；QSS 用 `font-weight:500/600` 钉死字重；`Fpx()` 只缩放格子不缩放 QML 文字 | QML 接收 `uiFontFamily/uiFontWeight/uiScale`，格子高度与文字尺寸同一套公式；QSS 字重按用户字重等比缩放；去重签名纳入可变轴 |
| 9 | 缺自定义缓存路径 | 封面/章节缓存目录写死 | `CachePath`（ui.ini `[cache] root`，env `JM_CACHE_DIR` 优先）+ 设置页"缓存目录"行（选择…/默认） |

## 逐项实现要点

### 1. 首页分区栏 `SectionStrip`（新文件）
- `QTabWidget` 只当页面容器，`tabBar()->hide()`；顶部换成自绘条。
- 滚动位置 `offset_ ∈ [0, contentWidth-viewport]`，绘制 `x = TabLeft(i) - offset_`。
- press 记位置；移动 ≥8dp 判拖动（纵向为主的滑动不抢），松手按速度做惯性；
  **拖动过程绝不触发选中**，位移小于阈值才按命中项切换。
- 选中项自动滚入可视区；右侧渐隐提示还有更多分区。
- 回归入口：`--strip-drag <dx>`（打印 offset/section）、`--strip-tap <x>`、`--home-geom`。

### 2. 打卡（`JmApi` / 侧栏）
- `JmApi::FetchDailySign()` → `GET /daily?user_id=`，解析 `daily_id` 与 `record[][].signed`。
- `JmApi::SignDaily()` → `POST /daily_chk`（body: `user_id`&`daily_id`）；`EnqueueFront` 插队。
- `UserState` 增加 `uid` / `signedToday`，`SessionStore` 一起落库（冷启动仍可打卡）。
- 登录成功即拉一次签到信息；未登录点打卡 → Toast + 跳登录页。
- 失败文案用服务端 `msg`（"今天已经签到"），成功后按钮变"已打卡"。
- 离线回归注入：`--sign-mock ok|already`（不联网，验证整条 UI 状态机）。

### 3. 侧栏动作行
- `#NavAction`：1px 边框 + hover 变色 + 右侧 chevron；三条（分流×2、屏蔽词）。
- 事件过滤器按"按下-抬起位移 ≤12dp"判点击，抽屉里从该行开始滑动不会误触。

### 4. 首页/搜索留白
- 分区内边距 9→6、单页时隐藏 spin/jump、"1/1 · 18 部"短文案。
- 手机端搜索页不再渲染大标题，输入框直接贴顶；边距 28→12、间距 18→10。
- 历史/热词 chip 改 `FlowLayout`，窄屏自动换行（原来 4 个横排会溢出被裁）。
- **收尾反馈（"空白只比文字略高"）**：`HomePager` 紧凑条不设 `setFixedHeight`，
  让条高由 `RowMeta` 标签决定，上下各留 2px；多页时 spin/jump 显示会自动长回 30px。
  两个坑：① `UiStyle::Fpx()` 有 **8px 下限**，拿它算 2dp 内边距会得到 8px，
  条高又变回 30；小留白必须自己乘 `FontScale()`。② 基座 `theme/*.qss` 给所有
  `QPushButton` 钉了 `height/min-height/max-height:34px`，代码里的 `setFixedHeight`
  顶不过 QSS，所以在覆盖层给 `QPushButton#Link` 单独注入 `@linkH`（=20×字号缩放）。
- 量测入口：`--blank-report`（桌面）/`--es jm.args "--page X --blank-report"`（AVD），
  打印 `home pagerH/pagerTextH`、`search headerH/titleTextH/linkH`、
  `settings toggleRows/sameLine/maxCenterDy`。

### 5. 设置页手势
- 根因是内容被长文案撑宽：`SectionHeader` 说明换行、`SettingRow` 说明换行、
  下拉框 `AdjustToMinimumContentsLengthWithIcon` + 限宽 260、页头副标题换行。
- `Touch::LockHorizontalPan()`：viewport 上装过滤器，吞掉"横向为主"的 move。
- `TouchSlider`：触摸模式下按在轨道上 `ignore()`（冒泡给滚动容器），只有按在手柄
  ±10dp 才改值；`TouchCombo`：纵向位移 >8dp 判为滚动，自己滚外层并吞掉 release；
  `ToggleSwitch` 同理（从开关上起手滑动 = 滚页面，松手不切换）。
- 验证：`--scroll-report` 在 360/420 宽下 `hmax=0`；AVD 横滑整页零位移（差异只有状态栏时钟）。

### 6. 开关命中区与同行排版
- `ToggleSwitch` 52×32、`hitButton` 返回整控件；`SettingRow` 对开关/复选行整行接收
  release 并 `click()`；QSS `#SettingRow[toggleRow="true"]:hover`。
- **收尾反馈（"开关与文字同一高度，不能分两行"）**：`ApplyDirection()` 对二元控件
  （`ToggleSwitch` / `QCheckBox`）**始终 `LeftToRight`**，只有滑块/下拉/按钮组才在窄屏
  上下堆叠；控件 `AlignVCenter` 与文字块同排。同时给开关和文字链设 `hitGrow=false`，
  避免触摸层把它们拉成 44dp 高再撑开整行。修前开关行 101px，修后 49px。
- 验证：`--toggle-test "HTTP/3"`（点标题区，before=1→after=0）；AVD 点"开机自启"
  标题区，开关由橙色变灰；`--blank-report` 打印 `sameLine=5/5, maxCenterDy≤10px`。

### 7. 上下滚动阅读
- `ReaderView::ReadMode{ Single, Spread, Vertical }`；`SetReadMode(m, persist)`。
- 纵向模式用 `QImageReader` 只读图头算每页高度（不解码），滚动条变化时只解码
  可视区 ±1 屏，窗口外的 pixmap 释放（一章几十页不 OOM）。
- 工具条新增"上下滚动"按钮（layers 图标）；设置页"默认阅读模式"扩成三项并落库
  （`ui.ini [reader] mode`），启动时套用但**不回写**（避免测试污染用户偏好）。
- AVD 实测：切到纵向后第 1 页整宽显示、第 2 页页头接在下方，计数器仍 1/12。

### 8. 字体链路
- `ComicGrid::SyncFont()` 把 family/weight/scale 推给 QML；`PageCards.qml` 的标题/作者/
  角标全部改用它，`cellH` 与 `titleH/metaH/textTop` 用同一套缩放公式。
- `AppFont::ApplyNow()` 去重签名加入 `wght/wdth` 轴（`QFont::key()` 在部分平台不含轴）。
- `theme.cpp` 新增 `ScaleFontWeight()`：`font-weight:500/600` 按 `用户字重/400` 缩放，
  否则 QSS 会把字重钉死。
- 验证（桌面）：`--font-family "Segoe UI" --font-weight 700 --font-width 150 --font-size 12`
  → `resolved=Segoe UI weight=700/700 stretch=150 size=12.0 scale=1.333`，
  QML `family=Segoe UI weight=700 scale=1.333 cellH=290→307`（格子随字号一起长，文字不再被裁）。

### 9. 自定义缓存路径
- `CachePath`：`JM_CACHE_DIR` > `ui.ini [cache] root` > `AppLocalData/cache`；
  子目录 `covers/`、`pages/`；`CoverCache` 与 `PageLoader` 都改走它。
- 设置页"下载与缓存 → 缓存目录"：显示末两级路径（tooltip 全路径）+ 选择…/默认。
- 验证：`--cache-dir <dir>` 后 `bench: ... dir=<dir>/covers`，目录内出现 covers/pages；
  `--cache-reset` 回到默认；AVD 设置页可见该行并可点。

## 顺带修掉的两个真实缺陷（本轮回归暴露）

1. **HTTP 封面冷缓存永远是占位图**
   - 缓存键是 `key|url`，而 `CoverCache::Loaded` 的订阅方只有原始 key；
     `OnNetworkLoaded` 回查时 url 为空 → 落到另一个键 → 取不到刚下载的图。
   - 修法：`CoverCache` 记 `urlByKey_`，url 为空时还原成同一个键；
     本地文件改为**同步解码并直接返回**（原来 `LoadLocalAsync` 在 Get 返回前就发信号，
     订阅方还没存占位图，bump 落空）。
2. **本机/私网地址被节点覆盖**
   - 选了 CDN 节点后，`127.0.0.1` 的本地测试图床也会被 `--resolve` 到节点 IP，
     拿到半截正文（表现为封面被拉成一条渐变）。
   - 修法：`ProxyConfig::ResolveRoute()` 对 loopback / RFC1918 直接返回直连。

## 测试结果

### 桌面（`--quiet` 后台、不抢焦点）
- `c1_ui_desktop_suite.py --only offline` → **81/81 PASS，FAIL 0，SKIP 0**
  （页面 21 张、分页/主题/字体/性能/边界、会话、WebP、分流与节点、DoH/ECH/H3 配置，
  以及新增第 10 节"留白高度 / 开关同行"4 条断言）。
- `host_test_soft_buttons.py` → 全部 guard 通过。
- 收尾反馈复测（`--touch --win-size 420x860 --blank-report`）：
  - 首页 `pagerH=18 pagerTextH=14`（+4px）；搜索页 `headerH=20 titleTextH=18`（+2px）
  - 设置页 `toggleRows=5 sameLine=5 maxCenterDy=8px`；开关行高 101→49px
  - `toggle-test "开机自启"`：`hitHeight=49 toggleSize=52x32 before=1 after=0`
- 逐项证据（stderr 机器行）：
  - `strip: drag=-160 offset 0->160 content=764 section 0->0`
  - `strip: tap x=300 before=0 after=3`
  - `nav-action: count=3`；点 `hide` 弹出"屏蔽词"输入框
  - `sign: need-login loggedIn=0 uid=0` / `sign: state=signed`（mock）
  - `scroll-report: ... hmax=0`（360×800 与 420×860）
  - `toggle-test: row=HTTP/3 hitHeight=61 toggleSize=52x44 before=1 after=0`
  - `reader-mode: set=2 actual=2 vertical=1`
  - `font-status: ... weight=700/700 stretch=150 ... qml ... cellH=307`
  - `cache-path: ... custom=1 covers=.../cachedir/covers`

### AVD（Pixel_9a，1080×2424，android-37.2）
- 首页：分区栏横滑可滚（内容左移、`最新上传` 选中不变）；点"连载更新"切到 6 项列表；
  底部只有一行"1/1 · 18 部"，卡片占满宽度。
- 抽屉：三条动作行有边框+chevron；登录/打卡按钮可见。
- 设置：宽控件仍是标题在上、控件在下；**开关改为与标题/说明同排同高**；横滑整页零位移；
  从下拉框起手纵向拖动 = 滚页面（不弹菜单）；点"开机自启"标题区 → 开关由开变关；
  缓存目录行路径与两个按钮完整可读。
- 阅读器：工具条 6 个按钮，点"上下滚动"后整页纵向连续（第 2 页页头接在第 1 页下方）。
- 收尾反馈量测（AVD，intent `--es jm.args "--page X --blank-report"`）：
  - 首页 `pagerH=18 pagerTextH=11 labelH=14`；搜索页 `headerH=18 titleTextH=16 linkH=17`
  - 设置页 `toggleRows=5 sameLine=5 maxCenterDy=9px`（与桌面一致）
  - 底部剩余空白是系统手势导航安全区 78dp，不是页码条留白
- APK：`build-android-x86_64/jm-ui-fix9.apk`（debug 签名、zipalign -p 4），`install -r` 通过
  （`jm-ui-fix8.apk` 为安全区改动前的包，`jm-ui-fix6.apk` 为更早一轮）。
- 截图归档：`docs/shots/fix_2026-11-08/`（avd_home / avd_drawer / avd_settings_before|after，
  以及本轮 `avd_final_home|search|settings.png`）。

### 未覆盖 / 需要账号的部分
- 真实 `/daily_chk` 联网打卡：需要用户账号（AVD 无网络、无凭据），本轮验证到
  "未登录拦截"与"注入成功/已签"两条状态机 + 请求构造（uid/daily_id/POST body）。
- 真机（vivo V2463A）未接入本轮；AVD 为 x86_64，arm64 需另出包。

## 追加：移除底部手势导航安全区（2026-11-09）

- 反馈：取消手势导航安全区原有的 78dp 高度，清除该高度限制及相关间距。
- 根因：`Touch::SafeArea()` 用 `QScreen::availableGeometry()` 反推系统栏，
  Android 上底部会拿到手势条高度（Pixel_9a 实测 78dp）；`MainWindow::ApplySafeArea()`
  把它作为 `rootLayout_` 的 bottom margin，等于在系统栏之外又留了一条应用内空白带。
- 修法：
  - `Touch::SafeArea()`：Android 只保留 left/top/right，`bottom` 恒为 0；
  - `MainWindow::ApplySafeArea()`：先把 `in.bottom` 归零再设 margin，
    即使 `--safe-insets`/`JM_SAFE_INSETS` 指定了底部也不会再产生留白。
- 验证（MuMu 15.0，1080×2400，density 384）：
  - 启动日志 `touch: safeArea applied = 0,0,0,0`（修复前 Pixel_9a 为 `0,0,0,78`）；
  - 首页页码条位于 y=2352..2370，屏底 2400，仅剩 ~12dp 页面内边距，无异常空白带；
  - 搜索页、设置页顶部/底部均无被导航条遮挡，开关仍与文字同排；
  - 桌面回归第 10 节 4/4 通过（留白/同行数值不变）。

## 工具链备注（Windows）

- 直接 `ninja -C build` 前必须把 `C:\Qt\Tools\mingw1310_64\bin` 与
  `C:\Qt\6.12.0\mingw_64\bin` 放进 PATH，否则 `cc1plus` 静默失败（无错误输出）。
- Android 出包：`ninja -C build-android-x86_64` 会跑完 androiddeployqt + Gradle，
  产出 `android-build-release-unsigned.apk`；再 `zipalign -p 4` + `apksigner`（debug keystore）。
  Windows 上 Gradle 要求 `JAVA_HOME` 用**反斜杠 + 盘符**的原生路径
  （`C:\Program Files\Java\jdk-25`），写成 `C:/...` 会被判为 invalid directory；
  本机默认 `java` 是 27，构建时要显式指到 25。
- AVD 传 CLI 参数用 `adb shell "am start -n ... --es jm.args '--page X --blank-report'"`：
  外层双引号 + 内层单引号，否则 `--page` 会被 `am` 当成自己的选项，extra 只剩半个。
- `--shot` 模式在保存截图后直接 `TerminateProcess(0)`：QQuickWidget 渲染线程在
  "窗口被移出屏幕"时会在析构/DLL 卸载阶段偶发卡死或堆损坏（Qt 6.12 + MinGW），
  正常关窗与真机不受影响。
