# 搜索记录（最近搜索）无法保留：排查与修复（2026-11-09）

## 1. 现象

搜索页「最近搜索」里始终是四个固定词（星海彼岸 / 雨夜便利店 / realcugan /
完结 科幻）：搜索任何关键词都不会出现在这里，重启、覆盖升级后也不会变成自己
搜过的词；点「清空」没有效果。

## 2. 按环节排查

按「查询（触发）→ 存储（落盘）→ 读取（恢复）」三段查，结果**三段全缺**，
不是某一环写坏了，而是功能此前根本没实现。

### 2.1 查询 / 触发环节

`SearchPage::RunSearch()`（`ui_pages_browse.cpp`）原本只做四件事：trim、记录
`keyword_`、隐藏历史区、发 `SearchRequested`。**没有任何写入搜索历史的调用**。
所有入口都汇聚到这里：

- 输入框回车 `SearchBox::Submitted`
- 「搜索」按钮 `clicked`
- 「热门标签」chip `clicked`
- 「最近搜索」chip `clicked`
- 外部触发 `SearchPage::Search()` / CLI `--demo-search` → `SearchDemo()`

因此不管用户从哪里发起搜索，历史都不会新增。

### 2.2 存储环节

全局搜索 `最近搜索 / searchHistory / recent` 只找到 `ui_pages_browse.cpp` 的
界面文案，**没有任何 `QSettings` 写入**。`ui.ini` 里从来没有 `[search]` 分组。
也就是说：

- 没有存储介质（既不在 `ui.ini`，也没有独立的 history 文件）；
- 没有 MRU 排序、忽略大小写去重、条数上限；
- 「清空」没有对应的删除/落库逻辑；
- 写失败无告警（这个坑在上一轮 `AppPaths` 修复里已处理，本次沿用）。

### 2.3 读取环节

`SearchPage` 构造函数里是硬编码：

```cpp
const QStringList hist = {"星海彼岸", "雨夜便利店", "realcugan", "完结 科幻"};
for (const QString &h : hist) { auto *chip = new Chip(h, ...); ... }
```

- 每次启动都用这四条重建 chips，磁盘里就算有数据也不会被读；
- `SectionHeader("最近搜索", …, "清空")` 创建后没有接 `ActionButton()` 的
  `clicked` —— 按钮点了什么都不会发生；
- `PageBase::OnEnter()` 未 override，切回搜索页不会刷新列表。

### 2.4 排除项（避免误判）

| 猜测 | 实测结论 |
|---|---|
| 升级把 `ui.ini` 删了 | `install -r` 前后 `ui.ini` md5 完全相同（见 `data_persistence_upgrade_fix_2026-11-09.md`） |
| Android 配置目录不可写 | `AppConfigLocation=/data/user/0/<pkg>/files/settings writable=yes`，同一文件里 `[font]`/`[proxy]`/`[ui]` 都能正常落库 |
| QSettings 写入被静默吞掉 | 本次实现统一 `sync()` + 检查 `status()`，并打日志 |

## 3. 修复

新增 `cpp/shell/ui/search_history.h/.cpp`：

- 存储：`ui.ini` 的 `[search] recent`（QStringList），路径走 `AppPaths::ConfigFile()`，
  与字体、分流、偏好同目录 —— 因此天然继承「同包同签名升级不丢」；
- `Add(keyword)`：trim → 忽略大小写去重（旧位置删掉）→ 插到队首 → 截断到 10 条
  → `QSettings::sync()` → 检查 `status()`，失败打日志；
- `Items()`：读回时再清洗一次（trim / 去空 / 去重 / 截断），手工编辑过的 ini 也不会
  把空 chip 或重复 chip 渲染出来；
- `Clear()`：`remove()` + `sync()`；`Report()` 给 `--data-report` 用。

`SearchPage` 接线：

