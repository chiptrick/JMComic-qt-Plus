#include "ui_pages.h"

#include "ui_main_window.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QtConcurrent>
#include <cstdio>
#include <functional>
#include <memory>

#include <QCoreApplication>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidgetAction>

#include "../reader.h"
#include "app_font.h"
#include "cache_path.h"
#include "download_manager.h"
#include "motion.h"
#include "proxy_config.h"
#include "sr_model_sheet.h"
#include "session.h"
#include "theme.h"
#include "touch.h"
#include "ui_kit.h"
#include "ui_prefs.h"

namespace {

QScrollArea *WrapScroll(QWidget *content, QWidget *parent);

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

QScrollArea *WrapScroll(QWidget *content, QWidget *parent)
{
    auto *area = new QScrollArea(parent);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setWidget(content);
    // 手机端纵向页面不该能左右滑（内容稍宽时 QScroller 会平移，滑完还不回位）
    Touch::LockHorizontalPan(area);
    return area;
}

QWidget *PageBody(QWidget *parent, int margin = -1)
{
    auto *w = new QWidget(parent);
    auto *lay = new QVBoxLayout(w);
    const int m = margin >= 0 ? margin : (Touch::IsTouchMode() ? UiStyle::Fpx(12) : 28);
    lay->setContentsMargins(m, m, m, m);
    lay->setSpacing(Touch::IsTouchMode() ? UiStyle::Fpx(10) : 16);
    return w;
}

QVBoxLayout *BodyLayout(QWidget *body)
{
    return qobject_cast<QVBoxLayout *>(body->layout());
}

QComboBox *MakeCombo(const QStringList &items, int current, QWidget *parent)
{
    auto *combo = new TouchCombo(parent);   // 触摸友好：拖动时不弹菜单
    combo->setObjectName(QStringLiteral("SettingCombo"));
    combo->addItems(items);
    combo->setCurrentIndex(qBound(0, current, items.size() - 1));
    combo->setCursor(Qt::PointingHandCursor);
    // 下拉框默认按"最长条目"算 sizeHint（字体名/主题名一长就 500+px，
    // 直接把设置页撑得比屏幕宽）。按 6 个字调整，多余的用省略号。
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMinimumContentsLength(6);
    combo->setMaximumWidth(UiStyle::Fpx(260));
    return combo;
}

const UiTokens &T() { return UiTheme::I().Tokens(); }

}  // namespace


// ============================================================ 字体设置
// 一段式解决"跟随系统 / 自定义（含可变字体轴）"：改动立即生效并写盘。
namespace {

QWidget *MakeSliderField(int min, int max, int step, int value, const QString &suffix,
                         QSlider **outSlider, QLabel **outValue, QWidget *parent)
{
    auto *host = new QWidget(parent);
    auto *lay = new QHBoxLayout(host);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);
    // TouchSlider：按在轨道上不跳值（竖滑交给页面滚动），横拖才改值；
    // 整条轨道都能起手，不再要求正好按住 12px 的手柄
    auto *slider = new TouchSlider(Qt::Horizontal, host);
    slider->setRange(min, max);
    slider->setSingleStep(step);
    slider->setPageStep(step * 4);
    slider->setValue(value);
    // 最小宽 = 设计宽度（随字号缩放），但**允许被拉长**：堆叠布局下整行都是它的，
    // 以前用 setFixedWidth 锁死在 150px，表格里就变成"滑块在左、数值被顶到行尾"，
    // 中间一大段空白——用户报的"界面被拉伸"正是这个。
    slider->setMinimumWidth(UiStyle::Fpx(Touch::IsTouchMode() ? 150 : 180));
    slider->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    slider->setCursor(Qt::PointingHandCursor);
    auto *label = new QLabel(host);
    label->setObjectName(QStringLiteral("RowMeta"));
    // 固定宽度：数值文本变长（50%→100%、9pt→12pt）时滑块宽度不该跟着抖
    label->setFixedWidth(UiStyle::Fpx(48));
    label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    label->setText(QString::number(value) + suffix);
    lay->addWidget(slider, 1);
    lay->addWidget(label, 0);
    *outSlider = slider;
    *outValue = label;
    return host;
}

}  // namespace

class FontSettingsGroup : public QWidget
{
public:
    explicit FontSettingsGroup(QWidget *parent) : QWidget(parent)
    {
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(10);

        lay->addWidget(new SectionHeader(
            QStringLiteral("字体"),
            QStringLiteral("跟随系统会随系统字体变更自动切换；自定义支持可变字体的字重/字宽轴"),
            QString(), this));

        auto *card = new QWidget(this);
        card->setObjectName(QStringLiteral("ListRow"));
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(14, 4, 14, 4);
        cl->setSpacing(0);
        lay->addWidget(card);

        source_ = MakeCombo({QStringLiteral("跟随系统"), QStringLiteral("自定义")}, 0, card);
        source_->setFixedWidth(180);
        cl->addWidget(new SettingRow(QStringLiteral("字体来源"),
                                     QStringLiteral("系统字体变化时自动同步"), source_, card));

        auto *famRow = new SettingRow(QStringLiteral("字体"),
                                      QStringLiteral("本机已安装的字体（缺中文时自动回退系统字体）"),
                                      nullptr, card);
        family_ = new TouchCombo(famRow);   // 弹窗列表：滑动只滚列表，点选才换字体
        family_->setObjectName(QStringLiteral("SettingCombo"));
        family_->setFixedWidth(240);
        family_->setMaxVisibleItems(18);
        family_->addItems(AppFont::Families());
        famRow->SetControl(family_);
        cl->addWidget(famRow);

        auto *wRow = new SettingRow(QStringLiteral("字重"),
                                    QStringLiteral("100 细 / 400 常规 / 700 粗 / 900 特粗"), nullptr,
                                    card);
        wRow->SetControl(MakeSliderField(100, 900, 25, 400, QString(), &weight_, &weightVal_, wRow));
        cl->addWidget(wRow);

        auto *wdRow = new SettingRow(QStringLiteral("字宽"),
                                     QStringLiteral("50%~200%；可变字体走 wdth 轴，普通字体按 stretch 匹配"),
                                     nullptr, card);
        wdRow->SetControl(MakeSliderField(50, 200, 5, 100, QStringLiteral("%"), &width_, &widthVal_,
                                          wdRow));
        cl->addWidget(wdRow);

        auto *szRow = new SettingRow(QStringLiteral("字号"),
                                     QStringLiteral("8~12pt，界面其余尺寸按比例跟着缩放"), nullptr,
                                     card);
        szRow->SetControl(MakeSliderField(8, 12, 1, 9, QStringLiteral("pt"), &size_, &sizeVal_, szRow));
        cl->addWidget(szRow);

        customRows_ = {famRow, wRow, wdRow, szRow};

        auto *footer = new QHBoxLayout();
        footer->setContentsMargins(0, 8, 0, 8);
        status_ = new QLabel(card);
        status_->setObjectName(QStringLiteral("SettingDesc"));
        status_->setWordWrap(true);
        status_->setMinimumWidth(0);
        footer->addWidget(status_, 1);
        auto *reset = new QPushButton(QStringLiteral("恢复默认"), card);
        reset->setObjectName(QStringLiteral("Ghost"));
        reset->setCursor(Qt::PointingHandCursor);
        footer->addWidget(reset);
        cl->addLayout(footer);

        connect(source_, &QComboBox::currentIndexChanged, this, [](int idx) {
            AppFont::I().SetSource(idx == 1 ? AppFont::Source::Custom : AppFont::Source::System);
        });
        connect(family_, &QComboBox::activated, this, [this](int) {
            AppFont::I().SetFamily(family_->currentText());
        });
        connect(weight_, &QSlider::valueChanged, this, [this](int v) {
            weightVal_->setText(QString::number(v));
            AppFont::I().SetWeight(v);
        });
        connect(width_, &QSlider::valueChanged, this, [this](int v) {
            widthVal_->setText(QString::number(v) + QStringLiteral("%"));
            AppFont::I().SetWidth(v);
        });
        connect(size_, &QSlider::valueChanged, this, [this](int v) {
            sizeVal_->setText(QString::number(v) + QStringLiteral("pt"));
            AppFont::I().SetPointSize(v);
        });
        connect(reset, &QPushButton::clicked, this, [] { AppFont::I().ResetToSystem(); });
        connect(&AppFont::I(), &AppFont::Changed, this, [this] { SyncFromConfig(); });

        SyncFromConfig();
    }

private:
    void SyncFromConfig()
    {
        const AppFont::Config &cfg = AppFont::I().Cfg();
        const bool custom = cfg.source == AppFont::Source::Custom;

        const QSignalBlocker b1(source_), b2(family_), b3(weight_), b4(width_), b5(size_);
        source_->setCurrentIndex(custom ? 1 : 0);
        // 跟随系统时这一行显示系统字体（禁用状态），切到自定义后从它起步
        const QString shown = custom ? cfg.family : AppFont::I().SystemFamily();
        int idx = family_->findText(shown);
        if (idx < 0)
            idx = family_->findText(AppFont::I().SystemFamily());   // 配置的字体不在库里
        if (idx >= 0)
            family_->setCurrentIndex(idx);
        weight_->setValue(cfg.weight);
        width_->setValue(cfg.widthPct);
        size_->setValue(cfg.pointSize);
        weightVal_->setText(QString::number(cfg.weight));
        widthVal_->setText(QString::number(cfg.widthPct) + QStringLiteral("%"));
        sizeVal_->setText(QString::number(cfg.pointSize) + QStringLiteral("pt"));

        family_->setEnabled(custom);
        for (QWidget *w : customRows_)
            w->setEnabled(custom);
        status_->setText(AppFont::I().StatusText());
    }

    QComboBox *source_ = nullptr;
    QComboBox *family_ = nullptr;
    QSlider *weight_ = nullptr;
    QSlider *width_ = nullptr;
    QSlider *size_ = nullptr;
    QLabel *weightVal_ = nullptr;
    QLabel *widthVal_ = nullptr;
    QLabel *sizeVal_ = nullptr;
    QLabel *status_ = nullptr;
    QVector<QWidget *> customRows_;
};

// ============================================================ 阅读器
namespace {

// QMenu 没有基座 qss 规则，在浅色主题的系统菜单上会出现"黑底深灰字"。
// 阅读器菜单统一按当前主题 token 上色，保证 9 个模式项可读。
//
// 尺寸必须显式覆盖基座 qss：python/res/theme/*.qss 里 `QMenu::item` 是
// "height:26px + border:8px transparent"，一条目 42px；菜单从工具条搬进十几个
// 控件后，42px/条会直接顶出手机屏高（实测 794px > 480×852 的可用高度）。
// 这里压到 ~34px/条，13 条 + 图片详情首块 ≈ 560px，竖屏也能整块显示。
void StyleReaderMenu(QMenu *menu)
{
    const UiTokens &t = T();
    menu->setStyleSheet(
        QStringLiteral("QMenu{background:%1;color:%2;border:1px solid %3;}"
                       "QMenu::item{height:22px;border:0;padding:6px 16px;}"
                       "QMenu::item:selected{background:%4;color:%2;}"
                       "QMenu::item:disabled{color:%5;}"
                       "QMenu::separator{height:1px;background:%3;margin:4px 8px;}")
            .arg(t.surface.name(), t.text.name(), t.border.name(), t.accentSoft.name(),
                 t.textFaint.name()));
}

// 首次进入看图界面的帮助浮层：点任意处关闭并落库（对应 Python ReadFrame.InitHelp）
class HelpHide : public QObject
{
public:
    explicit HelpHide(QLabel *label) : QObject(label), label_(label) {}

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (obj == label_ && ev->type() == QEvent::MouseButtonRelease) {
            // 先淡出再 hide：帮助层是整屏黑色遮罩，直接消失像"画面被切了一刀"
            UiPrefs::Set(QStringLiteral("readerHelpShown"), true);
            Motion::FadeOut(label_, Motion::Base, [this] {
                if (label_)
                    label_->hide();
            });
            return true;
        }
        return QObject::eventFilter(obj, ev);
    }

private:
    QLabel *label_ = nullptr;
};

}  // namespace

