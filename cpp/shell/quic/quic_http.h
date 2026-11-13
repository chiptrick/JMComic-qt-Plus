// HTTP/3 over QUIC 客户端（ngtcp2 + nghttp3 + OpenSSL 4.x QUIC-TLS）。
//
// 为什么单独实现而不是走 Qt：
//   Qt 6.12 的网络栈没有 QUIC/HTTP3，QNetworkAccessManager 只到 HTTP/2，
//   QSslConfiguration 也没有 ECH 入口。要让「ECH + HTTP/3」真正生效，
//   必须自己拿 OpenSSL 的 QUIC-TLS 回调（SSL_set_quic_tls_cbs）把
//   ngtcp2 的握手/密钥导出接起来，再用 nghttp3 在 QUIC 流上做 HTTP/3。
//
// 结构（Q2，0.89）：
//   连接池按 (host, port, echConfig 指纹) 复用连接，一条连接上并发多条请求流。
//   * Connection —— QUdpSocket + SSL + ngtcp2_conn + nghttp3_conn + 计时器
//   * Job        —— 一条 bidi 流 = 一个请求（响应累积、超时、回调）
//   单连接并发流上限 32，同 key 最多 2 条连接，全局 24 条；超限排队。
//   最后一个 Job 完成后进入空闲计时（15s），超时发 CONNECTION_CLOSE。
//
// 0-RTT（Q1，0.89）：
//   会话票据按 host 落盘（<state>/h3/<host>.pem + .meta），票据有效且
//   echConfig 指纹/证书指纹未变时用 SSL_set_session 复用，并在
//   SSL_SESSION_get_max_early_data()>0 时开启 QUIC early data。
//   仅对幂等 GET（无 body）启用；被服务端拒绝时丢弃 0-RTT 数据并在 1-RTT 重发。
//
// 本文件对外只暴露 Qt 类型（pimpl），因为 Android 端暂时没有 OpenSSL 4.x，
// 那里编译的是 Available()==false 的桩实现，不引入 ngtcp2 头文件。
#pragma once

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>

#include <functional>

namespace quic {

struct Request
{
    QString host;                       // :authority + 证书校验名（真实域名）
    QString connectIp;                  // 目标 IP（DoH 结果）；空则解析 host
    quint16 port = 443;
    QString path = QStringLiteral("/");
    QByteArray method = QByteArrayLiteral("GET");
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
    QByteArray echConfigList;           // 非空 → 走 ECH
    qint64 maxBytes = 8 * 1024 * 1024;
    int timeoutMs = 25000;
};

struct Response
{
    bool ok = false;
    int status = 0;
    QByteArray body;
    bool truncated = false;
    QString error;
    QString location;     // 3xx 的 Location（供调用方跟随重定向）
    QString version;      // "HTTP/3"
    QString alpn;         // "h3"
    QString tlsVersion;   // "TLSv1.3"
    QString echText;      // SUCCESS / GREASE / FAILED / 未启用 ...
    // 请求注入过 ECHConfigList、但服务端没有真正接受 ECH（FAILED / BACKEND /
    // BAD_NAME / 握手未完成）。Q4「隐藏 SNI 优先」：调用方据此丢弃 H3 结果
    // （即使 HTTP 层成功）并回退 TCP+ECH，不能拿一个暴露 SNI 的 H3 当成功。
    bool echRejected = false;
    QString earlyText;    // accepted / rejected / not_sent / 未启用
    bool reused = false;  // 复用了池中已握手的连接（没有新握手）
    qint64 ttfbMs = 0;    // 建连到首个响应字节（首包成本）
    qint64 elapsedMs = 0;
};

// 连接池计数（供 --h3-probe / 回归断言：连续 N 次同域请求只应发生 1 次握手）
struct PoolStats
{
    int connections = 0;   // 当前存活连接
    int jobs = 0;          // 在飞请求
    int queued = 0;        // 排队请求
    int handshakes = 0;    // 进程内累计新建连接数（= QUIC 握手次数）
    int reused = 0;        // 进程内累计复用次数
    int zeroRttTried = 0;
    int zeroRttAccepted = 0;
    int zeroRttRejected = 0;
};

// 单请求入口。内部走连接池：同一 key 的请求会复用同一条 QUIC 连接。
class Client : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const Response &)>;

    explicit Client(QObject *parent = nullptr);
    ~Client() override;

    void Start(const Request &req, Callback cb);

signals:
    void Log(const QString &line);
};

// 编译期是否带 QUIC 栈（Android 端为 false）
bool Available();
// "ngtcp2 1.26.90 / nghttp3 1.19.90 / OpenSSL 4.1.0"，不可用时给原因
QString BackendSummary();

// 进程内连接池快照（不可用时全 0）
PoolStats Stats();
// 关闭所有池内连接（应用退出 / 测试收尾）；不等待，直接发 CONNECTION_CLOSE
void ShutdownPool();

}   // namespace quic
