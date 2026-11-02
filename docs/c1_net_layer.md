# C1 Python 桥协议 v1

- 版本：v1（`protocol: 1`）
- 日期：2026-11-02
- 适用：`android/c1_shell/bridge.py`（Python 侧）↔ `android/c1_shell/python_bridge.{h,cpp}`（C++ 侧）
- 来源：C1-W6 的行协议 + C1-P4（W6 真实网络接入）

---

## 1. 传输

- C++ 用 `QProcess` 拉起 `libpythonbin.so bridge.py`，stdin/stdout 各一行一个 JSON 对象（UTF-8，`\n` 结尾）。
- stderr 不参与协议，只作为诊断输出。
- **单通道**：C++ 侧同时只允许一个 in-flight 请求。已有请求未完成时再次 `SendRequest` 会直接失败并给出 `bridge busy: <method> pending`，而不是覆盖 `pendingId_`。

## 2. 请求

```json
{"id": 7, "method": "net.request", "params": {"url": "https://example.com/"}}
```

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | int | 由 C++ 侧自增，响应必须原样带回 |
| `method` | string | 见 §4 |
| `params` | object | 可选，缺省为 `{}` |

## 3. 响应

成功：

```json
{"id": 7, "method": "net.request", "ok": true, "result": {...}, "protocol": 1}
```

失败：

```json
{"id": 7, "method": "net.request", "ok": false,
 "error": {"code": "TimeOut", "message": "...", "attempts": 2, "elapsed_ms": 1400},
 "protocol": 1}
```

约定：

1. **响应恒定回带 `method`**。P4 之前的版本不回带，导致 `main.cpp` 里 `method == "ping"` 的分支永远不成立；只靠 `id` 关联时，调用方无法判断回来的到底是哪个方法。
2. `error` 是**对象**（旧版是字符串）。`code` 取自 `src/tools/status.py` 的 `Status` 名字，便于与生产日志同一套词表读。
3. 非法 JSON 也必须有响应：`{"id": null, "method": null, "ok": false, "error": {"code": "UserError", ...}}`。
4. 未知方法返回 `UserError`，不静默丢弃。

### 3.1 错误码

| code | 触发条件 |
|---|---|
| `UserError` | 参数非法（scheme 非 http/https、method 不支持、timeout≤0）、未知方法、坏 JSON |
| `DnsError` | 域名解析失败 |
| `TimeOut` | 总预算内未拿到响应 |
| `ConnectErr` | 连接被拒 / 不可达 |
| `ResetErr` | 连接被重置 |
| `SSLErr` | TLS 握手失败 |
| `NetError` | 其它传输层异常 |
| `ProxyError` | 代理配置错误（预留给 C3） |

## 4. 方法

### 4.1 `ping` / `echo` / `crash`（W6 原有，保持兼容）

| 方法 | params | result |
|---|---|---|
| `ping` | — | `"pong"` |
| `echo` | 任意 object | 原样返回 |
| `crash` | — | 无响应：进程以 `SystemExit(3)` 退出，用于验证崩溃检测与自动重启 |

### 4.2 `net.request`（P4 新增）

params：

| 字段 | 默认 | 说明 |
|---|---|---|
| `method` | `GET` | GET/POST/PUT/DELETE/PATCH/HEAD |
| `url` | 必填 | 仅 http/https |
| `headers` | `{User-Agent: JMComic-C1-Bridge/1.0}` | 覆盖式合并 |
| `body` | 无 | 字符串，或 object/array（自动 JSON 序列化并补 `Content-Type`） |
| `timeout_ms` | `10000` | **总预算**，含重试；不是每次尝试各自超时 |
| `retries` | `1` | 额外尝试次数；只在 `DnsError/TimeOut/ConnectErr/ResetErr` 上重试，4xx/5xx 不重试 |
| `max_bytes` | `262144` | 响应体读取上限，超出置 `truncated` |
| `include_body` | `true` | false 时结果不带 `body`（避免桥里塞大对象） |
| `binary` | `false` | true 时用 `body_b64` 代替 `body` |
| `proxy` | 无 | 显式代理；**不继承**环境变量，保证行为可复现 |