ReaderPage::ReaderPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // ---- 顶部标题栏：返回 / 标题 / 加载进度 / 菜单 四项 ----
    // 52px 定高条里原本塞了 7 个控件（返回、模式 150px、缩放、自动滚动、全屏、
    // 图片信息、菜单），窄窗口下每个都被压到点不准。现在除这四个外全部收进
    // 右上角菜单：横排只剩"一个箭头 + 可伸缩的一行字 + 一段固定字 + 一个按钮"。
    auto *bar = new QWidget(this);
    bar_ = bar;
    bar->setObjectName(QStringLiteral("TitleBar"));
    bar->setFixedHeight(52);
    auto *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(14, 8, 12, 8);
    bl->setSpacing(8);

    // 0) 返回：移动端与桌面端都保留，走与 Esc / 菜单"返回"同一条历史栈路径
    auto *backBtn = new QPushButton(bar);
    backBtn->setObjectName(QStringLiteral("WinBtn"));
    backBtn->setFixedSize(30, 30);
    backBtn->setCursor(Qt::PointingHandCursor);
    backBtn->setIcon(Glyph::Icon(Glyph::Back, 16, T().text));
    backBtn->setIconSize(QSize(16, 16));
    backBtn->setToolTip(QStringLiteral("返回 (Esc)"));
    backBtn->setProperty("hitGrow", false);
    bl->addWidget(backBtn);
    connect(backBtn, &QPushButton::clicked, this, [this] { emit RequestBack(); });

    // 1) 标题：唯一可伸缩项，放不下就右侧省略（完整标题在 tooltip）
    title_ = new ElideLabel(QStringLiteral("阅读器"), bar);
    title_->setObjectName(QStringLiteral("RowTitle"));
    bl->addWidget(title_);

    view_ = new ReaderView(this);

    // 2) 加载进度 / 页码：内容固定，宽度由文字决定，不会被标题挤掉
    pageInfo_ = new QLabel(QStringLiteral("1 / 1"), bar);
    pageInfo_->setObjectName(QStringLiteral("PageSub"));
    pageInfo_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    pageInfo_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    bl->addWidget(pageInfo_);
    // 剩余宽度留给弹簧：标题紧挨页码（桌面端观感），窄屏时由弹簧让位给省略
    bl->addStretch(1);

    // 3) 恢复原尺寸：双指/菜单改变缩放之后一键回到 100%（适宽）。
    // 放大**或缩小**都算可还原 —— 判据用 CanResetZoom()，不能用只看 >100% 的
    // IsZoomed()，否则缩小状态下按钮被禁用，点击会被 Qt 直接丢弃。
    // 缩放变化（ZoomChanged）会经 SyncChromeButtons 同步可用状态。
    fitBtn_ = new QPushButton(QStringLiteral("1:1"), bar);
    fitBtn_->setObjectName(QStringLiteral("WinBtn"));
    fitBtn_->setFixedSize(30, 30);
    fitBtn_->setCursor(Qt::PointingHandCursor);
    fitBtn_->setToolTip(QStringLiteral("恢复原尺寸（100% 适宽）"));
    fitBtn_->setProperty("hitGrow", false);
    {
        // 30x30 里塞两个数字一个冒号：按 11px 排版，跟图标按钮的视觉重量一致
        QFont f = fitBtn_->font();
        f.setPixelSize(qMax(11, UiStyle::Fpx(11)));
        f.setBold(true);
        fitBtn_->setFont(f);
    }
    bl->addWidget(fitBtn_);
    connect(fitBtn_, &QPushButton::clicked, this, [this] {
        if (!view_)
            return;
        view_->ResetZoom();
        SyncChromeButtons();
        std::fprintf(stderr, "reader-zoom: reset -> %d%%\n", view_->ZoomPercent());
        std::fflush(stderr);
    });

    // 4) 菜单：其余控件的唯一入口（含图片详情首块）
    menuBtn_ = new QPushButton(bar);
    menuBtn_->setObjectName(QStringLiteral("WinBtn"));
    menuBtn_->setFixedSize(30, 30);
    menuBtn_->setCursor(Qt::PointingHandCursor);
    menuBtn_->setIcon(Glyph::Icon(Glyph::Menu, 16, T().text));
    menuBtn_->setIconSize(QSize(16, 16));
    menuBtn_->setToolTip(QStringLiteral("菜单"));
    menuBtn_->setProperty("hitGrow", false);
    bl->addWidget(menuBtn_);
    connect(menuBtn_, &QPushButton::clicked, this, [this] {
        ShowReaderMenu(menuBtn_->mapToGlobal(QPoint(0, menuBtn_->height())));
    });

    root->addWidget(view_, 1);

    // 底部翻页条：页码滑块 + 页码（与 Python 的 QtCustomSlider 同语义）
    bottom_ = new QWidget(this);
    bottom_->setObjectName(QStringLiteral("TitleBar"));
    bottom_->setFixedHeight(46);
    auto *bt = new QHBoxLayout(bottom_);
    bt->setContentsMargins(12, 6, 12, 6);
    bt->setSpacing(8);
    auto *prevBtn = new QPushButton(bottom_);
    prevBtn->setObjectName(QStringLiteral("WinBtn"));
    prevBtn->setFixedSize(30, 30);
    prevBtn->setCursor(Qt::PointingHandCursor);
    prevBtn->setIcon(Glyph::Icon(Glyph::ChevronRight, 15, T().textDim));
    prevBtn->setIconSize(QSize(15, 15));
    prevBtn->setToolTip(QStringLiteral("上一页"));
    prevBtn->setProperty("hitGrow", false);
    // 上一页 = 左箭头：把 ChevronRight 转 180° 不现实，直接用文字更稳
    prevBtn->setText(QStringLiteral("‹"));
    prevBtn->setIcon(QIcon());
    bt->addWidget(prevBtn);
    connect(prevBtn, &QPushButton::clicked, this, [this] { view_->PrevPage(); });

    pageSlider_ = new QSlider(Qt::Horizontal, bottom_);
    pageSlider_->setRange(1, 1);
    pageSlider_->setValue(1);
    pageSlider_->setMinimumWidth(80);
    pageSlider_->setCursor(Qt::PointingHandCursor);
    // 基座 qss 的槽色是 @border，压在 TitleBar 上也几乎看不见；这里用主题 token
    // 提亮槽色并保留主色进度，避免"只有一个白色小方块"的观感。
    pageSlider_->setStyleSheet(
        QStringLiteral("QSlider{min-height:24px;background:transparent;}"
                       "QSlider::groove:horizontal{height:4px;background:%1;}"
                       "QSlider::sub-page:horizontal{background:%2;}"
                       "QSlider::add-page:horizontal{background:%1;}"
                       "QSlider::handle:horizontal{background:%3;width:12px;margin:-6px 0;"
                       "border:none;}")
            .arg(T().border.lighter(190).name(), T().accent.name(), T().text.name()));
    bt->addWidget(pageSlider_, 1);
    connect(pageSlider_, &QSlider::sliderMoved, this, [this](int v) {
        view_->ScrollToPage(v - 1, false);
    });

    pageLabel_ = new QLabel(QStringLiteral("0 / 0"), bottom_);
    pageLabel_->setObjectName(QStringLiteral("PageSub"));
    pageLabel_->setMinimumWidth(58);
    pageLabel_->setAlignment(Qt::AlignCenter);
    bt->addWidget(pageLabel_);

    auto *nextBtn = new QPushButton(QStringLiteral("›"), bottom_);
    nextBtn->setObjectName(QStringLiteral("WinBtn"));
    nextBtn->setFixedSize(30, 30);
    nextBtn->setCursor(Qt::PointingHandCursor);
    nextBtn->setToolTip(QStringLiteral("下一页"));
    nextBtn->setProperty("hitGrow", false);
    bt->addWidget(nextBtn);
    connect(nextBtn, &QPushButton::clicked, this, [this] { view_->NextPage(); });

    // 工具条不入布局：作为浮层盖在内容上（见 ShowChrome），显示/隐藏只改位置。

    // 没有章节图片时的空态（与阅读视图互斥）
    emptyHost_ = new QWidget(this);
    auto *eh = new QVBoxLayout(emptyHost_);
    eh->setContentsMargins(0, 0, 0, 0);
    emptyHost_->hide();
    root->addWidget(emptyHost_, 1);

    connect(view_, &ReaderView::PageChanged, this,
            [this](int cur, int total, const QString &) {
                UpdatePageWidgets(cur, total);
                emit ProgressChanged(cur, total);
            });
    connect(view_, &ReaderView::StatusMessage, this, [this](const QString &text) {
        emit Toast(text);
    });
    connect(view_, &ReaderView::ZoomChanged, this, [this](int) { SyncChromeButtons(); });
    connect(view_, &ReaderView::ModeChanged, this, [this](int) { SyncChromeButtons(); });
    // 触控点按分区由阅读器发出（Android 上触摸不再合成鼠标事件）
    connect(view_, &ReaderView::TapZone, this, &ReaderPage::HandleTap);
    connect(view_, &ReaderView::RequestToggleChrome, this, [this] { ToggleChrome(); });
    connect(view_, &ReaderView::RequestFullScreen, this, [this] { emit RequestFullScreen(); });
    connect(view_, &ReaderView::RequestBack, this, [this] { emit RequestBack(); });
    connect(view_, &ReaderView::RequestNextChapter, this, [this] { emit RequestNextChapter(); });
    connect(view_, &ReaderView::RequestPrevChapter, this, [this] { emit RequestPrevChapter(); });
    connect(view_, &ReaderView::RequestContextMenu, this, &ReaderPage::ShowReaderMenu);
    connect(view_, &ReaderView::RequestSaveAs, this, &ReaderPage::SaveCurrentAs);
    UpdatePageWidgets(view_->CurrentPage(), view_->PageCount());
    // 构造期先按初始尺寸摆一次；真正进页时 resizeEvent 会再校正
    LayoutChrome();
    SyncChromeButtons();

    // 首次帮助：与 Python ReadFrame.InitHelp 同语义（点一下关闭，落库不再弹）
    // --quiet 是自动化回归：不弹帮助层，避免遮挡截图与合成手势
    if (!UiPrefs::GetBool(QStringLiteral("readerHelpShown"), false)
        && !QCoreApplication::arguments().contains(QStringLiteral("--quiet"))) {
        help_ = new QLabel(this);
        help_->setObjectName(QStringLiteral("ReaderHelp"));
        help_->setWordWrap(true);
        help_->setAlignment(Qt::AlignCenter);
        help_->setAttribute(Qt::WA_StyledBackground, true);
        help_->setStyleSheet(QStringLiteral(
            "QLabel#ReaderHelp{background:rgba(0,0,0,0.80);color:white;"
            "font-size:20px;padding:28px;line-height:150%;}"));
        help_->setText(QStringLiteral(
            "点按分区\n\n左下 上一页　│　中下 下滚半屏　│　右下 下一页\n"
            "左上/右上 显隐工具条　│　中上 上滚半屏\n\n"
            "左右滑动翻页　·　双指捏合缩放　·　放大后拖动平移\n\n"
            "点击任意处关闭"));
        help_->installEventFilter(new HelpHide(help_));
        QTimer::singleShot(0, this, [this] {
            if (!help_)
                return;
            help_->setGeometry(rect());
            help_->show();
            help_->raise();
            Motion::FadeIn(help_, Motion::Base);
        });
    }
}

void ReaderPage::resizeEvent(QResizeEvent *event)
{
    PageBase::resizeEvent(event);
    // 工具条是浮层，位置自己算（内容区始终占满整页）
    LayoutChrome();
    // isVisible() 在父页面还没显示时是 false；用 isHidden() 才能覆盖
    // "页面构造期就 show、首次进入才真正可见"这条路径。
    if (help_ && !help_->isHidden() && view_)
        help_->setGeometry(rect());
}

void ReaderPage::UpdatePageWidgets(int cur, int total)
{
    const QString text = QStringLiteral("%1 / %2").arg(cur).arg(qMax(0, total));
    if (pageInfo_)
        pageInfo_->setText(text);
    if (pageLabel_)
        pageLabel_->setText(text);
    if (pageSlider_) {
        const QSignalBlocker block(pageSlider_);
        pageSlider_->setRange(1, qMax(1, total));
        pageSlider_->setValue(qBound(1, cur, qMax(1, total)));
        pageSlider_->setEnabled(total > 0);
    }
}

void ReaderPage::ScrollHalf(int dir)
{
    if (view_)
        view_->ScrollByHalf(dir);
}

void ReaderPage::ToggleChrome(bool forceShow)
{
    if (!bar_)
        return;
    const bool show = forceShow || !chromeShown_;
    ShowChrome(show, true);
    std::fprintf(stderr, "reader-chrome: visible=%d\n", int(chromeShown_));
    std::fflush(stderr);
}

void ReaderPage::LayoutChrome()
{
    if (!bar_ || !bottom_)
        return;
    const int bw = width();
    const int bh = bar_->height();
    const int th = bottom_->height();
    bar_->setGeometry(0, chromeShown_ ? 0 : -bh, bw, bh);
    bottom_->setGeometry(0, chromeShown_ ? height() - th : height(), bw, th);
    bar_->raise();
    bottom_->raise();
    if (help_ && !help_->isHidden())
        help_->raise();
}

