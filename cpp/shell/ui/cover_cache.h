// 封面加载：内存缓存（LRU）→ 磁盘缓存 → 本地文件 / HTTP → 失败回落生成图。
//
// 为什么要单独一层：
//   * 卡片会被反复重绘（滚动、hover），每次 QImage 解码会把滚动打成 PPT；
//   * 列表虚拟化后 delegate 是"画完就忘"，缓存必须挂在 view 之外；
//   * 真实接口给的是 URL（http 或本地路径），没有网络时要有稳定占位图；
//   * 内存必须有上限：一张 168×224@2x 的封面约 1.2 MB，几千条不设限就是 OOM。
#pragma once

#include <QHash>
#include <QList>
#include <QObject>
#include <QPixmap>
#include <QQueue>
#include <QSet>
#include <QSize>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
class NodeHttp;
struct NodeHttpResponse;

class CoverCache : public QObject
{
    Q_OBJECT
public:
    static CoverCache &I();

    // 返回立即可用的图：缓存命中 → 解码后的真实封面；否则先给生成图并异步加载。
    QPixmap Get(const QString &key, const QString &title, const QString &url, const QSize &size);

    // 预热（可选）：列表拿到数据后批量请求，滚动时不至于一片占位图
    void Prefetch(const QString &key, const QString &title, const QString &url, const QSize &size);

    bool IsPending(const QString &key) const { return pending_.contains(key); }

    // 统计（自检/压测用）
    int EntryCount() const { return mem_.size(); }
    qint64 Bytes() const { return bytes_; }
    int Hits() const { return hits_; }
    int Misses() const { return misses_; }
    void Clear();
    QString Dir() const;

signals:
    void Loaded(const QString &key);

private:
    explicit CoverCache(QObject *parent = nullptr);

    QPixmap LoadLocal(const QString &path, const QSize &size) const;
    void LoadLocalAsync(const QString &key, const QString &path, const QSize &size);
    QPixmap CoverArtFallback(const QString &title, const QSize &size) const;
    QPixmap ScaleCrop(const QImage &img, const QSize &size) const;
    void FetchHttp(const QString &key, const QString &url, const QSize &size);
    // 同一张封面可能有 .jpg/.webp/.png 三种规格：某个规格 404 时按序回退，
    // 全部失败才标记 failed（JM 的相册封面并不总是 _3x4.jpg）。
    void FetchHttpTry(const QString &key, const QString &url, const QSize &size, int domainTry,
                      int specTry);
    static QString NextSpec(const QString &url);
    // 图片 CDN 轮换：真机实测同一张封面 jmapiproxy1 要 14s、cdn-msp2 只要 2.7s，
    // 某个域名超时/失败就换下一个，而不是干等。
    QString NextDomain(const QString &url);

    struct Pending {
        QString key;
        QString url;
        QSize size;
        int domainTry = 0;
        int specTry = 0;
    };
    void PumpHttp();
    void StartRequest(const Pending &p);
    // 两种传输（QNAM / NodeHttp）共用的收尾：重试、解码、落盘、发信号
    void OnHttpDone(const Pending &p, bool transportOk, int status,
                    const QByteArray &data, const QString &errText);
    // 缓存键解析：HTTP 源拼 URL（图片 CDN 轮换会变域名），本地源用原 key
    static QString ResolvedKey(const QString &key, const QString &url);
    static QString OriginalKey(const QString &resolved);
    static QString CacheDir();
    static QString DiskPath(const QString &cacheKey);

    void Insert(const QString &key, const QPixmap &pm);
    void Touch(const QString &key);
    void EvictIfNeeded();

    QHash<QString, QPixmap> mem_;
    QList<QString> lru_;        // 头部最新
    qint64 bytes_ = 0;
    int hits_ = 0;
    int misses_ = 0;

    QSet<QString> pending_;
    QSet<QString> failed_;
    // 原始 key → 最近一次用过的封面 URL。
    // HTTP 源的缓存键是 "key|url"，而 CoverCache::Loaded 的订阅方（QML provider /
    // 详情页）只拿得到原始 key；没有这张表，url 为空的回查会落到另一个键上，
    // 命中不到刚下载的图（冷缓存时表现为"封面永远是占位图"）。
    QHash<QString, QString> urlByKey_;
    QNetworkAccessManager *nam_ = nullptr;
    NodeHttp *nodeHttp_ = nullptr;
    QStringList imageDomains_;   // 与 JmApi::DefaultImageDomains 同表
    int imageIndex_ = 0;
    // 全局限并发：同时最多 3 个封面请求（真机 CDN 对突发并发敏感）
    static constexpr int kMaxInflight = 3;
    QQueue<Pending> queue_;
    int inflight_ = 0;

    // 上限：条目数 + 字节数双约束（先到者触发淘汰）
    static constexpr int kMaxEntries = 400;
    static constexpr qint64 kMaxBytes = 96LL * 1024 * 1024;
};
