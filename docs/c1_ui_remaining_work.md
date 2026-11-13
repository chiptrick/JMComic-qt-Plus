# C++ UI 剩余待办与已知限制（2026-11-04 第二轮）

> **2026-11-10 补充**：`DemoData` 已整体移除，`--real-data` 不再是开关（启动即拉
> `/promote` + `/latest`），`--demo-login` / `--demo-user` / `--demo-search` 等注入
> 参数已删除。本文中这些参数只作为当时的实现记录，仓库里已不存在。

> 状态口径：**已修** = 代码 + 自动化验证通过；**部分** = 代码已落地但受环境限制未完全验证；
> **阻塞** = 当前机器/设备不具备验证条件。

## 一、清单与优先级

| # | 项 | 优先级 | 影响 | 状态 |
|---|---|---|---|---|
| 1 | QSS 引用 Python 主题图标（`:/icon/theme/...`）缺失，下拉/SpinBox/标签箭头空白 + logcat 刷 `qt.svg` | P0（一眼假） | 全平台 | **已修** |
| 2 | 真实封面 HTTP + WebP 解码（Qt 6.12 不带 qwebp 插件） | P0（首页无图） | 全平台 | **已修** |
| 3 | 登录落库（重启后仍登录 / 退出登录清除） | P0（状态丢失） | 全平台 | **已修** |
| 4 | 真实 `/promote` + `/latest` 首页数据 | P0（数据是假的） | 桌面已验证；Android 受网络限制 | **部分** |
| 5 | 平板 / 折叠屏布局（600dp 断点、常驻侧栏、多列、详情并排） | P1 | 大屏 | **已修（桌面模拟验证）** |
| 6 | 横屏阅读器按宽度适屏导致要纵向滚动 | P1 | 横屏阅读 | **已修** |
| 7 | 16 KB 页对齐（Android 15+ 强制） | P1 | 装机/上架 | **通过（46/46）** |
| 8 | 真机性能 / 帧率 | P1 | 体验评估 | **阻塞**（无真机；AVD 为 swiftshader） |
| 9 | QNN HTP 超分 | P2 | 超分页 | **阻塞**（x86_64 AVD 无 HTP；需 arm64 + 模型） |
| 10 | 真实登录/收藏落库（token/avs）、详情页与章节真实数据 | P2 | 业务闭环 | 未做（接口已具备 Login/Favorite，UI 仍 demo） |
| 11 | 真实接口分页（`/latest` 翻页、`/promote` 多页） | P2 | 首页完整度 | 未做（只取第 0 页） |

## 二、逐项改动

### 1) 主题图标（已修）

- `theme.cpp`：新增 `RewriteLegacyIcons()`，把 QSS 里 6 类箭头
  （`downarrow/uparrow/leftarrow2/rightarrow2` 及 `*2`）替换为 `EnsureChevron()` 运行时生成的 PNG，
  颜色分 `primary`（textDim）与 `disabled`（textFaint），`EnsureChevron` 增加向上方向。
- 只替换界面真正用到的图标；未命中的规则不会触发加载，因此其余 40 个图标不需要打包。
- 验证：settings 页 stderr `qt.svg` 计数 **0**（改动前 4 条）。

### 2) WebP + 真实封面（已修）

- `ui/CMakeLists.txt`：`project(... LANGUAGES CXX C)`（C 源码需要启用 C 语言，否则 libwebp 被静默丢弃），
  编译 `../3rdparty/libwebp` 的 dec/dsp/utils 为 `jmwebp` 静态库，链接进 `jmcomic_ui`。
- `cover_cache.cpp`：`QImage::loadFromData` / `QImageReader` 失败且是 WebP 时，走 `JmWebp::Decode`
  （HTTP 与本地文件两条路径都补）。
- 验证：`docs/testdata/cover.webp`（真实 webp，RIFF/WEBP）+ 本地 HTTP 服务；
  HTTP 与本地目录两种方式截图 **逐字节一致**，且与占位图差异 100%。
- 真实链路：`--real-data` 时封面按 `https://<图片CDN>/media/albums/<id>_3x4.jpg` 拉取，
  桌面实测 167 张入缓存（98 MB），首页显示真实封面。

### 3) 登录落库（已修）

- 新增 `session.h/.cpp`：QSettings(INI) 写入 `loggedIn/name/title/coins/level/favorites/proxy/hideWords`；
  `JM_SESSION_FILE` 可指定路径（测试隔离）。
- `main_desktop.cpp` 启动时 `SessionStore::Load()`；登录页与 `--demo-login` 走 `Save()`；
  登录页新增「退出登录」（`Clear()` + 刷新侧栏）。
- 验证：登录后截图 vs 重启后（不带任何参数）截图 **md5 完全一致**；未登录与已登录侧栏不同。

### 4) 真实 `/promote` + `/latest`（部分）

- `jm_api.h/.cpp`：新增 `JmHomeBlock` 与 `FetchPromote()/FetchLatestPage()`，
  解密后按 Python `ParseIndex2/ParseLatest2` 解析；过滤 `novels/library` 两类区块。
- `MainWindow::LoadRealData()`：串行拉 latest + promote → `HomePage::SetSections()` 整组重建 tab；
  失败 Toast 并保留演示数据；新增 CLI `--real-data`。
- 桌面实测：`/latest` 80 条、`/promote` 8 区块 → 7 个分区；真实标题 + 真实封面。
- Android：AVD **无外网**（`ping 8.8.8.8` 100% 丢包），无法验证；代码路径与桌面一致，
  TLS 插件与 libssl/libcrypto 已随包（桌面已证明该链路可通）。

### 5) 平板 / 折叠屏（已修，桌面模拟验证）

