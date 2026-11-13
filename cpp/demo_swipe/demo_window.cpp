#include "demo_window.h"

#include "demo_widgets.h"
#include "swipe_pager.h"

#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

namespace {

const QColor kBg(0x31, 0x36, 0x3b);
const QColor kBgAlt(0x2a, 0x2e, 0x32);
const QColor kSurface(0x3a, 0x40, 0x45);
const QColor kSurfaceHover(0x44, 0x4b, 0x51);
const QColor kBorder(0x23, 0x26, 0x29);
const QColor kText(0xff, 0xff, 0xff);
const QColor kTextDim(0xb6, 0xbe, 0xc2);
const QColor kTextFaint(0x7d, 0x86, 0x8b);
const QColor kAccent(0x1d, 0xe9, 0xb6);

const QStringList kSections = {QStringLiteral("最新上传"), QStringLiteral("推荐"),
                               QStringLiteral("同人"), QStringLiteral("单本")};
const QStringList kModules = {QStringLiteral("首页"), QStringLiteral("搜索"),
                              QStringLiteral("分类"), QStringLiteral("收藏"),
                              QStringLiteral("设置")};

QLabel *Label(const QString &text, int px, const QColor &color, QWidget *parent, bool bold = false)
{
    auto *l = new QLabel(text, parent);
    l->setStyleSheet(QStringLiteral("color:%1;font-size:%2px;%3background:transparent;")
                         .arg(color.name())
                         .arg(px)
                         .arg(bold ? QStringLiteral("font-weight:600;") : QString()));
    return l;
}

QFrame *HLine(QWidget *parent)
{
    auto *line = new QFrame(parent);
    line->setFixedHeight(1);
    line->setStyleSheet(QStringLiteral("background:%1;").arg(kBorder.name()));
    return line;
}

QWidget *MakeHeader(const QString &title, const QString &hint, QWidget *parent)
{
    auto *host = new QWidget(parent);
    auto *h = new QVBoxLayout(host);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(0);
    auto *row = new QWidget(host);
    row->setFixedHeight(56);
    auto *l = new QHBoxLayout(row);
    l->setContentsMargins(16, 0, 16, 0);
    l->setSpacing(8);
    l->addWidget(Label(title, 17, kText, row, true));
    l->addWidget(Label(QStringLiteral("DEMO"), 10, kAccent, row));
    l->addStretch(1);
    if (!hint.isEmpty())
        l->addWidget(Label(hint, 11, kTextFaint, row));
    h->addWidget(row);
    h->addWidget(HLine(host));
    return host;
}

QPushButton *SmallButton(const QString &text, bool accent, QWidget *parent)
{
    auto *b = new QPushButton(text, parent);
    b->setCursor(Qt::PointingHandCursor);
    b->setFixedHeight(26);
    b->setFocusPolicy(Qt::NoFocus);
    b->setStyleSheet(
        QStringLiteral("QPushButton{background:%1;border:1px solid %2;color:%3;"
                       "font-size:11px;padding:0 8px;}"
                       "QPushButton:hover{background:%4;}")
            .arg(accent ? kSurface.name() : kSurface.name(),
                 accent ? kAccent.name() : kBorder.name(),
                 accent ? kAccent.name() : kTextDim.name(), kSurfaceHover.name()));
    return b;
}

// 卡片网格所在的滚动容器：纵向可滚（滚轮/内容），横向恒关
QScrollArea *ScrollHost(QWidget *inner, QWidget *parent)
{
    auto *sa = new QScrollArea(parent);
    sa->setWidgetResizable(true);
    sa->setFrameShape(QFrame::NoFrame);
    sa->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    sa->viewport()->setAutoFillBackground(false);
    sa->setStyleSheet(QStringLiteral("QScrollArea{background:transparent;border:0;}"));
    inner->setStyleSheet(QStringLiteral("background:transparent;"));
    sa->setWidget(inner);
    return sa;
}

}  // namespace

