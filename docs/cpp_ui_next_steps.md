# C++ 版后续执行计划（QUIC / HTTP-3 及遗留项）

> 编写时间：2026-11-07 17:34  
> 适用范围：`cpp/` 桌面壳（Windows / MinGW + Qt 6.12）与 Android 两个 ABI  
> 依据：`docs/cpp_tls_quic_ech_design.md` §10、`docs/c1_ui_remaining_work.md` 第十轮与"未闭环"段

## 0. 使用说明

- **状态图例**：`待办` / `进行中` / `待定`（需先决策）/ `阻塞`（缺外部依赖）。
- **优先级**：P0 = 影响可用性或交付完整性；P1 = 能力补齐；P2 = 优化与长期治理。
- 每项统一四段：**目标 / 具体步骤 / 所需输入 / 产出与验收**；不确定的先写成判定实验，再决定实现。
- 相对路径均以仓库根 `C:/Users/WQ159/source/repos/JMComic-qt` 为基准。

## 1. 里程碑总览

| ID | 主题                            | 优先级 | 依赖    | 当前状态         |
| -- | ----------------------------- | --- | ----- | ------------ |
| D1 | 桌面部署补齐 OpenSSL 4.x 运行时        | P0  | 无     | 完成（0.89）     |
| D2 | 回归套件分层（离线/联网）                 | P0  | 无     | 完成（0.89）     |
| D3 | 版本与变更记录收口                     | P1  | D1/D2 | 完成（0.89）     |
| Q1 | QUIC 0-RTT 与票据复用              | P1  | Q2    | 完成（0.89，首个请求限流） |
| Q2 | HTTP/3 连接池与多路复用               | P1  | 无     | 完成（0.89）     |
| Q3 | QUIC 首包耗时优化                   | P1  | Q2    | 部分（埋点/qlog/Alt-Svc 完成，调参实验待真机数据） |
| Q4 | ECH-over-QUIC 失败归因            | P1  | 无     | 阻塞（需实验环境与产品决策） |
| Q5 | 回退与降级的可观测性                    | P2  | D2    | 完成（0.89）     |
| A1 | OpenSSL 4.x 的 Android 交叉编译    | P1  | 外部源码  | 完成（0.89，Linux 宿主） |
| A2 | ngtcp2/nghttp3 的 Android 交叉编译 | P1  | A1    | 完成（0.89）     |
| A3 | APK 打包、体积与 16 KB 对齐           | P1  | A2    | 完成（0.89，Linux 出包 + 部署） |
| A4 | Android 端 H3/ECH 行为验证         | P1  | A3    | 完成（真机 H3 + ECH 均实测通过） |
| M1 | 真机 V2463A 全链路复测               | P0  | 真机    | 部分（18/20；返回键 2 项失败，见 §13.4） |
| M2 | 模拟器首页封面占位图定性                  | P2  | 可用节点  | 可执行（AVD 已可用，缺节点做三路由对照） |
| T1 | 依赖治理（可复现构建）                   | P2  | A1/A2 | 完成（0.89）     |
| T2 | TLS/QUIC 安全复核                 | P1  | 无     | 完成（0.89，修 2 个缺陷） |

**建议执行顺序**：D1 → M1 → D2 → Q2 → Q3 →（Q4 判定）→ T2 → A1/A2/A3/A4 → Q1 → Q5 → T1 → D3 → M2。  
理由：D1 是"交付即可用"的硬前提；M1 决定移动端是否还有系统性回归；D2 之后所有新增用例才有稳定的判定基线；Q2/Q3 依赖同一处连接生命周期代码，合并实施成本最低；A1 受外部源码可得性约束，可与 Q2/Q3 并行准备。

---

## 2. 阶段一：交付收尾

### D1 桌面部署补齐 OpenSSL 4.x 运行时（P0）

**目标**：`build_win.bat deploy` 产出的 `dist/` 目录可直接运行，不因缺少 `libssl-4-x64.dll` / `libcrypto-4-x64.dll` 而启动失败。

**背景（已核实）**：CMake 的 POST_BUILD 只把 DLL 复制到**构建目录**（`cpp/shell/ui/build/`），而 `deploy` 分支先 `rmdir dist` 再 `windeployqt`，并未复制这两个 DLL。当前 `dist/` 缺失运行时依赖。

**具体步骤**

1. 在 `cpp/shell/ui/build_win.bat` 的 `deploy` 分支中，`windeployqt` 之后增加从 `..\..\..\jmcomic-build\deps\openssl41-mingw\bin`（或 `JM_OPENSSL41_ROOT`）复制两个 DLL 到 `%DIST%`。
2. 用 `dumpbin /dependents`（或 MinGW `objdump -p`）确认 `jmcomic_ui.exe` 对 `libssl-4-x64.dll`、`libcrypto-4-x64.dll` 的隐式依赖，确保清单与实际一致。
3. 在 `dist/` 内直接双击 exe 做一次冷启动验证（不带 PATH 注入）。
4. 若 `JM_WITH_QUIC=OFF` 构建，deploy 分支应跳过复制，不得因缺文件报错。

**所需输入**：`JM_OPENSSL41_ROOT`（当前 `C:/Users/WQ159/jmcomic-build/deps/openssl41-mingw`）；`windeployqt` 可用的 Qt 安装。

**产出与验收**

- 修改后的 `build_win.bat`；`dist/` 内包含 exe + 两个 OpenSSL 4 DLL。
- 验收：干净目录冷启动成功；`--advanced-status` 输出 `quic=1 http3=1`；日志无 `The code execution cannot proceed` 类加载错误。

**风险**：`dist/` 同时被 Qt 的 OpenSSL 3.x 依赖使用（Qt TLS 后端走 `libssl-3-x64.dll`），两套 DLL 并存需确认无命名冲突（4.x 文件名带 `-4-`，实测可共存）。

### D2 回归套件分层：离线确定性用例与联网用例（P0）

**目标**：`c1_ui_desktop_suite.py` 的判定基线稳定，网络波动只影响"联网组"，不污染整体结论。

**背景（已核实）**：`desktop_2026-11-07_h3_final.log` 为 PASS 87 / FAIL 0 / SKIP 4，4 个 skip 全部是联网用例（真实搜索、HTTP/3 实测、ECH 握手、DoH 作用于流量）。同一构建在 `--only proxy` 下 H3 探针 466 ms 通过，说明是链路波动而非代码回归。

**具体步骤**

1. 给用例打标签：`offline`（自检、DNS wire 解析、配置默认值、路由优先级、页面渲染）与 `net`（真实接口、测速、探针实连）。
2. 增加 `--only offline` / `--only net` 两个入口；默认仍跑全量，但汇总输出拆成两行统计（`offline PASS x/y`、`net PASS x/y / SKIP n`）。
3. 联网用例失败时区分"网络不可达"与"断言不成立"：前者记 SKIP 并在汇总单列，后者必须 FAIL。
4. 修正已发现的脚本缺陷：H3 用例复用 `err` 变量会污染后续 `dohquery` 断言（已修，需加注释防回归）；`advanced_selftest` 的 `--shot-delay` 从 900 ms 提到 2000 ms；线路测速窗口 95 s。

**所需输入**：现有套件与历次日志（`docs/test_logs/desktop_2026-11-07*.log`）。

**产出与验收**

|   |   |
| - | - |

- 套件新增 `--only offline|net` 与分段汇总。
- 验收：断网状态下 `--only offline` 全通过；联网正常时 `--only net` 无 SKIP。

**风险**：把"接口慢"误判成"失败"会让套件失去意义，需显式设置每个联网用例的超时上限并在超时文案里打印耗时。

### D3 版本与变更记录收口（P1）

**目标**：对外交付物（APK、exe、文档）的版本与能力描述一致。

**具体步骤**

1. `cpp/shell/ui/CMakeLists.txt` 的 `project(... VERSION 0.88)`、`QT_ANDROID_VERSION_NAME`、APK 文件名三者对齐（本轮曾出现临时用 0.89 命名又回退的情况）。
2. 在 `docs/cpp_ui_parity.md` 的版本小节补一行"已支持：DoH / ECH(TCP) / HTTP-3(Windows)"。
3. 根 `CHANGELOG` 是上游 Python 版日志（作者 tonquer），**不改动**；C++ 壳的变更记录统一写在 `docs/`。

**所需输入**：最终 APK 与 exe。

**产出与验收**：版本号三处一致；文档描述与 `--advanced-status` 实际输出一致。

---

## 3. 阶段二：QUIC / HTTP-3 收尾

### Q1 QUIC 0-RTT 与会话票据复用（P1，待定）

**目标**：复用 TLS 会话票据，使后续连接在 0-RTT 内发出首个请求，省掉一次完整握手往返。

**待定前提**：是否值得做取决于 Q3 的测量结论。若 QUIC 首包耗时主要来自链路 RTT，0-RTT 收益明显；若主要来自本机处理或服务端排队，则收益有限。**Q3 完成后重新评估本项优先级。**

**具体步骤**

1. `SSL_CTX_set_session_cache_mode(SSL_SESS_CACHE_CLIENT)`，通过 `new_session_cb` 把票据落盘（按 host 分文件，如 `files/state/jmcomic-qt/h3/<host>.pem`）。
2. 新连接创建后 `SSL_set_session(ssl, session)`；当 `SSL_SESSION_get_max_early_data(session) > 0` 时调 `SSL_set_quic_tls_early_data_enabled(ssl, 1)`，并在 ngtcp2 侧启用 `NGTCP2_CONN_FLAG_EARLY_DATA`。
3. 握手后用 `ngtcp2_conn_get_early_data_status()` / `SSL_get_early_data_status()` 判定：`ACCEPTED` 直接复用已发送的请求；`REJECTED` 丢弃 0-RTT 数据并在 1-RTT 重发。
4. 仅对幂等 `GET` 启用（当前 H3 路径只服务 GET，天然满足）；POST 一律禁用。
5. 票据过期、证书变更、ECH 配置变更（`config_id` 变化）时主动作废票据。

**所需输入**：OpenSSL 4.1 session API、ngtcp2 early-data 相关标志、`cpp/shell/quic/quic_http.cpp`。

**产出与验收**

- `h3-timing` 日志中标注 `0-RTT accepted/rejected`。
- 验收：第二次连接首字节明显早于首次（约少 1 个 RTT）；被拒绝时请求仍成功且无重复提交。

**风险**：0-RTT 数据可被重放，服务端需 anti-replay 支持（Cloudflare 支持度**待验证**）；票据落盘涉及隐私与过期管理，需明确清理策略。

