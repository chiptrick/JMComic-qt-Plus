# 升级丢数据与 16KB 页对齐：根因定位与修复记录

日期：2026-11-14
前置分析：`docs/data_persistence_upgrade_analysis.md`（2026-11-09，P0-1 AppPaths 已落地）。
本轮补齐剩余 P0：迁移层（P0-2）、签名规范（P0-3）、写入可靠性（P1-2），并修复
Android 15+ 16KB 页对齐不兼容。

## 1. 升级丢数据：剩余根因与修复

AppPaths（2026-11-10 落地）已消除"配置写进 applicationDirPath"的单点，但升级
丢数据的其余三个根因仍然成立，本轮逐一修复：

### 1.1 根因 A：签名不一致 -> 覆盖安装失败 -> 卸载清数据（P0）

定位：
* `build_android.sh`（旧 326-335 行）/ `build_android.ps1`（旧 248-249 行）/
  `repack_apk.sh`（旧 132 行）：debug 包一律签 `~/.android/debug.keystore`。
  该文件**每台机器都不一样**（Windows 机器 2026-11-02 生成的那把 ≠ Linux 沙箱
  的那把 ≠ release key `CN=JMComic`）。
* 后果链：跨机器/跨渠道装包 -> `INSTALL_FAILED_UPDATE_INCOMPATIBLE` ->
  现场卸载重装 -> `/data/data/org.jmcomic.ui`（session.ini/ui.ini）整体删除。

修复：
* 新增随仓 dev keystore：`cpp/shell/ui/keys/jm-dev.keystore`
  （alias `jmdev`，口令 `android`，CN=JMComic Dev，有效期 30 年）。
* 三个构建脚本的 debug 分支全部改用它——**任何机器出的 debug 包签名一致**，
  `adb install -r` 永远可覆盖。
* release 构建仍用 release keystore（不随仓）。debug(dev-key) <-> release
  (release-key) 之间切换仍需一次导出/导入（见 1.3 的 CLI）。
* `QT_ANDROID_VERSION_CODE` 由 project VERSION 推导（`major*100+minor`，
  0.95 -> 195），杜绝手写回退触发 `INSTALL_FAILED_VERSION_DOWNGRADE`。

迁移注意（一次性）：设备上旧 debug 签名的安装换到 jm-dev 签名需要卸载一次；
先在旧版本上导出（旧版本无 --export-data 时用
`adb shell run-as org.jmcomic.ui cat files/.config/session.ini` 手工拉取）。

### 1.2 根因 B：无迁移层/schemaVersion，键变更静默回默认值（P0）

定位（修复前全仓库 grep 无 schemaVersion/DataMigrator）：
* 键改名后 `QSettings::value(key, default)` 直接返回默认值（静默）；
  唯一的键别名兼容是 `proxy_config.cpp` 里 proxy/http3 的双写。
* 没有升级前备份，无法回滚；没有"配置损坏"的检测与恢复。

修复（新增 `cpp/shell/ui/data_migrator.{h,cpp}`）：
* `DataMigrator::Run()` 在 `main_desktop.cpp` 的 **export-logs/update-check 等
  CLI 分支之前**调用（这些分支都会读写 ui.ini）——先于 AppFont/SessionStore/
  ProxyConfig 的所有 Load。
* 启动时三步：
  1) **损坏隔离** `QuarantineIfBroken()`：文件解析失败（status!=NoError）或
     "非空文件解析出零键"（半截写入/二进制垃圾的形态，QSettings 会静默读成
     空配置）时，改名 `.corrupt-<时间戳>` 保留现场，同名 `.bak` 可用则拷回；
  2) **升级备份**：`[meta] schemaVersion` 低于 `kCurrentSchema(=2)` 时，把
     ui.ini/session.ini（含 .bak）原样拷到 `<ConfigDir>/backup/v<from>/`，
     不覆盖已有备份；
  3) **键迁移** v1->v2：`proxy/http3` -> `proxy/http3On`（未知键一律保留，
     绝不 clear()）。失败不提升版本号，下次启动重试；成功写
     `schemaVersion=2` 与 `lastWriteVersion`。全程幂等。
* **滚动备份** `RollingBackup()`（写成功才覆盖 .bak，坏文件不滚进备份）接入
  五个 ui.ini 写盘点：`UiPrefs::Set`、`ProxyConfig::Save`、`CachePath::SetRoot`、
  `AppFont::Persist`、`SearchHistory`（Save/Clear）。session.ini 的 Save 自带
  等价逻辑（0.94 已有）。

### 1.3 根因 C：卸载重装/换设备没有任何数据带走的手段（P2）

