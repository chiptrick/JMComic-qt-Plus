#include "net_client.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslSocket>

#if defined(__ANDROID__)
#include <android/log.h>
#define C1_NET_INFO(...) __android_log_print(ANDROID_LOG_INFO, "C1Net", __VA_ARGS__)
#define C1_NET_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "C1Net", __VA_ARGS__)
#else
#include <QDebug>
#define C1_NET_INFO(...) qInfo(__VA_ARGS__)
#define C1_NET_ERROR(...) qWarning(__VA_ARGS__)
#endif

namespace {

QString ClassifyReply(QNetworkReply::NetworkError error, bool timedOut)
{
    if (timedOut)
        return QStringLiteral("TimeOut");
    switch (error) {
    case QNetworkReply::HostNotFoundError:
        return QStringLiteral("DnsError");
    case QNetworkReply::TimeoutError:
        return QStringLiteral("TimeOut");
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::NetworkSessionFailedError:
        return QStringLiteral("ConnectErr");
    case QNetworkReply::SslHandshakeFailedError:
        return QStringLiteral("SSLErr");
    default:
        return QStringLiteral("NetError");
    }
}

bool IsRetryable(const QString &code)
{
    // 与 Python 版一致的判据：只有传输层可恢复错误才重试，4xx/5xx 不重试
    return code == QLatin1String("DnsError") || code == QLatin1String("TimeOut")
           || code == QLatin1String("ConnectErr") || code == QLatin1String("ResetErr");
}

}  // namespace

QString NetResult::Summary() const
{
    if (!ok) {
        return QStringLiteral("net ERR %1: %2 (attempts=%3, %4ms)")
            .arg(errorCode, errorText)
            .arg(attempts)
            .arg(elapsedMs);
    }
    return QStringLiteral("net OK %1 %2B %3ms%4%5")
        .arg(httpStatus)
        .arg(bodyLen)
        .arg(elapsedMs)
        .arg(truncated ? QStringLiteral(" truncated") : QString())
        .arg(httpError ? QStringLiteral(" (http_error)") : QString());
}

NetClient::NetClient(QObject *parent)
    : QObject(parent)
    , manager_(new QNetworkAccessManager(this))
{
    attemptTimer_.setSingleShot(true);
    connect(&attemptTimer_, &QTimer::timeout, this, [this] {
        if (!reply_)
            return;
        timedOut_ = true;
        C1_NET_ERROR("attempt %d timed out after %dms", attempt_, attemptTimer_.interval());
        reply_->abort();  // 触发 finished，由 OnReplyFinished 统一收口
    });
}

int NetClient::EnsureCaCertificates(const QString &pemPath)
{
    QList<QSslCertificate> bundled;
    // 顺序：显式传入 > Qt 资源（编进二进制，最快）> APK assets。
    // 实测 assets 读 236KB 要 8 秒（Android assets 路径引擎的已知开销），所以资源优先。
    const QStringList candidates = {
        pemPath,
        QStringLiteral(":/assets/cacert.pem"),
        QStringLiteral(":/cacert.pem"),
        QStringLiteral("assets:/cacert.pem"),
    };
    for (const QString &path : candidates) {
        if (path.isEmpty())
            continue;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            C1_NET_INFO("CA candidate unreadable: %s (%s)", qPrintable(path),
                        qPrintable(f.errorString()));
            continue;
        }
        bundled = QSslCertificate::fromData(f.readAll(), QSsl::Pem);
        C1_NET_INFO("CA candidate %s -> %d certs", qPrintable(path), int(bundled.size()));
        if (!bundled.isEmpty())
            break;
    }

    QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
    QList<QSslCertificate> all = cfg.caCertificates();
    all += bundled;
    // 系统库在 Android 上未必可用（OpenSSL 后端拿不到 Java keystore），拿到就并进去，
    // 拿不到也不致命——内置 pem 才是主路径。
    const QList<QSslCertificate> system = QSslConfiguration::systemCaCertificates();
    all += system;
    cfg.setCaCertificates(all);
    cfg.setProtocol(QSsl::TlsV1_2OrLater);
    QSslConfiguration::setDefaultConfiguration(cfg);

    C1_NET_INFO("CA ready: bundled=%d system=%d total=%d (tls=%s)",
                int(bundled.size()), int(system.size()), int(all.size()),
                qPrintable(QSslSocket::activeBackend()));
    return all.size();
}

int NetClient::PerAttemptMs() const
{
    return qMax(200, pending_.timeoutMs / attemptsAllowed_);
}

