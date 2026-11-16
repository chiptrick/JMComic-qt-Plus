#include "ui_pages.h"

#include <cstdio>
#include <functional>

#include <QBoxLayout>
#include <QComboBox>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "../jm_api.h"
#include "cover_cache.h"
#include "motion.h"
#include "search_history.h"
#include "section_strip.h"
#include "section_swipe.h"
#include "theme.h"
#include "touch.h"
#include "ui_kit.h"

// ============================================================ 公共小工具
namespace {

QPixmap RoundedPixmap(const QPixmap &src, int radius)
{
    // 直角矩形风格：没有圆角要裁，直接返回原图（省一次全尺寸拷贝）。
    if (src.isNull() || UiStyle::Radius <= 0)
        return src;
    QPixmap out(src.size());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPainterPath path;
    path.addRoundedRect(QRectF(src.rect()), radius, radius);
    p.setClipPath(path);
    p.drawPixmap(0, 0, src);
    return out;
}

QWidget *MakeHeader(const QString &title, const QString &sub, QWidget *parent,
                    QHBoxLayout **out)
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
        // 副标题可能很长（"与 Python 版 config.ini 同 section/key"）：换行 + 不撑宽
        s->setWordWrap(true);
        s->setMinimumWidth(0);
        texts->addWidget(s);
    }
    // 文本列吃掉剩余宽度（副标题才能整行折行，而不是被挤成 3 行窄条）
    lay->addLayout(texts, 1);
    if (out)
        *out = lay;
    return host;
}

// 手机竖屏（含桌面 --touch/--drawer 验收）：页边距与区块间距都收紧。
// 之前固定 28px 边距 + 18px 间距，在 384dp 宽的屏上等于把两侧和行间都吃掉 1/6。
bool NarrowTouch()
{
    return Touch::IsTouchMode();
}

int BodyMargin()
{
    return NarrowTouch() ? UiStyle::Fpx(12) : 28;
}

int BodySpacing()
{
    return NarrowTouch() ? UiStyle::Fpx(10) : 18;
}

QScrollArea *WrapScroll(QWidget *content, QWidget *parent)
{
    auto *area = new QScrollArea(parent);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setWidget(content);
    // 触摸模式：禁掉横向平移（设置/详情这类纵向页面在手机上不该能左右滑）
    Touch::LockHorizontalPan(area);
    return area;
}

QWidget *PageBody(QWidget *parent, int margin = -1)
{
    auto *w = new QWidget(parent);
    auto *lay = new QVBoxLayout(w);
    const int m = margin >= 0 ? margin : BodyMargin();
    lay->setContentsMargins(m, m, m, m);
    lay->setSpacing(BodySpacing());
    return w;
}

QVBoxLayout *BodyLayout(QWidget *body)
{
    return qobject_cast<QVBoxLayout *>(body->layout());
}

QWidget *TagRow(const QStringList &tags, QWidget *parent)
{
    auto *host = new QWidget(parent);
    auto *lay = new QHBoxLayout(host);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(6);
    for (const QString &t : tags) {
        auto *l = new QLabel(t, host);
        l->setObjectName(QStringLiteral("TagDim"));
        lay->addWidget(l);
    }
    lay->addStretch(1);
    return host;
}

}  // namespace

// ============================================================ 首页
HomePage::HomePage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // 分区结构照 Python 版：第 0 页是「最新上传」（/latest），其余由 /promote 逐个建。
    // 数据没到之前不建任何页 —— 页面显示加载态，而不是先摆一批样例条目。
    //
    // 分区容器（SectionSwipe）：每个分区是一整页（自带分区栏 + 内容），切换时
    // 整页横向滑动、分区栏跟着页面一起走。QTabWidget 做不到"两页同屏滑动"，
    // 而且分区栏要跟着滑，所以它不再是页外常驻控件。
    // 加载扫描线：整页最上方 3px，只在"等接口"期间扫动（见 LoadingBar）
    loading_ = new LoadingBar(this);
    root->addWidget(loading_);

    deck_ = new SectionSwipe(this);
    deck_->hide();
    root->addWidget(deck_, 1);

    // 加载中 / 失败占位：与 deck_ 互斥，只切可见性。
    emptyHost_ = new QWidget(this);
    auto *el = new QVBoxLayout(emptyHost_);
    el->setContentsMargins(0, 0, 0, 0);
    root->addWidget(emptyHost_, 1);

    SetLoading();
}

void HomePage::ShowPlaceholder(const QString &title, const QString &text, const QString &action)
{
    if (deck_)
        deck_->hide();
    if (!emptyHost_)
        return;
    if (QLayout *l = emptyHost_->layout()) {
        while (QLayoutItem *it = l->takeAt(0)) {
            if (QWidget *w = it->widget())
                w->deleteLater();
            delete it;
        }
    }
    emptyHost_->show();
    auto *state = new EmptyState(title, text, action, emptyHost_);
    emptyHost_->layout()->addWidget(state);
    if (QPushButton *b = state->ActionButton())
        connect(b, &QPushButton::clicked, this, &HomePage::RetryRequested);
}

void HomePage::SetLoading(const QString &text)
{
    if (loading_)
        loading_->Start();
    ShowPlaceholder(QStringLiteral("正在加载首页…"),
                    text.isEmpty() ? QStringLiteral("内容来自禁漫接口，请稍候。") : text,
                    QString());
}

void HomePage::SetFailed(const QString &message)
{
    if (loading_)
        loading_->Stop();
    ShowPlaceholder(QStringLiteral("首页加载失败"),
                    message.isEmpty() ? QStringLiteral("请检查网络后重试。") : message,
                    QStringLiteral("重试"));
}

void HomePage::SetSections(const QVector<HomeSection> &sections)
{
    if (!deck_)
        return;
    if (sections.isEmpty()) {
        // 接口返回空 = 没有首页内容，不能拿本地样例顶上。
        SetFailed(QStringLiteral("接口没有返回任何首页分区，请稍后重试。"));
        return;
    }
    if (loading_)
        loading_->Stop();
    // 整组重建：旧页由容器 deleteLater，grids_ 是裸指针，先清空再建
    // （留着会指向已销毁对象，下一次 SetChapters 直接崩）。
    grids_.clear();
    sections_ = sections;
    latestPage_ = 0;
    latestMore_ = nullptr;
    latestStatus_ = nullptr;
    latestGrid_ = nullptr;

    QStringList titles;
    QVector<QWidget *> contents;
    titles.reserve(sections_.size());
    contents.reserve(sections_.size());
    for (const HomeSection &sec : sections_) {
        titles << sec.title;
        contents << BuildSection(sec);
    }
    if (deck_) {
        deck_->SetPanes(titles, contents);
        deck_->show();
    }
    if (emptyHost_) {
        if (QLayout *l = emptyHost_->layout()) {
            while (QLayoutItem *it = l->takeAt(0)) {
                if (QWidget *w = it->widget())
                    w->deleteLater();
                delete it;
            }
        }
        emptyHost_->hide();
    }
}

void HomePage::SetChapters(const QString &id, int chapters)
{
    // 首页每个分区各一个 grid：同 id 的卡片可能出现在多个分区里（最新上传 + 推荐），
    // 全部刷新。grids_ 与分区页同生命周期，SetSections 重建时已清空。
    for (ComicGrid *grid : grids_) {
        if (grid)
            grid->SetChapters(id, chapters);
    }
}

