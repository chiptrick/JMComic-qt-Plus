# C++ 版 TLS/QUIC 替换与 ECH/HTTP3 集成方案

> 状态：**P1 已完成（Windows）**——OpenSSL 4.1 ECH 通道已接入 `NodeHttp`，
> 实测 `SSL_ECH_STATUS_SUCCESS`（www.cdnhjk.net，TLS1.3，ALPN http/1.1，证书校验通过）；
> **P2+（QUIC/HTTP-3）尚未实现**，仍缺 ngtcp2/nghttp3。
> 本文同时记录实现细节与踩坑，供 P2 继续。

## 0. 本机实测结论（决定"能不能做"）

| 检查项 | 实测结果 |
|---|---|
| Qt 实际 TLS 后端 | `tls=openssl/OpenSSL 3.5.5`（Qt 加载了 Git 自带的 `libssl-3-x64.dll`） |
| OpenSSL 3.5.9（Win64） | **no-ech**：`SSL_set1_ech_config_list` / `SSL_ech_get1_status` 均未导出 |
| OpenSSL 4.1（OpenSSL-4.1） | **带 ECH**：两个 API 均导出，`ech.h` 含 `SSL_ech_set1_server_names` / `SSL_ech_set1_outer_alpn_protos`；用 `QLibrary` 动态加载即可，无需导入库 |
| Qt 的 `qopensslbackend.dll` | 不引用任何 ECH 符号（`strings` 无 `SSL_set1_ech_config_list`） |
| OpenSSL 开发包 | 无：`/mingw64/include/openssl` 与 `libssl.dll.a` 均不存在，只有 `openssl.exe` |
| QUIC 库 | 无：Qt / vcpkg / msys2 / 项目内都没有 ngtcp2、nghttp3、quiche |
| 网络 | `git ls-remote https://github.com/...` → `CONNECT tunnel failed, response 502` |
| 系统 curl | 8.18.0 Schannel，Features 无 HTTP3，`--http3-only` 报 "does not support" |
| QtNetwork | 无 `Http3AllowedAttribute`，二进制无 nghttp3/ngtcp2 符号 |

**结论**：加密层要支持 ECH，必须换来一份**带 ECH 的 OpenSSL 3.2+**（或 BoringSSL）；
传输层要支持 HTTP/3，必须引入 **ngtcp2 + nghttp3**。这两项在本机既无库也无下载通道，
因此无法在本次交付里"真实替换"。已交付的是可编译的**后端边界、能力探测与回退**，
以及可直接照做的替换方案与依赖清单。

## 1. 模块与接口边界

### 1.1 现状（已落地）

```
cpp/shell/net_backend.{h,cpp}      能力探测 + ECH 注入入口（本轮新增）
cpp/shell/doh_client.{h,cpp}       DoH / HTTPS RR → ECHConfigList（已有）
cpp/shell/node_http.{h,cpp}        HTTP/1.1 over QSslSocket（现有实际传输）
cpp/shell/net_client.{h,cpp}       QNAM 路径（HTTP/2，作为回退）
cpp/shell/ui/proxy_config.*        配置、优先级、回退状态
cpp/shell/ui/proxy_page.*          高级面板（DOH / ECH / HTTP3）
```

`net_backend` 暴露：

```cpp
struct TlsCapabilities { QString backend, version, note;
                         bool tls13, ech, echRuntime, earlyData, quicTls; };
struct QuicCapabilities { bool available, http3, zeroRtt;
                          QString stack, version, note; };
namespace NetBackend {
  TlsCapabilities Tls();      QuicCapabilities Quic();
  QString Summary();          // tls=openssl/OpenSSL 3.5.5 ech=0 echLib=0 quic=0 ...
  bool SetEchConfig(const QByteArray &list);  // ECHConfigList 注入点
  QString LastError();
}
```

`DohClient` 拿到 ECHConfigList 后**真实调用** `NetBackend::SetEchConfig()`，
日志输出 `ech config ok len=96 injected=0 (...)`；页面与自检直接显示 `Summary()`。
这样"能不能 ECH"由运行时决定，不再写死，也不会出现假开关。

### 1.2 目标架构（依赖到位后）

