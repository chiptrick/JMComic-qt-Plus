#include "page_loader.h"

#include <QDateTime>
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

#include <cstdio>

#include "../jm_crypto.h"
#include "../jm_webp.h"
#include "cache_path.h"
#include "node_http.h"
#include "proxy_config.h"

namespace {

constexpr int kMaxInFlight = 3;
constexpr qint64 kMaxImageBytes = 16LL * 1024 * 1024;
// 缓存代次：还原入参（分块数用的是带扩展名还是主干名字）变过就必须重下，
// 否则磁盘上那批"文件名正常、大小正常、内容错位"的旧图会被一直复用。
constexpr int kCacheSchema = 2;

QString SafeName(const QString &s)
{
    QString out = s;
    for (QChar &c : out) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('-'))
            c = QLatin1Char('_');
    }
    return out;
}

}  // namespace

PageLoader::PageLoader(JmApi *api, QObject *parent) : QObject(parent), api_(api)
{
    nam_ = new QNetworkAccessManager(this);

    connect(api_, &JmApi::ChapterReady, this,
            [this](bool ok, const JmChapterDetail &ch, const QString &error) {
                if (!busy_)
                    return;
                // 切章后旧响应可能晚到：响应里的 id 必须就是当前请求的那一章。
                if (!ch.id.isEmpty() && !chapterId_.isEmpty() && ch.id != chapterId_)
                    return;
                if (!ok || ch.images.isEmpty()) {
                    busy_ = false;
                    emit Finished(false, label_, {},
                                  error.isEmpty() ? QStringLiteral("章节没有图片") : error);
                    return;
                }
                scrambleId_ = ch.scrambleId.isEmpty() ? QStringLiteral("220980") : ch.scrambleId;
                photoId_ = ch.id.isEmpty() ? chapterId_ : ch.id;
                urls_ = ch.images;
                names_.clear();
                // 分带数 = md5(epsId + 文件名) 的末位，文件名必须**不带扩展名**：
                // Python 侧 pictureName 来自 name.split(".")[0]。带 ".webp" 会算出
                // 另一种分带数，还原出来的页是错位的横条（实测 1471982 第 1 页）。
                for (const QString &u : urls_) {
                    QString name = u.section(QLatin1Char('/'), -1);
                    const int dot = name.lastIndexOf(QLatin1Char('.'));
                    if (dot > 0)
                        name.truncate(dot);
                    names_ << name;
                }
                StartImages();
            });
}

QString PageLoader::CacheRoot()
{
    const QString override = qEnvironmentVariable("JM_PAGE_CACHE");
    if (!override.isEmpty())
        return override;
    // 跟随设置页的"缓存目录"（见 cache_path.h）
    return CachePath::PageDir();
}

void PageLoader::Load(const QString &chapterId, const QString &label)
{
    if (chapterId.isEmpty()) {
        emit Finished(false, label, {}, QStringLiteral("章节 id 为空"));
        return;
    }
    Cancel();
    ++generation_;
    busy_ = true;
    chapterId_ = chapterId;
    label_ = label;
    total_ = 0;
    done_ = 0;
    settled_ = 0;
    error_.clear();
    urls_.clear();
    names_.clear();
    results_.clear();
    queue_.clear();
    running_.clear();
    scrambleId_.clear();
    photoId_ = chapterId;
    // 缓存版本号：v1 的页是用"带扩展名"的分带数还原的（错位横条），
    // 改对算法后不能复用旧文件，否则用户看到的还是坏图。换目录即可失效，
    // 旧目录留给清理策略处理，不做原地删除。
    // 目录名管"算法换代"；同一代内再调参数（如分块数入参）由目录里的
    // .scramble-v 标记兜底，见 InvalidateStaleCache()。
    outDir_ = QDir(CacheRoot()).filePath(QStringLiteral("v2/") + SafeName(chapterId));

    emit Log(QStringLiteral("page-load start chapter=%1").arg(chapterId));
    emit Progress(0, 0);
    api_->FetchChapter(chapterId);
}

void PageLoader::Cancel()
{
    if (!busy_)
        return;
    ++generation_;
    busy_ = false;
    queue_.clear();
    running_.clear();
    results_.clear();
}

