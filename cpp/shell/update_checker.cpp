#include "update_checker.h"

#include "app_paths.h"
#include "node_http.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QSysInfo>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <memory>

namespace {

// 上游发布线（本移植对齐的 Python 版本）。移植线自己发版时改这一行即可；
// 也可以由 ui.ini [update] version 或环境变量 JM_APP_VERSION 覆盖（回归用）。
constexpr const char *kReleaseLine = "v1.3.7";

// Python config.AppUrl / AppUrl2 / AppUrl3
const char *kDefaultBases[] = {
    "https://app.jpacg.cc/JMComic",
    "https://app2.jpacg.cc/JMComic",
    "https://app3.jpacg.cc/JMComic",
};

QString PlatformTag()
{
    // Python 侧是 platform.platform()，服务端只当查询参数用；这里给一个稳定可读的值。
    const QString os = QSysInfo::productType();       // windows / android / osx
    const QString ver = QSysInfo::productVersion();   // 11 / 15 ...
    return ver.isEmpty() ? os : os + QLatin1Char('-') + ver;
}

QString ConfigIniPath()
{
    const QString f = AppPaths::ConfigFile(QStringLiteral("ui.ini"));
    return f;
}

}  // namespace

UpdateChecker::UpdateChecker(QObject *parent) : QObject(parent), http_(new NodeHttp(this))
{
    bases_ = DefaultBases();
    connect(http_, &NodeHttp::Log, this, [this](const QString &l) { emit Log(l); });
}

QString UpdateChecker::ApplicationVersion()
{
    const QString v = QCoreApplication::applicationVersion();
    return v.isEmpty() ? QStringLiteral("0.0") : v;
}

QString UpdateChecker::ReleaseLine()
{
    const QString env = qEnvironmentVariable("JM_APP_VERSION").trimmed();
    if (!env.isEmpty())
        return env;
    // ui.ini 里允许覆盖：不同分支（例如跟 Python 主干或自建发行线）不必改代码。
    const QString ini = ConfigIniPath();
    if (!ini.isEmpty()) {
        QSettings s(ini, QSettings::IniFormat);
        const QString v = s.value(QStringLiteral("update/version")).toString().trimmed();
        if (!v.isEmpty())
            return v;
    }
    return QString::fromLatin1(kReleaseLine);
}

QStringList UpdateChecker::DefaultBases()
{
    const QString env = qEnvironmentVariable("JM_UPDATE_BASE").trimmed();
    if (!env.isEmpty()) {
        QStringList out;
        for (const QString &s : env.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
            const QString t = s.trimmed();
            if (!t.isEmpty())
                out << t;
        }
        if (!out.isEmpty())
            return out;
    }
    QStringList out;
    for (const char *b : kDefaultBases)
        out << QString::fromLatin1(b);
    return out;
}

void UpdateChecker::SetBases(const QStringList &bases)
{
    if (bases.isEmpty())
        return;
    bases_ = bases;
    baseIndex_ = 0;
}

int UpdateChecker::VersionWeight(const QString &version, bool preRelease)
{
    static const QRegularExpression re(QStringLiteral("(\\d+)"));
    auto it = re.globalMatch(version);
    QVector<int> parts;
    while (it.hasNext() && parts.size() < 4)
        parts.append(it.next().captured(1).toInt());
    if (parts.isEmpty())
        return -1;
    while (parts.size() < 3)
        parts.append(0);
    int weight = parts.at(0) * 1000 + parts.at(1) * 100 + parts.at(2) * 10;
    if (parts.size() >= 4)
        weight += parts.at(3);
    // 非预发布：忽略第四段（Python: version //= 10）
    if (!preRelease)
        weight /= 10;
    return weight;
}

bool UpdateChecker::IsNewer(const QString &remote, const QString &local, bool preRelease)
{
    const int r = VersionWeight(remote, preRelease);
    const int l = VersionWeight(local, preRelease);
    if (r < 0 || l < 0)
        return false;
    return r > l;
}

QString UpdateChecker::ParseVersionText(const QByteArray &text)
{
    const QString t = QString::fromUtf8(text).trimmed();
    if (t.isEmpty() || t.size() > 32)
        return QString();
    static const QRegularExpression re(QStringLiteral("^v?(\\d{1,3}(\\.\\d{1,3}){1,3})$"));
    return re.match(t).hasMatch() ? t : QString();
}