```
                 ┌─────────────────────────────┐
 业务层          │ NetClient / NodeHttp / DoH  │  只依赖下面两个抽象
                 └──────────────┬──────────────┘
                                │
        ┌───────────────────────┴───────────────────────┐
        │                                               │
┌───────▼────────┐                            ┌─────────▼──────────┐
│  TlsChannel    │                            │  QuicTransport     │
│ (抽象接口)      │                            │ (抽象接口)          │
└───────┬────────┘                            └─────────┬──────────┘
        │                                               │
┌───────▼─────────┐   ┌──────────────────┐   ┌──────────▼─────────┐
│ OpenSslTlsChannel│  │ QtSslChannel     │   │ Ngtcp2Transport    │
│ (3.2+, ECH/0-RTT)│  │ (现状回退)        │   │ (ngtcp2+nghttp3)   │
└──────────────────┘  └──────────────────┘   └────────────────────┘
```

建议新增文件（本轮未建，依赖到位后按此落地）：

| 文件 | 职责 |
|---|---|
| `cpp/shell/tls/tls_channel.h` | `TlsChannel` 纯接口：`Connect(host, port, alpn, echConfig, session)`、`Read/Write`、`Info()` |
| `cpp/shell/tls/openssl_tls.cpp` | OpenSSL 3.2+ 实现：`SSL_set1_ech_config_list`、`SSL_set_quic_tls_cbs`、0-RTT |
| `cpp/shell/tls/qt_tls.cpp` | `QSslSocket` 实现，保持现有行为作为回退 |
| `cpp/shell/quic/quic_transport.h` | `QuicTransport` 接口：`Connect`、`OpenStream`、`ReadStream`、`Close`、`Stats` |
| `cpp/shell/quic/ngtcp2_transport.cpp` | ngtcp2 连接/流/定时器/丢包重传；TLS 回调接到 `ngtcp2_crypto_*` |
| `cpp/shell/http3/http3_client.cpp` | nghttp3 封装：控制流、QPACK、HEADERS/DATA、GOAWAY |
| `cpp/shell/net_backend.cpp` | 工厂：按配置/能力返回具体实现；`JM_HAVE_NGTCP2` 编译开关 |

**边界原则**：

1. 业务层不出现 `SSL_*`、`ngtcp2_*`、`nghttp3_*` 符号，只依赖 `TlsChannel` / `QuicTransport`。
2. `NetClient`（QNAM）保留为**回退实现**，HTTP/3 不可用时零改动可用。
3. `NodeHttp` 的 HTTP/1.1 解析保持不变，只把"建连 + 读写"换成 `TlsChannel`。
4. 能力探测结果进 `NetBackend`，页面与自检共用同一份，避免多处判断漂移。

## 2. ECH 配置与密钥分发

### 2.1 配置来源

ECH 配置来自 DNS 的 **HTTPS RR（type 65）** 中 `SvcParamKey=5 (ech)`，
值就是 `ECHConfigList`（可能含多个 `ECHConfig`，每个带 `config_id`、`public_name`、
`kem_id`、`public_key`、`cipher_suites`）。现有 `DohClient` 已完整实现：

- `POST <DoH> application/dns-message`，qtype=65，域名 `cloudflare-ech.com`；
- 解析 answer → HTTPS rdata → `priority + target + SvcParams`；
- key=5 的 value 原样 base64 → `proxy/echConfig` 持久化；
- 成功 15 分钟刷新、失败 1 分钟重试（对齐 Python `InitEchConfigBack`）。

### 2.2 分发与选择

```
DohClient.FetchEchConfig()
   └─ ECHConfigList(base64)
        ├─ NetBackend::SetEchConfig(list)     ← TLS 后端注入
        ├─ ProxyConfig::SetEchConfig(base64)  ← 落盘，重启可用
        └─ TlsChannel::Connect(..., list, publicName)
```

- **不是密钥分发**：ECHConfigList 是公开参数（含服务端公钥），不需要保密；
  真正需要保护的是 DoH 通道本身（必须校验证书、禁用降级）。
- 选择逻辑：按 `config_id` 匹配；客户端不知道服务端用哪个，TLS 栈会依次尝试
  `ECHConfigList` 中的条目（OpenSSL 内部处理）。
- 外层 SNI：用 `public_name`（Cloudflare 的公共名），内层 SNI 才是真实域名。
- 失效处理：`SSL_ech_get1_status()` 返回 `SSL_ECH_STATUS_NOT_TRIED / FAILED` 时，
  重新拉一次 HTTPS RR；仍失败则本次回退普通 SNI，并置 `echOff`（保存设置可重试）。

### 2.3 注入点（OpenSSL 3.2+）