### Q2 HTTP/3 连接池与多路复用（P1）

**目标**：把当前"一请求一连接"改为按 `(host, port)` 复用连接、在同一 QUIC 连接上并发多个请求流，降低连续请求（页图、封面、翻页）的首包成本。

**现状**：`quic::Client` 单请求单连接，响应结束即 `ngtcp2_conn_del`（`cpp/shell/quic/quic_http.cpp`）。

**具体步骤**

1. 把 `Impl` 拆为 `Connection`（`ngtcp2_conn` + `nghttp3_conn` + `QUdpSocket` + 计时器）与 `RequestJob`（stream_id、响应累积、回调）。
2. `nghttp3_conn_submit_request` 支持多流并发；`recv_stream_data` 按 `stream_id` 分发到对应 `RequestJob`。
3. 池化键：`host + port + echConfig 指纹`；空闲超时用 `ngtcp2_transport_params.max_idle_timeout`，与 ngtcp2 的 `handle_expiry` 一致。
4. 连接级失败（`read_pkt` 错误、`CONNECTION_CLOSE`）时，把该连接上所有在飞请求回调失败，由 `NodeHttp` 侧统一回退 TLS。
5. 上限与背压：单连接并发流上限、全局连接数上限（建议 2/域名），超限排队。
6. 关闭时机：最后一个请求完成后进入 idle 计时，超时或应用退出时发 `CONNECTION_CLOSE`。

**所需输入**：ngtcp2 连接生命周期 API（`ngtcp2_conn_open_bidi_stream`、`ngtcp2_conn_shutdown_stream_*`、`ngtcp2_conn_write_connection_close`）。

**产出与验收**

- 新增连接池实现与开关（建议 `proxy/http3Pool`，默认开）。
- 验收：连续 10 次同域请求只发生 1 次 QUIC 握手（日志可数）；并发 3 个请求互不阻塞；任一请求超时不拖垮整条连接。

**风险**：连接复用会把"单请求失败"放大成"连接级失败"，必须实现"失败只影响该流"的隔离；另外多流共享拥塞窗口，页图大流量可能挤压 API 小请求，需按流优先级或限流。

### Q3 QUIC 首包耗时优化（P1）

**目标**：将 H3 首包从实测的 0.5~17 s（波动大）收敛到稳定区间，并让"是否值得用 H3"有可量化依据。

**现状数据**：无 ECH 时 371~~6663 ms 波动；带 ECH 时 7~~16 s；应用内首次尝试曾因 12 s 整体超时而回退。

**具体步骤**

1. 埋点分段计时：DoH 解析、UDP 建连、QUIC 握手（Initial→Handshake→1-RTT）、首字节。输出一行 `h3-timing` 日志。
2. 打开 ngtcp2 qlog（`settings.qlog_write` 与环境变量开关）复现握手期 PTO 次数，判断丢包来自本机还是链路。
3. 参数调优实验：`max_tx_udp_payload_size`、`no_pmtud`、初始拥塞窗口、`initial_rtt` 初值。
4. **Alt-Svc 学习**：解析 HTTP/1.1 响应头 `alt-svc: h3=":443"`，命中后才走 H3，避免对不支持 H3 的域名做无谓尝试。
5. H3 预算策略回归：当前 `min(请求超时, 8s)`（`node_http.cpp`），用新数据决定是否调整。

**所需输入**：`JM_H3_VERBOSE=1`、qlog 查看工具（qvis 或文本解析）。

**产出与验收**

- `h3-timing` 日志与一份对比表（开启/关闭 ECH、不同域名）。
- 验收：连续 20 次请求的 P50/P95 首包时间有记录；不以"偶发 17 s"作为结论。

**风险**：qlog 会显著增大日志量，必须默认关闭；Alt-Svc 缓存需要持久化，否则每次冷启动都要重新探测（可先做进程内缓存）。

### Q4 ECH-over-QUIC 失败归因（P1，待定 → 需实验定性）

> **本节是 0.89/0.90 上半场的历史计划，结论已由 §14.7 收敛**：用户拍板
> 「隐藏 SNI 优先」—— H3 默认开、恢复组合尝试、被拒回退 TCP+ECH、
> 下次启动/换节点重试。下面的四组实验不再是前置条件，保留作归因留档。

**目标**：判定 `SSL_ech_get1_status=FAILED` 的成因是"服务端策略拒绝"还是"客户端未真正发出 ECH 扩展"，据此决定是否恢复 QUIC 侧 ECH。

**已知事实**：同一份 71 B ECHConfigList，TCP 通道 `SUCCESS`，QUIC 通道 `FAILED` 且首包显著变慢；`NodeHttp` 的 H3 分支目前**不注入** ECH。

**判定实验（按顺序做，任一有结论即可停止）**

1. **扩展是否发出**：在 QUIC 客户端设置 keylog（`SSL_CTX_set_keylog_callback`）或抓 `CRYPTO` 帧明文字节，检查 ClientHello 是否含 `encrypted_client_hello (0xfe0d)`。
2. **换服务端**：对另一个支持 ECH 的站点重复 `--h3-probe <host> --h3-ech`，观察状态是否仍为 FAILED。
3. **GREASE 对照**：用 `SSL_ech_set1_grease_suite` 走 GREASE 路径，若状态为 `GREASE` 说明扩展能发出但真实 config 被拒。
4. **外层参数组合**：outer ALPN 取 `h2,http/1.1`（当前实现）与仅 `h3` 两版对比状态码与耗时。

**所需输入**：`--h3-probe <host> --h3-ech`（排障日志用环境变量 `JM_H3_VERBOSE=1`）、可访问的 ECH 站点。

**产出与验收**

- 一份结论：`客户端缺陷`（→ 修复后恢复注入）或 `服务端策略`（→ 保持 TCP-only，并在文档与 UI 文案写清）。
- 验收：结论有可复现命令与日志片段支撑，不依赖推测。

**风险（待定项）**：若结论是"服务端策略"，则 QUIC+ECH 组合在 Cloudflare 系站点长期不可用；若用户主要目标是隐藏 SNI，需要额外方案（如仍走 TCP+ECH 的 h2 通道），这会削弱 H3 的收益，**需要产品侧决策**。

### Q5 回退与降级的可观测性（P2）

**目标**：H3 失败、ECH 降级、HTTP/2 回退等事件在 UI 与自检里可见、可统计，而不是只留在日志里。

**具体步骤**

1. 在 `ProxyConfig` 增加轻量计数器（H3 成功/失败/回退、ECH SUCCESS/FAILED），进程内保存，页面「高级」区展示一行摘要。
2. `--proxy-selftest` 增加输出：当前会话的 H3/ECH 计数。
3. 页面文案与 `NetBackend::Summary()` 共用同一数据源，避免两处不一致。

**所需输入**：现有 `AdvancedStatus()`、`net_backend.cpp`。

**产出与验收**：页面可见"N 次 H3 成功 / M 次回退"；自检输出同数字。

**风险**：计数需线程安全（当前 QUIC 回调都在主线程，风险低但应在实现中注明前提）。

---

## 4. 阶段三：Android 侧的 QUIC（当前阻塞）

### A1 OpenSSL 4.x 的 Android 交叉编译（P1，阻塞）

**目标**：得到 `arm64-v8a` 与 `x86_64` 的 `libssl.so` / `libcrypto.so`，且**导出** `SSL_set_quic_tls_cbs`、`SSL_set1_ech_config_list`、`SSL_ech_set1_server_names`。

**现状（已核实）**：仓库内 Android 依赖是 OpenSSL 3.3.1（`no-ech`，无 QUIC-TLS）；`C:/Program Files/OpenSSL-4.1` 只有 Windows x64 产物，本机无 4.1 源码。

**具体步骤**

1. 取得 OpenSSL 4.1 源码（或提供方给的 Android 预编译包）。
2. 用 NDK r30d 交叉编译：`--target=android-arm64`/`android-x86_64`，显式开启 `enable-ech` 与 QUIC 相关选项。  
   |  |  |  |  
   | --- | --- | --- |
3. 产物验收：`nm -D libssl.so | grep -E "SSL_set_quic_tls_cbs|SSL_set1_ech_config_list"` 有输出。
4. 放入 `deps/openssl41-android/<abi>/`，与 Windows 版目录结构对齐。

**所需输入**：OpenSSL 4.1 源码或预编译包；NDK r30d（已装）；`perl`/`make` 构建环境。

**产出与验收**：两个 ABI 的 `.so` + 头文件；符号检查通过。

**风险/待定**：**源码可得性未定**。若只能拿到 Windows 产物，A1~A4 整体阻塞，Android 维持 HTTP/1.1（现有行为，不影响可用性）。另外 `enable-ech` 是否与 NDK 的 BoringSSL 冲突需验证。

### A2 ngtcp2 / nghttp3 的 Android 交叉编译（P1，阻塞于 A1）

**目标**：产出两个 ABI 的 `libngtcp2.a`、`libngtcp2_crypto_ossl.a`、`libnghttp3.a`。

**具体步骤**

1. 复用桌面版构建命令，改 `CMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake`、`ANDROID_ABI`、`ANDROID_PLATFORM=android-34`。
2. `ENABLE_OPENSSL=ON` 指向 A1 产物；确认 `Looking for SSL_set_quic_tls_cbs - found`。
3. 交叉编译时不要加 `-mavx2`（x86_64 Android 模拟器未必支持；arm64 无此选项）。
4. 符号检查：`llvm-nm` 确认三个 `.a` 的目标架构正确。

**所需输入**：A1 产物、NDK r30d、CMake 4.2。

**产出与验收**：`deps/quic-android/<abi>/lib/*.a`；`file` 显示 ARM aarch64 / x86-64。

### A3 APK 打包、体积与 16 KB 对齐（P1）

**目标**：`JM_WITH_QUIC=ON` 的 Android 构建可产出可安装 APK，且通过现有的 16 KB 页对齐门禁。

**具体步骤**

1. CMake 放开 Android 的 `JM_WITH_QUIC` 开关（当前强制 OFF），链接 A2 的三个 `.a` 与 A1 的 `.so`。
2. 处理 OpenSSL `.so` 的打包：按现有 `UI_ANDROID_EXTRA_LIBS` 机制放进 APK，注意 Qt 侧的 `libssl_3.so` 命名约定与 4.x 的 `libssl.so` 不冲突。
3. `QT_ANDROID_EXTRA_LIBS` 中加入 OpenSSL 4 的 `.so`；确认 `androiddeployqt` 把 `lib/arm64-v8a/` 与 `lib/x86_64/` 都带上。
4. 体积评估：记录加入 QUIC 前后每个 ABI 的 APK 大小与解压后 `lib/` 体积。