- `MainWindow::UpdateMobileBreakpoint()`：触摸模式按 **600dp** 断点切换抽屉 / 常驻侧栏；
  切换时保存/恢复尺寸，避免被 `SetDrawerMode(false)` 的 980 下限弹回去。
- `ComicGrid`：触摸设备按视口列宽自适应（目标 172dp/列，2~6 列），桌面保持固定卡宽。
- 详情页：屏幕 ≥600dp 仍左图右文，手机竖屏才上下堆叠。
- 验证：800×1280 `--touch` → `layout: w=800 compact=0 rail=1`，3 列；详情并排、三个按钮可见。

### 6) 横屏阅读器（已修）

- `reader.cpp::RenderImage()`：视口宽>高时取 `min(fitW, fitH)`（按高度适屏），否则仍适宽。
- 验证：900×520 下探针 `target 高 520 ≤ 视口高 520`，整页可见。

### 7) 16 KB 页对齐（通过）

- 新增 `cpp/tools/check_ui_apk_16kb.py`：解包 APK，用 NDK `llvm-readelf` 检查每个 `.so` 的
  `PT_LOAD p_align`。
- 结果：**46/46 个 .so 均 ≥ 0x4000**（含 Qt 库、TLS 插件、libssl/libcrypto、自研 `libjmcomic_ui`）。
- 注意：这是静态门禁；真机 16 KB 内核未实跑。

### 8~9) 真机性能 / QNN 超分（阻塞）

- AVD 是 x86_64 + swiftshader，既无高通 HTP，也不代表真机 GL；冷启动 1.18 s、桌面首屏 grab 26 ms
  只能作为参考，不作为性能结论。
- QNN 超分需要 arm64 真机 + `libsr_qnn.so` + 模型；本轮未接入 C++ UI（SR 页仍是演示交互）。
  建议下一步在 arm64 真机上用 `build_android.ps1 -Abi arm64-v8a` 出包后单独验证。

## 三、剩余风险

1. **第三方接口不稳定**：域名轮换/加密密钥随服务端版本变化；目前只接首页，分页与详情仍 demo。
2. **封面规格假设**：列表接口 `image` 为空，按 `media/albums/<id>_3x4.jpg` 拼接；个别作品缺该规格时回落占位图。
3. **WebP 覆盖范围**：只做静态 WebP 解码，不支持动画 WebP / EXIF 方向（封面场景够用）。
4. **会话不是凭证**：session.ini 只存展示字段，不含 token；接真实登录时需换成加密存储。
5. **平板/折叠屏验证面窄**：仅桌面 `--touch` 模拟 800dp，未在平板 AVD / 真机 / 折叠态切换下跑。
6. **16 KB 只做静态检查**：未在 16 KB page size 的设备上实装运行。
7. **Android 真实数据未上机验证**：AVD 无外网（环境限制），需在能联网的设备上复跑
   `am start ... --es applicationArguments "--real-data"`。

---

# 第三轮：业务闭环（2026-11-05）

针对第二轮遗留的 #10 / #11（真实登录/收藏/详情/章节、真实分页），本轮全部实现：

| 项 | 实现 | 验证 |
|---|---|---|
| 真实详情 `/album` | `DetailPage::SetAlbum()`：标题/作者/简介/标签/章节列表/封面回填；`MainWindow::OpenDetail` 对 `Comic::real` 自动请求 | 桌面实测 313833 → 80 章 + 真实封面/标签；套件 `biz_detail` |
| 真实章节 `/chapter` | 新增 `PageLoader`：/chapter → 并发 6 下载页图 → WebP 解码 → `JmCrypto::Descramble` 还原 → 落盘缓存（`AppData/chapters/<id>`）→ ReaderView；进度条 + 图片 CDN 轮换重试 | 桌面实测 313833 第 1 章 27 页（26 成功 + 1 页 CDN 502），截图为还原后连贯漫画页；套件 `biz_reader`（网络波动时 skip） |
| 真实搜索 `/search` | `SearchPage::SearchRequested` → `JmApi::Search` → `SetResults`；"加载更多"翻页（`ComicGrid::AddComics` 不重置滚动）；失败回退演示搜索 | 实测 "女骑士" total=419 / "love" total=1876，80 条/页；套件 `biz_search` |
| 真实登录 `/login` | `LoginPage::LoginRequested` → `JmApi::Login`；成功把 AVS 写入 `session.ini`（重启恢复，请求自动带 Cookie）；失败显示服务端错误 | 错误凭据实测服务端 401 并回显（无测试账号，成功路径待真账号验证） |
| 真实收藏 `/favorite` | 详情页收藏按钮 → `FavoriteAdd`（POST aid，加/取消同端点）；收藏页 `OnEnter` → `FavoriteFolder(1)` → `SetItems` | 代码路径与 Python `AddAndDelFavoritesReq2` 一致；需登录态，待真账号联测 |
| 首页 `/latest` 翻页 | 最新上传 tab 增加"加载更多"→ `FetchLatestPage(n+1)` → `AppendLatest` 追加 | 实测 page=1 追加 80 条；套件 `biz_latest_more` |
| 封面规格回落 | `CoverCache`：`_3x4.jpg` → `.webp` → `.png` 依次回退；缓存键含 URL（演示基址/CDN 轮换不串缓存）；`JM_COVER_CACHE` 可指定目录 | 套件 WebP HTTP/本地一致 + 真实封面 |

新增 CLI：`--open-album <id>`、`--open-chapter <id>`、`--real-search <kw>`、`--login user:pass`、`--latest-more`。

**套件**：`c1_ui_desktop_suite.py` 新增第 7 节"业务闭环"（5 项，网络不可用时 skip 并打印诊断）。