void ReaderPage::ShowChrome(bool show, bool animate)
{
    if (!bar_)
        return;
    if (chromeShown_ == show) {
        if (!animate)
            LayoutChrome();
        return;
    }
    chromeShown_ = show;
    const int bh = bar_->height();
    const int th = bottom_ ? bottom_->height() : 0;
    if (show) {
        bar_->show();
        if (bottom_)
            bottom_->show();
    }
    const QPoint barTo(0, show ? 0 : -bh);
    const QPoint botTo(0, show ? height() - th : height());
    bar_->raise();
    if (bottom_)
        bottom_->raise();

    if (!animate || !Motion::Enabled()) {
        bar_->move(barTo);
        if (bottom_)
            bottom_->move(botTo);
        if (!show) {
            bar_->hide();
            if (bottom_)
                bottom_->hide();
        }
        return;
    }
    // 上条从顶部滑入、下条从底部滑入：方向与"它们各自贴着的边"一致，
    // 用户能立刻看出是同一组控件在进出，而不是两个不相干的块在闪。
    const QEasingCurve curve = show ? Motion::EnterCurve() : Motion::ExitCurve();
    Motion::SlideTo(bar_, barTo, Motion::Base, curve);
    if (bottom_)
        Motion::SlideTo(bottom_, botTo, Motion::Base, curve);
    if (!show) {
        // 滑出屏幕后再 hide：否则会在动画中途留下一条空条带
        QTimer::singleShot(Motion::Base + 30, this, [this] {
            if (!chromeShown_) {
                bar_->hide();
                if (bottom_)
                    bottom_->hide();
            }
        });
    }
}

void ReaderPage::SyncChromeButtons()
{
    // 工具条上只有一个状态按钮（恢复原尺寸）：只要和 100% 有偏离（放大或缩小）
    // 就可用；恰好 100% 时没有可还原的东西，禁用避免"点了没反应"的错觉。
    if (fitBtn_ && view_)
        fitBtn_->setEnabled(view_->CanResetZoom());
    if (!menuBtn_ || !view_)
        return;
    menuBtn_->setToolTip(QStringLiteral("菜单\n模式：%1\n缩放：%2%")
                             .arg(ReaderView::ModeShortName(view_->ModeValue()))
                             .arg(view_->ZoomPercent()));
}

void ReaderPage::HandleTap(const QString &zone, const QPoint &pos)
{
    // 分区语义与 Python 版 mobile_ui_reader._Tap 一致：
    //   上排：左右三分之一 = 显隐工具条；中间 = 上滚半屏
    //   下排：左右三分之一 = 上一页/下一页（放大时不翻页，避免丢视角）；中间 = 下滚半屏
    Q_UNUSED(pos);
    const bool top = zone.endsWith(QStringLiteral("-top"));
    if (top) {
        if (zone.startsWith(QStringLiteral("mid")))
            ScrollHalf(-1);
        else
            ToggleChrome();
    } else if (zone.startsWith(QStringLiteral("mid"))) {
        ScrollHalf(1);
    } else if (!view_->IsZoomed()) {
        if (zone.startsWith(QStringLiteral("left")))
            view_->PrevPage();
        else
            view_->NextPage();
    }

    std::fprintf(stderr, "reader-tap: zone=%s page=%d/%d bar=%d zoomed=%d\n",
                 qPrintable(zone), view_->CurrentPage(), view_->PageCount(),
                 int(chromeShown_), int(view_->IsZoomed()));
    std::fflush(stderr);
}

void ReaderPage::TapDemo(qreal fx, qreal fy)
{
    if (view_)
        view_->SimulateTap(fx, fy);
}

void ReaderPage::ResumePage(int index)
{
    if (view_ && index > 0 && index < view_->PageCount())
        view_->ScrollToPage(index, false);
}

int ReaderPage::LoadDirForTest(const QString &dir)
{
    if (!view_)
        return 0;
    if (emptyHost_)
        emptyHost_->hide();
    view_->show();
    ShowChrome(true, false);
    const int n = view_->LoadDir(dir);
    UpdatePageWidgets(view_->CurrentPage(), n);
    SyncChromeButtons();
    return n;
}

void ReaderPage::RunReaderParitySelfTest()
{
    if (view_)
        view_->RunParitySelfTest();
}

void ReaderPage::RunReaderTouchSelfTest()
{
    if (view_)
        view_->RunTouchEventSelfTest();
}

void ReaderPage::RunReaderProgressiveSelfTest(const QString &dir)
{
    // 自检直接在阅读器里建场：不经过外壳，也不依赖网络
    if (emptyHost_)
        emptyHost_->hide();
    if (view_) {
        view_->show();
        ShowChrome(true, false);
        view_->RunProgressiveSelfTest(dir);
        UpdatePageWidgets(view_->CurrentPage(), view_->PageCount());
    }
}

void ReaderPage::SwipeDemo(int dx, int dy)
{
    if (view_)
        view_->SimulateSwipe(dx, dy);
}

void ReaderPage::PinchDemo(qreal ratio)
{
    if (view_)
        view_->SimulatePinch(ratio);
}

void ReaderPage::ToggleSrForTest()
{
    if (view_)
        view_->ToggleSrCurrent();
}

QString ReaderPage::SrStatusForTest() const
{
    return view_ ? view_->SrStatusText() : QStringLiteral("no-view");
}

int ReaderPage::ZoomPercent() const
{
    return view_ ? view_->ZoomPercent() : 0;
}

void ReaderPage::ClickFitButtonForTest()
{
    if (fitBtn_)
        fitBtn_->click();
}

void ReaderPage::OpenMenuForTest()
{
    if (!menuBtn_)
        return;
    const QString out = qEnvironmentVariable("JM_READER_MENU_SHOT");
    if (out.isEmpty()) {
        ShowReaderMenu(menuBtn_->mapToGlobal(QPoint(0, menuBtn_->height())));
        return;
    }
    // 截图模式：QMenu 是独立顶层窗口，MainWindow::grab() 拍不到它；
    // 这里让菜单离屏布局后单独 grab，再合成到窗口截图上（位置与真实弹出对齐）。
    std::unique_ptr<QMenu> menu(BuildReaderMenu());
    menu->setAttribute(Qt::WA_DontShowOnScreen, true);
    menu->show();
    menu->adjustSize();
    QApplication::processEvents();
    const QPixmap menuShot = menu->grab();
    QWidget *top = window();
    QPixmap base = top->grab();
    const QPoint at = menuBtn_->mapTo(top, QPoint(menuBtn_->width(), menuBtn_->height()));
    const int x = qBound(0, at.x() - menuShot.width(), qMax(0, top->width() - menuShot.width()));
    const int y = qBound(0, at.y(), qMax(0, top->height() - menuShot.height()));
    {
        QPainter p(&base);
        p.drawPixmap(x, y, menuShot);
    }
    const bool ok = base.save(out);
    qWarning("reader-menu: menu=%dx%d composite=%dx%d saved=%d", menuShot.width(),
             menuShot.height(), base.width(), base.height(), int(ok));
}

// 菜单首块：图片详情。原来点"图片信息"按钮弹一个模态框，现在直接铺在菜单顶部，
// 打开菜单就能看到"第几页 / 文件 / 分辨率 / 大小 / 模式 / 缩放 / 超分"。
QWidget *ReaderPage::BuildInfoHeader(QWidget *parent)
{
    auto *host = new QWidget(parent);
    host->setObjectName(QStringLiteral("ReaderMenuInfo"));
    auto *lay = new QVBoxLayout(host);
    lay->setContentsMargins(Touch::IsTouchMode() ? 14 : 18, 8, 14, 6);
    lay->setSpacing(0);

    const QSize size = view_->CurrentImageSize();
    const qint64 bytes = qMax<qint64>(0, view_->CurrentFileSize());
    const QString path = view_->CurrentFilePath();
    const QString file = QFileInfo(path).fileName();
    // 菜单宽度由最长条目决定：给标题/文件名设上限，超长用中间省略，
    // 免得一个长文件名把整个弹菜单撑到屏幕外。
    const int maxW = Touch::IsTouchMode() ? UiStyle::Fpx(240) : UiStyle::Fpx(380);
    const QFontMetrics fm(font());
    auto clip = [&fm, maxW](const QString &s) {
        return fm.elidedText(s, Qt::ElideMiddle, maxW);
    };

    // 用一整块富文本而不是一列 QLabel：QSS 的 font-size 在 polish 之后才生效，
    // 一列 QLabel 的 sizeHint 会按默认字号算，行与行会叠在一起。
    const QString faint = T().textFaint.name();
    const QString strong = T().text.name();
    const QString chapter = title_ ? title_->FullText() : QString();

    QString html;
    html += QStringLiteral("<div style='font-size:%1px;font-weight:600;color:%2;'>第 %3 / %4 页%5</div>")
                .arg(UiStyle::Fpx(14))
                .arg(strong)
                .arg(view_->CurrentPage())
                .arg(view_->PageCount())
                .arg(chapter.isEmpty() ? QString()
                                       : QStringLiteral("　·　%1").arg(clip(chapter).toHtmlEscaped()));
    html += QStringLiteral("<div style='font-size:%1px;color:%2;margin-top:3px;'>文件　%3</div>")
                .arg(UiStyle::Fpx(12))
                .arg(faint)
                .arg((file.isEmpty() ? QStringLiteral("—") : clip(file)).toHtmlEscaped());
    html += QStringLiteral("<div style='font-size:%1px;color:%2;margin-top:2px;'>尺寸　%3 × %4　·　%5 KB　·　动画 %6</div>")
                .arg(UiStyle::Fpx(12))
                .arg(faint)
                .arg(size.width())
                .arg(size.height())
                .arg(bytes / 1024)
                .arg(view_->CurrentIsAnimated() ? QStringLiteral("是") : QStringLiteral("否"));
    html += QStringLiteral("<div style='font-size:%1px;color:%2;margin-top:2px;'>模式　%3　·　缩放 %4%　·　超分 %5</div>")
                .arg(UiStyle::Fpx(12))
                .arg(faint)
                .arg(ReaderView::ModeShortName(view_->ModeValue()))
                .arg(view_->ZoomPercent())
                .arg(clip(view_->SrStatusText()).toHtmlEscaped());

    auto *label = new QLabel(html, host);
    label->setTextFormat(Qt::RichText);
    label->setWordWrap(false);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    label->setToolTip(path);
    lay->addWidget(label);

    return host;
}

void ReaderPage::ShowReaderMenu(const QPoint &globalPos)
{
    if (!view_)
        return;
    QScopedPointer<QMenu> menu(BuildReaderMenu());
    menu->exec(globalPos);}

