# C++ 版未接入功能补齐（2026-11-13）

> 范围：把 C++ UI 里所有"尚未接入"的入口逐项接到真实实现，并保持已接入功能行为不变。
> 结论口径与 `docs/c1_ui_remaining_work.md` 一致：**已验证** = 本次实机/实测通过；
> **部分** = 代码已落地但受环境限制只验到降级路径；**阻塞** = 当前机器/设备不具备条件。

## 一、审计：改动前的"未接入"清单

审计方法：全仓搜索 `尚未接入` / `未接入` / 空态占位文案，再逐个对回 Python 实现。

| # | 功能 | 改动前位置与行为 | Python 对应 | 改动后状态 |
|---|---|---|---|---|
| 1 | 云端记录（观看记录） | `RemoteHistoryPage`：空态 + 「同步」Toast "云端记录接口尚未接入" | `GetHistoryReq2`（`GET /watch_list`）+ `view/history/remote_history_view.py` | **已验证**（未登录态 + 请求构造/解析；登录态需账号） |
| 2 | 我的评论 | `MyCommentPage`：只有空态，无请求 | `GetMyCommentReq2`（`GET /forum?mode=undefined&uid=`） | **已验证**（同上） |
| 3 | 每周必看 / 连载更新 | `WeekPage`：空态 "该列表需要连载更新接口" | `GetSerializationReq2`（`GET /serialization`）+ `index_view.GetSerializationInfo` | **已验证**（真实接口 239 部） |
| 4 | 本地收藏 | `LocalFavoritePage`：「重新扫描」Toast "本地归档目录为空" | `view/user/local_favorite_*`（本地库） | **已验证**（真实目录扫描） |
| 5 | 网络存储 / 节点管理 | `NasPage`：「添加节点」Toast "节点管理尚未接入" | `view/nas/nas_db.py`、`nas_add_view.py`、`task/upload_webdav.py` | **部分**（CRUD/校验/持久化已验证；WebDAV 浏览、SMB 协议栈见 §四） |
| 6 | 日志导出 | `AboutPage`：「导出日志」Toast "日志导出尚未接入" | `help_view.OpenLogDir`（只开目录） | **已验证**（单文件 zip，Python zipfile 校验通过） |
| 7 | 检查更新 | `AboutPage`：「检查更新」Toast "更新检查尚未接入" | `CheckUpdateReq` / `CheckUpdateInfoReq` / `CheckUpdateConfigReq` | **已验证**（真实服务 + 本地 mock 三态） |
| 8 | 注册 | `LoginPage`：「注册」Toast "注册页尚未接入" | `RegisterReq`（`POST /signup`）+ `GetCaptchaReq` | **部分**（mock 全链路通过；真实站点域名不可达） |
| 9 | 找回密码 | `LoginPage`：「忘记密码」Toast "找回密码尚未接入" | `ResetPasswordReq`（`POST /lost`）+ `RegisterVerifyMailReq`（`POST /confirm`） | **部分**（同上） |
| 11 | 作品评论（读 + 发） | `CommentPage`：只有空态；「发送」Toast "评论接口尚未接入" | `GetCommentReq2` + `SendCommentReq2`（`/forum?aid=` / `POST /comment`） | **已验证**（真实作品 313833：10 条评论） |
| 10 | QNN / HTP 超分后端 | `SrEngine` 只有 CPU EP；设置页开关置灰 + "NPU 后端未接入" | `tools/sr_backend.py` + `android/sr_qnn/cpp/sr_qnn.cpp` | **部分**（后端选择/探测/分级/回退真实；HTP 实测**阻塞**：本机 ORT 无 QNN EP） |

## 二、逐项实现

### 1) JmApi：云端记录 / 我的评论 / 连载更新

`cpp/shell/jm_api.{h,cpp}`

- 新增 `JmComment` 结构（`CID/UID/username/expinfo.level_name/content/likes/addtime/AID/name`），
  对齐 `ToolUtil.ParseBookComment`。
- 新增请求：`FetchWatchList(page)`、`FetchMyComments(page)`、`FetchSerialization(day,type,page)`；
  三者都走既有单通道队列（`Enqueue`），不新增并发。
- 新增**可单测的静态解析器** `ParseItemListPayload` / `ParseCommentListPayload`：
  - 与 `/favorite` 同源形状（`{list,total}`），复用同一条 `JItem()`；
  - **缺 `list` 即视为失败**：服务端 401/风控只回 `{code,errorMsg}`，不能把"登录失效"渲染成"你还没看过作品"。
