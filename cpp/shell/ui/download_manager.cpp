#include "download_manager.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include <cstdio>

#include "../jm_api.h"
#include "../jm_crypto.h"
#include "../jm_webp.h"
#include "app_paths.h"
#include "node_http.h"
#include "proxy_config.h"
#include "ui_prefs.h"

namespace {

constexpr int kMaxInFlight = 3;
constexpr qint64 kMaxImageBytes = 16LL * 1024 * 1024;
constexpr qint64 kMaxTaskBytes = 4LL * 1024 * 1024 * 1024;   // 单章上限，防目录错挂
const char *kMetaName = ".jm-download.json";

QString SafeName(const QString &s)
{
    QString out = s.trimmed();
    if (out.isEmpty())
        return QStringLiteral("untitled");
    for (QChar &c : out) {
        if (!c.isLetterOrNumber() && c != QLatin1Char('_') && c != QLatin1Char('-')
            && c != QLatin1Char(' ') && c != QLatin1Char('.'))
            c = QLatin1Char('_');
    }
    // 去掉首尾的点/空格：Windows 不允许 "." / ".." 结尾的目录名
    while (out.endsWith(QLatin1Char('.')) || out.endsWith(QLatin1Char(' ')))
        out.chop(1);
    if (out.isEmpty())
        out = QStringLiteral("untitled");
    if (out.size() > 60)
        out = out.left(60);
    return out;
}

QString StateToString(DownloadTask::State s)
{
    switch (s) {
    case DownloadTask::Queued:      return QStringLiteral("queued");
    case DownloadTask::Fetching:    return QStringLiteral("fetching");
    case DownloadTask::Running:     return QStringLiteral("running");
    case DownloadTask::Paused:      return QStringLiteral("paused");
    case DownloadTask::Done:        return QStringLiteral("done");
    case DownloadTask::Failed:      return QStringLiteral("failed");
    case DownloadTask::Interrupted: return QStringLiteral("interrupted");
    }
    return QStringLiteral("queued");
}

DownloadTask::State StateFromString(const QString &s)
{
    if (s == QLatin1String("queued"))      return DownloadTask::Queued;
    if (s == QLatin1String("fetching"))    return DownloadTask::Fetching;
    if (s == QLatin1String("running"))     return DownloadTask::Running;
    if (s == QLatin1String("paused"))      return DownloadTask::Paused;
    if (s == QLatin1String("done"))        return DownloadTask::Done;
    if (s == QLatin1String("failed"))      return DownloadTask::Failed;
    if (s == QLatin1String("interrupted")) return DownloadTask::Interrupted;
    return DownloadTask::Queued;
}

// 页图格式：优先 JPEG（体积小），但 Qt 的 JPEG 是插件——某些运行目录没有
// imageformats/qjpeg 时 save("JPG") 会静默失败（桌面裸构建实测）。这时退回
// PNG（QtGui 自带），保证"下载能落地"而不是整章报写入失败。
QString PageSuffix()
{
    static const QString s = [] {
        const QList<QByteArray> fmts = QImageWriter::supportedImageFormats();
        const bool jpeg = fmts.contains("jpg") || fmts.contains("jpeg");
        return jpeg ? QStringLiteral("jpg") : QStringLiteral("png");
    }();
    return s;
}

QString ReadMetaPath(const QString &dir)
{
    return QDir(dir).filePath(QString::fromLatin1(kMetaName));
}

// 目录里已有的页（续传）：00001.jpg ...
int CountExistingPages(const QString &dir)
{
    const QStringList names =
        QDir(dir).entryList({QStringLiteral("[0-9]*.jpg"), QStringLiteral("[0-9]*.png")},
                            QDir::Files);
    int n = 0;
    for (const QString &f : names) {
        if (QFileInfo(QDir(dir).filePath(f)).size() > 1024)
            ++n;
    }
    return n;
}

}  // namespace

QString DownloadTask::StateName() const
{
    switch (state) {
    case Queued:      return QStringLiteral("排队中");
    case Fetching:    return QStringLiteral("获取章节…");
    case Running:     return QStringLiteral("下载中");
    case Paused:      return QStringLiteral("已暂停");
    case Done:        return QStringLiteral("已完成");
    case Failed:      return QStringLiteral("失败");
    case Interrupted: return QStringLiteral("已中断");
    }
    return QStringLiteral("未知");
}