void HomePage::AppendLatest(const QVector<Comic> &items, int page)
{
    latestPage_ = page;
    if (!latestGrid_)
        return;
    if (items.isEmpty()) {
        if (latestMore_) {
            latestMore_->setText(QStringLiteral("没有更多了"));
            latestMore_->setEnabled(false);
        }
        UpdateLatestStatus();
        return;
    }
    latestGrid_->AddComics(items);
    if (latestMore_) {
        latestMore_->setEnabled(true);
        latestMore_->setText(QStringLiteral("加载更多"));
    }
    UpdateLatestStatus();
}

void HomePage::UpdateLatestStatus()
{
    if (!latestStatus_)
        return;
    const int n = latestGrid_ ? latestGrid_->Count() : 0;
    latestStatus_->setText(QStringLiteral("第 %1 页 · 已加载 %2 部")
                               .arg(qMax(1, latestPage_ + 1))
                               .arg(n));
}

QWidget *HomePage::BuildSection(const HomeSection &sec)
{
    // 父控件由 SectionSwipe 在 SetPanes 里接管（每页自带分区栏）
    auto *host = new QWidget;
    auto *lay = new QVBoxLayout(host);
    // Python 里 tab 内的 QVBoxLayout 用默认 9px 边距；手机上再收 3px，
    // 把横向空间留给卡片（384dp 宽时 9px 边距会明显缩小卡片）。
    const int m = NarrowTouch() ? UiStyle::Fpx(6) : UiStyle::Fpx(9);
    lay->setContentsMargins(m, m, m, m);
    lay->setSpacing(NarrowTouch() ? UiStyle::Fpx(4) : UiStyle::Fpx(6));

    auto *grid = new ComicGrid(host);
    grid->SetComics(sec.comics);
    connect(grid, &ComicGrid::Activated, this, &HomePage::OpenComic);
    connect(grid, &ComicGrid::ContextAction, this,
            [this](const Comic &, const QString &action) { emit Toast(action); });
    lay->addWidget(grid, 1);
    grids_.push_back(grid);
    if (sec.kind == HomeSectionKind::Latest)
        latestGrid_ = grid;

    // /latest 的翻页入口：只有服务端真实分页，页数未知，所以只给"加载更多"，
    // 不造页码框/总数（接口不返回 total）。
    if (sec.kind == HomeSectionKind::Latest) {
        auto *statusRow = new QWidget(host);
        statusRow->setObjectName(QStringLiteral("HomeBar"));
        auto *sl = new QHBoxLayout(statusRow);
        sl->setContentsMargins(0, 0, 0, 0);
        sl->setSpacing(8);
        latestStatus_ = new QLabel(statusRow);
        latestStatus_->setObjectName(QStringLiteral("RowMeta"));
        sl->addWidget(latestStatus_);
        sl->addStretch(1);
        latestMore_ = new QPushButton(QStringLiteral("加载更多"), statusRow);
        latestMore_->setObjectName(QStringLiteral("Ghost"));
        latestMore_->setCursor(Qt::PointingHandCursor);
        latestMore_->setFixedHeight(34);
        sl->addWidget(latestMore_);
        lay->addWidget(statusRow);
        connect(latestMore_, &QPushButton::clicked, this, [this] {
            if (!latestMore_ || !latestMore_->isEnabled())
                return;
            latestMore_->setEnabled(false);
            latestMore_->setText(QStringLiteral("加载中…"));
            emit LoadMoreLatest(latestPage_ + 1);
        });
        UpdateLatestStatus();
    }
    return host;
}


void HomePage::SelectSection(int index)
{
    // 截图/冒烟用：无动画落位（与 QTabWidget::setCurrentIndex 的旧语义一致）
    if (deck_ && deck_->Count() > 0)
        deck_->SetCurrent(qBound(0, index, deck_->Count() - 1), false);
}

int HomePage::CurrentSection() const
{
    return deck_ ? deck_->Current() : 0;
}

int HomePage::StripOffset() const { return deck_ ? deck_->StripOffset() : 0; }

int HomePage::StripContentWidth() const { return deck_ ? deck_->StripContentWidth() : 0; }

bool HomePage::StripDragging() const { return deck_ && deck_->StripDragging(); }

void HomePage::StripDrag(int dx)
{
    if (deck_)
        deck_->StripDrag(dx);
}

void HomePage::StripTap(int x)
{
    if (!deck_)
        return;
    const int before = CurrentSection();
    deck_->StripTap(x);
    qWarning("strip: tap x=%d before=%d after=%d offset=%d", x, before, CurrentSection(),
             deck_->StripOffset());
}

QString HomePage::StripStateReport() const
{
    SectionStrip *strip = deck_ ? deck_->CurrentStrip() : nullptr;
    return QStringLiteral("strip: offset=%1 max=%2 viewport=%3 content=%4 section=%5 %6")
        .arg(strip ? strip->Offset() : -1)
        .arg(strip ? qMax(0, strip->ContentWidth() - strip->width()) : -1)
        .arg(strip ? strip->width() : -1)
        .arg(strip ? strip->ContentWidth() : -1)
        .arg(CurrentSection())
        .arg(deck_ ? deck_->StateReport() : QStringLiteral("-"));
}

void HomePage::SwipeDemo(int dx, int dy)
{
    if (deck_)
        deck_->DemoSwipe(dx, dy);
}

void HomePage::FreezeSection(int from, int to, qreal progress)
{
    if (deck_)
        deck_->FreezeAt(from, to, progress);
}

QString HomePage::GeometryReport() const
{
    SectionStrip *strip = deck_ ? deck_->CurrentStrip() : nullptr;
    QWidget *pane = deck_ ? deck_->CurrentPane() : nullptr;
    const QString bar = latestStatus_ ? QString::number(latestStatus_->height()) : QStringLiteral("-1");
    const int textH = latestStatus_ ? QFontMetrics(latestStatus_->font()).height() : -1;
    return QStringLiteral(
               "page=%1x%2 strip=%3x%4 tabs=%5x%6 tabbar=%7 pane=%8x%9 pagerH=%10 pagerTextH=%11")
        .arg(width()).arg(height())
        .arg(strip ? strip->width() : -1).arg(strip ? strip->height() : -1)
        .arg(deck_ ? deck_->width() : -1).arg(deck_ ? deck_->height() : -1)
        .arg(strip ? strip->height() : -1)
        .arg(pane ? pane->width() : -1).arg(pane ? pane->height() : -1)
        .arg(bar).arg(textH);
}

int HomePage::StripHitTest(int x) const
{
    SectionStrip *strip = deck_ ? deck_->CurrentStrip() : nullptr;
    if (!strip)
        return -1;
    // SectionStrip 的命中测试是私有的；这里用"内容宽度 + 偏移"复算同一套规则，
    // 供回归脚本核对"拖动后点哪一格应该命中哪个分区"。
    const QStringList items = strip->Items();
    int left = -strip->Offset();
    for (int i = 0; i < items.size(); ++i) {
        QFont f = strip->font();
        f.setPixelSize(UiStyle::Fpx(13));
        const int w = QFontMetrics(f).horizontalAdvance(items.at(i)) + 28;
        if (x >= left && x < left + w)
            return i;
        left += w;
    }
    return -1;
}