**仍依赖环境/账号**：
1. 登录成功路径、收藏读写需要真实账号；
2. AVD 无外网，移动端真实链路未上机（代码与桌面同源）；
3. 单页 502（如 00027.webp）两镜像都失败时该页缺失，日志 `missing=1`；
4. `session.ini` 明文存 AVS（等价登录态），接发布前应换加密存储。

---

# 第四轮：登录卡顿修复 + 分流/节点测速（2026-11-05 晚，真机 V2463A）

## 一、登录"卡在登录中"

- **现象**：真机输入账号密码后长时间停在"登录中…"。
- **实测**：`logcat` 里 21:43:17 已经 `login ok user=misaka18240 avs=yes`，即服务端早已返回；
  慢在前面——`JmApi` 是单通道队列，登录 POST 排在首页 `/latest` + `/promote` 后面，
  两个请求各 12s×2 次，用户感知就是"卡住"。
- **修复**：`JmApi::EnqueueFront()`，登录改为插队；单次超时从 20s 降到 12s（24s 总预算）。
- **验证**：重装后 `session restored user=misaka18240`，侧栏显示"misaka18240 / 已登录（在线）"，
  session.ini 中的 AVS 在重装后仍然生效。

## 二、分流节点与测速（对齐 Python）

Python 原型：`python/src/config/global_config.py` + `python/src/view/user/login_proxy_new_widget.py`
+ `python/src/server/req.py` 的 `SpeedTestPingReq / SpeedTestPing2Req / SpeedTestReq`。

| 维度 | Python | C++ 实现 |
|---|---|---|
| 线路数量 | 6 条（4 条自动 + CDN + 代理），`ProxySelectIndex/ImgSelectIndex` 分别选 | `proxy_config.h` 同一份 6 条（域名逐条对齐） |
| 接口延迟 | `GET <api>/latest?page=0`，timeout=3s，指标 = 耗时毫秒 // 4 | `NodeTester::PingLine` 同口径（3s、//4） |
| 图片速度 | 下载 `<img>/media/photos/292840/00002.webp`，字节数/耗时 | `SpeedLine` 同一张样本、同一算法，格式 B/s→KB/s→MB/s |
| 节点 ping | 同上，但 DNS 覆盖到节点 IP（curl --resolve） | 同上，`http://<ip>` + `Host:` 头（Qt 无法设 SNI，见下） |
| 节点图片 | 固定封面 `media/albums/1447694_3x4.jpg`，指标 //4 | 同图同口径 |
| 排序 | 选中优先 → 非失败优先 → `延迟*0.8 + 下载*0.2` | `ProxyConfig::SortResults` 完全同权重 |
| 自动选择 | 无选中项时取排序第一 | `PickBest`：无选中→第一；**选中项已失败→换最优可用**（比 Python 收紧） |
| 并发/超时 | 全部并发、单次 3s | 并发 4、单次 3s、失败 250ms 后换 CDN 重试 |
| 持久化 | `Setting.ProxyIpValue` / `LastIpResult` | `ui.ini` 的 `proxy/{apiLine,imgLine,ip,nodes,results}` |
| 生效 | 线路影响 API/图床域名；节点在 CDN 线路时做 IP 覆盖 | 线路：`JmApi::SetDomains/SetImageDomains`；节点：见"差异" |

新增文件：`proxy_config.*`、`node_tester.*`、`proxy_dialog.*`、`ui_log.*`（桌面 qWarning 落文件）。
新增 CLI：`--proxy-dialog`、`--proxy-test lines|nodes`。

## 三、与 Python 的已知差异（必须记录）

1. **节点 IP 覆盖无法完全等价**：Qt 6.12 的 `QSslConfiguration` 没有 `setPeerVerifyName`，
   `QNetworkAccessManager` 不能对单请求改 SNI；https 直连 IP 会被 CF 直接断连（curl -k 亦然）。
   节点探测因此走 `http://<ip>` + `Host` 头（会拿到 301，按"有应答"判可达），**指标可用于排序**；
   但"把选中节点应用到真实 API/图片流量"在 Qt 下做不到，已选节点目前只做记录与排序，
   实际流量仍走所选线路的域名。若要完全对齐，需要用 QSslSocket 自建 HTTP 客户端（未做）。
2. 节点来源改为 **CDN 域名的当前 A 记录 + 历史节点**；内置 26 个 IP 实测只剩 4 个可达（已过期）。
3. 桌面端 qWarning 在 WIN32 子系统下不可见，新增 `ui_log` 写 `logs/ui-<date>.log`；
   Android 侧改为写 logcat（tag=jmcomic）。

## 四、真机验证结果

| 项 | 结果 |
|---|---|
| 登录态持久化 | ✅ 重装后 `session restored user=misaka18240`，侧栏已登录 |
| 分流配置应用 | ✅ 启动日志 `proxy-apply: api=线路 1 img=线路 1 node=(off)` |
| 设置页入口 | ✅ 设置 → 网络 → 分流与节点 → 打开，对话框正常显示且适配 384dp 宽 |
| 对话框内点击 | ⚠ 未通过：该 ROM 上模态对话框能显示但按钮点不动（桌面同版本正常） |
| 线路/节点测速 | ✅ 桌面实测（线路 280/337/399/240ms、57~78KB/s；节点 196ms/184ms 综合 193，失败项沉底） |

**待办**：把分流面板从模态对话框改成主窗口内的页面（与其它页一致的导航方式），
可同时解决真机点击不响应的问题。

---

# 第五轮：节点真正作用于流量 + 分流页（2026-11-06）

## 一、把"选中节点"接到真实流量上（上一轮的唯一硬缺口）

