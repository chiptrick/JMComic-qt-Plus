#include "ui_pages.h"

#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include "app_paths.h"
#include "download_manager.h"
#include "motion.h"
#include "theme.h"
#include "touch.h"
#include "ui_kit.h"

namespace {

QWidget *MakeHeader(const QString &title, const QString &sub, QWidget *parent, QHBoxLayout **out)
{
    auto *host = new QWidget(parent);
    auto *lay = new QHBoxLayout(host);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(12);
    auto *texts = new QVBoxLayout();
    texts->setSpacing(2);
    auto *t = new QLabel(title, host);
    t->setObjectName(QStringLiteral("PageTitle"));
    texts->addWidget(t);
    if (!sub.isEmpty()) {
        auto *s = new QLabel(sub, host);
        s->setObjectName(QStringLiteral("PageSub"));
        texts->addWidget(s);
    }
    lay->addLayout(texts);
    lay->addStretch(1);
    if (out)
        *out = lay;
    return host;
}

QScrollArea *WrapScroll(QWidget *content, QWidget *parent)
{
    auto *area = new QScrollArea(parent);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setWidget(content);
    // 所有页面级滚动容器统一禁横向平移（此前的副本漏了这一步）
    Touch::LockHorizontalPan(area);
    return area;
}

QWidget *PageBody(QWidget *parent, int margin = 28)
{
    auto *w = new QWidget(parent);
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(margin, margin, margin, margin);
    lay->setSpacing(16);
    return w;
}

QVBoxLayout *BodyLayout(QWidget *body)
{
    return qobject_cast<QVBoxLayout *>(body->layout());
}

void ClearLayout(QVBoxLayout *lay)
{
    while (QLayoutItem *item = lay->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }
}

const UiTokens &T() { return UiTheme::I().Tokens(); }

}  // namespace

// ============================================================ 下载
namespace {

QString HumanSize(qint64 n)
{
    if (n <= 0)
        return QStringLiteral("-");
    if (n >= qint64(1024) * 1024 * 1024)
        return QStringLiteral("%1 GB").arg(double(n) / (1024.0 * 1024 * 1024), 0, 'f', 2);
    if (n >= qint64(1024) * 1024)
        return QStringLiteral("%1 MB").arg(double(n) / (1024.0 * 1024), 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(double(n) / 1024.0, 0, 'f', 0);
}

QPushButton *RowButton(const QString &text, const QString &kind, QWidget *parent)
{
    auto *b = new QPushButton(text, parent);
    b->setObjectName(kind);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(26);
    b->setProperty("hitGrow", false);
    return b;
}

}  // namespace

DownloadPage::DownloadPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("下载"), QStringLiteral("章节下载队列（离开本页也会继续）"),
                            body, &header);
    lay->addWidget(head);
    auto *pauseAll = new QPushButton(QStringLiteral("全部暂停"), body);
    pauseAll->setObjectName(QStringLiteral("Ghost"));
    pauseAll->setCursor(Qt::PointingHandCursor);
    header->addWidget(pauseAll);
    auto *openDir = new QPushButton(QStringLiteral("打开目录"), body);
    openDir->setObjectName(QStringLiteral("Ghost"));
    openDir->setCursor(Qt::PointingHandCursor);
    header->addWidget(openDir);

    auto *tabs = new QWidget(body);
    auto *tl = new QHBoxLayout(tabs);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(8);
    const QStringList names = {QStringLiteral("全部"), QStringLiteral("进行中"),
                               QStringLiteral("已完成"), QStringLiteral("失败")};
    for (int i = 0; i < names.size(); ++i) {
        auto *chip = new Chip(names.at(i), true, tabs);
        chip->setChecked(i == 0);
        chip->setProperty("dlFilter", names.at(i));
        tl->addWidget(chip);
        connect(chip, &QPushButton::clicked, this, [this, tabs, chip, names, i] {
            for (auto *b : tabs->findChildren<Chip *>())
                b->setChecked(b == chip);
            filter_ = i == 0 ? QString() : names.at(i);
            Rebuild();
        });
    }
    tl->addStretch(1);
    lay->addWidget(tabs);

    summary_ = new QLabel(QStringLiteral("暂无下载任务"), body);
    summary_->setObjectName(QStringLiteral("SectionHint"));
    summary_->setWordWrap(true);
    summary_->setMinimumWidth(0);
    lay->addWidget(summary_);

    auto *listHost = new QWidget(body);
    list_ = new QVBoxLayout(listHost);
    list_->setContentsMargins(0, 0, 0, 0);
    list_->setSpacing(10);
    lay->addWidget(listHost);
    lay->addStretch(1);

    root->addWidget(WrapScroll(body, this));

    // 进度是高频信号：合并到 200ms 一次重建，避免每页一次布局
    refresh_ = new QTimer(this);
    refresh_->setSingleShot(true);
    refresh_->setInterval(200);
    connect(refresh_, &QTimer::timeout, this, &DownloadPage::Rebuild);

    connect(pauseAll, &QPushButton::clicked, this, [this] {
        if (!mgr_) {
            emit Toast(QStringLiteral("下载队列尚未初始化"));
            return;
        }
        // 有在跑/排队的就全部暂停，否则全部继续（同一个按钮两种语义，
        // 按当前状态决定，避免用户在两个按钮之间找）
        bool anyRunning = false;
        for (const DownloadTask &t : mgr_->Tasks()) {
            if (t.Active() || t.state == DownloadTask::Queued)
                anyRunning = true;
        }
        if (anyRunning) {
            mgr_->PauseAll();
            emit Toast(QStringLiteral("已暂停全部下载"));
        } else {
            mgr_->ResumeAll();
            emit Toast(QStringLiteral("已继续全部下载"));
        }
        Rebuild();
    });
    connect(openDir, &QPushButton::clicked, this, [this] {
        const QString dir = DownloadManager::Root();
        emit OpenDownloaded(dir);
    });
}