```cpp
SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
SSL *ssl = SSL_new(ctx);
// 关键：list 来自 DoH，长度是解码后的字节数
SSL_set1_ech_config_list(ssl, list.constData(), size_t(list.size()));
SSL_set_tlsext_host_name(ssl, publicName);   // 外层 SNI
SSL_connect(ssl);
switch (SSL_ech_get1_status(ssl)) {
case SSL_ECH_STATUS_SUCCESS:   /* 内层握手成功 */ break;
case SSL_ECH_STATUS_GREASE:    /* 服务端未启用，等同普通 TLS */ break;
default:                       /* 拒绝/失败 → 回退普通 SNI 重连 */ break;
}
```

## 3. QUIC 与 TLS 的集成点

以 **ngtcp2 + OpenSSL 3.5（quictls 风格 QUIC-TLS 回调）** 为例：

### 3.1 握手

1. `ngtcp2_conn_client_new()` 创建连接，设置 `ngtcp2_settings`（initial max data、
   max streams、idle timeout）与 `ngtcp2_transport_params`（含 `initial_max_udp_payload_size`）。
2. TLS 侧建立 `SSL` 对象，注册 QUIC 方法/回调：
   - OpenSSL 3.5：`SSL_set_quic_tls_cbs(ssl, &ngtcp2_crypto_ossl_cbs, conn)`；
   - BoringSSL：`SSL_set_quic_method(ssl, &quic_method)`。
3. 回调 `set_encryption_secrets(level, read_secret, write_secret, len)` 把
   Initial/Handshake/1-RTT 密钥交给 ngtcp2：
   `ngtcp2_conn_install_initial_key` / `ngtcp2_conn_install_handshake_key` /
   `ngtcp2_conn_install_1rtt_key`（含 key phase）。
4. 收包：`ngtcp2_conn_read_pkt(conn, path, &pi, data, len, ts)`；解出的 CRYPTO 帧数据
   通过 `SSL_provide_quic_data(ssl, level, data, len)` 喂给 TLS；TLS 产生的
   CRYPTO 数据由 `SSL_provide_quic_data` 回调交给 ngtcp2 打包发送。
5. 发包：`ngtcp2_conn_write_pkt()` 产出 UDP payload，交给 socket 发送；
   定时器由 `ngtcp2_conn_get_expiry()` + `ngtcp2_conn_handle_expiry()` 驱动（PTO/ACK 延迟）。
6. 握手完成：`ngtcp2_conn_get_handshake_completed()`；ALPN 必须是 `h3`。

### 3.2 0-RTT

- 首次连接后保存 `SSL_SESSION`（含 ticket）与 `ngtcp2_transport_params`（含
  `early_data` 相关参数、`max_early_data_size`）。
- 再次连接：`SSL_set_session(ssl, session)` → `SSL_set_quic_early_data_context()` →
  ngtcp2 用 `NGTCP2_CONN_FLAG_EARLY_DATA` 创建连接，0-RTT 数据走
  `ngtcp2_conn_writev_stream(..., NGTCP2_WRITE_STREAM_FLAG_MORE)`。
- 判断结果：`ngtcp2_conn_get_early_data_status()`：
  - `ACCEPTED` → 0-RTT 数据有效；
  - `REJECTED` → 必须重发这些请求（幂等请求才允许 0-RTT）。
- 安全约束：只对幂等请求（GET/HEAD）用 0-RTT；登录、收藏等 POST 禁用 early data，
  避免重放（服务端也可能拒绝）。
- ECH 与 0-RTT 可叠加：ECH 只影响 ClientHello 外层，0-RTT 走内层密钥。

### 3.3 密钥更新与连接迁移

- 密钥更新：`ngtcp2_conn_initiate_key_update()`；TLS 回调 `set_encryption_secrets`
  在 `level=1RTT` 且 key phase 变化时安装新密钥。
- 连接迁移：`ngtcp2_conn_set_remote_transport_params`、path validation
  （`PATH_CHALLENGE/PATH_RESPONSE`）；客户端切换 Wi-Fi/蜂窝时保持 connection id。
- 关闭：`ngtcp2_conn_write_connection_close()`，CONNECTION_CLOSE 带错误码。

## 4. HTTP/3 在 QUIC 之上的分层