// ============================================================ 路径
QString DownloadManager::DefaultRoot()
{
    // 默认落在应用数据目录（桌面即 AppLocalData/JMComic/JMComic/download）。
    // 不放系统"下载"目录：Windows 的"受控文件夹访问"会把未签名程序的写入
    // 直接拒绝（本机实测 mkpath 返回 false，整章报"写入失败"），而
    // 应用数据目录一定可写；需要放到别处时在设置页改"保存目录"。
    return QDir(AppPaths::DataDir()).filePath(QStringLiteral("download"));
}

QString DownloadManager::Root()
{
    const QString pref = UiPrefs::GetString(QStringLiteral("downloadDir"));
    if (!pref.isEmpty() && AppPaths::IsWritable(pref))
        return QDir(pref).absolutePath();
    const QString def = DefaultRoot();
    QDir().mkpath(def);
    return QDir(def).absolutePath();
}

bool DownloadManager::SetRoot(const QString &dir)
{
    if (dir.isEmpty()) {
        UiPrefs::Set(QStringLiteral("downloadDir"), QString());
        return true;
    }
    if (!AppPaths::IsWritable(dir))
        return false;
    UiPrefs::Set(QStringLiteral("downloadDir"), QDir(dir).absolutePath());
    return true;
}

// ============================================================
DownloadManager::DownloadManager(JmApi *api, QObject *parent)
    : QObject(parent), api_(api)
{
    nam_ = new QNetworkAccessManager(this);
    connect(api_, &JmApi::ChapterReady, this,
            [this](bool ok, const JmChapterDetail &ch, const QString &error) {
                if (activeKey_.isEmpty() || chapterId_.isEmpty())
                    return;
                if (ch.id.isEmpty() || ch.id != chapterId_)
                    return;
                const int idx = IndexOf(activeKey_);
                if (idx < 0)
                    return;
                if (!ok || ch.images.isEmpty()) {
                    firstError_ = error.isEmpty() ? QStringLiteral("章节没有图片") : error;
                    FinishTask(false, firstError_);
                    return;
                }
                DownloadTask &t = tasks_[idx];
                photoId_ = ch.id;
                scrambleId_ = ch.scrambleId.isEmpty() ? QStringLiteral("220980") : ch.scrambleId;
                urls_ = ch.images;
                names_.clear();
                for (const QString &u : urls_) {
                    QString name = u.section(QLatin1Char('/'), -1);
                    const int dot = name.lastIndexOf(QLatin1Char('.'));
                    if (dot > 0)
                        name.truncate(dot);
                    names_ << name;
                }
                t.total = urls_.size();
                t.state = DownloadTask::Running;
                t.error.clear();
                pageListArrived_ = true;
                const bool made = QDir().mkpath(t.dir);
                emit Log(QStringLiteral("dl: dir=%1 mkpath=%2").arg(t.dir).arg(int(made)));
                SaveTask(t);
                Publish();
                QueuePages();
            });
}

int DownloadManager::IndexOf(const QString &key) const
{
    for (int i = 0; i < tasks_.size(); ++i) {
        if (tasks_.at(i).key == key)
            return i;
    }
    return -1;
}

DownloadTask DownloadManager::Task(const QString &key) const
{
    const int i = IndexOf(key);
    return i < 0 ? DownloadTask() : tasks_.at(i);
}

QString DownloadManager::Report() const
{
    int active = 0;
    int done = 0;
    int failed = 0;
    int interrupted = 0;
    for (const DownloadTask &t : tasks_) {
        if (t.Active())
            ++active;
        else if (t.state == DownloadTask::Done)
            ++done;
        else if (t.state == DownloadTask::Failed)
            ++failed;
        else if (t.state == DownloadTask::Interrupted)
            ++interrupted;
    }
    QString activeInfo = QStringLiteral("-");
    if (const int i = IndexOf(activeKey_); i >= 0) {
        const DownloadTask &t = tasks_.at(i);
        activeInfo = QStringLiteral("%1:%2/%3").arg(t.chapterId).arg(t.done).arg(t.total);
    }
    return QStringLiteral("dl-report: tasks=%1 active=%2 done=%3 failed=%4 interrupted=%5 "
                          "running=%6 root=%7")
        .arg(tasks_.size())
        .arg(active)
        .arg(done)
        .arg(failed)
        .arg(interrupted)
        .arg(activeInfo)
        .arg(Root());
}