QMenu *ReaderPage::BuildReaderMenu()
{
    auto *menu = new QMenu(this);
    StyleReaderMenu(menu);

    // ---- 图片详情：菜单顶部直接显示（替代原工具条上的"图片信息"按钮） ----
    auto *head = new QWidgetAction(menu);
    head->setDefaultWidget(BuildInfoHeader(menu));
    menu->addAction(head);
    menu->addSeparator();

    // 返回：工具条上不再有返回箭头，这里放在最显眼的位置（Esc / 系统返回键同效）
    QAction *back = menu->addAction(QStringLiteral("返回 (Esc)"));
    connect(back, &QAction::triggered, this, [this] { emit RequestBack(); });
    menu->addSeparator();

    QAction *prev = menu->addAction(QStringLiteral("上一页 (←)"));
    connect(prev, &QAction::triggered, this, [this] { view_->PrevPage(); });
    QAction *next = menu->addAction(QStringLiteral("下一页 (→)"));
    connect(next, &QAction::triggered, this, [this] { view_->NextPage(); });
    QMenu *chapters = menu->addMenu(QStringLiteral("章节"));
    QAction *prevEps = chapters->addAction(QStringLiteral("上一章 (Shift+←)"));
    prevEps->setEnabled(hasPrevChapter_);
    connect(prevEps, &QAction::triggered, this, [this] { emit RequestPrevChapter(); });
    QAction *nextEps = chapters->addAction(QStringLiteral("下一章 (Shift+→)"));
    nextEps->setEnabled(hasNextChapter_);
    connect(nextEps, &QAction::triggered, this, [this] { emit RequestNextChapter(); });
    menu->addSeparator();

    // 阅读模式（9 种，与 Python ReadMode 同序）
    QMenu *modes = menu->addMenu(QStringLiteral("阅读模式"));
    const QStringList names = ReaderView::ModeNames();
    for (int i = 0; i < names.size(); ++i) {
        QAction *a = modes->addAction(QStringLiteral("%1. %2").arg(i + 1).arg(names.at(i)));
        a->setCheckable(true);
        a->setChecked(view_->ModeValue() == i);
        connect(a, &QAction::triggered, this, [this, i] {
            view_->SetReadMode(i);
            SyncChromeButtons();
        });
    }
    modes->addSeparator();
    QAction *align = modes->addAction(QStringLiteral("双页配对偏移一页 (F10)"));
    align->setEnabled(view_->IsDoublePage());
    connect(align, &QAction::triggered, this, [this] { view_->ToggleDoublePageAlign(); });

    QMenu *zoom = menu->addMenu(QStringLiteral("缩放"));
    for (int pct : {20, 50, 75, 100, 125, 150, 200, 300, 400, 500}) {
        QAction *a = zoom->addAction(QStringLiteral("%1%").arg(pct));
        a->setCheckable(true);
        a->setChecked(view_->ZoomPercent() == pct);
        connect(a, &QAction::triggered, this, [this, pct] { view_->SetZoomPercent(pct); });
    }
    zoom->addSeparator();
    QAction *fit = zoom->addAction(QStringLiteral("适宽 (100%)"));
    connect(fit, &QAction::triggered, this, [this] { view_->ResetZoom(); });

    QAction *autoAct = menu->addAction(QStringLiteral("自动滚动/翻页 (F5)"));
    autoAct->setCheckable(true);
    autoAct->setChecked(view_->IsAutoScroll());
    connect(autoAct, &QAction::triggered, this, [this] {
        view_->SetAutoScroll(!view_->IsAutoScroll());
        SyncChromeButtons();
    });
    // 自动滚动参数（Python 在工具条里有四个滑块，这里收进子菜单，功能等价）
    QMenu *autoSet = menu->addMenu(QStringLiteral("自动滚动设置"));
    auto addPreset = [autoSet](const QString &title, const QVector<int> &values,
                               int current, const std::function<void(int)> &apply,
                               const QString &suffix) {
        QMenu *sub = autoSet->addMenu(title);
        for (int v : values) {
            QAction *a = sub->addAction(QStringLiteral("%1%2").arg(v).arg(suffix));
            a->setCheckable(true);
            a->setChecked(v == current);
            QObject::connect(a, &QAction::triggered, a, [apply, v] { apply(v); });
        }
    };
    addPreset(QStringLiteral("滚动速度"), {10, 20, 40, 80}, ReaderView::PrefScrollStep(),
              [](int v) { ReaderView::SetPrefScrollStep(v); }, QStringLiteral("px"));
    addPreset(QStringLiteral("滚动间隔"), {50, 100, 200, 500}, ReaderView::PrefScrollMs(),
              [](int v) { ReaderView::SetPrefScrollMs(v); }, QStringLiteral("ms"));
    addPreset(QStringLiteral("翻页间隔"), {3000, 5000, 8000, 15000}, ReaderView::PrefTurnMs(),
              [](int v) { ReaderView::SetPrefTurnMs(v); }, QStringLiteral("ms"));
    addPreset(QStringLiteral("手动滚动"), {200, 400, 600}, ReaderView::PrefManualStep(),
              [](int v) { ReaderView::SetPrefManualStep(v); }, QStringLiteral("px"));

    // 超分（原工具条没有入口，一直是菜单项，这里合并成子菜单省一级高度）
    QMenu *sr = menu->addMenu(QStringLiteral("超分"));
    QAction *srThis = sr->addAction(QStringLiteral("本张超分 (F2)"));
    srThis->setCheckable(true);
    srThis->setChecked(view_->CurrentHasSr());
    connect(srThis, &QAction::triggered, this, [this] { view_->ToggleSrCurrent(); });
    QAction *autoSr = sr->addAction(QStringLiteral("自动超分（每页）"));
    autoSr->setCheckable(true);
    autoSr->setChecked(view_->IsAutoSr());
    connect(autoSr, &QAction::triggered, this, [this] {
        view_->SetAutoSr(!view_->IsAutoSr());
    });

    // 图片操作（复制/另存/存到上次目录）
    QMenu *img = menu->addMenu(QStringLiteral("图片"));
    QAction *copy = img->addAction(QStringLiteral("复制图片 (F1)"));
    connect(copy, &QAction::triggered, this, [this] { view_->CopyCurrentToClipboard(); });
    QAction *save = img->addAction(QStringLiteral("图片另存为… (F3)"));
    connect(save, &QAction::triggered, this, &ReaderPage::SaveCurrentAs);
    QAction *saveLast = img->addAction(QStringLiteral("保存到上次目录 (F4)"));
    connect(saveLast, &QAction::triggered, this, [this] { view_->SaveCurrentToLastDir(); });
    menu->addSeparator();

    QAction *full = menu->addAction(QStringLiteral("全屏 (F11)"));
    connect(full, &QAction::triggered, this, [this] { emit RequestFullScreen(); });
    QAction *hide = menu->addAction(QStringLiteral("隐藏工具条 (F12)"));
    connect(hide, &QAction::triggered, this, [this] { ToggleChrome(); });

    return menu;
}

void ReaderPage::SaveCurrentAs()
{
    if (!view_)
        return;
    const QString suggested = QFileInfo(view_->CurrentFilePath()).fileName();
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("图片另存为"),
        QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
            .filePath(suggested),
        QStringLiteral("图片 (*.jpg *.png *.webp)"));
    if (!path.isEmpty())
        view_->SaveCurrentTo(path);
}

void ReaderPage::SetChapterNavigation(bool hasPrev, bool hasNext)
{
    hasPrevChapter_ = hasPrev;
    hasNextChapter_ = hasNext;
}

void ReaderPage::ShowEmptyState()
{
    if (!emptyHost_)
        return;
    if (QLayout *l = emptyHost_->layout()) {
        while (QLayoutItem *it = l->takeAt(0)) {
            if (QWidget *w = it->widget())
                w->deleteLater();
            delete it;
        }
    }
    emptyHost_->layout()->addWidget(new EmptyState(
        QStringLiteral("还没有打开章节"),
        QStringLiteral("从作品详情选择章节后，\n这里显示接口返回的图片。"),
        QString(), emptyHost_));
    emptyHost_->show();
    if (view_)
        view_->hide();
    // 空态下工具条没有意义：直接收起（不播动画，避免和页面切换抢帧）
    if (bar_) {
        chromeShown_ = false;
        bar_->hide();
    }
    if (bottom_)
        bottom_->hide();
    UpdatePageWidgets(0, 0);
}

void ReaderPage::OnEnter()
{
    // 帮助层在页面构造时创建（那时视口尺寸还是占位值），首次真正进入时
    // 按当前视口重新贴一次，否则浮层会停留在 0 尺寸而看不见。
    if (help_ && !help_->isHidden() && view_) {
        help_->setGeometry(rect());
        help_->raise();
    }
    // 在线章节还在下载时不要覆盖状态，否则下载回来会闪一下空态。
    if (pendingRemote_)
        return;
    if (view_->PageCount() == 0)
        ShowEmptyState();
}

void ReaderPage::OpenRemote(const QString &title)
{
    pendingRemote_ = true;
    if (emptyHost_)
        emptyHost_->hide();
    if (view_)
        view_->show();
    ShowChrome(true, false);
    if (bottom_)
        bottom_->hide();   // 还没有页数，滑块无意义
    if (title_)
        title_->SetFullText(title);
    pageInfo_->setText(QStringLiteral("加载中…"));
    view_->LoadFiles({});
    SyncChromeButtons();
}

void ReaderPage::ShowImages(const QString &title, const QStringList &files)
{
    pendingRemote_ = false;
    if (!title.isEmpty() && title_)
        title_->SetFullText(title);
    if (files.isEmpty()) {
        ShowEmptyState();
        emit Toast(QStringLiteral("章节没有可用图片"));
        return;
    }
    if (emptyHost_)
        emptyHost_->hide();
    if (view_)
        view_->show();
    ShowChrome(true, false);
    if (bottom_) {
        bottom_->show();
        LayoutChrome();
    }
    const int n = view_->LoadFiles(files);
    UpdatePageWidgets(view_->CurrentPage(), n);
    SyncChromeButtons();
}

void ReaderPage::SetStatus(const QString &text)
{
    if (pageInfo_)
        pageInfo_->setText(text);
}

// ---------------------------------------------------------------- 渐进加载
//
// 与 OpenRemote 的分工：OpenRemote 只清屏（"获取章节…"），总页数一到就调
// BeginProgressive 铺占位页；之后每张图片到货调一次 SetPageReady，当前页那张
// 会立刻解码显示 —— 用户不用等整章下完。
void ReaderPage::BeginProgressive(int total)
{
    if (total <= 0 || !view_)
        return;
    progressive_ = true;
    loadDone_ = 0;
    loadTotal_ = total;
    // 已经有页数了：OnEnter 不要再画"还没有打开章节"的空态
    pendingRemote_ = false;
    if (emptyHost_)
        emptyHost_->hide();
    view_->show();
    ShowChrome(true, false);
    if (bottom_) {
        bottom_->show();
        LayoutChrome();
    }
    view_->BeginProgressive(total);
    UpdatePageWidgets(view_->CurrentPage(), total);
    SyncChromeButtons();
    qWarning("reader-progressive: begin pages=%d", total);
}

void ReaderPage::SetPageReady(int index, const QString &path)
{
    if (!view_)
        return;
    view_->SetProgressivePage(index, path);
    UpdatePageWidgets(view_->CurrentPage(), qMax(0, view_->PageCount()));
}

void ReaderPage::FinishProgressive(const QStringList &files)
{
    progressive_ = false;
    Q_UNUSED(files);   // 页序以 ReaderView 里的槽位为准（空槽=失败页）
    if (!view_)
        return;
    view_->EndProgressive();
    const int n = view_->PageCount();
    if (n <= 0) {
        ShowEmptyState();
        emit Toast(QStringLiteral("章节没有可用图片"));
        return;
    }
    UpdatePageWidgets(view_->CurrentPage(), n);
    SyncChromeButtons();
    qWarning("reader-progressive: done pages=%d current=%d", n, view_->CurrentPage());
}

void ReaderPage::SetLoadProgress(int done, int total)
{
    if (!view_)
        return;
    loadDone_ = done;
    loadTotal_ = total;
    // 当前页已经出图了就不要再显示"加载中 x/y"：页码才是用户关心的信息。
    if (view_->CurrentHasImage()) {
        UpdatePageWidgets(view_->CurrentPage(), qMax(0, view_->PageCount()));
        return;
    }
    if (pageInfo_ && total > 0)
        pageInfo_->setText(QStringLiteral("加载中 %1/%2").arg(done).arg(total));
}

void ReaderPage::SetFullScreenMode(bool on)
{
    fullScreen_ = on;
    if (on) {
        // 全屏只剩图片；帮助层是整屏遮罩，也一并收掉
        if (help_ && !help_->isHidden()) {
            UiPrefs::Set(QStringLiteral("readerHelpShown"), true);
            help_->hide();
        }
        ShowChrome(false, true);
    } else {
        ShowChrome(true, true);
    }
    qWarning("reader-fullscreen: chrome=%d", int(!fullScreen_));
}

bool ReaderPage::HasImages() const
{
    return view_->PageCount() > 0;
}

void ReaderPage::SetReadingMode(int mode, bool persist)
{
    if (view_)
        view_->SetReadMode(mode, persist);
}

int ReaderPage::ReadingMode() const
{
    return view_ ? int(view_->Mode()) : 0;
}

bool ReaderPage::IsVerticalReading() const
{
    return view_ && view_->IsVertical();
}

void ReaderPage::SetDirection(bool rtl)
{
    if (view_)
        view_->SetDirection(rtl);
}

void ReaderPage::SetZoomPercent(int percent)
{
    if (view_)
        view_->SetZoomPercent(percent);
}

int ReaderPage::CurrentPage() const
{
    return view_ ? view_->CurrentPage() : 0;
}

int ReaderPage::PageCount() const
{
    return view_ ? view_->PageCount() : 0;
}

void ReaderPage::SetAutoSr(bool on)
{
    if (view_)
        view_->SetAutoSr(on);
}

