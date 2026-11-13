# 迭代升级后无法保留原版本数据：原因分析与改进方案

日期：2026-11-09
范围：`cpp/shell/ui`（C++ 壳）与 `python/`（旧版）的数据落盘、Android 安装升级链路。

## 0. 结论（先看这一段）

升级丢数据不是单点 bug，按可能性从高到低有四层原因：

1. **应用身份变了**：旧版 Python 的包名是 `org.jmcomic.jmcomic`，C++ 壳是
   `org.jmcomic.ui`。Android 的私有数据按包名隔离，换包名等于换了一个应用，
   旧数据在新应用里**不可见**，也没有任何迁移代码。
2. **安装渠道签名不一致**：调试包用 Android debug key，发布包用 `CN=JMComic`
   key。两者 SHA-256 不同，`adb install -r` 会报
   `INSTALL_FAILED_UPDATE_INCOMPATIBLE`；现场通常选择"卸载再装"，而卸载会删除
   `/data/data/<包名>`，数据当场丢失。
3. **同包内也没有迁移层/版本号**：C++ 侧没有 `schemaVersion`，没有 `DataMigrator`，
   没有读取旧 `config.ini`/旧 `session.ini` 的代码。少量键做了别名（如
   `proxy/http3` → `proxy/http3On`），但不成体系。
4. **配置目录存在危险兜底**：`SessionStore`、`AppFont`、`CachePath` 在
   `AppConfigLocation` 为空时退回 `QCoreApplication::applicationDirPath()`。
   Android 上该路径位于 `/data/app/~~<hash>/<pkg>-<ver>/lib/<abi>`，**每次安装
   都会变**；一旦走到这个分支，配置会写进"下一次升级就消失"的目录。

另外，**登录态丢失会把"服务端数据还在"误判成"数据全丢"**：收藏、历史、阅读进度
都依赖 `session/avs` 去服务端拉取；`session.ini` 一丢，收藏页直接进"未登录"，
看起来就是"原版本的数据没了"。

---

## 1. 现状盘点

### 1.1 当前 C++ 壳持久化了什么

| 数据 | 落盘位置（代码） | 载体 | 同包升级是否保留 |
|---|---|---|---|
| 登录态 / avs / uid / signedToday / 屏蔽词 / 分流展示名 | `SessionStore::FilePath()`（`session.cpp:18`），`QStandardPaths::AppConfigLocation/session.ini` | INI | 同包同签名：保留 |
| 主题 / 字体 / 阅读模式 / 缓存目录 / 分流与节点 | `AppFont::ConfigPath()`、`ProxyConfig::SettingsPath()`、`CachePath::UiIniPath()`，均为 `AppConfigLocation/ui.ini` | INI | 同包同签名：保留 |
| 封面 / 页面缓存 | `CachePath::DefaultRoot()` = `AppLocalDataLocation/cache` | 文件 | 保留（可再生） |
| 收藏 / 历史 / 阅读进度 | 服务端 `/favorite`、`/history` 等；本地只有内存态/演示态 | 无本地库 | **依赖登录态** |

关键点：本地没有收藏/历史的数据库。登录态丢了，页面就回退到"未登录"或空状态
（`FavoritePage::OnEnter()` 在 `loggedIn=false` 时直接 `SetState(NotLoggedIn)`）。

### 1.2 旧版 Python 与新版 C++ 的对比

| 项目 | 旧版 Python | 新版 C++ |
|---|---|---|
| Android 包名 | `org.jmcomic.jmcomic`（见 `python/android/build_logs/avd-20261101-143332-x86_64-logcat.txt`） | `org.jmcomic.ui`（`AndroidManifest.xml`） |
| 配置目录 | `files/config/jmcomic-qt/config.ini`（`XDG_CONFIG_HOME=<root>/config`，`python/src/tools/platform_mobile.py:590`） | Qt `AppConfigLocation`（Android 通常是 `files/.config`），`ui.ini` + `session.ini` |
| 数据目录 | `files/data/jmcomic-qt` | `AppLocalDataLocation`（默认 `files`） |
| 缓存 | 外部 `Android/data/org.jmcomic.jmcomic/cache` | `AppLocalDataLocation/cache` |
| 桌面配置 | `%APPDATA%/jmcomic-qt/config.ini`（Windows 走 `data` 目录） | `%LOCALAPPDATA%/JMComic/JMComic/ui.ini` |

结论：两套应用的包名、目录、文件名、section/key 全不同；**默认情况下新版读不到
旧版任何数据**。