上一轮的结论是"Qt 做不到"——`QSslConfiguration` 没有 `setPeerVerifyName`，
`QNetworkAccessManager` 的 SNI 固定取 URL host，换成 IP 会被 Cloudflare 断连。
本轮找到可行入口并落地：

- **入口**：`QSslSocket::connectToHostEncrypted(hostName, port, sslPeerName)`
  —— TCP 连 `hostName`（传 IP），TLS 的 **SNI 与证书校验名**用 `sslPeerName`（传域名），
  正是 `curl --resolve` 的语义。
- **实证**（独立探针，连 `104.18.40.104`、SNI/Host=`www.cdngwc.cc`）：
  `encrypted in 1615ms proto=TLS1.3 peerVerifyName=www.cdngwc.cc` → `HTTP/1.1 200`。
- **新模块** `cpp/shell/node_http.{h,cpp}`：只做项目需要的 HTTP/1.1
  （GET/POST、Content-Length / chunked / 读到断连三种定界、最多 3 次重定向、
  超时与 maxBytes 上限、换 host 自动放弃 IP 覆盖）。

接入点（四处，语义一致：连接落节点 IP，SNI/Host/证书名仍是域名）：

| 位置 | 作用 |
|---|---|
| `NetClient::StartSocketAttempt` | `/latest`、`/album`、`/chapter`、`/login` 等全部 API |
| `CoverCache::StartRequest` | 封面 |
| `PageLoader::Request` | 章节页图 |
| `NodeTester::Get` | 节点探测本身（改回 **https**，删掉上一轮"http+Host、301 视为可达"的降级判据） |

配套修正：

1. **超时/断连只在响应体完整时算成功**。以前"收到响应头"就 `ok=true`，
   慢节点会产出 `net OK 200 0B` 再在解密层炸掉；现在 `BodySettled()` 统一判定，
   不完整一律按 `TimeOut`/`ResetErr` 失败，让上层换域名重试。
2. **节点图片测速 0 字节不算通过**（3s 只拿到头的节点会把分数刷得很好看）。
3. **全部节点都失败时不自动选中**（`PickBest` 返回原选择），避免"选中一个不通的节点"。
4. `JM_UI_CONFIG` 环境变量给 `ui.ini` 换目录：测试/多实例不再把"CDN 线路 + 选中节点"
   写进真实配置，后续用例不会再莫名其妙全走节点。

## 二、分流面板：模态对话框 → 一级页面

- 现象：真机 V2463A 上对话框能显示，但内部按钮点击无响应（桌面同版本正常）。
- 处理：新增 `ProxyPage`（`PageId::Proxy`），与其它页同一条导航/触摸事件链；
  侧栏"分流"两行、设置 → 网络 → 分流与节点 都进入同一页；`proxy_dialog.*` 删除。
- 附带：手机窄屏隐藏域名列、单选钮自绘圆形 indicator、失败文案统一为"失败"
  （完整原因放 tooltip，不再把 `Operation timed out` 塞进列里）。

## 三、验证

桌面（同一条代码路径）：

- API 经节点：`GET https://www.cdngwc.cc/album via 104.21.10.153 (sni=www.cdngwc.cc)`
  → `200 7830B 1085ms` → `detail-real: id=313833 chapters=80`。
- 页图经节点：`.../media/photos/313833/00013.webp via 104.21.10.153 sni=cdn-msp.jmapiproxy3.cc`。
- 节点测速（https+SNI）：`node-test done: ok=2 selected=104.21.13.76`，失败项沉底。

已知环境限制（非代码问题）：本机到图片 CDN 走节点时吞吐很低（单页 200KB~1MB，
20s 内下不完），27 页全部超时；同一网络直连域名可正常下载（上一轮 26/27 页）。
即"节点可用但太慢"时阅读会失败——这是节点本身的质量问题，页面上已经在
"当前生效"提示里标注"该节点的图片测速未通过，建议换节点"。

真机：APK 已构建（`cpp/dist/JMComic-ui-0.88-arm64-v8a-debug.apk`），
但复测时设备从 adb 掉线（`no devices/emulators found`），待重新连接后补测。

## 回归结果（2026-11-06 14:16，desktop_2026-11-06c.log）

`c1_ui_desktop_suite.py`：**PASS 73 / FAIL 0 / SKIP 3**。
skip 为网络波动（真实搜索、节点测速、节点作用于流量）。
另修：首屏 `/latest` 返回 200+空数组时换域名重试一次（此前会让"最新上传"整块空掉）。

---

# 第六轮：MuMu 模拟器实测（2026-11-06 晚，x86_64 / Android 15）

真机掉线后改用 MuMu（127.0.0.1:16384，1080×2400 @384dpi，x86_64），
重新出包 `JMComic-ui-0.88-x86_64-debug.apk` 并实测。

## 通过项（截图见 docs/shots/mumu_*.png、pp_*.png）

| 项 | 证据 |
|---|---|
| 分流页作为一级页面打开 | `pp_2.png`：页头/线路表/节点区完整，侧栏与面包屑正常 |
| 线路测速按钮可点 | 点击后按钮变“测速中…”，日志 `line-test: 5 lines` → `line-ping done`，表格填入 696ms / 18.1KB/s 等真实值（`pp_lines.png`） |
| 单选切换线路 | 点“线路 2”的接口圆点 → `proxy-apply: api=线路 2 img=线路 1 node=(off)` |
| 单选切到 CDN + 节点生效 | `proxy-apply: api=CDN 线路 img=线路 1 node=172.67.163.159(on)` |
| 节点测速按钮可点 | `node-test done: total=4 ping_ok=1 img_ok=0 selected=172.67.163.159` |
| 节点作用于真实 API | `node-http GET https://www.cdngwc.cc/latest/ via 172.67.163.159 sni=www.cdngwc.cc → 200 32964B`；`/promote/ → 200 75474B`；`real-data: sections=7 latest=80` |