// ============================================================ 超分
namespace {

QString SrBytes(qint64 n)
{
    if (n <= 0)
        return QStringLiteral("—");
    if (n >= qint64(1024) * 1024)
        return QStringLiteral("%1 MB").arg(double(n) / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(double(n) / 1024.0, 0, 'f', 0);
}

// 卡片容器（demo 的 .card）：11px 淡字小标题 + 内容。
// 用独立 id 而不是 #ListRow：卡片不该有 hover 变色（它是容器，不是可点的行）。
QWidget *MakeSrCard(QWidget *parent, const QString &title, QVBoxLayout **out)
{
    auto *card = new QWidget(parent);
    card->setObjectName(QStringLiteral("SrCard"));
    auto *l = new QVBoxLayout(card);
    l->setContentsMargins(UiStyle::Fpx(12), UiStyle::Fpx(10), UiStyle::Fpx(12),
                          UiStyle::Fpx(12));
    l->setSpacing(UiStyle::Fpx(8));
    if (!title.isEmpty()) {
        auto *t = new QLabel(title, card);
        t->setObjectName(QStringLiteral("SrCardTitle"));
        l->addWidget(t);
    }
    *out = l;
    return card;
}

// 卡片里的一行 kv：左键右值（值用省略标签，完整文本留在 tooltip）
ElideLabel *AddSrKv(QVBoxLayout *into, QWidget *parent, const QString &key,
                    const QString &init = QStringLiteral("—"))
{
    auto *row = new QWidget(parent);
    auto *l = new QHBoxLayout(row);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(UiStyle::Fpx(10));
    auto *k = new QLabel(key, row);
    k->setObjectName(QStringLiteral("SrKvKey"));
    l->addWidget(k, 0);
    auto *v = new ElideLabel(init, row);
    v->setObjectName(QStringLiteral("SrKvVal"));
    v->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    l->addWidget(v, 1);
    into->addWidget(row);
    return v;
}

}  // namespace

SrPage::SrPage(QWidget *parent) : PageBase(parent)
{
    watcher_ = new QFutureWatcher<SrEngine::Result>(this);
    connect(watcher_, &QFutureWatcher<SrEngine::Result>::finished, this, [this] {
        const SrEngine::Result r = watcher_->result();
        busy_ = false;
        if (runBtn_)
            runBtn_->setEnabled(true);
        if (progress_) {
            progress_->setVisible(false);
            progress_->setValue(0);
        }
        if (r.image.isNull()) {
            result_ = QImage();
            resultMs_ = 0.0;
            const QString err = r.error.isEmpty() ? QStringLiteral("未知错误") : r.error;
            if (detail_)
                detail_->SetFullText(QStringLiteral("超分失败：%1").arg(err));
            if (saveBtn_)
                saveBtn_->setEnabled(false);
            UpdatePreviewImage();
            emit Toast(QStringLiteral("超分失败：%1").arg(err));
            return;
        }
        result_ = r.image;
        resultMs_ = r.ms;
        if (saveBtn_)
            saveBtn_->setEnabled(true);
        if (detail_)
            detail_->SetFullText(QStringLiteral("转换完成"));
        UpdatePreviewImage();
        emit Toast(QStringLiteral("超分完成：%1→%2 · %3ms")
                       .arg(source_.width())
                       .arg(result_.width())
                       .arg(r.ms, 0, 'f', 0));
    });

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("图片超分"),
                            QStringLiteral("选择本地超分模型 · 一次一张"), body, &header);
    lay->addWidget(head);
    auto *bench = new QPushButton(QStringLiteral("测速"), body);
    bench->setObjectName(QStringLiteral("Ghost"));
    bench->setCursor(Qt::PointingHandCursor);
    header->addWidget(bench);
    connect(bench, &QPushButton::clicked, this, [this] {
        // 测速用固定 64x64 小图跑当前模型：给出"每块/整图"的真实耗时，
        // 而不是伪造一个进度条。失败时如实说原因。
        if (busy_) {
            emit Toast(QStringLiteral("上一次超分还在跑"));
            return;
        }
        UpdateStatusLine();
        if (!SrEngine::Available()) {
            emit Toast(SrEngine::Status());
            return;
        }
        QImage probe(64, 64, QImage::Format_RGB888);
        for (int y = 0; y < 64; ++y) {
            uchar *line = probe.scanLine(y);
            for (int x = 0; x < 64; ++x) {
                line[x * 3 + 0] = uchar(x * 4);
                line[x * 3 + 1] = uchar(y * 4);
                line[x * 3 + 2] = uchar((x + y) * 2);
            }
        }
        const SrEngine::Result r = SrEngine::Upscale(probe);
        if (r.image.isNull()) {
            emit Toast(QStringLiteral("测速失败：%1").arg(r.error));
            return;
        }
        if (detail_)
            detail_->SetFullText(QStringLiteral("测速 64×64 → %1×%2 · %3ms")
                                     .arg(r.image.width())
                                     .arg(r.image.height())
                                     .arg(r.ms, 0, 'f', 0));
        emit Toast(QStringLiteral("测速 %1ms（%2）").arg(r.ms, 0, 'f', 0).arg(r.model));
    });

    // ---- 预览区（demo 的 .imgstage）：定高 + 右上角分辨率角标 ----
    // 不再和模型列表左右分栏：窄屏并排会把页面撑到 700+px（文档 horizontal_pan_fix.md），
    // 统一成"上图下卡片"后 320dp 也不需要横向滚动。
    stage_ = new QWidget(body);
    stage_->setObjectName(QStringLiteral("SrStage"));
    stage_->setAttribute(Qt::WA_StyledBackground, true);
    stage_->setFixedHeight(Touch::IsTouchMode() ? UiStyle::Fpx(220) : UiStyle::Fpx(260));
    stage_->installEventFilter(this);
    preview_ = new QLabel(stage_);
    preview_->setObjectName(QStringLiteral("SrPreview"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setText(QStringLiteral("选择图片后显示结果"));
    preview_->setMinimumWidth(0);
    badge_ = new QLabel(stage_);
    badge_->setObjectName(QStringLiteral("SrBadge"));
    badge_->hide();
    lay->addWidget(stage_);

    // ---- 卡片 1：超分参数（整行选择栏 + 开始）----
    // 模型列表不再内联铺在页面里：39 行会把预览和按钮推到几屏之外，
    // 现在点选择栏开浮层（分组 + 吸顶 + 当前使用高亮）。
    QVBoxLayout *c1 = nullptr;
    auto *card1 = MakeSrCard(body, QStringLiteral("超分参数"), &c1);
    modelField_ = new SrModelField(card1);
    c1->addWidget(modelField_);
    connect(modelField_, &SrModelField::clicked, this, [this] {
        const QString picked = SrModelSheet::Pick(this, model_);
        if (!picked.isEmpty())
            SelectModel(picked);
    });

    auto *runRow = new QHBoxLayout();
    runRow->setContentsMargins(0, 0, 0, 0);
    runRow->setSpacing(UiStyle::Fpx(10));
    scaleLabel_ = new QLabel(QStringLiteral("放大倍数 —"), card1);
    scaleLabel_->setObjectName(QStringLiteral("SrFieldLabel"));
    scaleLabel_->setMinimumWidth(0);
    runRow->addWidget(scaleLabel_, 1);
    runBtn_ = new QPushButton(QStringLiteral("开始超分"), card1);
    runBtn_->setObjectName(QStringLiteral("Primary"));
    UiStyle::MarkSoftButton(runBtn_);
    runBtn_->setCursor(Qt::PointingHandCursor);
    runBtn_->setEnabled(false);
    runRow->addWidget(runBtn_, 0);
    c1->addLayout(runRow);

    progress_ = new QProgressBar(card1);
    progress_->setObjectName(QStringLiteral("RowProgress"));
    progress_->setRange(0, 100);
    progress_->setTextVisible(false);
    progress_->setVisible(false);
    c1->addWidget(progress_);
    lay->addWidget(card1);

    // ---- 卡片 2：图片信息（demo 的 .kv 三行 + 状态）----
    QVBoxLayout *c2 = nullptr;
    auto *card2 = MakeSrCard(body, QStringLiteral("图片信息"), &c2);
    infoRes_ = AddSrKv(c2, card2, QStringLiteral("分辨率"));
    infoSize_ = AddSrKv(c2, card2, QStringLiteral("大小"));
    infoMs_ = AddSrKv(c2, card2, QStringLiteral("耗时"));
    detail_ = AddSrKv(c2, card2, QStringLiteral("状态"), QStringLiteral("未开始"));
    lay->addWidget(card2);

    // ---- 卡片 3：其他（打开 / 保存）----
    QVBoxLayout *c3 = nullptr;
    auto *card3 = MakeSrCard(body, QStringLiteral("其他"), &c3);
    auto *btns = new QHBoxLayout();
    btns->setContentsMargins(0, 0, 0, 0);
    btns->setSpacing(UiStyle::Fpx(8));
    pickBtn_ = new QPushButton(QStringLiteral("打开图片…"), card3);
    pickBtn_->setObjectName(QStringLiteral("Ghost"));
    pickBtn_->setCursor(Qt::PointingHandCursor);
    saveBtn_ = new QPushButton(QStringLiteral("保存结果"), card3);
    saveBtn_->setObjectName(QStringLiteral("Ghost"));
    saveBtn_->setCursor(Qt::PointingHandCursor);
    saveBtn_->setEnabled(false);
    btns->addWidget(pickBtn_, 1);
    btns->addWidget(saveBtn_, 1);
    c3->addLayout(btns);
    lay->addWidget(card3);

    // 引擎状态：demo 没有这一行，但"模型缺文件/ORT 没加载"必须能一眼看到
    status_ = new QLabel(body);
    status_->setObjectName(QStringLiteral("SectionHint"));
    status_->setWordWrap(true);
    status_->setMinimumWidth(0);
    lay->addWidget(status_);

    lay->addStretch(1);
    root->addWidget(WrapScroll(body, this));

    connect(pickBtn_, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择要超分的图片"), QString(),
            QStringLiteral("图片 (*.png *.jpg *.jpeg *.webp *.bmp)"));
        if (!path.isEmpty())
            SetSourceForTest(path);
    });
    connect(runBtn_, &QPushButton::clicked, this, [this] {
        if (sourcePath_.isEmpty() || source_.isNull()) {
            emit Toast(QStringLiteral("先选一张图片"));
            return;
        }
        StartUpscale(sourcePath_);
    });
    connect(saveBtn_, &QPushButton::clicked, this, &SrPage::SaveResult);

    ReloadModels();
}

void SrPage::OnEnter() { ReloadModels(); }

void SrPage::ReloadModels()
{
    // 选的模型：内存里的当前模型 > ui.ini 的 srModelName > 第一个可用
    model_ = SrEngine::CurrentModel();
    UpdateModelField();
    UpdateStatusLine();
    UpdateInfoCard();
}

// 选择栏 + 放大倍数：模型一换这里必须跟着变（否则页面上写着 2×、实际跑 4×）
void SrPage::UpdateModelField()
{
    if (modelField_)
        modelField_->SetModel(model_);
    if (scaleLabel_) {
        const int sc = SrEngine::CurrentScale();
        scaleLabel_->setText(sc > 1 ? QStringLiteral("放大 %1× · 随模型").arg(sc)
                                    : QStringLiteral("1× · 只降噪"));
    }
}

bool SrPage::eventFilter(QObject *obj, QEvent *event)
{
    // 预览区尺寸一变，图就要重算缩放（QLabel 自己不会按新尺寸重画像素）
    if (obj == stage_ && event->type() == QEvent::Resize) {
        RelayoutStage();
        return false;
    }
    return PageBase::eventFilter(obj, event);
}

void SrPage::RelayoutStage()
{
    if (!stage_ || !preview_)
        return;
    const int pad = UiStyle::Fpx(10);
    preview_->setGeometry(stage_->rect().adjusted(pad, pad, -pad, -pad));
    if (badge_ && badge_->isVisible()) {
        badge_->adjustSize();
        badge_->move(stage_->width() - badge_->width() - UiStyle::Fpx(10), UiStyle::Fpx(9));
    }
    UpdatePreviewImage();
}

// 信息卡：分辨率/大小取原图，耗时取最近一次超分结果（没有就写 — 占位，行不跳）
void SrPage::UpdateInfoCard()
{
    if (infoRes_)
        infoRes_->SetFullText(source_.isNull()
                                  ? QStringLiteral("—")
                                  : QStringLiteral("%1 × %2").arg(source_.width()).arg(source_.height()));
    if (infoSize_) {
        const qint64 bytes = sourcePath_.isEmpty() ? 0 : QFileInfo(sourcePath_).size();
        infoSize_->SetFullText(SrBytes(bytes));
    }
    if (infoMs_)
        infoMs_->SetFullText(resultMs_ > 0 ? QStringLiteral("%1 s").arg(resultMs_ / 1000.0, 0, 'f', 2)
                                           : QStringLiteral("—"));
}

