#include "doh_client.h"

#include "net_backend.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QHostAddress>
#include <QHostInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QtEndian>

namespace {

// Python GlobalConfig.DohUrlList / EchDomain（顺序一致，配置地址会插到最前）
const char *kDohList[] = {
    "https://parse.jpacg.cc/parse",
    "https://doh.pub/dns-query",
    "https://parse2.jpacg.cc/parse",
    "https://dot.pub/dns-query",
};
const char *kEchDomain = "cloudflare-ech.com";
constexpr int kTimeoutMs = 5000;
constexpr int kEchRefreshSec = 15 * 60;
constexpr int kEchRetrySec = 60;

quint16 Be16(const QByteArray &b, int off)
{
    if (off < 0 || off + 2 > b.size())
        return 0;
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(b.constData() + off));
}

quint32 Be32(const QByteArray &b, int off)
{
    if (off < 0 || off + 4 > b.size())
        return 0;
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(b.constData() + off));
}

// DNS 名称：普通 label 或 0xC0 压缩指针
int SkipName(const QByteArray &p, int off)
{
    while (off < p.size()) {
        const quint8 len = static_cast<quint8>(p.at(off));
        if ((len & 0xC0) == 0xC0)
            return off + 2;
        if (len == 0)
            return off + 1;
        off += len + 1;
    }
    return off;
}

void Put16(QByteArray &b, quint16 v)
{
    b.append(char((v >> 8) & 0xFF));
    b.append(char(v & 0xFF));
}

}  // namespace

DohClient &DohClient::I()
{
    static DohClient *inst = new DohClient();
    return *inst;
}

DohClient::DohClient(QObject *parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
    echTimer_ = new QTimer(this);
    echTimer_->setSingleShot(true);
    connect(echTimer_, &QTimer::timeout, this, &DohClient::FetchEchConfig);
}

QStringList DohClient::CandidateAddresses() const
{
    QStringList out;
    if (!address_.trimmed().isEmpty())
        out << address_.trimmed();
    for (const char *u : kDohList) {
        const QString s = QString::fromLatin1(u);
        if (!out.contains(s))
            out << s;
    }
    return out;
}

void DohClient::Configure(const QString &dohAddress, bool echEnabled)
{
    const bool changed = address_ != dohAddress || echEnabled_ != echEnabled;
    if (address_ != dohAddress)
        cache_.clear();   // 换了 DoH 地址，旧解析结果不能复用
    address_ = dohAddress;
    echEnabled_ = echEnabled;
    if (!echEnabled) {
        echTimer_->stop();
        echStatus_ = QStringLiteral("ECH 已关闭");
        return;
    }
    if (!echFetching_ && (changed || echConfig_.isEmpty())) {
        echStatus_ = QStringLiteral("正在获取 ECH 配置…");
        FetchEchConfig();
    }
}

QStringList DohClient::CachedIps(const QString &host) const
{
    const auto it = cache_.constFind(host);
    if (it == cache_.constEnd())
        return {};
    if (it.value().expires < QDateTime::currentMSecsSinceEpoch())
        return {};
    return it.value().ips;
}

QString DohClient::CachedEch() const
{
    return echEnabled_ ? echConfig_ : QString();
}

QString DohClient::EchStatus() const
{
    return echStatus_;
}

void DohClient::Cache(const DohResult &r)
{
    if (r.ips.isEmpty() || !r.fromDoh)
        return;
    Entry e;
    e.ips = r.ips;
    e.expires = QDateTime::currentMSecsSinceEpoch() + qint64(qBound(30, r.ttlSec, 3600)) * 1000;
    cache_.insert(r.host, e);
}

QByteArray DohClient::BuildDnsQuery(const QString &domain, quint16 qtype)
{
    // Python GetEchConfigReq.build_dns_query 同构：标准 recursive query，1 条 question
    QByteArray q("\x00\x01\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00", 12);
    for (const QString &label : domain.split(QLatin1Char('.'))) {
        const QByteArray lb = label.toUtf8();
        if (lb.isEmpty() || lb.size() > 63)
            continue;
        q.append(char(lb.size()));
        q.append(lb);
    }
    q.append('\0');
    Put16(q, qtype);
    Put16(q, 1);
    return q;
}

DohResult DohClient::ParseJson(const QByteArray &response, const QString &host)
{
    DohResult r;
    r.host = host;
    const QJsonDocument doc = QJsonDocument::fromJson(response);
    if (!doc.isObject())
        return r;
    const QJsonArray answers = doc.object().value(QStringLiteral("Answer")).toArray();
    for (const QJsonValue &v : answers) {
        const QJsonObject o = v.toObject();
        if (o.value(QStringLiteral("type")).toInt() != 1)
            continue;
        const QString ip = o.value(QStringLiteral("data")).toString().trimmed();
        QHostAddress a;
        if (!a.setAddress(ip) || a.protocol() != QAbstractSocket::IPv4Protocol)
            continue;
        if (!r.ips.contains(ip))
            r.ips << ip;
        const int ttl = o.value(QStringLiteral("TTL")).toInt();
        if (ttl > 0 && (r.ttlSec == 300 || ttl < r.ttlSec))
            r.ttlSec = ttl;
    }
    r.fromDoh = !r.ips.isEmpty();
    return r;
}