**关键价值**：模拟器网络直连 `www.cdngwc.cc` / `www.cdnhjk.net` 都会 12s 超时，
套上节点后同一接口 8.8s 返回 200 —— 这正是“节点作用于真实流量”要解决的场景，
在 Android x86_64 上得到闭环验证（真机未测到这一步）。

## 模拟器操作要点（下次直接用）

- MuMu 把应用跑在**独立虚拟显示**上，每次启动 display id 会变；`screencap` 默认抓到的是启动器。
  取当前 id：`dumpsys window windows | grep -A2 'Window{.*<pkg>' | grep -o 'mDisplayId=[0-9]*'`。
- 输入必须带显示号：`input -d <id> tap x y`（`--display` 不被支持）。
- 截全屏：`screencap -a /sdcard/all.png`，应用画面通常在 `all_2.png`。
- `adb shell` 传路径要加 `MSYS_NO_PATHCONV=1`，否则 `/sdcard/...` 被 MSYS 改写。

## 未闭环

- 真实首页封面在模拟器上仍显示占位图。当 `real-data` 为空时页面回落到演示数据，
  占位图属正常；real-data 成功那次的封面请求在 60s 内没完成（直连图片 CDN 极慢），
  未拿到 `cover http failed` 就直接结束观察，**尚未定性**（倾向环境带宽，非代码）。
- 真机（V2463A）仍待复测，APK 已备好。

---

# 第七轮：线路选择 / 线路测速 / 三类 IP 分流重写（2026-11-07）

用户要求：线路选择结合测速结果与 IP 分流配置、覆盖全部线路与异常/默认行为；
线路测速覆盖全部可选线路且结果稳定可比；三类 IP 节点的配置、分类、顺序、匹配
与 Python 完全一致；完整支持 Proxy IP / CDN / 自定义 IP 分流的优先级与回退；
保持代码风格与外部接口兼容；页面与 Python 版一致。

## 改动文件

| 文件 | 变化 |
|---|---|
| `cpp/shell/ui/proxy_config.{h,cpp}` | 重写：6 条线路、三类节点配置/分类/顺序/匹配、排序/选优、测速结果与推荐、`RoutePlan` 统一路由、持久化 |
| `cpp/shell/ui/node_tester.{h,cpp}` | 重写：6 条线路全覆盖测速（2 次采样）、节点三阶段测速、稳定失败文案 |
| `cpp/shell/ui/proxy_page.{h,cpp}` | 重写：Python tab_4 版式；新增分流设置代理模式、自动选择、短失败状态 |
| `cpp/shell/jm_api.{h,cpp}` | `SetProxyRoute()`：US 反代 URL 改写（与 IP 覆盖互斥） |
| `cpp/shell/ui/cover_cache.cpp` / `page_loader.cpp` | 图片请求统一走 `ResolveRoute` |
| `cpp/shell/ui/ui_main_window.{h,cpp}` | `ApplyProxySettings` 按 RoutePlan 应用；`ProxySelfTest` 扩成离线对齐自检 |
| `cpp/shell/ui/main_desktop.cpp` | CLI `--proxy-page/--proxy-test/--proxy-selftest/--use-node`（旧 `--proxy-dialog` 保留） |
| `cpp/tools/c1_ui_desktop_suite.py` | 第 8 节扩为 11 项确定性 + 网络用例 |
| `docs/cpp_ui_parity.md` §22、本报告、测试报告 | 文档 |

外部接口保持：`Lines/LineAt/ApiLine/ImgLine/SelectedIp/Nodes/DefaultNodes/Results/
SortResults/PickBest/Load/Save`、`NodeTester::TestLines/TestNodes` 及原有信号、
`ProxyPage::StartLineTest/StartNodeTest/AutoSelectLines/Applied`、`PageId::Proxy` 均未改签名。

## 验证点

### 桌面自动化（`c1_ui_desktop_suite.py` 第 8 节）

`docs/test_logs/desktop_2026-11-07.log`：**PASS 82 / FAIL 0 / SKIP 0**；定稿改动后
`desktop_2026-11-07e.log`：**PASS 78 / FAIL 0 / SKIP 4**（第 8 节 11 项全 ok，4 个 skip
均为网络波动下的真实接口用例）。其中：

- `proxy-selftest`：表格 6 条、页面顺序 1,2,3,4,6,5；节点 56 = ProxyIP 26 + 自定义 6 + 优选 24；
  筛选 proxy=32 / cf=24；默认 api=1 img=1 无节点 无代理；异常索引 >=7 与 <=0 回落线路 1；
  路由默认直连；排序 order/best/keep/keepfail 与 Python `IpItem.__lt__` + `CheckSelectBox` 一致。
- `line-test: 6 lines`（2 次采样稳定值）、`node-test done: total=72 ping_ok=63 img_ok=63
  selected=138.2.21.53`、节点作用于 `/album` 返回 80 章。

### MuMu（x86_64 / Android 15，127.0.0.1:16384）

`docs/shots/mumu_deliver_*.png`、`mumu_final_lines.png`、`mumu_nodes_done.png`：

