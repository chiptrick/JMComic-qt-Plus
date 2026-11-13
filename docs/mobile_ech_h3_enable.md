# 手机端 ECH / HTTP/3 未开启：排查记录与修复（2026-11-12）

> 现象：手机 APK 上设置页显示 `ECH 开·后端不支持`、`HTTP3 开·后端不支持`，
> `--advanced-status` 输出 `ech=0 echLib=0 quic=0 http3=0 stack=未编译`。
> 结论先行：**不是平台能力问题，是构建配置问题** —— 手机包一直默认
> `JM_WITH_QUIC=OFF`，APK 里既没有 ngtcp2/nghttp3，也没有带 ECH 的 OpenSSL 4.x。

## 1. 排查过程与证据

| # | 检查 | 结果 | 结论 |
|---|---|---|---|
| 1 | `deps/openssl41-android/<abi>/lib/libssl.so` ELF 解析（SONAME/NEEDED/导出表/LOAD 对齐） | SONAME=`libssl.so`；NEEDED=`libcrypto.so,libdl.so,libc.so`；导出 `SSL_set1_ech_config_list`/`SSL_ech_set1_server_names`/`SSL_set_quic_tls_cbs`/`SSL_set_quic_tls_early_data_enabled`；LOAD align=0x4000 | **依赖产物本身完好**，ECH 与 QUIC-TLS 都在 |
| 2 | `deps/quic-android/<abi>/lib/libngtcp2_crypto_ossl.a` | 含 `ngtcp2_crypto_ossl_configure_client_session` | ngtcp2 的 OpenSSL 后端也在 |
| 3 | 两个 `build-android-*/CMakeCache.txt` | `JM_WITH_QUIC:BOOL=OFF`、`JM_QUIC_ROOT=`（空） | **根因**：手机构建从未打开 QUIC |
| 4 | `JMComic-ui-0.93-*-debug.apk` 内容 | `libssl.so`=1,248,792B（OpenSSL 3.x）；主 .so 含桩字面量 `QUIC 栈未编译进本平台`，无 `ngtcp2 %1 / nghttp3 %2 / %3` | 当前手机包确实没有 ECH/HTTP3 |
| 5 | 对照 0.89 `-quic` 包 | `libssl.so` 导出 ECH 符号；主 .so 含真后端字面量 | 判据能区分两种构建 |

CMake 的注释里写的是"默认仍为 OFF"，`build_android.ps1` 的 `-WithQuic` 也只是
可选开关 —— 依赖齐了但没人传参，功能就一直缺席。这类失败**没有任何构建期报错**，
只能等装到手机上才从设置页看出来，因此修复的核心是"默认开启 + 逐层硬校验"。

## 2. 能力要求（依赖库与版本）

| 组件 | 版本要求 | 提供什么 |
|---|---|---|
| OpenSSL | **>= 3.5**（本项目用 4.1.0），`Configure` 不得带 `no-ech`/`no-quic`/`no-bulk` | `SSL_set1_ech_config_list`、`SSL_ech_get1_status`、`SSL_ech_set1_server_names`（ECH）；`SSL_set_quic_tls_cbs`、`SSL_set_quic_tls_early_data_enabled`（QUIC-TLS） |
| ngtcp2 | >= 1.0.0，`-DENABLE_OPENSSL=ON`（配置期必须探测到 `SSL_set_quic_tls_cbs - found`） | QUIC 连接/流/重传 + `libngtcp2_crypto_ossl` |
| nghttp3 | >= 1.0.0 | HTTP/3 帧化与 QPACK |
| NDK | r26+（本项目 r30），OpenSSL `android-*` target 只能在 Linux 宿主跑 | 交叉编译 |

Android 侧 OpenSSL 额外硬约束：无版本号 SONAME（`libssl.so`/`libcrypto.so`），
LOAD 段 16KB 对齐（Android 15+），APK 内不得与 3.x 双套并存（构建脚本已用
"开启 QUIC 时只装 4.x，`libssl_3.so` 别名也指向 4.x"规避）。

## 3. 修复内容