**所需输入**：A2 产物、现有签名脚本、`cpp/tools/check_ui_apk_16kb.py`。

**产出与验收**：两个 ABI 的 APK；`check_ui_apk_16kb.py` 全部通过；`--advanced-status` 在 Android 上显示 `quic=1 http3=1`。

**风险**：APK 体积预计增加明显（OpenSSL 4 + ngtcp2/nghttp3 静态库），若超出分发预期，需评估"按需下载/仅 arm64 内置"的折中方案（**待定**）。


### A4 Android 端 H3/ECH 行为验证（P1）

**目标**：确认 Android 上 H3 真实可用、失败可回退，且与桌面行为一致。

**具体步骤**

1. MuMu（x86_64）用 `--h3-probe` 验证握手与请求。
2. 页面将 `proxy/http3` 打开，验证「高级」区文案从 `已回退 HTTP/2` 变为 `开（QUIC/HTTP3…）`。
3. 关闭 UDP（模拟器断网/DROP 规则）验证回退到 HTTP/1.1 且业务不失败。
4. 记录 Android 上的 ech 状态（预期与桌面一致或更差，取决于 A1 是否含 ECH）。

**所需输入**：A3 的 APK、MuMu（`127.0.0.1:16384`）、`MSYS_NO_PATHCONV=1` 的 adb 调用方式。

**产出与验收**：截图 + 日志；断 UDP 场景下业务仍成功。

---

## 5. 阶段四：移动端整体验证

### M1 真机 V2463A 全链路复测（P0）

**目标**：补齐此前缺失的真机验证（历史记录中真机曾从 adb 掉线，页面点击与节点链路未复测）。

**具体步骤**

1. 安装 `cpp/dist/JMComic-ui-0.88-arm64-v8a-debug.apk`，确认 `native-code` 只有 `arm64-v8a`。
2. 走查：首页加载 → 搜索 → 详情 → 阅读器翻页（含 GPU 覆盖层换页动画） → 分流页（线路/节点/测速） → 高级区（DoH/ECH/HTTP3 开关）。
3. 记录 `proxy-apply`、`node-test`、`line-test` 日志；确认页面与 Python 版逐项一致。
4. 长按/返回手势、亮度和 RTL 模式各抽一项验证。

**所需输入**：arm64 APK、真机、`adb`。

**产出与验收**：真机日志与截图；与桌面/模拟器结论一致的对照表。

**风险**：真机 adb 稳定性差（此前掉线），建议先固定 USB 连接再批量操作；若仍掉线，改用 `adb over Wi-Fi` 并记录环境问题。

### M2 模拟器首页封面占位图定性（P2，待定）

**目标**：判定"首页封面显示占位图"是环境带宽问题还是代码缺陷。

**现状**：`real-data` 为空时回落到演示数据，占位图属正常；但 real-data 成功那次封面请求 60 s 内未完成，未拿到 `cover http failed` 就结束观察，**尚未定性**。

**具体步骤**

1. 增加封面请求的结果日志（成功耗时 / 失败原因 / 是否 304）。
2. 分别用直连、DoH、节点三种路由各跑一次，记录 CDN 首字节时间。
3. 若失败集中在某一路由，按 `cover_cache.cpp` 的错误分支定位；若三路都慢，判定为环境带宽。

**所需输入**：`docs/shots/mumu_*.png`、`cover http` 日志。

**产出与验收**：一份结论 + 复现命令；若为代码问题，转入修复任务。

**风险（待定）**：图片 CDN 的连通性波动大，可能多次实验仍无法二分，此时应接受"环境因素"结论并记录判定依据。

---

## 6. 阶段五：质量与长期维护

### T1 依赖治理：让 QUIC 依赖可复现构建（P2）

**目标**：任何人按文档能重建 `deps/quic-mingw`，而不是依赖当前机器上的手工产物。

**具体步骤**

1. 新增 `cpp/shell/quic/tools/build_quic_deps.ps1`（或 `.sh`）：顺序执行 gendef/dlltool → nghttp3 → ngtcp2，参数化为 `--openssl-root`、`--prefix`、`--avx2`。
2. 在脚本内做构建后校验：库文件存在、`Looking for SSL_set_quic_tls_cbs - found`、符号检查。
3. 记录版本与校验和（`ngtcp2 1.26.90`、`nghttp3 1.19.90`、`OpenSSL 4.1.0`）到 `docs/`。
4. 说明 `-mavx2` 的取舍：仅桌面 x86_64 使用；交叉编译与 arm64 不加。

**所需输入**：`C:/Users/WQ159/ngtcp2`、`C:/Users/WQ159/nghttp3` 源码。

**产出与验收**：一条命令从零构建成功；CI 或新机器可复现。

**风险**：OpenSSL 4.1 只有 DLL 是前提，若上游改文件名（`libssl-4-x64.dll`）脚本需同步。

### T2 TLS / QUIC 安全复核（P1）

**目标**：确认 ECH 与 H3 的引入没有削弱既有安全属性。

**核查点**

1. **证书错误不得被回退掩盖**：H3 因证书失败时，`NodeHttp` 的回退路径必须同样失败（不能出现"H3 证书错误 → 回退后成功"）。需专门用例。
2. **主机名校验**：QUIC 通道使用 `SSL_set1_host`；节点覆盖场景下必须用域名而非 IP（与 `NodeHttp` 的 SNI 语义一致）。
3. **CA 来源**：`JM_CA_BUNDLE` → Git `ca-bundle.crt` → 默认路径的顺序必须有日志，便于排障时判断信任链来源。
4. **0-RTT 重放**（若实施 Q1）：仅对幂等 GET 使用；拒绝时重发，不做静默降级。
5. **UDP 明文暴露面**：QUIC 头部的 DCID/包号不可加密，确认没有把敏感信息放进自定义 CID 或 token。

**所需输入**：现有 `openssl_tls.cpp`、`quic_http.cpp`、`node_http.cpp`。

**产出与验收**：一份核查表（逐项：结论 + 证据）；发现的问题转为修复任务。

---

## 7. 待定与风险登记

| 编号 | 事项                                | 类型      | 影响                 | 判定方式           | 缓解                          |
| -- | --------------------------------- | ------- | ------------------ | -------------- | --------------------------- |
| R1 | OpenSSL 4.1 的 Android 源码/预编译包是否可得 | 待定（外部）  | A1~A4 全部阻塞         | 询问提供方或检查镜像     | Android 维持 HTTP/1.1；不影响现有功能 |
| R2 | ECH-over-QUIC 是客户端缺陷还是服务端策略       | 待定（实验）  | 决定是否在 QUIC 侧启用 ECH | Q4 的四组实验       | 保持 TCP-only 并如实文档化          |
| R3 | QUIC 首包 0.5~17 s 的波动来源            | 待定（测量）  | 影响 H3 是否默认开启       | Q3 分段计时 + qlog | H3 预算限制在 8 s，超时即回退          |
| R4 | 连接池引入后单流失败是否放大为连接失败               | 风险（设计）  | 可能反而降低可用性          | Q2 的并发/超时用例    | 流级隔离 + 失败只回退单请求             |
| R5 | APK 体积增加是否可接受                     | 待定（产品）  | 影响分发渠道             | A3 的体积测量       | 仅 arm64 内置 / 功能开关裁剪         |
| R6 | 真机 adb 稳定性                        | 风险（环境）  | M1 可能中途失败          | 复测时观察          | 改用 Wi-Fi adb，分段执行           |
| R7 | 封面占位图成因                           | 待定（未定性） | 影响首页观感             | M2 的三路由对照      | 若为环境因素则记录结论不再追踪             |
| R8 | 联网用例的 SKIP 是否被误读为"全部通过"           | 风险（流程）  | 掩盖真实回归             | D2 的分段汇总       | 汇总单列 SKIP 数量与原因             |

---

## 8. 附：常用命令与路径

```powershell
# 桌面构建（含 QUIC）
cd cpp/shell/ui
cmd /c build_win.bat

# HTTP/3 探针（可加 --h3-ech 复验 ECH-over-QUIC）
./build/jmcomic_ui.exe --page home --h3-probe cloudflare.com --h3-path / --shot out.png --shot-delay 20000

# 能力自检 / 页面状态
./build/jmcomic_ui.exe --page home --proxy-selftest --shot s.png --shot-delay 2000
./build/jmcomic_ui.exe --page home --advanced-status --shot s.png --shot-delay 1000

# 回归
py cpp/tools/c1_ui_desktop_suite.py --only proxy
py cpp/tools/c1_ui_desktop_suite.py            # 全量

# Android
"C:/Qt/Tools/Ninja/ninja.exe" -C cpp/shell/ui/build-android-arm64-v8a
py cpp/tools/check_ui_apk_16kb.py
```

| 用途          | 路径                                                               |
| ----------- | ---------------------------------------------------------------- |
| QUIC 客户端实现  | `cpp/shell/quic/quic_http.{h,cpp}`                               |
| H3 路由与回退    | `cpp/shell/node_http.cpp`（`StartQuic`）                           |
| 能力探测        | `cpp/shell/net_backend.cpp`（`Quic()`）                            |
| ECH（TCP 通道） | `cpp/shell/tls/openssl_tls.cpp`                                  |
| 依赖产物        | `C:/Users/WQ159/jmcomic-build/deps/{openssl41-mingw,quic-mingw}` |
| 设计文档        | `docs/cpp_tls_quic_ech_design.md` §10                            |

---

## 9. 本轮执行结果（0.89，2026-11-07 晚）

代码入口：`cpp/shell/quic/quic_http.{h,cpp}`（Q1/Q2/Q3）、`cpp/shell/node_http.cpp`（Q3/Q5/T2）、
`cpp/shell/ui/proxy_config.{h,cpp}`（Q5）、`cpp/shell/ui/build_win.bat`（D1）、
`cpp/tools/c1_ui_desktop_suite.py`（D2）、`cpp/shell/quic/tools/build_quic_deps.ps1`（T1）。

### D1 完成

- `deploy` 分支在 `windeployqt` 之后复制 `libssl-4-x64.dll` / `libcrypto-4-x64.dll`；
  `JM_WITH_QUIC=OFF` 时整段跳过（分支已实现，**未实跑**，见 §10）。
- 顺带修掉一个原有缺陷：`DIST` 原先在 `if (...)` 块内 `set`，cmd 的块级展开让它为空，
  `windeployqt` 收到的是 `\jmcomic_ui.exe`，deploy 必然失败。已把 `set DIST` 移到块外。
