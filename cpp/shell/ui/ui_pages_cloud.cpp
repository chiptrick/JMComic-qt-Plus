// 账号云端数据页：观看记录（/watch_list）、我的评论（/forum?uid=）、本地收藏（本地扫描）。
//
// 这三页原来都是"接口尚未接入"的占位页（空态 + Toast），本文件把它们接到真实数据：
//   * 观看记录 / 我的评论 —— 与收藏页同一套四态机（未登录 / 加载中 / 空 / 失败 / 就绪），
//     数据由 MainWindow 通过 JmApi 拉取后回填，本层不发请求、不碰网络；
//   * 本地收藏 —— 扫下载根目录下"含图片的子目录"，扫不到就是空态（不放样例）。
//
// 约定：不伪造数据。未登录就显示"去登录"，请求失败显示失败原因，列表为空就显示空态。
#include "ui_pages.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "download_manager.h"
#include "motion.h"
#include "theme.h"
#include "touch.h"
#include "ui_kit.h"

namespace {

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
    auto *s = new QLabel(sub, host);
    s->setObjectName(QStringLiteral("PageSub"));
    texts->addWidget(s);
    lay->addLayout(texts);
    lay->addStretch(1);
    if (out)
        *out = lay;
    return host;
}

// 把布局里的旧控件清掉（空态/行卡片重建时用）
void ClearLayout(QLayout *l, bool deleteWidgets = true)
{
    if (!l)
        return;
    while (QLayoutItem *it = l->takeAt(0)) {
        if (QWidget *w = it->widget()) {
            if (deleteWidgets)
                w->deleteLater();
            else
                w->setParent(nullptr);
        }
        if (QLayout *child = it->layout()) {
            ClearLayout(child, deleteWidgets);
            delete child;
        }
        delete it;
    }
}

QString FormatDate(qint64 unixSec)
{
    if (unixSec <= 0)
        return QString();
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(unixSec);
    if (!dt.isValid())
        return QString();
    return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

}  // namespace

// ============================================================ 观看记录（云端）
RemoteHistoryPage::RemoteHistoryPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    loading_ = new LoadingBar(this);
    root->addWidget(loading_);

    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);
    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("观看记录"),
                            QStringLiteral("账号云端记录 · 跨设备同步"), body, &header);
    lay->addWidget(head);
    hint_ = head->findChild<QLabel *>(QStringLiteral("PageSub"));

    sync_ = new QPushButton(QStringLiteral("同步"), body);
    sync_->setObjectName(QStringLiteral("Ghost"));
    sync_->setCursor(Qt::PointingHandCursor);
    sync_->setProperty("hitGrow", false);
    header->addWidget(sync_);
    connect(sync_, &QPushButton::clicked, this, [this] {
        page_ = 1;
        SetState(State::Loading);
        emit RefreshRequested(page_);
    });

    auto *content = new QWidget(body);
    content_ = content;
    auto *cl = new QVBoxLayout(content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);

    grid_ = new ComicGrid(content);
    connect(grid_, &ComicGrid::Activated, this, &RemoteHistoryPage::OpenComic);
    cl->addWidget(grid_, 1);

    emptyHost_ = new QWidget(content);
    new QVBoxLayout(emptyHost_);
    emptyHost_->layout()->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(emptyHost_, 1);

    moreHost_ = new QWidget(content);
    auto *ml = new QHBoxLayout(moreHost_);
    ml->setContentsMargins(0, 12, 0, 0);
    more_ = new QPushButton(QStringLiteral("加载更多"), moreHost_);
    more_->setObjectName(QStringLiteral("Ghost"));
    more_->setCursor(Qt::PointingHandCursor);
    more_->setProperty("hitGrow", false);
    ml->addStretch(1);
    ml->addWidget(more_);
    ml->addStretch(1);
    moreHost_->setVisible(false);
    cl->addWidget(moreHost_);
    connect(more_, &QPushButton::clicked, this, [this] {
        SetState(State::Loading);
        emit RefreshRequested(page_ + 1);
    });

    lay->addWidget(content, 1);
    root->addWidget(body);

    SetState(State::NotLoggedIn);
}

QString RemoteHistoryPage::StateName() const
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

int RemoteHistoryPage::CardCount() const
{
    return grid_ ? grid_->Count() : 0;
}

void RemoteHistoryPage::OnEnter()
{
    if (!UserSession::User().loggedIn) {
        SetState(State::NotLoggedIn);
        return;
    }
    if (state_ != State::Ready && state_ != State::Empty)
        SetState(State::Loading);
    page_ = 1;
    emit RefreshRequested(page_);
}