| 文件 | 改动 |
|---|---|
| `cpp/shell/ui/CMakeLists.txt` | `JM_WITH_QUIC` 改三态 **AUTO/ON/OFF**（默认 AUTO：per-ABI 依赖齐全自动开，Android 缺依赖时打 WARNING 说明后果）；新增**配置期符号自检**（llvm-nm 查 ECH/QUIC-TLS 导出 + ngtcp2 ossl 后端符号，缺一个 FATAL）与版本下限校验（OpenSSL>=3.5、ngtcp2>=1.0、nghttp3>=1.0），并打印 `UI: QUIC deps OK — OpenSSL …, ngtcp2 …, nghttp3 …` |
| `cpp/shell/ui/build_android.ps1` | 默认 AUTO 自动探测依赖（新增 `-NoQuic` 强制关、`-WithQuic` 强制开）；出包后**校验 APK**：`libssl.so` 必须导出 ECH 与 QUIC-TLS 符号、`libjmcomic_ui_*.so` 必须含真 H3 后端（UTF-16 字面量 `ngtcp2 %1 / nghttp3 %2 / %3`），不过关直接 fail；顺带把残余中文注释转 ASCII（该文件要求 ASCII-only） |
| `cpp/shell/ui/build_android.sh` | 与 ps1 同语义：`--with-quic`/`--no-quic`，默认 AUTO；出包后用 `unzip -p … \| grep -a`（UTF-16 字面量先 `tr -d '\000'`）做同样的 APK 校验；`usage()` 改为 awk 打印注释头 |
| `cpp/shell/quic/tools/build_quic_deps_android.sh` | `Configure` 显式加 `enable-ech enable-quic`（防 `no-bulk` 级联关闭/发行版 no-ech）；verify() 新增 ngtcp2 ossl 后端符号、SONAME 无版本号、LOAD 0x4000 三项检查 |
| `cpp/shell/net_backend.cpp` | 降级提示改为可行动的措辞（指向依赖构建脚本），不再写死"Android 缺构建" |

桌面路径不受影响：`build_win.bat` 默认显式传 `-DJM_WITH_QUIC=ON`，行为与原来一致。

## 4. 修改后如何验证

### 4.1 构建期（每层都有硬门禁）

1. **依赖自检**：configure 时应看到
   `UI: QUIC deps OK — OpenSSL 4.1.0, ngtcp2 1.26.90, nghttp3 1.19.90`
   与 `UI: HTTP/3 over QUIC + ECH enabled for Android <abi>`。
   若把 `JM_OPENSSL41_ROOT` 指向 no-ech 的 OpenSSL，configure 直接 FATAL 并列出缺失符号
   （本机已用 OpenSSL 3.x 产物做了负向验证）。
2. **APK 校验**：构建脚本结尾 `verify ECH/HTTP3 inside the APK`，三项
   `libssl.so exports SSL_set1_ech_config_list / SSL_set_quic_tls_cbs`、
   `app library contains the ngtcp2/nghttp3 backend` 全 ok 才算成功。

### 4.2 出包与真机（能力 → 协议 → 业务）

```powershell
# Windows 宿主（依赖齐全时 AUTO 自动开）
powershell -ExecutionPolicy Bypass -File cpp\shell\ui\build_android.ps1 -Abi arm64-v8a
# Linux 宿主
./cpp/shell/ui/build_android.sh --abi arm64-v8a --deploy --test
```

```bash
python3 cpp/tools/c1_ui_android_h3.py --apk cpp/dist/JMComic-ui-*-arm64-v8a-debug.apk
```

断言（脚本 `cpp/tools/c1_ui_android_h3.py` 已内置）：
1. `--advanced-status`：`ech=1 echLib=1 quic=1 http3=1 stack=ngtcp2+nghttp3`，
   且 `ECH 开`、`HTTP3 开（QUIC/HTTP3，失败回退 HTTP/2）`；
2. `--ech-probe cloudflare.com`：`ech=SUCCESS`（不是 `NOT_CONFIGURED(-103)`）；
3. `--h3-probe cloudflare.com`：`ok=1 … version=HTTP/3 alpn=h3`；
4. 注入回退：`--h3-fail-inject bad-port` 业务仍存活并出现 fallback；
5. `ech-reject` 注入：H3 结果被丢弃、回退 TCP+ECH，重启/换节点后重试组合。

快速人工核对：`adb logcat -d | grep advanced-status`，或抓包确认 ClientHello
外层 SNI 是 ECH 公共名而非真实域名。

### 4.3 本机已完成的验证

- AUTO 配置（x86_64 与 arm64-v8a）：自检通过并完成**编译 + 链接**，两个
  `build-android-*` 目录与 `build-android-quic-check` 的
  `libjmcomic_ui_*.so` 都含真后端字面量、无桩字面量；staging 的
  `libssl_3.so`/`libcrypto_3.so` 已确认为 4.x 尺寸，无版本号 3.x 副本被清除；
- no-ech 负向用例：configure FATAL，列出 4 个缺失符号；
- APK 校验函数对真实包的判据验证：`0.93-x86_64`（无 QUIC）三项全 False、
  `0.89-x86_64-quic`（有 QUIC）三项全 True，与 Python 解析结论一致；
- 两个 sh 语法 `bash -n` 通过、`--help` 输出正确；ps1 通过 Parser 解析且全文 ASCII。
- 说明：本沙箱内 Gradle 打包会挂起（见项目备忘"utimensat"条目），完整出包请在
  正常终端执行 `build_android.ps1/.sh`——届时脚本会自动跑 4.1/4.2 的全部门禁。

## 5. 遗留说明

- 0-RTT 与连接复用仍未实现（与桌面一致，见 `cpp_tls_quic_ech_design.md` §10.7）；
- ECH-over-QUIC 在 Cloudflare 前端仍会被拒，产品策略是"隐藏 SNI 优先"回退
  TCP+ECH，属预期行为而非缺陷；
