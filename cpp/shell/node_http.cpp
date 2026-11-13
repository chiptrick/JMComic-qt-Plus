#include "node_http.h"

#include "doh_client.h"
#include "net_backend.h"
#include "quic/quic_http.h"
#include "tls/tls_channel.h"
#include "ui/proxy_config.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QList>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QTcpSocket>
#include <QTimer>

#include <memory>

struct NodeHttpJob
{
    NodeHttpRequest req;
    NodeHttp::Callback cb;

    QAbstractSocket *sock = nullptr;
    TlsChannel *tls = nullptr;   // OpenSSL 4.x 通道（带 ECH 时启用）
    QTimer *timer = nullptr;

    QByteArray raw;        // 收到的原始字节（含响应头）
    QByteArray body;       // 解析后的响应体
    int bodyStart = 0;     // raw 中响应体起点（头结束 + 4）
    bool headersDone = false;
    int status = 0;
    qint64 contentLength = -1;
    bool chunked = false;
    int chunkPos = 0;      // chunked 增量解析位置
    bool bodyComplete = false;
    bool truncated = false;
    QString location;
    QByteArray altSvc;      // HTTP/1.1 响应头 alt-svc（Q3：命中才值得走 H3）
    bool finished = false;
    int redirects = 0;
    QElapsedTimer clock;
};

namespace {

// ---------------------------------------------------------------------------
// Alt-Svc 学习（Q3）
//
// 目标：不要对"明确不支持 H3 的域名"反复尝试。缓存三种状态（进程内）：
//   * Advertised —— 响应头给了 alt-svc: h3=":443"，30 分钟内优先走 H3；
//   * Denied     —— 最近一次 H3 失败，10 分钟内直接走 TLS，不再浪费一次握手；
//   * Unknown    —— 没有信息，允许试一次（首次请求不能因为"没学过"就永远不用 H3，
//                   否则功能等于默认关闭；这一点是对文档"命中后才走 H3"的有意放宽，
//                   用"每域名最多失败一次/10 分钟"换取可用性）。
// 缓存只在进程内，冷启动重新学习——与文档允许的"先做进程内缓存"一致。
constexpr qint64 kAltSvcTtlMs = 30 * 60 * 1000;
constexpr qint64 kH3DenyMs = 10 * 60 * 1000;

struct AltSvcEntry
{
    qint64 seenAt = 0;
    bool h3 = false;
};

QHash<QString, AltSvcEntry> &AltSvcCache()
{
    static QHash<QString, AltSvcEntry> cache;
    return cache;
}

// 落盘位置与 H3 票据同根（AppDataLocation），文件名固定，便于排障时直接看。
QString AltSvcFile()
{
    static const QString path = [] {
        const QString dir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(dir);
        return dir + QStringLiteral("/alt-svc.json");
    }();
    return path;
}

void SaveAltSvcCache(bool force = false)
{
    // 2s 防抖：TTL 是分钟级，时间戳差几秒不影响判定；没必要每个响应都写盘。
    // 代价是进程最后 2s 内的更新可能丢——下次收到 alt-svc 头会重新写入。
    // force 用于「换节点清 Denied」这类必须立刻落盘的时刻。
    static qint64 lastWrite = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - lastWrite < 2000)
        return;
    lastWrite = now;

    QJsonObject root;
    for (auto it = AltSvcCache().constBegin(); it != AltSvcCache().constEnd(); ++it) {
        const qint64 ttl = it->h3 ? kAltSvcTtlMs : kH3DenyMs;
        if (now - it->seenAt >= ttl)
            continue;   // 顺手清理过期项
        QJsonObject e;
        e.insert(QStringLiteral("t"), double(it->seenAt));
        e.insert(QStringLiteral("h3"), it->h3);
        root.insert(it.key(), e);
    }
    QSaveFile f(AltSvcFile());
    if (!f.open(QIODevice::WriteOnly))
        return;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    f.commit();
}

void LoadAltSvcCache()
{
    QFile f(AltSvcFile());
    if (!f.open(QIODevice::ReadOnly))
        return;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        // 损坏自愈：删掉重学。留着坏文件会让 H3 永远被判 Unknown/失效。
        f.close();
        QFile::remove(AltSvcFile());
        return;
    }
    const QJsonObject root = doc.object();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
        const QJsonObject e = it.value().toObject();
        const qint64 seen = qint64(e.value(QStringLiteral("t")).toDouble());
        const bool h3 = e.value(QStringLiteral("h3")).toBool();
        const qint64 ttl = h3 ? kAltSvcTtlMs : kH3DenyMs;
        // now < seen 是时钟回拨，不采信（否则会长期判成 Advertised/Denied）
        if (seen <= 0 || now < seen || now - seen >= ttl)
            continue;
        AltSvcCache().insert(it.key(), AltSvcEntry{seen, h3});
    }
}