| 操作 | 结果 |
|---|---|
| 清空配置启动 | `proxy-apply: api=分流1 img=分流1 node=(off) reason=line 1 direct`；两组单选均为分流1 |
| 点「测速」 | 6 条全覆盖，`line-test: 6 lines (2 ping samples)` → `line-test done: 6 lines`；短状态「连接超时/下载失败/未选节点」，实测 389ms / 0.26mb 等 |
| 点「自动选择」 | 按测速结果选到 US反代分流：`reason=proxy-line jm2-api.jpacg.cc | proxy-line jm2-img.jpacg.cc` |
| 筛选 PROXYIP / 优选 | 共 32 项 / 共 24 项，行内容分别为 ProxyIP N / 优选 N；「按延迟」排序生效 |
| 切「系统」代理 | 提示「*使用代理时 IP 分流不生效，节点 IP 覆盖自动关闭」，状态「代理模式 → 系统」 |
| 点「节点测速」 | `proxyip fetch(retry) 42 nodes` → `node-test done: total=72 ping_ok=8 img_ok=6 selected=104.26.10.124`；页面显示「共 72 项 · 可用 8」、已选 优选 8 · 267ms、各行延迟/下载分数 |

另：MuMu 的另一个虚拟显示上正好运行着 Python 版分流页，可作为版式对照——
C++ 页已具备同样的分流设置、4 列线路测速表、6 行两组单选、节点筛选/排序/自定义 IP 区。

## 产物

`cpp/dist/JMComic-ui-0.88-x86_64-debug.apk`（MuMu）、
`cpp/dist/JMComic-ui-0.88-arm64-v8a-debug.apk`（真机），均 12:01 重新编译并签名。

---

# 第八轮：DOH / ECH / HTTP3（2026-11-07 下午）

上一轮把 Python 高级面板的这三项列为「Qt 网络栈无对应实现，未做假控件」；
本轮补齐可落地的部分，并明确标注后端能力边界。逐条对齐见 `docs/cpp_ui_parity.md` §23。

## 改动文件

| 文件 | 变化 |
|---|---|
| `cpp/shell/doh_client.{h,cpp}` | **新增**：DoH JSON + DNS wire 客户端、A/HTTPS 记录解析、地址轮换、TTL 缓存、ECH 配置获取与 15min/1min 刷新 |
| `cpp/shell/ui/proxy_config.{h,cpp}` | 新增 5 个配置键、`AdvancedTlsSupported/EchOff/Http3Off/DowngradeAdvanced/ResetAdvancedFallback/AdvancedStatus/SyncDohClient`；`RoutePlan` 增 `viaDoh`；`ResolveRoute` 插入 DoH 优先级 |
| `cpp/shell/ui/proxy_page.{h,cpp}` | 新增「高级」区（DOH 地址 / 检查 / 接管开关 / ECH / HTTP3 / 状态 / 保存重试） |
| `cpp/shell/jm_api.cpp` | DoH 开启时按当前域名查缓存，把解析 IP 交给 `NodeHttp` |
| `cpp/shell/net_client.cpp` | 显式 `Http2AllowedAttribute`，首次成功打印实际传输协议 |
| `cpp/shell/ui/ui_main_window.cpp` | 自检增加 advanced/dohquery 两行；`ApplyProxySettings` 区分 `viaDoh`、同步 DoH 配置 |
| `cpp/shell/ui/main_desktop.cpp` | CLI `--doh-test` / `--doh-on` / `--advanced-status` / `--scroll-bottom` |
| `cpp/shell/ui/CMakeLists.txt` | 加入 `../doh_client.cpp`（桌面 + 两个 Android ABI 共用） |
| `cpp/tools/c1_ui_desktop_suite.py` | 新增第 9 节 6 项 |
| 文档 | `cpp_ui_parity.md` §23、本文件第八轮、测试报告第六轮、工作日志 |

## 能力边界（重要）

Qt 6.12 头文件与 `Qt6Network.dll` 均无 QUIC/HTTP3 与 ECH 支持，
`QDnsLookup::Protocol` 也只有 `Standard / DnsOverTls`。因此：

- **DoH 是真实生效的**：解析结果直接决定 TCP 连接 IP（`NodeHttp` 保留域名 SNI）。
- **ECH 配置是真实获取的**（桌面 / MuMu 均从 `cloudflare-ech.com` 的 HTTPS RR 拿到
  96 字节 ECHConfigList），但 Qt TLS 后端无法把它注入 ClientHello。
- **HTTP/3 无 QUIC 传输**，开启后回退 HTTP/2（`Http2AllowedAttribute` + `transport:` 日志）。

页面状态明确写出「ECH 开·后端不支持，已回退普通 TLS / HTTP3 开·后端不支持，
已回退 HTTP/2」，并保留 `EchOff/Http3Off`、`DowngradeAdvanced`、
`ResetAdvancedFallback` 与 Python `Http3EchFallback` 同语义的降级接口。

## 验证点

桌面全量回归 `docs/test_logs/desktop_2026-11-07_advanced2.log`：
**PASS 88 / FAIL 0 / SKIP 0**（第 9 节 6 项全 ok，第 8 节 11 项仍全 ok）。

桌面专项 `py c1_ui_desktop_suite.py --only advanced`：**PASS 6 / FAIL 0 / SKIP 0**：

- `advanced doh=https://parse.jpacg.cc/parse active=0 ech=1 http3=0 supported=0`（Python 默认值）
- `dohquery len=36 a=1.2.3.4 ech=1 wire=1`（手造 DNS 应答验证 A 记录与 HTTPS RR ech 参数）
- `doh-test: host=www.cdnhjk.net viaDoh=1 ips=104.21.44.113,172.67.199.12`
- `ech config ok len=96`
- `DoH 解析结果作用于 API 流量（/album 80 章）`

实测日志：`GET https://www.cdnhjk.net/album via node 172.67.199.12
(sni=www.cdnhjk.net)` → `detail-real: id=313833 chapters=80`。