```
HTTP/3 语义（nghttp3）
  ├─ 控制流（单向）：SETTINGS / GOAWAY / MAX_PUSH_ID
  ├─ QPACK：encoder 流、decoder 流 + HEADERS 内的动态表指令
  └─ 请求/响应：每个请求一个双向流
        HEADERS 帧（:method :path :authority + 普通头）
        DATA 帧（可多段）
        结束：FIN
QUIC（ngtcp2）
  ├─ 流：双向/单向、流级流量控制、RESET_STREAM/STOP_SENDING
  ├─ 可靠性：ACK、丢包检测、PTO 重传
  └─ 连接：握手、密钥更新、迁移、idle timeout
UDP
```

- **映射**：一个 `NetClient` 请求 = 一个双向流；响应 HEADERS/DATA 累积后交给现有
  `NetResult`（body/httpStatus/headers）。
- **QPACK 阻塞**：动态表未就绪时 HEADERS 可能阻塞，需要 `nghttp3_conn_read_stream`
  持续推进并处理 `NGHTTP3_ERR_QPACK_DECOMPRESSION_FAILED`。
- **优先级**：HTTP/3 `priority` 参数（RFC 9218）映射到 ngtcp2 流调度；
  图片/章节页可给低优先级，API 高优先级。
- **Alt-Svc 学习**：HTTP/2 响应头 `alt-svc: h3=":443"; ma=86400` 缓存到域名表；
  首次仍走 HTTP/2，后续尝试 HTTP/3（与 Python 的 `isH3` 逻辑一致）。
- **代理**：HTTP 代理不支持 UDP；只有 SOCKS5 `UDP ASSOCIATE` 才能承载 QUIC，
  否则直接回退 HTTP/2（Python `SetCurlOpt` 同样在代理下不开 H3）。

## 5. 兼容性与回退策略

| 场景 | 行为 |
|---|---|
| 后端无 ECH（现状） | `EchOff=false` 但 `ech=false`；页面显示"后端不支持，已回退普通 TLS"；不重试 |
| ECH 被拒（`ECH_REJECTED`） | 置 `echOff=true`，本次用普通 SNI 重连；保存设置/重启后重试 |
| 后端无 QUIC | `http3=false`；开关可保存但请求走 HTTP/2（QNAM）；首次成功打印 `transport: HTTP/2` |
| HTTP/3 握手失败 / UDP 被封 | `http3Off=true`，本次回退 HTTP/2，不阻塞请求；下次启动重试 |
| 代理模式（HTTP/Socks5 无 UDP） | 直接禁用 HTTP/3；SOCKS5 仅在支持 UDP ASSOCIATE 时尝试 |
| 证书错误 | 不降级、不忽略；ECH 外层证书必须匹配 `public_name`，内层匹配真实域名 |
| 线路 6 反代 | 沿用现有优先级（反代 > IP 覆盖 > DoH > 直连），QUIC 只替换传输，不改路由 |
| 旧系统/旧 CPU | 以 `NetBackend` 能力为准；无 TLS1.3 时 ECH/0-RTT 自动不可用 |

兼容性原则：**任何高级特性失败都只影响本次请求的传输方式，不改业务结果**；
`NetClient`/`NodeHttp` 保持同一套 `NetResult`，上层（页面、登录、收藏、阅读）零改动。

## 6. 需要验证的关键测试场景

| # | 场景 | 判据 |
|---|---|---|
| 1 | DoH HTTPS RR 解析 ECHConfigList | 已实现：`dohquery` 确定性自检 + 网络 `ech config ok len=96` |
| 2 | ECH 注入成功 | `SSL_ech_get1_status()==SUCCESS`；抓包看不到内层 SNI |
| 3 | ECH 被拒 / GREASE | 状态码正确；回退普通 SNI 后请求成功；`echOff` 置位 |
| 4 | TLS1.3 + ALPN 协商 | 客户端拿到 `h3`（QUIC）或 `h2`（TCP） |
| 5 | QUIC 握手 | Initial/Handshake/1-RTT 完成；`ngtcp2_conn_get_handshake_completed()` |
| 6 | 丢包/重传/PTO | 注入 10% 丢包仍能完成握手与请求 |
| 7 | 0-RTT accept | `EARLY_DATA_ACCEPTED`；首字节时间明显低于 1-RTT |
| 8 | 0-RTT reject / 重放 | `EARLY_DATA_REJECTED` 时重发幂等请求；POST 不使用 early data |
| 9 | HTTP/3 请求/响应 | HEADERS+DATA 正确解码；QPACK 动态表可用；GOAWAY 优雅关闭 |
| 10 | 回退 | UDP 443 阻断 → HTTP/2 成功；ECH 拒绝 → 普通 TLS 成功 |
| 11 | 证书校验 | 错误证书/错误 public_name 必须失败，不得降级 |
| 12 | 现有回归 | `c1_ui_desktop_suite.py` 88 项保持 PASS 0 FAIL |