// H3Verdict 在每个请求的热路径上，加载只做一次。
void EnsureAltSvcLoaded()
{
    static bool loaded = false;
    if (loaded)
        return;
    loaded = true;
    LoadAltSvcCache();
}

void LearnAltSvc(const QString &host, const QByteArray &value)
{
    if (host.isEmpty() || value.isEmpty())
        return;
    // 只认 h3 / h3-29 这类 token；h2 的 alt-svc 与本功能无关
    const bool advertised = value.toLower().contains(QByteArrayLiteral("h3"));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    AltSvcEntry &e = AltSvcCache()[host];
    // 实测失败优先于服务端广告。H3 超时回退到 HTTP/1.1 后，那个响应同样带
    // alt-svc: h3，若照单全收就会把 Denied 改回 Advertised —— 于是每个请求都
    // 再白等一次 8s 超时（A7 实测：同一 host 连试 3 次，每次 8s）。
    if (!e.h3 && e.seenAt > 0 && now - e.seenAt < kH3DenyMs)
        return;
    e.seenAt = now;
    e.h3 = advertised;
    SaveAltSvcCache();
}

void DenyH3(const QString &host)
{
    if (host.isEmpty())
        return;
    AltSvcEntry &e = AltSvcCache()[host];
    e.seenAt = QDateTime::currentMSecsSinceEpoch();
    e.h3 = false;
    SaveAltSvcCache();
}

// Q4：换节点后重新尝试 H3+ECH 组合。Alt-Svc 的 Denied 是「上次 H3 不通」的
// 持久化结论，换一条到前端的路径后未必成立；清掉 Denied（保留 Advertised），
// 并立刻落盘，避免冷启动又把它读回来。EchH3Blocks 由 SetSelectedIp 清空。
void ClearH3Denials()
{
    auto &cache = AltSvcCache();
    bool changed = false;
    for (auto it = cache.begin(); it != cache.end();) {
        if (!it->h3) {
            it = cache.erase(it);
            changed = true;
        } else {
            ++it;
        }
    }
    if (changed)
        SaveAltSvcCache(true);
}

// 0=Unknown 1=Advertised 2=Denied
int H3Verdict(const QString &host)
{
    EnsureAltSvcLoaded();
    // 注入模式忽略缓存：验收要的是"每次都走到注入点"，不能被上一轮留下的
    // Denied 挡住（否则第二次跑回归就测不到回退路径）。
    if (!ProxyConfig::H3FailInject().isEmpty())
        return 0;
    const auto it = AltSvcCache().constFind(host);
    if (it == AltSvcCache().constEnd())
        return 0;
    const qint64 age = QDateTime::currentMSecsSinceEpoch() - it->seenAt;
    const int verdict = it->h3 && age < kAltSvcTtlMs ? 1
                        : (!it->h3 && age < kH3DenyMs ? 2 : 0);
    // 只在命中缓存时打印：用来证明"冷启动后仍然记得这个域名支持/不支持 H3"
    // （A10 的验收点，进程内缓存时代这条日志永远只在本进程内出现）。
    if (verdict != 0) {
        qWarning("h3 alt-svc: host=%s verdict=%d age=%llds",
                 qPrintable(host), verdict, static_cast<long long>(age / 1000));
    }
    return verdict;
}

int FindHeaderEnd(const QByteArray &raw)
{
    return raw.indexOf("\r\n\r\n");
}

bool ParseHead(NodeHttpJob *j)
{
    const int idx = FindHeaderEnd(j->raw);
    if (idx < 0)
        return false;

    const QList<QByteArray> lines = j->raw.left(idx).split('\n');
    if (lines.isEmpty())
        return false;

    // "HTTP/1.1 200 OK"
    const QList<QByteArray> parts = lines.first().trimmed().simplified().split(' ');
    if (parts.size() < 2)
        return false;
    bool ok = false;
    const int status = parts.at(1).toInt(&ok);
    if (!ok || status <= 0)
        return false;
    j->status = status;

    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon <= 0)
            continue;
        const QByteArray key = line.left(colon).trimmed().toLower();
        const QByteArray val = line.mid(colon + 1).trimmed();
        if (key == QByteArrayLiteral("content-length")) {
            j->contentLength = val.toLongLong();
        } else if (key == QByteArrayLiteral("transfer-encoding")) {
            if (val.toLower().contains(QByteArrayLiteral("chunked")))
                j->chunked = true;
        } else if (key == QByteArrayLiteral("location")) {
            j->location = QString::fromLatin1(val);
        } else if (key == QByteArrayLiteral("alt-svc")) {
            j->altSvc = val;
        }
    }

    j->bodyStart = idx + 4;
    j->chunkPos = j->bodyStart;
    j->headersDone = true;
    if (!j->altSvc.isEmpty())
        LearnAltSvc(j->req.sniHost.isEmpty() ? j->req.url.host() : j->req.sniHost, j->altSvc);
    return true;
}