- 401 收口与收藏一致：清 `avs_/uid_` → `SessionExpired` → 页面回到"去登录"。
- 已登录判断在**发请求之前**（无会话直接回 `未登录`，不浪费一次 12s 超时）。

### 2) 页面层：四态机 + 分页

`cpp/shell/ui/ui_pages_cloud.cpp`（新 TU）

- `RemoteHistoryPage` / `MyCommentPage`：与收藏页同一套 `NotLoggedIn/Loading/Empty/Error/Ready`
  + `Motion::CrossFade`；`page==1` 替换、`>1` 追加（`ComicGrid::AddComics` 保留滚动位置）。
- `LocalFavoritePage`：扫 `DownloadManager::Root()` 下"含图片的子目录"，空目录→空态；
  点卡片走 `MainWindow::OpenLocalImages`（与本地页同一条阅读入口）。
- `WeekPage`（`ui_pages_extra.cpp`）：类别×星期两个下拉 + 加载更多，参数与 Python
  `weekBox.currentIndex()+1 / ["all","manga","hanman"]` 完全一致。

### 3) 网络存储节点管理

`cpp/shell/ui/nas_store.{h,cpp}` + `cpp/shell/ui/ui_pages_nas.cpp`（新 TU）

- 节点模型：`local / webdav / smb`，落 `ui.ini [nas] nodes`（JSON 数组，不引数据库）。
- 校验语义：
  - `local`：`QFileInfo` 是否目录；
  - `smb`：路径形态校验 + "系统是否已映射"（**不实现 SMB 协议栈**，见 §四）；
  - `webdav`：真实 `PROPFIND Depth:0`（207/200 可用，401/403 认证失败，404 路径不存在）。
- 页面：列表（类型角标/地址/状态徽标/校验·打开·删除）+ 内联表单（名称/类型/地址/账号）。
  添加后立即校验一次，结果只做运行时显示（不落盘），避免"上次可用"误导。
- 密码明文存 `ui.ini`（与 `session.ini` 同策略），表单标题里直接写明。

### 4) 日志导出

`cpp/shell/ui/log_export.{h,cpp}`

- 自实现 store-only ZIP（本地头 + 中央目录 + EOCD + CRC32），不引第三方归档库。
- 内容：`logs/*.log`（时间倒序，单文件 ≤8MB、整包 ≤32MB、最多 24 个）+ `summary.txt`
  （版本/系统/屏幕/`AppPaths::Report()`/账号名/分流线路/超分后端/网络后端）。
- **明确排除**：`session.ini`（明文 AVS）、`ui.ini`（自定义节点等），账号只写用户名。
- Android：`JmLog::Path()` 为空（日志进 logcat），此时只导 summary 并注明去哪取日志。

### 5) 检查更新 + 更新配置

`cpp/shell/update_checker.{h,cpp}`

- `GET <base>/version.txt?version&platform` → 文本版本；`ParseVersionText` 拒绝 HTML/空响应。
- 版本比较与 Python 同口径：四段转权重 `a*1000+b*100+c*10+d`，非预发布再 `//10`。
- 有新版本再拉 `<base>/<version>.txt` 作为更新日志；`config.txt` 解析
  `Ver/VerTime/UrlList/Url2List/PicUrlList/ProxyApiUrl/CdnApiUrl/HeaderVer`。
- **本地版本线**：C++ 构建号（0.9x）与上游发布线（v1.3.x）是两条线，拿 0.94 比 v1.3.7
  只会永远"有更新"。因此显式区分 `ApplicationVersion()`（显示）与 `ReleaseLine()`
  （参与比较，默认 `v1.3.7`，可由 `ui.ini [update] version` / `JM_APP_VERSION` 覆盖）。
- 配置只落 `UrlList`（账号网页流程要用），**不改** API/图床域名与 HeaderVer —— 那属于
  `ProxyConfig` 的职责，擅自覆盖会改变已验收的线路行为。
- 端点可用 `JM_UPDATE_BASE` 覆盖（离线回归指向本地 mock）。

### 6) 注册 / 找回密码 / 重发验证邮件

`cpp/shell/jm_account.{h,cpp}`

- 四条网页端点：`POST /signup`、`POST /lost`、`POST /confirm`、`GET /captcha`，
  **不走** JmApi（不加密、无 token 头、返回 HTML）。
- 浏览器式会话：先 `GET` 表单页收 `set-cookie`，再带 `Cookie` 提交（NodeHttp 新增响应头捕获）。
- 字段与 Python 逐字对齐（`age/terms=on`、`submit_signup`、`submit_lost="恢復密碼"`、
  `submit_confirm="發送EMAIL"`）。