void SrPage::UpdateStatusLine()
{
    if (!status_)
        return;
    const QVector<SrEngine::ModelInfo> installed = SrEngine::InstalledModels();
    const QVector<SrEngine::ModelInfo> all = SrEngine::Catalog();
    if (installed.isEmpty()) {
        status_->setText(QStringLiteral("没有可用模型：%1")
                             .arg(SrEngine::Status()));
        return;
    }
    QString line;
    if (SrEngine::Available())
        line = QStringLiteral("引擎就绪 · %1").arg(SrEngine::Status());
    else
        line = QStringLiteral("引擎不可用：%1").arg(SrEngine::Status());
    if (all.size() > installed.size()) {
        line += QStringLiteral(" · 已安装 %1/%2 个模型（缺的可在 sr_models/ 补文件）")
                    .arg(installed.size())
                    .arg(all.size());
    } else {
        line += QStringLiteral(" · 已安装 %1/%1 个模型").arg(installed.size());
    }
    status_->setText(line);
}

bool SrPage::SelectModel(const QString &name)
{
    if (name.isEmpty())
        return false;
    if (!SrEngine::SetModel(name)) {
        emit Toast(QStringLiteral("切换到 %1 失败：%2").arg(name, SrEngine::Status()));
        return false;
    }
    model_ = name;
    UiPrefs::Set(QStringLiteral("srModelName"), name);
    result_ = QImage();   // 换模型后旧结果不再对应当前模型
    resultMs_ = 0.0;
    if (saveBtn_)
        saveBtn_->setEnabled(false);
    if (detail_)
        detail_->SetFullText(QStringLiteral("已切换模型，旧结果已清空"));
    UpdateModelField();
    UpdateStatusLine();
    UpdatePreviewImage();
    emit Toast(QStringLiteral("已选择 %1").arg(name));
    return true;
}

QString SrPage::CurrentModelName() const
{
    return model_;
}

QString SrPage::Report() const
{
    const QVector<SrEngine::ModelInfo> installed = SrEngine::InstalledModels();
    return QStringLiteral("sr-page: models=%1 engine=%2 selected=%3 source=%4x%5 result=%6x%7 "
                          "ms=%8 busy=%9")
        .arg(installed.size())
        .arg(int(SrEngine::Available()))
        .arg(model_.isEmpty() ? QStringLiteral("-") : model_)
        .arg(source_.width())
        .arg(source_.height())
        .arg(result_.width())
        .arg(result_.height())
        .arg(resultMs_, 0, 'f', 0)
        .arg(int(busy_));
}

void SrPage::SetSourceForTest(const QString &path)
{
    QImage img(path);
    if (img.isNull()) {
        emit Toast(QStringLiteral("读不了这张图：%1").arg(QFileInfo(path).fileName()));
        return;
    }
    source_ = img;
    sourcePath_ = path;
    result_ = QImage();
    resultMs_ = 0.0;
    if (runBtn_)
        runBtn_->setEnabled(!busy_);
    if (saveBtn_)
        saveBtn_->setEnabled(false);
    if (detail_)
        detail_->SetFullText(QStringLiteral("已选择 %1，点「开始超分」")
                                 .arg(QFileInfo(path).fileName()));
    UpdatePreviewImage();
}

void SrPage::UpdatePreviewImage()
{
    if (!preview_)
        return;
    const QImage &img = result_.isNull() ? source_ : result_;
    if (img.isNull()) {
        preview_->setPixmap(QPixmap());
        preview_->setText(QStringLiteral("选择图片后显示结果"));
        if (badge_)
            badge_->hide();
        UpdateInfoCard();
        return;
    }
    const QSize box = preview_->size();
    if (box.width() > UiStyle::Fpx(16) && box.height() > UiStyle::Fpx(16)) {
        const QPixmap pm = QPixmap::fromImage(
            img.scaled(box - QSize(UiStyle::Fpx(6), UiStyle::Fpx(6)), Qt::KeepAspectRatio,
                       Qt::SmoothTransformation));
        preview_->setPixmap(pm);
    }
    if (badge_ && stage_) {
        badge_->setText(QStringLiteral("%1 × %2").arg(img.width()).arg(img.height()));
        badge_->adjustSize();
        badge_->move(stage_->width() - badge_->width() - UiStyle::Fpx(10), UiStyle::Fpx(9));
        badge_->show();
        badge_->raise();
    }
    UpdateInfoCard();
}

void SrPage::StartUpscale(const QString &path)
{
    if (busy_)
        return;
    QImage img(path);
    if (img.isNull()) {
        emit Toast(QStringLiteral("读不了这张图"));
        return;
    }
    if (!SrEngine::Available()) {
        emit Toast(QStringLiteral("超分不可用：%1").arg(SrEngine::Status()));
        return;
    }
    source_ = img;
    sourcePath_ = path;
    busy_ = true;
    if (runBtn_)
        runBtn_->setEnabled(false);
    if (saveBtn_)
        saveBtn_->setEnabled(false);
    if (progress_) {
        progress_->setRange(0, 100);
        progress_->setValue(0);
        progress_->setVisible(true);
    }
    if (detail_)
        detail_->SetFullText(QStringLiteral("处理中… %1×%2 → %3×")
                                 .arg(img.width())
                                 .arg(img.height())
                                 .arg(SrEngine::CurrentScale()));
    const QString useModel = model_;
    QPointer<SrPage> guard(this);
    auto progressCb = [guard](int done, int total) {
        if (!guard || total <= 0)
            return;
        const int pct = int(100.0 * done / total);
        QMetaObject::invokeMethod(
            guard,
            [guard, pct, done, total] {
                if (!guard || !guard->progress_)
                    return;
                guard->progress_->setValue(qBound(0, pct, 100));
                if (guard->detail_)
                    guard->detail_->SetFullText(QStringLiteral("处理中… %1/%2 行")
                                                    .arg(done)
                                                    .arg(total));
            },
            Qt::QueuedConnection);
    };
    watcher_->setFuture(QtConcurrent::run([img, useModel, progressCb] {
        return SrEngine::UpscaleEx(img, useModel, progressCb, nullptr);
    }));
}

bool SrPage::StartUpscaleForTest()
{
    if (sourcePath_.isEmpty() || source_.isNull() || busy_)
        return false;
    StartUpscale(sourcePath_);
    return true;
}

void SrPage::SaveResult()
{
    if (result_.isNull()) {
        emit Toast(QStringLiteral("还没有可保存的结果"));
        return;
    }
    const QString base = sourcePath_.isEmpty()
                             ? QStringLiteral("sr")
                             : QFileInfo(sourcePath_).completeBaseName() + QStringLiteral("_sr");
    const QString suggested =
        QDir(QStandardPaths::writableLocation(QStandardPaths::PicturesLocation))
            .filePath(base + QStringLiteral(".png"));
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("保存超分结果"),
                                                      suggested,
                                                      QStringLiteral("PNG (*.png)"));
    if (path.isEmpty())
        return;
    if (result_.save(path))
        emit Toast(QStringLiteral("已保存到 %1").arg(QFileInfo(path).fileName()));
    else
        emit Toast(QStringLiteral("保存失败：%1").arg(path));
}