// 增量解 chunked；返回 true 表示遇到终止块（0 长度），body 已完整。
bool FeedChunked(NodeHttpJob *j)
{
    while (true) {
        const int lineEnd = j->raw.indexOf("\r\n", j->chunkPos);
        if (lineEnd < 0)
            return false;
        const QByteArray lenHex =
            j->raw.mid(j->chunkPos, lineEnd - j->chunkPos).split(';').first().trimmed();
        bool ok = false;
        const int len = lenHex.toInt(&ok, 16);
        if (!ok || len < 0 || lenHex.isEmpty())
            return false;
        const int start = lineEnd + 2;
        if (len == 0) {
            j->chunkPos = start;
            return true;
        }
        if (j->raw.size() < start + len + 2)
            return false;
        j->body.append(j->raw.mid(start, len));
        j->chunkPos = start + len + 2;
    }
}

void TryComplete(NodeHttpJob *j)
{
    if (!j->headersDone || j->bodyComplete)
        return;
    if (j->chunked) {
        if (FeedChunked(j))
            j->bodyComplete = true;
        return;
    }
    if (j->contentLength >= 0) {
        const qint64 avail = j->raw.size() - j->bodyStart;
        if (avail >= j->contentLength) {
            j->body = j->raw.mid(j->bodyStart, int(j->contentLength));
            j->bodyComplete = true;
        }
    }
}

// 响应体是否已经"确定完整"：
//   * chunked：见到终止块才算完；
//   * 有 Content-Length：收满才算完；
//   * 两者都没有：只能等服务器断连（HTTP/1.0 风格）。
// 这是超时/断连时"算成功还是失败"的唯一依据：只收到响应头、体没下来（慢节点常见）
// 绝不能当成功，否则上层会拿着空 body 去解密，日志只剩一句"解密失败"。
bool BodySettled(const NodeHttpJob *j)
{
    if (j->bodyComplete)
        return true;
    if (!j->headersDone)
        return false;
    if (j->chunked)
        return false;
    if (j->contentLength >= 0)
        return qint64(j->raw.size() - j->bodyStart) >= j->contentLength;
    return false;
}

QString HeaderValue(const QHash<QString, QString> &headers, const char *name)
{
    const QString key = QString::fromLatin1(name);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        if (it.key().compare(key, Qt::CaseInsensitive) == 0)
            return it.value();
    }
    return QString();
}

QByteArray BuildHead(const NodeHttpRequest &req, const QString &hostHeader)
{
    QString target = req.url.path(QUrl::FullyEncoded);
    if (target.isEmpty())
        target = QStringLiteral("/");
    if (req.url.hasQuery())
        target += QLatin1Char('?') + req.url.query(QUrl::FullyEncoded);

    QByteArray head;
    head += req.method.isEmpty() ? QByteArrayLiteral("GET") : req.method;
    head += ' ';
    head += target.toLatin1();
    head += QByteArrayLiteral(" HTTP/1.1\r\nHost: ");
    head += hostHeader.toLatin1();
    head += QByteArrayLiteral("\r\n");

    bool hasLength = false;
    bool hasConnection = false;
    bool hasEncoding = false;
    for (auto it = req.headers.constBegin(); it != req.headers.constEnd(); ++it) {
        if (it.key().compare(QStringLiteral("Host"), Qt::CaseInsensitive) == 0)
            continue;   // 上面已按 sniHost 写过，避免重复 Host
        if (it.key().compare(QStringLiteral("Content-Length"), Qt::CaseInsensitive) == 0)
            hasLength = true;
        if (it.key().compare(QStringLiteral("Connection"), Qt::CaseInsensitive) == 0)
            hasConnection = true;
        if (it.key().compare(QStringLiteral("Accept-Encoding"), Qt::CaseInsensitive) == 0)
            hasEncoding = true;
        head += it.key().toLatin1();
        head += QByteArrayLiteral(": ");
        head += it.value().toLatin1();
        head += QByteArrayLiteral("\r\n");
    }
    if (!hasLength && !req.body.isEmpty())
        head += "Content-Length: " + QByteArray::number(req.body.size()) + "\r\n";
    if (!hasConnection)
        head += QByteArrayLiteral("Connection: close\r\n");
    // 不声明 gzip：自建客户端没有 zlib 解压，声明 identity 让服务端给原文
    if (!hasEncoding)
        head += QByteArrayLiteral("Accept-Encoding: identity\r\n");
    head += QByteArrayLiteral("\r\n");
    return head + req.body;
}