## 7. 依赖与落地步骤

### 7.1 需要补齐的依赖

1. **OpenSSL 3.5 且启用 ECH**（不能是 `no-ech`）：
   ```bash
   perl Configure mingw64 enable-ech --prefix=/c/deps/openssl-ech
   make -j && make install_sw
   ```
   Android（NDK r26d）：
   ```bash
   export ANDROID_NDK_ROOT=$LOCALAPPDATA/Android/Sdk/ndk/30.0.16248370
   perl Configure android-arm64 -D__ANDROID_API__=34 enable-ech \
        --prefix=/c/deps/openssl-ech-arm64
   make -j && make install_sw
   ```
   验证：`nm -D libssl.so | grep SSL_set1_ech_config_list` 必须有输出。

2. **ngtcp2 + nghttp3**（GitHub 当前 502，需要代理/镜像）：
   ```bash
   git clone https://github.com/ngtcp2/nghttp3 && cd nghttp3
   cmake -B build -DENABLE_LIB_ONLY=ON -DCMAKE_INSTALL_PREFIX=/c/deps/nghttp3
   cmake --build build -j && cmake --install build

   git clone https://github.com/ngtcp2/ngtcp2 && cd ngtcp2
   cmake -B build -DENABLE_OPENSSL=ON -DOPENSSL_ROOT_DIR=/c/deps/openssl-ech \
         -DCMAKE_INSTALL_PREFIX=/c/deps/ngtcp2
   cmake --build build -j && cmake --install build
   ```
   Android 交叉编译把 `-DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake
   -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-34` 加进去。

3. **Qt 侧切换**：不建议给 Qt 打补丁暴露 ECH；直接走自建 `TlsChannel`（OpenSSL），
   `NetClient` 的 QNAM 路径保留 HTTP/2 回退。

### 7.2 CMake 接入（草案）

```cmake
option(JM_WITH_ECH "OpenSSL with ECH" OFF)
option(JM_WITH_HTTP3 "ngtcp2 + nghttp3" OFF)
if(JM_WITH_ECH)
  find_package(OpenSSL 3.2 REQUIRED)
  target_compile_definitions(jmcomic_ui PRIVATE JM_HAVE_ECH=1)
  target_sources(jmcomic_ui PRIVATE tls/openssl_tls.cpp)
endif()
if(JM_WITH_HTTP3)
  find_path(NGTCP2_INCLUDE ngtcp2/ngtcp2.h REQUIRED)
  find_library(NGTCP2_LIB ngtcp2 REQUIRED)
  find_library(NGHTTP3_LIB nghttp3 REQUIRED)
  target_compile_definitions(jmcomic_ui PRIVATE JM_HAVE_NGTCP2=1
                             JM_NGTCP2_VERSION="1.x")
  target_sources(jmcomic_ui PRIVATE quic/ngtcp2_transport.cpp http3/http3_client.cpp)
  target_link_libraries(jmcomic_ui PRIVATE ${NGTCP2_LIB} ${NGHTTP3_LIB})
endif()
```

### 7.3 分阶段实施与验收

| 阶段 | 内容 | 验收 |
|---|---|---|
| P0（已完成） | 能力探测、ECH 注入入口、回退状态、DoH 配置获取 | 88 项回归 + 第 9 节 6 项 |
| P1 | OpenSSL(ECH) `TlsChannel`，`NodeHttp` 接入 | 场景 2/3/4/11 通过，91~95 项回归 |
| P2 | ngtcp2 QUIC 连接 + 1-RTT | 场景 5/6 通过；HTTP/3 请求可发 |
| P3 | nghttp3 HTTP/3 + QPACK | 场景 9/10 通过 |
| P4 | 0-RTT + ECH + H3 组合、Alt-Svc 缓存 | 场景 7/8 通过；首字节对比报告 |

## 7.4 P1 实现（已完成，Windows）

新增 `cpp/shell/tls/tls_channel.h` + `cpp/shell/tls/openssl_tls.cpp`：

- `OpenSslEchChannel`：`QTcpSocket` 负责传输，TLS 用 **memory BIO** 驱动
  （`BIO_s_mem` → `SSL_set_bio` → `BIO_write/SSL_read` → `BIO_ctrl(PENDING)/BIO_read` 写回 socket）。
