#include "browse_page.h"

#include "jm_api.h"
#include "jm_crypto.h"
#include "jm_webp.h"
#include "net_client.h"

#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <QVector>

#include <android/log.h>

#define LOGB(...) __android_log_print(ANDROID_LOG_INFO, "C1Browse", __VA_ARGS__)

BrowsePage::BrowsePage(NetClient *net, QWidget *parent)
    : QWidget(parent), net_(net)
{
    api_ = new JmApi(net_, this);
    BuildUi();

    connect(api_, &JmApi::Log, this, [this](const QString &line) {
        LOGB("%s", qPrintable(line));
        emit StatusMessage(line);
    });
    connect(api_, &JmApi::SearchReady, this,
            [this](bool ok, const QVector<JmComicItem> &items, int total, int page, const QString &err) {
                if (!ok) {
                    Fail(QStringLiteral("search"), err);
                    return;
                }
                ShowResults(items, total);
                if (selftest_) {
                    if (items.isEmpty()) {
                        Fail(QStringLiteral("search"), QStringLiteral("0 hits"));
                        return;
                    }
                    OpenAlbum(items.first().id);
                }
                Q_UNUSED(page)
            });
    connect(api_, &JmApi::AlbumReady, this, [this](bool ok, const JmAlbum &album, const QString &err) {
        if (!ok) {
            Fail(QStringLiteral("album"), err);
            return;
        }
        ShowAlbum(album);
        if (selftest_ && !album.chapters.isEmpty()) {
            api_->FetchChapter(album.chapters.first().id);
        } else if (selftest_) {
            // 单章本子：album 没有 series，直接用 album.id 当章节 id（JM 的约定）
            api_->FetchChapter(album.id);
        }
    });
    connect(api_, &JmApi::ChapterReady, this,
            [this](bool ok, const JmChapterDetail &chapter, const QString &err) {
                if (!ok) {
                    Fail(QStringLiteral("chapter"), err);
                    return;
                }
                ShowChapter(chapter);
                if (selftest_ && !chapter.images.isEmpty())
                    DownloadNext();
                else if (selftest_)
                    Fail(QStringLiteral("chapter"), QStringLiteral("0 pages"));
            });
}

void BrowsePage::BuildUi()
{
    auto *root = new QVBoxLayout(this);

    inner_ = new QStackedWidget(this);
    root->addWidget(inner_, 1);

    // ---- 搜索页 ----
    auto *searchPage = new QWidget(inner_);
    auto *sl = new QVBoxLayout(searchPage);
    auto *bar = new QHBoxLayout();
    keyword_ = new QLineEdit(searchPage);
    keyword_->setPlaceholderText(QStringLiteral("搜索关键字，或直接输入本子 id"));
    goBtn_ = new QPushButton(QStringLiteral("Search"), searchPage);
    bar->addWidget(keyword_, 1);
    bar->addWidget(goBtn_);
    sl->addLayout(bar);
    results_ = new QListWidget(searchPage);
    sl->addWidget(results_, 1);
    inner_->addWidget(searchPage);

    // ---- 详情页 ----
    auto *detailPage = new QWidget(inner_);
    auto *dl = new QVBoxLayout(detailPage);
    detailTitle_ = new QLabel(detailPage);
    detailTitle_->setWordWrap(true);
    detailMeta_ = new QLabel(detailPage);
    detailMeta_->setWordWrap(true);
    dl->addWidget(detailTitle_);
    dl->addWidget(detailMeta_);
    chapters_ = new QListWidget(detailPage);
    dl->addWidget(chapters_, 1);
    auto *dbar = new QHBoxLayout();
    backBtn_ = new QPushButton(QStringLiteral("Back"), detailPage);
    readBtn_ = new QPushButton(QStringLiteral("Read"), detailPage);
    dbar->addWidget(backBtn_);
    dbar->addWidget(readBtn_);
    dl->addLayout(dbar);
    inner_->addWidget(detailPage);

    status_ = new QLabel(this);
    status_->setWordWrap(true);
    root->addWidget(status_);

    connect(goBtn_, &QPushButton::clicked, this, [this] { OnSearchClicked(); });
    connect(keyword_, &QLineEdit::returnPressed, this, [this] { OnSearchClicked(); });
    connect(results_, &QListWidget::itemActivated, this, [this](QListWidgetItem *item) {
        if (item)
            OpenAlbum(item->data(Qt::UserRole).toString());
    });
    connect(results_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        // 触屏上 itemClicked 才是可靠的；itemActivated 在 Android 上经常不触发。
        if (item)
            OpenAlbum(item->data(Qt::UserRole).toString());
    });
    connect(chapters_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        if (!item)
            return;
        api_->FetchChapter(item->data(Qt::UserRole).toString());
    });
    connect(readBtn_, &QPushButton::clicked, this, [this] {
        if (!chapters_->currentItem() && chapters_->count() > 0)
            chapters_->setCurrentRow(0);
        if (chapters_->currentItem())
            api_->FetchChapter(chapters_->currentItem()->data(Qt::UserRole).toString());
    });
    connect(backBtn_, &QPushButton::clicked, this, [this] { inner_->setCurrentIndex(0); });
}