- 结果判定照搬 `ToolUtil.ParseMsg`：解析 `toastr['error'] / toastr['success']`；
  无 toastr 时 2xx 视为"已提交，去邮箱确认"，其余报 `HTTP n`；连不上按域名轮换后仍失败才报错。
- 界面：登录卡片下方内联面板（不弹窗），注册需验证码（点图/按钮刷新），
  验证码失败时明确提示"可改用浏览器注册"；面板展开后自动 `ensureWidgetVisible`。

### 7) 作品评论（列表 + 发送）

`cpp/shell/ui/ui_pages_library.cpp`（`CommentPage`）+ `cpp/shell/jm_api.{h,cpp}`

- 详情页新增「评论」按钮 → `CommentsRequested(Comic)` → `CommentPage::SetAlbum(id,title)` → `Go(Comment, true)`。
- 列表：`GET /forum?mode=manhua&aid=<id>&page&lang=cn`，解析复用 `ParseCommentListPayload`
  （与"我的评论"同一份实现），迟到响应按 albumId 丢弃。
- 发送：`POST /comment`（`comment` + `aid`，可选 `comment_id`），`EnqueueFront` 插队；
  成功回服务端 `msg`，失败回 `errorMsg`；401 与收藏同口径清会话。
- 未登录先提示"发表评论需要先登录"并跳登录页，不发注定 401 的请求；发送成功后自动刷新第 1 页。
- 未接入的部分（明确）：点赞、回复子评论、删除评论（Python 有独立入口，本轮未做）。

### 8) QNN / HTP 后端

`cpp/shell/sr_engine.{h,cpp}`

- 偏好：`JM_SR_BACKEND`（auto/qnn/cpu）> `ui.ini [sr] backend` > auto；
  `auto` 语义是"有 QNN 就 QNN，没有就 CPU"，绝不因为想用 NPU 把能出图的路径挡掉。
- 探测两件事（缺一即判不可用，避免"会话建成功但一个节点都没接住"）：
  1. `OrtApi::GetAvailableProviders()` 里是否有 `QNNExecutionProvider`；
  2. 后端 DSO（`libQnnHtp.so` / `QnnHtp.dll`，可用 `JM_QNN_BACKEND` 指定）是否存在。
- 建会话按 Python `sr_qnn.cpp` 的 tier 3→0 逐级降级（基础 → +fp16 → +性能/共享内存 →
  +VTCM），QNN 会话强制 `ORT_ENABLE_BASIC`（Level-2 会把 Conv+LeakyRelu 融成 CPU 专属
  `FusedConv`，QNN 分区全丢）；全部失败回退 CPU，并把首个真实错误写进 `BackendDetail()`。
- 状态可见性：`Status()` 用真实后端标签（"QNN HTP（NPU）" / "CPU（ONNX Runtime）"），
  设置页"优先使用 NPU"开关不再置灰，改为写偏好 + 释放会话（下次超分生效），
  降级原因放 tooltip，不撑高行高（设置页"开关与标题同排"断言要求单行）。
- 新增回归入口：`--sr-backend` 打印 `pref/chosen/tier/ort providers/qnn/note`。

## 三、验证结论（本机 Windows 11，桌面构建 Release）

构建：`ninja -C cpp/shell/ui/build`（Qt 6.12.0 / MinGW 13.1 / QUIC+ECH 开启），无 error。