result：

| 字段 | 说明 |
|---|---|
| `status` | `Ok` 表示拿到了响应（含 4xx/5xx） |
| `http_status` | HTTP 状态码；0 表示没拿到响应 |
| `final_url` | 跟随重定向后的 URL |
| `headers` | 响应头 |
| `body` / `body_b64` | 响应体（受 `include_body`/`binary`/`max_bytes` 约束） |
| `body_len` | 实际读到的字节数 |
| `truncated` | 是否被 `max_bytes` 截断 |
| `http_error` | true 表示 HTTP 状态码 ≥400（仍算 `ok: true`，因为"拿到了响应"） |
| `elapsed_ms` | 端到端耗时（含重试） |
| `attempts` | 实际尝试次数 |
| `backend` | 当前恒为 `stdlib` |

> 4xx/5xx 返回 `ok: true` 是刻意的：协议层把"网络不通"和"服务器说没有"分开，业务层才有机会读 403 的响应体做诊断。旧版把一切都塞进 `ok:false` 会让这两种情况无法区分。

### 4.3 `net.status`

result：`protocol_version / python / backend / pid / uptime_ms / rss_kb / stats{requests, failures, last_error}`。

`rss_kb` 读 `/proc/self/statm`；读不到时返回 `-1`（不伪造 0）。

### 4.4 `net.close`

回 `{"closing": true}` 后进程 `break` 退出（退出码 0）。用于验证"优雅关闭"：C++ 侧应观察到 `IsRunning() == false` 且没有 `Crashed` 信号。

## 5. 超时责任划分

| 层 | 超时 | 语义 |
|---|---|---|
| Python `net_client` | `timeout_ms` | 总预算，按尝试次数均分（每次至少 200ms） |
| C++ `NetRequest` | `timeoutMs` | 兜底：若 Python 没在预算内回话（卡死/崩溃），由 C++ 报 `request timeout method=net.request` |

`NetRequest` 会把这个预算减去 500ms 作为 Python 侧 `timeout_ms`，**保证先由 Python 报出带错误码的超时**；C++ 侧超时意味着"子进程没回话"，是另一类故障。

## 6. 进程生命周期

| 场景 | 行为 |
|---|---|
| Python 抛未捕获异常 / `SystemExit(non-zero)` | `Crashed` 信号 + 自动重启（`QProcess::ExitStatus` 是 `NormalExit`，**只判 `CrashExit` 会漏掉这种最常见的形态**） |
| 被信号杀死 | 同上 |
| `Stop()` 主动停止 | 不触发 `Crashed`、不重启 |
| `net.close` 正常退出 | 不触发 `Crashed`；`proc_` 置空 |
| 重启 | 100ms 后重启一次，`errorOccurred` 与 `finished` 双触发有去抖 |

## 7. 已知边界（不假装覆盖）

1. **TLS 指纹**：`net_client` 只用 stdlib urllib，不做 `curl_cffi` 的浏览器指纹伪装。真实站点若做指纹校验，需要用 C3 的收编方案替换后端，届时只换 `net_client`，协议不变。
2. **并发**：单通道一次一个请求；分页预取/并发下载需要时再扩展（届时改 `id` 关联为 `id→callback` 表）。
3. **端口/流式**：不支持流式响应与断点续传，属 C3。

## 8. 验证方式

| 层 | 命令 | 覆盖 |
|---|---|---|
| 协议级（无 Qt） | `python android/c1_shell/host_test_net_client.py` | 方法分发、错误码、重试次数、截断、优雅关闭 |
| 进程级（真实 `PythonBridge`） | `bash android/c1_shell/host_tests/run_host_tests.sh` | 起停、ping/echo、net.request 200/404/超时、net.status、crash+重启、net.close |
| 设备级 | `docs/c1_w7_report.md` 的 F9/F10 与 P4-5 用例 | 真机上一次真实请求 |
