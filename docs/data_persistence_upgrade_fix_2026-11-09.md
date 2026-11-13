# 同包升级后登录态与个性化设置丢失：定位与修复（2026-11-09）

场景：Android 安装 0.88 debug → 设置个性化项并登录 → `adb install -r` 覆盖安装
0.90 debug（同包名 `org.jmcomic.ui`、同 debug 签名、versionCode 88→90）→ 打开应用
显示未登录，个性化设置回到默认值。

## 1. 结论（先看这一段）

三件事叠在一起，被用户统一感知成"升级把数据弄丢了"：

| # | 问题 | 影响 | 性质 |
|---|---|---|---|
| 1 | **设置页绝大多数控件从未落库** | 主题、语言、开机自启、阅读方向、亮度、自动翻页、超时、并发、超分模型、NPU 开关改完只存在内存里，**进程一退就回默认** | 真 bug，与升级无关但升级后才被注意到 |
| 2 | **会话只在主文件里，且 401 立即清空** | 升级后首启若网络未就绪 / 服务端返回 401，`SessionExpired` 会 `Clear()` 掉 avs 与账号字段，且没有备份可回退 | 真 bug，放大了"升级后未登录" |
| 3 | **配置目录兜底到 `applicationDirPath()`** | Android 上该路径是 `/data/app/~~<hash>/<pkg>-<ver>/lib/<abi>`：只读、**每次安装都变**。一旦 `AppConfigLocation` 返回空串就会写进去（静默失败） | 潜伏雷，本次已拆 |

**不是**的原因：包名与签名。实测 0.88 与 0.90 都是 `org.jmcomic.ui`，证书
SHA-256 都是 debug key（`ed88c984…`），`install -r` 成功且数据目录未被删除。

## 2. 实测证据（MuMu，1080×2400，Android 15）

### 2.1 配置路径没问题，但写盘确实为空

安装 0.88 后（`--demo-login --font-size 12`）全盘搜索：

```
su 0 find /data/data/org.jmcomic.ui -name '*.ini'   →  无任何结果
```

同一台设备装 0.90 + 诊断版后打印：

```
path-report: AppConfigLocation=/data/user/0/org.jmcomic.ui/files/settings writable=yes
path-report: configDir=/data/user/0/org.jmcomic.ui/files/settings
path-report: session.ini=…/files/settings/session.ini exists=0
```

说明：目录是稳定的、可写的；0.88 那次没写是因为 `--demo-login` / `--font-size`
**根本没接进 Android 的参数白名单**（见 `main_desktop.cpp` 的 `jm.args` 分支），
不是路径坏了。

### 2.2 登录态确实只存在一个文件里

正常登录（或演示登录）后：

```
/data/user/0/org.jmcomic.ui/files/settings/session.ini   212 B
[user] name=南野 / title=Lv.7 · 收藏家 / favorites=42 …
[session] loggedIn=true
```

覆盖安装前后 md5 完全一致：

```
21d232f62fa0081f3c3133776a15185b  session.ini   （install -r 前后同值）
16490a0c69ddaaf00922ddbb03a542c8  ui.ini
```

启动日志 `data-report: loggedIn=1 name=南野 …`，即**同包同签名覆盖安装本身不会丢登录态**。
真正会让它丢的是 `SessionExpired` 分支：一次 401 就 `SessionStore::Clear()`，
旧版没有 `.bak`，没有回退余地。

### 2.3 个性化设置：只有一小部分落库

设置页逐项核对（`cpp/shell/ui/ui_pages_misc.cpp`）：

| 已落库 | 未落库（改完即丢） |
|---|---|
| 字体（`[font]`，AppFont） | 主题、语言 |
| 缓存目录（`[cache] root`，CachePath） | 开机自启、阅读方向、亮度、自动翻页 |
| 阅读模式（ReaderView） | 超时、并发数 |
| 分流/节点、H3、ECH（ProxyConfig） | 超分模型、NPU 开关 |

覆盖安装后 `ui.ini` 里只有 `[font]` 一组 —— 用户看到的"个性化设置全没了"，
本质是这些开关**从来没有被保存过**，重启也一样丢。

## 3. 修复内容

### 3.1 `AppPaths`（新增 `app_paths.h/.cpp`）

统一解析配置/数据/缓存目录，规则：