| # | 验证项 | 命令 | 结果 |
|---|---|---|---|
| 1 | 离线回归不退化 | `c1_ui_desktop_suite.py --only offline` | **PASS 75 / FAIL 0 / SKIP 0**（改动前后一致） |
| 2 | 连载更新真实接口 | `--page week --week-report` | `serialization page=1 items=40 total=239`，`week: page=1 items=40 total=239`，截图 `_week.png`（真实标题 暴夜/Junk? Junk!/…） |
| 3 | 观看记录未登录态 | `--page remote --watchlist-report` | 页面显示"登录后查看记录 / 去登录"；**未发请求**（无会话时 JmApi 直接返回 `未登录`） |
| 4 | 我的评论未登录态 | `--page mycomment --mycomment-report` | 同上，截图 `_mycomment.png` |
| 5 | 本地收藏 | `JM_UI_CONFIG` 指定 `downloadDir` 指向含 1 张图的目录，`--page localfav` | "共 1 个本地归档" + 卡片"测试作品A / 本地"，截图 `_localfav.png` |
| 6 | 网络存储（模块级） | `--nas-selftest` | `validate-local=1 probe-local=1 save=1 roundtrip=1 probe-missing=0 validate-bad-dav=0`，`pass=1` |
| 7 | 网络存储（页面级） | `--page nas --nas-ui-selftest` | `add ok=1 / probe ok=1 / remove ok=1 / missing-dir ok=0 / bad-dav accepted=0`，`restored=0`（不污染配置） |
| 8 | 日志导出 | `--export-logs <dir>` + `zipfile.ZipFile.testzip()` | 10 个条目（9 天日志 + summary.txt），`testzip=None`，12.1MB（<32MB 上限） |
| 9 | 更新检查·真实服务 | `--update-check` | `ok=1 hasUpdate=0 version=v1.3.7`，`config Ver=67 2026-6-5 web=6 api=4 img=4 header=2.0.14` |
| 10 | 更新检查·有新版本 | `--update-check --update-base http://127.0.0.1:18080/JMComic` | `hasUpdate=1 version=v1.3.9 changelog=1`，`config ver=99` |
| 11 | 更新检查·网络失败 | `--update-base http://127.0.0.1:18099/JMComic` | `ok=0 err=Connection refused`（配置 `ver=-1`，不冒充成功） |
| 12 | 关于页状态行 | `--page about --about-report` | `status=已是最新（v1.3.7 · 发行线 v1.3.7）`，`config=服务端配置：Ver 67 · 2026-6-5 · Web 6 个 / API 4 个 / 图床 4 个`，截图 `_about.png` |
| 13 | 账号表单编码/文案（离线） | `--account-selftest` | 表单 `p&w=d → p%26w%3Dd`（以 `toHex()` 断言，控制台会把 `%XX` 改写）；`toastr error/success` 解析正确；空 HTML 判"无提示" |
| 14 | 注册/找回/验证码（mock 全链路） | `JM_WEB_BASE=http://127.0.0.1:18081 --account-flow …` | captcha `ok=1 74B image/png 8x8`；register(0000) `ok=1 注册成功`；register(9999) `ok=0 验证码错误`；reset `ok=1`；confirm `ok=1` |
| 15 | 账号请求字段与 Cookie | mock 服务端日志 | `/signup` 字段 = `age,email,gender,password,password_confirm,submit_signup,terms,username,verification`；`/lost` = `email,submit_lost`；`/confirm` = `password,submit_confirm,username`；四条请求均带预取 Cookie |
| 16 | 账号网络失败 | `JM_WEB_BASE=http://127.0.0.1:18099` | `register ok=0 msg=Connection refused`（轮换后仍失败才报错） |
| 17 | 作品评论（真实作品） | `--page comment --comment-report 313833` | `api: forum aid=313833 page=1 items=10 total=10`；截图 `_comment.png`：10 条真实评论（用户名/称号/正文） |
| 18 | 超分后端探测 | `--sr-backend` | `pref=auto chosen=cpu tier=0 ort=[AzureExecutionProvider,CPUExecutionProvider] qnn=absent note=onnxruntime 未包含 QNN EP`；`Status()` = `… · CPU（ONNX Runtime） · 192px tile · …` |
| 19 | 设置页布局 | `--page settings --touch --win-size 420x860 --blank-report` | `toggleRows=6 sameLine=6 maxCenterDy=11`（"优先使用 NPU"行单行，未破坏同排断言） |

## 四、限制与未验证（必须交代）

### A. 依赖外部条件（本机做不到，代码已就绪）

1. **登录态下的云端记录/我的评论**：`/watch_list`、`/forum?uid=` 需要真实 AVS。本次只验到
   "未登录不发请求 + 页面进 not-logged-in"；登录后的列表解析由静态解析器 + 与 `/favorite`
   同源形状保证，**未用真账号跑过**。
2. **真实站点注册/找回密码**：`GlobalConfig.UrlList` 里的 Web 域名在本机全部不可达
   （`curl` 返回 000 / 15s 超时；推测 DNS/网络策略），因此真实验证只能验到
   "验证码加载失败 → 提示改用浏览器注册"这条降级路径。请求构造/字段/解析已用 mock 全覆盖。
   另外：真实站点有 Cloudflare 人机校验时，纯 HTTP 提交可能被拒（Python 版同样受限）。
3. **QNN/HTP 实推理**：本机 onnxruntime 只带 `Azure/CPU` provider，且 x86 无 HTP。
   代码路径（EP 探测 → tier 降级 → 回退 CPU → 原因上报）已按 Python 真机实现对齐，
   但 **HTP 上跑出图**需要在 arm64 真机 + `onnxruntime-android-qnn`（含 `libQnnHtp.so`）
   + `_q16` 量化模型上验证。当前 CMake 仍把 `_q16` 排除在打包之外，接 HTP 前要一并调整。