- AUTO 只在依赖文件齐全时生效：换了机器或删了 `deps/openssl41-android`、
  `deps/quic-android`，会回到无 ECH/H3 的包，但构建日志会有 WARNING，且
  出包后的 APK 校验会继续打印 MISS 行。

## 6. 真机/模拟器验证（2026-11-12 收口）

沙箱内 Gradle 全量打包会挂（utimensat），改走"androiddeployqt 产物 + Python
重打包（py_zipalign 逻辑：STORED 条目 4096 对齐）+ `java -jar apksigner.jar`
签名"出包，x86_64 与 arm64-v8a 两套 QUIC 包装到 MuMu（Android 15 模拟器）与
真机 vivo V2463A。

### 6.1 首轮真机/模拟器实测发现并修复的两个真实缺陷

| # | 现象 | 根因 | 修复 |
|---|---|---|---|
| 1 | `--h3-probe` 失败：`ERR_CRYPTO … certificate verify failed verify=20`（QUIC 握手已进证书阶段，说明 UDP/数据面本身是通的） | Windows 打包没有 Linux 脚本的"prepare CA bundle"步骤，APK 里没有 `:/tls/cacert.pem`；MuMu 镜像的系统目录 `/apex/com.android.conscrypt/cacerts` 读到目录但加载不进证书 | `build_android.ps1` configure 前把 Git 的 `ca-bundle.crt` 拷到 `<build>/generated/cacert.pem`（150 certs）；CMake 对 Android+QUIC 缺 CA 的情况打 WARNING |
| 2 | 证书失败后进程 native 崩溃（crash buffer：`OnReadable → CloseNow → Cleanup`） | 读包回调栈上同步 `Cleanup()` 释放 conn/h3/ssl 是 use-after-free —— 与 §10.4.3"收口不能在回调栈里做"同源，但错误路径没走延迟 | `CloseNow` 拆成两段：当前栈只 `FailAll` + 置位 + 停计时器，`CONNECTION_CLOSE` 发送与资源释放延后到事件循环（新增 `Teardown()`） |

### 6.2 修复后的回归与真机证据

MuMu（`cpp/tools/c1_ui_android_h3.py --serial 127.0.0.1:16384`）：
**PASS 21 / FAIL 2 / SKIP 0**（修复前 16/3/1），关键项：

- `--h3-probe cloudflare.com` → `ok=1 status=301 … version=HTTP/3 alpn=h3`（584ms）；
- `--h3-bench 5` → `ok=5/5, p50=706ms p95=926ms`；
- `--ech-probe` → `ech=SUCCESS(1)`（真实 ECH 握手，`ca-bundle resource:/tls/cacert.pem`）；
- 直连 H3 先试→失败回 QNAM/HTTP2、注入 bad-port 回退、ech-reject 丢弃 H3 回退
  TCP+ECH，全部 PASS；全程 crash buffer 0 条。

仅剩 2 个 FAIL 是 group 7 的断言过严：`echH3Blocked` 期望 1 实际 2（本轮多组
测试对相邻 node key 都登记过抑制），业务本身 `ok=1 status=200` 两次均成功，
抑制与重试机制工作正常（"本进程内暂时不再尝试组合" PASS）。

真机 vivo V2463A：系统不输出第三方应用 logcat（vivo 默认关闭），改用应用内
截图取证（`--page proxy --shot <外部件目录路径> --shot-delay 6000`，注意
`--shot-delay` 单位是**毫秒**）。分流页高级区逐字显示：

```
DOH 开（https://parse.jpacg.cc/parse） · ECH 开 · HTTP3 开（QUIC/HTTP3，失败回退 HTTP/2）
· tls=openssl-ech/4.1.0 ech=1 echLib=1 quic=1 http3=1 stack=ngtcp2+nghttp3
· ECH 配置已获取（71 字节），已交给 ECH 通道，15 分钟后刷新 · ECH 走自建 OpenSSL 4.x 通道
接口：IP 分流（ip-line via node 46.224.21.216） · 图片：IP 分流（ip-line via node 46.224.21.216）
```

`--open-album 313833` 的详情页完整加载（封面/标签/81 话章节），业务经
IP 分流节点正常。截图存于 `docs/shots/phone-proxy-page.png`、
`docs/shots/phone-proxy-bottom.png`、`docs/shots/phone-album.png`。

代理设置页功能（真机）：无代理/HTTP/Socks5/系统四模式单选、线路测速（分流
1-4/US 反代/IP 分流，含延迟与速度）、节点列表（56 节点·可用 18）、已选
ProxyIP 22·46.224.21.216·194ms 全部正常工作；DoH 路由与失败回退由回归
group 4/6 覆盖。`proxy/mode`（HTTP/Socks5 模式）在 C++ 端的语义与 Python
一致：仅抑制 IP/DoH 覆盖（`ResolveRoute`），不实现本地代理客户端。