void BrowsePage::OnSearchClicked()
{
    const QString text = keyword_->text().trimmed();
    if (text.isEmpty())
        return;
    selftest_ = false;
    status_->setText(QStringLiteral("searching %1 ...").arg(text));
    LOGB("search requested: %s", qPrintable(text));
    if (text.at(0).isDigit()) {
        OpenAlbum(text);
        return;
    }
    api_->FetchSetting();
    api_->Search(text, 1);
}

void BrowsePage::OpenAlbum(const QString &albumId)
{
    if (albumId.isEmpty())
        return;
    status_->setText(QStringLiteral("album %1 ...").arg(albumId));
    api_->FetchAlbum(albumId);
}

void BrowsePage::ShowResults(const QVector<JmComicItem> &items, int total)
{
    results_->clear();
    for (const JmComicItem &item : items) {
        auto *row = new QListWidgetItem(
            QStringLiteral("%1  [%2]  %3").arg(item.id, item.author, item.name));
        row->setData(Qt::UserRole, item.id);
        results_->addItem(row);
    }
    const QString line = QStringLiteral("search: %1 results (total %2)").arg(items.size()).arg(total);
    status_->setText(line);
    LOGB("%s", qPrintable(line));
}

void BrowsePage::ShowAlbum(const JmAlbum &album)
{
    album_ = album;
    detailTitle_->setText(QStringLiteral("[%1] %2").arg(album.id, album.name));
    QString meta = album.author;
    if (!album.tags.isEmpty())
        meta += QStringLiteral("  |  ") + album.tags.join(QStringLiteral(" "));
    if (!album.description.trimmed().isEmpty())
        meta += QStringLiteral("\n") + album.description.left(600);
    detailMeta_->setText(meta);

    chapters_->clear();
    if (album.chapters.isEmpty()) {
        // 单章本子：album 自身就是唯一章节
        auto *row = new QListWidgetItem(QStringLiteral("(single) %1").arg(album.name));
        row->setData(Qt::UserRole, album.id);
        chapters_->addItem(row);
    } else {
        for (const JmChapter &ch : album.chapters) {
            auto *row = new QListWidgetItem(
                QStringLiteral("%1  %2").arg(ch.sort.isEmpty() ? QStringLiteral("-") : ch.sort, ch.name));
            row->setData(Qt::UserRole, ch.id);
            chapters_->addItem(row);
        }
    }
    inner_->setCurrentIndex(1);
    LOGB("album shown: %s chapters=%d", qPrintable(album.name), int(chapters_->count()));
}

void BrowsePage::ShowChapter(const JmChapterDetail &chapter)
{
    chapter_ = chapter;
    pendingUrls_ = chapter.images;
    savedFiles_.clear();
    downloadIndex_ = 0;
    imageRetried_ = false;
    cacheDir_ = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
                + QStringLiteral("/online/") + chapter.id;
    QDir().mkpath(cacheDir_);
    const QString line = QStringLiteral("chapter %1 pages=%2 scramble=%3")
                             .arg(chapter.id).arg(chapter.images.size()).arg(chapter.scrambleId);
    status_->setText(line);
    LOGB("%s", qPrintable(line));
    if (!selftest_)
        DownloadNext();  // 手动路径：点章节就开下
}