UpdateChecker::Config UpdateChecker::ParseConfig(const QByteArray &text, bool *ok)
{
    Config c;
    const QString all = QString::fromUtf8(text);
    int lines = 0;
    for (const QString &raw : all.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')))
            continue;
        const int eq = line.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        const QString key = line.left(eq).trimmed();
        const QString val = line.mid(eq + 1).trimmed();
        ++lines;
        if (key == QLatin1String("Ver")) {
            c.ver = val.toInt();
        } else if (key == QLatin1String("VerTime")) {
            c.verTime = val;
        } else if (key == QLatin1String("HeaderVer")) {
            c.headerVer = val;
        } else if (key == QLatin1String("UrlList")) {
            c.webUrls = val.split(QLatin1Char(','), Qt::SkipEmptyParts);
        } else if (key == QLatin1String("Url2List")) {
            c.apiUrls = val.split(QLatin1Char(','), Qt::SkipEmptyParts);
        } else if (key == QLatin1String("PicUrlList")) {
            c.imageUrls = val.split(QLatin1Char(','), Qt::SkipEmptyParts);
        } else if (key == QLatin1String("ProxyApiUrl")) {
            c.proxyApi = val;
        } else if (key == QLatin1String("CdnApiUrl")) {
            c.cdnApi = val;
        }
    }
    for (QStringList *list : {&c.webUrls, &c.apiUrls, &c.imageUrls}) {
        for (QString &u : *list)
            u = u.trimmed();
        list->removeAll(QString());
    }
    // 只有 Ver 或域名表至少有一项才算"解析到了东西"：错误页/空响应不能冒充配置。
    c.ok = lines > 0 && (c.ver > 0 || !c.webUrls.isEmpty() || !c.apiUrls.isEmpty());
    if (ok)
        *ok = c.ok;
    return c;
}

void UpdateChecker::RotateBase(const QString &why)
{
    if (bases_.size() <= 1)
        return;
    baseIndex_ = (baseIndex_ + 1) % bases_.size();
    emit Log(QStringLiteral("update base -> %1（%2）").arg(bases_.value(baseIndex_), why));
}

void UpdateChecker::GetText(const QString &path, int timeoutMs,
                            const std::function<void(bool, int, const QByteArray &, const QString &)> &cb)
{
    // base 轮换：网络层失败才换（与 JmApi 同口径）；HTTP 4xx/5xx 说明已经连上了。
    auto attempt = std::make_shared<std::function<void(int)>>();
    auto self = this;
    *attempt = [self, path, timeoutMs, cb, attempt](int tried) {
        const QString base = self->bases_.value(self->baseIndex_);
        NodeHttpRequest req;
        req.method = QByteArrayLiteral("GET");
        QUrl url(base + path);
        if (!url.isValid()) {
            cb(false, 0, QByteArray(), QStringLiteral("更新地址非法：%1").arg(base + path));
            return;
        }
        req.url = url;
        req.timeoutMs = timeoutMs;
        req.maxBytes = 256 * 1024;
        // 固定走 TLS/HTTP1.1：这条路径要的是"稳定拿到文本"，不需要 H3 的额外延迟。
        req.allowQuic = false;
        req.headers.insert(QStringLiteral("Accept"), QStringLiteral("text/plain,*/*"));
        self->http_->Send(req, [self, tried, cb, attempt](const NodeHttpResponse &r) {
            if (r.ok && r.status >= 200 && r.status < 300) {
                cb(true, r.status, r.body, QString());
                return;
            }
            if (!r.ok && tried + 1 < self->bases_.size()) {
                self->RotateBase(r.error.isEmpty() ? QStringLiteral("transport") : r.error);
                (*attempt)(tried + 1);
                return;
            }
            cb(false, r.status, r.body,
               r.ok ? QStringLiteral("HTTP %1").arg(r.status)
                    : (r.error.isEmpty() ? QStringLiteral("网络失败") : r.error));
        });
    };
    (*attempt)(0);
}