NodeHttpResponse MakeResponse(NodeHttpJob *j, bool ok, const QString &error)
{
    // 有头就尽量留下已收到的字节：失败时 bodyLen 能说明"收到了多少"，
    // 无定界（HTTP/1.0 风格）时这就是完整响应体。
    if (j->headersDone && j->body.isEmpty() && !j->chunked)
        j->body = j->raw.mid(j->bodyStart);

    NodeHttpResponse r;
    r.ok = ok;
    r.status = j->status;
    r.httpError = j->status >= 400;
    r.body = j->body;
    r.truncated = j->truncated;
    r.error = error;
    r.elapsedMs = j->clock.elapsed();
    r.finalUrl = j->req.url.toString();
    return r;
}

void Cleanup(const std::shared_ptr<NodeHttpJob> &j, QObject *owner)
{
    if (j->timer) {
        j->timer->stop();
        j->timer->deleteLater();
        j->timer = nullptr;
    }
    if (j->sock) {
        j->sock->disconnect(owner);
        j->sock->abort();
        j->sock->deleteLater();
        j->sock = nullptr;
    }
    if (j->tls) {
        j->tls->disconnect(owner);
        j->tls->Close();
        j->tls->deleteLater();
        j->tls = nullptr;
    }
}

}  // namespace

NodeHttp::NodeHttp(QObject *parent) : QObject(parent) {}

void NodeHttp::Send(const NodeHttpRequest &req, Callback cb)
{
    // Q4：节点变了就清掉「该域名不支持 H3」的持久化冷却，给 H3+ECH 组合
    // 一次新机会。EchH3Blocks 已由 ProxyConfig::SetSelectedIp() 清空。
    const QString selected = ProxyConfig::SelectedIp();
    if (selected != lastSelectedIp_) {
        lastSelectedIp_ = selected;
        ClearH3Denials();
    }
    Start(req, 0, std::move(cb));
}