| 位置 | 改动 |
|---|---|
| `RunSearch()` | 非空关键词调用 `SearchHistory::Add()`，变化则 `ReloadHistory()` |
| `SearchDemo()` | 离线演示搜索同样记一条（让 CLI 验收能走真实落库链路） |
| 构造函数 | 「最近搜索」改为可重建的 `historyGroup_` + `histFlow_` + 占位文案 |
| `ReloadHistory()` | 清空旧 `QLayoutItem`（连 widget 一起释放），按 `Items()` 重建 chips，点 chip 仍走 `RunSearch`（会自动把该词提到队首） |
| 「清空」 | `connect(histHeader->ActionButton(), clicked)` → `ClearHistory()` → `Clear()` + 刷新 + Toast |
| `OnEnter()` | 切回搜索页时重新读盘并刷新 |

整组**常驻**（空记录时显示「暂无搜索记录」），因为桌面布局回归要量
「最近搜索 / 清空」这一行的高度。实测空/非空两种状态都是
`headerH=20 titleTextH=18 linkH=20`。

验收入口：

- `--data-report` 增加一行 `history=N first=xxx`；
- `--history-clear`：等价于点「清空」文字链（桌面 CLI 与 Android `jm.args` 同名）；
- Android 白名单补 `--demo-search <词>`。

## 4. 可复现的测试步骤

### 4.1 桌面 CLI（干净环境，约 1 分钟）

```bash
cd cpp/shell/ui
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/6.12.0/mingw_64/bin:$PATH"
export JM_UI_CONFIG="<repo>/cpp/shell/ui/shots_fix/_cfg_hist"   # 隔离配置
rm -rf "$JM_UI_CONFIG"
run() { ./build/jmcomic_ui.exe --page search --touch --win-size 420x860 \
          --quiet --shot /tmp/h.png --shot-delay 200 "$@" 2>&1; }

run --data-report                         # 期望 history=0 first=-
run --demo-search "星海彼岸"
run --data-report                         # 期望 history=1 first=星海彼岸
run --demo-search "雨夜便利店"
run --data-report                         # 期望 history=2 first=雨夜便利店
run --demo-search "星海彼岸"               # 重复词
run --data-report                         # 期望仍 2 条、first=星海彼岸（MRU）
cat "$JM_UI_CONFIG/ui.ini"                # 期望 [search] recent=星海彼岸, 雨夜便利店
for i in $(seq 1 12); do run --demo-search "关键词$i" >/dev/null; done
run --data-report                         # 期望 history=10（上限截断，最新在前）
run --history-clear --data-report         # 期望 history=0
```

### 4.2 手工 UI（最贴近用户）

1. 启动应用 → 搜索页，确认显示「暂无搜索记录」+ 热门标签。
2. 输入 `关键词A` 回车 → 搜索结果出现；切回/重启应用 → 「最近搜索」出现 `关键词A`。
3. 再搜 `关键词B` → 顺序变成 `关键词B, 关键词A`；点 `关键词A` chip → 它回到队首。
4. 点「清空」→ 提示「已清空最近搜索」，列表变回占位文案；重启仍为空。
5. 覆盖升级（`adb install -r`）后重复 2–4。

### 4.3 Android / MuMu

```bash
adb install -r -t jm-ui-0.91-history.apk
adb shell "am start -n org.jmcomic.ui/org.qtproject.qt.android.bindings.QtActivity \
  --es jm.args '--history-clear --data-report'"                 # history=0
adb shell am force-stop org.jmcomic.ui
adb shell "am start ... --es jm.args '--demo-search 测试词甲'"
adb shell am force-stop org.jmcomic.ui
adb shell "am start ... --es jm.args '--demo-search 测试词乙 --data-report'"  # history=2
adb shell am force-stop org.jmcomic.ui
adb shell "am start ... --es jm.args '--data-report'"          # 仅重启 → 仍 2
adb install -r -t jm-ui-0.91-history.apk                       # 覆盖升级
adb shell "am start ... --es jm.args '--page search --data-report'"  # 仍 2
adb shell "su 0 grep -A2 '\[search\]' /data/user/0/org.jmcomic.ui/files/settings/ui.ini"
```