修复：`--export-data <dir>` / `--import-data <dir>`（桌面 CLI 与手机
`am start --es jm.args "--export-data ..."` 同义）：
* 导出 session.ini/ui.ini（含 .bak）到目录（透明可审阅，不引入手工 zip 解析）；
* 导入把当前现场滚动进 .bak 后替换；桌面导入后立即生效（在 Load 之前），
  手机导入后重读 session/proxy 并打日志。
* `--path-report` 增加 `schemaVersion` 输出（桌面与 Android 两处）。

## 2. 16KB 内存页对齐：根因与修复

### 2.1 现象与实测

Android 15+（16KB 内核页）设备启动时弹"应用兼容性提醒"。实测
`JMComic-ui-0.95-vk-arm64-v8a-debug.apk`（0.95 全量 48 个 .so）：

| 维度 | 结果 |
|---|---|
| ELF `PT_LOAD p_align` | **48/48 = 0x4000**（NDK r30 链接器默认，无需改编译参数） |
| zip 条目数据偏移 | **仅 4KB 对齐，38/48 个 mod 16K != 0**（如 libQt6Gui offset mod = 12288） |
| 条目压缩方式 | 48/48 STORED（manifest `extractNativeLibs=false`，加载器直接 mmap） |

结论：**根因是 zip 维度**。`extractNativeLibs=false` 时加载器直接从 APK 里
mmap .so，条目数据偏移必须按内核页对齐；16KB 页设备要求 mod 16384 == 0。
而 AGP 打包与脚本里的 `zipalign -p -f 4`（ps1 甚至默认跳过 zipalign）都只做
4KB。**NDK 编译参数不需要改**（r27+ 链接器默认 max-page-size=16384；若用
r26 及以下才需要 `-Wl,-z,max-page-size=16384`）。

### 2.2 修复点

| 文件 | 修复 |
|---|---|
| `cpp/tools/apk_repack.py` | `ALIGN 4096 -> 16384`（沙箱出包替代链的对齐器） |
| `cpp/shell/ui/build_android.sh` | `zipalign -f -P 16 4`；签名后 `zipalign -c -P 16 4` 硬门禁；ELF 检查改数值比较且不过即 `die` |
| `cpp/shell/ui/repack_apk.sh` | 同上（`-P 16` + `-c` 门禁） |
| `cpp/shell/ui/build_android.ps1` | zipalign 分支加 `-P 16`；**默认（无 -WithZipAlign）改走 Python 重对齐**（zipalign.exe 在本机 300MB+ APK 上会挂），并在签名后跑 `check_ui_apk_16kb.py` 双维度门禁 |
| `cpp/tools/check_ui_apk_16kb.py` | 原来只查 ELF PT_LOAD（所以旧门禁没拦住）；增加 **zip 数据偏移 mod 16384** 与 **STORED** 两个检查维度，默认自动选 dist 最新 APK |

### 2.3 验证

1. 旧 0.95 APK 跑新版门禁：FAIL（38 个 zip 维度不齐）——确认门禁能抓住真问题。
2. `apk_repack.py --base 0.95 --out aligned16k.apk`（纯重对齐，字节级保留其它
   条目）-> `apksigner sign --ks keys/jm-dev.keystore` -> 门禁
   **48/48 zip=ok + elf=0x4000，OK**。
   产物：`cpp/dist/JMComic-ui-0.95-vk-arm64-v8a-16k.apk`
   （V3 签名 CN=JMComic Dev，SHA-256 ecd4f2ec...）。
   该包可直接覆盖安装（签名与历史 debug 包不同，**首次需卸载**——先导出数据）。

## 3. DataMigrator 验证

* 宿主单测 `cpp/shell/host_tests/host_test_data_migrator.cpp`（新 target
  `host_test_data_migrator`）：5 个场景全 PASS——全新安装不建文件、v1->v2
  迁移+备份字节一致+未知键保留、幂等、损坏隔离+.bak 恢复、导出/导入+导入前
  滚动备份、RollingBackup 不滚坏文件。
* 真机桌面 exe 端到端（JM_UI_CONFIG 隔离）：v1 配置启动 -> `backup/v1/` 生成
  两份原始 ini、ui.ini 补 `http3On=true` 与 `schemaVersion=2`；
  `--import-data backup/v1` -> `--export-data` -> 删 session.ini ->
  `--import-data exp` -> `--data-report`：`loggedIn=1 name=cli uid=7 avsLen=8`
  ——登录态完整恢复。

## 4. 遗留与建议

* debug(dev-key) <-> release(release-key) 升级路径仍需一次导出/导入；若希望
  完全消除，需统一两把 key（release key 不宜随仓）。
* session.ini 明文存 AVS 的敏感性问题见 `data_persistence_upgrade_analysis.md`
  §1.1 的风险注记，本轮未处理。
* Python -> C++ 跨包迁移（旧 `org.jmcomic.jmcomic` 数据导入）仍按原分析文档
  方案 B 待做。