void DownloadPage::SetManager(DownloadManager *m)
{
    mgr_ = m;
    if (!mgr_)
        return;
    connect(mgr_, &DownloadManager::Changed, this, [this] {
        if (refresh_ && !refresh_->isActive())
            refresh_->start();
    });
    connect(mgr_, &DownloadManager::TaskFinished, this,
            [this](const QString &, bool ok, const QString &message) {
                emit Toast(message.isEmpty() ? (ok ? QStringLiteral("下载完成")
                                                    : QStringLiteral("下载失败"))
                                             : message);
            });
    Rebuild();
}

void DownloadPage::OnEnter() { Rebuild(); }

void DownloadPage::Rebuild()
{
    if (!list_)
        return;
    ClearLayout(list_);
    if (!mgr_) {
        if (summary_)
            summary_->setText(QStringLiteral("下载队列尚未初始化"));
        return;
    }
    const QVector<DownloadTask> tasks = mgr_->Tasks();
    int running = 0;
    int done = 0;
    int failed = 0;
    qint64 bytes = 0;
    for (const DownloadTask &t : tasks) {
        bytes += t.bytes;
        if (t.Active() || t.state == DownloadTask::Queued)
            ++running;
        else if (t.state == DownloadTask::Done)
            ++done;
        else if (t.state == DownloadTask::Failed || t.state == DownloadTask::Interrupted)
            ++failed;
    }
    if (summary_) {
        if (tasks.isEmpty()) {
            summary_->setText(QStringLiteral("暂无下载任务 · 保存到 %1").arg(DownloadManager::Root()));
        } else {
            summary_->setText(QStringLiteral("共 %1 个任务 · 进行中 %2 · 已完成 %3 · 失败/中断 %4 · "
                                             "已占 %5 · %6")
                                  .arg(tasks.size())
                                  .arg(running)
                                  .arg(done)
                                  .arg(failed)
                                  .arg(HumanSize(bytes), DownloadManager::Root()));
        }
    }

    auto matches = [this](const DownloadTask &t) {
        if (filter_.isEmpty())
            return true;
        if (filter_ == QStringLiteral("进行中"))
            return t.Active() || t.state == DownloadTask::Queued || t.state == DownloadTask::Paused;
        if (filter_ == QStringLiteral("已完成"))
            return t.state == DownloadTask::Done;
        return t.state == DownloadTask::Failed || t.state == DownloadTask::Interrupted;
    };

    auto *host = list_->parentWidget();
    if (!host)
        return;
    int shown = 0;
    for (const DownloadTask &t : tasks) {
        if (!matches(t))
            continue;
        ++shown;
        auto *row = new QWidget(host);
        row->setObjectName(QStringLiteral("ListRow"));
        auto *rl = new QVBoxLayout(row);
        rl->setContentsMargins(14, 10, 14, 10);
        rl->setSpacing(6);

        auto *line1 = new QHBoxLayout();
        line1->setSpacing(10);
        auto *name = new ElideLabel(t.chapterName, row);
        name->setObjectName(QStringLiteral("RowTitle"));
        name->setMinimumWidth(0);
        line1->addWidget(name, 1);
        auto *state = new QLabel(t.StateName(), row);
        const bool live = t.Active() || t.state == DownloadTask::Queued;
        state->setObjectName(live ? QStringLiteral("Tag") : QStringLiteral("TagDim"));
        line1->addWidget(state);

        // 每行只放两个动作：按状态给出"最该点的那个" + 移除（避免四五个按钮挤成一行）
        if (t.state == DownloadTask::Done) {
            auto *open = RowButton(QStringLiteral("打开"), QStringLiteral("Ghost"), row);
            connect(open, &QPushButton::clicked, this, [this, t] { emit OpenDownloaded(t.dir); });
            line1->addWidget(open);
        } else if (t.Active() || t.state == DownloadTask::Queued) {
            auto *pause = RowButton(QStringLiteral("暂停"), QStringLiteral("Ghost"), row);
            connect(pause, &QPushButton::clicked, this, [this, t] {
                if (mgr_)
                    mgr_->Pause(t.key);
                Rebuild();
            });
            line1->addWidget(pause);
        } else if (t.state == DownloadTask::Paused || t.state == DownloadTask::Interrupted
                   || t.state == DownloadTask::Failed) {
            auto *go = RowButton(t.state == DownloadTask::Failed ? QStringLiteral("重试")
                                                                 : QStringLiteral("继续"),
                                 QStringLiteral("Primary"), row);
            connect(go, &QPushButton::clicked, this, [this, t] {
                if (!mgr_)
                    return;
                if (t.state == DownloadTask::Failed)
                    mgr_->Retry(t.key);
                else
                    mgr_->Resume(t.key);
                Rebuild();
            });
            line1->addWidget(go);
        }
        auto *del = RowButton(QStringLiteral("移除"), QStringLiteral("Ghost"), row);
        connect(del, &QPushButton::clicked, this, [this, t] {
            if (!mgr_)
                return;
            const bool keepFiles = t.state != DownloadTask::Done;
            if (!ModalDialog::Confirm(this, QStringLiteral("移除下载任务"),
                                      keepFiles ? QStringLiteral("确定移除「%1」？已下载的 %2 页文件会保留。")
                                                      .arg(t.chapterName)
                                                      .arg(t.done)
                                                : QStringLiteral("确定移除「%1」？已下载的文件会一并删除。")
                                                      .arg(t.chapterName),
                                      QStringLiteral("移除"), QStringLiteral("取消"),
                                      ModalDialog::Warning))
                return;
            mgr_->Remove(t.key, !keepFiles);
            Rebuild();
        });
        line1->addWidget(del);
        rl->addLayout(line1);

        auto *line2 = new QHBoxLayout();
        line2->setSpacing(10);
        QString meta = QStringLiteral("%1 · %2/%3 页")
                           .arg(t.albumTitle.isEmpty() ? QStringLiteral("未知作品") : t.albumTitle)
                           .arg(t.done)
                           .arg(t.total > 0 ? QString::number(t.total) : QStringLiteral("?"));
        if (t.bytes > 0)
            meta += QStringLiteral(" · %1").arg(HumanSize(t.bytes));
        if (!t.error.isEmpty())
            meta += QStringLiteral(" · %1").arg(t.error);
        auto *info = new ElideLabel(meta, row);
        info->setObjectName(QStringLiteral("RowMeta"));
        info->setMinimumWidth(0);
        if (!t.error.isEmpty())
            info->setToolTip(t.error);
        line2->addWidget(info, 1);
        if (t.total > 0) {
            auto *pct = new QLabel(QStringLiteral("%1%").arg(t.Percent()), row);
            pct->setObjectName(QStringLiteral("RowMeta"));
            line2->addWidget(pct);
        }
        rl->addLayout(line2);

        if (t.total > 0) {
            auto *bar = new QProgressBar(row);
            bar->setObjectName(QStringLiteral("RowProgress"));
            bar->setRange(0, 100);
            bar->setValue(t.Percent());
            bar->setTextVisible(false);
            rl->addWidget(bar);
        }
        list_->addWidget(row);
    }
    if (shown == 0) {
        const QString title = tasks.isEmpty() ? QStringLiteral("还没有下载任务")
                                              : QStringLiteral("这个筛选下没有任务");
        list_->addWidget(new EmptyState(
            title,
            QStringLiteral("在作品详情页点「下载」选择章节，任务会显示在这里，\n可暂停/继续，退出应用后也能接着下。"),
            QString(), host));
    }
}

