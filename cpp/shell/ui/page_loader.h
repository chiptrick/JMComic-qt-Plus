// 在线章节加载：/chapter 拿页表 → 并发下载 → WebP 解码 → scramble 还原 → 落盘。
//
// 为什么单独一层而不是塞进 ReaderPage：
//   * 一个章节十几到几十页，每页 1~5 MB，必须限并发（同时 6 个）并带重试；
//   * 还原需要 chapter id + scramble_id + 文件名三者一起算分条数，逻辑属于协议层；
//   * 还原后的图要缓存到磁盘（换章再回来不该重新下一遍），缓存键是章节 id + 页序号。
//
// 与 JmApi 的分工：JmApi 只发一次 /chapter（含 /chapter_view_template），
// 图片走独立的 QNetworkAccessManager —— NetClient 是单通道，用它下图片会把
// 整个 API 队列堵死。
#pragma once

#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include "jm_api.h"

class QNetworkAccessManager;
class QNetworkReply;
class NodeHttp;
struct NodeHttpResponse;

class PageLoader : public QObject
{
    Q_OBJECT
public:
    explicit PageLoader(JmApi *api, QObject *parent = nullptr);

    // 一次只处理一个章节；重复调用会取消上一次（旧回调按 generation 丢弃）。
    void Load(const QString &chapterId, const QString &label);
    void Cancel();

    bool IsBusy() const { return busy_; }
    QString CurrentChapter() const { return chapterId_; }

    // 缓存目录（测试可覆盖：JM_PAGE_CACHE）
    static QString CacheRoot();

signals:
    void Progress(int done, int total);
    // 单页就绪（成功落盘）：阅读器据此"先显示当前页"，不必等整章下完。
    // 缓存命中的页在 StartImages 里就会补发一遍，保证调用方拿到的顺序一致。
    void PageReady(int index, int total, const QString &path);
    void Finished(bool ok, const QString &label, const QStringList &files, const QString &error);
    void Log(const QString &line);

private:
    void StartImages();
    // 还原入参变过之后，旧缓存（用错分块数还原的 jpg）必须作废重下
    void InvalidateStaleCache();
    void Pump();
    void Download(int index);
    // 发一次页图请求（attempt=0 首试，1 换 CDN 后重试）；两种传输共用收尾
    void Request(int index, int attempt);
    void HandlePage(int index, bool netError, int status, const QByteArray &data,
                    const QString &errText, int attempt);
    void MaybeFinished();
    void FinishSuccess();
    QString OutPath(int index) const;
    // 把 URL 的 host 换成当前图片 CDN（域名轮换后用）
    static QString WithImageDomain(const QString &url, const QString &domain);

    JmApi *api_ = nullptr;
    QNetworkAccessManager *nam_ = nullptr;
    NodeHttp *nodeHttp_ = nullptr;

    bool busy_ = false;
    int generation_ = 0;

    QString chapterId_;
    QString label_;
    QString photoId_;
    QString scrambleId_;
    QStringList names_;      // 接口给的 images（"00001.webp" ...）
    QStringList urls_;
    QString outDir_;

    QQueue<int> queue_;
    QSet<int> running_;
    QVector<QString> results_;
    int done_ = 0;      // 成功页数（含缓存命中）
    int settled_ = 0;   // 已结算页数（成功 + 失败），用它判断"全部回来了"
    int total_ = 0;
    QString error_;
};