void NodeHttp::Start(const NodeHttpRequest &req, int redirects, Callback cb)
{
    auto job = std::make_shared<NodeHttpJob>();
    job->req = req;
    job->cb = std::move(cb);
    job->redirects = redirects;
    job->clock.start();

    const QString scheme = req.url.scheme().toLower();
    const bool tls = scheme == QLatin1String("https");
    if (!req.url.isValid() || (!tls && scheme != QLatin1String("http"))) {
        job->finished = true;
        const NodeHttpResponse bad = MakeResponse(job.get(), false, QStringLiteral("bad url"));
        const Callback cb0 = job->cb;
        if (cb0)
            cb0(bad);
        return;
    }

    const QString sni = req.sniHost.isEmpty() ? req.url.host() : req.sniHost;
    const QString connectHost = req.connectIp.isEmpty() ? req.url.host() : req.connectIp;
    if (connectHost.isEmpty() || sni.isEmpty()) {
        job->finished = true;
        const NodeHttpResponse bad = MakeResponse(job.get(), false, QStringLiteral("empty host"));
        const Callback cb0 = job->cb;
        if (cb0)
            cb0(bad);
        return;
    }
    const quint16 port = quint16(req.url.port(tls ? 443 : 80));
    const QString hostHeader =
        HeaderValue(req.headers, "Host").isEmpty() ? sni : HeaderValue(req.headers, "Host");
    const QByteArray head = BuildHead(req, hostHeader);

    // HTTP/3（QUIC）优先：只对 https + GET + 无 body 生效，0.90 起默认开启。
    // 任何失败都回落到下面的 TLS/HTTP1.1，不影响业务结果。
    // Alt-Svc 学习（Q3）：被判定为 Denied 的域名在冷却期内不再尝试 H3。
    // Q4：ECH-over-QUIC 被服务端策略拒绝后，本进程内 (host, 节点) 直接走
    // TCP+ECH（隐藏 SNI 优先）；换节点或冷启动后重新尝试组合。
    const int h3Verdict = H3Verdict(sni);
    const bool echBlocked =
        ProxyConfig::EchH3Blocked(sni, ProxyConfig::EchH3NodeKey(sni, req.connectIp));
    const bool h3Eligible = tls && req.allowQuic && req.method == QByteArrayLiteral("GET")
                            && req.body.isEmpty() && ProxyConfig::Http3Enabled()
                            && quic::Available() && h3Verdict != 2 && !echBlocked;
    if (echBlocked && req.allowQuic) {
        Log(QStringLiteral("node-http ech/h3 combo suppressed for %1 (retry after restart / node change)")
                    .arg(sni));
    }
    if (req.quicOnly) {
        // NetClient 的直连探测：只借本文件的 H3 通道，不借它的 TCP/HTTP1.1 回退。
        // 没资格做 H3（开关关 / 冷却中 / POST / http）就立刻把控制权还给调用方，
        // 让它照旧走 QNetworkAccessManager（HTTP/2），不能悄悄换成另一套 HTTP 栈。
        if (!h3Eligible) {
            Settle(job, false, QLatin1String(kH3Unavailable));
            return;
        }
        StartQuic(job);
        return;
    }
    if (h3Eligible) {
        StartQuic(job);
        return;
    }

    QAbstractSocket *sock = nullptr;
    if (tls) {
        // 优先走 OpenSSL 4.x 通道：它能注入 ECHConfigList（Qt 的 QSslSocket 不行）。
        // ECH 配置为空（开关关/尚未取到）或 4.x 不可用时回退 QSslSocket。
        // Q4：回退自 H3+ECH 时 req.echConfigList 带着同一份配置，不能因为 DoH
        // 缓存过期就悄悄退化成暴露 SNI 的普通 TLS。
        const QByteArray ech = !job->req.echConfigList.isEmpty()
                                   ? job->req.echConfigList
                                   : QByteArray::fromBase64(
                                         DohClient::I().CachedEch().toLatin1());
        TlsChannel *ch = nullptr;
        if (!ech.isEmpty() && EchChannelAvailable())
            ch = CreateEchChannel(this);

        if (ch) {
            job->tls = ch;
            connect(ch, &TlsChannel::Connected, this, [ch, head] {
                // Q5：ECH 成功/失败都记一次，页面与自检读同一份计数
                ProxyConfig::NoteEch(ch->Info().echStatus == 1);
                ch->Write(head);
            });
            connect(ch, &TlsChannel::ReadyRead, this,
                    [this, job, ch] { Consume(job, ch->ReadAll()); });
            connect(ch, &TlsChannel::Error, this, [this, job](const QString &e) {
                if (!job->finished)
                    Settle(job, false, e);
            });
            connect(ch, &TlsChannel::Disconnected, this, [this, job] {
                if (job->finished)
                    return;
                TryComplete(job.get());
                const bool settled = BodySettled(job.get())
                                     || (job->headersDone && !job->chunked
                                         && job->contentLength < 0);
                Settle(job, settled,
                       settled ? QString()
                               : (job->headersDone ? QStringLiteral("closed mid-body")
                                                   : QStringLiteral("closed before response")));
            });
            connect(ch, &TlsChannel::Log, this, [this](const QString &l) { Log(l); });
            Log(QStringLiteral("node-http ech-tls host=%1 sni=%2 ech=%3B")
                    .arg(connectHost, sni)
                    .arg(ech.size()));
            // NodeHttp 只说 HTTP/1.1，ALPN 不能带 h2
            ch->Connect(connectHost, port, sni, ech, {QStringLiteral("http/1.1")});
        } else {
            auto *ssl = new QSslSocket(this);
            QSslConfiguration cfg = QSslConfiguration::defaultConfiguration();
            cfg.setPeerVerifyMode(QSslSocket::VerifyPeer);
            ssl->setSslConfiguration(cfg);
            connect(ssl, &QSslSocket::sslErrors, this, [this](const QList<QSslError> &errors) {
                for (const QSslError &e : errors)
                    Log(QStringLiteral("node-http ssl: %1").arg(e.errorString()));
            });
            connect(ssl, &QSslSocket::encrypted, this, [this, ssl, head] {
                const qint64 n = ssl->write(head);
                Log(QStringLiteral("node-http encrypted, wrote %1/%2 bytes")
                         .arg(n)
                         .arg(head.size()));
            });
            sock = ssl;
            // 关键：TCP 连 IP，TLS 的 SNI 与证书校验名用域名（等价 curl --resolve）
            ssl->connectToHostEncrypted(connectHost, port, sni);
        }
    } else {
        auto *tcp = new QTcpSocket(this);
        connect(tcp, &QTcpSocket::connected, this, [this, tcp, head] {
            const qint64 n = tcp->write(head);
            Log(QStringLiteral("node-http connected, wrote %1/%2 bytes").arg(n).arg(head.size()));
        });
        sock = tcp;
        tcp->connectToHost(connectHost, port);
    }
    job->sock = sock;
    if (sock) {

    connect(sock, &QAbstractSocket::readyRead, this,
            [this, job, sock] { Consume(job, sock->readAll()); });

    connect(sock, &QAbstractSocket::errorOccurred, this,
            [this, job](QAbstractSocket::SocketError) {
                if (job->finished)
                    return;
                // 服务器带 Connection: close 时，收完数据会报 RemoteHostClosed/TLS closed，
                // 这属于正常收尾；但只有"体确实完整"才算成功。
                TryComplete(job.get());
                const bool settled = BodySettled(job.get())
                                     || (job->headersDone && !job->chunked
                                         && job->contentLength < 0 && !job->raw.isEmpty());
                if (settled) {
                    Settle(job, true, QString());
                } else {
                    Settle(job, false,
                           job->headersDone
                               ? QStringLiteral("incomplete body")
                               : (job->sock ? job->sock->errorString()
                                            : QStringLiteral("socket error")));
                }
            });

    connect(sock, &QAbstractSocket::disconnected, this, [this, job] {
        if (job->finished)
            return;
        TryComplete(job.get());
        const bool settled = BodySettled(job.get())
                             || (job->headersDone && !job->chunked && job->contentLength < 0);
        Settle(job, settled,
               settled ? QString()
                       : (job->headersDone ? QStringLiteral("closed mid-body")
                                           : QStringLiteral("closed before response")));
    });
    }   // if (sock)：ECH 通道自己管理 socket，不走 QAbstractSocket 信号

    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, job] {
        if (job->finished)
            return;
        TryComplete(job.get());
        const bool settled = BodySettled(job.get());
        Log(QStringLiteral("node-http timeout raw=%1 headers=%2 settled=%3")
                .arg(job->raw.size())
                .arg(job->headersDone)
                .arg(settled));
        // 只收到半个 body（慢节点）→ 按超时失败，交给上层重试/换域名；
        // 以前有响应头就判成功，会产出"net OK 200 0B" 再在解密层炸掉。
        Settle(job, settled, QStringLiteral("timeout"));
    });
    timer->start(qMax(1000, req.timeoutMs));
    job->timer = timer;

    Log(QStringLiteral("node-http %1 %2 via %3 sni=%4")
            .arg(QString::fromLatin1(req.method), req.url.toString(QUrl::RemoveQuery), connectHost,
                 sni));
}