1. `JM_UI_CONFIG` 显式覆盖（回归隔离，优先级最高）；
2. `AppConfigLocation`；
3. `AppLocalDataLocation/settings`、`AppDataLocation/settings`；
4. Android：`HOME/settings`、`HOME`（Qt 设为 `/data/user/0/<pkg>/files`）；
5. 桌面：`applicationDirPath()/config` —— **仅非 Android**。

每个候选目录用探针文件实测可写（`QTemporaryFile`），避免只读挂载的
`isWritable()` 假阳性。目录不可用时 `SessionStore::Save()` 明确告警，
不再静默失败。`MigrateLegacyFile()` 会把旧位置（桌面 `config/`、
`GenericConfigLocation/JMComic/JMComic`）的 `session.ini` / `ui.ini` 复制过来。

### 3.2 `UiPrefs`（新增 `ui_prefs.h/.cpp`）+ 设置页接线

`ui.ini` 的 `[ui]` 分组保存：`theme / language / autoStart / readDirection /
brightness / autoNext / netTimeout / netConcurrency / srModel / srNpu`。

- 设置页每个控件先按偏好初始化，再接 `currentIndexChanged` / `toggled` 立即写入；
- H3 / ECH 两个开关改为读写 `ProxyConfig`（`proxy/http3On`、`proxy/enableEch`）
  并调用 `ProxyConfig::Save()`，不再与设置页各说各话；
- 启动时若命令行未显式指定 `--theme`，用 `UiPrefs::Theme()` 应用上次的主题。

### 3.3 会话加固（`session.cpp`）

- 写成功后再滚动一份 `session.ini.bak`（只在写成功时更新，避免坏文件覆盖好备份）；
- `Load()` 时主文件缺失/解析为空 → 自动回退 `.bak`；
- 写失败打印 `status` 与路径，不再无声无息。

### 3.4 可量测入口

`--path-report` / `--data-report`（桌面 CLI 与 Android `jm.args` 同名同义）：

```
path-report: AppConfigLocation=… writable=yes / configDir=… / session.ini=… exists=1
data-report: loggedIn=1 name=南野 …
data-report: prefs theme=light_pink … themeNow=light_pink
```

顺手补上 Android 缺失的 `--demo-login`、`--font-size` 白名单项——之前它们只在
桌面生效，手机端验收根本触发不到落库路径。

## 4. 验证结果

### 4.1 桌面（420×860 触摸模式）

| 用例 | 结果 |
|---|---|
| 点"开机自启" → 写盘 | `[ui] autoStart=false`，`toggle-test before=1 after=0` |
| 重启后再点同一开关 | `before=0 after=1` —— 状态确实从磁盘恢复 |
| 预置 `theme=light_pink` 后启动 | `themeNow=light_pink`，界面转浅色 |
| 开关排布回归 | `toggleRows=5 sameLine=5 maxCenterDy=8`（未回退） |
| 完整离线回归 | `offline PASS 87/87`（FAIL 0 / SKIP 0） |

### 4.2 MuMu（`install -r` 覆盖安装）

| 步骤 | 结果 |
|---|---|
| 预置 `[ui] theme=light_pink …` 后启动 | `prefs theme=light_pink … themeNow=light_pink`，截图确认浅色主题 |
| `install -r` 覆盖同包同签名 | `session.ini` / `session.ini.bak` / `ui.ini` 三份 md5 全部不变 |
| 覆盖后启动 | `loggedIn=1 name=南野`、`themeNow=light_pink`、`autoStart=0`、`srModel=2` 全部恢复 |
| 删除 `session.ini` 再启动 | 日志 `session: 主会话文件不可用，改用备份 …session.ini.bak`，登录态照常恢复 |

### 4.2.1 只改版本号的升级实测（0.90 vc90 → 0.91 vc91，真实账号）

设备上先用真实账号登录并手工改过两项：**分流节点**（IP分流 / ProxyIP 39
`191.222.247.114`）与**字体**（自定义 `Noto Serif Hebrew` / 8pt / 字重 400 / 字宽 100%）。
随后只把 `QT_ANDROID_VERSION_CODE 90→91`、`QT_ANDROID_VERSION_NAME "0.90"→"0.91"`，
其余代码不动，重新出包并 `adb install -r`。