DemoWindow::DemoWindow(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("DemoRoot"));
    setWindowTitle(QStringLiteral("JM 首页横向滑动 · 独立 DEMO（未改动工程文件）"));
    resize(468, 884);
    setMinimumSize(380, 600);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(BuildBody(), 1);

    auto *bar = BuildDebugBar();
    root->addWidget(bar);

    setStyleSheet(QStringLiteral("QWidget#DemoRoot{background:%1;}").arg(kBg.name()));

    debugTimer_ = new QTimer(this);
    debugTimer_->setInterval(60);
    connect(debugTimer_, &QTimer::timeout, this, &DemoWindow::UpdateDebug);
    debugTimer_->start();

    toastTimer_ = new QTimer(this);
    toastTimer_->setSingleShot(true);
    toastTimer_->setInterval(1100);
    connect(toastTimer_, &QTimer::timeout, this, [this] {
        if (toast_)
            toast_->hide();
    });

    UpdateDebug();
}

QWidget *DemoWindow::BuildBody()
{
    auto *body = new QWidget(this);
    auto *row = new QHBoxLayout(body);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);

    rail_ = new NavRail(body);
    rail_->SetItems(kModules);
    row->addWidget(rail_);

    modules_ = new SlidePager(body);
    modules_->SetSlideDuration(240);   // Motion::Slow / Page：模块与分区同一条时间轴
    modules_->SetCrossFadeDuration(180);   // 对照：现状 NavMotion::Lateral 用 Motion::Base
    row->addWidget(modules_, 1);

    modules_->AddPage(BuildHome());
    modules_->AddPage(BuildSearch());
    modules_->AddPage(BuildCategory());
    modules_->AddPage(BuildFavorites());
    modules_->AddPage(BuildSettings());

    connect(rail_, &NavRail::Activated, this, [this](int idx) {
        if (modules_)
            modules_->SetCurrent(idx, true);
    });
    connect(modules_, &SlidePager::ProgressChanged, this, [this](qreal p) {
        if (rail_)
            rail_->SetState(modules_->Current(), p);
    });
    connect(modules_, &SlidePager::CurrentChanged, this, [this](int idx) {
        if (rail_)
            rail_->SetState(idx, 0.0);
    });
    connect(modules_, &SlidePager::SwipeRejected, this, [this](const QString &why) {
        Toast(why);
    });
    return body;
}

QWidget *DemoWindow::BuildHome()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(MakeHeader(QStringLiteral("首页"),
                            QStringLiteral("←  左右滑动切换分区  →"), page));

    strip_ = new DemoStrip(page);
    strip_->SetItems(kSections);
    v->addWidget(strip_);

    sections_ = new SlidePager(page);
    sections_->SetSlideDuration(240);
    sections_->SetCrossFadeDuration(120);   // 对照：现状 Motion::CrossFade(Fast)

    for (int i = 0; i < kSections.size(); ++i) {
        auto *inner = new QWidget;
        auto *iv = new QVBoxLayout(inner);
        iv->setContentsMargins(14, 12, 14, 12);
        iv->setSpacing(10);
        iv->addWidget(Label(QStringLiteral("%1 · 示例 8 部（假数据）").arg(kSections.at(i)), 11,
                            kTextFaint, inner));

        auto *grid = new QGridLayout;
        grid->setSpacing(10);
        for (int k = 0; k < 8; ++k) {
            auto *card = new DemoCard(
                QStringLiteral("%1-%2").arg(kSections.at(i).left(1)).arg(k + 1, 2, 10, QChar('0')),
                QStringLiteral("示例作品 %1").arg(k + 1, 2, 10, QChar('0')), 168 + i * 42 + k * 7,
                DemoCard::Wide, inner);
            connect(card, &DemoCard::Activated, this, [this](const QString &t) {
                Toast(QStringLiteral("点击卡片：%1").arg(t));
            });
            grid->addWidget(card, k / 2, k % 2);
        }
        iv->addLayout(grid);
        iv->addStretch(1);

        sections_->AddPage(ScrollHost(inner, sections_));
    }
    v->addWidget(sections_, 1);

    connect(strip_, &DemoStrip::Activated, this, [this](int idx) {
        // 点分区栏 = 程序切换：时长恒 240ms，方向由索引差决定
        if (sections_)
            sections_->SetCurrent(idx, true);
    });
    connect(sections_, &SlidePager::ProgressChanged, this, [this](qreal p) {
        if (strip_)
            strip_->SetState(sections_->Current(), p);
        if (rail_ && modules_)
            rail_->SetState(modules_->Current(), modules_->Progress());
    });
    connect(sections_, &SlidePager::CurrentChanged, this, [this](int idx) {
        if (strip_)
            strip_->SetState(idx, 0.0);
    });
    return page;
}