MuMu（x86_64 / Android 15）：高级区三个开关与检查按钮均可点；「检查」显示
`检查通过：www.cdnhjk.net -> 104.21.44.113,172.67.199.12`；三项全开后
`proxy-apply ... doh=1 reason=doh www.cdnhjk.net via 172.67.199.12`，
API 请求 `req 1/2 GET /promote/ via node 172.67.199.12 (sni=www.cdnhjk.net)`，
`ech config ok len=96`；页面底部同时显示接口/图片两条 DoH 路由。

操作要点：该 ROM 上 QCheckBox 的**小方块区域点击不生效，点文字标签可以**
（自动化点击请用标签坐标）；另 `input` 坐标按 PNG 真实像素，ViewImage 显示的是缩略图。

## 产物

`cpp/dist/JMComic-ui-0.88-{x86_64,arm64-v8a}-debug.apk`，12:54 重编签名。

---

# 第九轮：TLS 替换为 ECH / 引入 QUIC（2026-11-07 下午）

目标：把 TLS 换成支持 ECH 的实现、引入 QUIC 传输栈，完成 ECH 与 HTTP/3 的
加密层/传输层集成；并说明模块边界、ECH 配置分发、QUIC-TLS 集成点、HTTP/3 分层、
兼容性与测试场景。

**结论：本机依赖不满足，无法真实完成替换。** 实测证据：

| 检查 | 结果 |
|---|---|
| Qt 实际 TLS 后端 | `openssl/OpenSSL 3.5.5`（加载的是 Git 自带的 `libssl-3-x64.dll`） |
| 该库是否带 ECH | 否：`SSL_set1_ech_config_list`、`SSL_ech_get1_status` 均未导出（no-ech 构建） |
| Qt `qopensslbackend.dll` | 不引用任何 ECH 符号 |
| OpenSSL 开发包 | 无（只有 `openssl.exe`，没有头文件/导入库） |
| QUIC 库 | 无 ngtcp2 / nghttp3 / quiche；vcpkg、msys2 均不存在 |
| 网络 | `git ls-remote github.com` → `CONNECT tunnel failed, response 502` |
| 系统 curl | 8.18.0 Schannel，Features 无 HTTP3，`--http3-only` 直接报不支持 |

## 已交付（可编译、已回归）

| 文件 | 内容 |
|---|---|
| `cpp/shell/net_backend.{h,cpp}` | 新增：TLS/QUIC 能力探测、`SetEchConfig()` 注入入口、`Summary()`；`echRuntime` 用 `QLibrary` 查 `SSL_set1_ech_config_list` 导出 |
| `cpp/shell/ui/proxy_config.{h,cpp}` | `AdvancedTlsSupported()` 改为真实探测；`AdvancedStatus()` 附带 `tls=... ech=... echLib=... quic=...` |
| `cpp/shell/doh_client.cpp` | 拿到 ECHConfigList 后真实调用 `SetEchConfig()`，日志 `injected=0/1` + 原因 |
| `cpp/shell/ui/ui_main_window.cpp` | 自检输出 backend 字段，页面与自检同源 |
| `cpp/shell/ui/CMakeLists.txt` | 加入 `../net_backend.{h,cpp}`（桌面 + 两个 ABI） |
| `docs/cpp_tls_quic_ech_design.md` | **新增**：模块边界、ECH 配置/分发、QUIC-TLS 握手与 0-RTT、HTTP/3 分层、回退、12 项测试矩阵、依赖清单与 P1~P4 阶段计划 |

设计文档已把替换所需的依赖与命令写全：
OpenSSL `perl Configure ... enable-ech`（Windows mingw64 / Android arm64）、
ngtcp2 + nghttp3 的 CMake 构建、Qt 侧走自建 `TlsChannel` 而不改 Qt、
CMake 开关 `JM_WITH_ECH` / `JM_WITH_HTTP3`。

## 验证

- 自检：`proxy-align: advanced ... tls=openssl/OpenSSL 3.5.5 ech=0 echLib=0
  quic=0 http3=0 stack=未编译`，证明探测读到真实后端而不是写死。
- ECH 路径：`ech config ok len=96 injected=0 (OpenSSL 3.5.5 为 no-ech 构建…)`，
  说明配置获取真实、注入点被真实调用且失败原因可诊断。
- 全量回归：`docs/test_logs/desktop_2026-11-07_tls_backend.log`
  **PASS 88 / FAIL 0 / SKIP 0**（第 8/9 节共 17 项全通过）。
- APK：13:20 重编签名，16 KB 页对齐 46/46 通过。

## 待办（依赖到位后）

按设计文档 §7.3 执行 P1（OpenSSL-ECH `TlsChannel` + `NodeHttp` 接入）、
P2（ngtcp2 QUIC 1-RTT）、P3（nghttp3 HTTP/3 + QPACK）、P4（0-RTT + ECH + H3 组合）。
业务层与页面不需要再改。

---

# 第十轮：P1 完成（OpenSSL 4.1 ECH 通道，2026-11-07）

用户在本机安装 OpenSSL 3.5.9（no-ech）与 **OpenSSL 4.1（带 ECH）** 后进入 P1。

## 改动文件