bool NetClient::Send(const NetRequest &req)
{
    if (busy_) {
        lastError_ = QStringLiteral("net busy: request in flight");
        C1_NET_ERROR("%s", qPrintable(lastError_));
        return false;
    }
    const QString scheme = req.url.scheme().toLower();
    if (!req.url.isValid() || (scheme != QLatin1String("http") && scheme != QLatin1String("https"))) {
        NetResult r;
        r.errorCode = QStringLiteral("UserError");
        r.errorText = QStringLiteral("bad url: %1").arg(req.url.toString());
        r.attempts = 0;
        lastError_ = r.errorCode + QStringLiteral(": ") + r.errorText;
        C1_NET_ERROR("%s", qPrintable(lastError_));
        emit Finished(r);
        return false;
    }

    pending_ = req;
    pending_.method = req.method.toUpper();
    attempt_ = 0;
    attemptsAllowed_ = qMax(1, req.retries + 1);
    busy_ = true;
    started_.start();
    StartAttempt();
    return true;
}

bool NetClient::Get(const QUrl &url, int timeoutMs)
{
    NetRequest req;
    req.url = url;
    req.timeoutMs = timeoutMs;
    return Send(req);
}

void NetClient::StartAttempt()
{
    ++attempt_;
    body_.clear();
    truncated_ = false;
    abortedForLimit_ = false;
    timedOut_ = false;

    QNetworkRequest qr(pending_.url);
    for (auto it = pending_.headers.constBegin(); it != pending_.headers.constEnd(); ++it)
        qr.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    if (!pending_.headers.contains(QStringLiteral("User-Agent")))
        qr.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JMComic-C1/1.0"));
    qr.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                    QNetworkRequest::NoLessSafeRedirectPolicy);
    qr.setMaximumRedirectsAllowed(5);

    const QByteArray verb = pending_.method.toLatin1();
    reply_ = manager_->sendCustomRequest(qr, verb, pending_.body);
    connect(reply_, &QNetworkReply::readyRead, this, &NetClient::Accumulate);
    connect(reply_, &QNetworkReply::finished, this, &NetClient::OnReplyFinished);
    // SSL 错误显式记录：默认 Qt 会直接判失败，但日志里只有 "SSL handshake failed"，
    // 分不清是缺 CA、协议版本还是域名不匹配。
    connect(reply_, &QNetworkReply::sslErrors, this, [](const QList<QSslError> &errors) {
        for (const QSslError &e : errors)
            C1_NET_ERROR("ssl error: %s", qPrintable(e.errorString()));
    });

    C1_NET_INFO("req %d/%d %s %s (timeout=%dms/attempt)",
                attempt_, attemptsAllowed_, verb.constData(),
                qPrintable(pending_.url.toString()), PerAttemptMs());
    attemptTimer_.start(PerAttemptMs());
}

void NetClient::Accumulate()
{
    if (!reply_)
        return;
    const qint64 remain = pending_.maxBytes - body_.size();
    if (remain <= 0) {
        truncated_ = true;
        abortedForLimit_ = true;
        reply_->abort();
        return;
    }
    body_.append(reply_->read(remain));
    if (body_.size() >= pending_.maxBytes) {
        truncated_ = true;
        abortedForLimit_ = true;
        reply_->abort();
    }
}

void NetClient::OnReplyFinished()
{
    if (!reply_)
        return;
    attemptTimer_.stop();

    QNetworkReply *reply = reply_;
    reply_ = nullptr;
    reply->deleteLater();

    const int httpStatus =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    NetResult r;
    r.attempts = attempt_;
    r.elapsedMs = started_.elapsed();
    r.httpStatus = httpStatus;
    r.truncated = truncated_;

    // 被 maxBytes 截断时 abort 会带来 OperationCanceledError，但我们确实拿到了响应头与部分体。
    // 超时也一样会 abort，但**状态不同**：超时后到达的 finished 即使带着 200 头，
    // body 也可能是空的（实测模拟器上响应头先到、体被 7.5s 预算掐掉）——
    // 若不拦住，会把 "net OK 200 0B" 当成功交给上层，解密必然失败且难查。
    if ((timedOut_ || (!abortedForLimit_ && reply->error() != QNetworkReply::NoError && httpStatus < 100))) {
        const QString code = ClassifyReply(reply->error(), timedOut_);
        r.errorCode = code;
        r.errorText = reply->errorString();
        lastError_ = code + QStringLiteral(": ") + r.errorText;
        C1_NET_ERROR("attempt %d failed: %s", attempt_, qPrintable(lastError_));

        if (IsRetryable(code) && attempt_ < attemptsAllowed_) {
            // 通道仍归本次逻辑请求所有：下一轮 attempt 继续复用 busy_ 状态
            QTimer::singleShot(150 * attempt_, this, [this] { StartAttempt(); });
            return;
        }
        Finish(r);
        return;
    }

    // 拿到响应（含 4xx/5xx）
    if (!abortedForLimit_)
        body_.append(reply->readAll());
    r.ok = true;
    r.httpError = httpStatus >= 400;
    r.body = body_;
    r.bodyLen = body_.size();
    Finish(r);
}

void NetClient::Finish(const NetResult &result)
{
    busy_ = false;
    C1_NET_INFO("%s", qPrintable(result.Summary()));
    emit Finished(result);
}