MuMu 抓图：`am start --display 2 … --es jm.args '--page search'` +
`screencap -a -p /sdcard/x`，按内容挑出 1080×2400 的浅色那一屏。

## 5. 验证结果

| 场景 | 期望 | 实测 |
|---|---|---|
| 空白配置 | 0 条、显示「暂无搜索记录」 | `history=0 first=-` |
| 搜 1 / 2 个词并重启 | 1 / 2 条，最新在前 | `history=1`、`history=2 first=雨夜便利店` |
| 重复搜索同一个词 | 条数不变、回到队首 | `history=2 first=星海彼岸`，`recent=星海彼岸, 雨夜便利店` |
| 连搜 12 个词 | 截断到 10 条 | `history=10 first=关键词12`，ini 里正好 10 项 |
| 清空 + 重启 | 0 条、`[search]` 键被删除 | `history=0 first=-` |
| MuMu 仅重启 | 记录保留 | `history=2 first=测试词乙` |
| MuMu `install -r` 覆盖升级 | 记录保留，其它设置不受影响 | `history=2`；`loggedIn=1 misaka18240`、`Noto Serif Hebrew 8pt`、`IP分流 191.222.247.114`、`themeNow=light_pink` 全部不变 |
| 布局回归 | 「最近搜索 / 清空」行高不变 | 空/非空均 `headerH=20 titleTextH=18 linkH=20` |
| 完整离线回归 | 全过 | `c1_ui_desktop_suite.py --only offline` → **94/94 PASS，FAIL 0，SKIP 0**（其中第 12 节新增 5 条搜索历史断言） |

截图：`docs/shots/fix_2026-11-08/search_hist_empty.png`（空状态）、
`search_hist_two.png`（桌面两条历史）、`mumu_search_hist_1.png`（MuMu 升级后）。

## 6. 检查方式速查

```bash
# 1) 看运行时是否真的从磁盘恢复
./build/jmcomic_ui.exe --page search --data-report --quiet --shot /tmp/a.png 2>&1 \
  | grep 'data-report: history='

# 2) 看落盘内容
grep -A2 '\[search\]' "$JM_UI_CONFIG/ui.ini"

# 3) 设备上看落盘内容
adb shell "su 0 grep -A2 '\[search\]' /data/user/0/org.jmcomic.ui/files/settings/ui.ini"

# 4) 看界面：搜索页「最近搜索」下应只有真实搜过的词，且「清空」可点
#    点完出现「已清空最近搜索」，重启不再出现。
```

## 7. 改动文件

| 文件 | 改动 |
|---|---|
| `cpp/shell/ui/search_history.h/.cpp` | 新增：持久化、MRU、去重、上限、清空、诊断 |
| `cpp/shell/ui/ui_pages_browse.cpp` | 搜索页接线：记录、重建 chips、清空、OnEnter |
| `cpp/shell/ui/ui_pages.h` | `SearchPage` 新增 `OnEnter` / `ClearHistory` / `ReloadHistory` 与成员 |
| `cpp/shell/ui/main_desktop.cpp` | `--history-clear`、`--data-report` 增 `history=`、Android 补 `--demo-search` |
| `cpp/shell/ui/CMakeLists.txt` | 加入新源文件 |
| `cpp/tools/c1_ui_desktop_suite.py` | 新增第 12 节 5 条搜索历史断言（可 `py cpp/tools/c1_ui_desktop_suite.py --only history,offline` 单独跑，约 40 秒） |

产物：`cpp/shell/ui/build-android-x86_64/jm-ui-0.91-history.apk`
（0.91 / vc91、x86_64、debug 签名、`zipalign -p 4` 校验通过）。