void BrowsePage::DownloadNext()
{
    if (downloadIndex_ >= pendingUrls_.size() || downloadIndex_ >= pageLimit_) {
        const QString summary = QStringLiteral("online ready: %1 pages (scramble=%2)")
                                    .arg(savedFiles_.size()).arg(chapter_.scrambleId);
        status_->setText(summary);
        lastSummary_ = summary;
        LOGB("%s", qPrintable(summary));
        if (!savedFiles_.isEmpty())
            emit OpenReaderRequested(savedFiles_, chapter_.name);
        if (selftest_)
            emit SelfTestDone(summary);
        return;
    }

    const QString url = pendingUrls_.at(downloadIndex_);
    NetRequest req;
    req.url = QUrl(url);
    req.timeoutMs = 40000;
    req.retries = 1;
    req.maxBytes = 12 * 1024 * 1024;
    req.headers.insert(QStringLiteral("User-Agent"),
                       QStringLiteral("Mozilla/5.0 (Linux; Android 9) AppleWebKit/537.36 "
                                      "(KHTML, like Gecko) Chrome/91.0.4472.114 Safari/537.36"));
    req.headers.insert(QStringLiteral("Referer"),
                       QStringLiteral("https://%1/").arg(api_->Domain()));

    // 一次性 handler：NetClient 只会发一次 Finished，这里用 lambda 连接再断开。
    auto conn = std::make_shared<QMetaObject::Connection>();
    *conn = connect(net_, &NetClient::Finished, this,
                    [this, conn, url](const NetResult &r) {
                        disconnect(*conn);
                        if (!r.ok || r.httpError || r.body.isEmpty()) {
                            const QString err = QStringLiteral("image %1 failed: %2")
                                                    .arg(url, r.errorText.isEmpty() ? r.errorCode
                                                                                    : r.errorText);
                            LOGB("%s", qPrintable(err));
                            status_->setText(err);
                            // CDN 域名在部分网络会整段不可达（实测 jmapiproxy1 超时、
                            // jmapiproxy2 正常）：换一次域重试当前这张，再失败才跳过。
                            if (!imageRetried_ && !pendingUrls_.isEmpty()) {
                                imageRetried_ = true;
                                const QString oldHost = QUrl(pendingUrls_.first()).host();
                                api_->RotateImageDomain();
                                const QString newHost = api_->ImageDomain();
                                for (QString &u : pendingUrls_)
                                    u.replace(QStringLiteral("https://%1/").arg(oldHost),
                                              QStringLiteral("https://%1/").arg(newHost));
                                DownloadNext();
                                return;
                            }
                            // 单张失败不终止整章：跳过继续，读到的页数如实上报。
                            ++downloadIndex_;
                            DownloadNext();
                            return;
                        }
                        QImage img;
                        if (JmWebp::IsWebp(r.body))
                            img = JmWebp::Decode(r.body);   // Qt 6.12 无 qwebp 插件，必须自解码
                        if (img.isNull())
                            img.loadFromData(r.body);
                        if (img.isNull()) {
                            LOGB("image decode failed (%lld bytes) %s",
                                 static_cast<long long>(r.body.size()), qPrintable(url));
                            ++downloadIndex_;
                            DownloadNext();
                            return;
                        }
                        // 分块数必须用服务端给的真实文件名主干（"00005.webp" -> "00005"）：
                        // 按序号猜名字在"编号有洞"的章节上会算错（00027 下架后真正是 00028），
                        // 而带扩展名会让 md5 落到另一条分块数上，两者都会把图切错。
                        const int num = JmCrypto::SegmentationNum(
                            chapter_.id.toLongLong(), chapter_.scrambleId.toLongLong(),
                            JmCrypto::ScrambleStem(url));
                        const QImage fixed = JmCrypto::Descramble(img, num);
                        const QString path = QStringLiteral("%1/%2.png")
                                                 .arg(cacheDir_)
                                                 .arg(downloadIndex_ + 1, 3, 10, QLatin1Char('0'));
                        if (!fixed.save(path, "PNG")) {
                            LOGB("save failed: %s", qPrintable(path));
                        } else {
                            savedFiles_ << path;
                        }
                        LOGB("image %d/%d num=%d saved=%s", downloadIndex_ + 1,
                             int(pendingUrls_.size()), num, qPrintable(path));
                        ++downloadIndex_;
                        DownloadNext();
                    });
    if (!net_->Send(req)) {
        disconnect(*conn);
        LOGB("image send rejected: %s", qPrintable(net_->GetLastError()));
        ++downloadIndex_;
        DownloadNext();
    }
}

void BrowsePage::Fail(const QString &stage, const QString &error)
{
    const QString line = QStringLiteral("%1 failed: %2").arg(stage, error);
    status_->setText(line);
    lastSummary_ = line;
    LOGB("%s", qPrintable(line));
    emit StatusMessage(line);
    if (selftest_)
        emit SelfTestDone(line);
}

void BrowsePage::RunSelfTest(const QString &keyword, int pages)
{
    selftest_ = true;
    pageLimit_ = pages > 0 ? pages : 3;
    LOGB("browse selftest: keyword=%s pages=%d", qPrintable(keyword), pageLimit_);
    status_->setText(QStringLiteral("selftest: search %1").arg(keyword));
    api_->FetchSetting();
    api_->Search(keyword, 1);
}