void NodeHttp::Consume(const std::shared_ptr<NodeHttpJob> &job, const QByteArray &data)
{
    if (!job || job->finished || data.isEmpty())
        return;
    job->raw += data;

    // 超过上限：保留已有部分并标记 truncated（与 NetClient 同语义）
    if (job->raw.size() > job->req.maxBytes + 64 * 1024) {
        job->truncated = true;
        if (!job->headersDone)
            ParseHead(job.get());
        TryComplete(job.get());
        if (!job->bodyComplete && job->headersDone)
            job->body = job->raw.mid(job->bodyStart);
        Settle(job, job->status > 0, QString());
        return;
    }

    if (!job->headersDone && !ParseHead(job.get()))
        return;
    TryComplete(job.get());
    if (job->bodyComplete)
        Settle(job, true, QString());
}

void NodeHttp::StartQuic(const std::shared_ptr<NodeHttpJob> &job)
{
    const QString sni = job->req.sniHost.isEmpty() ? job->req.url.host() : job->req.sniHost;
    const QString nodeKey = ProxyConfig::EchH3NodeKey(sni, job->req.connectIp);

    auto *client = new quic::Client(this);
    QObject::connect(client, &quic::Client::Log, this,
                     [](const QString &l) { qWarning("node-http h3: %s", qPrintable(l)); });

    quic::Request qr;
    qr.host = sni;
    qr.port = quint16(job->req.url.port(443));
    // A7：受控失败注入。换成没有 QUIC 服务的端口（discard），既保证一定失败，
    // 也保证不会复用 443 上已有的池化连接（池化键含 port），因此后续走的是
    // 与真实失败完全相同的 r.ok==false 分支，而不是另写一条假路径。
    if (ProxyConfig::TakeH3FailInject()) {
        qr.port = 9;
        qWarning("node-http h3 inject: forced port 9 for %s", qPrintable(sni));
    }
    qr.connectIp = job->req.connectIp;
    if (qr.connectIp.isEmpty() && ProxyConfig::DohEnabled()) {
        const QStringList ips = DohClient::I().CachedIps(sni);
        if (!ips.isEmpty())
            qr.connectIp = ips.first();
    }

    // ---------------- Q4（0.90 产品决策）：H3 + ECH 组合 ----------------
    // 历史记录：Cloudflare 前端对 ECH-over-QUIC 回 SSL_ech_get1_status=FAILED，
    // 首包 2s → 7~16s，而同一份配置在 TCP 通道是 SUCCESS。0.89 因此彻底
    // 关掉了 QUIC 侧 ECH 注入。现在的决策是「隐藏 SNI 优先」：
    //   * 默认重新尝试 H3+ECH 组合（H3 已默认开）；
    //   * 一旦服务端没接受 ECH，即使 HTTP/3 返回 200 也丢弃，回退 TCP+ECH；
    //   * 本进程内按 (host, 节点) 抑制，冷启动 / 换节点再重试，绝不落盘。
    // quicOnly 是 NetClient 直连探测：它的回退目标是 QNAM/HTTP2（Qt 没有 ECH
    // 能力），注入 ECH 后一旦被拒也无法「回退到 TCP+ECH」，所以这条路径不
    // 伪装成组合，保持 0.90 的行为。
    const QByteArray cachedEch =
        QByteArray::fromBase64(DohClient::I().CachedEch().toLatin1());
    const bool injectEchReject =
        ProxyConfig::H3FailInject() == QLatin1String("ech-reject");
    // 注入模式没有 DoH 缓存时也要让组合成立：给 QUIC 一个占位配置，
    // TCP 回退仍用真实缓存（见下方 echForTcp），避免把垃圾配置喂给 TLS 通道。
    // 只影响业务路径；quicOnly 直连探测不参与模拟，免得污染首页真实请求。
    if (injectEchReject && !job->req.quicOnly && cachedEch.isEmpty())
        qr.echConfigList = QByteArrayLiteral("injected-ech-config");
    const bool echCombo =
        !job->req.quicOnly
        && (injectEchReject
            || (ProxyConfig::EchEnabled() && !ProxyConfig::EchOff()
                && !cachedEch.isEmpty() && EchChannelAvailable()
                && !ProxyConfig::EchH3Blocked(sni, nodeKey)));
    if (echCombo && qr.echConfigList.isEmpty())
        qr.echConfigList = cachedEch;
    const QByteArray echForTcp = cachedEch;   // 回退时带同一份真实配置
    if (echCombo)
        Log(QStringLiteral("node-http h3+ech combo host=%1 node=%2 ech=%3B")
                    .arg(sni, nodeKey.isEmpty() ? QStringLiteral("direct") : nodeKey)
                    .arg(qr.echConfigList.size()));

    qr.timeoutMs = qMin(job->req.timeoutMs, 8000);   // H3 快速失败以便回退

    QByteArray path = job->req.url.path().toUtf8();
    if (path.isEmpty())
        path = "/";
    if (job->req.url.hasQuery())
        path += '?' + job->req.url.query(QUrl::FullyEncoded).toUtf8();
    qr.path = QString::fromUtf8(path);
    qr.maxBytes = job->req.maxBytes;
    for (auto it = job->req.headers.begin(); it != job->req.headers.end(); ++it) {
        if (it.key().compare(QStringLiteral("Host"), Qt::CaseInsensitive) == 0)
            continue;
        qr.headers.append(qMakePair(it.key().toLower().toUtf8(), it.value().toUtf8()));
    }

    const int redirects = job->redirects;
    auto onQuic = [this, job, client, redirects, sni, nodeKey, echCombo,
                   echForTcp](const quic::Response &r) {
        client->deleteLater();
        if (job->finished)
            return;

        // Q4 优先于 r.ok：ECH 没被服务端接受时，HTTP/3 层的成功依然是
        // 「SNI 暴露」的成功，按产品决策必须丢弃并改走 TCP+ECH。
        if (echCombo && r.echRejected) {
            ProxyConfig::NoteH3(false);
            ProxyConfig::NoteH3Fallback();
            ProxyConfig::NoteEchH3Rejected(sni, nodeKey);
            // 刻意不调 DenyH3()：那是写进 alt-svc.json 的「该域名不支持 H3」
            // 持久化结论，而这里只是本进程/该节点上的组合被拒，冷启动要重试。
            Log(QStringLiteral("node-http ech/h3 rejected ech=%1 err=%2 -> tcp+ech "
                               "(retry after restart / node change)")
                    .arg(r.echText, r.error));
            if (job->req.quicOnly) {
                Settle(job, false, QLatin1String(kH3Unavailable));
                return;
            }
            NodeHttpRequest next = job->req;
            next.allowQuic = false;
            next.echConfigList = echForTcp;   // 隐藏 SNI 不因缓存过期而丢
            const Callback cb = job->cb;
            job->finished = true;
            Cleanup(job, this);
            Start(next, redirects, cb);
            return;
        }

        if (r.ok && r.status > 0) {
            ProxyConfig::NoteH3(true);
            job->status = r.status;
            job->body = r.body;
            job->headersDone = true;
            job->truncated = r.truncated;
            job->location = r.location;
            Log(QStringLiteral("node-http h3 %1 %2 -> %3 %4B %5ms alpn=%6 ech=%7")
                    .arg(QString::fromLatin1(job->req.method),
                         job->req.url.toString(QUrl::RemoveQuery))
                    .arg(r.status)
                    .arg(r.body.size())
                    .arg(r.elapsedMs)
                    .arg(r.alpn, r.echText));
            Settle(job, true, QString());
            return;
        }
        // T2：证书错误不得被回退掩盖。QUIC 通道用 JM_CA_BUNDLE/Git 的 CA 校验，
        // 而回退的 QSslSocket 用 Qt 自己的信任库——两者信任集不同，一旦把
        // "证书校验失败"当成"传输失败"回退，就可能用另一套 CA 把请求做成成功。
        if (r.error.contains(QStringLiteral("certificate verify"))
            || r.error.contains(QStringLiteral("verify="))) {
            ProxyConfig::NoteH3(false);
            Log(QStringLiteral("node-http h3 cert failure, no fallback: %1").arg(r.error));
            // quicOnly 时把哨兵透给调用方：它必须停手，不能换一套 CA 用 QNAM 重试。
            Settle(job, false, job->req.quicOnly
                                   ? QString::fromLatin1(kH3CertFailure) + r.error
                                   : r.error);
            return;
        }

        // 回退：同一请求改用 TLS/HTTP1.1（限一次，避免来回切）
        ProxyConfig::NoteH3(false);
        ProxyConfig::NoteH3Fallback();
        // Q4 组合失败同样不写持久化 Denied：下次启动还要重新试组合。
        if (!echCombo)
            DenyH3(sni);   // 普通 H3 失败：冷却期内同域名不再试 H3
        Log(QStringLiteral("node-http h3 fallback (%1)").arg(r.error));
        if (job->req.quicOnly) {
            // 直连路径的回退目标是 QNAM/HTTP2，不是本文件自建的 HTTP/1.1。
            Settle(job, false, QLatin1String(kH3Unavailable));
            return;
        }
        NodeHttpRequest next = job->req;
        next.allowQuic = false;
        if (echCombo)
            next.echConfigList = echForTcp;
        const Callback cb = job->cb;
        job->finished = true;
        Cleanup(job, this);
        Start(next, redirects, cb);
    };

    if (injectEchReject && echCombo) {
        // 确定性验收：跳过真实 QUIC 往返（真实环境要等 8s 超时且不一定
        // 复现服务端策略），其余抑制 / 回退 / TCP+ECH 链路与真失败完全共用。
        // quicOnly 的 echCombo 恒 false，不参与模拟，避免污染首页真实请求。
        quic::Response injected;
        injected.ok = false;
        injected.echRejected = true;
        injected.echText = QStringLiteral("FAILED (injected)");
        injected.error = QStringLiteral("injected ech-over-quic policy rejection");
        qWarning("node-http h3 inject: ech-reject for %s", qPrintable(sni));
        onQuic(injected);
        return;
    }
    client->Start(qr, onQuic);
}

