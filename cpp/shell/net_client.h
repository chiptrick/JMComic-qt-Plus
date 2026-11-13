// C1 网络层（把 C3 的"网络收编"提前到 C1）：不依赖 Python 子进程的原生 HTTP 客户端。
//
// 设计取舍：
//   * 用 QNetworkAccessManager 而不是自建 socket/TLS：TLS 交给 Qt 的 OpenSSL 后端，
//     证书校验、重定向、HTTP/1.1 分块都由 Qt 负责，C1 不需要自己造轮子；
//   * 显式开启 HTTP/3 后，直连请求先用 quicOnly 模式借 NodeHttp 的 H3 通道试一次，
//     失败（含冷却/证书失败）再回到 QNAM/HTTP2；证书校验失败直接判失败，不换 CA 重试；
//   * connectIp 非空（节点/DoH）时整条请求走 NodeHttp，因为它要自定义 SNI；
//   * 单通道语义沿用桥协议 v1：一次只有一个在飞请求，避免"谁的超时"说不清；
//   * 错误码沿用 src/tools/status.py 的 Status 词表（DnsError/TimeOut/ConnectErr/
//     ResetErr/SSLErr/NetError/UserError），日志口径与 Python 版一致；
//   * timeoutMs 是**总预算**（按尝试次数均分），不是每次尝试各自超时；
//   * 4xx/5xx 视为"拿到响应"（ok=true + httpError=true）：协议层把"网络不通"和
//     "服务器说没有"分开，业务层才有机会读 403 的响应体做诊断；
//   * maxBytes 到顶即 abort 并标 truncated，不让一张大图把进程内存打爆。
#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

struct NetRequest
{
    QString method = QStringLiteral("GET");
    QUrl url;
    QHash<QString, QString> headers;
    QByteArray body;
    int timeoutMs = 10000;  // 总预算，含重试
    int retries = 1;        // 额外尝试次数
    qint64 maxBytes = 256 * 1024;
    // 节点 IP 覆盖（等价 curl --resolve）：
    //   connectIp 非空 → TCP 直连该 IP；peerName 是该连接的 SNI 与证书校验名（域名）。
    // 两者都为空表示普通请求（走 QNetworkAccessManager）。
    QString connectIp;
    QString peerName;
};

struct NetResult
{
    bool ok = false;          // 是否拿到响应（4xx/5xx 也算拿到）
    bool httpError = false;   // httpStatus >= 400
    int httpStatus = 0;
    QByteArray body;
    qint64 bodyLen = 0;
    bool truncated = false;
    int attempts = 0;
    qint64 elapsedMs = 0;
    QString errorCode;        // 失败时非空，取值见头注释
    QString errorText;

    // 供状态栏/日志用的一行摘要
    QString Summary() const;
};

class NodeHttp;
struct NodeHttpResponse;

class NetClient : public QObject
{
    Q_OBJECT
public:
    explicit NetClient(QObject *parent = nullptr);

    bool IsBusy() const { return busy_; }
    QString GetLastError() const { return lastError_; }

    // 发起请求。已有请求在飞时返回 false（不排队、不覆盖）。
    bool Send(const NetRequest &req);
    bool Get(const QUrl &url, int timeoutMs = 10000);

    // 加载 CA：优先用传入的 pem，其次 assets:/cacert.pem，再并上系统证书库。
    // 返回最终可用的证书总数（0 表示 HTTPS 一定失败，调用方应显式告警）。
    static int EnsureCaCertificates(const QString &pemPath = QString());

signals:
    void Finished(const NetResult &result);

private:
    void StartAttempt();
    void Accumulate();
    void OnReplyFinished();
    // 普通直连：Qt 的 QNAM（HTTP/2，失败回 HTTP/1.1）
    void StartQnamAttempt();
    // h3Only=false：节点 IP 覆盖，整条请求走 NodeHttp（自定义 SNI，见 node_http.h）
    // h3Only=true ：只借 NodeHttp 试一次 H3，失败由 OnSocketResponse 回落到 StartQnamAttempt
    void StartSocketAttempt(bool h3Only);
    void OnSocketResponse(const NodeHttpResponse &r);
    void Finish(const NetResult &result);
    int PerAttemptMs() const;

    QNetworkAccessManager *manager_ = nullptr;
    NodeHttp *nodeHttp_ = nullptr;
    QNetworkReply *reply_ = nullptr;
    QTimer attemptTimer_;

    NetRequest pending_;
    int attempt_ = 0;
    int attemptsAllowed_ = 1;
    QByteArray body_;
    bool truncated_ = false;
    bool abortedForLimit_ = false;
    bool timedOut_ = false;
    bool busy_ = false;
    QElapsedTimer started_;
    QString lastError_;
};