// ============================================================ 持久化
void DownloadManager::SaveTask(const DownloadTask &t)
{
    if (t.dir.isEmpty())
        return;
    QDir().mkpath(t.dir);
    QJsonObject o;
    o.insert(QStringLiteral("v"), 1);
    o.insert(QStringLiteral("albumId"), t.albumId);
    o.insert(QStringLiteral("albumTitle"), t.albumTitle);
    o.insert(QStringLiteral("chapterId"), t.chapterId);
    o.insert(QStringLiteral("chapterName"), t.chapterName);
    o.insert(QStringLiteral("coverUrl"), t.coverUrl);
    o.insert(QStringLiteral("dir"), t.dir);
    o.insert(QStringLiteral("total"), t.total);
    o.insert(QStringLiteral("done"), t.done);
    o.insert(QStringLiteral("failed"), t.failed);
    o.insert(QStringLiteral("bytes"), double(t.bytes));
    o.insert(QStringLiteral("state"), StateToString(t.state));
    o.insert(QStringLiteral("error"), t.error);
    o.insert(QStringLiteral("updatedAt"), double(QDateTime::currentSecsSinceEpoch()));
    QFile f(ReadMetaPath(t.dir));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    f.close();
}

void DownloadManager::LoadPersisted()
{
    const QString root = Root();
    // kMetaName 以 "." 开头：QDir 默认过滤隐藏文件，不加 QDir::Hidden 会导致
    // 重启后一个任务都扫不到（真机实测：下载已完成 27/27 页，页面却显示空态）。
    QDirIterator it(root, {QString::fromLatin1(kMetaName)}, QDir::Files | QDir::Hidden,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString meta = it.next();
        QFile f(meta);
        if (!f.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        f.close();
        DownloadTask t;
        t.albumId = o.value(QStringLiteral("albumId")).toString();
        t.chapterId = o.value(QStringLiteral("chapterId")).toString();
        if (t.chapterId.isEmpty())
            continue;
        t.key = t.albumId + QLatin1Char('/') + t.chapterId;
        if (IndexOf(t.key) >= 0)
            continue;
        t.albumTitle = o.value(QStringLiteral("albumTitle")).toString();
        t.chapterName = o.value(QStringLiteral("chapterName")).toString();
        t.coverUrl = o.value(QStringLiteral("coverUrl")).toString();
        t.dir = QFileInfo(meta).absolutePath();
        t.total = o.value(QStringLiteral("total")).toInt();
        t.done = o.value(QStringLiteral("done")).toInt();
        t.failed = o.value(QStringLiteral("failed")).toInt();
        t.bytes = qint64(o.value(QStringLiteral("bytes")).toDouble());
        t.state = StateFromString(o.value(QStringLiteral("state")).toString());
        t.error = o.value(QStringLiteral("error")).toString();
        t.updatedAt = qint64(o.value(QStringLiteral("updatedAt")).toDouble());
        // 上次进程退出时还在跑的任务：如实标成"已中断"，等用户点继续。
        // 这里不能自动重跑：用户可能是主动杀进程/断电，静默重启会让人以为
        // 下载器自己乱动网络。
        if (t.state == DownloadTask::Fetching || t.state == DownloadTask::Running
            || t.state == DownloadTask::Queued) {
            t.state = DownloadTask::Interrupted;
            t.error = QStringLiteral("上次退出时未完成");
            SaveTask(t);
        }
        // 磁盘上已有多少页，以文件为准（防止 meta 与文件不一致）
        const int have = CountExistingPages(t.dir);
        if (have > t.done)
            t.done = have;
        if (t.total > 0 && t.done >= t.total && t.failed == 0)
            t.state = DownloadTask::Done;
        tasks_.push_back(t);
        emit Log(QStringLiteral("dl: restore %1 state=%2 %3/%4")
                     .arg(t.chapterName, t.StateName())
                     .arg(t.done)
                     .arg(t.total));
    }
    if (!tasks_.isEmpty())
        Publish();
}

// ============================================================ 入队
void DownloadManager::EnqueueChapters(const QString &albumId, const QString &albumTitle,
                                      const QStringList &chapterIds,
                                      const QStringList &chapterNames, const QString &coverUrl)
{
    if (albumId.isEmpty())
        return;
    int added = 0;
    for (int i = 0; i < chapterIds.size(); ++i) {
        const QString cid = chapterIds.at(i);
        if (cid.isEmpty())
            continue;
        const QString key = albumId + QLatin1Char('/') + cid;
        if (IndexOf(key) >= 0) {
            // 已存在：如果是失败/中断，重新入队（等价于点"重试"）
            const int idx = IndexOf(key);
            if (tasks_[idx].state == DownloadTask::Failed
                || tasks_[idx].state == DownloadTask::Interrupted) {
                tasks_[idx].state = DownloadTask::Queued;
                tasks_[idx].error.clear();
                ++added;
            }
            continue;
        }
        DownloadTask t;
        t.key = key;
        t.albumId = albumId;
        t.albumTitle = albumTitle.isEmpty() ? QStringLiteral("未命名作品") : albumTitle;
        t.chapterId = cid;
        t.chapterName = chapterNames.value(i).isEmpty()
                            ? QStringLiteral("第 %1 话").arg(i + 1)
                            : chapterNames.at(i);
        t.coverUrl = coverUrl;
        // 目录：<root>/<作品>/<章节>；同名章节（"第 1 话"重复）用章节 id 消歧
        const QString albumDir = QDir(Root()).filePath(SafeName(t.albumTitle));
        QString dir = QDir(albumDir).filePath(SafeName(t.chapterName));
        if (QFileInfo::exists(ReadMetaPath(dir))) {
            QFile f(ReadMetaPath(dir));
            if (f.open(QIODevice::ReadOnly)) {
                const QString other =
                    QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("chapterId")).toString();
                f.close();
                if (!other.isEmpty() && other != cid)
                    dir = QDir(albumDir).filePath(SafeName(t.chapterName) + QLatin1Char('-') + cid);
            }
        }
        t.dir = dir;
        t.state = DownloadTask::Queued;
        t.updatedAt = QDateTime::currentSecsSinceEpoch();
        tasks_.push_back(t);
        SaveTask(t);
        ++added;
        emit Log(QStringLiteral("dl: enqueue %1 %2 -> %3").arg(t.albumTitle, t.chapterName, t.dir));
    }
    if (added > 0)
        Publish();
    Pump();
}