void HomePage::OnEnter() {}

void HomePage::ScrollToBottom()
{
    if (ComicGrid *g = grids_.value(CurrentSection()))
        g->ScrollToBottom();
}

int HomePage::ItemCount() const
{
    // 冒烟脚本量的是"首页第一个分区加载了多少条"，与分析口径一致
    ComicGrid *g = grids_.value(0);
    return g ? g->Count() : 0;
}

// ============================================================ 分类
CategoryPage::CategoryPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    lay->addWidget(MakeHeader(QStringLiteral("分类与排行"),
                              QStringLiteral("按题材浏览，结果来自站内搜索接口"), body, nullptr));

    auto *filters = new QWidget(body);
    auto *fl = new QHBoxLayout(filters);
    fl->setContentsMargins(0, 0, 0, 0);
    fl->setSpacing(10);

    auto *tag = new QComboBox(filters);
    tag->setObjectName(QStringLiteral("Filter"));
    tag->addItems({QStringLiteral("全部题材"), QStringLiteral("恋爱"), QStringLiteral("科幻"),
                   QStringLiteral("悬疑"), QStringLiteral("日常"), QStringLiteral("奇幻"),
                   QStringLiteral("治愈")});
    tag->setCursor(Qt::PointingHandCursor);
    tag->setFixedHeight(32);
    fl->addWidget(tag);

    auto *apply = new QPushButton(QStringLiteral("应用筛选"), filters);
    apply->setObjectName(QStringLiteral("Primary"));
    apply->setCursor(Qt::PointingHandCursor);
    fl->addWidget(apply);
    fl->addStretch(1);
    lay->addWidget(filters);

    // 筛选结果是服务端 /search 的返回：本页不预置任何卡片，交由外壳跳到搜索结果。
    auto *host = new QWidget(body);
    auto *hl = new QVBoxLayout(host);
    hl->setContentsMargins(0, 0, 0, 0);
    auto *empty = new EmptyState(QStringLiteral("选择题材开始浏览"),
                                 QStringLiteral("选择题材后点「应用筛选」，\n结果由搜索接口返回。"),
                                 QString(), host);
    hl->addWidget(empty);
    lay->addWidget(host, 1);

    connect(apply, &QPushButton::clicked, this, [this, tag] {
        const QString keyword = tag->currentIndex() <= 0 ? QString() : tag->currentText();
        if (keyword.isEmpty()) {
            emit Toast(QStringLiteral("请选择具体题材"));
            return;
        }
        emit FilterRequested(keyword, QString());
    });

    root->addWidget(body);
}

// ============================================================ 搜索
SearchPage::SearchPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    loading_ = new LoadingBar(this);
    root->addWidget(loading_);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    // 手机上不再放大标题：搜索页的第一屏应该直接是输入框 + 最近搜索。
    // 原来 "搜索 / 支持漫画名、作者、标签与站内 ID" 两行 + 18px 间距 + 28px 边距
    // 一共吃掉 ~120px，正是用户报的"顶部被大量空白占据"。
    if (!NarrowTouch()) {
        lay->addWidget(MakeHeader(QStringLiteral("搜索"),
                                  QStringLiteral("支持漫画名、作者、标签与站内 ID"), body, nullptr));
    }

    auto *bar = new QWidget(body);
    auto *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(0, 0, 0, 0);
    bl->setSpacing(NarrowTouch() ? 8 : 10);
    box_ = new SearchBox(QStringLiteral("输入关键词，回车搜索"), bar);
    // 整行高度唯一来源 = 输入框的"可见高度"。SearchBox 的可见边框是内部
    // QLineEdit（基座 qss height:18px 钉内容盒 + 覆盖层 padding 6×2 + 边框 2
    // = 恒 32px，与字号缩放无关），容器本身透明；之前把容器钉成 Fpx(42)/Fpx(40)
    // 并让按钮钉同一高度，而按钮实际被基座 qss 的 34px 内容盒 + #Primary padding
    // 顶到 50px 且压过 setFixedHeight——字号缩放下按钮比输入框高出一大截
    // （桌面非触摸同样错位 8px）。现在行高取输入框自然高度，按钮高度由 QSS 侧
    // [searchGo="true"] 钉成同一值（见 theme.cpp 覆盖层注释），并退出触摸层
    // 44dp 命中区放大：两者严格等高、顶/底同轴，桌面与触摸共用一条路径。
    const int barH = qMax(box_->Edit()->sizeHint().height(), 28);
    box_->setFixedHeight(barH);
    // 360 的最小宽在 384dp 手机上会把"搜索"按钮挤出屏幕（还会撑出横向滚动范围）
    box_->setMinimumWidth(NarrowTouch() ? 0 : 360);
    bl->addWidget(box_, 1);
    auto *go = new QPushButton(QStringLiteral("搜索"), bar);
    go->setObjectName(QStringLiteral("Primary"));
    go->setProperty("searchGo", true);   // QSS 侧钉高度，必须在 show/polish 前打上
    go->setCursor(Qt::PointingHandCursor);
    // 与输入框同行等高：退出触摸层 44dp 命中区放大（Touch::PolishHitTargets）。
    // 那一步在布局前遍历，会把"未布局默认 100x30"的按钮冻结成 setFixedSize(100,44)，
    // 同时顶掉 setFixedHeight(barH)——按钮变成 44px 高，与输入框错位。
    // 整行（输入框+按钮）本身就是一块连续的可点区域，行高按输入框走。
    go->setProperty("hitGrow", false);
    go->setFixedHeight(barH);
    bl->addWidget(go);
    lay->addWidget(bar);

    // 历史 + 热搜
    historyBox_ = new QWidget(body);
    auto *hbl = new QVBoxLayout(historyBox_);
    hbl->setContentsMargins(0, 0, 0, 0);
    hbl->setSpacing(NarrowTouch() ? 8 : 12);

    // 最近搜索：从 ui.ini 读，搜索时写入。整组在有记录时才显示；
    // 之前这里是写死的四个演示词，用户搜过的词一个都不会留下。
    historyGroup_ = new QWidget(historyBox_);
    auto *hgl = new QVBoxLayout(historyGroup_);
    hgl->setContentsMargins(0, 0, 0, 0);
    hgl->setSpacing(NarrowTouch() ? 8 : 12);
    auto *histHeader = new SectionHeader(QStringLiteral("最近搜索"), QString(),
                                         QStringLiteral("清空"), body);
    connect(histHeader->ActionButton(), &QPushButton::clicked, this,
            [this] { ClearHistory(); });
    hgl->addWidget(histHeader);

    // 用流式布局：手机上 4 个历史词横排一定溢出（384dp 只能放 3 个），
    // 换成 FlowLayout 自动换行，避免右侧被裁掉、也避免撑出横向滚动。
    histChips_ = new QWidget(historyGroup_);
    histFlow_ = new FlowLayout(histChips_, 8);
    hgl->addWidget(histChips_);
    histEmpty_ = new QLabel(QStringLiteral("暂无搜索记录"), historyGroup_);
    histEmpty_->setObjectName(QStringLiteral("RowMeta"));
    histEmpty_->setMinimumWidth(0);
    hgl->addWidget(histEmpty_);
    hbl->addWidget(historyGroup_);

    lay->addWidget(historyBox_);

    // 结果
    resultBox_ = new QWidget(body);
    auto *rbl = new QVBoxLayout(resultBox_);
    rbl->setContentsMargins(0, 0, 0, 0);
    rbl->setSpacing(12);
    resultHint_ = new QLabel(resultBox_);
    resultHint_->setObjectName(QStringLiteral("SectionTitle"));
    // 统计行只有一行字，垂直方向必须"刚好装下文字"：默认 Preferred 策略会让
    // QVBoxLayout 把富余高度平摊给它（与卡片墙各一半），标签被拉到上千 px、
    // 文字垂直居中，于是搜索框与卡片之间出现两条比文字高几十倍的空白带。
    // Fixed + 无内边距 → 行高 = 字高，多余高度全部交给卡片墙（stretch=1）。
    resultHint_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    resultHint_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    resultHint_->setContentsMargins(0, 0, 0, 0);
    resultHint_->setMinimumWidth(0);
    rbl->addWidget(resultHint_);
    grid_ = new ComicGrid(resultBox_);
    connect(grid_, &ComicGrid::Activated, this, &SearchPage::OpenComic);
    connect(grid_, &ComicGrid::ContextAction, this,
            [this](const Comic &, const QString &action) { emit Toast(action); });
    rbl->addWidget(grid_, 1);
    more_ = new QPushButton(QStringLiteral("加载更多"), resultBox_);
    more_->setObjectName(QStringLiteral("Ghost"));
    more_->setCursor(Qt::PointingHandCursor);
    more_->setFixedHeight(34);
    more_->hide();
    rbl->addWidget(more_, 0, Qt::AlignHCenter);
    connect(more_, &QPushButton::clicked, this, [this] {
        if (keyword_.isEmpty())
            return;
        more_->setEnabled(false);
        more_->setText(QStringLiteral("加载中…"));
        emit SearchRequested(keyword_, page_ + 1);
    });
    resultBox_->hide();
    lay->addWidget(resultBox_, 1);
    // 结果区隐藏时要有个"吃空间"的东西，否则 QVBoxLayout 会把多余高度平摊给
    // 标题/副标题/搜索框，页面被拉成一条条大间距（见修复前的 search 截图）。
    tailSpacer_ = new QWidget(body);
    tailSpacer_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    lay->addWidget(tailSpacer_);

    connect(go, &QPushButton::clicked, this, [this] { RunSearch(box_->Text()); });
    connect(box_, &SearchBox::Submitted, this, [this](const QString &t) {
        if (!t.trimmed().isEmpty())
            RunSearch(t);
    });

    ReloadHistory();   // 首帧就把上次落库的记录摆出来
    root->addWidget(body);

}

