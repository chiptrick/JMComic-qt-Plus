# QUIC 依赖的版本与校验和

> 对应 `docs/cpp_ui_next_steps.md` 的 T1。目的：任何人按这份记录能判断"手上的
> `deps/` 是不是同一套产物"，并能用脚本重建。

## 版本

| 组件 | 版本 | 来源 |
| --- | --- | --- |
| ngtcp2 | 1.26.90 | `C:/Users/WQ159/ngtcp2` |
| nghttp3 | 1.19.90 | `C:/Users/WQ159/nghttp3` |
| OpenSSL | 4.1.0 | `C:/Users/WQ159/source/repos/openssl-4.1.0`（Windows 产物另有安装包） |
| Android NDK | r30（30.0.16248370） | Linux 版：`~/.android/ndk/30.0.16248370` |
| 构建宿主 | Fedora 44 / GCC 16.2.1 / CMake 4.1.2 | Android 交叉编译 |
| 构建宿主 | Windows / MinGW-w64 13.1 | 桌面 DLL 与导入库 |

## 构建入口

| 目标 | 脚本 | 说明 |
| --- | --- | --- |
| Windows 桌面（`deps/quic-mingw`） | `cpp/shell/quic/tools/build_quic_deps.ps1` | gendef/dlltool → nghttp3 → ngtcp2；`-Avx2` 仅桌面 x86_64 |
| Android（`deps/{openssl41,quic}-android`） | `cpp/shell/quic/tools/build_quic_deps_android.sh` | 必须 Linux 宿主（Windows 上 OpenSSL 的 `android-*` 配置跑不起来） |

两者的构建后校验一致：库文件存在、`Looking for SSL_set_quic_tls_cbs - found`、
符号表非空（Android 版还额外校验 `SSL_set1_ech_config_list` / `SSL_ech_set1_server_names`
/ `SSL_set_quic_tls_early_data_enabled`）。

Android 版（2026-11-12 起）`Configure` 显式带 `enable-ech enable-quic`：两者在
OpenSSL 4.x 默认开，但会被 `no-bulk`/`no-dgram`/`no-tls1_3` 级联关闭，某些发行版
也用 `no-ech` 打包；缺了它们链接照样成功，只有运行期 `ech=0`、无 QUIC-TLS 回调。
verify() 还会检查 `libngtcp2_crypto_ossl.a` 里的
`ngtcp2_crypto_ossl_configure_client_session`、libssl 的 SONAME 必须是无版本号的
`libssl.so`，以及 LOAD 段 16KB 对齐（Android 15+）。

CMake 侧（`cpp/shell/ui/CMakeLists.txt`）`JM_WITH_QUIC` 为三态 AUTO/ON/OFF，
默认 AUTO：per-ABI 依赖齐全自动打开，并用 llvm-nm 对 libssl 导出符号与 ngtcp2
ossl 后端做**配置期硬校验**（缺符号直接 FATAL，见 `docs/mobile_ech_h3_enable.md`）。

## 校验和（SHA256）

### Windows 桌面 `deps/quic-mingw/lib/`

| 文件 | SHA256 |
| --- | --- |
| libngtcp2.a | `075aa76b50be7732bffca431cd2488f553b4c3f7339b4dd33ea59374262699ea` |
| libngtcp2_crypto_ossl.a | `d82ba4a1b1e3af17ca063611fe26fe6b5fdf648935b80d30394bd77ff1d24ea4` |
| libnghttp3.a | `9027d0e6139c9da0deebab189d0a5726cee54910ea3155d9a83ada1a63224917` |

### Android `deps/quic-android/arm64-v8a/lib/`

| 文件 | SHA256 |
| --- | --- |
| libngtcp2.a | `0703afa231a795029cc594a884f7381aeb21e21b63aac4827e027dcc42bc5f96` |
| libngtcp2_crypto_ossl.a | `3baef49556b57f541953167a13a11e262643f4fcbd8ae03d7734c78bd09f31e3` |
| libnghttp3.a | `f35682a700a117f144b651217481e8a1e7034ee161b3d1c1ff817d32efa8e6c7` |

### Android `deps/openssl41-android/arm64-v8a/lib/`

| 文件 | SHA256 | 大小 |
| --- | --- | --- |
| libssl.so | `d30a890fa396872dbf60d08703c98823b732def7c5ea1445fc5fc9dc5349a69b` | 1,288,352 B |
| libcrypto.so | `62cfa0a427b0a82ac8bf9977f34c36d8acb864c712de8d2d70b8be0ecda18e08` | 6,179,144 B |

### Android `deps/quic-android/x86_64/lib/`

| 文件 | SHA256 |
| --- | --- |
| libngtcp2.a | `167ac6471d49d5cafbf69560217c422331a7759e5e8d9d8d0d61838683acb03f` |
| libngtcp2_crypto_ossl.a | `01f5bad807cf29c9c0676c536b0833908242ee63e4d5def4027afc437063eb9c` |
| libnghttp3.a | `c912c5f2a8021525eec63c81d91353c447d35b62d12fa19bc9a234fd10364eef` |

### Android `deps/openssl41-android/x86_64/lib/`

| 文件 | SHA256 | 大小 |
| --- | --- | --- |
| libssl.so | `234b3c7bff08034805c99dd0df87dcfcbabbc755afa97ae48d14f5695b40075e` | 1,281,376 B |
| libcrypto.so | `ee929c4a4ae1e6c52c060f9c8f74815ded9b40119c59d8f91f59c0161351deda` | 6,381,872 B |

## 已知差异（Android）

- Android 版安装的是**无版本号**的 `libssl.so` / `libcrypto.so`（SONAME 同名），
  与桌面 `libssl-4-x64.dll` 的命名无关；APK 里因此不会与 Qt 的 `libssl_3.so` 冲突。
- 交叉编译**不加** `-mavx2`：x86_64 模拟器不保证暴露 AVX2，arm64 也没有该选项。
- 编译时不要额外传 `-D__ANDROID_API__`：NDK 的 `aarch64-linux-android34-clang`
  包装器已经定义，重复定义会产生满屏重定义告警。
- `deps/` 位于 NTFS 挂载点，`install` 返回后文件可能延迟可见；构建脚本的校验带重试，
  手工核对时若第一次 `nm` 为空，等一两秒再看。