// ============================================================ 队列推进
void DownloadManager::Publish()
{
    emit Changed();
}

void DownloadManager::Pump()
{
    if (!activeKey_.isEmpty())
        return;
    for (const DownloadTask &t : tasks_) {
        if (t.state == DownloadTask::Queued) {
            StartTask(t.key);
            return;
        }
    }
}

void DownloadManager::StartTask(const QString &key)
{
    const int idx = IndexOf(key);
    if (idx < 0)
        return;
    DownloadTask &t = tasks_[idx];
    ++generation_;
    activeKey_ = key;
    chapterId_ = t.chapterId;
    photoId_ = t.chapterId;
    scrambleId_.clear();
    urls_.clear();
    names_.clear();
    results_.clear();
    queue_.clear();
    running_.clear();
    settled_ = 0;
    bytesThisRun_ = 0;
    firstError_.clear();
    pageListArrived_ = false;
    t.state = DownloadTask::Fetching;
    t.error.clear();
    t.failed = 0;
    QDir().mkpath(t.dir);
    // 续传：目录里已有的页先算进 done（总数要等 /chapter 才知道）
    t.done = CountExistingPages(t.dir);
    SaveTask(t);
    Publish();
    emit Log(QStringLiteral("dl: start %1 (%2)").arg(t.chapterName, t.chapterId));
    api_->FetchChapter(t.chapterId);
}

void DownloadManager::QueuePages()
{
    const int idx = IndexOf(activeKey_);
    if (idx < 0)
        return;
    DownloadTask &t = tasks_[idx];
    const int total = urls_.size();
    results_.resize(total);
    settled_ = 0;
    int have = 0;
    for (int i = 0; i < total; ++i) {
        const QString path = OutPath(t, i);
        if (QFileInfo::exists(path) && QFileInfo(path).size() > 1024) {
            results_[i] = path;
            ++have;
            ++settled_;
            continue;
        }
        queue_.push_back(i);
    }
    t.total = total;
    t.done = qMax(t.done, have);
    SaveTask(t);
    Publish();
    if (queue_.isEmpty()) {
        FinishTask(true, QString());
        return;
    }
    const int gen = generation_;
    QTimer::singleShot(0, this, [this, gen] {
        if (gen != generation_)
            return;
        for (int i = 0; i < kMaxInFlight && !queue_.isEmpty(); ++i) {
            const int idxPage = queue_.takeFirst();
            running_.push_back(idxPage);
            RequestPage(idxPage, 0);
        }
    });
}

