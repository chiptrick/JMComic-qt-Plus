#include "cover_provider.h"

#include <QQuickImageProvider>
#include <QTimer>
#include <utility>

#include "cover_cache.h"
#include "models.h"
#include "theme.h"

namespace {

// LRU 容量：一屏 3 列 × 若干屏 + 预载，60 张足够；封面 480x640x4 ≈ 1.2MB/张
constexpr int kLruCapacity = 60;

QString B64(const QString &s)
{
    return QString::fromLatin1(s.toUtf8().toBase64());
}

QString UnB64(const QString &s)
{
    return QString::fromUtf8(QByteArray::fromBase64(
        s.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

// QQuickImageProvider 壳：engine 接管所有权，requestImage 在 QML 加载线程调用
class CoverImageShell : public QQuickImageProvider
{
public:
    CoverImageShell() : QQuickImageProvider(QQuickImageProvider::Image) {}

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override
    {
        Q_UNUSED(size);
        Q_UNUSED(requestedSize);
        return CoverProvider::I().Request(id);
    }
};

}   // namespace

CoverProvider &CoverProvider::I()
{
    static CoverProvider p;
    return p;
}

CoverProvider::CoverProvider()
{
    // 占位底色在主线程快照一次（Request 在 QML 加载线程跑，不能读 UiTheme）
    placeholder_ = UiTheme::I().Tokens().surface;
    // 网络封面到货（CoverCache::Insert 后 emit Loaded，主线程）→ 转进 LRU
    connect(&CoverCache::I(), &CoverCache::Loaded,
            this, &CoverProvider::OnNetworkLoaded);
}

QQuickImageProvider *CoverProvider::MakeShell()
{
    return new CoverImageShell;
}

CoverProvider::Req CoverProvider::Parse(const QString &id)
{
    const QStringList parts = id.split(QLatin1Char('.'));
    Req r;
    if (parts.size() < 6)
        return r;
    r.key = UnB64(parts.at(0));
    r.url = UnB64(parts.at(1));
    r.title = UnB64(parts.at(2));
    r.w = qMax(1, parts.at(3).toInt());
    r.h = qMax(1, parts.at(4).toInt());
    // parts.at(5) 是 coverRev，只用于让 QML 的 URL 变化，参与解析忽略
    r.ck = r.key + QStringLiteral("@%1x%2").arg(r.w).arg(r.h);
    return r;
}

QImage CoverProvider::Find(const QString &ck)
{
    QMutexLocker lock(&mutex_);
    const auto it = lru_.constFind(ck);
    if (it == lru_.constEnd())
        return {};
    order_.removeAll(ck);
    order_.append(ck);
    return it.value();
}

void CoverProvider::Store(const QString &ck, const QImage &img)
{
    QMutexLocker lock(&mutex_);
    if (lru_.contains(ck))
        order_.removeAll(ck);
    lru_.insert(ck, img);
    order_.append(ck);
    while (order_.size() > kLruCapacity) {
        const QString oldest = order_.takeFirst();
        lru_.remove(oldest);
    }
}

QImage CoverProvider::Request(const QString &id)
{
    const Req r = Parse(id);
    if (r.key.isEmpty())
        return {};

    if (const QImage hit = Find(r.ck); !hit.isNull())
        return hit;

    // 未命中：先给一张占位色（直角风格，与页面 surface 一致），同时往主线程
    // 投递预取。到货后 bump → model dataChanged → QML 的 URL 变化重取。
    if (!QMetaObject::invokeMethod(this, [this, r] { PrimeMain(r); },
                                   Qt::QueuedConnection)) {
        qWarning("cover-provider: queued prime failed for %s", qPrintable(r.key));
    }
    QImage placeholder(r.w, r.h, QImage::Format_ARGB32_Premultiplied);
    {
        QMutexLocker lock(&mutex_);
        placeholder.fill(placeholder_);
    }
    return placeholder;
}

void CoverProvider::Prime(const QString &key, const QString &title, const QString &url,
                          int w, int h)
{
    Req r;
    r.key = key;
    r.title = title;
    r.url = url;
    r.w = qMax(1, w);
    r.h = qMax(1, h);
    r.ck = r.key + QStringLiteral("@%1x%2").arg(r.w).arg(r.h);
    if (!QMetaObject::invokeMethod(this, [this, r] { PrimeMain(r); },
                                   Qt::QueuedConnection))
        qWarning("cover-provider: queued prime failed for %s", qPrintable(key));
}

void CoverProvider::PrimeMain(const Req &r)
{
    {
        QMutexLocker lock(&mutex_);
        if (lru_.contains(r.ck) || pending_.contains(r.ck))
            return;
        pending_.append(r.ck);
    }
    // 主线程：允许访问 CoverCache / QPixmap
    const QPixmap pm = CoverCache::I().Get(r.key, r.title, r.url, QSize(r.w, r.h));
    {
        QMutexLocker lock(&mutex_);
        pending_.removeAll(r.ck);
    }
    if (!pm.isNull()) {
        Store(r.ck, pm.toImage());
        emit CoverReady(r.key);   // 占位 → 真图，触发 QML 重取
    }
    // pm 为空（url 空 + fallback 也为空）时不再 bump：占位图就是最终态
}

void CoverProvider::OnNetworkLoaded(const QString &key)
{
    // 网络封面到货（CoverCache::Loaded）：可能对应多个尺寸请求；这里从
    // CoverCache 把已缓存的 pixmap 转进 LRU，逐尺寸 bump。
    QMutexLocker lock(&mutex_);
    QStringList sized;
    for (const QString &ck : std::as_const(order_)) {
        const int at = ck.lastIndexOf(QLatin1Char('@'));
        if (at > 0 && ck.left(at) == key)
            sized << ck;
    }
    lock.unlock();

    bool bumped = false;
    for (const QString &ck : std::as_const(sized)) {
        const int at = ck.lastIndexOf(QLatin1Char('@'));
        const QStringList wh = ck.mid(at + 1).split(QLatin1Char('x'));
        if (wh.size() != 2)
            continue;
        const int w = wh.at(0).toInt();
        const int h = wh.at(1).toInt();
        if (w <= 0 || h <= 0)
            continue;
        const QPixmap pm = CoverCache::I().Get(key, QString(), QString(), QSize(w, h));
        if (!pm.isNull()) {
            Store(ck, pm.toImage());
            bumped = true;
        }
    }
    if (bumped)
        emit CoverReady(key);
}