4. **WebDAV 实连**：`PROPFIND` 校验路径已实现，但本机没有可用的 WebDAV 服务，
   只验证了"地址校验/网络失败/认证失败"三类分支（用坏地址与不可达端口）。

### B. 明确不做（能力边界，不是遗漏）

1. **SMB 协议栈**：需要在系统层映射网络驱动器；C++ 侧只校验 `\\host\share` 形态与可见性。
   写一个半成品 SMB/NTLM 客户端既不可靠也无法维护，故不做（Python 侧走 `smbprotocol`）。
2. **WebDAV 浏览/下载**：节点校验已真实；列目录/读图需要 `PROPFIND Depth:1 + XML + GET`
   的完整客户端，本期未做，点"打开"会明确提示"只支持校验"。
3. **收藏夹多文件夹管理**：`FavoritePage` 的"管理收藏夹"仍是说明性弹窗。
   Python 侧接口齐全（`AddFavoritesFoldReq2 / DelFavoritesFoldReq2 / MoveFavoritesFoldReq2`），
   但需要"收藏夹列表 + 移动/重命名"整套交互，本轮未做。
4. **评论的点赞/回复/删除**：读与发已接入；点赞（`/comment` 的 like 分支）、
   子评论回复（`replys`）、删除评论未做。
5. **更新包自动下载/安装**：只做"发现新版本 + 更新日志 + 前往下载页"，
   与 Python 行为一致（`OpenUpdateUrl`）。

### C. 需后续确认

1. **发行线口径**：`UpdateChecker::ReleaseLine()` 默认 `v1.3.7`（本移植对齐的 Python 版本）。
   C++ 构建号 0.95 与它不同线；如果后续 cpp-ui 自建发布线，需要把默认值改成自己的版本
   并在发布包里同步 `version.txt`。
2. **注册性别取值**：Python 传 `Male/Female`（`ui_register_widget` 的两个单选），
   C++ 用同一个字符串映射，未在真实站点验证服务端是否接受。
3. **验证码会话**：mock 证明 Cookie 会带上；真实站点若把验证码与会话强绑定，
   首次进入面板到提交之间若跨域/换域名会失效（当前实现换域名会清 Cookie）。
4. **日志体积**：整包上限 32MB（按时间倒序截断并在 summary 注明）。若后续日志量再涨，
   建议在 `JmLog` 侧做按天滚动 + 保留 7 天。

## 五、改动文件清单

新增：

- `cpp/shell/update_checker.{h,cpp}`
- `cpp/shell/jm_account.{h,cpp}`
- `cpp/shell/ui/log_export.{h,cpp}`
- `cpp/shell/ui/nas_store.{h,cpp}`
- `cpp/shell/ui/ui_pages_cloud.cpp`
- `cpp/shell/ui/ui_pages_nas.cpp`
- `cpp/tools/update_mock_server.py`、`cpp/tools/account_mock_server.py`（离线回归用）

修改：

- `cpp/shell/jm_api.{h,cpp}`：`JmComment`、五条请求（watch_list/forum×2/serialization/comment）、两个静态解析器、五个信号
- `cpp/shell/node_http.{h,cpp}`：`NodeHttpResponse::headers`（含 set-cookie 累积）
- `cpp/shell/sr_engine.{h,cpp}`：后端偏好/探测/分级/QNN EP/回退与诊断
- `cpp/shell/ui/ui_pages.h`：三个云端页 + `NasPage` + `CommentPage` + `AboutPage` + `LoginPage` 接口
- `cpp/shell/ui/ui_pages_browse.cpp`：详情页「评论」入口
- `cpp/shell/ui/ui_pages_library.cpp`：`CommentPage` 真实列表 + 发送
- `cpp/shell/ui/ui_pages_extra.cpp`：移除旧占位页、补 `WeekPage`
- `cpp/shell/ui/ui_pages_misc.cpp`：`AboutPage`（导出/更新）、`LoginPage`（注册/找回面板）、设置页 NPU 开关
- `cpp/shell/ui/ui_main_window.{h,cpp}`：服务对象装配、12 条信号接线、`ExportLogsTo/CheckUpdates/AboutReport/RequestWatchList/RequestMyComments/RequestWeek/NasSelfTest/OpenLocalImages`
- `cpp/shell/ui/main_desktop.cpp`：`--export-logs/--sr-backend/--nas-selftest/--nas-ui-selftest/--account-selftest/--account-flow/--update-check/--update-base/--about-report/--watchlist-report/--mycomment-report/--week-report/--comment-report/--login-panel`
- `cpp/shell/ui/CMakeLists.txt`：新增 5 个编译单元
- `docs/cpp_ui_parity.md`：页面表与"补齐的 7 个页面"数据来源列
