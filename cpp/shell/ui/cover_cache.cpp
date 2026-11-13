#include "cover_cache.h"

#include "node_http.h"

#include <cstdio>

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include "../jm_webp.h"
#include "cache_path.h"
#include "proxy_config.h"
#include "ui_kit.h"

CoverCache &CoverCache::I()
{
    static CoverCache cache;
    return cache;
}

CoverCache::CoverCache(QObject *parent) : QObject(parent)
{
    // 与 JmApi::DefaultImageDomains 保持同一张表（顺序也一致，便于日志复现）
    imageDomains_ = {QStringLiteral("cdn-msp.jmapiproxy1.cc"),
                     QStringLiteral("cdn-msp.jmapiproxy2.cc"),
                     QStringLiteral("cdn-msp2.jmapiproxy2.cc")};
}

QString CoverCache::NextDomain(const QString &url)
{
    QUrl u(url);
    if (!u.isValid() || imageDomains_.isEmpty())
        return QString();
    for (int i = 0; i < imageDomains_.size(); ++i) {
        imageIndex_ = (imageIndex_ + 1) % imageDomains_.size();
        const QString host = imageDomains_.at(imageIndex_);
        if (host != u.host()) {
            u.setHost(host);
            return u.toString();
        }
    }
    return QString();
}

QString CoverCache::CacheDir()
{
    // 测试/受限环境：JM_COVER_CACHE 指定目录（与 JM_SESSION_FILE 同一套思路）
    const QString override = qEnvironmentVariable("JM_COVER_CACHE");
    if (!override.isEmpty()) {
        QDir().mkpath(override);
        return override;
    }
    // 设置页的自定义缓存目录（cache/covers）
    return CachePath::CoverDir();
}

QString CoverCache::Dir() const
{
    return CacheDir();
}

QString CoverCache::DiskPath(const QString &cacheKey)
{
    // 文件名取 key 的 md5：key 里可能有 "detail:123" 这类带冒号的串，
    // 直接当文件名在 Windows 上非法。
    const QByteArray hash =
        QCryptographicHash::hash(cacheKey.toUtf8(), QCryptographicHash::Md5).toHex();
    return QDir(CacheDir()).filePath(QString::fromLatin1(hash) + QStringLiteral(".png"));
}

QPixmap CoverCache::CoverArtFallback(const QString &title, const QSize &size) const
{
    return CoverArt::Render(title, size, int(qHash(title) & 0x7fffffff));
}

QString CoverCache::OriginalKey(const QString &resolved)
{
    // ResolvedKey 会在 HTTP 源上拼 "|<url>"；信号订阅方（详情页等）只认原始 key。
    return resolved.section(QLatin1Char('|'), 0, 0);
}

QString CoverCache::ResolvedKey(const QString &key, const QString &url)
{
    // 同一 key（作品 id）在不同封面源下是两张不同的图：
    // 图片 CDN 会随分流设置轮换域名，同一 id 在切换线路后 URL 也不同。
    // HTTP 源把 URL 拼进缓存键；本地路径保持原 key（文件内容即标识）。
    if (url.startsWith(QLatin1String("http"), Qt::CaseInsensitive))
        return key + QLatin1Char('|') + url;
    return key;
}

QPixmap CoverCache::Get(const QString &key, const QString &title, const QString &url,
                        const QSize &size)
{
    if (key.isEmpty())
        return CoverArtFallback(title, size);
    // 记录/复用原始 key 对应的 URL：OnNetworkLoaded 回查时 url 为空，
    // 必须还原成同一个 "key|url"，否则永远读不到刚下载的那张图。
    QString effUrl = url;
    if (!url.isEmpty())
        urlByKey_.insert(key, url);
    else
        effUrl = urlByKey_.value(key);
    const QString ck = ResolvedKey(key, effUrl);

    auto it = mem_.find(ck);
    if (it != mem_.end()) {
        ++hits_;
        Touch(ck);
        return it.value();
    }

    // 磁盘缓存：先于网络，避免每次启动都重下同一批封面
    const QString disk = DiskPath(ck);
    if (QFileInfo::exists(disk)) {
        QPixmap pm;
        if (pm.load(disk) && !pm.isNull()) {
            Insert(ck, pm);
            return pm;
        }
        QFile::remove(disk);   // 损坏的缓存文件直接丢掉，别每次启动都卡一下
    }

    ++misses_;
    if (url.isEmpty())
        return CoverArtFallback(title, size);

    if (url.startsWith(QLatin1String("http"), Qt::CaseInsensitive)) {
        if (!failed_.contains(ck) && !pending_.contains(ck))
            FetchHttp(ck, url, size);
        return CoverArtFallback(title, size);
    }

    // 本地文件：直接同步解码并返回真图。
    // 之前走 LoadLocalAsync，它在 Get 返回之前就 emit Loaded —— 订阅方（QML provider）
    // 此刻还没把占位图存进 LRU，bump 找不到条目，于是冷缓存下封面永远停在占位图
    // （有磁盘缓存时 Get 会提前命中，所以这个 bug 只在第一次运行/换缓存目录时出现）。
    if (failed_.contains(ck))
        return CoverArtFallback(title, size);
    const QPixmap local = LoadLocal(url, size);
    if (local.isNull()) {
        failed_.insert(ck);
        return CoverArtFallback(title, size);
    }
    Insert(ck, local);
    local.toImage().save(DiskPath(ck), "PNG");
    return local;
}