### 1.3 现有版本号与签名（实测）

| APK | 包名 | versionCode | 签名 |
|---|---|---|---|
| 0.88 x86_64 debug | `org.jmcomic.ui` | 88 | debug key |
| 0.89 x86_64 debug | `org.jmcomic.ui` | 89 | debug key |
| 0.89 arm64 release | `org.jmcomic.ui` | 89 | `CN=JMComic` |
| 0.90 arm64 release | `org.jmcomic.ui` | 90 | `CN=JMComic` |
| `jm-ui-fix*.apk`（本轮调试包） | `org.jmcomic.ui` | 90 | debug key |

- debug 证书 SHA-256：`65460e3cc2234086cd90074d5247966a3eba5c03fad445e0b1ed241003d5e446`
- release 证书 SHA-256：`5357e11191ff66db423593e99ddf0808497def99763d6d27bc9550f4fb7e02f4`

**debug → release（或反向）覆盖安装必然失败；卸载重装 = 数据删除。**

---

## 2. 原因分析（按可能性与影响排序）

### 2.1 P0：应用身份变化（跨包升级）

- Python 版 `org.jmcomic.jmcomic` → C++ 版 `org.jmcomic.ui`。
- Android 的 `/data/data/<pkg>`、`/sdcard/Android/data/<pkg>` 都按包名隔离，
  新包没有权限读取旧包目录；即使同签名也不行，除非使用 `sharedUserId`
  （已废弃且要求同签名）或旧包主动导出。
- 代码里没有任何读取旧包 `files/config/jmcomic-qt/config.ini` 或
  `files/data/jmcomic-qt` 的逻辑（`grep -i "migrat|legacy" cpp/shell/ui` 无结果）。
- 这解释了"升级后什么都没了"，而且**无法靠新版单方面读文件修复**。

### 2.2 P0：签名不一致导致必须卸载

- `adb install -r` 只允许同签名、versionCode 不低于已装版本的覆盖安装。
- 调试包与发布包证书不同（见 §1.3）。从发布版 0.90 换成 `jm-ui-fix9.apk`
  （debug）会失败；现场若卸载再装，`/data/data/org.jmcomic.ui` 连同
  `session.ini`/`ui.ini` 一起删除。
- 这不是代码 bug，但属于**升级流程设计缺陷**：没有稳定的渠道签名策略，也没有
  "失败时禁止卸载"的提示。

### 2.3 P0：没有迁移层，也没有 schema 版本

- `cpp/shell/ui` 没有任何 `schemaVersion`/`configVersion`/`DataMigrator`。
- `session.ini` 新增过 `session/uid`、`session/signedToday`；`proxy` 新增过
  `proxy/http3On`、9 字段节点格式。目前只有两处零散兼容：
  - `proxy_config.cpp:813` 读新键 `proxy/http3On`，`Save()` 同时写旧键
    `proxy/http3` 供旧版本回读；
  - `proxy_config.cpp:819` 节点 5 字段 → 9 字段的解析兼容。
- 其他键（主题、字体、阅读模式、缓存目录）一旦重命名，旧值静默丢失，
  `QSettings::value(key, default)` 直接返回默认值，没有任何告警。
- 没有"升级前备份旧文件"的步骤；迁移失败也没有回滚路径。

### 2.4 P1：配置路径兜底到 `applicationDirPath()`

- `session.cpp:26`、`app_font.cpp:35`、`cache_path.cpp:40`：
  `AppConfigLocation` 为空时退回 `QCoreApplication::applicationDirPath()`。
- Android 的 `applicationDirPath()` 在 `/data/app/~~<hash>/<pkg>-<ver>/lib/<abi>`，
  **随每次安装变化**；写到这里等于"下次升级必丢"。
- 目前 Qt Android 一般能返回非空的 `AppConfigLocation`，所以多数情况没触发；
  但这是潜伏的严重风险，也让"偶发丢数据"的现场无法复现。

### 2.5 P1：存储结构与字段变更

- Python 的 `config.ini` 是 `section/key` 结构（`[Login]`、`[Proxy]`、
  `[DownloadSetting]`、`[Reader]`…），C++ 的 `ui.ini` 是另一套键
  （`proxy/apiLine`、`font/*`、`cache/root`、`reader/mode`…），**没有映射表**。
- `QSettings` 对未知键是保留的（我们只 `setValue`，不 `clear()`），这点没问题；
  问题是旧键名不会被新代码读取，表现为"值回到默认"而不是文件损坏。
