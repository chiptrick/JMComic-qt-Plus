// QML 封面图源：QQuickImageProvider 的壳 + 主线程预取的单例。
//
// 为什么这么绕：QML 的 requestImage 在 QML 加载线程调用，而 CoverCache
// （QHash 缓存、QNetworkAccessManager）只允许主线程访问；QPixmap 也只能在
// GUI 线程构造。所以壳只做三件事：
//   1. 查自己的 QImage LRU（QMutex 保护，可跨线程）；
//   2. 未命中 → 返回占位色，并把请求投递到主线程预取；
//   3. 预取命中（或 CoverCache::Loaded 网络封面到货）→ 存 LRU + bump(key)。
// ComicListModel 收到 bump 后对相应行做精确 dataChanged，QML delegate 的
// image source 里拼了 coverRev 代次，URL 变化即重取。
#pragma once

#include <QHash>
#include <QColor>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QStringList>

class QQuickImageProvider;

class CoverProvider : public QObject
{
    Q_OBJECT
public:
    static CoverProvider &I();

    // 每个 QQuickWidget 的 engine 都要一个 provider 实例（engine 接管所有权），
    // 这些壳全部转发到单例，缓存共享、命中率不受分区数量影响。
    QQuickImageProvider *MakeShell();

    // id 形如 "b64url(key).b64url(url).b64url(title).w.h.rev"——全串只含
    // base64url 字母表与 '.'，QML 的 URL percent-encode 不会改坏它
    QImage Request(const QString &id);

    // 主线程接口：让 QML 显示前先把图拉进 LRU（SetComics 后批量调用）
    void Prime(const QString &key, const QString &title, const QString &url, int w, int h);

private:
    explicit CoverProvider();

signals:
    // key = comic id（original key）。ComicListModel 据此做精确 dataChanged。
    void CoverReady(const QString &key);

private:
    struct Req
    {
        QString key, title, url;
        int w = 0, h = 0;
        QString ck;   // LRU 键：key + 尺寸
    };

    static Req Parse(const QString &id);
    void PrimeMain(const Req &r);          // 主线程：查 CoverCache 并入 LRU
    void OnNetworkLoaded(const QString &key);  // CoverCache::Loaded（网络封面到货）
    QImage Find(const QString &ck);
    void Store(const QString &ck, const QImage &img);

    QMutex mutex_;
    QHash<QString, QImage> lru_;
    QStringList order_;
    QStringList pending_;   // 已投递主线程、尚未落地的请求键，防重复预取
    QColor placeholder_;    // 占位底色：主线程快照（渲染线程不读 UiTheme）
};