void CoverCache::Prefetch(const QString &key, const QString &title, const QString &url,
                          const QSize &size)
{
    if (!key.isEmpty() && !url.isEmpty())
        urlByKey_.insert(key, url);
    const QString ck = ResolvedKey(key, url);
    if (key.isEmpty() || url.isEmpty() || mem_.contains(ck) || pending_.contains(ck)
        || failed_.contains(ck) || QFileInfo::exists(DiskPath(ck)))
        return;
    if (url.startsWith(QLatin1String("http"), Qt::CaseInsensitive))
        FetchHttp(ck, url, size);
    else
        LoadLocalAsync(ck, url, size);
}

QPixmap CoverCache::LoadLocal(const QString &path, const QSize &size) const
{
    QImage img(path);
    if (img.isNull()) {
        // Qt 没带 qwebp：本地 .webp 走内嵌解码
        QFile f(path);
        if (f.open(QIODevice::ReadOnly)) {
            const QByteArray data = f.readAll();
            if (JmWebp::IsWebp(data))
                img = JmWebp::Decode(data);
        }
    }
    return ScaleCrop(img, size);
}

void CoverCache::LoadLocalAsync(const QString &key, const QString &path, const QSize &size)
{
    pending_.insert(key);
    const QPixmap pm = LoadLocal(path, size);
    pending_.remove(key);
    if (pm.isNull()) {
        failed_.insert(key);
    } else {
        Insert(key, pm);
        pm.toImage().save(DiskPath(key), "PNG");
    }
    emit Loaded(OriginalKey(key));
}

QPixmap CoverCache::ScaleCrop(const QImage &img, const QSize &size) const
{
    if (img.isNull() || size.isEmpty())
        return QPixmap();
    const QImage scaled = img.scaled(size, Qt::KeepAspectRatioByExpanding,
                                     Qt::SmoothTransformation);
    const int x = qMax(0, (scaled.width() - size.width()) / 2);
    const int y = qMax(0, (scaled.height() - size.height()) / 2);
    return QPixmap::fromImage(scaled.copy(x, y, size.width(), size.height()));
}

void CoverCache::FetchHttp(const QString &cacheKey, const QString &url, const QSize &size)
{
    if (pending_.contains(cacheKey))
        return;
    pending_.insert(cacheKey);
    FetchHttpTry(cacheKey, url, size, 0, 0);
}

QString CoverCache::NextSpec(const QString &url)
{
    // 只对自家封面命名做回退（media/albums/<id>_3x4.<ext>）：
    // JM 的相册封面并不总是 .jpg，个别作品只有 .webp/.png。
    if (!url.contains(QLatin1String("_3x4.")))
        return QString();
    const int dot = url.lastIndexOf(QLatin1Char('.'));
    if (dot < 0)
        return QString();
    const QString base = url.left(dot);
    const QString ext = url.mid(dot + 1).toLower();
    if (ext == QLatin1String("jpg"))
        return base + QStringLiteral(".webp");
    if (ext == QLatin1String("webp"))
        return base + QStringLiteral(".png");
    return QString();
}

void CoverCache::FetchHttpTry(const QString &cacheKey, const QString &url, const QSize &size,
                              int domainTry, int specTry)
{
    // 真机实测（vivo V2463A / Android 17）：一次性发 60 个封面请求时 CDN 会直接
    // 断连（RemoteHostClosedError，http=0 bytes=0），逐张重试也全部失败。
    // 因此这里做**全局限并发 + 排队**：同时最多 3 个在飞，且失败后 250ms 再试。
    Pending p;
    p.key = cacheKey;
    p.url = url;
    p.size = size;
    p.domainTry = domainTry;
    p.specTry = specTry;
    queue_.enqueue(p);
    PumpHttp();
}

void CoverCache::PumpHttp()
{
    if (!nam_)
        nam_ = new QNetworkAccessManager(this);

    while (inflight_ < kMaxInflight && !queue_.isEmpty()) {
        const Pending p = queue_.dequeue();
        StartRequest(p);
    }
}

