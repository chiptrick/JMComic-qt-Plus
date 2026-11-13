# TLS / QUIC 安全复核（T2，2026-11-07）

> 对应 `docs/cpp_ui_next_steps.md` 的 T2。目的：确认引入 ECH 与 HTTP/3 之后，
> 既有安全属性没有被削弱。结论按"核查点 → 结论 → 证据"给出，证据一律是
> 代码位置或可复现命令的输出，不含推测。

## 0. 结论摘要

| # | 核查点 | 结论 |
| - | --- | --- |
| 1 | 证书错误不得被回退掩盖 | **发现 2 个缺陷，均已修**（回退遮蔽 + 会话复用跳过重校验） |
| 2 | 主机名校验用域名而非 IP | 通过 |
| 3 | CA 来源可判定 | 通过（每次连接一行日志） |
| 4 | 0-RTT 仅幂等 GET，拒绝即重发 | 通过（按首个请求限流） |
| 5 | UDP 明文暴露面 | 通过（CID 全随机，未承载业务信息） |

## 1. 证书错误不得被回退掩盖

**风险**：QUIC 通道用 `JM_CA_BUNDLE`/Git 的 CA 校验，而回退用的 `QSslSocket`
走 Qt 自己的信任库。若把"证书校验失败"当成"传输失败"回退，就可能出现
"QUIC 侧证书错误 → 回退后成功"，等于用另一套 CA 把请求做成成功。

**核查**：

- QUIC 侧确实校验：`quic_http.cpp:953` `SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr)`。
- 用一份无关自签 CA（`docs/testdata/tls/not-a-ca.pem`）冒充信任锚时，QUIC 握手失败：

  ```
  h3-probe: done ok=0 status=0 bytes=0 983ms version=HTTP/3 alpn= tls= ech=
    err=read_pkt: ERR_CRYPTO ssl=error:0A000086:SSL routines::certificate verify failed verify=20
  ```

  说明证书链校验真实生效（`verify=20` = unable to get local issuer certificate）。
- TCP/ECH 通道同样失败：同一 `JM_CA_BUNDLE` 下 `--ech-probe` 的输出为
  `tls ca path=...not-a-ca.pem load=1`，随后握手失败；走 API 时表现为
  `album-fail: handshake: ssl error`（无 `detail-real`）。

**发现的缺陷**：`NodeHttp::StartQuic` 的失败分支原先无条件回退到 TLS/HTTP1.1，
不区分失败原因。已修：

```cpp
// node_http.cpp
if (r.error.contains("certificate verify") || r.error.contains("verify=")) {
    ProxyConfig::NoteH3(false);
    Log("node-http h3 cert failure, no fallback: ...");
    Settle(job, false, r.error);   // 证书错误直接失败，不回退
    return;
}
```

### 1.2 会话复用会跳过证书链重校验（本轮新增能力引入的风险）

0-RTT/会话复用把"把上次的会话票据拿来继续用"变成常态，而 TLS 1.3 的会话复用
**不会重新下发证书链**，OpenSSL 也就不会重新做链校验。实测证据（修复前）：

```
h3 ca-bundle .../not-a-ca.pem
h3 session resumed (early data allowed)
h3 handshake ok version=TLSv1.3 alpn=h3 ...
h3-probe: done ok=1 status=301 ...          <-- 用错误 CA 也成功了
```

这不是理论风险：只要换掉 `JM_CA_BUNDLE`（或证书轮换后信任锚变化），客户端仍会
复用旧票据，把本该失败的连接做成成功。

**修复**：票据按"信任锚指纹"绑定。`<host>.meta` 增加 `ca=<sha256(path|size|mtime)[0..8]>`，
加载时与当前 CA 指纹不一致即删除票据（`quic_http.cpp` 的 `CaFingerprint` /
`LoadTicket`）。修复后同一场景：

```
h3 ca-bundle .../not-a-ca.pem
h3-probe: done ok=0 ... err=read_pkt: ERR_CRYPTO ... certificate verify failed verify=20
```

没有 `session resumed` 行，说明票据已被判定不可复用。

另注：`cert=` 指纹在握手后做二次校验（证书轮换即丢弃票据），但它是"事后判定"，
不能替代 `ca=` 这个"事前判据"。

**回归守卫**：`c1_ui_desktop_suite.py` 的 `h3_bad_ca` 用例（net 层）用
`JM_CA_BUNDLE=docs/testdata/tls/not-a-ca.pem` 跑 `--h3-probe cloudflare.com`，
断言 `ok=0` 且错误里出现 `certificate verify failed`/`verify=`。
注意该用例属于联网层：拿不到任何输出时记 SKIP，但只要探针启动就必须严格判定。

## 2. 主机名校验