QWidget *DemoWindow::BuildSearch()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(MakeHeader(QStringLiteral("搜索"), QStringLiteral("示例页：整页一起滑动"), page));

    auto *body = new QWidget(page);
    auto *bv = new QVBoxLayout(body);
    bv->setContentsMargins(14, 14, 14, 14);
    bv->setSpacing(12);

    auto *box = new QLineEdit(body);
    box->setPlaceholderText(QStringLiteral("搜索作品 / 作者"));
    box->setFixedHeight(36);
    box->setStyleSheet(QStringLiteral("QLineEdit{background:%1;border:1px solid %2;color:%3;"
                                      "padding:0 10px;font-size:13px;}"
                                      "QLineEdit:focus{border-color:%4;}")
                           .arg(kSurface.name(), kBorder.name(), kText.name(), kAccent.name()));
    bv->addWidget(box);

    bv->addWidget(Label(QStringLiteral("热门搜索"), 12, kTextDim, body));
    auto *chips = new QWidget(body);
    auto *ch = new QHBoxLayout(chips);
    ch->setContentsMargins(0, 0, 0, 0);
    ch->setSpacing(8);
    for (const QString &kw : {QStringLiteral("标签一"), QStringLiteral("标签二"),
                              QStringLiteral("标签三"), QStringLiteral("标签四")}) {
        auto *chip = Label(kw, 11, kAccent, chips);
        chip->setStyleSheet(QStringLiteral("color:%1;background:rgba(29,233,182,28);"
                                           "border:1px solid rgba(29,233,182,70);"
                                           "font-size:11px;padding:5px 10px;")
                                .arg(kAccent.name()));
        ch->addWidget(chip);
    }
    ch->addStretch(1);
    bv->addWidget(chips);

    for (int i = 0; i < 6; ++i) {
        auto *row = new QWidget(body);
        auto *rh = new QHBoxLayout(row);
        rh->setContentsMargins(0, 6, 0, 6);
        rh->setSpacing(10);
        auto *thumb = new QWidget(row);
        thumb->setFixedSize(38, 50);
        thumb->setStyleSheet(
            QStringLiteral("background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 %1,stop:1 %2);")
                .arg(QColor::fromHsl((190 + i * 23) % 360, 90, 74).name(),
                     QColor::fromHsl((210 + i * 23) % 360, 100, 44).name()));
        rh->addWidget(thumb);
        auto *col = new QVBoxLayout;
        col->setContentsMargins(0, 0, 0, 0);
        col->setSpacing(4);
        col->addWidget(Label(QStringLiteral("示例结果 %1").arg(i + 1), 13, kText, row));
        col->addWidget(Label(QStringLiteral("作者 · 24 话"), 11, kTextFaint, row));
        rh->addLayout(col, 1);
        rh->addWidget(Label(QStringLiteral("→"), 13, kTextFaint, row));
        bv->addWidget(row);
        bv->addWidget(HLine(body));
    }
    bv->addStretch(1);
    v->addWidget(ScrollHost(body, page), 1);
    return page;
}

QWidget *DemoWindow::BuildCategory()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(MakeHeader(QStringLiteral("分类"), QStringLiteral("示例页"), page));

    auto *body = new QWidget(page);
    auto *bv = new QVBoxLayout(body);
    bv->setContentsMargins(14, 14, 14, 14);
    bv->setSpacing(10);
    auto *grid = new QGridLayout;
    grid->setSpacing(10);
    for (int i = 0; i < 6; ++i) {
        auto *tile = new QWidget(body);
        tile->setFixedHeight(92);
        tile->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;")
                                .arg(kSurface.name(), kBorder.name()));
        auto *tv = new QVBoxLayout(tile);
        tv->setContentsMargins(12, 10, 12, 10);
        tv->setSpacing(2);
        tv->addWidget(Label(QStringLiteral("分类 %1").arg(i + 1), 14, kText, tile, true));
        tv->addWidget(Label(QStringLiteral("%1 部作品").arg(120 + i * 37), 11, kTextFaint, tile));
        tv->addStretch(1);
        grid->addWidget(tile, i / 2, i % 2);
    }
    bv->addLayout(grid);
    bv->addStretch(1);
    v->addWidget(ScrollHost(body, page), 1);
    return page;
}