// ============================================================ 设置
SettingsView::SettingsView(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    body_ = qobject_cast<QVBoxLayout *>(body->layout());

    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("设置"), QStringLiteral("主题、字体、阅读、网络与缓存设置"),
                            body, &header);
    body_->addWidget(head);

    auto *search = new SearchBox(QStringLiteral("搜索设置项"), body);
    search->setFixedHeight(36);
    search->setMaximumWidth(320);
    body_->addWidget(search);
    connect(search, &SearchBox::Changed, this, [this, body](const QString &kw) {
        for (auto *row : body->findChildren<SettingRow *>()) {
            const auto *label = row->findChild<QLabel *>(QStringLiteral("SettingTitle"));
            row->setVisible(kw.isEmpty() || (label && label->text().contains(kw, Qt::CaseInsensitive)));
        }
        for (auto *sec : body->findChildren<SectionHeader *>())
            sec->setVisible(kw.isEmpty());
    });

    auto *appearance = BuildGroup(QStringLiteral("外观"));
    // 主题 = 明暗 × 强调色。三套强调色与 Python 的 qss 一一对应，
    // 橙色（#FF7B00）是 Python 的默认档，所以排第一。
    static const char *kThemeNames[] = {"dark_orange",  "dark_pink",  "dark_teal",
                                        "light_orange", "light_pink", "light_teal"};
    const QStringList themeLabels = {QStringLiteral("深色 · 橙色"), QStringLiteral("深色 · 粉色"),
                                     QStringLiteral("深色 · 青色"), QStringLiteral("浅色 · 橙色"),
                                     QStringLiteral("浅色 · 粉色"), QStringLiteral("浅色 · 青色")};
    int themeIndex = 0;
    for (int i = 0; i < 6; ++i) {
        if (UiTheme::I().Name() == QLatin1String(kThemeNames[i]))
            themeIndex = i;
    }
    auto *themeCombo = MakeCombo(themeLabels, themeIndex, appearance);
    connect(themeCombo, &QComboBox::currentIndexChanged, this, [](int idx) {
        static const char *names[] = {"dark_orange",  "dark_pink",  "dark_teal",
                                      "light_orange", "light_pink", "light_teal"};
        const QString name = QString::fromLatin1(names[qBound(0, idx, 5)]);
        UiTheme::I().Apply(name);
        // 主题是"重启后还在吗"最容易被用户检验的一项，必须落库。
        UiPrefs::Set(QStringLiteral("theme"), name);
    });
    static_cast<QVBoxLayout *>(appearance->layout())
        ->addWidget(new SettingRow(QStringLiteral("主题"), QStringLiteral("切换后立即生效"), themeCombo,
                                   appearance));
    auto *langCombo = MakeCombo({QStringLiteral("简体中文"), QStringLiteral("繁體中文"),
                                 QStringLiteral("English")},
                                UiPrefs::GetInt(QStringLiteral("language"), 0), appearance);
    connect(langCombo, &QComboBox::currentIndexChanged, this, [](int idx) {
        UiPrefs::Set(QStringLiteral("language"), idx);
    });
    static_cast<QVBoxLayout *>(appearance->layout())
        ->addWidget(new SettingRow(QStringLiteral("语言"), QStringLiteral("界面文案语言"),
                                   langCombo,
                                   appearance));
    auto *autoStart = new ToggleSwitch(appearance);
    autoStart->setChecked(UiPrefs::GetBool(QStringLiteral("autoStart"), true));
    connect(autoStart, &ToggleSwitch::toggled, this, [](bool on) {
        UiPrefs::Set(QStringLiteral("autoStart"), on);
    });
    static_cast<QVBoxLayout *>(appearance->layout())
        ->addWidget(new SettingRow(QStringLiteral("开机自启"), QStringLiteral("仅桌面端生效"), autoStart,
                                   appearance));

    body_->addWidget(new FontSettingsGroup(body));

    auto *reading = BuildGroup(QStringLiteral("阅读"));
    // 阅读模式：0~8，与 Python ReadMode / ReaderView 同序
    readMode_ = MakeCombo(ReaderView::ModeNames(), ReaderView::PrefMode(), reading);
    {
        const QSignalBlocker b(readMode_);
        readMode_->setCurrentIndex(ReaderView::PrefMode());   // 与阅读器共用一份偏好
    }
    connect(readMode_, &QComboBox::currentIndexChanged, this, [this](int idx) {
        emit ReadingModeChanged(idx);
    });
    static_cast<QVBoxLayout *>(reading->layout())
        ->addWidget(new SettingRow(QStringLiteral("默认阅读模式"),
                                   QStringLiteral("9 种模式，与看图工具条同一份偏好"), readMode_,
                                   reading));
    auto *dirCombo = MakeCombo({QStringLiteral("从左到右"), QStringLiteral("从右到左")},
                               ReaderView::IsRtlMode(ReaderView::PrefMode()) ? 1 : 0, reading);
    connect(dirCombo, &QComboBox::currentIndexChanged, this, [this](int idx) {
        UiPrefs::Set(QStringLiteral("readDirection"), idx);
        emit ReadingDirectionChanged(idx == 1);
    });
    static_cast<QVBoxLayout *>(reading->layout())
        ->addWidget(new SettingRow(QStringLiteral("阅读方向"),
                                   QStringLiteral("日漫从右往左（对双页/滚动模式生效）"), dirCombo,
                                   reading));
    const QVector<int> zooms = {50, 75, 100, 125, 150, 200, 300, 400, 500};
    QStringList zoomLabels;
    for (int v : zooms)
        zoomLabels << QStringLiteral("%1%").arg(v);
    int zoomIdx = zooms.indexOf(ReaderView::PrefZoom());
    if (zoomIdx < 0)
        zoomIdx = 2;   // 100%
    auto *zoomCombo = MakeCombo(zoomLabels, zoomIdx, reading);
    connect(zoomCombo, &QComboBox::currentIndexChanged, this, [this, zooms](int idx) {
        if (idx < 0 || idx >= zooms.size())
            return;
        ReaderView::SetPrefZoom(zooms.at(idx));
        emit ReadingZoomChanged(zooms.at(idx));
    });
    static_cast<QVBoxLayout *>(reading->layout())
        ->addWidget(new SettingRow(QStringLiteral("默认缩放"),
                                   QStringLiteral("也可在看图时双指捏合（20%~500%）"), zoomCombo,
                                   reading));
    auto *autoSr = new ToggleSwitch(reading);
    autoSr->setChecked(ReaderView::PrefAutoSr());
    connect(autoSr, &ToggleSwitch::toggled, this, [this](bool on) {
        ReaderView::SetPrefAutoSr(on);
        emit ReadingAutoSrChanged(on);
    });
    static_cast<QVBoxLayout *>(reading->layout())
        ->addWidget(new SettingRow(QStringLiteral("自动超分"),
                                   QStringLiteral("每页进入时跑 waifu2x（CPU 较慢，建议按需开启）"),
                                   autoSr, reading));
    auto *brightCombo = MakeCombo({QStringLiteral("100%"), QStringLiteral("75%"),
                                   QStringLiteral("50%"), QStringLiteral("25%")},
                                  UiPrefs::GetInt(QStringLiteral("brightness"), 0), reading);
    connect(brightCombo, &QComboBox::currentIndexChanged, this, [](int idx) {
        UiPrefs::Set(QStringLiteral("brightness"), idx);
    });
    static_cast<QVBoxLayout *>(reading->layout())
        ->addWidget(new SettingRow(QStringLiteral("亮度"), QStringLiteral("阅读时压暗画面"),
                                   brightCombo,
                                   reading));
    auto *autoNext = new ToggleSwitch(reading);
    autoNext->setChecked(UiPrefs::GetBool(QStringLiteral("autoNext"), false));
    connect(autoNext, &ToggleSwitch::toggled, this, [](bool on) {
        UiPrefs::Set(QStringLiteral("autoNext"), on);
    });
    static_cast<QVBoxLayout *>(reading->layout())
        ->addWidget(new SettingRow(QStringLiteral("自动翻页"), QStringLiteral("每 8 秒翻一页"), autoNext,
                                   reading));

    auto *network = BuildGroup(QStringLiteral("网络"));
    // H3/ECH 的真值存在 ProxyConfig 里（proxy/http3On、proxy/enableEch），
    // 这里只做"改了就存"，避免两套配置各说各话。
    ProxyConfig::Load();
    auto *h3 = new ToggleSwitch(network);
    h3->setChecked(ProxyConfig::Http3Enabled());
    connect(h3, &ToggleSwitch::toggled, this, [](bool on) {
        ProxyConfig::SetHttp3Enabled(on);
        ProxyConfig::Save();
    });
    static_cast<QVBoxLayout *>(network->layout())
        ->addWidget(new SettingRow(QStringLiteral("HTTP/3"), QStringLiteral("CDN 支持时优先走 QUIC"), h3,
                                   network));
    auto *ech = new ToggleSwitch(network);
    ech->setChecked(ProxyConfig::EchEnabled());
    connect(ech, &ToggleSwitch::toggled, this, [](bool on) {
        ProxyConfig::SetEchEnabled(on);
        ProxyConfig::Save();
    });
    static_cast<QVBoxLayout *>(network->layout())
        ->addWidget(new SettingRow(QStringLiteral("ECH"), QStringLiteral("失败自动降级并记住"), ech,
                                   network));
    static_cast<QVBoxLayout *>(network->layout())
        ->addWidget(new SettingRow(QStringLiteral("代理"), QStringLiteral("当前：直连"),
                                   MakeCombo({QStringLiteral("直连"), QStringLiteral("系统代理"),
                                              QStringLiteral("自定义节点")},
                                             0, network),
                                   network));
    // 真机上侧栏的"分流"值是靠事件过滤器接的，点中区域偏小；设置页给一个明确入口
    auto *proxyRow = new SettingRow(QStringLiteral("分流与节点"),
                                    QStringLiteral("线路 / CDN 节点测速与选择"), nullptr, network);
    auto *proxyOpen = new QPushButton(QStringLiteral("打开"), proxyRow);
    proxyOpen->setObjectName(QStringLiteral("Ghost"));
    proxyOpen->setCursor(Qt::PointingHandCursor);
    proxyOpen->setMaximumWidth(UiStyle::Fpx(150));
    proxyRow->SetControl(proxyOpen);
    connect(proxyOpen, &QPushButton::clicked, this, [this] { emit Navigate(PageId::Proxy); });
    static_cast<QVBoxLayout *>(network->layout())->addWidget(proxyRow);

    auto *timeoutCombo = MakeCombo({QStringLiteral("10"), QStringLiteral("20"),
                                    QStringLiteral("30"), QStringLiteral("60")},
                                   UiPrefs::GetInt(QStringLiteral("netTimeout"), 1), network);
    connect(timeoutCombo, &QComboBox::currentIndexChanged, this, [](int idx) {
        UiPrefs::Set(QStringLiteral("netTimeout"), idx);
    });
    static_cast<QVBoxLayout *>(network->layout())
        ->addWidget(new SettingRow(QStringLiteral("超时"), QStringLiteral("秒"), timeoutCombo,
                                   network));

    auto *download = BuildGroup(QStringLiteral("下载与缓存"));
    auto *concurrencyCombo = MakeCombo({QStringLiteral("1"), QStringLiteral("2"),
                                        QStringLiteral("3"), QStringLiteral("4")},
                                       UiPrefs::GetInt(QStringLiteral("netConcurrency"), 1), download);
    connect(concurrencyCombo, &QComboBox::currentIndexChanged, this, [](int idx) {
        UiPrefs::Set(QStringLiteral("netConcurrency"), idx);
    });
    static_cast<QVBoxLayout *>(download->layout())
        ->addWidget(new SettingRow(QStringLiteral("并发数"), QStringLiteral("同时下载的任务数"),
                                   concurrencyCombo,
                                   download));

    // 自定义缓存路径：封面磁盘缓存 + 章节图片缓存都落在这一层下面
    auto *cacheRow = new SettingRow(QStringLiteral("缓存目录"),
                                    QStringLiteral("封面与章节图片的磁盘缓存位置"), nullptr,
                                    download);
    auto *cacheHost = new QWidget(cacheRow);
    auto *chl = new QHBoxLayout(cacheHost);
    chl->setContentsMargins(0, 0, 0, 0);
    chl->setSpacing(8);
    cacheLabel_ = new QLabel(cacheHost);
    cacheLabel_->setObjectName(QStringLiteral("RowMeta"));
    cacheLabel_->setMinimumWidth(0);
    cacheLabel_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    // 容器必须能撑满整行：否则紧凑布局下两个按钮会把路径挤成 0，
    // 按钮自己也被压到只剩一个字（模拟器 1080x2424 实测）。
    cacheHost->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    chl->addWidget(cacheLabel_, 1);
    auto *cachePick = new QPushButton(QStringLiteral("选择…"), cacheHost);
    cachePick->setObjectName(QStringLiteral("Ghost"));
    cachePick->setCursor(Qt::PointingHandCursor);
    cachePick->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    cachePick->setMinimumWidth(UiStyle::Fpx(64));
    auto *cacheReset = new QPushButton(QStringLiteral("默认"), cacheHost);
    cacheReset->setObjectName(QStringLiteral("Ghost"));
    cacheReset->setCursor(Qt::PointingHandCursor);
    cacheReset->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    cacheReset->setMinimumWidth(UiStyle::Fpx(56));
    chl->addWidget(cachePick, 0);
    chl->addWidget(cacheReset, 0);
    cacheRow->SetControl(cacheHost);
    static_cast<QVBoxLayout *>(download->layout())->addWidget(cacheRow);

    auto updateCacheLabel = [this] { UpdateCacheLabel(); };
    updateCacheLabel();
    connect(cachePick, &QPushButton::clicked, this, [this, updateCacheLabel] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择缓存目录"), CachePath::Root());
        if (dir.isEmpty())
            return;
        if (!ApplyCacheDir(dir)) {
            emit Toast(QStringLiteral("该目录不可写，缓存目录未修改"));
            return;
        }
        updateCacheLabel();
    });
    connect(cacheReset, &QPushButton::clicked, this, [this, updateCacheLabel] {
        ApplyCacheDir(QString());
        updateCacheLabel();
    });

    // 下载保存目录：默认在应用数据目录下（系统"下载"目录在部分 Windows 上
    // 被"受控文件夹访问"保护，未签名程序写入会被拒），可改到任意可写目录。
    auto *saveRow = new SettingRow(QStringLiteral("保存目录"),
                                   QStringLiteral("章节下载的落盘位置"), nullptr, download);
    auto *saveHost = new QWidget(saveRow);
    auto *shl = new QHBoxLayout(saveHost);
    shl->setContentsMargins(0, 0, 0, 0);
    shl->setSpacing(8);
    auto *saveLabel = new QLabel(saveHost);
    saveLabel->setObjectName(QStringLiteral("RowMeta"));
    saveLabel->setMinimumWidth(0);
    saveLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    shl->addWidget(saveLabel, 1);
    auto *savePick = new QPushButton(QStringLiteral("选择…"), saveHost);
    savePick->setObjectName(QStringLiteral("Ghost"));
    savePick->setCursor(Qt::PointingHandCursor);
    savePick->setMinimumWidth(UiStyle::Fpx(64));
    auto *saveReset = new QPushButton(QStringLiteral("默认"), saveHost);
    saveReset->setObjectName(QStringLiteral("Ghost"));
    saveReset->setCursor(Qt::PointingHandCursor);
    saveReset->setMinimumWidth(UiStyle::Fpx(56));
    shl->addWidget(savePick, 0);
    shl->addWidget(saveReset, 0);
    saveRow->SetControl(saveHost);
    static_cast<QVBoxLayout *>(download->layout())->addWidget(saveRow);
    auto updateSaveLabel = [saveLabel, saveRow] {
        const QString root = DownloadManager::Root();
        saveLabel->setText(root);
        saveLabel->setToolTip(root);
        saveRow->setToolTip(root);
    };
    updateSaveLabel();
    connect(savePick, &QPushButton::clicked, this, [this, updateSaveLabel] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择下载保存目录"), DownloadManager::Root());
        if (dir.isEmpty())
            return;
        if (!DownloadManager::SetRoot(dir)) {
            emit Toast(QStringLiteral("该目录不可写（可能被系统保护），未修改"));
            return;
        }
        updateSaveLabel();
        emit Toast(QStringLiteral("下载将保存到 %1").arg(DownloadManager::Root()));
    });
    connect(saveReset, &QPushButton::clicked, this, [this, updateSaveLabel] {
        DownloadManager::SetRoot(QString());
        updateSaveLabel();
    });

    auto *sr = BuildGroup(QStringLiteral("超分"));
    // 模型下拉框按 SrEngine::Catalog() 全量生成（原来写死三个下标，新增模型
    // 在设置页根本选不到）。存的是模型名而不是下标：换顺序/增删模型都不会串。
    QStringList srNames;
    const QString curSr = SrEngine::CurrentModel();
    for (const SrEngine::ModelInfo &m : SrEngine::InstalledModels())
        srNames << m.name;
    const QStringList srItems =
        srNames.isEmpty() ? QStringList{QStringLiteral("（没有可用模型）")} : srNames;
    int srIndex = qMax(0, srNames.indexOf(curSr));
    auto *srModelCombo = MakeCombo(srItems, srIndex, sr);
    if (srNames.isEmpty())
        srModelCombo->setEnabled(false);
    connect(srModelCombo, &QComboBox::currentIndexChanged, this, [srNames](int idx) {
        if (idx < 0 || idx >= srNames.size())
            return;
        UiPrefs::Set(QStringLiteral("srModelName"), srNames.at(idx));
        SrEngine::SetModel(srNames.at(idx));
    });
    static_cast<QVBoxLayout *>(sr->layout())
        ->addWidget(new SettingRow(QStringLiteral("默认模型"),
                                   QStringLiteral("阅读器/超分页使用的超分模型（共 %1 个可用）")
                                       .arg(srNames.size()),
                                   srModelCombo, sr));
    auto *npu = new ToggleSwitch(sr);
    npu->setChecked(UiPrefs::GetBool(QStringLiteral("srNpu"), true));
    // QNN/HTP 后端尚未接入：这一项目前不改变实际执行设备，如实说明，
    // 不要让用户以为打开就真的跑在 NPU 上。
    npu->setEnabled(false);
    connect(npu, &ToggleSwitch::toggled, this, [](bool on) {
        UiPrefs::Set(QStringLiteral("srNpu"), on);
    });
    static_cast<QVBoxLayout *>(sr->layout())
        ->addWidget(new SettingRow(QStringLiteral("优先使用 NPU"),
                                   QStringLiteral("当前版本为 CPU 推理（ONNX Runtime），NPU 后端未接入"),
                                   npu, sr));

    body_->addStretch(1);
    root->addWidget(WrapScroll(body, this));
}