QString DownloadPage::Report() const
{
    if (!mgr_)
        return QStringLiteral("dl-page: manager=0");
    const QString s = mgr_->Report();
    return QStringLiteral("%1 filter=%2").arg(s, filter_.isEmpty() ? QStringLiteral("-") : filter_);
}

// ============================================================ 本地
LocalPage::LocalPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("本地漫画"), QStringLiteral("读取已下载或自选目录"),
                            body, &header);
    lay->addWidget(head);

    auto *pick = new QPushButton(QStringLiteral("选择目录"), body);
    pick->setObjectName(QStringLiteral("Primary"));
    pick->setCursor(Qt::PointingHandCursor);
    header->addWidget(pick);
    connect(pick, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("选择漫画目录"));
        if (!dir.isEmpty())
            Scan(dir);
    });

    path_ = new QLabel(QStringLiteral("默认目录：未选择"), body);
    path_->setObjectName(QStringLiteral("SectionHint"));
    // 绝对路径（/data/user/0/<pkg>/files）不换行会把整页撑宽 59px，
    // 锁定横向平移后右侧会被裁；这里折行 + 允许压缩到 0。
    path_->setWordWrap(true);
    path_->setMinimumWidth(0);
    lay->addWidget(path_);

    auto *listHost = new QWidget(body);
    list_ = new QVBoxLayout(listHost);
    list_->setContentsMargins(0, 0, 0, 0);
    list_->setSpacing(8);
    lay->addWidget(listHost);
    lay->addStretch(1);

    root->addWidget(WrapScroll(body, this));
    // 默认扫描应用数据目录：手机上下载/导入的章节都落在这里，用户不必先
    // 走一次系统文件选择器（Android 上那是 SAF，选完还要拿 URI 权限）。
    Scan(AppPaths::DataDir());
}