QWidget *DemoWindow::BuildFavorites()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(MakeHeader(QStringLiteral("收藏"), QStringLiteral("示例页"), page));

    auto *body = new QWidget(page);
    auto *bv = new QVBoxLayout(body);
    bv->setContentsMargins(14, 14, 14, 14);
    bv->setSpacing(10);
    auto *grid = new QGridLayout;
    grid->setSpacing(10);
    for (int i = 0; i < 9; ++i) {
        auto *card = new DemoCard(QStringLiteral("F-%1").arg(i + 1), QStringLiteral("收藏 %1").arg(i + 1),
                                  260 + i * 11, DemoCard::Narrow, body);
        connect(card, &DemoCard::Activated, this, [this](const QString &t) {
            Toast(QStringLiteral("点击卡片：%1").arg(t));
        });
        grid->addWidget(card, i / 3, i % 3);
    }
    bv->addLayout(grid);
    bv->addStretch(1);
    v->addWidget(ScrollHost(body, page), 1);
    return page;
}

QWidget *DemoWindow::BuildSettings()
{
    auto *page = new QWidget;
    auto *v = new QVBoxLayout(page);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);
    v->addWidget(MakeHeader(QStringLiteral("设置"), QStringLiteral("示例页"), page));

    auto *body = new QWidget(page);
    auto *bv = new QVBoxLayout(body);
    bv->setContentsMargins(14, 8, 14, 14);
    bv->setSpacing(0);
    const QStringList rows = {QStringLiteral("自动超分"), QStringLiteral("按阅读方式预加载"),
                              QStringLiteral("仅 Wi-Fi 下载"), QStringLiteral("显示页码"),
                              QStringLiteral("从右到左阅读"), QStringLiteral("记住阅读位置")};
    for (int i = 0; i < rows.size(); ++i) {
        auto *row = new QWidget(body);
        row->setFixedHeight(48);
        auto *rh = new QHBoxLayout(row);
        rh->setContentsMargins(0, 0, 0, 0);
        rh->addWidget(Label(rows.at(i), 13, kText, row));
        rh->addStretch(1);
        auto *sw = new ToggleSwitch(i % 2 == 0, row);
        connect(sw, &ToggleSwitch::Toggled, this, [this, i](bool on) {
            Toast(QStringLiteral("开关 %1：%2").arg(i + 1).arg(on ? QStringLiteral("开")
                                                                : QStringLiteral("关")));
        });
        rh->addWidget(sw);
        bv->addWidget(row);
        bv->addWidget(HLine(body));
    }
    bv->addWidget(Label(QStringLiteral("在开关上横滑不会被误触：拖过 12px 就判为手势"), 11,
                        kTextFaint, body));
    bv->addStretch(1);
    v->addWidget(ScrollHost(body, page), 1);
    return page;
}

QWidget *DemoWindow::BuildDebugBar()
{
    auto *bar = new QWidget(this);
    bar->setFixedHeight(96);
    bar->setStyleSheet(QStringLiteral("background:%1;border-top:1px solid %2;")
                           .arg(kBgAlt.name(), kBorder.name()));
    auto *v = new QVBoxLayout(bar);
    v->setContentsMargins(12, 7, 12, 7);
    v->setSpacing(4);

    params_ = Label(QString(), 11, kTextDim, bar);
    params_->setWordWrap(true);
    v->addWidget(params_);

    live_ = Label(QString(), 11, kTextFaint, bar);
    live_->setStyleSheet(QStringLiteral("color:%1;font-size:10px;font-family:Consolas,monospace;"
                                        "background:transparent;")
                             .arg(kTextFaint.name()));
    v->addWidget(live_);

    auto *row = new QWidget(bar);
    auto *rh = new QHBoxLayout(row);
    rh->setContentsMargins(0, 0, 0, 0);
    rh->setSpacing(6);
    auto *autoBtn = SmallButton(QStringLiteral("自动演示"), true, row);
    auto *edgeBtn = SmallButton(QStringLiteral("边界回弹"), false, row);
    cmpBtn_ = SmallButton(QStringLiteral("A/B 现状淡入"), false, row);
    ruleBtn_ = SmallButton(QStringLiteral("边界说明"), false, row);
    rh->addWidget(autoBtn);
    rh->addWidget(edgeBtn);
    rh->addWidget(cmpBtn_);
    rh->addWidget(ruleBtn_);
    rh->addStretch(1);
    v->addWidget(row);

    connect(autoBtn, &QPushButton::clicked, this, &DemoWindow::StartAutoSequence);
    connect(edgeBtn, &QPushButton::clicked, this, &DemoWindow::EdgeReboundDemo);
    connect(cmpBtn_, &QPushButton::clicked, this, [this] { SetCompareFade(!compare_); });
    connect(ruleBtn_, &QPushButton::clicked, this, [this] { ShowRules(!RulesVisible()); });

    SetCompareFade(false);
    return bar;
}