- 证据：`[ok] OpenSSL 4.x runtime copied`、`[ok] implicit OpenSSL 4.x imports confirmed`；
  干净 PATH 冷启动 `dist/jmcomic_ui.exe --advanced-status` →
  `tls=openssl-ech/4.1.0 ech=1 echLib=1 quic=1 http3=1 stack=ngtcp2+nghttp3`。

### D2 完成

- 用例分 `offline` / `net` 两层；`--only` 同时接受章节标签与层标签。
- 汇总两行：`offline PASS x/y`、`net PASS x/y  SKIP n`；SKIP 单独列出（不再混进 PASS）。
- 联网用例改为 `NetCheck(ok, name, detail, reachable)`：拿不到可判定输出才 SKIP，
  有输出而断言不成立一律 FAIL。
- 实测：`--only offline` 76/77（唯一失败是断言行含中文导致解码失配，已改 ASCII），
  `--only advanced,net` 9/9 PASS / 0 SKIP。

### D3 完成

- `project(VERSION 0.89)`、`QT_ANDROID_VERSION_CODE 89` / `NAME "0.89"`、
  `build_android.ps1 -VersionName '0.89'` 三处一致；APK 名 `JMComic-ui-0.89-<abi>-debug.apk`。
- `docs/cpp_ui_parity.md` 新增 §26（版本 + 能力行）；根 `CHANGELOG` 未改。

### Q2 完成

- `quic::Client` 内部拆成 `Connection`（socket/TLS/ngtcp2/nghttp3/计时器）与 `Job`（一条 bidi 流）。
- 池化键 `host|port|echConfig指纹`；单连接 32 流、同域 2 连接、全局 24 连接，超限排队。
- 连接级失败只让该连接上的在飞请求失败（`FailAll`），逐请求由 `NodeHttp` 回退。
- 空闲 15s 后发 `CONNECTION_CLOSE`；`quic::ShutdownPool()` 供退出/测试收口。
- 证据：`--h3-burst 10` → `ok=10 handshakes=1 reused=10 conns=1 jobs=0 queued=0`。

### Q1 完成（按"首个请求"限流）

- 票据落盘 `<state>/h3/<host>.pem` + `.meta`（`exp` / `ech` / `cert` / `ca` / `tp`）。
- `SSL_set_session` + `SSL_set_quic_tls_early_data_enabled`；0-RTT 先在 ngtcp2 侧
  `decode_and_set_0rtt_transport_params`，否则 `open_bidi_stream` 直接失败。
- **只让每个连接的首个 GET 走 0-RTT**：多条流同时提前发送时，服务端拒绝会连带丢弃
  nghttp3 的控制流，握手期写控制流会阻塞到 `handshake_timeout`（实测 burst=10 卡 10 s）。
- 被拒时不手工重发：ngtcp2 会把 0-RTT 流数据在 1-RTT 重传（实测 301/167B，319 ms），
  手工重建流会产生重复请求。
- 票据失效条件：过期、ECH 指纹变化、证书指纹变化、**信任锚指纹变化**（见 T2）。

### Q3 部分完成

- 完成：每个请求一行 `h3-timing`（`resolve/udp/hs/ttfb/total/0rtt/reused`）；
  `JM_H3_QLOG=1` 写 qlog（默认关）；Alt-Svc 进程内学习 + H3 失败域名 10 分钟冷却。
- 未完成：`max_tx_udp_payload_size` / `initial_rtt` / 拥塞窗口的调参对比表，
  以及"连续 20 次请求 P50/P95"的样本——需要真机/长时间实测，见 §10。
- 有意偏离：文档写"命中 alt-svc 后才走 H3"。实际实现为"未学习过则允许试一次，
  失败后冷却 10 分钟"，否则首次请求永远用不上 H3（等于默认关闭）。理由写在
  `node_http.cpp` 的 `H3Verdict` 注释里。

### Q5 完成

- `ProxyConfig::NoteH3/NoteH3Fallback/NoteEch/Counters/CountersLine`，原子量计数。
- 页面「高级」区（`AdvancedStatus()`）与 `--proxy-selftest` 共用同一份；自检行是
  ASCII `proxy-align: counters h3ok=0 h3fail=0 h3fb=0 echok=0 echfail=0`
  （中文在 zh-CN 控制台是 GBK，会让 `(\d+)` 断言在解码阶段失配）。

### T1 完成

- 新增 `cpp/shell/quic/tools/build_quic_deps.ps1`：gendef/dlltool → nghttp3 → ngtcp2，
  参数化 `-OpenSslRoot / -Prefix / -Avx2`，内置三条校验（库存在、
  `Looking for SSL_set_quic_tls_cbs - found`、`nm` 符号），最后打印版本与 SHA256。
- 新增 `cpp/shell/quic/tools/build_quic_deps_android.sh`（Linux 宿主，A1+A2），
  与上面同一套校验，额外检查 ECH / early-data 符号；`SKIP_OPENSSL=1` 可只重跑 A2。
- `-mavx2` 仅用于桌面 x86_64；交叉编译与 arm64 不加（两个脚本的注释都写明）。
- 版本与校验和固化在 `docs/quic_deps_versions.md`（含两个 ABI 的 SHA256 与体积）。
- 实跑情况：Linux 版已实跑（A1+A2 ×2 ABI，全绿）；Windows 版尚未实跑（需清空
  `deps/quic-mingw`，会破坏当前可用的桌面构建）。

### T2 完成（修 2 个缺陷）

完整核查表见 `docs/cpp_tls_quic_security_review.md`。两个真实缺陷：

1. `NodeHttp` 在 H3 失败后无条件回退，若失败原因是证书校验，回退用的 `QSslSocket`
   走另一套信任库，可能把请求做成成功 → 已改为"证书错误不回退"。
2. 会话复用不重新下发证书链，换掉 `JM_CA_BUNDLE` 后旧票据仍能握手成功
   （实测 `ok=1`）→ 票据已绑定信任锚指纹（`ca=`），不一致即作废。
- 回归守卫：`h3_bad_ca` 用例（net 层），错误 CA 下断言 `ok=0` + `certificate verify failed`。

### 未完成 / 风险

| 项 | 状态 | 说明 |
| -- | --- | --- |
| A1 | 完成 | Fedora 宿主（Linux 版 NDK r30）交叉编译通过。源码 `C:/Users/WQ159/source/repos/openssl-4.1.0`；<br>产出 `deps/openssl41-android/{arm64-v8a,x86_64}`（各 24 MB，ELF for Android 34）。<br>符号齐全：`SSL_set_quic_tls_cbs`、`SSL_set1_ech_config_list`、`SSL_ech_set1_server_names`、<br>`SSL_set_quic_tls_early_data_enabled`、`SSL_CTX_sess_set_new_cb`。<br>**Windows 宿主仍不可用**（`android-*` 目标要求 Unix perl + `/` 分隔的 NDK 路径，<br>Win32 `canonpath` 产出反斜杠），所以脚本明确要求 Linux。 |
| A2 | 完成 | `deps/quic-android/<abi>/lib/` 三个 `.a`：ngtcp2 680 符号、crypto_ossl 72、nghttp3 396；<br>configure 日志确认 `Looking for SSL_set_quic_tls_cbs - found`。 |
| A3 | 完成 | Linux 宿主一套脚本出包：`cpp/shell/ui/build_android.sh`（环境检查 → 依赖 staging → qt-cmake/ninja → zipalign/apksigner → 可选部署）。<br>两种产物：`JMComic-ui-0.89-x86_64-debug.apk`（无 QUIC，63 MB）与 `JMComic-ui-0.89-x86_64-quic-debug.apk`（含 QUIC，64 MB）。<br>`aapt2 dump badging`：`org.jmcomic.ui` 0.89 / targetSdk 34 / `native-code: x86_64`；16 KB 对齐门禁通过。<br>**两个关键坑**：①同一 APK 不能同时装 OpenSSL 3.x 与 4.x——`libssl_3.so` 的 DT_NEEDED 仍是 `libcrypto.so`，会被 4.x 顶掉（`cannot locate "X509_NAME_free"`），QUIC 版改为只带 4.x；②Android 没有 OpenSSL 认得的 CA 文件，系统 cacerts 目录也加载不到证书，改为构建期把宿主 bundle 编成 `:/tls/cacert.pem`。 |
| A4 | 完成 | `--h3-probe cloudflare.com` 在 AVD 上实测：`handshake ok version=TLSv1.3 alpn=h3` → `done ok=1 status=301 bytes=167 932ms version=HTTP/3`。<br>`--advanced-status` → `quic=1 http3=1 stack=ngtcp2+nghttp3`；`--http3-on` 后文案为"HTTP3 开（QUIC/HTTP3，失败回退 HTTP/2）"。<br>**未构造**：H3 失败→HTTP/1.1 回退（需 DoH/节点路由，模拟器上该链路未拿到 `detail-real`）；ECH 在 Android 上仍为 `ech=0 echLib=1`（`EchChannelAvailable()` 依赖 Windows DLL）。 |
| M1 | 未执行 | 只有 AVD（`jm_x86_64`），没有真机 V2463A。 |
| M2 | 未执行 | AVD 已可用，但"封面占位图"需要直连/DoH/节点三路由对照，本轮聚焦出包与 H3，未做。 |
| Q4 | 阻塞 | 需要能复现 ECH-over-QUIC FAILED 的实验环境 + "隐藏 SNI 是否优先于 H3 收益"的产品决策。 |
| Q3 调参 | 未执行 | 需要 20 次采样与 qlog 对照。本机已有 Qt 6.12 的 `gcc_64` 套件，<br>桌面壳可在 Linux 侧构建后取数，但本轮聚焦 Android 出包，未做。 |
| D1 `JM_WITH_QUIC=OFF` 分支 | 未实跑 | 分支已实现；实跑需要一次完整重建并会让 `dist/` 暂时变成无 QUIC 版本。 |
| T1 脚本实跑 | 部分 | Linux 版 `build_quic_deps_android.sh` 已实跑（A1+A2，两个 ABI）；<br>Windows 版 `build_quic_deps.ps1` 仍未实跑（需清空 `deps/quic-mingw`）。 |
| R4 单流超时 | 部分验证 | 代码上 `Abort` 只关该流；未构造"单请求慢/超时"的实网用例。 |

## 10. 下一轮建议顺序

