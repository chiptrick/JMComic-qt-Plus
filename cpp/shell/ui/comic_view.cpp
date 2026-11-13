#include "comic_view.h"

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QInputDevice>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickImageProvider>
#include <QQuickItem>
#include <QQuickWidget>
#include <QScroller>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>

#include <cstdio>

#include "cover_cache.h"
#include "cover_provider.h"
#include "app_font.h"
#include "theme.h"
#include "touch.h"
#include "ui_kit.h"

// 封面 image provider 的请求串编码（见 ComicListModel::CoverReqRole）。
// 必须用 base64url（无 +/=）：QML 会把 image source 当 URL 做 percent-encode，
// 标准 base64 的 + 会变空格、/ 会被当路径段、| 会被编码成 %7C，provider 端
// 拿到的 id 就对不上请求串了。分隔符用 '.'（base64url 字母表里没有）。
namespace QCFmt {
QString b64(const QString &s)
{
    const QByteArray raw = s.toUtf8().toBase64(QByteArray::Base64UrlEncoding
                                               | QByteArray::OmitTrailingEquals);
    return QString::fromLatin1(raw);
}
}

// 轻量压测数据：--shot 时打印，用来证明"只画可见区域"（定义见 comic_view.h）

ComicBench g_comicBench;

// ============================================================ 模型
ComicListModel::ComicListModel(QObject *parent) : QAbstractListModel(parent) {}

int ComicListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : items_.size();
}

QVariant ComicListModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= items_.size())
        return {};
    const Comic &c = items_.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return c.title;
    case Qt::ToolTipRole: {
        // chapters==0 表示列表接口没给章节数（不是真的 0 话）：
        // 此时 tooltip 只写到作者，别把"未知"说成"0 话"。
        QString tip = c.title;
        if (!c.author.isEmpty())
            tip += QLatin1Char('\n') + c.author;
        if (c.chapters > 0)
            tip += QStringLiteral(" · %1 话").arg(c.chapters);
        return tip;
    }
    case ComicRole:
        return QVariant::fromValue(index.row());
    case IdRole:
        return c.id;
    case TitleRole:
        return c.title;
    case AuthorRole:
        return c.author;
    case StatusRole:
        return c.status;
    case ChaptersRole:
        return c.chapters;
    case MetaRole:
        // 成稿文案由 C++ 决定（QML 侧不再拼 "作者 + ' · ' + chapters + ' 话'"）
        return ComicMetaText(c);
    case ReadChapterRole:
        return c.readChapter;
    case UpdatedRole:
        return c.updated;
    case CoverUrlRole:
        return c.coverUrl;
    case CoverReqRole:
        // "b64(key)|b64(url)|b64(title)|w|h"——base64 不含 '|'，QML 端拼上
        // coverRev 作为整体 URL。宽高与显示尺寸一致，provider 不再缩放。
        return QCFmt::b64(c.id) + QLatin1Char('.') + QCFmt::b64(c.coverUrl)
               + QLatin1Char('.') + QCFmt::b64(c.title) + QLatin1Char('.')
               + QString::number(coverW_) + QLatin1Char('.')
               + QString::number(coverW_ * 4 / 3);
    case CoverRevRole:
        return revById_.value(c.id, 0);
    default:
        return {};
    }
}

QHash<int, QByteArray> ComicListModel::roleNames() const
{
    auto names = QAbstractListModel::roleNames();
    names.insert(ComicRole, "comic");
    names.insert(IdRole, "id");
    names.insert(TitleRole, "title");
    names.insert(AuthorRole, "author");
    names.insert(StatusRole, "status");
    names.insert(ChaptersRole, "chapters");
    names.insert(MetaRole, "metaText");
    names.insert(ReadChapterRole, "readChapter");
    names.insert(UpdatedRole, "updated");
    names.insert(CoverUrlRole, "coverUrl");
    names.insert(CoverReqRole, "coverReq");
    names.insert(CoverRevRole, "coverRev");
    return names;
}

void ComicListModel::SetCoverWidth(int w)
{
    if (coverW_ == w)
        return;
    coverW_ = qMax(96, w);
    // coverReq 的宽高变了：全列刷新（尺寸变化通常伴随 resize，QML 会整体重排）
    if (!items_.isEmpty())
        emit dataChanged(index(0), index(items_.size() - 1), {CoverReqRole});
}

int ComicListModel::RowOfKey(const QString &key) const
{
    for (int i = 0; i < items_.size(); ++i) {
        if (items_.at(i).id == key)
            return i;
    }
    return -1;
}