QWidget *SettingsView::BuildGroup(const QString &title)
{
    auto *parent = body_->parentWidget();
    body_->addWidget(new SectionHeader(title, QString(), QString(), parent));

    auto *card = new QWidget(parent);
    card->setObjectName(QStringLiteral("ListRow"));
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(14, 4, 14, 4);
    cl->setSpacing(0);
    body_->addWidget(card);
    return card;
}

void SettingsView::UpdateCacheLabel()
{
    if (!cacheLabel_)
        return;
    const QString root = CachePath::Root();
    cacheLabel_->setToolTip(root);
    // 路径可能很长：只显示末两级，完整路径放 tooltip（手机上横排空间只有 ~150dp）
    const QStringList parts =
        QDir::fromNativeSeparators(root).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString shortPath = parts.mid(qMax(0, parts.size() - 2)).join(QLatin1Char('/'));
    if (parts.size() > 2)
        shortPath.prepend(QStringLiteral("…/"));
    cacheLabel_->setText(CachePath::IsCustom()
                             ? shortPath
                             : QStringLiteral("默认（%1）").arg(shortPath));
}

void SettingsView::SetReadingMode(int mode, bool persist)
{
    Q_UNUSED(persist);   // 设置页只是显示当前值；落库由阅读器负责
    if (!readMode_)
        return;
    const QSignalBlocker b(readMode_);
    readMode_->setCurrentIndex(qBound(0, mode, 2));
}

int SettingsView::ReadingMode() const
{
    return readMode_ ? readMode_->currentIndex() : 0;
}

QString SettingsView::CacheDirLabel() const
{
    return CachePath::Root();
}

bool SettingsView::ApplyCacheDir(const QString &path)
{
    if (!CachePath::SetRoot(path))
        return false;
    UpdateCacheLabel();
    emit CachePathChanged(CachePath::Root());
    emit Toast(path.trimmed().isEmpty()
                   ? QStringLiteral("已恢复默认缓存目录：%1").arg(CachePath::Root())
                   : QStringLiteral("缓存目录已切换到：%1").arg(CachePath::Root()));
    return true;
}

// ============================================================ 关于
AboutPage::AboutPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    lay->addWidget(MakeHeader(QStringLiteral("关于"), QStringLiteral("JMComic · Qt C++ 版"),
                              body, nullptr));

    auto *card = new QWidget(body);
    card->setObjectName(QStringLiteral("ListRow"));
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(20, 20, 20, 20);
    cl->setSpacing(10);

    auto *logo = new QLabel(card);
    logo->setPixmap(Glyph::Pixmap(Glyph::Sparkle, 40, T().accent));
    cl->addWidget(logo);

    auto *name = new QLabel(QStringLiteral("JMComic for Windows"), card);
    QFont nf = name->font();
    nf.setPixelSize(UiStyle::Fpx(18));
    nf.setWeight(QFont::Medium);
    name->setFont(nf);
    cl->addWidget(name);

    auto *ver = new QLabel(QStringLiteral("版本 0.88 · Qt 6.12.0 · C++17 · MinGW 13.1"), card);
    ver->setObjectName(QStringLiteral("PageSub"));
    cl->addWidget(ver);

    auto *line = new QFrame(card);
    line->setObjectName(QStringLiteral("Divider"));
    line->setFixedHeight(1);
    cl->addWidget(line);

    const QStringList rows = {
        QStringLiteral("构建工具链：JDK 25 LTS / Gradle 9.8.2 / NDK r30（与 Python 版同一口径）"),
        QStringLiteral("界面层：Qt Widgets，复用 res/theme 下的 6 套 QSS"),
        QStringLiteral("数据层：JmApi（C++，进程内请求，无 Python 子进程）"),
        QStringLiteral("开源协议：MIT · 仅用于学习与备份，请支持正版"),
    };
    for (const QString &r : rows) {
        auto *l = new QLabel(r, card);
        l->setObjectName(QStringLiteral("SettingDesc"));
        l->setWordWrap(true);
        cl->addWidget(l);
    }
    lay->addWidget(card);

    auto *btns = new QHBoxLayout();
    auto *log = new QPushButton(QStringLiteral("导出日志"), body);
    log->setObjectName(QStringLiteral("Ghost"));
    log->setCursor(Qt::PointingHandCursor);
    auto *check = new QPushButton(QStringLiteral("检查更新"), body);
    check->setObjectName(QStringLiteral("Ghost"));
    check->setCursor(Qt::PointingHandCursor);
    btns->addWidget(log);
    btns->addWidget(check);
    btns->addStretch(1);
    lay->addLayout(btns);
    connect(log, &QPushButton::clicked, this, [this] { emit Toast(QStringLiteral("日志导出尚未接入")); });
    connect(check, &QPushButton::clicked, this, [this] { emit Toast(QStringLiteral("更新检查尚未接入")); });

    lay->addStretch(1);
    root->addWidget(WrapScroll(body, this));
}

namespace {

// 登录页的居中列：宽度 = min(可视宽, 420 逻辑像素)，左右由 stretch 平分。
// 不能依赖 QVBoxLayout 的 AlignHCenter —— 那只会给列 sizeHint 宽（约 240），
// 平板上表单会缩成一条；也不能用 UiStyle::Fpx(420)（会随字号缩放）。
class LoginColumnHost : public QWidget
{
public:
    explicit LoginColumnHost(QWidget *parent = nullptr) : QWidget(parent) {}
    QWidget *column = nullptr;

protected:
    void resizeEvent(QResizeEvent *e) override
    {
        QWidget::resizeEvent(e);
        if (column)
            column->setFixedWidth(qMin(width(), 420));
    }
};

}  // namespace

// ============================================================ 登录
LoginPage::LoginPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    // 整页只有一列内容：页头 + 卡片。列在可视区域内水平居中，
    // 上下各一条 stretch 做垂直居中；窗口太矮时列仍然可以滚动。
    auto *host = new LoginColumnHost(body);
    auto *hl = new QHBoxLayout(host);
    hl->setContentsMargins(0, 0, 0, 0);
    hl->setSpacing(0);
    hl->addStretch(1);
    auto *column = new QWidget(host);
    column->setObjectName(QStringLiteral("LoginColumn"));
    host->column = column;
    hl->addWidget(column, 0);
    hl->addStretch(1);

    auto *colLay = new QVBoxLayout(column);
    colLay->setContentsMargins(0, 0, 0, 0);
    colLay->setSpacing(UiStyle::Fpx(14));
    colLay->addWidget(MakeHeader(QStringLiteral("账号"),
                                QStringLiteral("登录后可同步收藏与阅读进度"), column, nullptr));

    auto *card = new QWidget(column);
    card->setObjectName(QStringLiteral("ListRow"));
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(24, 24, 24, 24);
    cl->setSpacing(12);

    auto *avatar = new QLabel(card);
    avatar->setFixedSize(56, 56);
    avatar->setAlignment(Qt::AlignCenter);
    avatar->setText(QStringLiteral("JM"));
    // 圆形头像 → 直角方块（直角矩形风格；半径跟 UiStyle::Radius 走）
    avatar->setStyleSheet(QStringLiteral("background:%1; color:%2; border-radius:%3px; font-size:18px;")
                              .arg(T().accentSoft.name(), T().accent.name())
                              .arg(UiStyle::Radius));
    cl->addWidget(avatar, 0, Qt::AlignHCenter);

    auto *user = new QLineEdit(card);
    user->setObjectName(QStringLiteral("Input"));
    user->setPlaceholderText(QStringLiteral("用户名 / 邮箱"));
    user->setFixedHeight(38);
    cl->addWidget(user);

    auto *pass = new QLineEdit(card);
    pass->setObjectName(QStringLiteral("Input"));
    pass->setPlaceholderText(QStringLiteral("密码"));
    pass->setEchoMode(QLineEdit::Password);
    pass->setFixedHeight(38);
    cl->addWidget(pass);

    auto *login = new QPushButton(QStringLiteral("登录"), card);
    login->setObjectName(QStringLiteral("Primary"));
    UiStyle::MarkSoftButton(login);   // 近似直角（3px）
    login->setCursor(Qt::PointingHandCursor);
    login->setFixedHeight(38);
    loginBtn_ = login;
    cl->addWidget(login);

    auto *links = new QHBoxLayout();
    auto *reg = new QPushButton(QStringLiteral("注册"), card);
    reg->setObjectName(QStringLiteral("Link"));
    reg->setCursor(Qt::PointingHandCursor);
    auto *forgot = new QPushButton(QStringLiteral("忘记密码"), card);
    forgot->setObjectName(QStringLiteral("Link"));
    forgot->setCursor(Qt::PointingHandCursor);
    links->addWidget(reg);
    links->addStretch(1);
    links->addWidget(forgot);
    cl->addLayout(links);

    auto *proxy = new QPushButton(QStringLiteral("代理与节点设置"), card);
    proxy->setObjectName(QStringLiteral("Ghost"));
    proxy->setCursor(Qt::PointingHandCursor);
    cl->addWidget(proxy);

    status_ = new QLabel(QStringLiteral("未登录"), card);
    status_->setObjectName(QStringLiteral("PageSub"));
    cl->addWidget(status_);

    // 退出登录：只有已登录时可见。会话落库后，"重启仍登录"和"退出后重启回到未登录"
    // 都要能验证，所以这里必须有一个显式的清除入口。
    auto *logout = new QPushButton(QStringLiteral("退出登录"), card);
    logout->setObjectName(QStringLiteral("Ghost"));
    logout->setCursor(Qt::PointingHandCursor);
    logoutBtn_ = logout;
    logout->setVisible(UserSession::User().loggedIn);
    cl->addWidget(logout);
    if (UserSession::User().loggedIn)
        status_->setText(QStringLiteral("已登录：%1").arg(UserSession::User().name));

    colLay->addWidget(card);
    // 整页只有这一列：上下 stretch 让内容垂直居中；窗口变矮时列仍可滚动。
    lay->addStretch(1);
    lay->addWidget(host);
    lay->addStretch(1);

    connect(login, &QPushButton::clicked, this, [this, user, pass] {
        if (user->text().trimmed().isEmpty() || pass->text().isEmpty()) {
            emit Toast(QStringLiteral("请填写用户名与密码"));
            return;
        }
        // 真实登录交给外壳：它负责 /login、avs 落库、侧栏刷新。
        // 这里只做"进行中"的视觉反馈 —— 登录失败时不能先把界面改成已登录。
        SetBusy(true);
        status_->setText(QStringLiteral("正在登录…"));
        emit LoginRequested(user->text().trimmed(), pass->text());
    });
    connect(logout, &QPushButton::clicked, this, [this] {
        SessionStore::Clear();
        SetLoggedIn(false);
        emit LogoutRequested();   // 外壳负责清 JmApi 里的 avs
        emit Toast(QStringLiteral("已退出登录"));
        emit Navigate(PageId::Home);
    });
    connect(proxy, &QPushButton::clicked, this, [this] {
        emit Navigate(PageId::Settings);
    });
    connect(reg, &QPushButton::clicked, this, [this] { emit Toast(QStringLiteral("注册页尚未接入")); });
    connect(forgot, &QPushButton::clicked, this, [this] { emit Toast(QStringLiteral("找回密码尚未接入")); });

    root->addWidget(WrapScroll(body, this));
}

void LoginPage::SetStatus(const QString &text, bool ok)
{
    if (!status_)
        return;
    status_->setText(text);
    status_->setStyleSheet(ok ? QString() : QStringLiteral("color:%1;").arg(T().danger.name()));
}

void LoginPage::SetBusy(bool busy)
{
    if (!loginBtn_)
        return;
    loginBtn_->setEnabled(!busy);
    loginBtn_->setText(busy ? QStringLiteral("登录中…") : QStringLiteral("登录"));
}

void LoginPage::SetLoggedIn(bool on, const QString &name)
{
    if (logoutBtn_)
        logoutBtn_->setVisible(on);
    if (on) {
        const QString who = name.isEmpty() ? UserSession::User().name : name;
        SetStatus(who.isEmpty() ? QStringLiteral("已登录") : QStringLiteral("已登录：%1").arg(who), true);
        return;
    }
    SetStatus(QStringLiteral("未登录"), true);
}