// 缓存代次不对就只清掉本章目录里的 *.jpg（目录是我们自己的缓存，不碰用户文件）。
// 只看"文件存在且 >1KB"是不够的：用错分块数还原出来的 jpg 同样存在、同样有大小。
void PageLoader::InvalidateStaleCache()
{
    const QDir dir(outDir_);
    const QString stampPath = dir.filePath(QStringLiteral(".scramble-v"));
    QFile stamp(stampPath);
    if (stamp.open(QIODevice::ReadOnly)) {
        const QByteArray have = stamp.readAll().trimmed();
        stamp.close();
        if (have == QByteArray::number(kCacheSchema))
            return;
    }

    const QStringList stale = dir.entryList(QStringList{QStringLiteral("*.jpg")}, QDir::Files);
    for (const QString &name : stale)
        QFile::remove(dir.filePath(name));
    if (stamp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        stamp.write(QByteArray::number(kCacheSchema));
        stamp.close();
    }
    emit Log(QStringLiteral("page-cache reset chapter=%1 (schema=%2, dropped=%3)")
                 .arg(chapterId_)
                 .arg(kCacheSchema)
                 .arg(stale.size()));
}

void PageLoader::StartImages()
{
    total_ = urls_.size();
    results_.resize(total_);
    done_ = 0;
    settled_ = 0;
    QDir().mkpath(outDir_);
    InvalidateStaleCache();

    for (int i = 0; i < total_; ++i) {
        const QString path = OutPath(i);
        if (QFileInfo::exists(path) && QFileInfo(path).size() > 1024) {
            results_[i] = path;
            ++done_;
            ++settled_;
            continue;
        }
        queue_.enqueue(i);
    }
    // 先报总页数：阅读器要按它铺占位页（"当前位置先显示"的前提）。
    emit Progress(done_, total_);
    // 缓存命中的页立刻交付，阅读器第一帧就能出图（换章再回来时几乎是瞬开）。
    for (int i = 0; i < total_; ++i) {
        if (!results_.at(i).isEmpty())
            emit PageReady(i, total_, results_.at(i));
    }
    if (queue_.isEmpty()) {
        FinishSuccess();
        return;
    }
    Pump();
}

void PageLoader::Pump()
{
    while (running_.size() < kMaxInFlight && !queue_.isEmpty()) {
        const int idx = queue_.dequeue();
        running_.insert(idx);
        Download(idx);
    }
}

void PageLoader::Download(int index)
{
    Request(index, 0);
}

void PageLoader::Request(int index, int attempt)
{
    if (!busy_ || index < 0 || index >= urls_.size())
        return;
    const int gen = generation_;
    const RoutePlan plan = ProxyConfig::ResolveRoute(QUrl(urls_.at(index)), false);
    QUrl imgUrl = plan.url;
    const QString imgHost = QUrl(urls_.at(index)).host();

    // 路由优先级由 ProxyConfig::ResolveRoute 统一裁决：
    // 线路 6 反代改写 URL；线路 5 + 有效节点才走 NodeHttp；否则域名直连。
    if (plan.viaNode) {
        if (!nodeHttp_) {
            nodeHttp_ = new NodeHttp(this);
            connect(nodeHttp_, &NodeHttp::Log, this, [](const QString &line) {
                qWarning("page-http: %s", qPrintable(line));
            });
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
        hr.timeoutMs = 20000;
        hr.maxBytes = kMaxImageBytes;
        nodeHttp_->Send(hr, [this, index, gen, attempt](const NodeHttpResponse &r) {
            if (gen != generation_ || !busy_)
                return;
            HandlePage(index, !r.ok, r.status, r.body, r.error, attempt);
        });
        return;
    }

    QNetworkRequest req{imgUrl};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("JMComic/0.88 (Qt %1)").arg(QString::fromLatin1(qVersion())));
    // 与封面一致：缺 Referer 时同一 CDN 在高并发下会断连（真机实测 RemoteHostClosed）
    req.setRawHeader("Referer", "https://www.cdnhjk.net/");
    req.setRawHeader("Accept", "image/avif,image/webp,image/*,*/*;q=0.8");
    req.setTransferTimeout(20000);
    req.setMaximumRedirectsAllowed(5);

    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, index, gen, attempt] {
        reply->deleteLater();
        // 已切章：直接丢弃。不要动 running_/settled_ —— 那是新章节的状态，
        // 旧回调减一下就把新章节的进度打乱了。
        if (gen != generation_ || !busy_)
            return;
        const bool netError = reply->error() != QNetworkReply::NoError;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = netError ? QByteArray() : reply->readAll();
        HandlePage(index, netError, status, data, reply->errorString(), attempt);
    });
}