void ComicListModel::BumpRow(int row)
{
    if (row < 0 || row >= items_.size())
        return;
    ++revById_[items_.at(row).id];
    emit dataChanged(index(row), index(row), {CoverRevRole});
}

void ComicListModel::OnCoverReady(const QString &key)
{
    BumpRow(RowOfKey(key));
}

void ComicListModel::SetComics(const QVector<Comic> &comics)
{
    beginResetModel();
    items_ = comics;
    endResetModel();
}

void ComicListModel::AddComics(const QVector<Comic> &more)
{
    if (more.isEmpty())
        return;
    const int first = items_.size();
    beginInsertRows(QModelIndex(), first, first + more.size() - 1);
    items_ += more;
    endInsertRows();
}

bool ComicListModel::SetChapters(const QString &id, int chapters)
{
    if (id.isEmpty() || chapters <= 0)
        return false;
    bool hit = false;
    for (int i = 0; i < items_.size(); ++i) {
        Comic &c = items_[i];
        if (c.id != id)
            continue;
        hit = true;
        if (c.chapters == chapters)
            continue;
        c.chapters = chapters;
        // 两个角色都要通知：QML 进度条读 chapters，副标题读 metaText。
        emit dataChanged(index(i), index(i), {ChaptersRole, MetaRole});
    }
    return hit;
}

void ComicListModel::SetPagedSource(std::function<QVector<Comic>(int, int)> loader, int total)
{
    beginResetModel();
    items_.clear();
    loader_ = std::move(loader);
    total_ = total;
    hasMore_ = (total_ > 0) && static_cast<bool>(loader_);
    endResetModel();
    if (hasMore_)
        fetchMore(QModelIndex());
}

bool ComicListModel::canFetchMore(const QModelIndex &parent) const
{
    return !parent.isValid() && hasMore_;
}

void ComicListModel::fetchMore(const QModelIndex &parent)
{
    if (parent.isValid() || !hasMore_ || !loader_)
        return;
    constexpr int kLimit = 60;
    const int offset = items_.size();
    const QVector<Comic> more = loader_(offset, kLimit);
    if (more.isEmpty()) {
        hasMore_ = false;
        return;
    }
    beginInsertRows(QModelIndex(), items_.size(), items_.size() + more.size() - 1);
    items_ += more;
    endInsertRows();
    g_comicBench.rows = qMax(g_comicBench.rows, items_.size());
    if (items_.size() >= total_ || more.size() < kLimit)
        hasMore_ = false;
}

const Comic &ComicListModel::At(int row) const
{
    static const Comic empty;
    if (row < 0 || row >= items_.size())
        return empty;
    return items_.at(row);
}

// ============================================================ Delegate
ComicDelegate::ComicDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

void ComicDelegate::SetCardWidth(int w)
{
    cardWidth_ = qMax(96, w);
}

QSize ComicDelegate::sizeHint(const QStyleOptionViewItem &, const QModelIndex &) const
{
    return CardPaint::Size(cardWidth_);
}

void ComicDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                          const QModelIndex &index) const
{
    const auto *model = qobject_cast<const ComicListModel *>(index.model());
    if (!model)
        return;
    const Comic &c = model->At(index.row());

    ++g_comicBench.painted;
    const bool hover = option.state & QStyle::State_MouseOver;
    const bool selected = option.state & QStyle::State_Selected;

    QRect r = option.rect;
    r.setSize(CardPaint::Size(cardWidth_));
    // 单元格可能比卡片宽（最后一列），水平居中
    r.moveLeft(option.rect.left() + (option.rect.width() - r.width()) / 2);

    painter->save();
    CardPaint::Paint(painter, r, c, hover, selected);
    // 键盘导航：当前项画一圈强调色，避免"用 Tab 进来不知道焦点在哪"
    if (option.state & QStyle::State_HasFocus) {
        const UiTokens &t = UiTheme::I().Tokens();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setBrush(Qt::NoBrush);
        painter->setPen(QPen(t.accent, 1.4));
        UiStyle::DrawPanel(painter, QRectF(r).adjusted(0.5, 0.5, -0.5, -0.5));
    }
    painter->restore();
}

