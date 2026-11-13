// DNS-over-HTTPS 客户端：对齐 Python 版 DnsOverHttpsReq / GetEchConfigReq。
//
// Python 侧语义（server/req.py）：
//   * DnsOverHttpsReq：GET <DohAddress>?name=<host>&type=A，
//     Accept/Content-Type: application/dns-json，timeout=5，读 Answer[].data。
//   * GetEchConfigReq：POST <DohAddress>，Content-Type/Accept: application/dns-message，
//     body 是自己拼的 DNS wire 查询（qtype=65 HTTPS），解析 SvcParam key=5 拿到
//     ECHConfigList 的 base64；地址列表按 DohUrlList 顺序轮换。
//   * 配置：Setting.DohAddress（默认 https://parse.jpacg.cc/parse）、
//     Setting.EnableEch（默认 1）、Setting.IsOpenHTTP3（默认 0）；
//     ECH 配置 15 分钟刷新一次，失败 1 分钟重试（login_new_view.InitEchConfigBack）。
//
// C++ 侧把 DoH 结果真正接到流量上：解析到的 A 记录交给 ProxyConfig::ResolveRoute，
// 由 NodeHttp 连该 IP、域名做 SNI（等价 curl --doh-url + --resolve）。
// ECH 配置同样通过 DoH 的 HTTPS 记录获取并持久化；Qt 6.12 的 TLS 后端没有
// ECH / HTTP3 接口，因此这两项只提供配置、能力探测与降级状态，见 ProxyConfig。
#pragma once

#include <functional>

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

// 一次 DoH 解析结果
struct DohResult
{
    QString host;
    QStringList ips;
    QString echConfig;   // ECHConfigList 的 base64（仅 HTTPS 查询有值）
    bool fromDoh = false;
    int ttlSec = 300;
    QString error;
};

class DohClient : public QObject
{
    Q_OBJECT
public:
    static DohClient &I();

    // 从 ProxyConfig 读进来的配置；可在运行中变更（页面保存时调用）
    void Configure(const QString &dohAddress, bool echEnabled);
    // 是否把 DoH 解析结果用于真实请求（ProxyConfig::DohEnabled 同步进来）
    void SetActive(bool on) { active_ = on; }
    bool Active() const { return active_; }

    // 同步读缓存（供 ResolveRoute / JmApi 在构造请求时调用，不发网络请求）
    QStringList CachedIps(const QString &host) const;
    QString CachedEch() const;
    QString EchStatus() const;
    QString DohAddress() const { return address_; }

    // 异步解析 A 记录；缓存未命中时顺带预热，本次请求可先走系统 DNS
    void Resolve(const QString &host);
    // 页面「检查」按钮：只测给定地址，不轮换
    void CheckAddress(const QString &address, const QString &testHost);
    // 获取 ECH 配置（DNS wire / HTTPS RR），按 Python 的 15min/1min 节奏重试
    void FetchEchConfig();
    void SetEchEnabled(bool on);

    QStringList CandidateAddresses() const;

    // DNS wire 工具（Python GetEchConfigReq 的同义实现，供自检/测试直接调用）
    static QByteArray BuildDnsQuery(const QString &domain, quint16 qtype);
    static DohResult ParseWire(const QByteArray &response, const QString &host);
    static DohResult ParseJson(const QByteArray &response, const QString &host);

signals:
    void Resolved(const QString &host, const QStringList &ips, bool viaDoh,
                  const QString &error);
    void Checked(const QString &address, bool ok, const QString &detail);
    void EchReady(const QString &base64, bool ok, const QString &detail);
    void Log(const QString &line);

private:
    explicit DohClient(QObject *parent = nullptr);
    void ResolveTry(const QString &host, int index);
    void WireQuery(const QString &host, quint16 qtype, int index,
                   std::function<void(const DohResult &)> done);
    void JsonQuery(const QString &host, const QString &address,
                   std::function<void(const DohResult &)> done);
    void Cache(const DohResult &r);
    void ScheduleEch(int seconds);

    QNetworkAccessManager *nam_ = nullptr;
    QTimer *echTimer_ = nullptr;
    QString address_;
    bool echEnabled_ = true;
    bool active_ = false;
    bool echFetching_ = false;
    QString echConfig_;
    QString echStatus_;
    struct Entry {
        QStringList ips;
        qint64 expires = 0;
    };
    QHash<QString, Entry> cache_;
    QHash<QString, bool> resolving_;
};
