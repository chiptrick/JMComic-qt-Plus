// TLS / QUIC 后端能力探测与接口边界。
//
// 目的：把"当前能用什么加密与传输"从业务层抽出来，避免页面/配置里写死
// "Qt 不支持 ECH/QUIC" 这种会随依赖变化的结论。
//
// 现状（Qt 6.12 + 本机依赖）：
//   * Windows：Qt 的 TLS 后端是 Schannel（Qt bin 里没有 OpenSSL 运行库），
//     Qt 的 qopensslbackend.dll 也不引用 SSL_set1_ech_config_list；
//   * Android：Qt 只带 qopensslbackend 插件与 FFmpegStub 的假 ssl/crypto，
//     没有可链接的 OpenSSL 3.2+ 开发包；
//   * 两个平台的 QtNetwork 都没有 QUIC/HTTP3（无 Http3AllowedAttribute、
//     二进制里没有 nghttp3/ngtcp2 符号）。
//
// 因此本文件给出真实的运行时能力，并预留两个替换点：
//   TlsProvider  —— 未来接 OpenSSL 3.2+/BoringSSL（ECH、TLS1.3 0-RTT、QUIC-TLS 回调）
//   QuicTransport —— 未来接 ngtcp2 + nghttp3（HTTP/3）
// 业务层只依赖 NetBackend::Tls()/Quic() 的返回值，不直接碰后端 API。
#pragma once

#include <QByteArray>
#include <QString>

struct TlsCapabilities
{
    QString backend;      // schannel / openssl / certonly / none
    QString version;      // 后端库版本字符串（QSslSocket 提供）
    bool tls13 = false;
    bool ech = false;         // 能否把 ECHConfigList 注入 ClientHello
    bool echRuntime = false;  // 底层库是否导出 ECH API（OpenSSL 需非 no-ech 构建）
    bool earlyData = false;   // TLS 1.3 0-RTT
    bool quicTls = false;     // 是否提供 QUIC 握手所需的 TLS 回调接口
    QString note;             // 不可用原因（页面/日志直接用）
};

struct QuicCapabilities
{
    bool available = false;
    QString stack;        // ngtcp2+nghttp3 / 未编译
    QString version;
    bool http3 = false;
    bool zeroRtt = false; // QUIC 0-RTT（配合 TLS early data）
    QString note;
};

namespace NetBackend {

TlsCapabilities Tls();
QuicCapabilities Quic();

// 一行摘要：tls=schannel/... ech=0 quic=0 http3=0 stack=...
QString Summary();

// ECHConfigList 注入点。当前后端无法注入时返回 false 并写明原因；
// 未来 OpenSSL 后端应调用 SSL_set1_ech_config_list()。
bool SetEchConfig(const QByteArray &echConfigList);
QString LastError();

}  // namespace NetBackend