// ============================================================ 视图
ComicListView::ComicListView(QWidget *parent) : QListView(parent)
{
    setObjectName(QStringLiteral("ComicListView"));
    setViewMode(QListView::IconMode);
    setResizeMode(QListView::Adjust);
    setMovement(QListView::Static);
    setWrapping(true);
    setFlow(QListView::LeftToRight);
    setUniformItemSizes(true);
    setSelectionMode(QAbstractItemView::NoSelection);
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFrameShape(QFrame::NoFrame);
    setMouseTracking(true);
    setSpacing(0);
    setEditTriggers(QAbstractItemView::NoEditTriggers);
    setFocusPolicy(Qt::StrongFocus);

    // 只有真的有触摸屏才接管拖拽滚动，否则桌面端鼠标拖拽会变成"甩列表"
    for (const QInputDevice *dev : QInputDevice::devices()) {
        if (dev->type() == QInputDevice::DeviceType::TouchScreen) {
            QScroller::grabGesture(viewport(), QScroller::LeftMouseButtonGesture);
            break;
        }
    }
    // 网格按视口宽度换行，横向本就没有可滚内容；锁掉横移避免整块卡片墙左右漂
    Touch::LockHorizontalPan(this);
}

void ComicListView::ArmLongPress(const QPoint &pos, const QModelIndex &index)
{
    if (!pressTimer_) {
        pressTimer_ = new QTimer(this);
        pressTimer_->setSingleShot(true);
        pressTimer_->setInterval(520);
        connect(pressTimer_, &QTimer::timeout, this, [this] {
            if (!pressIndex_.isValid())
                return;
            emit ContextRequested(pressIndex_, viewport()->mapToGlobal(pressPos_));
        });
    }
    pressPos_ = pos;
    pressIndex_ = index;
    pressTimer_->start();
}

void ComicListView::CancelLongPress()
{
    if (pressTimer_)
        pressTimer_->stop();
    pressIndex_ = QPersistentModelIndex();
}

void ComicListView::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::RightButton) {
        const QModelIndex idx = indexAt(e->position().toPoint());
        if (idx.isValid())
            emit ContextRequested(idx, e->globalPosition().toPoint());
        return;
    }
    QListView::mousePressEvent(e);
    if (e->button() == Qt::LeftButton)
        ArmLongPress(e->position().toPoint(), indexAt(e->position().toPoint()));
}

void ComicListView::mouseReleaseEvent(QMouseEvent *e)
{
    CancelLongPress();
    QListView::mouseReleaseEvent(e);
}

void ComicListView::mouseMoveEvent(QMouseEvent *e)
{
    // 手指/鼠标移动超过阈值就认为是在滚动，取消长按
    if (pressTimer_ && pressTimer_->isActive()
        && (e->position().toPoint() - pressPos_).manhattanLength() > 8) {
        CancelLongPress();
    }
    QListView::mouseMoveEvent(e);
    // hover 高亮由 QAbstractItemView::mouseMoveEvent 内建处理：hover index
    // 变化时只重绘新旧两格。这里**不要** viewport()->update() —— 触摸拖动时
    // 每次 mousemove（~60-120Hz）都会触发一次全视口脏区，与滚动重绘叠加后
    // 真机（1440x3168@1.33dpr，约 32MB/帧的 CPU 光栅化）实测只有 ~20fps。
}

// 滚动 = 卡片墙整体平移（静态布局，滚动不改变任何项的几何）。
// QAbstractItemView::scrollContentsBy 会对整个 viewport 调 update()：
// 真机每帧约 32MB 的 CPU 光栅化 + 同量级的 flush，实测滚动只有 ~20fps
// （换页/抽屉动画走 QQuickWidget GPU 覆盖层，同一屏是 ~112fps）。
// 改用 QWidget::scroll 的 backing-store blit：把已有像素 memcpy 平移，
// 只重绘新暴露的条带，flush 脏区也从 32MB 缩到一条 —— 符号与
// QAbstractScrollArea 传参一致（dx/dy 即内容平移量，QScrollArea 的
// widget->move(pos + QPoint(dx,dy)) 可证）。
void ComicListView::scrollContentsBy(int dx, int dy)
{
    if (dx == 0 && dy == 0) {
        QListView::scrollContentsBy(0, 0);
        return;
    }
    viewport()->scroll(dx, dy);
}