void LocalPage::OpenDir(const QString &path)
{
    if (path.isEmpty())
        return;
    Scan(path);
}

void LocalPage::Scan(const QString &root)
{
    root_ = root;
    QWidget *host = list_ ? list_->parentWidget() : nullptr;
    // 扫描是"整块列表替换"：目录选完立刻换一批行会显得生硬，做一次短交叉淡出
    auto rebuild = [this, root] {
        ClearLayout(list_);
        auto *host = list_ ? list_->parentWidget() : nullptr;
        if (!host)
            return;

        if (root.isEmpty()) {
            if (path_)
                path_->setText(QStringLiteral("默认目录：未选择"));
            list_->addWidget(new EmptyState(
                QStringLiteral("尚未选择目录"),
                QStringLiteral("点右上角「选择目录」后，\n这里按真实目录列出本地漫画。"),
                QString(), host));
            return;
        }
        if (path_)
            path_->setText(QStringLiteral("目录：%1").arg(root));

        // 真实扫描：递归找出所有"直接含图片"的目录（最多 3 层）。
        // 下载目录是 <root>/<作品>/<章节>/*.jpg 两层结构，只扫一层会一行都出不来。
        const QStringList filters = {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                                     QStringLiteral("*.png"), QStringLiteral("*.webp")};
        const QDir dir(root);
        QVector<QFileInfo> subs;
        if (!dir.entryList(filters, QDir::Files).isEmpty())
            subs.push_back(QFileInfo(root));   // 根本身就是一章
        QDirIterator walk(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (walk.hasNext()) {
            const QString d = walk.next();
            if (QDir(d).entryList(filters, QDir::Files).isEmpty())
                continue;
            // 深度限制：root/a/b 就够（作品/章节），再深多半是用户自己的目录树
            const QString rel = QDir(root).relativeFilePath(d);
            if (rel.count(QLatin1Char('/')) > 2)
                continue;
            subs.push_back(QFileInfo(d));
        }
        std::sort(subs.begin(), subs.end(), [](const QFileInfo &a, const QFileInfo &b) {
            return a.absoluteFilePath() < b.absoluteFilePath();
        });
        int rows = 0;
        for (const QFileInfo &fi : subs) {
            const QDir sub(fi.absoluteFilePath());
            const int pages = sub.entryList(filters, QDir::Files).size();
            if (pages <= 0)
                continue;   // 不含图片的目录不占一行
            auto *row = new QWidget(host);
            row->setObjectName(QStringLiteral("ListRow"));
            auto *rl = new QHBoxLayout(row);
            rl->setContentsMargins(14, 10, 14, 10);
            rl->setSpacing(12);

            auto *icon = new QLabel(row);
            icon->setPixmap(Glyph::Pixmap(Glyph::Folder, 22, T().accent));
            rl->addWidget(icon);

            auto *mid = new QVBoxLayout();
            mid->setSpacing(2);
            const QString rel = QDir(root).relativeFilePath(fi.absoluteFilePath());
            auto *t = new QLabel(rel.isEmpty() ? QDir(root).dirName() : rel, row);
            t->setObjectName(QStringLiteral("RowTitle"));
            // 目录名可能很长：折行 + 允许压缩，避免整行/整页被撑宽
            t->setWordWrap(true);
            t->setMinimumWidth(0);
            mid->addWidget(t);
            auto *m = new QLabel(QStringLiteral("%1 页").arg(pages), row);
            m->setObjectName(QStringLiteral("RowMeta"));
            mid->addWidget(m);
            rl->addLayout(mid, 1);

            auto *open = new QPushButton(QStringLiteral("阅读"), row);
            open->setObjectName(QStringLiteral("Ghost"));
            open->setCursor(Qt::PointingHandCursor);
            open->setFixedHeight(28);
            rl->addWidget(open);
            const QString path = fi.absoluteFilePath();
            connect(open, &QPushButton::clicked, this, [this, path] { emit OpenLocal(path); });

            list_->addWidget(row);
            ++rows;
        }
        if (rows == 0) {
            list_->addWidget(new EmptyState(
                QStringLiteral("该目录下没有可读的漫画"),
                QStringLiteral("只统计包含 jpg/png/webp 图片的子目录。"),
                QString(), host));
        }
    };
    if (!host || !Motion::Enabled() || !host->isVisible())
        rebuild();
    else
        Motion::CrossFade(host, rebuild, Motion::Base);
}

// ============================================================ 收藏
FavoritePage::FavoritePage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    loading_ = new LoadingBar(this);
    root->addWidget(loading_);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("我的收藏"), QStringLiteral("登录后同步你的收藏"), body,
                            &header);
    lay->addWidget(head);
    hint_ = head->findChild<QLabel *>(QStringLiteral("PageSub"));
    auto *manage = new QPushButton(QStringLiteral("管理收藏夹"), body);
    manage->setObjectName(QStringLiteral("Ghost"));
    manage->setCursor(Qt::PointingHandCursor);
    header->addWidget(manage);
    connect(manage, &QPushButton::clicked, this, [this] {
        ModalDialog::Message(this, QStringLiteral("管理收藏夹"),
                             QStringLiteral("支持新建、重命名与排序。当前仅支持默认收藏夹，"
                                            "多收藏夹管理尚未接入。"));
    });

    auto *tabs = new QWidget(body);
    auto *tl = new QHBoxLayout(tabs);
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(8);
    const QStringList folders = {QStringLiteral("全部"), QStringLiteral("默认收藏夹"),
                                 QStringLiteral("在看"), QStringLiteral("完结归档")};
    for (int i = 0; i < folders.size(); ++i) {
        auto *chip = new Chip(folders.at(i), true, tabs);
        chip->setChecked(i == 0);
        tl->addWidget(chip);
        connect(chip, &QPushButton::clicked, this, [tabs, chip] {
            for (auto *b : tabs->findChildren<Chip *>())
                b->setChecked(b == chip);
        });
    }
    tl->addStretch(1);
    lay->addWidget(tabs);

    // 内容区：卡片网格 + 空状态互斥。两者都常驻，只切可见性——重建网格会
    // 丢掉封面缓存与滚动位置。
    auto *content = new QWidget(body);
    content_ = content;
    auto *cl = new QVBoxLayout(content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);

    grid_ = new ComicGrid(content);
    connect(grid_, &ComicGrid::Activated, this, &FavoritePage::OpenComic);
    connect(grid_, &ComicGrid::ContextAction, this,
            [this](const Comic &, const QString &action) { emit Toast(action); });
    cl->addWidget(grid_, 1);

    emptyHost_ = new QWidget(content);
    auto *el = new QVBoxLayout(emptyHost_);
    el->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(emptyHost_, 1);

    lay->addWidget(content, 1);
    root->addWidget(body);

    SetState(State::NotLoggedIn);
}