void DownloadManager::RequestPage(int index, int attempt)
{
    const int idx = IndexOf(activeKey_);
    if (idx < 0 || index < 0 || index >= urls_.size())
        return;
    const int gen = generation_;
    const RoutePlan plan = ProxyConfig::ResolveRoute(QUrl(urls_.at(index)), false);
    if (plan.viaNode) {
        // 节点线路：QNAM 无法指定 connect IP，必须交给 NodeHttp（与阅读器
        // 缓存同一套路径）。失败回调走同一个 HandlePage，重试/换 CDN 逻辑不分叉。
        if (!nodeHttp_) {
            nodeHttp_ = new NodeHttp(this);
            connect(nodeHttp_, &NodeHttp::Log, this,
                    [](const QString &line) { qWarning("dl-http: %s", qPrintable(line)); });
        }
        NodeHttpRequest hr;
        hr.url = plan.url;
        hr.connectIp = plan.connectIp;
        hr.sniHost = plan.sniHost;
        hr.headers.insert(QStringLiteral("User-Agent"),
                          QStringLiteral("JMComic/0.93 (Qt %1)")
                              .arg(QString::fromLatin1(qVersion())));
        hr.headers.insert(QStringLiteral("Referer"), QStringLiteral("https://www.cdnhjk.net/"));
        hr.headers.insert(QStringLiteral("Accept"),
                          QStringLiteral("image/avif,image/webp,image/*,*/*;q=0.8"));
        hr.timeoutMs = 20000;
        hr.maxBytes = kMaxImageBytes;
        nodeHttp_->Send(hr, [this, index, gen, attempt](const NodeHttpResponse &r) {
            if (gen != generation_ || activeKey_.isEmpty())
                return;
            HandlePage(index, !r.ok, r.status, r.body, r.error, attempt);
        });
        return;
    }
    QNetworkRequest req{plan.url};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("JMComic/0.93 (Qt %1)").arg(QString::fromLatin1(qVersion())));
    req.setRawHeader("Referer", "https://www.cdnhjk.net/");
    req.setRawHeader("Accept", "image/avif,image/webp,image/*,*/*;q=0.8");
    req.setTransferTimeout(20000);
    req.setMaximumRedirectsAllowed(5);

    QNetworkReply *reply = nam_->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, index, gen, attempt] {
        reply->deleteLater();
        if (gen != generation_ || activeKey_.isEmpty())
            return;
        const bool netError = reply->error() != QNetworkReply::NoError;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray data = netError ? QByteArray() : reply->readAll();
        HandlePage(index, netError, status, data, reply->errorString(), attempt);
    });
}