// ============================================================ ComicGrid
// 0.90 性能版：widgets 的 QListView 是 CPU 光栅化，真机（1440x3168@1.33dpr）
// 滚动只有 ~30fps（blit 优化后），换页/抽屉动画走 QQuickWidget 的 GPU 路径
// 却有 ~112fps。这里把视图层换成 QQuickWidget + PageCards.qml（Qt Quick
// scene graph），对外接口（SetComics/AddComics/SetPagedSource/SetCardWidth/
// Count/ScrollToBottom + 两个信号）保持不变——首页/分类/搜索/收藏的调用点
// 零改动。旧 widgets 视图（ComicListView/ComicDelegate）保留在文件里，
// 桌面 --shot 压测与回归仍可引用。
ComicGrid::ComicGrid(QWidget *parent) : QWidget(parent)
{
    model_ = new ComicListModel(this);
    // 封面就绪（provider 预取完成）→ 精确行 dataChanged
    connect(&CoverProvider::I(), &CoverProvider::CoverReady,
            model_, &ComicListModel::OnCoverReady);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    quick_ = new QQuickWidget(this);
    quick_->setObjectName(QStringLiteral("ComicQuickGrid"));
    quick_->setResizeMode(QQuickWidget::SizeRootObjectToView);
    quick_->setClearColor(UiTheme::I().Tokens().bg);
    quick_->setFocusPolicy(Qt::NoFocus);
    quick_->engine()->addImageProvider(QStringLiteral("covers"),
                                       CoverProvider::I().MakeShell());
    quick_->rootContext()->setContextProperty(QStringLiteral("comicModel"), model_);
    quick_->rootContext()->setContextProperty(QStringLiteral("cardBridge"), this);
    quick_->setSource(QUrl(QStringLiteral("qrc:/qml/PageCards.qml")));
    if (quick_->status() != QQuickWidget::Ready) {
        std::fprintf(stderr, "comic-grid: QML load failed (status=%d)\n",
                     int(quick_->status()));
        const auto errs = quick_->errors();
        for (const QQmlError &e : errs)
            std::fprintf(stderr, "  %s\n", qPrintable(e.toString()));
    }
    lay->addWidget(quick_);

    SyncTheme();
    SyncFont();
    ApplyGeometry(width());
    connect(&UiTheme::I(), &UiTheme::Changed, this, [this] { SyncTheme(); });
    // 字体/字重/字号变化：文字尺寸与格子高度都要重算（否则卡片文字不跟着变，
    // 或者格子变高而文字不变 —— 用户看到的就是"页面被拉长"）
    connect(&AppFont::I(), &AppFont::Changed, this, [this] {
        SyncFont();
        ApplyGeometry(width());
    });
}

ComicGrid::~ComicGrid() = default;

void ComicGrid::SyncTheme()
{
    const UiTokens &t = UiTheme::I().Tokens();
    if (quick_)
        quick_->setClearColor(t.bg);
    QObject *root = quick_ ? quick_->rootObject() : nullptr;
    if (!root)
        return;
    root->setProperty("cBg", t.bg);
    root->setProperty("cSurface", t.surface);
    root->setProperty("cSurfaceHover", t.surfaceHover);
    root->setProperty("cText", t.text);
    root->setProperty("cTextDim", t.textDim);
    root->setProperty("cAccent", t.accent);
}

void ComicGrid::SyncFont()
{
    QObject *root = quick_ ? quick_->rootObject() : nullptr;
    if (!root)
        return;
    const QFont f = AppFont::I().Build();
    root->setProperty("uiFontFamily", f.family());
    root->setProperty("uiFontWeight", int(f.weight()));
    root->setProperty("uiScale", UiStyle::FontScale());
    // QML 默认字体：没显式设 font.* 的元素（如空状态文本）也跟着走
    if (quick_)
        quick_->setFont(f);
}

// 复刻原 Relayout 的列数规则：触摸设备按视口宽切列（160dp 一格，1~6 列），
// 桌面沿用固定卡宽。几何全部在 C++ 算好，QML 只消费 cardW/cellW/cellH。
void ComicGrid::ApplyGeometry(int viewWidth)
{
    int w = cardWidth_;
    int gap = 18;
    if (Touch::IsTouchMode()) {
        // 目标格宽取 160dp：不能用 172——真机 384dp 宽整除得 1 会退化成单列
        // （实测 vivo V2463A）。QQuickWidget 没有滚动条占位，不再扣那条宽度。
        const int avail = qMax(viewWidth, 200);
        const int cols = qBound(1, avail / 160, 6);
        gap = 12;
        w = qBound(96, avail / cols - gap - 1, 240);
    }
    coverW_ = w;
    model_->SetCoverWidth(w);
    QObject *root = quick_ ? quick_->rootObject() : nullptr;
    if (root) {
        // 文字块高度与 PageCards.qml 的 titlePx/metaPx/titleH/metaH 同一套公式。
        // 只缩放字号而不缩放格子，或反之，都会出现"文字被裁 / 大片留白"。
        const qreal s = UiStyle::FontScale();
        const int titleH = qRound(18 * s);
        const int metaH = qRound(14 * s);
        const int textTop = qRound(8 * s);
        const int padBottom = qRound(6 * s);
        const int contentH = w * 4 / 3 + textTop + titleH + qRound(2 * s) + metaH + padBottom;
        root->setProperty("cardW", w);
        root->setProperty("cellW", w + gap);
        root->setProperty("cellH", contentH + gap);
    }
}