QString FavoritePage::StateName() const
{
    switch (state_) {
    case State::NotLoggedIn:
        return QStringLiteral("not-logged-in");
    case State::Loading:
        return QStringLiteral("loading");
    case State::Empty:
        return QStringLiteral("empty");
    case State::Error:
        return QStringLiteral("error");
    case State::Ready:
        return QStringLiteral("ready");
    }
    return QStringLiteral("unknown");
}

int FavoritePage::CardCount() const
{
    return grid_ ? grid_->Count() : 0;
}

void FavoritePage::OnEnter()
{
    if (!UserSession::User().loggedIn) {
        SetState(State::NotLoggedIn);
        return;
    }
    // 已有内容时不闪回"加载中"：刷新在后台做，返回后原地替换。
    if (state_ != State::Ready && state_ != State::Empty)
        SetState(State::Loading);
    emit RefreshRequested();
}

void FavoritePage::SetItems(const QVector<Comic> &items, int total)
{
    total_ = total >= 0 ? total : items.size();
    if (items.isEmpty()) {
        // 空收藏是合法结果，必须显式告诉用户"没有"，不能拿演示卡片顶上
        // （那会被理解成"这是我账号里的收藏"）。
        if (grid_)
            grid_->SetComics({});
        SetState(State::Empty);
        return;
    }
    if (grid_)
        grid_->SetComics(items);
    SetState(State::Ready);
}