void DownloadManager::HandlePage(int index, bool netError, int status, const QByteArray &data,
                                 const QString &errText, int attempt)
{
    const int idx = IndexOf(activeKey_);
    if (idx < 0)
        return;

    // 网络错误或 404 都值得换 CDN 重试（该 CDN 没这份文件，另一个镜像往往有）。
    // 最多 3 次：首试 + 两次轮换。真机上单页偶发超时很常见，只重试一次会让
    // 整章因为一页被判失败。
    if ((netError || status >= 400 || data.size() < 128) && attempt < 2) {
        const QString before = api_->ImageDomain();
        api_->RotateImageDomain();
        const QString next = api_->ImageDomain();
        if (!next.isEmpty() && next != before) {
            urls_[index] = WithImageDomain(urls_.at(index), next);
            emit Log(QStringLiteral("dl: page %1 retry on %2 (http=%3)").arg(index + 1).arg(next).arg(status));
            const int gen = generation_;
            QTimer::singleShot(250, this, [this, index, gen, attempt] {
                if (gen != generation_ || activeKey_.isEmpty())
                    return;
                RequestPage(index, attempt + 1);
            });
            return;
        }
    }

    auto settle = [this, index](bool ok, const QString &path, const QString &err) {
        const int i = IndexOf(activeKey_);
        if (i < 0)
            return;
        DownloadTask &task = tasks_[i];
        running_.removeAll(index);
        ++settled_;
        if (ok) {
            ++task.done;
            results_[index] = path;
            task.bytes += QFileInfo(path).size();
            bytesThisRun_ += QFileInfo(path).size();
            if (task.bytes > kMaxTaskBytes) {
                task.error = QStringLiteral("单章超过 4GB，已停止");
                FinishTask(false, task.error);
                return;
            }
        } else {
            ++task.failed;
            if (firstError_.isEmpty())
                firstError_ = err;
        }
        // 每 5 页落一次盘：太频繁会把小文件写放大拖慢下载，太稀疏则强杀丢进度
        if (settled_ % 5 == 0 || settled_ == task.total)
            SaveTask(task);
        Publish();
        if (settled_ >= task.total) {
            const bool okAll = task.failed == 0;
            FinishTask(okAll, okAll ? QString()
                                    : QStringLiteral("%1 页失败：%2").arg(task.failed).arg(firstError_));
            return;
        }
        // 继续补位
        if (!queue_.isEmpty()) {
            const int next = queue_.takeFirst();
            running_.push_back(next);
            RequestPage(next, 0);
        }
    };

    if (netError || status >= 400 || data.size() < 128) {
        settle(false, QString(), netError ? errText : QStringLiteral("http %1").arg(status));
        return;
    }

    QImage img;
    if (!img.loadFromData(data) && JmWebp::IsWebp(data))
        img = JmWebp::Decode(data);
    if (img.isNull()) {
        settle(false, QString(), QStringLiteral("图片解码失败"));
        return;
    }
    // pictureName 必须只用主干（服务端算的是 md5(epsId + "00005")）
    const int num = JmCrypto::SegmentationNum(photoId_.toLongLong(), scrambleId_.toLongLong(),
                                              JmCrypto::ScrambleStem(names_.value(index)));
    if (num > 1)
        img = JmCrypto::Descramble(img, num);

    const DownloadTask snap = tasks_.at(idx);
    const QString path = OutPath(snap, index);
    QDir().mkpath(snap.dir);
    const bool saved = PageSuffix() == QLatin1String("jpg") ? img.save(path, "JPG", 92)
                                                            : img.save(path, "PNG");
    if (!saved) {
        settle(false, QString(),
               QStringLiteral("写入失败：%1（目录%2，格式 %3）")
                   .arg(path, QDir(snap.dir).exists() ? QStringLiteral("存在") : QStringLiteral("不存在"),
                        PageSuffix()));
        return;
    }
    settle(true, path, QString());
}

void DownloadManager::FinishTask(bool ok, const QString &message)
{
    const int idx = IndexOf(activeKey_);
    if (idx < 0) {
        activeKey_.clear();
        return;
    }
    DownloadTask &t = tasks_[idx];
    const QString key = t.key;
    const QString name = t.chapterName;
    if (ok) {
        t.state = DownloadTask::Done;
        t.error.clear();
        t.done = t.total > 0 ? t.total : t.done;
    } else {
        t.state = DownloadTask::Failed;
        t.error = message.isEmpty() ? QStringLiteral("下载失败") : message;
    }
    t.updatedAt = QDateTime::currentSecsSinceEpoch();
    SaveTask(t);
    activeKey_.clear();
    chapterId_.clear();
    pageListArrived_ = false;
    queue_.clear();
    running_.clear();
    ++generation_;
    emit Log(QStringLiteral("dl: %1 %2 (%3/%4) bytes=%5")
                 .arg(ok ? QStringLiteral("done") : QStringLiteral("failed"), name)
                 .arg(t.done)
                 .arg(t.total)
                 .arg(t.bytes));
    Publish();
    emit TaskFinished(key, ok, ok ? QStringLiteral("%1 下载完成（%2 页）").arg(name).arg(t.total)
                                  : message);
    QTimer::singleShot(0, this, [this] { Pump(); });
}

void DownloadManager::StopActive(const QString &reason, DownloadTask::State state)
{
    if (activeKey_.isEmpty())
        return;
    ++generation_;
    const int idx = IndexOf(activeKey_);
    if (idx >= 0) {
        DownloadTask &t = tasks_[idx];
        t.state = state;
        t.error = reason;
        t.failed = 0;
        t.updatedAt = QDateTime::currentSecsSinceEpoch();
        SaveTask(t);
    }
    activeKey_.clear();
    chapterId_.clear();
    pageListArrived_ = false;
    queue_.clear();
    running_.clear();
    Publish();
}