void RemoteHistoryPage::SetItems(const QVector<Comic> &items, int total, int page)
{
    total_ = total >= 0 ? total : items.size();
    page_ = qMax(1, page);
    if (page_ == 1) {
        if (grid_)
            grid_->SetComics(items);
    } else if (grid_) {
        grid_->AddComics(items);   // 追加：保留滚动位置
    }
    if (!items.isEmpty() && page_ == 1 && items.size() >= total_) {
        // 只有一页：隐藏"加载更多"
    }
    if (grid_ && grid_->Count() == 0) {
        SetState(State::Empty);
        return;
    }
    SetState(State::Ready);
}

void RemoteHistoryPage::SetState(State state, const QString &detail)
{
    state_ = state;
    stateDetail_ = detail;
    ApplyState();
}

void RemoteHistoryPage::ApplyState()
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
            grid_->SetComics({});

        if (hint_) {
            switch (state_) {
            case State::NotLoggedIn:
                hint_->setText(QStringLiteral("登录后显示云端记录"));
                break;
            case State::Loading:
                hint_->setText(QStringLiteral("正在同步…"));
                break;
            case State::Error:
                hint_->setText(QStringLiteral("同步失败"));
                break;
            case State::Empty:
            case State::Ready:
                hint_->setText(QStringLiteral("共 %1 条 · 第 %2 页").arg(total_).arg(page_));
                break;
            }
        }
        // "加载更多"只在"已就绪且还有下一页"时出现
        if (moreHost_)
            moreHost_->setVisible(ready && total_ > 0 && page_ * kPageSize < total_);
        if (sync_)
            sync_->setEnabled(state_ != State::Loading);

        if (ready)
            return;

        ClearLayout(emptyHost_->layout());
        QString title;
        QString text;
        QString action;
        switch (state_) {
        case State::Loading:
            title = QStringLiteral("正在加载观看记录…");
            text = QStringLiteral("正在从账号拉取云端记录。");
            break;
        case State::Empty:
            title = QStringLiteral("还没有观看记录");
            text = QStringLiteral("在其它设备阅读后\n这里会显示最近看过的作品。");
            action = QStringLiteral("去首页逛逛");
            break;
        case State::Error:
            title = QStringLiteral("观看记录加载失败");
            text = stateDetail_.isEmpty() ? QStringLiteral("请检查网络后重试。") : stateDetail_;
            action = QStringLiteral("重试");
            break;
        case State::NotLoggedIn:
        default:
            title = QStringLiteral("登录后查看记录");
            text = QStringLiteral("观看记录与账号同步。");
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
                    page_ = 1;
                    SetState(State::Loading);
                    emit RefreshRequested(page_);
                } else {
                    emit Navigate(PageId::Home);
                }
            });
        }
    };

    if (!Motion::Enabled() || !content_ || !content_->isVisible()) {
        apply();
        return;
    }
    Motion::CrossFade(content_, apply, Motion::Base);
}

// ============================================================ 我的评论
MyCommentPage::MyCommentPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    loading_ = new LoadingBar(this);
    root->addWidget(loading_);

    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);
    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("我的评论"), QStringLiteral("账号评论 · 按时间倒序"), body,
                            &header);
    lay->addWidget(head);
    hint_ = head->findChild<QLabel *>(QStringLiteral("PageSub"));

    refresh_ = new QPushButton(QStringLiteral("刷新"), body);
    refresh_->setObjectName(QStringLiteral("Ghost"));
    refresh_->setCursor(Qt::PointingHandCursor);
    refresh_->setProperty("hitGrow", false);
    header->addWidget(refresh_);
    connect(refresh_, &QPushButton::clicked, this, [this] {
        page_ = 1;
        SetState(State::Loading);
        emit RefreshRequested(page_);
    });

    auto *area = new QScrollArea(body);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    Touch::LockHorizontalPan(area);

    auto *host = new QWidget(area);
    list_ = new QVBoxLayout(host);
    list_->setContentsMargins(0, 0, 0, 0);
    list_->setSpacing(10);
    list_->addStretch(1);
    area->setWidget(host);
    area_ = area;
    lay->addWidget(area, 1);
    root->addWidget(body);

    SetState(State::NotLoggedIn);
}