- QUIC：`quic_http.cpp:986` `SSL_set1_host(ssl, host)`，其中 `host` 是真实域名；
  IP 只用于 UDP 目的地址（`connectIp`），不参与证书校验。节点覆盖场景因此与
  `NodeHttp` 的 SNI 语义一致（`connectToHostEncrypted(connectHost, port, sni)`，
  `node_http.cpp:426`）。
- TCP/ECH：`openssl_tls.cpp:485` 设 `SSL_VERIFY_PEER`，`openssl_tls.cpp:598`
  取 `SSL_get_verify_result`，并在 `:606` 把 `verifyResult == 0 && hostOk == 1`
  作为可用条件，主机名不匹配不会被当成成功。
- 回退的 `QSslSocket` 路径：`node_http.cpp:412` `setPeerVerifyMode(VerifyPeer)`。

## 3. CA 来源可判定

- QUIC 每次建连打印一行：`h3 ca-bundle <来源>`，来源是
  `env:JM_CA_BUNDLE@…` / `git:ca-bundle.crt@…` / `default:SSL_CTX_set_default_verify_paths`
  三者之一（`quic_http.cpp:954`）。
- TCP/ECH 通道：`ech-probe: tls ca path=... load=1`（`openssl_tls.cpp` 的加载分支），
  加载失败会回退到默认路径并如实打印。
- 实测样本：`h3 ca-bundle C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt`。

## 4. 0-RTT

- 只对幂等请求启用：`quic_http.cpp:1220`
  `canEarly = zeroRttArmed_ && !handshakeDone_ && earlyBudget_ > 0 && method == GET && body.isEmpty()`。
- 每连接只允许 1 条流提前发送（`earlyBudget_ = 1`）。原因见 §6：多条流同时提前
  发送时若被拒绝，nghttp3 的控制流会一起失效，握手期写控制流会阻塞到
  `handshake_timeout`（实测 burst=10 卡 10s）。
- 被拒绝时不做静默降级：`SSL_get_early_data_status()` 会记入 `0rtt=rejected`，
  并输出 `h3 0-rtt rejected by server (stream data re-sent in 1-RTT)`；
  实测请求仍成功（`301 167B total=319ms`），说明 ngtcp2 在 1-RTT 重传了流数据。
- 不做手工重发：手工重建流会让同一请求出现两条流（GET 浪费、POST 重复提交），
  因此只记录事件。

## 5. UDP 明文暴露面

QUIC 头部（DCID/包号）不加密，可被观察。核查实现没有把敏感信息放进自定义字段：

- DCID/SCID 全部来自 `RAND_bytes`（`quic_http.cpp` 建连处），不含 host/用户标识。
- `OnGetNewCid` 生成的 CID 与 stateless reset token 也是纯随机（`quic_http.cpp:720`）。
- 未使用 NEW_TOKEN / 自定义 token 传递任何业务信息。
- 唯一可见的元数据是"到某个 IP:443 的 UDP 流量规模与时长"，与 TLS 时代的 IP 可见性同级。

## 6. 遗留风险

1. **0-RTT 重放**：仅 GET，且服务端（Cloudflare）实测拒绝 0-RTT；若将来对
   接受 0-RTT 的服务端启用，重放窗口由服务端 anti-replay 决定，客户端不做补偿。
2. **两套信任库**：QUIC/ECH 用 `JM_CA_BUNDLE`，回退用 Qt 的信任库。已用
   "证书错误不回退"堵住混淆，但两套 CA 的差异本身仍在，排障时需看
   `h3 ca-bundle` 与 `ech-probe: tls ca path=` 两行。
3. **票据落盘**：`<state>/h3/<host>.pem` 含会话票据（可恢复会话）。当前只在
   到期/证书变更/ECH 指纹变化时删除，没有独立的"清理"入口；如需合规清理，
   删目录即可（进程不会重建，除非再次握手）。

## 7. 复现命令

```powershell
# 1) QUIC 必须校验证书（期望 ok=0 + certificate verify failed）
$env:JM_CA_BUNDLE = "C:/Users/WQ159/source/repos/JMComic-qt/docs/testdata/tls/not-a-ca.pem"
./cpp/shell/ui/build/jmcomic_ui.exe --page home --h3-probe cloudflare.com --h3-path / `
    --shot t.png --shot-delay 12000

# 2) 正常 CA 下同一探针应成功（期望 ok=1 status=301 version=HTTP/3）
Remove-Item Env:JM_CA_BUNDLE
./cpp/shell/ui/build/jmcomic_ui.exe --page home --h3-probe cloudflare.com --h3-path / `
    --shot t.png --shot-delay 12000

# 3) 证书错误不参与回退（期望 album-fail，且没有 detail-real）
$env:JM_CA_BUNDLE = ".../not-a-ca.pem"
./cpp/shell/ui/build/jmcomic_ui.exe --page home --http3-on --doh-on --open-album 313833 `
    --shot t.png --shot-delay 25000
```