1. **A3**：在带 Qt Android kit 的机器（Windows + `C:\Qt\6.12.0\android_*`）上跑
   `build_android.ps1 -Abi arm64-v8a -WithQuic`。前置只需把
   `deps/openssl41-android/<abi>` 与 `deps/quic-android/<abi>` 拷到该机
   （Linux 侧一条命令即可重生成：`cpp/shell/quic/tools/build_quic_deps_android.sh`）。
   首次构建重点看三件事：`UI_ANDROID_EXTRA_LIBS` 里是否同时有 3.x 别名与 4.x 的
   `libssl.so`、APK 的 `lib/<abi>/` 是否只有一套无版本号 SSL、`check_ui_apk_16kb.py` 是否通过。
2. **A4**：装到 MuMu（`127.0.0.1:16384`）跑 `--h3-probe`，再按 §4 的四步验证回退。
3. **M1**：接真机后按 §5 走查，重点复测"证书不回退"与"连接池复用"两条新逻辑。
4. **Q3 调参**：`JM_H3_QLOG=1` + 20 次采样，形成 P50/P95 对比表，再决定 8 s 预算是否调整。
5. **Q4**：在能复现 FAILED 的环境按四组实验定性；若结论是"服务端策略"，把
   "ECH 仅 TCP"写进 UI 文案而不是只留在注释里。
## 11. Linux 侧 Android 构建（0.89 新增）

入口：`cpp/shell/ui/build_android.sh`（`build_android.ps1` 的 Linux 对应物）。

```
./build_android.sh --abi x86_64                 # 普通包
./build_android.sh --abi x86_64 --with-quic     # 含 QUIC/HTTP3 的包
./build_android.sh --abi x86_64 --deploy        # 装到已启动的 AVD 并拉起
./build_android.sh --abi x86_64 --test          # 再跑 c1_ui_android_smoke.py
```

脚本覆盖四段，每段都有前置检查：

1. **环境**：Qt Android kit + 宿主 Qt（`androiddeployqt` 要用）、ninja、**带 javac 的 JDK（≤25）**、
   SDK platform `android-34`、build-tools（zipalign/apksigner）、NDK、
   OpenSSL 3.x（Qt 的 TLS 插件）、`--with-quic` 时的 A1/A2 产物。
2. **依赖 staging**：Qt 的 `libssl_3.so`/`libcrypto_3.so` 别名；QUIC 版把 OpenSSL 4 同时用作
   Qt 后端与自建 QUIC（两套 3.x/4.x 不能共存，见 §9 A3）；构建期把宿主 CA bundle 生成到
   `<build>/generated/cacert.pem` 并编成 `:/tls/cacert.pem`。
3. **构建**：`qt-cmake`（`ANDROID_ABI`/`ANDROID_PLATFORM`/`QT_HOST_PATH`/`UI_ANDROID_EXTRA_LIBS`）
   → `ninja`（含 `androiddeployqt` + Gradle）。
4. **签名与校验**：`zipalign -p -f 4` → `apksigner`（自动创建 `~/.android/debug.keystore`）
   → `aapt2 dump badging` → 解包检查所有 `.so` 的 PT_LOAD 是否 `0x4000`（16 KB 门禁）。

产物：`cpp/dist/JMComic-ui-0.89-<abi>[-quic]-debug.apk`。

### 本机特有的坑（脚本已处理，写下来免得重踩）

| 现象 | 原因与处理 |
| --- | --- |
| Gradle 下载卡在 40 KB/s | 直接复用 Windows 侧 `%USERPROFILE%\.gradle\wrapper\dists` 里已解压的发行包；<br>依赖用 `GRADLE_RO_DEP_CACHE` 指向另一份 `.gradle/caches` 只读复用。 |
| `Unsupported class file major version 71` | Gradle 9.5.1 不能在 JDK 27 上跑；装 JDK 21（华为镜像）并设 `JAVA_HOME`。 |
| `Toolchain ... does not provide [JAVA_COMPILER]` | Fedora 的 `java-25-openjdk` 是 JRE（无 javac）；脚本会检查 `$JAVA_HOME/bin/javac`。 |
| `Unsupported value: android-37.0-2` | SDK 里有重复安装的畸形平台目录（`avdmanager` 也会警告）；已移到 `~/.android/_bak/`。 |
| 模拟器 SIGSEGV | `-gpu swiftshader_indirect` 在本机必崩；改用 `-gpu host`（headless 也可）。 |
| `adb shell am start --es jm.args "多 词"` 被拆 | 设备端 shell 会重新分词；要么整体加引号，要么只传单个开关。 |

### 设备侧验证入口

```
# 能力摘要（quic=1 http3=1）
adb shell "am start -n org.jmcomic.ui/org.qtproject.qt.android.bindings.QtActivity \
  --es jm.args '--advanced-status'"

# 真实 HTTP/3 请求
adb shell "am start ... --es jm.args '--h3-probe cloudflare.com --h3-path /'"
adb logcat -d | grep h3-probe
```

---

## 12. 下一阶段开发计划（0.90）

> 编写时间：2026-11-07 23:30  
> 起点：0.89 已交付三份 APK（x86_64 普通/QUIC、arm64 QUIC）、H3 在 AVD 实测可用、
> 安全复核修掉两个真实缺陷。  
> **一句话结论：能力已经齐了，缺的是"验证闭环"。** 下一阶段不铺新功能，先把三个
> 验证洞补上，再讨论"是否默认开启"。

### 12.1 现状盘点：三个验证洞

| 洞 | 证据 | 性质 |
| -- | --- | --- |
| A4-a：Android 的 ECH 恒为 0 | `--advanced-status` → `ech=0 echLib=1`；`EchChannelAvailable()` 里 `TryLoad()` 只找 `libssl-4-x64.dll` | **代码没跟上，不是平台不支持**——APK 里已经有 OpenSSL 4.1 的 `libssl.so` |
| A4-b：H3 失败 → HTTP/1.1 回退未构造 | 需要 DoH/节点链路才能触发；模拟器上那条链路没拿到 `detail-real` | **不可控**，应改为受控注入而不是等链路 |
| Q3：只有单次样本 | AVD 一次 932 ms；无 P50/P95 | **无法判断 8 s 预算是否合理**，也无法回答"是否值得默认开 H3" |

这三个洞的共同点：都不是"要写新功能"，而是"现有能力缺一条可复跑的证明"。
所以 0.90 的主题定为 **把 QUIC 从"能跑"变成"可验收"**。

### 12.2 第一梯队（0.90 主线，无外部依赖）

| 序 | ID | 任务 | 关键改动 | 验收 |
| -- | -- | --- | --- | --- |
| 1 | A5 | Android TLS/ECH 通道平台化 | `cpp/shell/tls/openssl_tls.cpp` 的 `TryLoad()` 按平台选库名与目录：Android 从 native lib 目录加载 `libssl.so`/`libcrypto.so`，Windows 仍走 `-4-x64.dll` | `--advanced-status` 出 `echLib=1`；`--ech-probe cloudflare.com` 的状态不是 `NOT_CONFIGURED` |
| 2 | A6 | Android 端 CLI 对齐 | `main_desktop.cpp` 的 Android 分支补齐 `--ech-probe`、`--proxy-selftest`、`--h3-bench`、`--h3-fail-inject` | 桌面能跑的验收开关，Android 上同名同义 |
| 3 | A7 | H3 回退的受控注入 | 新增 `--h3-fail-inject=bad-ip\|drop-first\|bad-alpn`，只在 `node_http.cpp` 的 H3 入口生效，不影响 TLS 路径 | 业务仍成功；`Counters` 的 `h3fail`/`h3fb` 各 +1；无重复请求；页面数据正常 |
| 4 | A8 | 采样基准化 | 新增 `--h3-bench N`：串行 N 次独立 `quic::Client`，输出 P50/P95/min/max 与逐次 CSV | 20 次采样；汇总值与逐行 `h3-timing` 能对上；形成"开/关 ECH"两组对照 |
| 5 | A9 | Android H3 回归脚本 | 新增 `cpp/tools/c1_ui_android_h3.py`：能力 / ECH / 探针 / 回退 / 基准 五项 | 一条命令全绿；SKIP 与 FAIL 分开计，SKIP 必须带原因 |
| 6 | A10 | Alt-Svc 持久化 | 进程内 `QHash` → `<state>/alt-svc.json`，带 TTL 清理与损坏自愈 | 冷启动后第二次访问直接命中缓存，不再做无谓探测 |

**顺序理由**：A5 与 A6 改的是同一条编译链（`openssl_tls.cpp` + `main_desktop.cpp`），
合并成一次编译；A7 依赖 A6 提供的开关通道；A8 要等 A5 之后才能在 Android 上取到
"开 ECH / 不开 ECH"的对照数据；A9 收口；A10 独立，可并行。

**A5 的风险与回退**：Qt 的 TLS 插件与 ECH 通道可能 dlopen 同一份 `libssl.so`。
若出现符号冲突或初始化竞态，回退方案是"只在 ECH 通道内 `RTLD_LOCAL` 解析符号"
（Windows 侧已是类似思路：先 crypto 后 ssl，复用已加载模块）。因此 A5 的验收必须包含
**"普通 HTTPS 请求仍正常"** 这一条，不能只看 ECH。

### 12.3 第二梯队（需外部输入）

| ID | 任务 | 缺什么 | 有输入后的动作 |
| -- | --- | --- | --- |
| M1 | 真机 V2463A 全链路 | 设备 | 按 §5 走查；重点三条新逻辑：证书错误不回退、连接池复用、0-RTT 只给首个请求 |
| M2 | 封面占位图定性 | 可用节点 | 直连 / DoH / 节点三路由各截一张，比对占位图出现的条件；若为环境因素则记录结论后关闭 |
| Q4 | ECH-over-QUIC 归因 | 产品决策 | 先做实验 1（keylog 看 ClientHello 是否含 `0xfe0d`），再决定是否恢复注入 |

Q4 的决策点值得单独说清：若结论是"服务端策略拒绝"，则"隐藏 SNI"与"H3 收益"二者
不可兼得。此时应在 UI 文案里明确写"ECH 仅 TCP"，而不是让用户以为开了 H3 就同时有 ECH。

### 12.4 第三梯队（治理，不阻塞发版）

- **体积（R5）**：QUIC 版 67.4 MB / 普通版 65.4 MB。先测三件事——`strip` 后 `.so` 的实际
  占比、Qt 翻译与 `qt.conf` 裁剪空间、是否还有可去的大资源。目标 ≤55 MB/ABI；
  达不到就把"仅 arm64 内置 QUIC"作为分发策略写进说明。
- **0.90 收口**：版本号三处统一 + `docs/cpp_ui_parity.md` 增 §27 + `CHANGELOG` 条目。