void CoverCache::StartRequest(const Pending &p)
{
    ++inflight_;

    // 分流：选中 CDN 线路且已选节点时，封面请求也走节点（连接落 IP，
    // SNI / Host / 证书校验名仍是图床域名）。QNAM 做不到自定义 SNI，只能走 NodeHttp。
    const RoutePlan plan = ProxyConfig::ResolveRoute(QUrl(p.url), false);
    if (plan.viaNode) {
        if (!nodeHttp_) {
            nodeHttp_ = new NodeHttp(this);
            connect(nodeHttp_, &NodeHttp::Log, this,
                    [](const QString &line) { qWarning("cover: %s", qPrintable(line)); });
        }
        NodeHttpRequest hr;
        hr.url = plan.url;
        hr.connectIp = plan.connectIp;
        hr.sniHost = plan.sniHost;
        hr.headers.insert(QStringLiteral("User-Agent"),
                          QStringLiteral("JMComic/0.88 (Qt %1)")
                              .arg(QString::fromLatin1(qVersion())));
        hr.headers.insert(QStringLiteral("Referer"), QStringLiteral("https://www.cdnhjk.net/"));
        hr.headers.insert(QStringLiteral("Accept"),
                          QStringLiteral("image/avif,image/webp,image/*,*/*;q=0.8"));
        hr.timeoutMs = 15000;
        hr.maxBytes = 8 * 1024 * 1024;
        nodeHttp_->Send(hr, [this, p](const NodeHttpResponse &r) {
            OnHttpDone(p, r.ok, r.status, r.body, r.error);
        });
        return;
    }

    // 线路 6 的 URL 已被 ResolveRoute 改写成反代形式；其余保持原域名
    QNetworkRequest req{plan.url};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    // 弱网下单张封面可能十几秒：15s 传输超时就换 CDN 重试。
    req.setTransferTimeout(15000);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("JMComic/0.88 (Qt %1)").arg(QString::fromLatin1(qVersion())));
    // 不少图床会校验 Referer；缺了照样 200，但并发时更容易被断连。
    req.setRawHeader("Referer", "https://www.cdnhjk.net/");
    req.setRawHeader("Accept", "image/avif,image/webp,image/*,*/*;q=0.8");

    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, p] {
        reply->deleteLater();
        const bool transportOk = reply->error() == QNetworkReply::NoError;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = transportOk ? reply->readAll() : QByteArray();
        OnHttpDone(p, transportOk, status, data, reply->errorString());
    });
}

void CoverCache::OnHttpDone(const Pending &p, bool transportOk, int status,
                            const QByteArray &data, const QString &errText)
{
    --inflight_;

    auto retry = [&]() {
        Pending next = p;
        if (p.domainTry < 2) {
            const QString d = NextDomain(p.url);
            if (!d.isEmpty()) {
                next.url = d;
                next.domainTry = p.domainTry + 1;
                QTimer::singleShot(250, this, [this, next] {
                    queue_.enqueue(next);
                    PumpHttp();
                });
                return true;
            }
        }
        if (p.specTry < 2) {
            const QString s = NextSpec(p.url);
            if (!s.isEmpty()) {
                next.url = s;
                next.domainTry = 0;
                next.specTry = p.specTry + 1;
                QTimer::singleShot(250, this, [this, next] {
                    queue_.enqueue(next);
                    PumpHttp();
                });
                return true;
            }
        }
        return false;
    };

    if (!transportOk || status >= 400) {
        if (retry()) {
            PumpHttp();
            return;
        }
        pending_.remove(p.key);
        static int shown = 0;
        if (shown++ < 3)
            qWarning("cover http failed: %s -> %s (http=%d)", qPrintable(p.url),
                     qPrintable(errText), status);
        failed_.insert(p.key);
        emit Loaded(OriginalKey(p.key));
        PumpHttp();
        return;
    }

    QImage img;
    if (!img.loadFromData(data) && JmWebp::IsWebp(data))
        img = JmWebp::Decode(data);   // JM CDN 全是 webp

    if (img.isNull()) {
        if (retry()) {
            PumpHttp();
            return;
        }
        pending_.remove(p.key);
        failed_.insert(p.key);
        emit Loaded(OriginalKey(p.key));
        PumpHttp();
        return;
    }

    const QPixmap pm = ScaleCrop(img, p.size);
    pending_.remove(p.key);
    if (pm.isNull()) {
        failed_.insert(p.key);
    } else {
        Insert(p.key, pm);
        const QString diskPath = DiskPath(p.key);
        if (!pm.toImage().save(diskPath, "PNG"))
            qWarning("cover save failed: %s", qPrintable(diskPath));
    }
    emit Loaded(OriginalKey(p.key));
    PumpHttp();
}

void CoverCache::Insert(const QString &key, const QPixmap &pm)
{
    if (pm.isNull())
        return;
    auto it = mem_.find(key);
    if (it != mem_.end()) {
        bytes_ -= qint64(it.value().width()) * it.value().height() * 4;
        it.value() = pm;
    } else {
        mem_.insert(key, pm);
    }
    bytes_ += qint64(pm.width()) * pm.height() * 4;
    Touch(key);
    EvictIfNeeded();
}

void CoverCache::Touch(const QString &key)
{
    lru_.removeAll(key);
    lru_.prepend(key);
}

void CoverCache::EvictIfNeeded()
{
    while (!lru_.isEmpty() && (mem_.size() > kMaxEntries || bytes_ > kMaxBytes)) {
        const QString key = lru_.takeLast();
        auto it = mem_.find(key);
        if (it == mem_.end())
            continue;
        bytes_ -= qint64(it.value().width()) * it.value().height() * 4;
        mem_.erase(it);
    }
}

void CoverCache::Clear()
{
    mem_.clear();
    lru_.clear();
    bytes_ = 0;
    failed_.clear();
    pending_.clear();
    hits_ = 0;
    misses_ = 0;
}