// ============================================================ 单任务操作
void DownloadManager::Pause(const QString &key)
{
    if (key == activeKey_) {
        StopActive(QStringLiteral("已暂停"), DownloadTask::Paused);
        return;
    }
    const int idx = IndexOf(key);
    if (idx < 0)
        return;
    if (tasks_[idx].state != DownloadTask::Queued)
        return;
    tasks_[idx].state = DownloadTask::Paused;
    SaveTask(tasks_[idx]);
    Publish();
}

void DownloadManager::Resume(const QString &key)
{
    const int idx = IndexOf(key);
    if (idx < 0)
        return;
    DownloadTask &t = tasks_[idx];
    if (t.state == DownloadTask::Done)
        return;
    t.state = DownloadTask::Queued;
    t.error.clear();
    t.failed = 0;
    SaveTask(t);
    Publish();
    Pump();
}

void DownloadManager::Retry(const QString &key)
{
    Resume(key);
}

void DownloadManager::Remove(const QString &key, bool deleteFiles)
{
    if (key == activeKey_)
        StopActive(QStringLiteral("已取消"), DownloadTask::Paused);
    const int idx = IndexOf(key);
    if (idx < 0)
        return;
    const QString dir = tasks_[idx].dir;
    tasks_.removeAt(idx);
    QFile::remove(ReadMetaPath(dir));
    if (deleteFiles) {
        QDir d(dir);
        // 只删我们自己写的文件（页图 + meta），不递归删用户目录
        const QStringList pages =
            d.entryList({QStringLiteral("[0-9]*.jpg"), QStringLiteral("[0-9]*.png")}, QDir::Files);
        for (const QString &p : pages)
            QFile::remove(d.filePath(p));
        QDir().rmdir(dir);   // 空目录才删得掉
        const QString parent = QFileInfo(dir).absolutePath();
        QDir().rmdir(parent);
    }
    Publish();
    Pump();
}

void DownloadManager::PauseAll()
{
    if (!activeKey_.isEmpty())
        StopActive(QStringLiteral("全部暂停"), DownloadTask::Paused);
    for (DownloadTask &t : tasks_) {
        if (t.state == DownloadTask::Queued) {
            t.state = DownloadTask::Paused;
            SaveTask(t);
        }
    }
    Publish();
}

void DownloadManager::ResumeAll()
{
    for (DownloadTask &t : tasks_) {
        if (t.state == DownloadTask::Paused || t.state == DownloadTask::Failed
            || t.state == DownloadTask::Interrupted) {
            t.state = DownloadTask::Queued;
            t.error.clear();
            t.failed = 0;
            SaveTask(t);
        }
    }
    Publish();
    Pump();
}

void DownloadManager::Shutdown()
{
    if (!activeKey_.isEmpty())
        StopActive(QStringLiteral("退出时未完成"), DownloadTask::Interrupted);
    for (DownloadTask &t : tasks_)
        SaveTask(t);
}

bool DownloadManager::WaitIdle(int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(120);
    QObject::connect(&poll, &QTimer::timeout, &loop, [this, &loop, &timer, timeoutMs] {
        bool busy = !activeKey_.isEmpty();
        if (!busy) {
            for (const DownloadTask &t : tasks_) {
                if (t.state == DownloadTask::Queued || t.Active())
                    busy = true;
            }
        }
        if (!busy || timer.elapsed() > timeoutMs)
            loop.quit();
    });
    poll.start();
    loop.exec();
    bool busy = !activeKey_.isEmpty();
    for (const DownloadTask &t : tasks_) {
        if (t.state == DownloadTask::Queued || t.Active())
            busy = true;
    }
    return !busy;
}

QString DownloadManager::OutPath(const DownloadTask &t, int index) const
{
    return QDir(t.dir).filePath(
        QStringLiteral("%1.%2").arg(index + 1, 5, 10, QLatin1Char('0')).arg(PageSuffix()));
}

QString DownloadManager::WithImageDomain(const QString &url, const QString &domain)
{
    QUrl u(url);
    if (!u.isValid() || domain.isEmpty())
        return url;
    u.setHost(domain);
    return u.toString();
}