### 12.5 需要拍板的四件事

1. **H3 是否默认开启**：现在默认关。AVD 一次 932 ms，但缺真机数据，建议等 A8 的
   P50/P95 出来后再定阈值（例如 P95 < 1.5 s 才默认开）。
2. **ECH 与 H3 冲突时的优先级**：决定 Q4 的结论怎么落到 UI 与文档。
3. **APK 体积上限**：决定是否要为体积裁掉部分功能或资源。
4. **arm64 是否出 release 签名包**：当前三份 APK 都是 debug 签名，只能侧载。

### 12.6 建议的执行批次

按"一次编译改多处"的原则，分三批：

1. **第一批**：A5 + A6 —— 同一条编译链，跑通 Android ECH，确认普通 HTTPS 不受影响。
2. **第二批**：A7 + A8 —— 都在请求路径上（`node_http.cpp` / `quic_http.cpp`），
   出第一份 P50/P95 与第一份回退证据。
3. **第三批**：A9 + A10 —— 收口成一条命令 + 缓存落盘，然后进入 0.90 发版。

---

## 13. 0.90 执行结果（2026-11-08，真机 V2463A 接入）

> 设备：vivo V2463A / Android 17 / SDK 37 / arm64-v8a / 1440x3168  
> 产物：`cpp/dist/JMComic-ui-0.89-arm64-v8a-quic-release.apk`（62 MB，H3+ECH，release 签名）  
> **一句话结论：A5–A11 全部完成并真机验收；过程中发现 3 个真实缺陷（2 个已修）。**

### 13.1 逐项结果

| ID | 结果 | 证据 |
| -- | --- | --- |
| A5 Android TLS/ECH 平台化 | **完成** | `advanced-status` → `ech=1 echLib=1 tls=openssl-ech/4.1.0`；`ech-probe` → `handshake ok version=TLSv1.3 alpn=h2 ech=SUCCESS verify=0/1`，`backend=OpenSSL 4.1.0 @ libssl.so` |
| A6 Android CLI 对齐 | **完成** | Android 分支改为"先收集再执行"，`--ech-probe`/`--h3-bench`/`--h3-burst`/`--h3-fail-inject`/`--doh-on`/`--use-node` 与桌面同义；`--proxy-selftest` **有意不接**（同步自检会堵住启动） |
| A7 H3 回退受控注入 | **完成** | `--h3-fail-inject bad-port` → `node-http h3 inject: forced port 9` → `h3 fallback (timeout 8000ms)` → 应用存活；修复后同域只失败一次 |
| A8 采样基准化 | **完成** | `--h3-bench 5` → `n=5 ok=5 p50=329ms p95=3669ms ttfb_p50=… csv=/data/user/0/org.jmcomic.ui/files/h3-bench-*.csv` |
| A9 Android H3 回归脚本 | **完成** | 新增 `cpp/tools/c1_ui_android_h3.py`，五项断言，真机 `PASS 10 / FAIL 0 / SKIP 1`（SKIP 为 ECH 首次取配置超时，已加重试） |
| A10 Alt-Svc 持久化 | **完成** | `<state>/alt-svc.json`（2s 防抖 + TTL 清理 + 损坏自愈）；真机冷启动日志 `h3 alt-svc: host=www.cdnhjk.net verdict=2 age=456s`，`age` 只能来自磁盘 |
| A11 arm64 release 签名包 | **完成** | RSA 4096 / 25 年 keystore（`~/.android/jmcomic-release.keystore`，密码在 `.pass`，均 0600）；`apksigner verify` 通过；`versionName 0.89 / targetSdk 34 / native-code arm64-v8a`；`.so` PT_LOAD `0x4000` |

### 13.2 真机性能数据（Q3 的第一批真机样本）

| 场景 | 结果 |
| -- | --- |
| 单次 H3 探针（cloudflare.com） | `301 / 167B / 258~885ms`，`hs=210ms` |
| `--h3-bench 5`（每次全新握手） | `p50=329~656ms`，`p95=2214~3669ms`，`ok=5/5` |
| H3 失败预算 | 8s 超时后回退，业务不受影响 |

结论：真机 P50 在 300~700ms 区间，比 AVD（932ms）好；P95 波动来自首次握手，**8s 预算足够，无需调整**。

### 13.3 新发现并修复的缺陷

**(1) Alt-Svc 广告覆盖"实测失败"（已修）**

H3 超时回退到 HTTP/1.1 后，那个响应同样带 `alt-svc: h3=":443"`，旧逻辑照单全收，把 Denied 改回 Advertised → 每个请求都白等一次 8s。
修复：`LearnAltSvc` 在 Denied 冷却期内忽略广告（`node_http.cpp`）。
证据：修复前同一 host 连试 3 次（8s×3），修复后只 1 次。

**(2) 注入模式被历史缓存挡住（已修）**

第二次跑回归时，上一轮留下的 `Denied` 让 H3 分支根本进不去，回退用例直接 FAIL。
修复：`H3Verdict` 在 `H3FailInject` 非空时返回 Unknown，保证验收可重复。

**(3) Android 17 返回键直接退出应用（未修，需能跑 gradle 的环境）**

清单里 `android:enableOnBackInvokedCallback=true`，真机（SDK 37）走预测性返回，QtActivity 未接管 → 搜索页/阅读器按返回直接 finish（截图显示退到桌面浏览器）。
计划：用 `QT_ANDROID_PACKAGE_SOURCE_DIR` 提供自定义 `AndroidManifest.xml`，把该属性设为 `false`，回到 `onBackPressed` 转发路径。**改清单必须重新打包，本环境做不到**。

### 13.4 M1 真机冒烟（`c1_ui_android_smoke.py`）

`PASS 18 / FAIL 2 / SKIP 1`。全部失败项都是上面第 (3) 条：
- 返回键从搜索页回首页（只退一级）
- 阅读器返回后仍在应用内

其余通过：安装启动、首页非空白、抽屉、进详情、收藏切换、进阅读器、翻页、长按菜单、旋转后存活、首页返回退出、logcat 无 FATAL/ANR。横屏仍为 SKIP（headless 判定），需真机旋转验证。

### 13.5 环境限制：Gradle 打包不可用

本机沙箱**禁止 `utimensat`**（`touch` 在 `/tmp`、`/home`、工作区均报 EPERM，`dangerouslyDisableSandbox` 也不解除）。Gradle 的 `mergeReleaseJniLibFolders` 用 `Files.copy` 设置 mtime，必然失败（22 处 `不允许的操作`），换 build 目录、清中间产物都无效。

绕行：新增 `cpp/shell/ui/repack_apk.sh` —— 以既有 APK 为模板，替换目标 ABI 的 `libjmcomic_ui_<abi>.so`（`llvm-strip` → `zip -0` 保持 STORED → `zipalign -p 4` → `apksigner`）。
**前提**：资源与清单无变化，只有 C++ 变更。0.89 的本轮 APK 即由此产出，安装与运行均正常。

Gradle 侧的改动（`GRADLE_USER_HOME` 自动探测、`--release` 签名模式）已写进 `build_android.sh`，在正常环境可直接用。

### 13.6 仍遗留

| 项 | 说明 |
| -- | -- |
| 返回键（Android 17） | 需自定义 manifest + 重新打包，见 13.3(3) |
| 直连不走 H3 | `NetClient` 只有 `connectIp` 非空（DoH/节点）才走 `NodeHttp`；直连走 `QNetworkAccessManager`，没有 H3 分支。要让"默认开启 H3"有意义，须补这条路径 |
| M2 封面占位图 | 只做了直连+DoH 两路由，未定性；需第三路由（节点） |
| Q4 ECH-over-QUIC | 未做，需产品决策 |
| 版本号 | 已统一 0.90（见 §14.3）；不再需要 Gradle 打包 |

---

## 14. 0.90 收口：清单修补 / 直连 H3 / 返回键定性（2026-11-08 下午）

> 起点：§13.6 的四条遗留里，"返回键"和"版本号"都被判成"必须能跑 Gradle 才能改"，
> "直连不走 H3"被登记为覆盖缺口。本轮把**改清单**从 Gradle 里解耦出来，
> 三条里两条闭环，第三条（返回键）的性质也被实测推翻并重新定性。

### 14.1 二进制清单原地修补（绕开 Gradle 的 utimensat 限制）

Qt 的 Android 模板把所有属性写进 `AndroidManifest.xml`，Gradle 只是把它编成 AXML
塞进 APK。AXML 是**长度稳定**的二进制格式，所以"改一个布尔属性 / 一个整型属性 /
一个等长的字符串"根本不需要重新打包资源：

- 新增 `cpp/shell/ui/patch_apk_manifest.py`：完整解析 string pool（UTF-8/UTF-16）与
  `RES_XML_START_ELEMENT_TYPE` 的 attrExt，按属性名定位后只改 4 字节
  `Res_value.data`（`TYPE_INT_BOOLEAN` / `TYPE_INT_DEC`），或等长替换字符串池里的
  UTF-16 载荷（`TYPE_STRING`）。作用域严格限定在 `enableOnBackInvokedCallback` /
  `versionName` / `versionCode` 三个属性，结构不符、出现次数不为一、字符串长度变化
  都直接报错；写盘前用同一套解析器重新读一遍自检。
- `cpp/shell/ui/repack_apk.sh` 增加 `--manifest`，`--so` 变为可选：
  `.so` 仍用 `zip -0`（STORED，`extractNativeLibs=false` 要求），清单保持 deflate，
  之后统一 `zipalign -p 4` → `apksigner`（release keystore）。
- **适用边界**：资源、dex、包名不变，只改清单属性与 C++ `.so`。要增删 Activity、
  改权限或改 Java 类仍需真正的 Gradle 构建。

### 14.2 直连也走 H3（补上 §13.6 的覆盖缺口）

0.89 的 `NetClient::StartAttempt()` 只在 `connectIp` 非空（节点/DoH）时走 `NodeHttp`，
直连固定走 `QNetworkAccessManager`，因此"默认开启 H3"对直连流量完全没有意义。

改动：