void DemoWindow::SetCompareFade(bool on)
{
    compare_ = on;
    const auto style = on ? SlidePager::Style::CrossFade : SlidePager::Style::Slide;
    if (sections_)
        sections_->SetStyle(style);
    if (modules_)
        modules_->SetStyle(style);
    if (cmpBtn_) {
        cmpBtn_->setText(on ? QStringLiteral("A/B 现状淡入 ✓") : QStringLiteral("A/B 现状淡入"));
        cmpBtn_->setStyleSheet(
            QStringLiteral("QPushButton{background:%1;border:1px solid %2;color:%3;"
                           "font-size:11px;padding:0 8px;}"
                           "QPushButton:hover{background:%4;}")
                .arg(kSurface.name(), on ? kAccent.name() : kBorder.name(),
                     on ? kAccent.name() : kTextDim.name(), kSurfaceHover.name()));
    }
    if (params_) {
        params_->setText(
            on ? QStringLiteral("对照现状：分区 = CrossFade 120ms（Motion::Fast）· 模块 = Fade 180ms"
                                "（Motion::Base）· 曲线 InOutQuad · 无手势跟手")
               : QStringLiteral("本次方案：分区 / 模块 同为 240ms + OutCubic · 跟手无缓动 · 离手"
                                "阈值 28% 或 0.35px/ms · 轴锁 12px · 边界橡皮筋 ×0.32"));
    }
}

void DemoWindow::UpdateDebug()
{
    if (!live_)
        return;
    SlidePager *active = (modules_ && modules_->Busy()) ? modules_ : sections_;
    const QString phase = active ? active->PhaseName() : QStringLiteral("-");
    const qreal sp = sections_ ? sections_->Progress() : 0.0;
    const qreal mp = modules_ ? modules_->Progress() : 0.0;
    live_->setText(QStringLiteral("分区 %1/%2 p=%3 %4 ｜ 模块 %5/%6 p=%7 ｜ %8ms")
                       .arg(sections_ ? sections_->Current() + 1 : 0)
                       .arg(sections_ ? sections_->Count() : 0)
                       .arg(sp, 0, 'f', 2)
                       .arg(phase)
                       .arg(modules_ ? modules_->Current() + 1 : 0)
                       .arg(modules_ ? modules_->Count() : 0)
                       .arg(mp, 0, 'f', 2)
                       .arg(active ? active->LastFrameMs() : 0.0, 0, 'f', 1));
}

void DemoWindow::Toast(const QString &text)
{
    if (!toast_) {
        toast_ = new QLabel(this);
        toast_->setStyleSheet(QStringLiteral("background:rgba(12,15,17,232);color:#ffffff;"
                                             "border:1px solid %1;font-size:12px;padding:7px 12px;")
                                  .arg(kAccent.name()));
        toast_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }
    toast_->setText(text);
    toast_->adjustSize();
    toast_->move((width() - toast_->width()) / 2,
                 height() - 96 - 44 - toast_->height());
    toast_->show();
    toast_->raise();
    toastTimer_->start();
}

void DemoWindow::StartAutoSequence()
{
    if (!sections_ || !modules_)
        return;
    ShowRules(false);
    sections_->SetCurrent(0, false);
    modules_->SetCurrent(0, false);

    int t = 150;
    auto at = [this](int ms, const std::function<void()> &fn) { QTimer::singleShot(ms, this, fn); };

    at(t, [this] { sections_->StartAutoSwipe(-270, 0, 220); });           // 快甩：速度提交
    t += 700;
    at(t, [this] { sections_->StartAutoSwipe(-270, 0, 200); });
    t += 700;
    at(t, [this] { sections_->StartAutoSwipe(320, 0, 300); });             // 慢拖 320px/300ms：距离提交
    t += 780;
    at(t, [this] { modules_->SetCurrent(1, true); });                      // 相邻模块
    t += 620;
    at(t, [this] { modules_->SetCurrent(2, true); });
    t += 620;
    at(t, [this] { modules_->SetCurrent(4, true); });                      // 非相邻：整页滑一屏
    t += 620;
    at(t, [this] { modules_->SetCurrent(0, true); });
    t += 640;
    at(t, [this] { sections_->SetCurrent(3, true); });                     // 点分区栏 = 程序切换
    t += 620;
    at(t, [this] { sections_->SetCurrent(0, true); });
}