void SearchPage::OnEnter()
{
    // 从别的页面切回来时刷新：记录可能在别的入口（CLI / 上一个进程）刚写过。
    ReloadHistory();
    if (resultBox_ && !resultBox_->isVisible()) {
        historyBox_->show();
        if (tailSpacer_)
            tailSpacer_->show();
    }
}

void SearchPage::ReloadHistory()
{
    if (!histFlow_ || !historyGroup_)
        return;
    auto rebuild = [this] {
        // 清空旧 chips：QLayoutItem 要连 widget 一起释放，否则叠加出重复按钮。
        while (QLayoutItem *item = histFlow_->takeAt(0)) {
            if (QWidget *w = item->widget())
                delete w;
            delete item;
        }
        const QStringList hist = SearchHistory::Items();
        // 整组常驻：布局回归要量「最近搜索 / 清空」这一行；无记录时只换占位文案。
        histChips_->setVisible(!hist.isEmpty());
        if (histEmpty_)
            histEmpty_->setVisible(hist.isEmpty());
        for (const QString &h : hist) {
            auto *chip = new Chip(h, false, histChips_);
            histFlow_->addWidget(chip);
            connect(chip, &QPushButton::clicked, this, [this, h] {
                box_->SetText(h);
                RunSearch(h);
            });
        }
        histChips_->adjustSize();
    };
    // 「最近搜索」会被搜索、清空、重进页面反复改写；直接重建会让整排词"跳变"。
    if (!Motion::Enabled() || !historyGroup_->isVisible()) {
        rebuild();
        return;
    }
    Motion::CrossFade(historyGroup_, rebuild, Motion::Fast);
}

void SearchPage::ClearHistory()
{
    SearchHistory::Clear();
    ReloadHistory();
    emit Toast(QStringLiteral("已清空最近搜索"));
}

void SearchPage::Search(const QString &keyword)
{
    box_->SetText(keyword);
    RunSearch(keyword);
}

void SearchPage::RunSearch(const QString &kw)
{
    const QString key = kw.trimmed();
    if (key.isEmpty()) {
        emit Toast(QStringLiteral("请输入关键词"));
        return;
    }
    keyword_ = key;
    // 记录搜索词：必须在 trim + 非空判定之后，点历史 chip 也走这里，
    // 所以「最近搜索」会按真实使用顺序更新（MRU）。
    if (SearchHistory::Add(key))
        ReloadHistory();
    page_ = 0;
    total_ = 0;
    SetSearching(key);
    emit SearchRequested(key, 1);
}

void SearchPage::SetSearching(const QString &keyword)
{
    historyBox_->hide();
    if (tailSpacer_)
        tailSpacer_->hide();
    if (loading_)
        loading_->Start();
    resultBox_->show();
    if (more_)
        more_->hide();
    // 结果区从"上一次的结果/空态"换成"正在搜索"：交叉淡出，避免整块内容瞬间清空
    Motion::CrossFade(resultBox_, [this, keyword] {
        resultHint_->setText(QStringLiteral("正在搜索“%1”…").arg(keyword));
        grid_->SetComics({});
    }, Motion::Fast);
}

void SearchPage::SetResults(const QVector<Comic> &items, int total, int page, bool append)
{
    historyBox_->hide();
    if (tailSpacer_)
        tailSpacer_->hide();
    resultBox_->show();
    if (loading_)
        loading_->Stop();

    auto swap = [this, items, total, page, append] {
        if (!append) {
            page_ = page;
            total_ = total;
            grid_->SetComics(items);
        } else {
            page_ = page;
            total_ = total;
            grid_->AddComics(items);
        }

        const int shown = grid_->Count();
        if (shown == 0) {
            resultHint_->setText(QStringLiteral("“%1” 没有匹配的作品").arg(keyword_));
        } else {
            resultHint_->setText(QStringLiteral("“%1” 的搜索结果 · 共 %2 部（已显示 %3）")
                                     .arg(keyword_)
                                     .arg(total_)
                                     .arg(shown));
        }
        if (more_) {
            const bool hasMore = total_ > shown && !items.isEmpty();
            more_->setVisible(hasMore);
            more_->setEnabled(true);
            more_->setText(QStringLiteral("加载更多"));
        }
    };
    // 首屏结果替换：旧内容（"正在搜索…"）淡出；翻页追加不做淡入，
    // 否则用户盯着列表底部时整块会闪一下。
    if (append)
        swap();
    else
        Motion::CrossFade(resultBox_, swap, Motion::Base);

    std::fprintf(stderr, "search-real: kw=%s page=%d items=%d total=%d append=%d\n",
                 qPrintable(keyword_), page, int(items.size()), total_, int(append));
    std::fflush(stderr);
}