- 若未来把某个键的类型从 int 改成 string（或反之），`QVariant` 转换失败会
  返回默认值，同样静默。

### 2.6 P2：缓存与自定义路径

- `cache/root` 保存的是绝对路径；如果用户自定义到外部目录，升级后路径仍在，
  数据不丢；但如果新版本改了默认缓存目录，旧缓存会变成孤儿（可再生，不影响账号）。
- 旧版 Python 的缓存在外部 `Android/data/org.jmcomic.jmcomic/cache`，新版不会读，
  属于"缓存丢失"，不是核心数据。

### 2.7 症状放大：登录态丢失 = 收藏/历史"看起来全丢"

- 收藏、历史、阅读进度都在服务端，本地只存 `avs`/`uid`。
- `session.ini` 丢失后，`DemoData::User().loggedIn=false`，收藏页进入
  `NotLoggedIn`，侧栏收藏数显示 `—`；用户看到的就是"我的数据没了"。
- 因此**优先保住 `session.ini`（登录态）**，比保缓存更能解决用户感知。

---

## 3. 改进方案

### 3.1 P0：统一应用身份与数据路径

1. **冻结 Android 包名**：`org.jmcomic.ui` 作为长期 applicationId，写进
   `CMakeLists.txt`/manifest，CI 里加断言：`aapt dump badging` 的 package 必须等于
   该值，versionCode 必须单调递增。
2. 新增 `cpp/shell/ui/app_paths.h/.cpp`，所有落盘路径只从这里取：
   - `ConfigDir()`：`QStandardPaths::AppConfigLocation`；**Android 上为空时退回
     `AppLocalDataLocation`，绝不退回 `applicationDirPath()`**。
   - `DataDir()`、`CacheDir()`、`SessionFile()`、`UiIniFile()`。
   - 启动时打印 `data: config=... data=... schema=...`，并提供 `--path-report`。
3. 桌面与 Android 共用同一套 key；Android 的 `applicationDirPath()` 只用于
   只读资源（QML/图标），不写用户数据。

### 3.2 P0：`DataMigrator`（同包内版本迁移）

设计要点：

- 在 `main_desktop.cpp` 里、`SessionStore::Load()` / `ProxyConfig::Load()` /
  `AppFont::Load()` **之前**执行一次。
- `ui.ini` 增加 `[meta] schemaVersion`（当前设为 2；0.90 及以前视为 1）。
- 迁移步骤按版本号顺序执行，每步幂等：`v1→v2`、`v2→v3`…
- 每步只做三件事：读旧键/旧文件 → 写新键/新文件 → 记录日志。
- 写文件一律用 `QSaveFile`（原子替换 + fsync），失败保留旧文件。
- 迁移前把 `ui.ini`、`session.ini` 复制到 `backup/v<old>/`，不删除任何旧文件。
- 只 `setValue` 已知键，不 `clear()`，保留未知键，便于回滚。
- 失败策略：单步失败不回滚已成功步骤，但**不提升 schemaVersion**，下次启动重试；
  日志 `migrate: v1->v2 step=... ok/fail`。

代码骨架：

```cpp
// app_paths.h
namespace AppPaths {
QString ConfigDir();      // Android 空时退 AppLocalDataLocation
QString DataDir();
QString CacheDir();
QString UiIni();
QString SessionIni();
QString BackupDir(const QString &tag);
}

// data_migrator.h
namespace DataMigrator {
int SchemaVersion();                 // 读 [meta] schemaVersion，缺省 1
bool Run();                          // 顺序执行，返回是否全部成功
void BackupBeforeUpgrade(int from);  // backup/v<from>/
}
```

```cpp
// 示例：v1 -> v2
static bool MigrateV1ToV2(QSettings &ui, QSettings &session)
{
    // 1) 键别名：旧键存在且新键不存在时才搬
    if (ui.contains("proxy/http3") && !ui.contains("proxy/http3On"))
        ui.setValue("proxy/http3On", ui.value("proxy/http3"));
    // 2) 目录变更：旧 cache/root 指向旧默认目录时改写
    // 3) 字段补齐：session/uid 缺省 0、signedToday 缺省 false
    // 4) 不动未知键
    return true;
}
```

### 3.3 P0：跨包迁移（Python → C++）

跨包无法直接读私有目录，必须走"旧包导出 → 新包导入"或"同包名升级"：