DohResult DohClient::ParseWire(const QByteArray &response, const QString &host)
{
    DohResult r;
    r.host = host;
    if (response.size() < 12)
        return r;
    const quint16 qd = Be16(response, 4);
    const quint16 an = Be16(response, 6);
    int off = 12;
    for (int i = 0; i < qd; ++i)
        off = SkipName(response, off) + 4;
    if (off > response.size())
        return r;

    for (int i = 0; i < an; ++i) {
        off = SkipName(response, off);
        if (off + 10 > response.size())
            break;
        const quint16 type = Be16(response, off);
        off += 2;                                  // class
        off += 2;
        const quint32 ttl = Be32(response, off);   // ttl
        off += 4;
        const quint16 len = Be16(response, off);
        off += 2;
        if (off + len > response.size())
            break;
        const QByteArray data = response.mid(off, len);
        off += len;
        if (ttl > 0 && (r.ttlSec == 300 || int(ttl) < r.ttlSec))
            r.ttlSec = int(ttl);

        if (type == 1 && data.size() >= 4) {
            const QHostAddress a(qFromBigEndian<quint32>(
                reinterpret_cast<const uchar *>(data.constData())));
            const QString ip = a.toString();
            if (!r.ips.contains(ip))
                r.ips << ip;
        } else if (type == 65 && r.echConfig.isEmpty() && data.size() >= 3) {
            // HTTPS RR: priority(2) + target(name) + SvcParam(key:2,len:2,value)
            int p = 2;
            p = SkipName(data, p);
            while (p + 4 <= data.size()) {
                const quint16 key = Be16(data, p);
                const quint16 vlen = Be16(data, p + 2);
                p += 4;
                if (p + vlen > data.size())
                    break;
                if (key == 5)
                    r.echConfig = QString::fromLatin1(data.mid(p, vlen).toBase64());
                p += vlen;
            }
        }
    }
    r.fromDoh = !r.ips.isEmpty() || !r.echConfig.isEmpty();
    return r;
}

void DohClient::JsonQuery(const QString &host, const QString &address,
                          std::function<void(const DohResult &)> done)
{
    QUrl url(address);
    QUrlQuery q(url.query());
    q.addQueryItem(QStringLiteral("name"), host);
    q.addQueryItem(QStringLiteral("type"), QStringLiteral("A"));
    url.setQuery(q);
    QNetworkRequest req{url};
    req.setRawHeader("Accept", "application/dns-json");
    req.setRawHeader("Content-Type", "application/dns-json");
    req.setTransferTimeout(kTimeoutMs);
    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, host, done] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            DohResult r;
            r.host = host;
            r.error = reply->errorString();
            done(r);
            return;
        }
        done(ParseJson(reply->readAll(), host));
    });
}

void DohClient::WireQuery(const QString &host, quint16 qtype, int index,
                          std::function<void(const DohResult &)> done)
{
    const QStringList urls = CandidateAddresses();
    if (index >= urls.size()) {
        DohResult r;
        r.host = host;
        r.error = QStringLiteral("all DoH endpoints failed");
        done(r);
        return;
    }
    QNetworkRequest req{QUrl(urls.at(index))};
    req.setRawHeader("Accept", "application/dns-message");
    req.setRawHeader("Content-Type", "application/dns-message");
    req.setTransferTimeout(kTimeoutMs);
    const QByteArray query = BuildDnsQuery(host, qtype);
    QNetworkReply *reply = nam_->post(req, query);
    connect(reply, &QNetworkReply::finished, this, [this, reply, host, qtype, index, done] {
        reply->deleteLater();
        DohResult r;
        if (reply->error() == QNetworkReply::NoError)
            r = ParseWire(reply->readAll(), host);
        else
            r.error = reply->errorString();
        if (r.fromDoh) {
            done(r);
            return;
        }
        emit Log(QStringLiteral("doh wire %1 qtype=%2 via %3: %4")
                     .arg(host)
                     .arg(qtype)
                     .arg(CandidateAddresses().value(index),
                          r.error.isEmpty() ? QStringLiteral("no answer") : r.error));
        WireQuery(host, qtype, index + 1, done);
    });
}

void DohClient::Resolve(const QString &host)
{
    if (host.isEmpty() || resolving_.contains(host))
        return;
    if (!CachedIps(host).isEmpty())
        return;
    resolving_.insert(host, true);
    ResolveTry(host, 0);
}