bool ComicGrid::TouchMode() const
{
    return Touch::IsTouchMode();
}

QObject *ComicGrid::QmlRootForTest() const
{
    return quick_ ? quick_->rootObject() : nullptr;
}

void ComicGrid::resizeEvent(QResizeEvent *ev)
{
    QWidget::resizeEvent(ev);
    // 视口定宽 / 旋屏后重算格子；桌面端沿用固定卡宽，不重排
    if (Touch::IsTouchMode() && width() != lastViewportW_) {
        lastViewportW_ = width();
        ApplyGeometry(width());
    }
}

void ComicGrid::SetCardWidth(int w)
{
    cardWidth_ = qMax(96, w);
    ApplyGeometry(width());
}

void ComicGrid::SetPagedSource(std::function<QVector<Comic>(int, int)> loader, int total)
{
    model_->SetPagedSource(std::move(loader), total);
    ApplyGeometry(width());
    if (quick_ && quick_->rootObject())
        QMetaObject::invokeMethod(quick_->rootObject(), "scrollToTop");
}

void ComicGrid::AddComics(const QVector<Comic> &comics)
{
    if (comics.isEmpty())
        return;
    model_->AddComics(comics);
    ApplyGeometry(width());
    for (int i = 0; i < qMin(30, comics.size()); ++i) {
        const Comic &c = comics.at(i);
        CoverProvider::I().Prime(c.id, c.title, c.coverUrl,
                                 coverW_, coverW_ * 4 / 3);
    }
}

void ComicGrid::SetComics(const QVector<Comic> &comics)
{
    QElapsedTimer timer;
    timer.start();
    model_->SetComics(comics);
    // 取最大值：一个窗口里有多个网格（首页/分类/搜索/收藏），只关心最大的那个
    g_comicBench.rows = qMax(g_comicBench.rows, comics.size());
    g_comicBench.setMs = qMax(g_comicBench.setMs, timer.elapsed());
    ApplyGeometry(width());
    // 预热前 60 个封面（一屏左右），滚动时不至于全是占位图
    const int n = qMin(60, comics.size());
    for (int i = 0; i < n; ++i) {
        const Comic &c = comics.at(i);
        CoverProvider::I().Prime(c.id, c.title, c.coverUrl,
                                 coverW_, coverW_ * 4 / 3);
    }
    if (quick_ && quick_->rootObject())
        QMetaObject::invokeMethod(quick_->rootObject(), "scrollToTop");
}

void ComicGrid::SetChapters(const QString &id, int chapters)
{
    // 只刷新命中的那一行（dataChanged → QML 重新读 chapters/metaText），
    // 不重建模型、不重置滚动位置。
    if (model_)
        model_->SetChapters(id, chapters);
}

void ComicGrid::ScrollToBottom()
{
    if (quick_ && quick_->rootObject())
        QMetaObject::invokeMethod(quick_->rootObject(), "scrollToEnd");
}

int ComicGrid::Count() const
{
    return model_ ? model_->Count() : 0;
}

void ComicGrid::cardClicked(int row)
{
    if (swipeLock_)
        return;   // 分区横滑中：这次 release 不算点击
    if (row >= 0 && row < model_->Count())
        emit Activated(model_->At(row));
}

void ComicGrid::cardLongPress(int row)
{
    if (swipeLock_)
        return;
    // 触摸长按时 Qt 会合成鼠标位置，QCursor::pos() 即手指位置
    ShowCardMenu(row, QCursor::pos());
}

void ComicGrid::ShowCardMenu(int row, const QPoint &globalPos)
{
    if (row < 0 || row >= model_->Count())
        return;
    const Comic c = model_->At(row);
    QMenu menu(this);
    QAction *open = menu.addAction(Glyph::Icon(Glyph::Info, 14, UiTheme::I().Tokens().textDim),
                                   QStringLiteral("查看详情"));
    QAction *fav = menu.addAction(Glyph::Icon(Glyph::Heart, 14, UiTheme::I().Tokens().textDim),
                                  QStringLiteral("加入收藏"));
    QAction *dl = menu.addAction(Glyph::Icon(Glyph::Download, 14, UiTheme::I().Tokens().textDim),
                                 QStringLiteral("下载"));
    QAction *picked = menu.exec(globalPos);
    if (!picked)
        return;
    if (picked == open)
        emit Activated(c);
    else
        emit ContextAction(c, picked == fav ? QStringLiteral("已加入收藏")
                                            : QStringLiteral("已加入下载队列"));
}