void SearchPage::SetChapters(const QString &id, int chapters)
{
    if (grid_)
        grid_->SetChapters(id, chapters);
}

void SearchPage::SetFailed(const QString &message)
{
    // 接口失败就如实显示失败 + 重试，不拿本地样例填充结果区。
    historyBox_->hide();
    if (tailSpacer_)
        tailSpacer_->hide();
    if (loading_)
        loading_->Stop();
    resultBox_->show();
    Motion::CrossFade(resultBox_, [this, message] {
        grid_->SetComics({});
        if (more_)
            more_->hide();
        resultHint_->setText(QStringLiteral("“%1” 搜索失败：%2")
                                 .arg(keyword_)
                                 .arg(message.isEmpty() ? QStringLiteral("请检查网络后重试")
                                                        : message));
        total_ = 0;
    }, Motion::Base);
}

// ============================================================ 详情
DetailPage::DetailPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    body_ = body;
    auto *lay = BodyLayout(body);

    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("作品详情"), QString(), body, &header);
    lay->addWidget(head);
    auto *back = new QPushButton(QStringLiteral("返回"), body);
    back->setObjectName(QStringLiteral("Ghost"));
    back->setCursor(Qt::PointingHandCursor);
    connect(back, &QPushButton::clicked, this, [this] { emit Navigate(PageId::Home); });
    header->addWidget(back);

    auto *top = new QWidget(body);
    // 手机竖屏只有 ~363dp 宽：封面 180 + 22 间距 + 信息列（简介 max 620）远超出屏宽，
    // 右侧"收藏"按钮和简介尾行会被直接裁掉。窄屏改成上下堆叠，宽屏保持左图右文。
    //
    // 判定**不能**用主屏宽度：页面在主窗口构造期就建好了，桌面验收窗口即使只有
    // 427px 宽，primaryScreen 仍是 1920，按主屏算会一直走"左图右文"——信息列
    // （三个按钮的最小宽 280 + 封面 180 + 间距 22 = 482）把内容顶到 514px，
    // 视口只有 427px，hmax=87，标题/标签/简介整体右侧裁切（11-13 363dp 实测）。
    // 现在统一由 ApplyTopLayout() 按页面实际宽度算，并在 resize 时重算。
    QBoxLayout *tl = new QBoxLayout(QBoxLayout::LeftToRight, top);
    topLayout_ = tl;
    tl->setContentsMargins(0, 0, 0, 0);
    tl->setSpacing(22);

    cover_ = new QLabel(top);
    cover_->setFixedSize(QSize(180, 240));
    tl->addWidget(cover_, 0, Qt::AlignTop);

    auto *info = new QVBoxLayout();
    info->setSpacing(10);
    title_ = new QLabel(top);
    QFont tf = title_->font();
    tf.setPixelSize(UiStyle::Fpx(24));
    tf.setWeight(QFont::Medium);
    title_->setFont(tf);
    // 真实标题很长（"[汉化组] 原名 ..."）：不换行时 QLabel 的 minimumSizeHint 会把
    // 整页撑得比屏幕还宽，右侧（封面、返回按钮）被裁掉。真机实测过。
    title_->setWordWrap(true);
    info->addWidget(title_);

    meta_ = new QLabel(top);
    meta_->setObjectName(QStringLiteral("PageSub"));
    meta_->setWordWrap(true);
    info->addWidget(meta_);
    // 作者名是 meta_ 里的行内链接（见 UpdateMeta）。href 只做哨兵，
    // 真正的搜索词始终取当前作品的作者，避免把展示文本（可能被转义/折行）当关键词。
    connect(meta_, &QLabel::linkActivated, this, [this](const QString &href) {
        if (href != QLatin1String("jm-author"))
            return;
        const QString author = c_.author.trimmed();
        if (!author.isEmpty())
            emit SearchRequested(author);
    });
    // 换主题会换强调色：行内链接的 color 是写进富文本的，得按新 token 重刷一次
    connect(&UiTheme::I(), &UiTheme::Changed, this, [this] { UpdateMeta(); });

    auto *stat = new QHBoxLayout();
    stat->setSpacing(8);
    info->addLayout(stat);

    tagRow_ = new QWidget(top);
    // 6 个标签在 363dp 竖屏上必然宽过一屏：改用流式布局让它们自己折行，
    // 否则右侧标签会被裁掉（点不到），页面也会被撑出横向范围。
    // Python 版的标签区同样是 FlowLayout。
    tagFlow_ = new FlowLayout(tagRow_, 6);
    // 折行后的高度只有在容器的 sizePolicy 声明了 heightForWidth 时才会被外层
    // 布局采纳；不声明的话外层按"一行 chip"给高度，第二行起会被裁掉（点不到）。
    QSizePolicy tagPolicy = tagRow_->sizePolicy();
    tagPolicy.setHeightForWidth(true);
    tagRow_->setSizePolicy(tagPolicy);
    tagRow_->setVisible(false);
    info->addWidget(tagRow_);

    desc_ = new QLabel(top);
    desc_->setObjectName(QStringLiteral("SettingDesc"));
    desc_->setWordWrap(true);
    desc_->setMaximumWidth(620);   // 宽屏上限；窄屏由 ApplyTopLayout 解掉
    info->addWidget(desc_);

    auto *btns = new QHBoxLayout();
    btns->setSpacing(10);
    auto *read = new QPushButton(QStringLiteral("开始阅读"), top);
    read->setObjectName(QStringLiteral("Primary"));
    UiStyle::MarkSoftButton(read);   // 近似直角（3px）
    read->setCursor(Qt::PointingHandCursor);
    auto *dl = new QPushButton(QStringLiteral("下载本章"), top);
    dl->setObjectName(QStringLiteral("Ghost"));
    dl->setCursor(Qt::PointingHandCursor);
    favBtn_ = new QPushButton(QStringLiteral("收藏"), top);
    favBtn_->setObjectName(QStringLiteral("Ghost"));
    UiStyle::MarkSoftButton(favBtn_);   // "收藏 / 已收藏"是同一个按钮，造型保持不变
    favBtn_->setCursor(Qt::PointingHandCursor);
    auto *comment = new QPushButton(QStringLiteral("评论"), top);
    comment->setObjectName(QStringLiteral("Ghost"));
    comment->setCursor(Qt::PointingHandCursor);
    btns->addWidget(read);
    btns->addWidget(dl);
    btns->addWidget(favBtn_);
    btns->addWidget(comment);
    btns->addStretch(1);
    connect(comment, &QPushButton::clicked, this, [this] {
        if (c_.id.isEmpty()) {
            emit Toast(QStringLiteral("缺少作品 ID，无法查看评论"));
            return;
        }
        emit CommentsRequested(c_);
    });
    info->addLayout(btns);

    connect(read, &QPushButton::clicked, this, [this] {
        currentChapter_ = qBound(0, currentChapter_, qMax(0, chapters_.size() - 1));
        emit StartReading(c_, currentChapter_);
    });
    // 下载入口：直接给"本章"太窄（用户常常想整本），全量又太粗（几百话）。
    // 点一下弹三项，覆盖两种意图，并且每一项都能立刻看到任务出现在下载页。
    connect(dl, &QPushButton::clicked, this, [this, dl] {
        const int n = chapters_.size();
        if (n <= 0) {
            emit Toast(QStringLiteral("章节还没加载出来，请稍后重试"));
            return;
        }
        QMenu menu(dl);
        const int cur = qBound(0, currentChapter_, n - 1);
        QAction *one = menu.addAction(QStringLiteral("下载本话（%1）")
                                          .arg(chapters_.at(cur).name));
        QAction *recent = n > 1 ? menu.addAction(QStringLiteral("下载最新 5 话")) : nullptr;
        QAction *all = menu.addAction(QStringLiteral("下载全部（%1 话）").arg(n));
        QAction *picked = menu.exec(dl->mapToGlobal(QPoint(0, dl->height() + 4)));
        if (!picked)
            return;
        QVector<int> idx;
        if (picked == one) {
            idx << cur;
        } else if (recent && picked == recent) {
            for (int i = qMax(0, n - 5); i < n; ++i)
                idx << i;
        } else if (picked == all) {
            idx.reserve(n);
            for (int i = 0; i < n; ++i)
                idx << i;
        }
        if (idx.isEmpty())
            return;
        currentChapter_ = cur;
        emit DownloadRequested(c_, idx);
    });
    connect(favBtn_, &QPushButton::clicked, this, [this] {
        const bool wanted = !c_.favorite;
        // 收藏是账号数据：只走 /favorite 异步请求，成功后由 SetFavorite() 回填文案。
        // 失败就不会出现"显示已收藏但其实没收藏"。
        if (c_.id.isEmpty()) {
            emit Toast(QStringLiteral("缺少作品 ID，无法收藏"));
            return;
        }
        emit FavoriteRequested(c_, wanted);
    });

    tl->addLayout(info, 1);
    lay->addWidget(top);

    auto *line = new QFrame(body);
    line->setObjectName(QStringLiteral("Divider"));
    line->setFixedHeight(1);
    lay->addWidget(line);

    chapterHeader_ = new SectionHeader(QStringLiteral("章节目录"), QStringLiteral("暂无章节"),
                                      QStringLiteral("倒序"), body);
    lay->addWidget(chapterHeader_);
    chapterHost_ = new QWidget(body);
    lay->addWidget(chapterHost_);
    lay->addStretch(1);

    scroll_ = WrapScroll(body, this);
    root->addWidget(scroll_);

    // 未经卡片点击直接进本页时不摆样例作品：只留空态提示，等真实作品/章节回填。
    title_->setText(QStringLiteral("未选择作品"));
    desc_->setText(QStringLiteral("从首页、搜索或收藏进入作品后，这里显示详情与章节。"));
    favBtn_->hide();
    BuildChapters();
    UpdateCover();
    ApplyTopLayout();   // 构造期宽度还没定，先按 0 宽给堆叠态，首次 resize 会纠正

    // 真实封面是异步的：Get() 先给占位图，下载完成后再回填一次。
    connect(&CoverCache::I(), &CoverCache::Loaded, this, [this](const QString &key) {
        if (key == QStringLiteral("detail:%1").arg(c_.id))
            UpdateCover();
    });
}