- **动态加载** OpenSSL 4.1（`QLibrary`，先 `libcrypto-4-x64.dll` 再 `libssl-4-x64.dll`），
  不链接、不改 Qt；`JM_OPENSSL_ECH_DIR` 可覆盖安装目录。
- 关键调用顺序（缺一不可，均已实测）：
  1. `SSL_CTX_ctrl(ctx, 123, 0x0304)` 锁 TLS1.3 —— 不锁会退到 1.2，服务端回 `illegal_parameter`；
  2. `SSL_set1_ech_config_list(ssl, ecl, len)`；
  3. `SSL_ech_set1_server_names(ssl, inner, outer, 0)` —— inner=真实域名，outer=ECHConfig 的 `public_name`；
  4. `SSL_set_alpn_protos` + `SSL_ech_set1_outer_alpn_protos` —— 缺 outer ALPN 同样 `illegal_parameter`；
  5. `SSL_set_bio` + `SSL_set_connect_state` + `SSL_do_handshake` 循环；
  6. 握手后 `SSL_ech_get1_status(ssl, &inner, &outer)` 判定，`SSL_get_verify_result` +
     `X509_check_host(cert, inner)` 做证书校验。
- CA：`JM_CA_BUNDLE` → Git 的 `mingw64/etc/ssl/certs/ca-bundle.crt` → Qt 系统证书临时 PEM →
  `SSL_CTX_set_default_verify_paths`。
- `NodeHttp` 接入：`CachedEch()` 非空且通道可用时走 ECH 通道（ALPN 只给 `http/1.1`，
  因为 NodeHttp 是 HTTP/1.1 客户端），否则回退 `QSslSocket`；
  收包统一走 `NodeHttp::Consume()`，与明文路径共用同一套头/体解析。
- **踩坑**：`SSL_read` 返回 `ZERO_RETURN` 时，必须先 `emit ReadyRead()` 再
  `emit Disconnected()`；顺序反了会让最后一批响应体丢失，表现为
  `closed mid-body`（ECH + chunked 必现）。
- 能力探测：`NetBackend::Tls()` 检测到通道可用时返回
  `tls=openssl-ech/4.1.0 ech=1 echLib=1`，页面/自检据此显示"ECH 开"。

## 8. 本轮实际改动

- 新增 `cpp/shell/net_backend.{h,cpp}`：TLS/QUIC 能力探测、`SetEchConfig` 注入点、
  `Summary()`；`echRuntime` 用 `QLibrary` 直接查 `SSL_set1_ech_config_list` 导出。
- `ProxyConfig::AdvancedTlsSupported()` 改为真实探测；`AdvancedStatus()` 末尾附带
  `tls=... ech=... echLib=... quic=...`。
- `DohClient` 拿到配置后真实调用 `SetEchConfig`，日志输出 `injected=0/1` 与原因。
- 自检新增 backend 字段，页面状态与自检同源。
- 本文档；`docs/cpp_ui_parity.md` §24 摘要。

## 9. 结论

- **P1 已完成（Windows）**：OpenSSL 4.1 通道 + ECH 注入接入 `NodeHttp`，
  实测 `www.cdnhjk.net`：`TLSv1.3 / ALPN http/1.1 / ech=SUCCESS / verify=0/1`，
  `/album` 返回 200、80 章。页面/自检显示 `tls=openssl-ech/4.1.0 ech=1 echLib=1`。
- **未完成**：P2~P4（QUIC/HTTP-3）。仍缺 ngtcp2/nghttp3（GitHub 502，本机无包），
  `QuicCapabilities.available` 保持 false，HTTP3 开关按设计回退 HTTP/2。
- **Android**：`openssl_tls.cpp` 已编入两个 ABI；MuMu 实测启动正常、无崩溃，
  `doh: ech config ok len=71 injected=0 (OpenSSL 3.3.1 为 no-ech 构建…)`，
  即 `CreateEchChannel()` 返回 nullptr 后自动回退 `QSslSocket`，业务不受影响。
  要让 Android 也走 ECH，需按 §7.1 交叉编译 `enable-ech` 的 OpenSSL 4.1 并打进 APK。
- 业务层与页面已按接口解耦：补齐 QUIC 依赖后只需实现 `QuicTransport` 并打开
  `JM_HAVE_NGTCP2`，无需再改 UI / 配置 / 路由。