void DemoWindow::EdgeReboundDemo()
{
    if (!sections_)
        return;
    ShowRules(false);
    sections_->SetCurrent(sections_->Count() - 1, false);
    Toast(QStringLiteral("已在最后一个分区：继续左滑只回弹，不会跳到别的模块"));
    QTimer::singleShot(90, this, [this] { sections_->StartAutoSwipe(-280, 0, 220); });
}

void DemoWindow::BuildRulesOverlay()
{
    rules_ = new QWidget(this);
    rules_->setStyleSheet(QStringLiteral("background:rgba(10,12,13,236);"));
    auto *v = new QVBoxLayout(rules_);
    v->setContentsMargins(18, 18, 18, 18);
    v->setSpacing(10);

    auto *card = new QWidget(rules_);
    card->setStyleSheet(QStringLiteral("background:%1;border:1px solid %2;")
                            .arg(kBgAlt.name(), kBorder.name()));
    auto *cv = new QVBoxLayout(card);
    cv->setContentsMargins(16, 14, 16, 14);
    cv->setSpacing(8);
    cv->addWidget(Label(QStringLiteral("分区 / 模块 切换边界"), 15, kText, card, true));

    auto *text = new QLabel(card);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setStyleSheet(QStringLiteral("color:%1;font-size:12px;background:transparent;")
                            .arg(kTextDim.name()));
    text->setText(QStringLiteral(
        "<style>td{padding:3px 6px 3px 0;}</style><table>"
        "<tr><td><b>1 分区首/尾</b></td><td>橡皮筋回弹（位移 ×0.32），<b>不</b>级联到模块切换</td></tr>"
        "<tr><td><b>2 分区栏</b></td><td>横滑仍是滚动标签；点按 = 整页滑到该分区</td></tr>"
        "<tr><td><b>3 纵向手势</b></td><td>12px 内判轴，|dy| 占优即整段放弃横滑</td></tr>"
        "<tr><td><b>4 模块首/尾</b></td><td>同上橡皮筋，不绕回</td></tr>"
        "<tr><td><b>5 非相邻模块</b></td><td>按方向整页滑一屏，不做多屏连滑</td></tr>"
        "<tr><td><b>6 层级页</b></td><td>详情/阅读仍用 Push/Pop，不并入本次改动</td></tr>"
        "<tr><td><b>7 动画中按下</b></td><td>从当前进度接管，不回到 0 重播</td></tr>"
        "<tr><td><b>8 一次手势</b></td><td>最多一页；拖过 12px 不再触发子控件点击</td></tr>"
        "<tr><td><b>9 关动效</b></td><td>时长归 0，逻辑与落点完全不变</td></tr>"
        "</table>"));
    cv->addWidget(text);

    auto *close = SmallButton(QStringLiteral("关闭"), true, card);
    close->setFixedHeight(30);
    connect(close, &QPushButton::clicked, this, [this] { ShowRules(false); });
    auto *row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->addStretch(1);
    row->addWidget(close);
    cv->addLayout(row);

    v->addStretch(1);
    v->addWidget(card);
    v->addStretch(1);
    rules_->hide();
    rules_->setGeometry(0, 0, width(), height() - 96);
}

void DemoWindow::ShowRules(bool on)
{
    if (!rules_)
        BuildRulesOverlay();
    if (!rules_)
        return;
    rules_->setGeometry(0, 0, width(), height() - 96);
    rules_->setVisible(on);
    if (on)
        rules_->raise();
    if (ruleBtn_)
        ruleBtn_->setText(on ? QStringLiteral("关闭说明") : QStringLiteral("边界说明"));
}

bool DemoWindow::RulesVisible() const
{
    return rules_ && rules_->isVisible();
}

void DemoWindow::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (rules_)
        rules_->setGeometry(0, 0, width(), height() - 96);
    if (toast_ && toast_->isVisible()) {
        toast_->move((width() - toast_->width()) / 2,
                     height() - 96 - 44 - toast_->height());
    }
}