void MyCommentPage::OnEnter()
{
    if (!UserSession::User().loggedIn) {
        SetState(State::NotLoggedIn);
        return;
    }
    if (state_ != State::Ready && state_ != State::Empty)
        SetState(State::Loading);
    page_ = 1;
    emit RefreshRequested(page_);
}

QString MyCommentPage::StateName() const
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

int MyCommentPage::RowCount() const
{
    return rows_;
}

void MyCommentPage::SetItems(const QVector<JmComment> &items, int total, int page)
{
    total_ = total >= 0 ? total : items.size();
    page_ = qMax(1, page);
    if (!list_)
        return;
    if (page_ == 1) {
        // 清空除尾部 stretch 以外的所有行
        while (list_->count() > 1) {
            QLayoutItem *it = list_->takeAt(0);
            if (QWidget *w = it->widget())
                w->deleteLater();
            delete it;
        }
        rows_ = 0;
    }
    for (const JmComment &c : items) {
        auto *card = new QWidget(area_ ? area_->widget() : this);
        card->setObjectName(QStringLiteral("ListRow"));
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(14, 10, 14, 10);
        cl->setSpacing(4);

        auto *top = new QHBoxLayout();
        top->setContentsMargins(0, 0, 0, 0);
        auto *who = new QLabel(c.user.isEmpty() ? QStringLiteral("我") : c.user, card);
        who->setObjectName(QStringLiteral("RowTitle"));
        top->addWidget(who);
        const QString when = FormatDate(c.addTime);
        if (!when.isEmpty()) {
            auto *time = new QLabel(when, card);
            time->setObjectName(QStringLiteral("SettingDesc"));
            top->addSpacing(8);
            top->addWidget(time);
        }
        top->addStretch(1);
        if (c.likes > 0) {
            auto *likes = new QLabel(QStringLiteral("赞 %1").arg(c.likes), card);
            likes->setObjectName(QStringLiteral("SettingDesc"));
            top->addWidget(likes);
        }
        cl->addLayout(top);

        auto *text = new QLabel(c.content, card);
        text->setObjectName(QStringLiteral("SettingDesc"));
        text->setWordWrap(true);
        text->setMinimumWidth(0);
        text->setTextInteractionFlags(Qt::TextSelectableByMouse);
        cl->addWidget(text);

        if (!c.albumName.isEmpty() || !c.albumId.isEmpty()) {
            auto *link = new QPushButton(
                QStringLiteral("→ %1").arg(c.albumName.isEmpty() ? c.albumId : c.albumName), card);
            link->setObjectName(QStringLiteral("Link"));
            link->setCursor(Qt::PointingHandCursor);
            link->setProperty("hitGrow", false);
            link->setFlat(true);
            const QString albumId = c.albumId;
            connect(link, &QPushButton::clicked, this, [this, albumId] {
                if (!albumId.isEmpty())
                    emit OpenAlbum(albumId);
            });
            cl->addWidget(link, 0, Qt::AlignLeft);
        }
        list_->insertWidget(list_->count() - 1, card);
        ++rows_;
    }
    if (rows_ == 0) {
        SetState(State::Empty);
        return;
    }
    SetState(State::Ready);
}

void MyCommentPage::SetState(State state, const QString &detail)
{
    state_ = state;
    stateDetail_ = detail;
    ApplyState();
}