void FavoritePage::SetChapters(const QString &id, int chapters)
{
    if (grid_)
        grid_->SetChapters(id, chapters);
}

void FavoritePage::SetState(State state, const QString &detail)
{
    state_ = state;
    stateDetail_ = detail;
    ApplyState();
}

void FavoritePage::ApplyState()
{
    if (!grid_ || !emptyHost_)
        return;
    if (loading_) {
        if (state_ == State::Loading)
            loading_->Start();
        else
            loading_->Stop();
    }

    auto apply = [this] {
        const bool ready = state_ == State::Ready;
        grid_->setVisible(ready);
        emptyHost_->setVisible(!ready);
        if (state_ == State::NotLoggedIn)
            grid_->SetComics({});   // 退出登录/会话失效后不留下上一个账号的封面

        if (hint_) {
            switch (state_) {
            case State::NotLoggedIn:
                hint_->setText(QStringLiteral("登录后同步你的收藏"));
                break;
            case State::Loading:
                hint_->setText(QStringLiteral("正在同步…"));
                break;
            case State::Error:
                hint_->setText(QStringLiteral("同步失败"));
                break;
            case State::Empty:
            case State::Ready:
                hint_->setText(QStringLiteral("共 %1 部").arg(total_));
                break;
            }
        }
        if (ready)
            return;

        // 空状态四种文案各一组动作：重试 / 去登录 / 去首页；加载中不给按钮。
        if (QLayout *l = emptyHost_->layout()) {
            while (QLayoutItem *it = l->takeAt(0)) {
                if (QWidget *w = it->widget())
                    w->deleteLater();
                delete it;
            }
        }
        QString title;
        QString text;
        QString action;
        switch (state_) {
        case State::Loading:
            title = QStringLiteral("正在加载收藏…");
            text = QStringLiteral("正在同步账号收藏夹。");
            break;
        case State::Empty:
            title = QStringLiteral("还没有收藏");
            // 文案按 9 个汉字/行断行：EmptyState 的说明列很窄，长句会在词中间折行
            text = QStringLiteral("在作品详情页收藏后\n这里就会显示。");
            action = QStringLiteral("去首页逛逛");
            break;
        case State::Error:
            title = QStringLiteral("收藏加载失败");
            text = stateDetail_.isEmpty() ? QStringLiteral("请检查网络后重试。") : stateDetail_;
            action = QStringLiteral("重试");
            break;
        case State::NotLoggedIn:
        default:
            title = QStringLiteral("登录后查看收藏");
            text = QStringLiteral("收藏与账号同步。");
            action = QStringLiteral("去登录");
            break;
        }
        empty_ = new EmptyState(title, text, action, emptyHost_);
        emptyHost_->layout()->addWidget(empty_);
        if (QPushButton *b = empty_->ActionButton()) {
            connect(b, &QPushButton::clicked, this, [this] {
                if (state_ == State::NotLoggedIn) {
                    emit LoginRequested();
                } else if (state_ == State::Error) {
                    SetState(State::Loading);
                    emit RefreshRequested();
                } else {
                    emit Navigate(PageId::Home);
                }
            });
        }
    };

    // 加载中 → 有内容 / 空 / 失败，是同一次"刷新"的不同阶段：内容区交叉淡出，
    // 避免卡片墙和空态在 0ms 内互换（观感上像闪了一下）。
    if (!Motion::Enabled() || !content_ || !content_->isVisible()) {
        apply();
        return;
    }
    Motion::CrossFade(content_, apply, Motion::Base);
}