void DohClient::ResolveTry(const QString &host, int index)
{
    const QStringList urls = CandidateAddresses();
    if (index >= urls.size()) {
        // Python：DoH 失败并不阻断请求，回落系统 DNS
        QHostInfo::lookupHost(host, this, [this, host](const QHostInfo &info) {
            QStringList ips;
            for (const QHostAddress &a : info.addresses()) {
                if (a.protocol() == QAbstractSocket::IPv4Protocol && !ips.contains(a.toString()))
                    ips << a.toString();
            }
            resolving_.remove(host);
            emit Log(QStringLiteral("doh fallback %1 -> %2 (%3)")
                         .arg(host, ips.isEmpty() ? QStringLiteral("-") : ips.join(QLatin1Char(',')),
                              info.errorString()));
            emit Resolved(host, ips, false, info.errorString());
        });
        return;
    }

    const QString address = urls.at(index);
    JsonQuery(host, address, [this, host, index, address](const DohResult &json) {
        if (json.fromDoh) {
            Cache(json);
            resolving_.remove(host);
            emit Log(QStringLiteral("doh json %1 -> %2 via %3")
                         .arg(host, json.ips.join(QLatin1Char(',')), address));
            emit Resolved(host, json.ips, true, QString());
            return;
        }
        // 标准 DoH 端点（doh.pub / dot.pub）不返回 dns-json，改用 DNS wire
        WireQuery(host, 1, index, [this, host, index](const DohResult &wire) {
            if (wire.fromDoh) {
                Cache(wire);
                resolving_.remove(host);
                emit Log(QStringLiteral("doh wire %1 -> %2")
                             .arg(host, wire.ips.join(QLatin1Char(','))));
                emit Resolved(host, wire.ips, true, QString());
                return;
            }
            ResolveTry(host, index + 1);
        });
    });
}

void DohClient::CheckAddress(const QString &address, const QString &testHost)
{
    const QString host = testHost.isEmpty() ? QStringLiteral("www.cdnhjk.net") : testHost;
    JsonQuery(host, address, [this, address, host](const DohResult &json) {
        if (json.fromDoh) {
            emit Checked(address, true, QStringLiteral("%1 -> %2").arg(host, json.ips.join(
                                                                                 QLatin1Char(','))));
            return;
        }
        // 再试 wire 形式；只测这一个地址，不轮换（Python CheckDohReq 同语义）
        const QUrl u(address);
        QNetworkRequest req{u};
        req.setRawHeader("Accept", "application/dns-message");
        req.setRawHeader("Content-Type", "application/dns-message");
        req.setTransferTimeout(kTimeoutMs);
        QNetworkReply *reply = nam_->post(req, BuildDnsQuery(host, 1));
        connect(reply, &QNetworkReply::finished, this, [this, reply, address, host] {
            reply->deleteLater();
            if (reply->error() == QNetworkReply::NoError) {
                const DohResult r = ParseWire(reply->readAll(), host);
                if (r.fromDoh) {
                    emit Checked(address, true,
                                 QStringLiteral("%1 -> %2").arg(host, r.ips.join(QLatin1Char(','))));
                    return;
                }
            }
            emit Checked(address, false,
                         reply->error() == QNetworkReply::NoError
                             ? QStringLiteral("no answer")
                             : reply->errorString());
        });
    });
}

void DohClient::SetEchEnabled(bool on)
{
    Configure(address_, on);
}

void DohClient::ScheduleEch(int seconds)
{
    if (!echEnabled_)
        return;
    echTimer_->start(seconds * 1000);
}

void DohClient::FetchEchConfig()
{
    if (!echEnabled_ || echFetching_)
        return;
    echFetching_ = true;
    echTimer_->stop();
    WireQuery(QString::fromLatin1(kEchDomain), 65, 0, [this](const DohResult &r) {
        echFetching_ = false;
        if (r.fromDoh && !r.echConfig.isEmpty()) {
            echConfig_ = r.echConfig;
            // 真实注入点：只有 TLS 后端提供 SSL_set1_ech_config_list 才能成功。
            // 当前 Schannel / Qt 绑定返回 false，日志里写明原因，页面据此显示回退。
            const QByteArray list = QByteArray::fromBase64(echConfig_.toLatin1());
            const bool injected = NetBackend::SetEchConfig(list);
            echStatus_ = QStringLiteral("ECH 配置已获取（%1 字节）%2，15 分钟后刷新")
                             .arg(list.size())
                             .arg(injected ? QStringLiteral("，已交给 ECH 通道")
                                           : QStringLiteral("，当前 TLS 后端无法注入"));
            emit Log(QStringLiteral("ech config ok len=%1 injected=%2%3")
                         .arg(list.size())
                         .arg(int(injected))
                         .arg(injected ? QString()
                                       : QStringLiteral(" (") + NetBackend::LastError()
                                             + QStringLiteral(")")));
            emit EchReady(echConfig_, true, echStatus_);
            ScheduleEch(kEchRefreshSec);
            return;
        }
        echStatus_ = QStringLiteral("ECH 配置获取失败：%1，1 分钟后重试")
                         .arg(r.error.isEmpty() ? QStringLiteral("no HTTPS ech param") : r.error);
        emit Log(echStatus_);
        emit EchReady(QString(), false, echStatus_);
        ScheduleEch(kEchRetrySec);
    });
}