**方案 A（推荐，长期）**：C++ 壳沿用旧包名 `org.jmcomic.jmcomic` 出第一个版本，
这样 Android 直接把旧数据交给新版本；后续再改包名时用方案 B 迁移。
代价：当前 `org.jmcomic.ui` 的 0.90 数据需要一次导出/导入。

**方案 B（兼容当前已装 0.90）**：一次性迁移工具

1. 旧 Python 包增加"导出数据"：把 `config/jmcomic-qt/config.ini`、
   `data/jmcomic-qt` 打成一个 zip，写到
   `/sdcard/Android/data/org.jmcomic.jmcomic/files/export/jm-data.zip`
   （外部私有目录，新包可读），或通过 SAF 让用户选保存位置。
2. 新 C++ 包首次启动检测该 zip（或用户通过"从旧版导入"选择文件），
   解压到临时目录，按映射表导入：
   - `[Login]`/`[User]` → `session.ini` 的 `session/avs`、`session/uid`、`user/name`
   - `[Proxy]` → `proxy/*`（线路、节点、自定义 IP）
   - `[DownloadSetting] CachePath` → `cache/root`
   - `[Reader]` → `reader/mode`
   - 主题/字体/屏蔽词 → `ui.ini` 对应键
3. 导入前备份目标 INI；导入结果写入 `migrate: import python ok n=<key数>`，
   失败逐项记录，不删除 zip。

**方案 C（桌面）**：桌面版可直接读 `%APPDATA%/jmcomic-qt/config.ini`，
在 `DataMigrator` 里做同样的映射；这条不需要用户操作。

### 3.4 P1：升级安装规范（签名与版本号）

1. **调试/发布共用一把 key**（或明确两个包名，如 `org.jmcomic.ui.debug`），
   保证 `adb install -r` 永远可覆盖；CI 校验证书 SHA-256 与上版一致。
2. `versionCode` 从 CMake `project(VERSION)` 生成，单调递增；禁止回退发版。
3. 升级脚本固定用 `adb install -r -t`；遇到
   `INSTALL_FAILED_UPDATE_INCOMPATIBLE` 时**禁止自动卸载**，提示"签名不一致，
   请用同渠道包或先导出数据"。
4. README/发版说明写清：不要卸载旧版；确需卸载时先 `--export-data`。

### 3.5 P1：写入可靠性

- `SessionStore::Save`、`ProxyConfig::Save`、`AppFont::Save` 已调用 `sync()`；
  统一改成 `QSaveFile` 写临时文件再原子替换，避免写一半掉电留下半个 INI。
- 增加"最后写入版本"字段：`[meta] lastWriteVersion`，升级后首次启动可判断
  是否需要备份。
- 配置读取失败（文件损坏/权限）时保留原文件，复制为 `.corrupt-<ts>` 后使用默认值，
  不要静默覆盖。

### 3.6 P2：可观测与人工兜底

- 新增 CLI（桌面与 Android `jm.args` 同义）：
  - `--path-report`：打印 Config/Data/Cache/Session 路径与 schemaVersion
  - `--data-report`：打印登录态、收藏数、主题、字体、缓存目录
  - `--export-data <zip>` / `--import-data <zip>`：人工迁移与客服兜底
- 启动日志固定三行：`data: paths=...`、`data: schema=...`、`data: migrate=...`，
  方便用户反馈时一眼定位。

---

## 4. 验证步骤

### 4.1 迁移器单测（无设备，最快）

1. 准备 fixtures：
   - `v1/ui.ini`（0.88 键：`proxy/http3`、无 `schemaVersion`）
   - `v1/session.ini`（无 `uid`/`signedToday`）
   - Python `config.ini`（`[Login]`、`[Proxy]`、`[DownloadSetting]`）
2. 用临时目录跑 `DataMigrator::Run()`：
   - 断言 `schemaVersion==2`、旧键值搬到新键、未知键仍在；
   - 再跑一次（幂等）：文件内容不变；
   - 删除/损坏 `ui.ini`：不崩溃、生成 `.corrupt-*`、默认值可用；
   - 断言 `backup/v1/` 下存在两份原始文件。
3. 用 `QTemporaryDir` + `QSettings` 做纯 C++ 测试，纳入 `ctest`/宿主脚本。

### 4.2 桌面覆盖升级（同目录替换 exe）

1. 用旧 exe 启动，设置主题=浅色、字体=自定义、分流=线路 3、缓存目录=自定义，
   登录（演示登录即可），退出。