| 检查项 | 升级前 | 升级后 |
|---|---|---|
| `session.ini` md5 | `542cfc8d…` | `542cfc8d…`（一致） |
| `session.ini.bak` md5 | `542cfc8d…` | `542cfc8d…`（一致） |
| `ui.ini` md5 | `6617fd08…` | `6617fd08…`（一致） |
| 登录态 | `misaka18240 / uid=13909003 / avsLen=26` | 同名同 uid 恢复 |
| 字体 | 自定义 Noto Serif Hebrew 8pt | `app-font: source=custom family=Noto Serif Hebrew size=8` |
| 分流 | IP分流 / `191.222.247.114` | `proxy-apply: api=IP分流 img=IP分流 node=191.222.247.114(on)` |
| 主题 | 浅色 · 粉色 | `themeNow=light_pink`，设置页仍显示「浅色 · 粉色」 |
| 设置页外观 | — | 主题浅色·粉色、语言简体中文、开机自启关、字体自定义/Noto Serif Hebrew/8pt |
| 分流页 | — | API 与图片分流均为「IP分流」，节点列表选中 ProxyIP 39 `191.222.247.114`（694ms） |
| 首页渲染 | — | 升级前后截图**像素级一致**（max diff = 1，变化像素 0%） |

截图：`docs/shots/fix_2026-11-08/settings091_1.png`（设置页）、
`proxy091_1.png`（分流与节点页）、`upgraded091b_2.png`（首页）。

### 4.3 回归

- `c1_ui_desktop_suite.py --only offline` → **87/87 PASS，FAIL 0，SKIP 0**
  （含页面 21 张、分页/主题/字体/性能/会话、收藏页四态、分流与节点、DoH/ECH/H3）。
- `host_test_soft_buttons.py` → 守卫全过。

## 5. 仍需真机验证的项

1. **真实账号登录 + 升级**：MuMu 上已用真实账号（`misaka18240`）验证通过；
   真机 V2463A 建议按同一套 `--data-report` 复测一次。
2. **签名混装**：release 包与 debug 包互刷仍会 `INSTALL_FAILED_UPDATE_INCOMPATIBLE`，
   现场若"卸载再装"必然丢数据；发版流程应统一签名，或先做导出/导入。
3. **跨包迁移**：Python 版（`org.jmcomic.jmcomic`）→ C++ 版仍需导出/导入，
   本次未实现，见 `docs/data_persistence_upgrade_analysis.md` §4。

## 6. 改动文件

| 文件 | 改动 |
|---|---|
| `cpp/shell/ui/app_paths.h/.cpp` | 新增：统一目录解析、可写探针、Android 禁 `applicationDirPath`、旧位置迁移 |
| `cpp/shell/ui/ui_prefs.h/.cpp` | 新增：`ui.ini [ui]` 偏好读写 |
| `cpp/shell/ui/session.cpp/.h` | 改用 AppPaths；写入告警；`session.ini.bak` 滚动与回退；暴露 `Path()` |
| `cpp/shell/ui/cache_path.cpp` | `UiIniPath()` / `DefaultRoot()` 走 AppPaths |
| `cpp/shell/ui/app_font.cpp` | 字体配置走 AppPaths；写失败告警 |
| `cpp/shell/ui/proxy_config.cpp` | `SettingsPath()` 走 AppPaths，保留 `JM_UI_CONFIG` |
| `cpp/shell/ui/ui_pages_misc.cpp` | 设置页 10 个控件接 `UiPrefs`；H3/ECH 接 `ProxyConfig` 并保存 |
| `cpp/shell/ui/main_desktop.cpp` | 启动恢复主题；`--path-report` / `--data-report`；Android 补 `--demo-login`/`--font-size`；版本号改 0.90 |
| `cpp/shell/ui/CMakeLists.txt` | 加入新源文件 |
| `cpp/tools/c1_ui_desktop_suite.py` | 清会话时一并清 `.bak`（否则"未登录"用例会被备份恢复） |

产物：
- `cpp/shell/ui/build-android-x86_64/jm-ui-fix17.apk`（0.90 / vc90，x86_64、debug 签名、
  `zipalign -p 4` 校验通过）
- `cpp/shell/ui/build-android-x86_64/jm-ui-0.91-vc91.apk`（0.91 / vc91，仅版本号变化，
  用于本次升级验证）
- 桌面端同步到 0.91（`QApplication::setApplicationVersion` 与 CMake `VERSION`）。