void NodeHttp::Settle(const std::shared_ptr<NodeHttpJob> &job, bool ok, const QString &error)
{
    if (!job || job->finished)
        return;

    // 3xx + Location：跟随重定向；换 host 时放弃 IP 覆盖（新域名不一定有同一节点）
    if (ok && job->status >= 300 && job->status < 400 && !job->location.isEmpty()
        && job->redirects < job->req.maxRedirects) {
        QUrl loc(job->location);
        if (loc.isRelative())
            loc = job->req.url.resolved(loc);
        if (loc.isValid() && !loc.host().isEmpty()) {
            const NodeHttpRequest prev = job->req;
            NodeHttpRequest next = prev;
            next.url = loc;
            if (loc.host().compare(prev.url.host(), Qt::CaseInsensitive) != 0) {
                next.connectIp.clear();
                next.sniHost = loc.host();
                for (auto it = next.headers.begin(); it != next.headers.end();) {
                    if (it.key().compare(QStringLiteral("Host"), Qt::CaseInsensitive) == 0)
                        it = next.headers.erase(it);
                    else
                        ++it;
                }
            }
            if (job->status == 303
                || ((job->status == 301 || job->status == 302)
                    && next.method == QByteArrayLiteral("POST"))) {
                next.method = QByteArrayLiteral("GET");
                next.body.clear();
            }
            const Callback cb = job->cb;
            const int nextRedirects = job->redirects + 1;
            job->finished = true;
            Cleanup(job, this);
            Log(QStringLiteral("node-http redirect %1 -> %2").arg(job->status).arg(loc.toString()));
            Start(next, nextRedirects, cb);
            return;
        }
    }

    job->finished = true;
    const NodeHttpResponse r = MakeResponse(job.get(), ok, error);
    Cleanup(job, this);
    Log(QStringLiteral("node-http %1 %2 -> %3 %4B %5ms%6")
            .arg(QString::fromLatin1(job->req.method), job->req.url.toString(QUrl::RemoveQuery))
            .arg(r.ok ? r.status : 0)
            .arg(r.body.size())
            .arg(r.elapsedMs)
            .arg(r.ok ? QString() : QStringLiteral(" (%1)").arg(r.error)));
    const Callback cb = job->cb;
    job->cb = nullptr;
    if (cb)
        cb(r);
}