| 文件 | 内容 |
| --- | --- |
| `node_http.h` | `NodeHttpRequest::quicOnly`（默认 false）+ 两个哨兵错误 `kH3Unavailable` / `kH3CertFailure` |
| `node_http.cpp` | `quicOnly` 模式只借 QUIC 通道；没资格做 H3（开关关 / 冷却 / POST / http）立刻回哨兵，**绝不**落到本文件自建的 HTTP/1.1 |
| `net_client.cpp` | 抽出 `StartQnamAttempt()`；`--http3-on` 且 https GET 时先 `StartSocketAttempt(true)`，收到 `kH3Unavailable` 就回落到 QNAM/HTTP2；收到 `kH3CertFailure` 直接判 `SSLErr` 收口，**不换一套 CA 重试**（延续 T2 的"证书错误不回退"） |
| `main_desktop.cpp` | 新增 `--direct-h3-probe <url>`（桌面 + Android 同义），建一个干净的 `NetClient`，不受节点/DoH 配置影响。**不复用 `--net-probe`**：那个是桌面回归套件依赖的同步 `QEventLoop` 探针，同名会触发 `QCommandLineParser: option already added` |
| `cpp/tools/c1_ui_android_h3.py` | 新增第 6 组用例：直连 H3 成功、注入失败回退 QNAM 且业务成功 |

关键取舍（写于 14.2 完成时）：当时 **H3 默认关，该分支完全不进**，行为与 0.89
逐字节一致；打开后回退目标仍是 Qt 的 HTTP/2（而不是 NodeHttp 的 HTTP/1.1），
与 UI 上"失败回退 HTTP/2"的文案一致。**§14.7 起 H3 已默认开启**，这条分支现在
默认就会进，`--http3-on` 退化为显式覆盖；表格里的开关条件应结合 §14.7 理解。

### 14.3 版本号收口

四处统一为 **0.90**：`CMakeLists.txt` 的 `project(VERSION)`、
`QT_ANDROID_VERSION_CODE=90` / `QT_ANDROID_VERSION_NAME="0.90"`，以及 APK 的
`versionCode=90` / `versionName=0.90`（由 14.1 的工具补丁写入）。
产物：`cpp/dist/JMComic-ui-0.90-arm64-v8a-quic-release.apk`（64 MB、arm64-v8a、
release 签名），`apksigner verify` 通过、16 KB PT_LOAD 门禁 46/46 通过。

### 14.4 返回键：推翻 §13.3(3) 的归因

§13.3(3) 把它写成"Android 17 预测性返回 + QtActivity 未接管，需自定义 manifest 重打包"。
本轮实测**只对了一半**：

- Qt 6.12 的 `QtActivityBase` 只有 `dispatchKeyEvent` / `onKeyDown` / `onKeyUp`，
  **没有** `onBackPressed`，也没有 `OnBackInvokedCallback` 注册；模板却把
  `enableOnBackInvokedCallback` 写死为 `true`。结果是系统默认回调直接 `finish`，
  并且 Qt 侧 back 事件双发（0.89 的 `keyPressEvent` 那段 600ms 去重就是为此写的）。
- 用 14.1 把属性改成 `false` 后，logcat 实测每次物理返回**只投递一发**
  `Qt::Key_Back (0x1000061)`；非首页返回由 `MainWindow::keyPressEvent` 的 `Back()`
  消费，应用留在前台（详情页按返回 → `topResumedActivity` 仍是本应用）。
- **冒烟里那两条 FAIL 不是返回键缺陷**：`c1_ui_android_smoke.py` 的抽屉坐标
  `(153,1953)` 以 1080x2400 为基准，在真机 1440x3168（安全区 + 字体缩放）上
  命中的是「首页」项——`docs/shots/android/03_search.png` 实际仍是首页，
  日志 `jm-key: ... page=0 drawer=0` 也印证 `current_ == Home`。
  于是在首页按返回，按设计就该退出应用（脚本最后一条用例专门验这个）。
- 修正 `c1_ui_android_smoke.py`：把"搜索页返回"改成确定性断言
  **"抽屉展开时按返回只收起抽屉、应用留在前台"**；把"阅读器连按两次返回"的
  Focused 断言移到**第一次返回**之后（第二次若已到首页，按设计会退出）。

结论：**清单补丁保留**（消除双发、让 Qt 正常收到返回键），返回键本身没有待修的
平台缺陷；此前的 FAIL 是回归脚本的导航假设失效。

### 14.5 顺带修掉的工具链缺陷

- `cpp/tools/check_ui_apk_16kb.py` 的 `FindReadElf()` 只拼 `windows-x86_64` +
  `.exe`，Linux 宿主必然误报 "llvm-readelf not found"；改为按宿主三平台探测并回退 PATH。
  本机 `/tmp` 仅 10 MB，验证 64 MB APK 需 `TMPDIR` 指向大容量目录。
- `c1_ui_android_h3.py` 的直连用例加了冷启动重放：真机 `force-stop` 后首启偶发
  在等待窗口内一条应用日志都没有（ART/odex 检查 + Qt 初始化），不能算功能失败。

### 14.5.1 真机验收（V2463A / Android 17 / SDK 37 / arm64-v8a）

| 验收 | 入口 | 结果 |
| --- | --- | --- |
| H3 全量回归 | `c1_ui_android_h3.py --skip-install --bench 5` | **PASS 16 / FAIL 0 / SKIP 0** |
| 能力 / ECH | `--advanced-status`、`--ech-probe` | `quic=1 http3=1 echLib=1 tls=openssl-ech/4.1.0`；`ech=SUCCESS(1)` |
| HTTP/3 探针 | `--h3-probe cloudflare.com` | `301 / 167B / 310ms / HTTP/3 / alpn=h3` |
| 节点/DoH 回退 | `--doh-on --http3-on --h3-fail-inject bad-port --open-album` | 注入生效 → HTTP/1.1 回退 → 应用存活 |
| 首包采样 | `--h3-bench 5` | `ok=5/5 p50=325ms p95=1934ms`（8s 预算充足） |
| 直连 H3 | `--http3-on --direct-h3-probe https://cloudflare.com` | `via h3-probe` → `direct-h3-probe: net OK` |
| 直连注入回退 | 再加 `--h3-fail-inject bad-port` | `forced port 9` → `h3-unavailable` → `fallback to QNAM` → `net OK` |
| 交付包核验 | `aapt2 dump` / `apksigner verify` / 16 KB 门禁 | `versionCode=90 versionName=0.90`；签名 OK；46/46 `.so` 的 PT_LOAD ≥ `0x4000`；66,323,751 B |
| M1 全链路冒烟 | `c1_ui_android_smoke.py --no-install` | **PASS 19 / FAIL 0 / SKIP 0** |

M1 本轮 19 项全绿，SKIP 清零（横屏旋转这次在真机上也成功）。此前被误判为
「Android 17 返回键缺陷」的两条用例，按 14.4 改成
「抽屉展开时返回只收起抽屉、应用留前台」与「阅读器第一次返回后仍在应用内」后均 PASS；
最后一条「首页返回退出应用」仍 PASS，说明返回语义自洽。

### 14.6 仍遗留（需外部输入或产品决策）

| 项 | 缺什么 | 建议 |
| --- | --- | --- |
| H3 是否默认开启 | **已决策（用户）** | 0.90 起默认开启；实现与迁移见 §14.7 |
| Q4 ECH-over-QUIC 归因 | **已决策（用户）** | 隐藏 SNI 优先：组合被拒 → TCP+ECH；下次启动/换节点重试组合。见 §14.7 |
| M2 封面占位图定性 | 第三个路由（节点）截图对照 | 已做直连 + DoH 两路由；若为环境因素，记录结论后关闭 |
| Windows 侧 `build_quic_deps.ps1` 实跑 / `JM_WITH_QUIC=OFF` 分支实跑 | Windows 宿主（不受沙箱限制） | 非阻塞；Linux 侧依赖脚本已实跑两个 ABI |
| 改 Java / 增删 Activity / 改权限 | 能跑 Gradle 的环境 | 14.1 的 AXML 补丁只覆盖**属性级**改动，不能替代真正的打包 |
| 横屏渲染冒烟 | 无（本轮真机旋转成功，M1 已 PASS） | 早期 headless AVD 记 SKIP 的现象未再复现 |

### 14.7 Q4 决策落地：H3 默认开 / 隐藏 SNI 优先 / 重启换节点重试

用户给出的三条决策，本轮全部落到代码与可复跑回归里。

**1. H3 默认开启**

`proxy_config.cpp` 的 `State::http3Enabled` 改为 `true`，并改读新配置键
`proxy/http3On`（旧键仍写，供旧版回读）。原因是 0.89 的 `Save()` 把默认
`false` 写进过 ini —— 继续读旧键会让老安装升级后仍然是关，决策落不了地。
`--advanced-status` 在不带 `--http3-on` 的真机上实测输出
`HTTP3 开（QUIC/HTTP3，失败回退 HTTP/2）`。

**2. 服务端拒绝 ECH-over-QUIC → 回退 TCP+ECH**

| 文件 | 改动 |
| --- | --- |
| `cpp/shell/quic/quic_http.h/.cpp` | `Response.echRejected`：注入过 ECH 但 `SSL_ech_get1_status() != 1` 为真；握手未完成时在 `FinishJob()` 补读一次状态（`ssl` 此时尚未 `Cleanup`）。`--h3-probe` 输出加 `echRejected=` |
| `cpp/shell/node_http.h/.cpp` | `NodeHttpRequest::echConfigList`；`StartQuic()` 重新注入 `CachedEch()` 尝试组合；回调里 **先判 `echRejected` 再判 `r.ok`**，丢弃暴露 SNI 的 H3 结果，带同一份配置回退 TCP 通道；组合失败不调 `DenyH3()` |
| `cpp/shell/net_client.cpp` | 直连 `quicOnly` 分支同样受抑制集合约束；注释同步为「0.90 起默认开」 |
| `cpp/shell/ui/proxy_config.h/.cpp` | `EchH3Blocked/NoteEchH3Rejected/ResetEchH3Blocks/EchH3NodeKey`；`AdvancedStatus()` 加 `echH3Blocked=N` |

**3. 重试策略**

抑制集合按 `(host, 节点 IP)` 记，**只存内存**：冷启动自动为空 → 重新尝试组合；
`SetSelectedIp()` 换节点清空；`NodeHttp::Send()` 检测到节点变化还会清掉
Alt-Svc 里持久化的 `Denied` 并强制落盘，避免冷启动又读回来。页面保存
（原 `ResetAdvancedFallback()`）也清空，等价于给组合一次新机会。

**4. 验收入口（真机可复跑）**

```
--h3-fail-inject ech-reject --ech-h3-probe https://www.cloudflare.com/
--ech-h3-selftest
--advanced-status
```

`ech-reject` 只跳过真实 QUIC 往返，抑制 / 回退 / TCP+ECH 三条链路与真失败共用。
`--ech-h3-probe` 连发两次：第一次验「拒绝 → TCP+ECH 且业务成功」，第二次验
「本进程内 `combo suppressed`、不再白试组合」。用 `www.cloudflare.com` 固定域名
——裸域会 301 换 host，重定向会再登记一条 `(host,node)` 抑制，让计数不确定。