void UpdateChecker::Check()
{
    if (checking_)
        return;
    checking_ = true;
    const QString local = ReleaseLine();
    const bool pre = qEnvironmentVariable("JM_UPDATE_PRERELEASE") == QLatin1String("1");
    const QString file = pre ? QStringLiteral("/version_pre.txt") : QStringLiteral("/version.txt");

    QUrlQuery q;
    q.addQueryItem(QStringLiteral("version"), local);
    q.addQueryItem(QStringLiteral("platform"), PlatformTag());
    const QString path = file + QLatin1Char('?') + q.toString(QUrl::FullyEncoded);

    emit Log(QStringLiteral("update check: local=%1 pre=%2 base=%3")
                 .arg(local, pre ? QStringLiteral("1") : QStringLiteral("0"),
                      bases_.value(baseIndex_)));

    GetText(path, 12000, [this, local, pre](bool ok, int status, const QByteArray &body,
                                            const QString &error) {
        if (!ok) {
            checking_ = false;
            emit Log(QStringLiteral("update check failed: status=%1 err=%2").arg(status).arg(error));
            emit CheckFinished(false, false, QString(), error);
            return;
        }
        const QString remote = ParseVersionText(body);
        if (remote.isEmpty()) {
            checking_ = false;
            emit Log(QStringLiteral("update check: 非法版本正文 len=%1").arg(body.size()));
            emit CheckFinished(false, false, QString(),
                               QStringLiteral("版本文件格式不符（返回的不是版本号）"));
            return;
        }
        if (!IsNewer(remote, local, pre)) {
            checking_ = false;
            emit Log(QStringLiteral("update check: already latest (remote=%1 local=%2)")
                         .arg(remote, local));
            emit CheckFinished(true, false, remote, QString());
            return;
        }
        emit Log(QStringLiteral("update check: new version %1 (local=%2)").arg(remote, local));
        emit CheckFinished(true, true, remote, QString());

        // 拉更新日志：与 Python 一致，路径是 <base>/<新版本>.txt
        QUrlQuery q2;
        q2.addQueryItem(QStringLiteral("version"), local);
        q2.addQueryItem(QStringLiteral("platform"), PlatformTag());
        const QString version = remote.startsWith(QLatin1Char('v')) ? remote
                                                                    : QLatin1Char('v') + remote;
        const QString logPath = QStringLiteral("/%1.txt?%2")
                                    .arg(version, q2.toString(QUrl::FullyEncoded));
        GetText(logPath, 12000, [this, remote](bool ok2, int status2, const QByteArray &body2,
                                               const QString &error2) {
            checking_ = false;
            const QString text = QString::fromUtf8(body2).trimmed();
            if (!ok2 || text.isEmpty()) {
                emit Log(QStringLiteral("update log failed: status=%1 err=%2")
                             .arg(status2)
                             .arg(error2));
                emit ChangelogReady(false, remote, QString(),
                                    error2.isEmpty() ? QStringLiteral("更新日志为空") : error2);
                return;
            }
            emit ChangelogReady(true, remote, text, QString());
        });
    });
}

void UpdateChecker::FetchConfig()
{
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("version"), ReleaseLine());
    q.addQueryItem(QStringLiteral("platform"), PlatformTag());
    const QString path = QStringLiteral("/config.txt?") + q.toString(QUrl::FullyEncoded);

    GetText(path, 12000, [this](bool ok, int status, const QByteArray &body, const QString &error) {
        if (!ok) {
            emit Log(QStringLiteral("update config failed: status=%1 err=%2").arg(status).arg(error));
            emit ConfigReady(false, config_, error);
            return;
        }
        bool parsed = false;
        const Config c = ParseConfig(body, &parsed);
        if (!parsed) {
            emit Log(QStringLiteral("update config: 解析失败 len=%1").arg(body.size()));
            emit ConfigReady(false, config_, QStringLiteral("配置内容无法解析"));
            return;
        }
        config_ = c;
        emit Log(QStringLiteral("update config: Ver=%1 VerTime=%2 web=%3 api=%4 img=%5 header=%6")
                     .arg(c.ver)
                     .arg(c.verTime)
                     .arg(c.webUrls.size())
                     .arg(c.apiUrls.size())
                     .arg(c.imageUrls.size())
                     .arg(c.headerVer));
        // 只落"账号网页流程需要"的字段：域名表缓存下来，下次启动即使更新服务不可达，
        // 注册/找回密码仍有可用的 Web 域名。API/图床域名不落盘、也不应用
        // （它们由 ProxyConfig 负责，擅自覆盖会改变已验收的线路行为）。
        const QString ini = ConfigIniPath();
        if (!ini.isEmpty() && !c.webUrls.isEmpty()) {
            {
                QSettings s(ini, QSettings::IniFormat);
                s.setValue(QStringLiteral("update/ver"), c.ver);
                s.setValue(QStringLiteral("update/verTime"), c.verTime);
                s.setValue(QStringLiteral("update/webUrls"), c.webUrls.join(QLatin1Char(',')));
                s.setValue(QStringLiteral("update/checkedAt"),
                           QDateTime::currentDateTime().toString(Qt::ISODate));
            }
        }
        emit ConfigReady(true, c, QString());
    });
}