2. 记录 `%LOCALAPPDATA%/JMComic/JMComic/ui.ini`、`session.ini` 的 md5。
3. 用新 exe 覆盖启动：
   - `--data-report` 断言主题/字体/分流/缓存目录/登录态全部保留；
   - `--path-report` 断言路径未变、schemaVersion 已升级；
   - `backup/v1/` 存在且 md5 与升级前一致。
4. 反向验证：把 `ui.ini` 的 `[meta] schemaVersion` 改回 1，重启应只迁移一次。

### 4.3 Android 同包升级（核心场景）

1. 安装 0.88 debug 包，登录（或注入 `session.ini`），设置主题/字体/分流/缓存目录。
2. `adb shell run-as org.jmcomic.ui md5sum files/.config/ui.ini files/.config/session.ini`
   记录基线；`adb shell run-as ... ls files/cache` 记录缓存。
3. **不卸载**，直接 `adb install -r -t jm-ui-0.90-debug.apk`（同签名、versionCode 更高）。
4. 启动后：
   - `--data-report` 五项全部保留；
   - `--path-report` 路径与基线一致；
   - `run-as` md5 对比：迁移只改预期键，未知键未丢；
   - 收藏页显示账号收藏（服务端），不是"未登录"。
5. 负例：先装 release 0.90，再 `adb install -r` debug 包 → 断言命令失败且
   设备上原应用与数据仍在（`pm path` + `run-as md5` 不变），不要卸载。

### 4.4 降级/回滚

1. 装 0.90（versionCode 90），再 `adb install -r` 0.89（vc 89）：
   预期 `INSTALL_FAILED_VERSION_DOWNGRADE`，数据不变。
2. 如需回滚：先 `--export-data`，卸载，装旧版，`--import-data`，逐项断言。

### 4.5 Python → C++ 迁移

1. 在旧 Python 包中生成真实数据：登录、改主题/字体、下载 1 章、加 2 个收藏。
2. 旧包执行"导出数据"，确认 zip 位于
   `/sdcard/Android/data/org.jmcomic.jmcomic/files/export/jm-data.zip`。
3. 安装 C++ 包，首次启动触发导入：
   - `--data-report` 断言登录态、主题、字体、缓存目录、屏蔽词；
   - 收藏页能拉到账号收藏（服务端）；
   - `migrate: import python ok n=...` 日志与预期键数一致；
   - 原 zip 保留，可重复导入。
4. 负例：zip 缺 section、字段类型错误、半截文件 → 导入失败但应用正常启动，
   原数据不被覆盖。

### 4.6 掉电/强杀容灾

1. 在 `Save()` 执行中 `adb shell am force-stop` / 断模拟器电源。
2. 重启后：旧 INI 仍可解析（QSaveFile 保证要么旧要么新），登录态不丢。
3. 连续 50 次"改设置→立即强杀→重启"，统计无一次空文件/半文件。

### 4.7 验收标准

- 同包同签名升级：`ui.ini`/`session.ini` 关键字段保留率 100%，未知键 100% 保留；
- 跨包迁移：登录态、主题、字体、缓存目录、屏蔽词 5 项全部成功；
- 任何失败路径都不删除原文件，且 `backup/` 可用；
- `--path-report`/`--data-report` 在桌面与 Android 输出一致。

---

## 5. 落地顺序建议

| 阶段 | 内容 | 预估工作量 |
|---|---|---|
| P0-1 | `AppPaths` 统一路径，去掉 Android `applicationDirPath` 兜底；`--path-report` | 0.5 天 |
| P0-2 | `DataMigrator` + `schemaVersion` + 备份 + 单测 | 1~1.5 天 |
| P0-3 | 签名/versionCode 规范与 CI 断言；升级脚本禁止自动卸载 | 0.5 天 |
| P1-1 | Python `config.ini` 导出/导入（方案 B） | 1~2 天 |
| P1-2 | `QSaveFile` 原子写、损坏文件保护 | 0.5 天 |
| P2 | `--export-data`/`--import-data`、启动日志 | 1 天 |

---

## 6. 当前工作区状态

- `ui_main_window.cpp` / `ui_pages.h` / `ui_pages_library.cpp` 有一组并行落地的
  "收藏页状态机 + 收藏分页"改动（`FavoritePage::State`、`CurrentState()`、
  `SetItems(items, total)`、`favRequestKind_`）。中途曾因头文件未同步导致构建失败，
  22:13 后已收敛，`ninja -C build` 当前为 `no work to do`。
- 本分析未改动上述文件；§4.3/§4.5 的设备验证可以直接在此基础上执行。
