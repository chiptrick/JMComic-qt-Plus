# C1-W8 验收报告

- 日期：2026-11-02
- 范围：C1（Qt C++ 壳 + 导航 + 本地阅读器 + 设置 + C++ 网络层）验收结论与发布建议
- 依据：`docs/c1_w7_report.md`、`android/build_logs/c1_w7_20261102-222611/`、Windows 侧构建门禁

---

## 1. 能不能发

**可以发 debug/内测包**（`JMComic-c1-0.2-arm64-v8a-debug.apk`，68MB）。
不建议作为正式版发布，原因只有一个：**翻页帧率尚未取得可信数据**（W7 §4），
其余验收项均通过或优于 Python 版。

| 门槛 | 结论 |
|---|---|
| 功能闭环（阅读器/设置/网络/直链） | ✅ 通过 |
| 冷启动不劣于 Python 版 | ✅ 189ms vs 211ms |
| 内存 ≤450MB | ✅ 206MB（Python 版 547MB） |
| 16KB 门禁 | ✅ 23 对齐 / 0 未对齐 |
| 直链（`libsr_qnn.so`） | ✅ DT_NEEDED 命中 |
| 崩溃 | ✅ 0 |
| 帧率 p95 ≥110fps | ⛔ 未取得（仪器不适用） |

## 2. 与 Python 版同机对照

同机（vivo V2463A / Android 17）、同测试口径、3 次冷启动取中位：

| 指标 | C1（C++） | Python 版 | 差异 |
|---|---|---|---|
| 冷启动 | **189ms** | 211ms | −10% |
| PSS | **206MB** | 547MB | −62% |
| APK 体积 | **68MB** | 176MB（0.88 arm64） | −61% |
| 启动方式 | Qt C++ 进程内 | Python + Kivy/PySide 运行时 | — |
| 网络 | C++ `NetClient`（Qt/OpenSSL） | `curl_cffi`（Python） | 指纹能力弱于 Python 版 |
| 依赖 SELinux | 无 | 需可执行私有目录（真机不可用） | C1 才能在 Android 17 上用 |

## 3. W7 发现的问题与处置

| # | 问题 | 处置 | 状态 |
|---|---|---|---|
| 1 | 帧率仪器不适用（gfxinfo 只覆盖 HWUI；SF --latency 无数据） | 计划在 C1 内加 `QWindow::frameSwapped` 打点 | 遗留 |
| 2 | vivo 上 `adb install` 偶发静默挂起 | 取证脚本支持 `--no-install`，装包手工执行 | 已缓解 |
| 3 | vivo 上 `am start` 紧跟 `force-stop` 偶发不生效 | 脚本加 `start_app`（`-W` + pid 校验重试） | 已修复 |
| 4 | 投图走 `/data/local/tmp` 中转失败（应用 uid 无法遍历） | 改 base64 命令行直写（md5 对拍） | 已修复 |
| 5 | `run-as pkg sh -c '...'` 引号被 adb 拆分 | 整串作为一个参数传给 `adb shell` | 已修复 |
| 6 | `grep -c \|\| echo 0` 产出两行，整数比较报错 | 改用 `head -1` + 默认值 | 已修复 |

## 4. 已知风险（发版前必须知情）

1. **TLS 指纹**：C++ 侧用 Qt/OpenSSL 默认 ClientHello，不等同浏览器的 JA3/JA4。
   若目标站点做指纹校验，需要 C3 阶段换 curl-impersonate 后端（接口不变）。
2. **帧率数据缺失**：无法证明"翻页 p95 ≥110fps"。若该项是硬门槛，必须补打点后再评。
3. **人工视觉项未填**：F2/F3/F5/F6/F7 的观感、S2/S3 的人工确认仍待执行（W7 §5）。
4. **构建依赖缓存**：Windows 构建需 `C:\Users\WQ159\jmcomic-build\deps` 内的
   `libs/<abi>` 与 `openssl/<abi>`（不入库，由 `build_windows.ps1` 校验存在）。
5. **OpenSSL 体积**：为满足 Qt 的 `libssl_3.so` 命名要求，APK 内含两套同内容别名（约 6MB）。

## 5. 复现与归档

| 交付物 | 路径 |
|---|---|
| APK | `android/dist/JMComic-c1-0.2-arm64-v8a-debug.apk`（68MB） |
| 构建脚本 | `android/c1_shell/build_windows.ps1`（Windows，无需 WSL） |
| 取证脚本 | `android/tools/c1_w7_regression.sh` |
| 原始日志 | `android/build_logs/c1_w7_20261102-222611/`（logcat / c1_log / meminfo / gfxinfo / report） |
| 网络层设计 | `docs/c1_net_layer.md` |
| W7 明细 | `docs/c1_w7_report.md` |

## 6. 决策建议

按方案 §9 的判据：

- 功能闭环 ✅、性能（可测项）✅ → **可以进入 C2（业务替换：列表/下载/登录/收藏）**；
- 但两条前置未清：
  1) 帧率打点（1~2 小时工程量，决定 P3 是否成立）；
  2) 人工 checklist 走一遍（半天，需人）。
- 建议顺序：**先补帧率打点 → 人工 checklist → 再开 C2**。C2 会引入大量列表/图片渲染，
  帧率基线不清楚时做 C2，等于把性能风险推到后面。

CI 恢复（方案 P5-5）仍未执行：workflow `378532840/841/843` 仍为 `disabled_manually`，
建议在 C2 开工前恢复，让后续改动重新受自动守门约束。