// ============================================================ 历史
HistoryPage::HistoryPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("本地记录"), QStringLiteral("阅读记录（本地）"), body,
                            &header);
    lay->addWidget(head);
    auto *clear = new QPushButton(QStringLiteral("清空历史"), body);
    clear->setObjectName(QStringLiteral("Ghost"));
    clear->setCursor(Qt::PointingHandCursor);
    header->addWidget(clear);
    connect(clear, &QPushButton::clicked, this, [this] {
        if (!ModalDialog::Confirm(this, QStringLiteral("清空历史"),
                                  QStringLiteral("将删除全部本地阅读记录，且无法恢复。"),
                                  QStringLiteral("清空"), QStringLiteral("取消"),
                                  ModalDialog::Danger)) {
            return;
        }
        while (QLayoutItem *item = list_->takeAt(0)) {
            if (QWidget *w = item->widget())
                w->deleteLater();
            delete item;
        }
        // 清空是"一整列记录消失"：交叉淡出到空态，避免列表凭空蒸发
        QWidget *host = list_->parentWidget();
        auto showEmpty = [this, host] {
            auto *empty = new EmptyState(QStringLiteral("暂无阅读记录"),
                                         QStringLiteral("打开任意作品后，这里会按时间倒序记录。"),
                                         QString(), host ? host : this);
            list_->addWidget(empty);
        };
        if (host && Motion::Enabled() && host->isVisible())
            Motion::CrossFade(host, showEmpty, Motion::Base);
        else
            showEmpty();
        emit Toast(QStringLiteral("历史已清空"));
    });

    auto *listHost = new QWidget(body);
    list_ = new QVBoxLayout(listHost);
    list_->setContentsMargins(0, 0, 0, 0);
    list_->setSpacing(10);
    lay->addWidget(listHost);
    lay->addStretch(1);

    // 阅读记录只来自真实阅读行为产生的本地记录；没有记录就显示空态。
    list_->addWidget(new EmptyState(QStringLiteral("暂无阅读记录"),
                                    QStringLiteral("打开任意作品后，这里会按时间倒序记录。"),
                                    QString(), listHost));

    root->addWidget(WrapScroll(body, this));
}

