# C1 网络层（C++ 进程内实现）

- 版本：v2（取代 v1 的"Python 子进程桥"）
- 日期：2026-11-02
- 代码：`cpp/shell/net_client.{h,cpp}`；CA：`cpp/shell/assets/cacert.pem`
- 决策依据：用户拍板"把 C3 提前，一次性去掉 Python 运行时"

---

## 1. 为什么不再用 Python 子进程

v1 的桥（`bridge.py` + `PythonBridge`）把 Python 运行时打进 APK，`net.request` 走子进程。
实测结论：

| 环境 | SELinux | 结果 |
|---|---|---|
| MuMu 12 / Android 15 AVD | Permissive | ✅ 可 exec |
| Android 17 16KB 镜像 | Enforcing | ❌ `execve: Permission denied` |

原因是实现把 `libpythonbin.so` 复制到 `files/pybin/` 再 exec，而 `app_data_file` 在 Enforcing
下不可执行。量产机一律 Enforcing，因此该方案在真机上不可能成立。

改为进程内实现后：

- 不再依赖 SELinux 策略，也不需要可执行文件落盘；
- APK 从 **177MB → 76MB**（去掉 96MB Python bundle 与运行时库）；
- 少一个进程，故障面从"进程起停/崩溃重启/协议解析"收敛为"HTTP 客户端"。

## 2. 分层

```
UI (main.cpp)
   └─ NetClient                 // QObject，单通道，信号 Finished(NetResult)
        └─ QNetworkAccessManager
             └─ Qt TLS 后端（qopensslbackend 插件）
                  └─ libssl.so / libcrypto.so（随包，Android）
```

- **TLS** 交给 Qt 的 OpenSSL 后端，不自建 TLS。OpenSSL 由 `build_wsl.sh` 从现有 p4a dist
  复制到 `openssl/<abi>/`，再经 `QT_ANDROID_EXTRA_LIBS` 进 APK 的 `lib/<abi>/`。
  两者 `p_align=0x4000`（16KB 合规）。

  这里有两个**必须照做**的 Android 细节，缺一个就是"插件加载成功但后端不可用"：

  1. **库名**：Qt 的 OSSL 后端在 Android 上按 `libssl_3.so` / `libcrypto_3.so` 找库
     （它自家 android_openssl 包的命名）。只放 `libssl.so` 会得到
     `qt.tlsbackend.ossl: Failed to load libssl/libcrypto.`，最终表现为
     `TLS initialization failed`。因此同一份文件同时放两种名字。
  2. **TLS 插件要自己打包**：`target_link_libraries(Qt6::Network)` 不会让 androiddeployqt
     把 `libplugins_tls_qopensslbackend_<abi>.so` 放进 APK（实测包内 `plugins/` 为空）。
     必须把它加进 `QT_ANDROID_EXTRA_LIBS`，Qt 才能在 `lib/<abi>/` 下按名加载。

- **证书**：Android 的 OpenSSL 后端取不到系统证书库，因此把 Mozilla CA 包
  （`assets/cacert.pem`，118 张）**编进 Qt 资源**，启动时读入并并入系统证书。
  走 qrc 而不是 `assets:/`：实测 assets 路径引擎读 236KB 要 **8 秒**，qrc 是毫秒级。
  不预置的后果是所有 HTTPS 直接 `SSLErr`，而日志只有 "handshake failed"。

## 3. 接口

```cpp
struct NetRequest {
    QString method = "GET";
    QUrl url;
    QHash<QString, QString> headers;
    QByteArray body;
    int timeoutMs = 10000;   // 总预算，含重试
    int retries = 1;         // 额外尝试次数
    qint64 maxBytes = 256 * 1024;
};

struct NetResult {
    bool ok;            // 是否拿到响应（4xx/5xx 也算拿到）
    bool httpError;     // httpStatus >= 400
    int httpStatus;
    QByteArray body;
    qint64 bodyLen;
    bool truncated;
    int attempts;
    qint64 elapsedMs;
    QString errorCode;  // 失败时非空，见 §4
    QString errorText;
    QString Summary() const;   // 一行摘要，状态栏与 logcat 共用
};

class NetClient : public QObject {
    bool Send(const NetRequest &req);   // 单通道，已在飞时返回 false
    bool Get(const QUrl &url, int timeoutMs = 10000);
    static int EnsureCaCertificates(const QString &pemPath = QString());
signals:
    void Finished(const NetResult &result);
};
```

## 4. 语义约定