void MyCommentPage::ApplyState()
{
    if (!list_)
        return;
    if (loading_) {
        if (state_ == State::Loading)
            loading_->Start();
        else
            loading_->Stop();
    }
    if (hint_) {
        switch (state_) {
        case State::NotLoggedIn:
            hint_->setText(QStringLiteral("登录后显示我的评论"));
            break;
        case State::Loading:
            hint_->setText(QStringLiteral("正在加载…"));
            break;
        case State::Error:
            hint_->setText(QStringLiteral("加载失败"));
            break;
        case State::Empty:
        case State::Ready:
            hint_->setText(QStringLiteral("共 %1 条").arg(total_));
            break;
        }
    }
    if (refresh_)
        refresh_->setEnabled(state_ != State::Loading);

    if (state_ == State::Ready || state_ == State::Loading)
        return;
    if (state_ == State::Empty) {
        // 空态：保留列表容器，插入一条 EmptyState（列表已空）
        if (empty_) {
            empty_->deleteLater();
            empty_ = nullptr;
        }
        empty_ = new EmptyState(QStringLiteral("还没有评论"),
                                QStringLiteral("在作品评论区发言后\n这里会按时间倒序显示。"),
                                QStringLiteral("去首页逛逛"), area_ ? area_->widget() : this);
        list_->insertWidget(list_->count() - 1, empty_);
        if (QPushButton *b = empty_->ActionButton())
            connect(b, &QPushButton::clicked, this, [this] { emit Navigate(PageId::Home); });
        return;
    }
    if (empty_) {
        empty_->deleteLater();
        empty_ = nullptr;
    }
    if (state_ == State::NotLoggedIn || state_ == State::Error) {
        empty_ = new EmptyState(
            state_ == State::NotLoggedIn ? QStringLiteral("登录后查看我的评论")
                                         : QStringLiteral("评论加载失败"),
            state_ == State::Error
                ? (stateDetail_.isEmpty() ? QStringLiteral("请检查网络后重试。") : stateDetail_)
                : QStringLiteral("评论与账号绑定。"),
            state_ == State::NotLoggedIn ? QStringLiteral("去登录") : QStringLiteral("重试"),
            area_ ? area_->widget() : this);
        list_->insertWidget(list_->count() - 1, empty_);
        if (QPushButton *b = empty_->ActionButton()) {
            connect(b, &QPushButton::clicked, this, [this] {
                if (state_ == State::NotLoggedIn)
                    emit LoginRequested();
                else {
                    page_ = 1;
                    SetState(State::Loading);
                    emit RefreshRequested(page_);
                }
            });
        }
    }
}

// ============================================================ 本地收藏（本地扫描）
LocalFavoritePage::LocalFavoritePage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("本地收藏"),
                            QStringLiteral("下载目录里已归档的作品 · 不依赖网络"), body, &header);
    lay->addWidget(head);
    hint_ = head->findChild<QLabel *>(QStringLiteral("PageSub"));

    auto *scan = new QPushButton(QStringLiteral("重新扫描"), body);
    scan->setObjectName(QStringLiteral("Ghost"));
    scan->setCursor(Qt::PointingHandCursor);
    scan->setProperty("hitGrow", false);
    header->addWidget(scan);
    connect(scan, &QPushButton::clicked, this, [this] { Rescan(); });

    grid_ = new ComicGrid(body);
    connect(grid_, &ComicGrid::Activated, this, [this](const Comic &c) {
        if (!c.id.isEmpty())
            emit OpenLocal(c.id);   // 本地项把目录路径放在 id 里
    });
    lay->addWidget(grid_, 1);

    empty_ = new EmptyState(QStringLiteral("暂无本地收藏"),
                            QStringLiteral("下载或导入作品后\n这里会显示本地归档目录。"),
                            QStringLiteral("去下载页"), body);
    lay->addWidget(empty_, 1);
    if (QPushButton *b = empty_->ActionButton())
        connect(b, &QPushButton::clicked, this, [this] { emit Navigate(PageId::Download); });

    root->addWidget(body);
    Rescan();
}

void LocalFavoritePage::OnEnter()
{
    Rescan();
}

QString LocalFavoritePage::Report() const
{
    return QStringLiteral("local-favorite root=%1 items=%2")
        .arg(root_.isEmpty() ? QStringLiteral("-") : root_)
        .arg(grid_ ? grid_->Count() : 0);
}

void LocalFavoritePage::Rescan()
{
    const QString root = DownloadManager::Root();
    root_ = root;
    QVector<Comic> items;
    QDir base(root);
    if (base.exists()) {
        const QFileInfoList subs =
            base.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &sub : subs) {
            // 只认"含图片的子目录"：这就是已归档作品的落盘形态
            int images = 0;
            QDirIterator it(sub.absoluteFilePath(),
                            {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                             QStringLiteral("*.png"), QStringLiteral("*.webp")},
                            QDir::Files);
            while (it.hasNext()) {
                it.next();
                if (++images >= 1)
                    break;
            }
            if (images == 0)
                continue;
            Comic c;
            c.id = sub.absoluteFilePath();
            c.title = sub.fileName();
            c.author = QStringLiteral("本地");
            c.coverUrl = QString();   // 本地目录没有统一封面，用占位图
            items.append(c);
        }
    }

    if (grid_)
        grid_->SetComics(items);
    const bool has = !items.isEmpty();
    if (grid_)
        grid_->setVisible(has);
    if (empty_)
        empty_->setVisible(!has);
    if (hint_)
        hint_->setText(has ? QStringLiteral("共 %1 个本地归档 · %2").arg(items.size()).arg(root)
                           : QStringLiteral("下载目录：%1").arg(root));
}