// ============================================================ 评论
CommentPage::CommentPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    lay->addWidget(MakeHeader(QStringLiteral("评论"),
                              QStringLiteral("评论来自作品评论接口"), body, nullptr));

    auto *composer = new QWidget(body);
    composer->setObjectName(QStringLiteral("ListRow"));
    auto *cl = new QHBoxLayout(composer);
    cl->setContentsMargins(12, 10, 12, 10);
    cl->setSpacing(10);
    auto *edit = new QLineEdit(composer);
    edit->setObjectName(QStringLiteral("Input"));
    edit->setPlaceholderText(QStringLiteral("写下你的看法…"));
    cl->addWidget(edit, 1);
    auto *send = new QPushButton(QStringLiteral("发送"), composer);
    send->setObjectName(QStringLiteral("Primary"));
    send->setCursor(Qt::PointingHandCursor);
    cl->addWidget(send);
    connect(send, &QPushButton::clicked, this, [this, edit] {
        if (edit->text().trimmed().isEmpty())
            return;
        // 评论发送需要服务端接口；在接口接入前不伪造"已发送"的结果。
        emit Toast(QStringLiteral("评论接口尚未接入，暂不能发送"));
    });
    lay->addWidget(composer);

    auto *listHost = new QWidget(body);
    list_ = new QVBoxLayout(listHost);
    list_->setContentsMargins(0, 0, 0, 0);
    list_->setSpacing(10);
    lay->addWidget(listHost);
    lay->addStretch(1);

    // 没有任何评论数据时显示空态；不再渲染预置评论。
    list_->addWidget(new EmptyState(QStringLiteral("暂无评论"),
                                    QStringLiteral("评论接口返回后会显示在这里。"),
                                    QString(), listHost));

    root->addWidget(WrapScroll(body, this));
}