void PageLoader::HandlePage(int index, bool netError, int status, const QByteArray &data,
                            const QString &errText, int attempt)
{
    // 失败的两种情况都值得换一次图片 CDN：网络层错误（被墙/超时）与 404
    // （该 CDN 没这份文件，另一个镜像往往有）。
    if ((netError || status >= 400 || data.size() < 128) && attempt < 1) {
        const QString before = api_->ImageDomain();
        api_->RotateImageDomain();
        const QString next = api_->ImageDomain();
        if (!next.isEmpty() && next != before) {
            urls_[index] = WithImageDomain(urls_.at(index), next);
            emit Log(QStringLiteral("page %1 retry on %2 (http=%3 err=%4)")
                         .arg(index + 1)
                         .arg(next)
                         .arg(status)
                         .arg(netError ? errText : QStringLiteral("-")));
            const int gen = generation_;
            // 换域名后不要立刻重发：同一 CDN 连续被拒时，隔 250ms 成功率明显更高
            QTimer::singleShot(250, this, [this, index, gen, attempt] {
                if (gen != generation_ || !busy_)
                    return;
                Request(index, attempt + 1);
            });
            return;
        }
    }

    if (netError || status >= 400 || data.size() < 128) {
        running_.remove(index);
        ++settled_;
        if (error_.isEmpty())
            error_ = netError ? errText : QStringLiteral("http %1").arg(status);
        emit Log(QStringLiteral("page %1 failed: %2").arg(index + 1).arg(error_));
        Pump();
        MaybeFinished();
        return;
    }

    // 解码：Qt 6.12 不带 qwebp 插件，JM 的页图又几乎全是 webp。
    QImage img;
    if (!img.loadFromData(data) && JmWebp::IsWebp(data))
        img = JmWebp::Decode(data);
    if (img.isNull()) {
        running_.remove(index);
        ++settled_;
        if (error_.isEmpty())
            error_ = QStringLiteral("decode failed");
        emit Log(QStringLiteral("page %1 decode failed (%2 bytes)").arg(index + 1).arg(data.size()));
        Pump();
        MaybeFinished();
        return;
    }

    // pictureName 必须只用主干：服务端算的是 md5(epsId + "00005")，把扩展名一起
    // 喂进去会得到另一条分块数（实测第 5 页要 12 条却算成 6 条），还原出来的图
    // 就是"被切成几条、条与条之间错位"的样子。见 JmCrypto::ScrambleStem。
    const int num = JmCrypto::SegmentationNum(photoId_.toLongLong(), scrambleId_.toLongLong(),
                                              JmCrypto::ScrambleStem(names_.value(index)));
    if (num > 1)
        img = JmCrypto::Descramble(img, num);

    const QString path = OutPath(index);
    if (!img.save(path, "JPG", 92)) {
        running_.remove(index);
        ++settled_;
        if (error_.isEmpty())
            error_ = QStringLiteral("save failed");
        Pump();
        MaybeFinished();
        return;
    }

    results_[index] = path;
    running_.remove(index);
    ++done_;
    ++settled_;
    // 交付单页：阅读器据此立刻显示（当前页那张一到就出图，不等整章）。
    emit PageReady(index, total_, path);
    emit Progress(done_, total_);
    Pump();
    MaybeFinished();
}

void PageLoader::MaybeFinished()
{
    // 失败页既不在 done_ 也不在队列里，所以必须用 settled_ 判断收敛，
    // 否则"全部页都失败"时会永远等不到 Finished。
    if (settled_ < total_ || !running_.isEmpty() || !queue_.isEmpty())
        return;
    FinishSuccess();
}

void PageLoader::FinishSuccess()
{
    QStringList files;
    int missing = 0;
    for (int i = 0; i < results_.size(); ++i) {
        if (results_.at(i).isEmpty())
            ++missing;
        else
            files << results_.at(i);
    }
    busy_ = false;
    const QString label = label_;
    if (files.isEmpty()) {
        emit Log(QStringLiteral("page-load failed chapter=%1 (%2)").arg(chapterId_, error_));
        emit Finished(false, label, {}, error_.isEmpty() ? QStringLiteral("没有可用图片") : error_);
        return;
    }
    if (missing > 0)
        emit Log(QStringLiteral("page-load partial chapter=%1 missing=%2")
                     .arg(chapterId_)
                     .arg(missing));
    emit Log(QStringLiteral("page-load ok chapter=%1 pages=%2").arg(chapterId_).arg(files.size()));
    emit Finished(true, label, files,
                  missing > 0 ? QStringLiteral("缺 %1 页").arg(missing) : QString());
}

QString PageLoader::OutPath(int index) const
{
    return QDir(outDir_).filePath(QStringLiteral("%1.jpg").arg(index + 1, 5, 10, QLatin1Char('0')));
}

QString PageLoader::WithImageDomain(const QString &url, const QString &domain)
{
    QUrl u(url);
    if (!u.isValid() || domain.isEmpty())
        return url;
    u.setHost(domain);
    return u.toString();
}