## 10. P2 + P3 落地：QUIC 传输与 HTTP/3（已完成，Windows）

### 10.1 依赖构建（本机实测命令）

```bash
# 1) OpenSSL 4.1 只有 DLL，先用 gendef/dlltool 造 MinGW 导入库
gendef "C:/Program Files/OpenSSL-4.1/bin/libssl-4-x64.dll"
dlltool -d libssl-4-x64.def -D libssl-4-x64.dll -l libssl.dll.a
#   （libcrypto 同理；输出到 deps/openssl41-mingw/lib，头文件从安装目录拷入 include）

# 2) nghttp3（静态库，x86_64 可带 AVX2）
cmake -S nghttp3 -B build/nghttp3 -G Ninja -DCMAKE_BUILD_TYPE=Release   -DENABLE_LIB_ONLY=ON -DENABLE_SHARED_LIB=OFF -DENABLE_STATIC_LIB=ON -DBUILD_TESTING=OFF   -DCMAKE_C_FLAGS="-O2 -mavx2" -DCMAKE_INSTALL_PREFIX=deps/quic-mingw
cmake --build build/nghttp3 --target install

# 3) ngtcp2（OpenSSL 后端，会自动探测 SSL_set_quic_tls_cbs）
cmake -S ngtcp2 -B build/ngtcp2 -G Ninja -DCMAKE_BUILD_TYPE=Release   -DENABLE_LIB_ONLY=ON -DENABLE_SHARED_LIB=OFF -DENABLE_STATIC_LIB=ON -DBUILD_TESTING=OFF   -DENABLE_OPENSSL=ON -DOPENSSL_ROOT_DIR=deps/openssl41-mingw   -DCMAKE_INSTALL_PREFIX=deps/quic-mingw
cmake --build build/ngtcp2 --target install
```

配置阶段的判定行是 `Looking for SSL_set_quic_tls_cbs - found`：它命中即启用 `libngtcp2_crypto_ossl`
（OpenSSL 3.5+/4.x 的 QUIC-TLS 回调接口），没命中会退回 quictls 并不可用。

### 10.2 模块边界（新增 `cpp/shell/quic/`）

| 层 | 文件 | 职责 |
|---|---|---|
| 业务门面 | `quic/quic_http.{h,cpp}` | `quic::Client::Start(Request, cb)`；pimpl 隐藏 ngtcp2 头，Android 编译成 `Available()==false` 的桩 |
| 连接/传输 | 同上（`Impl`） | QUdpSocket + ngtcp2_conn：握手、丢包重传、流控、计时器 |
| TLS | 同上 | 进程内直连 OpenSSL 4.1：`SSL_set_quic_tls_cbs`、TLS1.3、ALPN=h3、可选 ECH |
| HTTP/3 | 同上 | nghttp3 client：control/QPACK 三条单向流、请求双向流、HEADERS/DATA 解析 |
| 路由 | `node_http.cpp` | 开关打开且有 QUIC 栈时先试 H3，失败回退 TLS/HTTP1.1（`allowQuic=false` 防循环） |

`NodeHttp` 的 H3 分支只在 `https + GET + 无 body + Http3Enabled()` 时启用；3xx 仍由
`Settle()` 统一跟随重定向，`job->location` 由 H3 的 `location` 首部回填。

### 10.3 与库的集成点（ngtcp2 1.26.90 / nghttp3 1.19.90）

- **握手驱动**：不需要手动 `SSL_do_handshake`。ngtcp2 在 `conn_recv_crypto_data`
  里对空数据调用一次，触发 `client_initial` → `SSL_do_handshake`，ClientHello 经
  `OSSL_FUNC_SSL_QUIC_TLS_CRYPTO_SEND` 回调写回 ngtcp2 的 CRYPTO 流。
- **密钥导出**：`ngtcp2_crypto_ossl_configure_client_session()` 注册 QUIC-TLS 回调，
  `YIELD_SECRET` 把 Initial/Handshake/1-RTT 密钥交给 ngtcp2 安装。
- **收发**：`ngtcp2_conn_read_pkt` 收包；发送用 `nghttp3_conn_writev_stream` 取
  `nghttp3_vec`，再 `ngtcp2_conn_writev_stream` 组包，`NGTCP2_ERR_WRITE_MORE` 时
  回填 `nghttp3_conn_add_write_offset`。