| 文件 | 变化 |
|---|---|
| `cpp/shell/tls/tls_channel.h` | **新增**：`TlsChannel` 抽象（Connect/Write/ReadAll/Close/Info/SupportsEch + 信号）、`EchChannelAvailable/Status/CreateEchChannel` |
| `cpp/shell/tls/openssl_tls.cpp` | **新增**：`OpenSslEchChannel`，QTcpSocket + memory BIO；动态加载 4.1；ECH 注入、内/外层名字、outer ALPN、证书校验、状态机 |
| `cpp/shell/node_http.{h,cpp}` | ECH 通道接入（ALPN 仅 http/1.1）；收包抽成 `Consume()`；`Cleanup` 释放通道 |
| `cpp/shell/net_backend.cpp` | 探测到 ECH 通道时返回 `openssl-ech/4.1.0 ech=1 echLib=1`；`SetEchConfig` 返回 true |
| `cpp/shell/ui/ui_main_window.{h,cpp}` | `EchProbe(host)`；自检 Summary 输出 |
| `cpp/shell/ui/main_desktop.cpp` | CLI `--ech-probe`；`--doh-on` 等 ECH 配置就绪 |
| `cpp/shell/ui/proxy_page.cpp` | 高级状态文案改用 `NetBackend::Tls().note` |
| `cpp/shell/doh_client.cpp` | ECH 日志 `injected=1`、状态"已交给 ECH 通道" |
| `cpp/shell/ui/CMakeLists.txt` | 加入 `tls/openssl_tls.cpp`、`tls/tls_channel.h` |
| `cpp/tools/c1_ui_desktop_suite.py` | 第 9 节新增 ECH 握手、API 经 ECH 两项；`supported=1` 断言 |
| `docs/cpp_tls_quic_ech_design.md` | P1 实现细节与踩坑（§7.4）、结论更新 |

## 关键调用顺序（缺一即 `illegal_parameter`）

1. `SSL_CTX_ctrl(ctx, 123, 0x0304)` 锁 TLS1.3；
2. `SSL_set1_ech_config_list(ssl, ecl, len)`；
3. `SSL_ech_set1_server_names(ssl, inner, outer, 0)`（outer 来自 ECHConfig 的 `public_name`）；
4. `SSL_set_alpn_protos` + `SSL_ech_set1_outer_alpn_protos`；
5. `SSL_set_bio`（memory BIO）+ `SSL_set_connect_state` + `SSL_do_handshake`；
6. 握手后 `SSL_ech_get1_status` + `SSL_get_verify_result` + `X509_check_host(inner)`。

CA：`JM_CA_BUNDLE` → Git 的 `mingw64/etc/ssl/certs/ca-bundle.crt` → Qt 系统证书临时 PEM →
默认路径。动态加载：先 `libcrypto-4-x64.dll` 再 `libssl-4-x64.dll`，`JM_OPENSSL_ECH_DIR` 可覆盖。

## 验证

- 专项：`--only advanced` **PASS 8 / FAIL 0 / SKIP 0**：
  `proxy-align: advanced ... supported=1 ... tls=openssl-ech/4.1.0 ech=1 echLib=1`；
  `ech-probe: www.cdnhjk.net ok ech=SUCCESS(1) version=TLSv1.3 alpn=http/1.1 verify=1`；
  `node-http ech-tls` + `ech=SUCCESS` + `/album 80 章`。
- 全量：`docs/test_logs/desktop_2026-11-07_ech_p1.log`
  **PASS 90 / FAIL 0 / SKIP 0**（原 88 项 + ECH 握手/ECH 流量 2 项）。
- Android 两端编译通过（`openssl_tls.cpp` 已编入）；运行时无 4.1 → 回退 QSslSocket。
- APK：14:15 重编签名，16 KB 页对齐 46/46。

## 第十轮：QUIC / HTTP-3（已完成，Windows）

用户把 ngtcp2 1.26.90 与 nghttp3 1.19.90 clone 到本机后，P2/P3 落地：

- 依赖：OpenSSL 4.1 的 DLL 用 `gendef`+`dlltool` 生成 MinGW 导入库；nghttp3 以
  `-O2 -mavx2` 构建静态库；ngtcp2 用 `ENABLE_OPENSSL=ON`，配置阶段
  `Looking for SSL_set_quic_tls_cbs - found` 命中后启用 `libngtcp2_crypto_ossl`。
- 代码：新增 `cpp/shell/quic/quic_http.{h,cpp}`（QUdpSocket + ngtcp2 + nghttp3 +
  OpenSSL 4.x QUIC-TLS，pimpl 隔离）；`NodeHttp` 增加 H3 优先路径与自动回退；
  `NetBackend::Quic()` 改为读真实能力；页面状态文案区分"开（QUIC/HTTP3）"与"已回退 HTTP/2"。
- 构建：`JM_WITH_QUIC`（默认 OFF，Android 强制 OFF），`JM_QUIC_ROOT` /
  `JM_OPENSSL41_ROOT` 指定依赖；POST_BUILD 把 `libssl-4-x64.dll`、`libcrypto-4-x64.dll`
  拷到 exe 同目录。
- 验收：`--h3-probe cloudflare.com` → `done ok=1 status=301 version=HTTP/3 alpn=h3 TLSv1.3`；
  应用内 `proxy/http3=true` + `--doh-on --open-album 313833` →
  `node-http h3 GET /album -> 200 7736B alpn=h3`，`detail-real: chapters=80`。
- 如实记录：ECH-over-QUIC 在 Cloudflare 上被拒（`ech=FAILED`，同配置 TCP 通道是 SUCCESS），
  且首包从 2s 恶化到 7~16s；因此 H3 分支默认不注入 ECH，ECH 保留在 TCP 通道。

## 未完成

- 0-RTT 与会话票据复用、H3 连接池（一期是一请求一连接）。
- Android 的 QUIC：现有 OpenSSL 3.3.1 无 QUIC-TLS，需交叉编译 OpenSSL 4.x（含 ECH）
  并把 .so 打进 APK；`JM_WITH_QUIC` 在 Android 保持 OFF，业务自动走 HTTP/1.1。