void DetailPage::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (width() <= 0 || topCheckPending_)
        return;
    // 与 SettingRow 同一套做法：resizeEvent 里视口/页边距还没落到最终值，
    // 延后到本轮布局结束再按稳定宽度判一次，避免"先判成堆叠就再也翻不回来"。
    topCheckPending_ = true;
    QTimer::singleShot(0, this, [this] {
        topCheckPending_ = false;
        ApplyTopLayout();
    });
}

void DetailPage::ApplyTopLayout()
{
    if (!topLayout_ || !cover_ || !desc_)
        return;
    // 可用宽度 = 滚动视口宽 - 内容左右页边距。门槛与 Python 版一致：>=600dp 才左右排。
    // 注意这里必须用页面/视口的真实宽度，不能用主屏宽：见构造函数里的注释。
    const int vp = (scroll_ && scroll_->viewport()->width() > 0) ? scroll_->viewport()->width()
                                                                 : width();
    // 页边距取 body 布局的实际值（构造期按当时的 touch 模式算过一次，不能再猜）。
    int margins = 2 * BodyMargin();
    if (body_ && body_->layout())
        margins = body_->layout()->contentsMargins().left()
                  + body_->layout()->contentsMargins().right();
    const int avail = vp - margins;
    const bool side = avail >= UiStyle::Fpx(600);
    if (topApplied_ && side == topSide_)
        return;
    topApplied_ = true;
    topSide_ = side;

    topLayout_->setDirection(side ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    topLayout_->setSpacing(side ? 22 : 14);
    cover_->setFixedSize(side ? QSize(180, 240) : QSize(120, 160));
    // 堆叠态封面居中；横排态贴顶，别让 QBoxLayout 把固定尺寸的封面竖向拉伸。
    topLayout_->setAlignment(cover_, side ? Qt::AlignTop : Qt::AlignHCenter);
    // 简介在横排态限宽（行宽 620 左右最易读）；堆叠态必须放开通栏，否则右侧留一条空白。
    desc_->setMaximumWidth(side ? 620 : QWIDGETSIZE_MAX);
    // 这是"页面被撑宽"的定位锚点：视口宽、可用宽、当前模式三个数一起看。
    qWarning("detail-layout: vp=%d avail=%d side=%d", vp, avail, int(side));
}

void DetailPage::UpdateMeta()
{
    // 列表接口不带章节数：c_.chapters == 0 表示"还没取到"，不能写成
    // "更新至 0 话"。/album 成功后 SetAlbum() 写入真实值再调本函数。
    QStringList parts;
    if (!c_.status.isEmpty())
        parts << c_.status;
    if (c_.chapters > 0)
        parts << QStringLiteral("更新至 %1 话").arg(c_.chapters);
    if (!c_.updated.isEmpty())
        parts << c_.updated;
    if (!meta_)
        return;

    const QString tail = parts.join(QStringLiteral(" · "));
    const QString author = c_.author.trimmed();
    if (author.isEmpty()) {
        // 没有作者（列表占位 / 接口没给）就整段退化成纯文本：不留一个点了没反应的锚点
        meta_->setTextFormat(Qt::PlainText);
        meta_->setTextInteractionFlags(Qt::NoTextInteraction);
        meta_->setText(tail);
        return;
    }

    // 作者名做成行内链接：点它按作者名搜其全部作品（Python BookInfoView 的
    // autorList 点击行为）。用富文本锚点而不是另起一行的按钮 —— 作者名再长也只是
    // 一行会 wordWrap 的文字；QPushButton 的 minimumSizeHint 会把整页最小宽度
    // 顶出屏幕（标题栏那次的教训，见 title_ 的注释）。
    meta_->setTextFormat(Qt::RichText);
    meta_->setTextInteractionFlags(Qt::LinksAccessibleByMouse);
    meta_->setText(QStringLiteral("<a href=\"jm-author\" style=\"color:%1; text-decoration:none;\">%2</a>%3%4")
                       .arg(UiTheme::I().Tokens().accent.name(), author.toHtmlEscaped(),
                            tail.isEmpty() ? QString() : QStringLiteral(" · "),
                            tail.toHtmlEscaped()));
}

void DetailPage::ShowComic(const Comic &c)
{
    c_ = c;
    chapters_.clear();      // 换作品必须清掉上一本的真实章节
    currentChapter_ = 0;    // "本话"跟着作品走，不能把上一本的章节号带过来
    coverUrl_ = c.coverUrl;
    title_->setText(c.title);
    UpdateMeta();
    desc_->setText(c.desc);
    // 先把上一本的标签清掉，再按卡片带入的分类重建；不重建会把上一本的标签
    // 留在页面上一直到 /album 回来 —— 现在标签可点，那等于点错作品的关键词。
    BuildTags();
    favBtn_->show();
    favBtn_->setText(c.favorite ? QStringLiteral("已收藏") : QStringLiteral("收藏"));
    cover_->setPixmap(RoundedPixmap(
        QPixmap::fromImage(CoverArt::Render(c.title, QSize(180, 240), int(qHash(c.title) & 0x7fffffff))
                               .toImage()),
        12));
    UpdateCover();

    if (auto *lay = qobject_cast<QVBoxLayout *>(chapterHost_->layout())) {
        while (QLayoutItem *item = lay->takeAt(0)) {
            if (QWidget *w = item->widget())
                w->deleteLater();
            delete item;
        }
        delete lay;
    }
    BuildChapters();

    // 章节列表来自 /album：请求在路上时提示"加载中"，避免误读成"没有章节"
    if (!c.id.isEmpty()) {
        if (auto *hint = chapterHeader_->findChild<QLabel *>(QStringLiteral("SectionHint")))
            hint->setText(QStringLiteral("加载中…"));
    }

    // 章节按钮会抢焦点，QScrollArea 会因此自动滚到第一个按钮，
    // 表现为"页面头部被顶到标题栏下面"。这里显式回到顶部。
    QTimer::singleShot(0, this, [this] {
        if (auto *area = findChild<QScrollArea *>())
            area->verticalScrollBar()->setValue(0);
    });
}

void DetailPage::SetAlbum(const JmAlbum &a)
{
    // /album 回来时用户可能已经切到别的作品：id 不一致就丢弃。
    if (!c_.id.isEmpty() && !a.id.isEmpty() && a.id != c_.id)
        return;

    if (!a.name.isEmpty())
        c_.title = a.name;
    if (!a.author.isEmpty())
        c_.author = a.author;
    if (!a.description.isEmpty())
        c_.desc = a.description;
    else if (c_.desc.isEmpty() || c_.desc == QStringLiteral("正在读取作品信息…"))
        c_.desc = QStringLiteral("暂无简介");   // 接口已返回但没有简介：不能继续显示"读取中"
    if (!a.tags.isEmpty())
        c_.tags = a.tags;
    chapters_.clear();
    for (const JmChapter &jch : a.chapters) {
        Chapter ch;
        ch.id = jch.id;
        ch.name = jch.name.isEmpty() ? QStringLiteral("第 %1 话").arg(chapters_.size() + 1)
                                     : jch.name;
        chapters_.append(ch);
    }
    c_.chapters = chapters_.size();

    // 详情页的封面优先用真实图；拿不到再保留卡片带入的 URL。
    if (!a.cover.isEmpty())
        coverUrl_ = a.cover;

    title_->setText(c_.title);
    UpdateMeta();
    desc_->setText(c_.desc);

    // 标签：真实数据才有，重建一行（每个标签可点，点了按标签名搜索）。
    BuildTags();

    if (chapterHeader_) {
        if (auto *hint = chapterHeader_->findChild<QLabel *>(QStringLiteral("SectionHint")))
            hint->setText(QStringLiteral("共 %1 话").arg(chapters_.size()));
    }

    // 章节从"加载中"换成真实目录：整块交叉淡出，别让十几行按钮一次性跳出来
    Motion::CrossFade(chapterHost_, [this] {
        if (auto *lay = qobject_cast<QVBoxLayout *>(chapterHost_->layout())) {
            while (QLayoutItem *item = lay->takeAt(0)) {
                if (QWidget *w = item->widget())
                    w->deleteLater();
                delete item;
            }
            delete lay;
        }
        BuildChapters();
    });
    UpdateCover();
    QTimer::singleShot(0, this, [this] {
        if (auto *area = findChild<QScrollArea *>())
            area->verticalScrollBar()->setValue(0);
    });

    std::fprintf(stderr, "detail-real: id=%s chapters=%d cover=%s\n", qPrintable(c_.id),
                 int(chapters_.size()), coverUrl_.isEmpty() ? "-" : qPrintable(coverUrl_));
    std::fflush(stderr);
}

void DetailPage::BuildTags()
{
    if (!tagRow_ || !tagFlow_)
        return;
    // 换作品 / 换标签都整行重建：旧 chip 要连 widget 一起删，否则会叠出两排
    while (QLayoutItem *item = tagFlow_->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    const QStringList tags = c_.tags.mid(0, 6);
    for (const QString &tag : tags) {
        // 非复选：点一下就是一次搜索，不该在详情页留下"已选中"的状态
        auto *chip = new Chip(tag, false, tagRow_);
        chip->setToolTip(QStringLiteral("搜索标签“%1”").arg(tag));
        connect(chip, &QPushButton::clicked, this, [this, tag] {
            // Python ClickTagsItem：text.split("#", 2)，含 "#" 只取后半段。
            // C++ 侧的标签来自 /album，本身不带分组前缀；"作品#xxx / 登场人物#xxx"
            // 这类被拼出来的标签按同一规则切一次，两边行为保持一致。
            const int hash = tag.indexOf(QLatin1Char('#'));
            const QString keyword = (hash >= 0 ? tag.mid(hash + 1) : tag).trimmed();
            if (keyword.isEmpty())
                return;
            emit SearchRequested(keyword);
        });
        tagFlow_->addWidget(chip);
    }

    // 没有标签就整行隐藏：不留一块空白（与改造前一致）
    tagRow_->setVisible(!tags.isEmpty());
    tagFlow_->invalidate();
    tagRow_->updateGeometry();
}

void DetailPage::SetFavorite(bool on)
{
    c_.favorite = on;
    if (favBtn_)
        favBtn_->setText(on ? QStringLiteral("已收藏") : QStringLiteral("收藏"));
}

int DetailPage::ChapterCount() const
{
    return chapters_.size();
}

QString DetailPage::ChapterIdAt(int index) const
{
    if (index < 0 || index >= chapters_.size())
        return QString();
    return chapters_.at(index).id;
}

QString DetailPage::ChapterNameAt(int index) const
{
    if (index < 0 || index >= chapters_.size())
        return QString();
    return chapters_.at(index).name;
}

void DetailPage::UpdateCover()
{
    if (!cover_)
        return;
    if (c_.id.isEmpty()) {
        // 未选择作品：不画生成封面，避免看起来像"已经有一部作品"
        cover_->clear();
        cover_->hide();
        return;
    }
    cover_->show();
    const QSize size = cover_->size();
    const QString shownKey = QStringLiteral("%1|%2").arg(c_.id, coverUrl_);
    const bool changed = shownKey != lastCoverShown_;
    lastCoverShown_ = shownKey;
    if (coverUrl_.isEmpty()) {
        cover_->setPixmap(RoundedPixmap(
            QPixmap::fromImage(CoverArt::Render(c_.title, QSize(180, 240),
                                                int(qHash(c_.title) & 0x7fffffff))
                                   .toImage()),
            12));
    } else {
        const QString key = QStringLiteral("detail:%1").arg(c_.id);
        QPixmap pm = CoverCache::I().Get(key, c_.title, coverUrl_, size);
        cover_->setPixmap(RoundedPixmap(pm, 12));
    }
    // 封面是异步到的（先占位、再真图）：换图时淡入一次，避免"啪"地换一张
    if (changed)
        Motion::FadeIn(cover_, Motion::Base);
}

// ---------------------------------------------------------------- 章节网格
namespace {

// 章节瓷砖的版式常量。改这里必须同步 theme.cpp 里
// `QPushButton#Ghost[chapTile="true"]` 的 padding / height：
// 一处算可用文字宽度、一处画最终高度，两边对不上就会"字被裁"或"行高跳动"。
constexpr int kChapGap = 6;       // 格子间距（不随字号缩放，qss 也不缩放 padding）
constexpr int kChapPadH = 6;      // qss 的左右 padding
constexpr int kChapFontPx = 12;   // qss 的 font-size（该值会被 ScaleFontPx 按字号缩放）
constexpr int kChapMinCols = 4;   // 移动端硬要求：一行至少 4 个
constexpr int kChapMaxCols = 8;   // 桌面/平板别把章节铺成一墙小方块
constexpr int kChapCellMin = 44;  // 单格宽度下限：再窄手指点不准，也放不下编号

// 章节目录的等宽网格。
//
// 原来每块是 setFixedSize(200, 34)：363dp 竖屏去掉页边距后可用宽度只有 ~339，
// 一块就吃掉 200，第二块必然换行 —— 一行只出一个，整本几百话要滚几十屏。
// 这里改成"按当前可用宽度反算列宽"：
//   * 列数下限 4（移动端要求），上限 8（大屏不铺成密集小方块）；
//   * 格宽取整除结果，行尾最多剩 cols-1 px，永远不会溢出/错位；
//   * 宽度变化时只重排宽度与文字省略，不重建按钮（900 话也不卡）。
class ChapterGrid : public QWidget
{
public:
    explicit ChapterGrid(QWidget *parent = nullptr) : QWidget(parent)
    {
        flow_ = new FlowLayout(this, kChapGap);
        // 字号/主题变了：格宽可能不变（例如都被钳在 4 列），但省略宽度要按新字号重算。
        connect(&UiTheme::I(), &UiTheme::Changed, this, [this] {
            lastCell_ = -1;
            Relayout();
        });
    }

    void SetPickHandler(std::function<void(int)> fn) { pick_ = fn; }

    void SetChapters(const QVector<Chapter> &chapters)
    {
        Clear();
        chapters_ = chapters;
        buttons_.reserve(chapters_.size());
        for (int i = 0; i < chapters_.size(); ++i) {
            auto *btn = new QPushButton(this);
            btn->setObjectName(QStringLiteral("Ghost"));
            // 造型/配色/高度全部交给 theme.cpp 的 #Ghost[chapTile]：
            // 一话一份 setStyleSheet 在几百话时既慢、换主题也不会跟着变。
            btn->setProperty("chapTile", true);
            btn->setProperty("chapRead", chapters_.at(i).read);
            // 触摸模式的 44dp 兜底会把"未布局控件"的 min/max 改写成 100x44，
            // 直接把网格打乱；这里的宽度是算出来的，必须自己说了算。
            btn->setProperty("hitGrow", false);
            btn->setCursor(Qt::PointingHandCursor);
            btn->setToolTip(chapters_.at(i).name);
            connect(btn, &QPushButton::clicked, this, [this, i] {
                if (pick_)
                    pick_(i);
            });
            flow_->addWidget(btn);
            buttons_.append(btn);
        }
        lastCell_ = -1;   // 换作品/换目录必须重算文字省略
        Relayout();
    }

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        Relayout();
    }

private:
    void Clear()
    {
        while (QLayoutItem *item = flow_->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        buttons_.clear();
        chapters_.clear();
    }

    void Relayout()
    {
        if (buttons_.isEmpty())
            return;
        const int avail = width();
        if (avail <= 0)
            return;

        int cols = (avail + kChapGap) / (PreferredCell() + kChapGap);
        cols = qBound(kChapMinCols, cols, kChapMaxCols);
        int cell = (avail - kChapGap * (cols - 1)) / cols;
        if (cell < kChapCellMin) {
            // 极窄（桌面窗口被拖到 <200px）：先保证不溢出，再按 44px 硬下限减列。
            cols = qMax(1, (avail + kChapGap) / (kChapCellMin + kChapGap));
            cell = (avail - kChapGap * (cols - 1)) / cols;
            cell = qBound(1, cell, avail);
        }
        if (cell == lastCell_)
            return;
        lastCell_ = cell;

        // 省略宽度必须按 qss 的实际字号算（font-size 是 px，且会被字号缩放），
        // 不能用 buttons_.first()->font() 的磅值：那是应用默认字号，会多省掉几个字。
        // 基座 QPushButton 还有 font-weight: bold，量宽时也要带上，否则粗体会再被裁一点。
        QFont f = buttons_.first()->font();
        f.setPixelSize(qMax(10, qRound(kChapFontPx * UiStyle::FontScale())));
        f.setWeight(QFont::Bold);
        const QFontMetrics fm(f);
        const int inner = qMax(1, cell - 2 * kChapPadH - 2);   // 左右 padding + 1px 边框×2
        for (int i = 0; i < buttons_.size(); ++i) {
            QPushButton *btn = buttons_.at(i);
            btn->setFixedWidth(cell);
            // 章节名可能是"[汉化组] 第 12 话 xxx"：放不下就省略，
            // 完整名字留在 tooltip；不省略会被按钮硬裁，末尾半个字更难看。
            btn->setText(fm.elidedText(chapters_.at(i).name, Qt::ElideRight, inner));
        }
        flow_->invalidate();
        updateGeometry();
    }

    static int PreferredCell()
    {
        // 76px ≈ "第 1234 话"在 12px 字号下的自然宽度（~58）+ 左右 padding。
        // 宽度够放下一个首选格就多排一列，不够就退回 kChapMinCols 列的等分。
        return qMax(kChapCellMin + 2, qRound(76 * UiStyle::FontScale()));
    }

    FlowLayout *flow_ = nullptr;
    QVector<Chapter> chapters_;
    QVector<QPushButton *> buttons_;
    std::function<void(int)> pick_;
    int lastCell_ = -1;   // 上一次算出的格宽，宽度没变就不重复设置文字
};

}  // namespace

void DetailPage::BuildChapters()
{
    auto *lay = new QVBoxLayout(chapterHost_);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(8);

    if (chapters_.isEmpty()) {
        // 章节只来自 /album。没拿到就显示空态，不生成任何章节按钮。
        lay->addWidget(new EmptyState(QStringLiteral("暂无章节"),
                                      QStringLiteral("章节列表来自作品详情接口，\n加载完成后会显示在这里。"),
                                      QString(), chapterHost_));
        return;
    }

    auto *grid = new ChapterGrid(chapterHost_);
    grid->SetPickHandler([this](int i) {
        currentChapter_ = i;
        emit StartReading(c_, i);
    });
    grid->SetChapters(chapters_);
    lay->addWidget(grid);
}