- **计时器**：`ngtcp2_conn_get_expiry2()` → QTimer → `ngtcp2_conn_handle_expiry()`。

### 10.4 调试中定位的四个必需要点（缺一即失败）

1. **`SSL_set_app_data(ssl, &conn_ref)` 必须挂**：ossl 后端的 CRYPTO_SEND/RECV 回调
   通过 `SSL_get_app_data` 取 `ngtcp2_crypto_conn_ref` 反查连接；缺失时首个包就
   `writev_stream: ERR_CALLBACK_FAILURE`（SSL 错误是含糊的 `internal error`）。
2. **本地地址要问内核**：`QUdpSocket` 在 `bind(Any)` 后 `localAddress()` 仍是 `0.0.0.0`，
   用 `getsockname(socketDescriptor())` 取真实本地 IP 填进 `ngtcp2_path`。
3. **收口不能在回调栈里做**：`nghttp3` 的 `end_stream`/`stream_close` 回调返回后库还要用
   连接对象，直接 `ngtcp2_conn_del` 会 use-after-free（表现为请求成功后进程崩溃）。
   改为 `QTimer::singleShot(0, ...)` 延后 `Finish()`。
4. **必须有 `NGTCP2_STATICLIB` / `NGHTTP3_STATICLIB`**：否则头文件按 `dllimport` 声明，
   链接静态库时报 `__imp_ngtcp2_*` 未定义。

### 10.5 ECH 与 HTTP/3 的组合结论（如实记录）

| 通道 | 同配置（71B ECHConfigList）实测 |
|---|---|
| TLS/TCP（`openssl_tls.cpp`） | `ech=SUCCESS`（`--ech-probe cloudflare.com`） |
| QUIC（`quic_http.cpp`） | `ech=FAILED`，连接仍以 TLS1.3+h3 成功（服务端 ECH 拒绝后回落明文 SNI） |

因此（0.89 → 0.90 上半场）`NodeHttp` 的 H3 分支**不注入** ECHConfigList：注入后
首包从约 2s 恶化到 7~16s，且状态仍是 FAILED。ECH 只在 TCP 通道启用（已验证
SUCCESS）。需要强制验证 ECH-over-QUIC 时用 `--h3-probe <host> --h3-ech`。

> **0.90 补录（`docs/cpp_ui_next_steps.md` §14.7 / `cpp_ui_parity.md` §28）**：
> 上面的实测结论仍然成立，但产品决策已变 —— H3 默认开启，业务路径重新尝试
> H3+ECH 组合；一旦 `SSL_ech_get1_status != SUCCESS` 就丢弃 H3 结果、回退
> TCP+ECH（隐藏 SNI 优先），并在下次启动/换节点时重试组合。可用
> `--h3-fail-inject ech-reject --ech-h3-probe <url>` 确定性验收。

### 10.6 验收

- 探针：`--h3-probe cloudflare.com` → `done ok=1 status=301 ... version=HTTP/3 alpn=h3 tls=TLSv1.3`
- 应用内（`ui.ini` 设 `proxy/http3=true` + `--doh-on --open-album 313833`）：
  `node-http h3 GET https://www.cdnhjk.net/album -> 200 7736B alpn=h3`，`detail-real: chapters=80`
- 自检：`proxy-align: advanced ... supported=1 ... quic=1 http3=1 stack=ngtcp2+nghttp3`
- 回退：H3 失败（超时/无 UDP）自动改走 TLS/HTTP1.1，业务结果不变；日志
  `node-http h3 fallback (...)`。

### 10.7 仍未做

- **0-RTT**：需会话票据持久化（`SSL_set_session` + `NGTCP2_CONN_FLAG_EARLY_DATA`），
  当前每请求新建连接，`QuicCapabilities.zeroRtt=false`。
- **连接复用**：一期按"一请求一连接"实现，HTTP/3 的多路复用能力尚未利用；
  连接池是后续优化项（收益主要在连续页图/封面请求）。
- **Android**：现有 OpenSSL 3.3.1 无 `SSL_set_quic_tls_cbs`，也没有带 ECH 的 4.x 交叉编译产物，
  `JM_WITH_QUIC` 在 Android 强制 OFF，`quic::Available()==false` → 业务自动走 HTTP/1.1。
- **QUIC 侧握手耗时**：首次连接含 PTO 重传时约 6~17s（Cloudflare ECH 前端更慢），
  已把 H3 单次预算压到 `min(请求超时, 8s)`，超时即回退，避免拖慢整体。
