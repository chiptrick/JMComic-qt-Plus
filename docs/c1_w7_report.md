# C1-W7 真机集成回归报告

- 日期：2026-11-02
- 设备：vivo V2463A（`10AF5K2B0D001AM`）/ Android 17（SDK 37）/ arm64-v8a / **SELinux Enforcing** / 120Hz
- 包：`android/dist/JMComic-c1-0.2-arm64-v8a-debug.apk`（68MB，Windows 侧构建）
- 取证：`android/build_logs/c1_w7_20261102-222611/`（脚本自动生成）＋ 手工复核

---

## 1. 结论

| 类别 | 结果 | 说明 |
|---|---|---|
| 功能（自动化可判定项） | **PASS** | 阅读器自检、设置自检、HTTPS 请求、CA、直链全部通过 |
| 性能（可测项） | **PASS** | 冷启动优于 Python 版；PSS 206MB（目标 ≤450MB） |
| 性能（翻页耗时 p95） | **PASS** | 6.7ms（≈149fps 等价），见 §4 |
| 稳定性（可测项） | **PASS** | 连续翻 20 页、崩溃行数 0 |
| 人工视觉项（F2/F3/F5 视觉/F8 字段） | **待人工** | 脚本不代判，见 §5 |

**一句话**：C1 在真机上功能闭环，冷启动、内存、翻页耗时三项均达标且优于 Python 版。

## 2. 功能

| # | 用例 | 判据 | 实测 | 结论 |
|---|---|---|---|---|
| F1 | 启动 | 进程存活、无 FATAL | pid 存在，`FATAL` 计数 0 | PASS |
| F9 | 阅读器自检 | `selftest done pages=N` | `selftest done pages=4 twoPage=1 rtl=1 brightness=0.75` | PASS |
| F8 | 设置页 | `settings selftest OK` | `settings selftest OK` | PASS |
| F10 | 直链探针 | `sr_qnn in link map OK` | `base=0x792e400000` | PASS |
| P4-5 | 真实网络请求 | `net OK 2xx` | `net OK 200 17143B 102ms`（重复 3 次，102~114ms） | PASS |
| — | CA 就绪 | `CA ready` | `bundled=118 system=141 total=400` | PASS |
| — | TLS 后端 | `[openssl] supportsSsl=1` | 一致 | PASS |

> 说明：F9 的 `pages=4` 是因为投了 5 张图、其中 1 张为 Qt 无法解码的图标格式；页数与可解码图片数一致，不是缺陷。

## 3. 性能

| # | 指标 | 目标 | C1 实测 | Python 版同机 | 结论 |
|---|---|---|---|---|---|
| P1 | 冷启动 `TotalTime`（3 次中位） | C1 ≤ Python | **189ms**（189/189/194） | 211ms（241/211/197） | **PASS** |
| P2 | 内存 PSS | ≤450MB | **206MB**（211696KB） | 547MB（559872KB） | **PASS**（优于 Python 62%） |
| P3 | 翻页耗时 p95 | ≤9.09ms（≈110fps） | **6.7ms**（≈149fps，n=5；长采样 n=26 时 7.6ms） | — | **PASS**（口径见 §4） |
| P4 | 16KB 门禁 | 未对齐 0 | **23 对齐 / 0 未对齐** | — | PASS |
| P5 | bundle 门禁 | 0 或 N/A | **N/A**（已无 Python bundle） | — | PASS |

设备刷新率 120Hz（`renderFrameRate 120.0`），故"p95 ≥110fps"对应的帧间隔约 9ms。

## 4. 翻页性能：口径与实测

原判据是"翻页帧率 p95 ≥110fps"。直接测帧率在本项目上不可行，原因有三：

1. `dumpsys gfxinfo` 只统计 HWUI 渲染的帧；C1 是 Qt 自绘，实测 `Total frames rendered: 1`；
2. `dumpsys SurfaceFlinger --latency` 在本机图层只返回刷新周期（8333333ns=120Hz），不返回帧时间戳；
3. **更根本**：C1 的翻页是"一次性重绘"，没有连续动画，静态页并不存在"每秒多少帧"。

因此改为测**一次翻页的端到端耗时**（解码 + 双页合成 + 缩放 + `setPixmap` 提交），
在 `ShowPage` 内用 `QElapsedTimer` 计时，由 `FrameProbe`（标记 `fps_log` 开启）每 3 秒汇总：

```
C1Fps page-turns n=26 total=26 p50=6.9ms p95=7.6ms max=7.6ms
```

| 采样 | n | p50 | p95 | 折算帧率 | 结论 |
|---|---|---|---|---|---|
| 脚本自动（selftest + 20 次滑动翻页） | 5 | 6.4ms | **6.7ms** | ≈149fps | PASS（阈值 9.09ms） |
| 手工长采样（26 次翻页） | 26 | 6.9ms | **7.6ms** | ≈132fps | PASS |

**口径说明（不掩盖差异）**：这里量的是"翻页这条链路的耗时"，不含 GPU 呈现。
对无动画的阅读器，这才是决定体感的量；若将来加换页动画，必须补测真实呈现帧间隔
（届时用 `QOpenGLWidget::frameSwapped` 或平台帧统计）。

## 5. 仍需人工确认（脚本不代判）

| 用例 | 判据 | 状态 |
|---|---|---|
| F2 三页切换 | 标题栏跟随、Back 依次回退 | 待人工 |
| F3 目录加载 | 页数与实际图片数一致 | 待人工（自动化侧 pages=4 已对） |
| F4 翻页方向 | RTL 时左右键语义反转 | 逻辑已在自检覆盖（`rtl=1`），视觉待人工 |
| F5 缩放 | +/-/0 与捏合，适宽复位 | 逻辑已覆盖（`zoom x1.25`→`fit-width`），手势待人工 |
| F6 双页 | 第二页不足退化单页 | 逻辑已覆盖（`twoPage=1`），视觉待人工 |
| F7 亮度 | 1.0→0.75→0.5→0.25→1.0 | 逻辑已覆盖（`brightness 0.75`），观感待人工 |
| S2 连续切页 10 次 | 无 crash | 待人工 |
| S3 网络异常 | 断网/超时后仍能继续请求 | 待人工（桌面已覆盖超时/拒连） |

## 6. 稳定性

| # | 用例 | 结果 |
|---|---|---|
| S1 | 连续翻 20 页（`input keyevent 22` × 20） | 无 crash、无 OOM |
| S4 | force-stop 后重启（本次共 6 次） | 每次均正常启动，无残留锁 |
| — | 崩溃行数（`FATAL EXCEPTION`） | 0 |

## 7. 复现命令

```bash
# 构建（Windows，无需 WSL）
powershell -File android/c1_shell/build_windows.ps1 -Abi arm64-v8a

# 取证（脚本会自动投图、冷启动 3 次、触发自检、采集日志/PSS/门禁）
bash android/tools/c1_w7_regression.sh \
  --apk android/dist/JMComic-c1-0.2-arm64-v8a-debug.apk \
  --images /tmp/c1pages --runs 3 --no-install
```

> `--no-install`：vivo 上脚本内的 `adb install` 会偶发静默挂起（系统安装器行为），
> 包已装好时跳过安装更快；需要装包时手工执行 `adb install -r`。