| 项 | 约定 | 理由 |
|---|---|---|
| `timeoutMs` | **总预算**，按尝试次数均分（每次 ≥200ms） | 若按"每次尝试"计，`retries=1` 时总耗时可到 2 倍预算，调用方的等待上限就失真 |
| 重试 | 仅 `DnsError/TimeOut/ConnectErr/ResetErr`；4xx/5xx 不重试 | 重试 404 只是把同一个 404 再打一遍 |
| 4xx/5xx | `ok=true` + `httpError=true` | 把"网络不通"和"服务器说没有"分开，业务层才能读 403 响应体做诊断 |
| `maxBytes` | 到顶即 abort 并置 `truncated=true` | 不让大响应打爆内存；abort 不算失败 |
| 并发 | 单通道，第二个请求直接返回 false | 避免"谁的超时/谁的结果"说不清；需要并发时再扩成 id→回调表 |
| 重定向 | `NoLessSafeRedirectPolicy`，最多 5 跳 | 默认策略不接受 https→http 降级 |

### 错误码

| code | 触发 |
|---|---|
| `UserError` | URL 非法（scheme 非 http/https）、参数错误 |
| `DnsError` | `HostNotFoundError` |
| `TimeOut` | 单次尝试超过预算（`abort()` 后归类） |
| `ConnectErr` | 连接被拒 / 对端关闭 / 会话建立失败 |
| `ResetErr` | 连接重置（预留；Qt 通常归到 ConnectErr） |
| `SSLErr` | TLS 握手失败（含缺 CA、域名不匹配） |
| `NetError` | 其它传输层错误 |

词表与 `python/src/tools/status.py` 的 `Status` 同名，Python 版的日志经验可直接复用。

## 5. 自检钩子（W7 取证入口）

```bash
adb shell run-as org.jmcomic.c1shell sh -c 'touch files/net_selftest'
adb shell am force-stop org.jmcomic.c1shell
adb shell am start -n org.jmcomic.c1shell/org.qtproject.qt.android.bindings.QtActivity
```

预期日志（tag `C1Net` / `C1Shell`）：

```
C1Net  : CA ready: bundled=118 system=N total=... (tls=openssl)
C1Net  : req 1/1 GET https://www.baidu.com/ (timeout=14500ms/attempt)
C1Shell: net OK 200 17143B 148ms
```

URL 可用环境变量 `JM_C1_NET_URL` 覆盖（默认 `https://www.baidu.com/`）。

## 6. 验证

| 层 | 命令 | 覆盖 |
|---|---|---|
| 桌面 | `bash cpp/shell/host_tests/run_host_tests.sh` | 200 / 404 / 超时 / 截断 / POST / 拒连重试 / 非法 URL / 单通道（9 项） |
| 设备 | `bash python/android/tools/c1_w7_regression.sh --apk <apk>` | 安装、启动、`net_selftest`、PSS、gfxinfo、两条门禁 |
| 门禁 | `build_wsl.sh` 内置 | APK 含 `assets/cacert.pem`、`lib/<abi>/libssl.so`、`libcrypto.so`、`libplugins_tls_qopensslbackend_<abi>.so`；16KB 对齐；`libsr_qnn.so` 直链 |

### 实测（2026-11-02）

| 设备 | Android | SELinux | 结果 |
|---|---|---|---|
| `emulator-5554` | 15 | Permissive | `TotalTime 1222ms`；`CA bundled=118`；`net OK 200 17143B 601ms`；阅读器/设置自检均通过 |
| `emulator-5556` | 17 (SDK 37) / 16KB | **Enforcing** | `CA bundled=118`；`net OK 200 17143B 1260ms` |

同一份 APK 在 Enforcing 上不再出现 `execve: Permission denied`——这是移除 Python 子进程的
直接收益。APK 体积 177MB → **82MB**（75.7MB + 两套 OpenSSL 别名共 6MB）。

## 7. 已知边界（不假装覆盖）

1. **TLS 指纹**：Qt/OpenSSL 的 ClientHello 与浏览器不同。目标站点若做 JA3/JA4 校验，
   需要在 C3 阶段换成 curl-impersonate 一类后端；届时只替换 `net_client` 实现，接口不变。
2. **HTTP/2、连接池、断点续传**：走 Qt 默认策略，未针对分页预取优化，属 C3。
3. **并发**：单通道。列表页并行拉取前需要先扩接口（id→回调）。
4. **代理**：当前随 Qt 的系统代理，显式代理参数留给 C3。

## 8. 与 v1 桥的关系

v1（`bridge.py` + `PythonBridge` + `net_client.py`）已随本次改造删除，代码在提交 `734cb9c`
可达。保留的历史教训（对将来任何子进程组件都成立）：

- 响应必须回带 `method`，否则调用方无法判断回来的是哪个方法；
- 非 0 退出码也是"死了"（`QProcess` 报 `NormalExit`，只判 `CrashExit` 会漏掉最常见的崩溃）；
- `Stop()` 里 `terminate()/waitForFinished()` 会同步派发 `finished`，先把 `proc_` 摘空再收拾。