真机手动取证（V2463A）：

```
node-http ech/h3 rejected ech=FAILED (injected) ... -> tcp+ech
ech-h3-probe: pass=1 ok=1 status=200 ... echH3Blocked=1
node-http ech/h3 combo suppressed for www.cloudflare.com
tls handshake ok version=TLSv1.3 alpn=http/1.1 ech=SUCCESS verify=0/1
ech-h3-probe: pass=2 ok=1 status=200 ... echH3Blocked=1
ech-h3-selftest: ... blockedAfter=1 otherNode=0 ... afterNodeChange=0 ... -> PASS
```

`ech=SUCCESS` 证明回退确实保留了隐藏 SNI，不是退化成普通 TLS。

**5. 顺带修掉的工具链问题**

- `repack_apk.sh` 用 `unzip -v ... | grep -q " 0%"` 判 `.so` 是否 STORED；
  在 `set -o pipefail` 下 `grep -q` 命中首行即退出，`unzip` 收到 SIGPIPE 让整个
  管道判失败 —— `.so` 明明是 `Stored 0%` 却误报 deflated。改成 shell 模式匹配。
- 真机 `adb install -r -t` 偶发 `INSTALL_FAILED_ABORTED: User rejected permissions`
  （安装器 UI 被系统自动拒绝，屏幕上没有可点的确认框）；`adb push` 到
  `/data/local/tmp` 后用 shell 侧 `pm install -r -t` 可绕开。已写进
  `c1_ui_android_h3.py` 的安装回退分支。
- `c1_ui_android_h3.py` 的 logcat 抓取从 `-t 4000` 放宽到 `-t 8000`：H3 默认开后
  启动期业务请求也会做一次 QUIC 探测，早期 `advanced-status` 会被挤掉。

**6. 边界（如实记录）**

`quicOnly` 直连探测（NetClient → QNAM/HTTP2）不注入 ECH：Qt 6.12 的 QNAM 没有
ECH 能力，注入后被拒也无处「回退到 TCP+ECH」。Q4 的「隐藏 SNI 优先」因此
只作用于 `NodeHttp` 业务路径；直连路径只受抑制集合约束。若将来要让所有流量
都优先 ECH，需要给 `NetClient` 接一条独立的 TCP+ECH 通道，属新功能而非本轮收口。

默认开启的代价也要如实记：真机启动时首页域名 `www.cdnhjk.net` 的 H3 探测实测
握手 5448ms、请求 8291ms 超时后才回退 QNAM。普通 H3 失败仍会写 Alt-Svc 的
`Denied` 冷却（10 分钟，持久化），因此只有首次冷启会吃这 8s；这与 Q4 组合拒绝
**不是同一类判定** —— 后者代表「ECH 组合不行」，按决策必须下次重启再试，所以
刻意不写 Denied。若后续要优化首屏，方向是缩短 H3 探测预算或按域名预热，
而不是把默认开关改回去。

**7. 最终交付包与验收**

| 项 | 结果 |
| --- | --- |
| H3 全量回归 | `c1_ui_android_h3.py --skip-install --bench 5` → **PASS 23 / FAIL 0 / SKIP 0**（第 7 组 7 条全绿） |
| 默认开启 / 冷启动复位 | 不带 `--http3-on` 的 `advanced-status` → `HTTP3 开` + `echH3Blocked=0` |
| 策略拒绝回退 | `pass=1 ok=1 echH3Blocked=1` → `combo suppressed` → `pass=2 ok=1`；TCP 侧 `ech=SUCCESS` |
| bench | `ok=5/5 p50=348ms p95=9854ms`（首次握手偶发长尾，与 14.6 记录的 P95 风险一致） |
| 交付包 | `cpp/dist/JMComic-ui-0.90-arm64-v8a-quic-release.apk`，66,356,519 B，SHA256 前缀 `bb0cf8b2…`，`apksigner verify` OK，`versionCode=90 / versionName=0.90`，16 KB PT_LOAD 46/46 |
| M1 UI 冒烟 | `c1_ui_android_smoke.py --no-install` → **PASS 19 / FAIL 0 / SKIP 0**（最终包，含返回键与横屏） |

回归里两处判定也随本轮决策更新：第 4 组 DoH 路由现在带 ECH 组合，回退日志从
`h3 fallback (...)` 变成 `ech/h3 rejected ... -> tcp+ech`，断言同时接受两种；
`Launch()` 增加「冷启动整段无应用日志自动重放」，修掉 bench 偶发 SKIP。
桌面 `c1_ui_desktop_suite.py` 第 9 组的 `http3=(\d+)` 断言从 `0` 改为 `1`
（C++ 壳默认开，Python 端仍默认关，属刻意分歧）。

## 15. 真机滚动性能：量化 → widgets 优化 → QML 列表（2026-11-08 晚）

用户报"真机运行帧率低不流畅"。定位 + 两轮修复，滚动呈现从 **20fps → 60fps**。

### 15.1 量化方法（Qt 应用专属）

Qt 不走 Android HWUI，`dumpsys gfxinfo` 恒为 0 帧，**对 Qt 无效**。有效手段：

- `adb shell screenrecord` + `ffprobe` 逐帧 pts：统计"变化帧"的中位间隔
  （screenrecord 静止不出帧，gap 是无内容变化，不是卡顿；要结合操作时序看）。
- `top -H`：确认瓶颈不是算力（滚动时主线程仅 1~4%，整机 768/800 idle）。

### 15.2 量化结论

| 场景 | 路径 | 呈现帧率 |
| --- | --- | --- |
| 抽屉/换页动画 | QQuickWidget 场景图（GPU） | **~112fps** |
| 列表滚动（优化前） | QListView CPU 光栅化 | **~20fps**（50ms/帧） |

每帧 50ms ≈ 1440x3168@1.33dpr ≈ **32MB** 的全视口重绘 + flush。三个放大因素：
`ComicListView::mouseMoveEvent` 每次 mousemove 全视口 `viewport()->update()`（hover
高亮）、`QAbstractItemView::scrollContentsBy` 的全视口 update、触摸拖动时两者叠加。

### 15.3 第一轮：widgets 内优化（20 → 30fps）

- 删 mouseMoveEvent 的全视口 update（QAbstractItemView 内建 hover 只重绘新旧两格）。
- `scrollContentsBy` 改 `viewport()->scroll(dx,dy)` 走 backing-store blit（只 memcpy
  平移 + 重绘暴露条带）。符号与 QScrollArea 的 `widget->move(pos+QPoint(dx,dy))` 一致。
- `CardPaint::Paint` 的 clipPath 改 `if constexpr (UiStyle::Radius > 0)`（Radius=0
  直角风格下是纯开销）。

**注意**：`DontSavePainterState` 是 QGraphicsView 的枚举，QAbstractItemView 没有。
30fps 是 CPU raster 的天花板（blit memcpy + 条带重绘 + flush 的固定内存带宽成本）。

### 15.4 第二轮：列表搬进 Qt Quick（30 → 60fps）

用户拍板 GPU 化。**ComicGrid 对外接口不变**（SetComics/AddComics/SetPagedSource/
SetCardWidth/Count/ScrollToBottom + Activated/ContextAction），内部换成
QQuickWidget + `qml/PageCards.qml`（GridView + scene graph）——首页/分类/搜索/收藏
的调用点零改动。

| 文件 | 内容 |
| --- | --- |
| `cpp/shell/ui/cover_provider.h/.cpp` | QML 封面图源：单例 QImage LRU（QMutex，QML 加载线程访问）+ 主线程预取（CoverCache/QPixmap 只允许主线程）；bump(key) → 模型按行 dataChanged |
| `cpp/shell/ui/comic_view.cpp/.h` | ComicListModel 加 QML roles（id/title/author/…/coverReq/coverRev）；coverRev **按 comic id 记**（全局代次会让整屏 Image 一起重载、异步间隙闪占位色）；ComicGrid 内部 QQuickWidget 化；几何（cardW/cellW/cellH）仍由 C++ ApplyGeometry 算好 setProperty，列数规则只有一份 |
| `cpp/shell/ui/qml/PageCards.qml` | GridView delegate：封面/角标/进度条/标题/作者 + 点击与长按（cardBridge.cardClicked/cardLongPress → QMenu 三项，与 widgets 版一致） |

**踩过的坑（都值得记住）**：

1. **QML 会 percent-encode image source**：请求串里的 `|` 变 `%7C`、标准 base64 的
   `+` 变空格、`/` 被当路径段。解法：**base64url**（`QByteArray::Base64UrlEncoding |
   OmitTrailingEquals`）+ `.` 分隔符（base64url 字母表没有 `.`）。
2. **APK 里没有 QtQuick.Controls**：repack 路径不会重跑 androiddeployqt，base APK
   只带 qtquick2plugin。`ScrollIndicator is not a type` → 用 `visibleArea.yPosition`
   自绘指示条，不要 import Controls。
3. **卡片墙用 GridView 不是 ListView**：ListView 是单列列表，cellWidth 是 GridView
   的属性；第一版误用 ListView 导致单列大卡。
4. **构造期 width()≈0**：ApplyGeometry 初次 avail=100 → w=187 大卡；resizeEvent
   （触摸模式 + 宽度变化）随后重算，QML 属性经 rootObject()->setProperty 生效。

### 15.5 真机验收（V2463A）

- **滚动 60fps**：真实搜索 65 部长列表，录屏 286 帧/16.7s，中位间隔 17ms。
- 点击卡片 → 详情页（真实数据、标签/简介/章节完整）；长按 → 三项菜单正常。
- 交付包 `JMComic-ui-0.90-arm64-v8a-quic-release.apk`（66,405,671 B，SHA256
  前缀 `16a0a8ec…`）。
- **M1 全链路冒烟 `PASS 19 / FAIL 0 / SKIP 0`**（QML 列表替换后：首页滚动/点卡片
  进详情/长按菜单/阅读器/横屏全部通过）。

### 15.6 已知边界

- 首页 7 个分区 = 7 个 QQuickWidget（各持独立 GL FBO）；隐藏页 scene graph 不渲染，
  真机未见内存异常，后续若出现可改懒加载。
- 封面 LRU 60 张（约 70MB QImage），超出的封面回滚占位色再预取。
- `QtQuick.Controls` 依赖被刻意避开；若未来需要 Controls 组件，必须改用
  `build_android.sh` 正常打包（androiddeployqt 会按 QML import 自动带模块）。
