#include "ui_main_window.h"

#include "../doh_client.h"
#include "../quic/quic_http.h"
#include "../net_backend.h"
#include "../tls/tls_channel.h"
#include "jm_api.h"
#include "net_client.h"
#include "download_manager.h"
#include "page_loader.h"
#include "proxy_config.h"
#include "proxy_page.h"
#include "region_code.h"
#include "session.h"
#include "ui_prefs.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QEventLoop>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLayout>
#include <QKeyEvent>
#include <QMenu>
#include <QRegularExpression>
#include <QPropertyAnimation>
#include <QPointer>
#include <QMouseEvent>
#include <QScreen>
#include <QStackedWidget>
#include <QStyle>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QEasingCurve>
#include <QElapsedTimer>
#include <QSharedPointer>
#include <QFrame>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QVariantAnimation>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include "theme.h"
#include "comic_view.h"
#include "cover_cache.h"
#include "app_font.h"
#include "motion.h"
#include "page_transition.h"
#include "touch.h"
#include "ui_kit.h"
#include "../reader.h"

namespace {

struct NavSpec {
    PageId id;
    const char *label;
    Glyph::Name icon;
    int group;   // 0=用户 1=导航 2=其他：对应 Python 侧 label / label_2 / label_3 三段
};

// 顺序、文案、图标全部照抄 src/interface/ui_navigation.py 的 retranslateUi：
//   用户：我的收藏 / 我的评论 / 观看记录 / 本地收藏 / 本地记录
//   导航：首页 / 搜索 / 分类与排行 / 每周必看 / 评论
//   其他：下载 / 网络存储 / 本地漫画 / 图片超分 / 批量超分
// 底部（tailWidget）：帮助 / 设置
const NavSpec kNav[] = {
    {PageId::Favorite,      "我的收藏",   Glyph::Heart,    0},
    {PageId::MyComment,     "我的评论",   Glyph::Comment,  0},
    {PageId::RemoteHistory, "观看记录",   Glyph::Clock,    0},
    {PageId::LocalFavorite, "本地收藏",   Glyph::Folder,   0},
    {PageId::History,       "本地记录",   Glyph::Clock,    0},
    {PageId::Home,          "首页",       Glyph::Home,     1},
    {PageId::Search,        "搜索",       Glyph::Search,   1},
    {PageId::Category,      "分类与排行", Glyph::Grid,     1},
    {PageId::Week,          "每周必看",   Glyph::Calendar, 1},
    {PageId::Comment,       "评论",       Glyph::Comment,  1},
    {PageId::Download,      "下载",       Glyph::Download, 2},
    {PageId::Nas,           "网络存储",   Glyph::Server,   2},
    {PageId::Local,         "本地漫画",   Glyph::Folder,   2},
    {PageId::Sr,            "图片超分",   Glyph::Sparkle,  2},
    {PageId::BatchSr,       "批量超分",   Glyph::Layers,   2},
};

const char *const kGroupTitles[] = {"用户", "导航", "其他"};

// 同级跳转的方向判据：按"用户看到的导航顺序"排位（kNav + 底部 帮助/设置），
// 目标排在后面 = 从右侧滑入。不在导航表里的页（详情/阅读/登录…）退回枚举顺序。
int NavRank(PageId id)
{
    int rank = 0;
    for (const NavSpec &s : kNav) {
        if (s.id == id)
            return rank;
        ++rank;
    }
    if (id == PageId::Help)
        return rank;
    ++rank;
    if (id == PageId::Settings)
        return rank;
    ++rank;
    return rank + int(id);
}

const NavSpec *SpecFor(PageId id)
{
    for (const NavSpec &s : kNav) {
        if (s.id == id)
            return &s;
    }
    return nullptr;
}

// 面包屑 / 页面标题用的名字（Python 取 windowTitle()）
QString PageTitle(PageId id)
{
    if (const NavSpec *s = SpecFor(id))
        return QString::fromUtf8(s->label);
    switch (id) {
    case PageId::Detail:  return QStringLiteral("作品详情");
    case PageId::Reader:  return QStringLiteral("阅读");
    case PageId::Login:   return QStringLiteral("登录");
    case PageId::About:   return QStringLiteral("关于");
    // 帮助 / 设置放在 tailWidget 里，不在 kNav 表中，必须单独给名字，
    // 否则面包屑会退到 default 显示成"首页"。
    case PageId::Help:    return QStringLiteral("帮助");
    case PageId::Settings:return QStringLiteral("设置");
    case PageId::Proxy:   return QStringLiteral("分流与节点");
    default:              return QStringLiteral("首页");
    }
}

const UiTokens &T() { return UiTheme::I().Tokens(); }

#ifdef Q_OS_ANDROID

// 沉浸式全屏：Android 上 Qt 的 showFullScreen() 只改窗口状态，状态栏与手势条
// 仍由系统绘制在窗口上层（真机实测：调用后画面尺寸没变，状态栏照样压着顶部）。
// 真正让系统栏让位要动 Activity 的 decorView：
//   IMMERSIVE_STICKY 让系统栏隐藏后，用户从边缘划出也只是半透明浮现，几秒后自动收回；
//   其余四个 LAYOUT_*/HIDE_* 保证窗口铺到系统栏底下而不是被"缩"回去。
// 关闭时清 0 即可恢复系统默认（含 Activity 主题里的状态栏）。
void AndroidSetSystemBarsHidden(bool hidden)
{
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid()) {
        qWarning("fullscreen: no android activity");
        return;
    }
    QJniObject window = activity.callObjectMethod("getWindow", "()Landroid/view/Window;");
    if (!window.isValid())
        return;
    QJniObject decor = window.callObjectMethod("getDecorView", "()Landroid/view/View;");

    // 路径 A（API 30+，Android 15 上唯一完全生效的一条）：
    //   WindowInsetsController.hide/systemBars()，并把行为设成
    //   BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE(2)：边缘划出时系统栏只临时浮现。
    //   Type.systemBars() = statusBars(1) | navigationBars(2) = 3。
    bool viaController = false;
    QJniObject controller = window.callObjectMethod("getInsetsController",
                                                    "()Landroid/view/WindowInsetsController;");
    if (controller.isValid()) {
        controller.callMethod<void>("setSystemBarsBehavior", "(I)V", jint(2));
        if (hidden)
            controller.callMethod<void>("hide", "(I)V", jint(3));
        else
            controller.callMethod<void>("show", "(I)V", jint(3));
        viaController = true;
    }

    // 路径 B（旧 API 兜底）：decorView 的 systemUiVisibility。
    // Android 15 对 targetSdk 35 已忽略其中的 LAYOUT_* 位，但 HIDE_* 位在
    // 老设备/老 ROM 上仍然有效，两条都调一次才覆盖全版本。
    if (decor.isValid()) {
        constexpr jint kLayoutStable = 0x00000100;
        constexpr jint kLayoutHideNav = 0x00000200;
        constexpr jint kLayoutFullscreen = 0x00000400;
        constexpr jint kHideNav = 0x00000002;
        constexpr jint kFullscreen = 0x00000004;
        constexpr jint kImmersiveSticky = 0x00001000;
        const jint flags = hidden ? (kLayoutStable | kLayoutHideNav | kLayoutFullscreen | kHideNav
                                     | kFullscreen | kImmersiveSticky)
                                  : 0;
        decor.callMethod<void>("setSystemUiVisibility", "(I)V", flags);
    }
    const jint readback = decor.isValid()
                              ? decor.callMethod<jint>("getSystemUiVisibility", "()I")
                              : -1;
    qWarning("fullscreen: android immersive=%d controller=%d decor=%d sysui=0x%x", int(hidden),
             int(viaController), int(decor.isValid()), unsigned(readback));
}

#endif   // Q_OS_ANDROID

}  // namespace

MainWindow::~MainWindow()
{
    // 退出前把下载进度写盘：正在跑的任务会被标成"已中断"，下次启动能看到
    // 并继续，而不是留一个永远停在 60% 的假任务。
    if (downloads_)
        downloads_->Shutdown();
}

MainWindow::MainWindow(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("Root"));
    setWindowTitle(QStringLiteral("JMComic"));
    setWindowFlags(Qt::FramelessWindowHint | Qt::Window | Qt::WindowMinMaxButtonsHint);
    setAttribute(Qt::WA_TranslucentBackground, false);
    setMouseTracking(true);
    resize(1280, 840);
    setMinimumSize(980, 660);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    rootLayout_ = root;
    titleBar_ = BuildTitleBar();
    root->addWidget(titleBar_);

    auto *body = new QHBoxLayout();
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    bodyLayout_ = body;
    navRail_ = BuildNavRail();
    body->addWidget(navRail_);

    // 右列：面包屑条 + 页面栈。对应 ui_main.ui 里 navigationWidget 右边那套
    // verticalLayout：先 horizontalLayout_3（menuLayout），再 subStackWidget。
    auto *rightCol = new QVBoxLayout();
    rightCol->setContentsMargins(0, 0, 0, 0);
    rightCol->setSpacing(0);

    crumbBar_ = BuildCrumbBar();
    rightCol->addWidget(crumbBar_);

    stack_ = new QStackedWidget(this);
    stack_->setObjectName(QStringLiteral("PageStack"));
    rightCol->addWidget(stack_, 1);
    body->addLayout(rightCol, 1);
    root->addLayout(body, 1);

    BuildPages();
    UpdateNavState();

    // 全局点击反馈：所有按钮按下即出涟漪（位置跟手指/光标一致）。
    // 放在外壳安装而不是每个页面各装一次，保证"任何入口的按钮观感一致"。
    Motion::InstallTapFeedback();

    // 菜单栏/弹出菜单的防误触：滑动手势不得被当成点击（见 touch.h 的两条阈值）。
    // 也装在外壳：侧栏、阅读器菜单、下拉框弹窗走同一条判定。
    Touch::InstallTapGuard();


    // ---- 换页动画覆盖层（QML）----
    // host 传 stack_：覆盖层挂在窗口上、几何跟随页面栈，不会被 QStackedWidget 切走。
    transition_ = new PageTransition(stack_);
    connect(transition_, &PageTransition::FirstFrame, this, [this] {
        // 覆盖层已经画出内容，这时藏旧页不会闪黑（对应 Python 版 _OnCoverFirstFrame）
        if (leavingPage_)
            leavingPage_->hide();
    });
    connect(transition_, &PageTransition::Finished, this, &MainWindow::LandPending);
    connect(transition_, &PageTransition::Failed, this, [this](const QString &why) {
        std::fprintf(stderr, "main-window: transition failed (%s\n)", qPrintable(why));
        if (pendingIdx_ < 0)
            return;
        const int idx = pendingIdx_;
        pendingIdx_ = -1;
        leavingPage_ = nullptr;
        ApplyPage(idx);
        if (transition_)
            transition_->End();
    });

    // ---- 触摸适配层 ----
    touch_ = new TouchAdapter(this);
    connect(touch_, &TouchAdapter::BackRequested, this, &MainWindow::Back);
    touch_->Install();
    ApplySafeArea();

    // ---- 手机形态（编译期判定，桌面不受影响）----
    // 之前只有命令行 --touch/--drawer 才会切抽屉，导致 Android 上直接跑出来是
    // "260px 常驻侧栏 + 980px 最小宽度"的桌面布局，屏幕右边被切掉一大块。
    // 手机上应该：隐藏自绘标题栏（最小化/最大化/关闭没有意义）→ 触摸 + 抽屉导航
    // → 窗口铺满可用区域。
    if (Touch::IsMobile()) {
        if (titleBar_)
            titleBar_->hide();
        EnableTouchMode(true);   // 内含 SetDrawerMode(true)，最小尺寸降到 320×480
        UpdateMobileBreakpoint();   // 平板/折叠屏展开（>=600dp）改回常驻侧栏
        // 横屏只有 411dp 高：480 的下限会把底部工具条顶出屏幕，手机形态统一放到 320。
        setMinimumSize(UiStyle::Fpx(320), UiStyle::Fpx(320));
        if (QScreen *s = QGuiApplication::primaryScreen())
            resize(s->availableGeometry().size());
        // 返回键：Qt 把 Android back 当 Qt::Key_Back 送给"焦点控件"，没有焦点控件时
        // 事件直接被丢掉、Activity 被系统 finish（实测：设置页按返回会退到桌面）。
        // 主窗口自己拿焦点，keyPressEvent 才有机会处理（子控件不处理时也会冒泡上来）。
        setFocusPolicy(Qt::StrongFocus);
        setFocus(Qt::OtherFocusReason);
    }

    connect(&AppFont::I(), &AppFont::Changed, this, &MainWindow::UpdateNavMetrics);

    connect(&UiTheme::I(), &UiTheme::Changed, this, [this] {
        UpdateIcons();
        update();
    });
}

QWidget *MainWindow::BuildTitleBar()
{
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("TitleBar"));
    bar->setFixedHeight(44);
    bar->installEventFilter(this);

    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(16, 0, 8, 0);
    lay->setSpacing(10);

    auto *logo = new QLabel(bar);
    logo->setPixmap(Glyph::Pixmap(Glyph::Sparkle, 18, T().accent));
    lay->addWidget(logo);

    auto *brand = new QLabel(QStringLiteral("JMComic"), bar);
    brand->setObjectName(QStringLiteral("Brand"));
    lay->addWidget(brand);

    auto *sub = new QLabel(QStringLiteral("C++ UI"), bar);
    sub->setObjectName(QStringLiteral("BrandSub"));
    lay->addWidget(sub);
    lay->addStretch(1);

    auto *themeBtn = new QToolButton(bar);
    themeBtn->setObjectName(QStringLiteral("WinBtn"));
    themeBtn->setFixedSize(32, 30);
    themeBtn->setCursor(Qt::PointingHandCursor);
    themeBtn->setToolTip(QStringLiteral("切换明暗主题"));
    themeBtn->setIcon(Glyph::Icon(T().dark ? Glyph::Sun : Glyph::Moon, 15, T().textDim));
    themeBtn->setIconSize(QSize(15, 15));
    lay->addWidget(themeBtn);
    connect(themeBtn, &QToolButton::clicked, this, [this, themeBtn] {
        // 换主题会重刷全站 QSS（几十到上百毫秒），直接切会有一次"半新半旧"的闪烁。
        // 用整窗交叉淡出盖住这次重排：旧画面先顶上，换完再淡掉。
        Motion::CrossFade(this, [this, themeBtn] {
            UiTheme::I().ToggleDark();
            themeBtn->setIcon(Glyph::Icon(T().dark ? Glyph::Sun : Glyph::Moon, 15, T().textDim));
        }, Motion::Base);
        Toast::Show(this, UiTheme::I().IsDark() ? QStringLiteral("已切换到深色主题")
                                                : QStringLiteral("已切换到浅色主题"));
    });

    auto *minBtn = new QToolButton(bar);
    minBtn->setObjectName(QStringLiteral("WinBtn"));
    minBtn->setFixedSize(38, 30);
    minBtn->setCursor(Qt::PointingHandCursor);
    minBtn->setIcon(Glyph::Icon(Glyph::Minimize, 14, T().textDim));
    minBtn->setIconSize(QSize(14, 14));
    lay->addWidget(minBtn);
    connect(minBtn, &QToolButton::clicked, this, &QWidget::showMinimized);

    auto *maxBtn = new QToolButton(bar);
    maxBtn->setObjectName(QStringLiteral("WinBtn"));
    maxBtn->setFixedSize(38, 30);
    maxBtn->setCursor(Qt::PointingHandCursor);
    maxBtn->setIcon(Glyph::Icon(Glyph::Maximize, 13, T().textDim));
    maxBtn->setIconSize(QSize(13, 13));
    lay->addWidget(maxBtn);
    connect(maxBtn, &QToolButton::clicked, this, [this] {
        isMaximized() ? showNormal() : showMaximized();
    });

    auto *closeBtn = new QToolButton(bar);
    closeBtn->setObjectName(QStringLiteral("WinBtnClose"));
    closeBtn->setStyleSheet(QStringLiteral("QToolButton#WinBtnClose{background:transparent;border:none;"
                                           "border-radius:%1px;}").arg(UiStyle::Radius));
    closeBtn->setFixedSize(38, 30);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setIcon(Glyph::Icon(Glyph::Close, 14, T().textDim));
    closeBtn->setIconSize(QSize(14, 14));
    lay->addWidget(closeBtn);
    connect(closeBtn, &QToolButton::clicked, this, &QWidget::close);

    // 拖动 / 双击最大化
    bar->setProperty("dragArea", true);
    return bar;
}

QWidget *MainWindow::BuildNavRail()
{
    // Python NavigationWidget：固定 260 宽，纵向 = topWidget(用户区) +
    // scrollArea(三段导航) + tailWidget(帮助/设置)。
    auto *rail = new QWidget(this);
    rail->setObjectName(QStringLiteral("NavRail"));
    rail->setFixedWidth(UiStyle::Fpx(260));

    auto *outer = new QVBoxLayout(rail);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // ---------------- topWidget：用户区 ----------------
    auto *top = new QWidget(rail);
    top->setObjectName(QStringLiteral("NavTop"));
    auto *tl = new QVBoxLayout(top);
    tl->setContentsMargins(UiStyle::Fpx(8), UiStyle::Fpx(8), UiStyle::Fpx(8), UiStyle::Fpx(6));
    tl->setSpacing(UiStyle::Fpx(4));

    navAvatar_ = new QLabel(top);
    navAvatar_->setObjectName(QStringLiteral("NavAvatar"));
    navAvatar_->setAlignment(Qt::AlignCenter);
    navAvatar_->setFixedSize(UiStyle::Fpx(100), UiStyle::Fpx(100));
    navAvatar_->setCursor(Qt::PointingHandCursor);
    navAvatar_->setToolTip(QStringLiteral("点击登录"));
    {
        // 与 Python 同一个资源：res/icon/placeholder_avatar.png（qrc 里别名为
        // assets/placeholder_avatar.png）。之前用主色方块 + "JM"，是自造的占位。
        const QPixmap pm(QStringLiteral(":/ui/assets/placeholder_avatar.png"));
        if (!pm.isNull())
            navAvatar_->setPixmap(pm.scaled(navAvatar_->size(), Qt::KeepAspectRatioByExpanding,
                                            Qt::SmoothTransformation));
        else
            navAvatar_->setText(QStringLiteral("JM"));   // 资源缺失时的兜底
    }
    tl->addWidget(navAvatar_, 0, Qt::AlignHCenter);

    auto *authRow = new QHBoxLayout();
    authRow->setContentsMargins(0, 0, 0, 0);
    authRow->setSpacing(UiStyle::Fpx(8));
    authRow->addStretch(1);
    auto *login = new QPushButton(QStringLiteral("登录"), top);
    login->setObjectName(QStringLiteral("Primary"));
    UiStyle::MarkSoftButton(login);   // 近似直角（3px），与"确定/开始阅读"同一套造型
    login->setCursor(Qt::PointingHandCursor);
    navAuthBtn_ = login;
    auto *sign = new QPushButton(QStringLiteral("打卡"), top);
    sign->setObjectName(QStringLiteral("Ghost"));
    UiStyle::MarkSoftButton(sign);
    sign->setCursor(Qt::PointingHandCursor);
    signBtn_ = sign;
    authRow->addWidget(login);
    authRow->addWidget(sign);
    authRow->addStretch(1);
    tl->addLayout(authRow);

    connect(login, &QPushButton::clicked, this, [this] { Go(PageId::Login); });
    // 打卡走真实接口：未登录先提示并跳登录页（0.90 之前是一句"打卡成功（演示）"，
    // 登录后也一样——用户报的"登录账号后无法打卡"就是这个假实现）。
    connect(sign, &QPushButton::clicked, this, &MainWindow::SignIn);

    // 用户名 / 称号
    auto *nameRow = new QHBoxLayout();
    nameRow->setContentsMargins(0, 0, 0, 0);
    nameRow->setSpacing(UiStyle::Fpx(6));
    navName_ = new QLabel(top);
    navName_->setObjectName(QStringLiteral("NavName"));
    navTitle_ = new QLabel(top);
    navTitle_->setObjectName(QStringLiteral("NavTitle"));
    nameRow->addWidget(navName_);
    nameRow->addWidget(navTitle_, 1);
    tl->addLayout(nameRow);

    // 键值行：顺序与 ui_navigation.ui 完全一致
    auto addKv = [top, tl](const QString &key, bool accent) -> QLabel * {
        auto *row = new QWidget(top);
        auto *l = new QHBoxLayout(row);
        l->setContentsMargins(0, 0, 0, 0);
        l->setSpacing(UiStyle::Fpx(6));
        auto *k = new QLabel(key, row);
        k->setObjectName(QStringLiteral("NavKey"));
        auto *v = new QLabel(row);
        v->setObjectName(accent ? QStringLiteral("NavValueAccent") : QStringLiteral("NavValue"));
        l->addWidget(k);
        l->addWidget(v, 1);
        tl->addWidget(row);
        return v;
    };
    navCoins_ = addKv(QStringLiteral("J Coins："), true);
    navLevel_ = addKv(QStringLiteral("等级:"), false);
    navFav_ = addKv(QStringLiteral("收藏数："), false);

    // 三条"可交互"行：分流（接口）/ 分流（图床）/ 屏蔽词。
    // 只读行（Coins/等级/收藏数）是一行纯文字；这三行必须一眼看出能点，
    // 所以整行做一个带边框的按钮块，并把点击区域从"几个字"扩到整行。
    auto addAction = [this, top, tl](const QString &key, const QString &actionId,
                                     const QString &tip) -> QLabel * {
        auto *row = new QWidget(top);
        row->setObjectName(QStringLiteral("NavAction"));
        row->setProperty("navAction", actionId);
        row->setProperty("navActionBordered", true);
        row->setCursor(Qt::PointingHandCursor);
        row->setAttribute(Qt::WA_Hover, true);
        row->setToolTip(tip);
        auto *l = new QHBoxLayout(row);
        l->setContentsMargins(UiStyle::Fpx(8), UiStyle::Fpx(5), UiStyle::Fpx(6), UiStyle::Fpx(5));
        l->setSpacing(UiStyle::Fpx(6));
        auto *k = new QLabel(key, row);
        k->setObjectName(QStringLiteral("NavKey"));
        auto *v = new QLabel(row);
        v->setObjectName(QStringLiteral("NavValue"));
        auto *chev = new QLabel(row);
        chev->setPixmap(Glyph::Pixmap(Glyph::ChevronRight, UiStyle::Fpx(12), T().textFaint));
        chev->setFixedSize(UiStyle::Fpx(12), UiStyle::Fpx(12));
        l->addWidget(k);
        l->addWidget(v, 1);
        l->addWidget(chev);
        // 手指在行上按下/抬起都要能命中（子 QLabel 不接鼠标，事件会冒泡到 row）
        row->installEventFilter(this);
        tl->addWidget(row);
        navActions_.push_back(row);
        return v;
    };
    navProxy_ = addAction(QStringLiteral("分流："), QStringLiteral("proxy"),
                          QStringLiteral("接口线路与节点测速"));
    navProxyImg_ = addAction(QStringLiteral("分流："), QStringLiteral("proxyImg"),
                             QStringLiteral("图床线路与节点测速"));
    navHide_ = addAction(QStringLiteral("屏蔽词："), QStringLiteral("hide"),
                         QStringLiteral("编辑屏蔽词（分类 / 标签 / 标题命中即过滤）"));
    UpdateNavUser();

    outer->addWidget(top);

    // ---------------- scrollArea：三段导航 ----------------
    auto *scroll = new QScrollArea(rail);
    scroll->setObjectName(QStringLiteral("NavScroll"));
    navScroll_ = scroll;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // 滚动时第一个可见项会被视口上沿裁成半行，正好贴着用户区那根分隔线。
    // setViewportMargins 是 protected，这里用一个带 4px 边距的容器包一层。

    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("NavContent"));
    auto *cl = new QVBoxLayout(content);
    cl->setContentsMargins(0, UiStyle::Fpx(9), UiStyle::Fpx(2), UiStyle::Fpx(12));
    cl->setSpacing(UiStyle::Fpx(6));

    int lastGroup = -1;
    for (const NavSpec &spec : kNav) {
        if (spec.group != lastGroup) {
            if (lastGroup >= 0) {
                // Python 在分段之间放了一条 HLine
                auto *line = new QFrame(content);
                line->setObjectName(QStringLiteral("NavLine"));
                line->setFrameShape(QFrame::HLine);
                line->setFixedHeight(1);
                cl->addWidget(line);
                navSeparators_.push_back(line);
            }
            auto *label = new QLabel(QString::fromUtf8(kGroupTitles[spec.group]), content);
            label->setObjectName(QStringLiteral("NavGroup"));
            cl->addWidget(label);
            lastGroup = spec.group;
        }

        auto *btn = new QToolButton(content);
        btn->setObjectName(QStringLiteral("NavItem"));
        btn->setText(QString::fromUtf8(spec.label));
        // Python：ToolButtonTextBesideIcon + iconSize 32 + 最小 150x40
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        btn->setIcon(Glyph::Icon(spec.icon, UiStyle::Fpx(32), T().textDim));
        btn->setIconSize(QSize(UiStyle::Fpx(32), UiStyle::Fpx(32)));
        btn->setMinimumHeight(UiStyle::Fpx(40));
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setCheckable(true);
        btn->setAutoExclusive(false);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        cl->addWidget(btn);
        navItems_.push_back({spec.id, btn});
        connect(btn, &QToolButton::clicked, this, [this, id = spec.id] { Go(id, false); });
    }
    cl->addStretch(1);
    scroll->setWidget(content);
    // 侧栏也只允许纵向滚动：横向拖动会把导航整块拖离左边缘
    Touch::LockHorizontalPan(scroll);

    auto *scrollHost = new QWidget(rail);
    auto *shl = new QVBoxLayout(scrollHost);
    shl->setContentsMargins(0, UiStyle::Fpx(4), 0, UiStyle::Fpx(4));
    shl->setSpacing(0);
    shl->addWidget(scroll);
    outer->addWidget(scrollHost, 1);

    // ---------------- tailWidget：帮助 / 设置 ----------------
    auto *tail = new QWidget(rail);
    tail->setObjectName(QStringLiteral("NavTail"));
    auto *tal = new QVBoxLayout(tail);
    tal->setContentsMargins(0, UiStyle::Fpx(6), 0, UiStyle::Fpx(8));
    tal->setSpacing(UiStyle::Fpx(6));
    const struct {
        PageId id;
        const char *label;
        Glyph::Name icon;
    } tailItems[] = {
        {PageId::Help, "帮助", Glyph::Info},
        {PageId::Settings, "设置", Glyph::Gear},
    };
    for (const auto &it : tailItems) {
        auto *btn = new QToolButton(tail);
        btn->setObjectName(QStringLiteral("NavItem"));
        btn->setText(QString::fromUtf8(it.label));
        btn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        btn->setIcon(Glyph::Icon(it.icon, UiStyle::Fpx(32), T().textDim));
        btn->setIconSize(QSize(UiStyle::Fpx(32), UiStyle::Fpx(32)));
        btn->setMinimumHeight(UiStyle::Fpx(40));
        btn->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        tal->addWidget(btn);
        navItems_.push_back({it.id, btn});
        connect(btn, &QToolButton::clicked, this, [this, id = it.id] { Go(id, false); });
    }
    outer->addWidget(tail);

    return rail;
}

QWidget *MainWindow::BuildCrumbBar()
{
    // Python menuLayout：左边一个 menuButton（收起侧栏），右边是"首页 > 详情 > 阅读"
    auto *bar = new QWidget(this);
    bar->setObjectName(QStringLiteral("CrumbBar"));
    bar->setFixedHeight(UiStyle::Fpx(44));
    // 面包屑不该反过来决定窗口最小宽度（长标题 + 大字号时会顶到 400+）
    bar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);

    auto *lay = new QHBoxLayout(bar);
    lay->setContentsMargins(UiStyle::Fpx(8), 0, UiStyle::Fpx(12), 0);
    lay->setSpacing(UiStyle::Fpx(4));

    crumbMenu_ = new QToolButton(bar);
    crumbMenu_->setObjectName(QStringLiteral("CrumbMenu"));
    crumbMenu_->setFixedSize(UiStyle::Fpx(30), UiStyle::Fpx(30));
    crumbMenu_->setCursor(Qt::PointingHandCursor);
    crumbMenu_->setToolTip(QStringLiteral("收起 / 展开侧栏"));
    crumbMenu_->setIcon(Glyph::Icon(Glyph::Menu, UiStyle::Fpx(15), T().textDim));
    crumbMenu_->setIconSize(QSize(UiStyle::Fpx(15), UiStyle::Fpx(15)));
    lay->addWidget(crumbMenu_);
    connect(crumbMenu_, &QToolButton::clicked, this, [this] {
        // Python：CheckShowMenu 里 mobile_ui.ToggleDrawer 优先，桌面才走 aniShow/aniHide
        if (drawerMode_)
            ToggleDrawer();
        else
            ToggleNav();
    });

    crumbLayout_ = new QHBoxLayout();
    crumbLayout_->setContentsMargins(0, 0, 0, 0);
    crumbLayout_->setSpacing(UiStyle::Fpx(4));
    lay->addLayout(crumbLayout_);
    lay->addStretch(1);
    return bar;
}

void MainWindow::RebuildCrumbs()
{
    if (!crumbLayout_ || !crumbBar_)
        return;
    // 面包屑是"层级摘要"，换页时内容整行换掉。直接重建会让整行"啪"地跳一下，
    // 用旧画面淡出（120ms）把这次替换藏起来；动画关掉时走同步路径。
    if (!Motion::Enabled() || !crumbBar_->isVisible()) {
        RebuildCrumbsNow();
        return;
    }
    Motion::CrossFade(crumbBar_, [this] { RebuildCrumbsNow(); }, Motion::Fast);
}

void MainWindow::RebuildCrumbsNow()
{
    if (!crumbLayout_ || !crumbBar_)
        return;
    while (QLayoutItem *item = crumbLayout_->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    QVector<PageId> path = history_;
    if (path.isEmpty() || path.last() != current_)
        path.push_back(current_);
    // 连续重复（同一页重复进入）只留一个
    QVector<PageId> uniq;
    for (PageId id : path) {
        if (uniq.isEmpty() || uniq.last() != id)
            uniq.push_back(id);
    }

    for (int i = 0; i < uniq.size(); ++i) {
        // 手机上路径超过 3 级时只保留"首页 … 上一级 当前"，
        // 否则一行面包屑比屏幕还宽（会被裁掉，还把窗口顶宽）
        if (drawerMode_ && uniq.size() > 3 && i > 0 && i < uniq.size() - 2) {
            if (i == 1) {
                auto *sep = new QLabel(QStringLiteral("…"), crumbBar_);
                sep->setObjectName(QStringLiteral("CrumbSep"));
                crumbLayout_->addWidget(sep);
            }
            continue;
        }
        if (i > 0) {
            auto *sep = new QLabel(QStringLiteral(">"), crumbBar_);
            sep->setObjectName(QStringLiteral("CrumbSep"));
            crumbLayout_->addWidget(sep);
        }
        auto *btn = new QToolButton(crumbBar_);
        btn->setObjectName(QStringLiteral("Crumb"));
        btn->setText(PageTitle(uniq.at(i)));
        btn->setCheckable(true);
        btn->setChecked(i == uniq.size() - 1);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        crumbLayout_->addWidget(btn);
        const int index = i;
        connect(btn, &QToolButton::clicked, this, [this, index] { GoToCrumb(index); });
    }
}

void MainWindow::GoToCrumb(int index)
{
    QVector<PageId> path = history_;
    if (path.isEmpty() || path.last() != current_)
        path.push_back(current_);
    if (index < 0 || index >= path.size() - 1)
        return;   // 点的是当前页
    // 回到第 index 级：把返回栈截到它之前，再"无动画"地切过去
    history_ = path.mid(0, index);
    // 点面包屑是往上退一级，用返回语义（从左侧推回），比同级淡入更符合直觉
    backNext_ = true;
    Go(path.at(index), false);
}

void MainWindow::ToggleNav()
{
    if (!navRail_)
        return;
    const int full = UiStyle::Fpx(260);
    auto *anim = new QVariantAnimation(this);
    // 85ms 太短：宽度跳变在中低端机上只来得及出 3~4 帧，看起来像"瞬间贴边"。
    // 180ms + OutCubic 让边缘有一次可读的收束过程。
    anim->setDuration(Motion::Base);
    anim->setEasingCurve(Motion::EnterCurve());
    if (!navCollapsed_) {
        anim->setStartValue(navRail_->width());
        anim->setEndValue(0);
        connect(anim, &QVariantAnimation::finished, this, [this] { navRail_->hide(); });
        navCollapsed_ = true;
    } else {
        navRail_->show();
        anim->setStartValue(0);
        anim->setEndValue(full);
        navCollapsed_ = false;
    }
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        navRail_->setFixedWidth(v.toInt());
    });
    anim->start(QAbstractAnimation::DeleteWhenStopped);
}

void MainWindow::BuildPages()
{
    home_ = new HomePage(this);
    search_ = new SearchPage(this);
    detail_ = new DetailPage(this);
    reader_ = new ReaderPage(this);

    pages_.resize(int(PageId::Proxy) + 1);
    pages_[int(PageId::Home)] = home_;
    pages_[int(PageId::Category)] = new CategoryPage(this);
    pages_[int(PageId::Search)] = search_;
    pages_[int(PageId::Detail)] = detail_;
    pages_[int(PageId::Reader)] = reader_;
    pages_[int(PageId::Download)] = new DownloadPage(this);
    pages_[int(PageId::Local)] = new LocalPage(this);
    pages_[int(PageId::Favorite)] = new FavoritePage(this);
    pages_[int(PageId::History)] = new HistoryPage(this);
    pages_[int(PageId::Comment)] = new CommentPage(this);
    pages_[int(PageId::Sr)] = new SrPage(this);
    pages_[int(PageId::Settings)] = new SettingsView(this);
    pages_[int(PageId::About)] = new AboutPage(this);
    pages_[int(PageId::Login)] = new LoginPage(this);
    // 对齐 Python 导航栏时补齐的 7 个页面（见 docs/cpp_ui_parity.md §13）
    pages_[int(PageId::MyComment)] = new MyCommentPage(this);
    pages_[int(PageId::RemoteHistory)] = new RemoteHistoryPage(this);
    pages_[int(PageId::LocalFavorite)] = new LocalFavoritePage(this);
    pages_[int(PageId::Week)] = new WeekPage(this);
    pages_[int(PageId::Nas)] = new NasPage(this);
    pages_[int(PageId::BatchSr)] = new BatchSrPage(this);
    pages_[int(PageId::Help)] = new HelpPage(this);
    pages_[int(PageId::Proxy)] = new ProxyPage(this);

    for (int i = 0; i < pages_.size(); ++i) {
        if (!pages_.at(i))
            continue;
        stack_->addWidget(pages_.at(i));
        WirePage(pages_.at(i), PageId(i));
    }

    // 卡片 → 详情（真实条目会顺带拉 /album）
    connect(home_, &HomePage::OpenComic, this, &MainWindow::OpenDetail);
    connect(search_, &SearchPage::OpenComic, this, &MainWindow::OpenDetail);
    connect(qobject_cast<CategoryPage *>(pages_[int(PageId::Category)]), &CategoryPage::OpenComic,
            this, &MainWindow::OpenDetail);
    // 分类页的筛选只是入口：真正结果由 /search 返回并落到搜索页
    connect(qobject_cast<CategoryPage *>(pages_[int(PageId::Category)]),
            &CategoryPage::FilterRequested, this,
            [this](const QString &keyword, const QString &) { RealSearch(keyword); });
    connect(qobject_cast<FavoritePage *>(pages_[int(PageId::Favorite)]), &FavoritePage::OpenComic,
            this, &MainWindow::OpenDetail);

    // 搜索 / 登录 / 收藏：页面只发意图，网络与落库都在外壳
    connect(search_, &SearchPage::SearchRequested, this, [this](const QString &kw, int page) {
        EnsureApi();
        qWarning("search-req: %s p%d\n", qPrintable(kw), page);
        api_->Search(kw, page);
    });
    auto *loginPage = qobject_cast<LoginPage *>(pages_[int(PageId::Login)]);
    connect(loginPage, &LoginPage::LoginRequested, this, [this](const QString &u, const QString &p) {
        EnsureApi();
        api_->Login(u, p);
    });
    connect(loginPage, &LoginPage::LogoutRequested, this, [this] {
        if (api_) {
            api_->RestoreSession(QByteArray(), QString());
            api_->SetUid(0);
        }
        // 退出后收藏页不能再留着上一个账号的卡片
        if (auto *fp = qobject_cast<FavoritePage *>(pages_[int(PageId::Favorite)]))
            fp->SetState(FavoritePage::State::NotLoggedIn);
        if (auto *lp = qobject_cast<LoginPage *>(pages_[int(PageId::Login)]))
            lp->SetLoggedIn(false);
        UpdateNavUser();
    });
    connect(qobject_cast<FavoritePage *>(pages_[int(PageId::Favorite)]),
            &FavoritePage::RefreshRequested, this, &MainWindow::RefreshFavoriteList);
    // 收藏页空状态里的"去登录"
    connect(qobject_cast<FavoritePage *>(pages_[int(PageId::Favorite)]),
            &FavoritePage::LoginRequested, this, [this] { Go(PageId::Login); });

    // 分流页：选择变化立刻应用到 API / 封面 / 页图
    connect(qobject_cast<ProxyPage *>(pages_[int(PageId::Proxy)]), &ProxyPage::Applied,
            this, &MainWindow::ApplyProxySettings);

    // 下载队列：全局只建一份（下载页展示、详情页入队、退出时落盘）。
    // 以前这里是"下载队列尚未接入"的 toast —— 入口点了没反应，本版补上真实队列。
    EnsureApi();
    downloads_ = new DownloadManager(api_, this);
    connect(downloads_, &DownloadManager::Log, this,
            [](const QString &l) { qWarning("dl: %s", qPrintable(l)); });
    if (auto *dp = qobject_cast<DownloadPage *>(pages_[int(PageId::Download)])) {
        dp->SetManager(downloads_);
        connect(dp, &DownloadPage::OpenDownloaded, this, [this](const QString &dir) {
            if (auto *lp = qobject_cast<LocalPage *>(pages_[int(PageId::Local)]))
                lp->OpenDir(dir);
            Go(PageId::Local);
        });
    }
    // 上次没跑完的任务先标成"已中断"，让用户自己决定继续（不静默重跑）
    downloads_->LoadPersisted();

    // 阅读方式：设置页下拉 → 阅读器（同一份偏好，重启后仍生效）
    if (auto *sv = qobject_cast<SettingsView *>(pages_[int(PageId::Settings)])) {
        connect(sv, &SettingsView::ReadingModeChanged, this, [this](int mode) {
            if (reader_)
                reader_->SetReadingMode(mode);
        });
        connect(sv, &SettingsView::ReadingDirectionChanged, this, [this](bool rtl) {
            if (reader_)
                reader_->SetDirection(rtl);
        });
        connect(sv, &SettingsView::ReadingZoomChanged, this, [this](int percent) {
            if (reader_)
                reader_->SetZoomPercent(percent);
        });
        connect(sv, &SettingsView::ReadingAutoSrChanged, this, [this](bool on) {
            if (reader_)
                reader_->SetAutoSr(on);
        });
        sv->SetReadingMode(ReaderView::PrefMode());
    }
    if (reader_)
        reader_->SetReadingMode(ReaderView::PrefMode(), false);

    // 阅读器：章末自动跳章、全屏、返回
    if (reader_) {
        connect(reader_, &ReaderPage::RequestNextChapter, this, [this] { OpenAdjacentChapter(+1); });
        connect(reader_, &ReaderPage::RequestPrevChapter, this, [this] { OpenAdjacentChapter(-1); });
        connect(reader_, &ReaderPage::RequestFullScreen, this, &MainWindow::ToggleReaderFullScreen);
        connect(reader_, &ReaderPage::RequestBack, this, &MainWindow::Back);
        // 继续阅读：每次换页落库；恢复期间（resumePending_）不覆盖上次进度
        connect(reader_, &ReaderPage::ProgressChanged, this, [this](int page, int) {
            if (page > 0 && !resumePending_ && !currentChapterId_.isEmpty())
                UiPrefs::Set(QStringLiteral("readerLastPage"), page);
        });
    }

    // 首页"加载更多"：/latest 翻页
    connect(home_, &HomePage::LoadMoreLatest, this, [this](int page) {
        EnsureApi();
        api_->FetchLatestPage(page);
    });
    // 首页错误态的"重试"：重新拉一次 /promote + /latest
    connect(home_, &HomePage::RetryRequested, this, &MainWindow::LoadRealData);

    // 详情 → 阅读：章节 id 就是 photo id，统一走 /chapter 下载并按 scramble_id 还原
    connect(detail_, &DetailPage::StartReading, this, [this](const Comic &c, int idx) {
        const QString cid = detail_->ChapterIdAt(idx);
        if (cid.isEmpty()) {
            Toast::Show(this, QStringLiteral("章节信息加载中，请稍后重试"));
            return;
        }
        OpenChapterById(cid, QStringLiteral("%1 · 第 %2 话").arg(c.title).arg(idx + 1));
    });
    connect(detail_, &DetailPage::DownloadRequested, this,
            [this](const Comic &c, const QVector<int> &indexes) { EnqueueDownload(c, indexes); });
    connect(detail_, &DetailPage::FavoriteRequested, this, [this](const Comic &c, bool wanted) {
        EnsureApi();
        if (!api_->LoggedIn()) {
            Toast::Show(this, QStringLiteral("收藏需要先登录"));
            Go(PageId::Login);
            return;
        }
        pendingFavId_ = c.id;
        pendingFavWanted_ = wanted;
        api_->FavoriteAdd(c.id);
        Toast::Show(this, wanted ? QStringLiteral("正在加入收藏…") : QStringLiteral("正在取消收藏…"));
    });

    // 详情页的标签 / 作者名 → 搜索。对齐 Python BookInfoView：
    //   ClickTagsItem → QtOwner().OpenSearch2(text)        （关键词 = 标签名）
    //   ClickAutorItem → QtOwner().OpenSearch2Author(text) （关键词 = 作者名）
    // 两者最终都是 SwitchWidget(searchView2, text=关键词)：压栈进搜索页（面包屑里
    // 多一层、按返回回到详情），并在 SwitchCurrent 里立刻 SendSearch(1)——
    // 用户不需要再点搜索按钮。这里用 Go(..., true) 复现压栈，再调 Search() 触发搜索。
    connect(detail_, &DetailPage::SearchRequested, this, [this](const QString &keyword) {
        const QString kw = keyword.trimmed();
        if (kw.isEmpty())
            return;
        // 先灌关键词再换页：换页动画抓的是"下一页的当前画面"，顺序反过来会先闪一帧
        // 上一次的搜索状态。搜索页此时还没显示，内部淡入会直接改内容（不叠遮罩）。
        search_->Search(kw);
        Go(PageId::Search, true);
    });

    // 本地 → 阅读：直接把目录里的真实图片交给阅读器
    connect(qobject_cast<LocalPage *>(pages_[int(PageId::Local)]), &LocalPage::OpenLocal, this,
            [this](const QString &path) {
                const QDir dir(path);
                const QStringList names = dir.entryList(
                    {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"), QStringLiteral("*.png"),
                     QStringLiteral("*.webp")},
                    QDir::Files, QDir::Name);
                if (names.isEmpty()) {
                    Toast::Show(this, QStringLiteral("该目录没有可读图片"));
                    return;
                }
                QStringList files;
                files.reserve(names.size());
                for (const QString &n : names)
                    files << dir.absoluteFilePath(n);
                reader_->ShowImages(dir.dirName(), files);
                Go(PageId::Reader);
            });
}

void MainWindow::WirePage(PageBase *page, PageId id)
{
    connect(page, &PageBase::Toast, this, [this](const QString &text) {
        Toast::Show(this, text);
    });
    connect(page, &PageBase::Navigate, this, [this, id](PageId target) {
        if (target == PageId::Home) {
            Back();
            return;
        }
        Go(target, id != PageId::Detail);
    });
}

void MainWindow::Go(PageId id, bool pushHistory)
{
    const int idx = int(id);
    if (idx < 0 || idx >= pages_.size() || !pages_.at(idx))
        return;

    // 离开阅读页必须退出全屏：否则退到首页仍是没有标题栏的全屏窗口
    const bool leaveReaderFullScreen =
        current_ == PageId::Reader && id != PageId::Reader && readerFullScreen_;

    // 动效语义先定下来：抽屉收起会延后一次重入，语义必须跟着带过去
    const NavMotion motion = hasNextMotion_
                                 ? nextMotion_
                                 : (backNext_ ? NavMotion::Pop
                                              : (pushHistory ? NavMotion::Push
                                                             : NavMotion::Lateral));
    hasNextMotion_ = false;
    backNext_ = false;

    // Python mobile_ui：抽屉展开时先收起来再换页，否则收起动画和首帧抢重绘。
    // 收起后重入本函数，换页动画照常播（这就是"抽屉里点导航没动画"的修复点）。
    if (drawerOpen_) {
        CloseDrawer();
        QTimer::singleShot(Motion::Slow, this, [this, id, pushHistory, motion] {
            nextMotion_ = motion;
            hasNextMotion_ = true;
            Go(id, pushHistory);
        });
        return;
    }

    // 上一轮动画还没落地（用户连点导航）：先把状态收干净，否则 pendingIdx_ 被覆盖，
    // 返回栈和真正显示的页会错开。
    if (pendingIdx_ >= 0)
        LandPending();

    if (current_ != id && pushHistory) {
        if (history_.isEmpty() || history_.last() != current_)
            history_.push_back(current_);
    }
    if (auto *prev = qobject_cast<PageBase *>(stack_->currentWidget()))
        prev->OnLeave();

    // 同级跳转的滑动方向要在 current_ 改写之前定下来
    if (motion == NavMotion::Lateral)
        lateralForward_ = NavRank(id) >= NavRank(current_);

    current_ = id;
    navMotion_ = motion;
    UpdateNavState();
    SwitchTo(idx);

    if (leaveReaderFullScreen) {
        // 等换页动画落地再退出：showNormal() 会改窗口尺寸，会掐掉正在播的转场
        QTimer::singleShot(Motion::Page + 80, this, [this] {
            if (readerFullScreen_)
                ToggleReaderFullScreen();
        });
    }
}

void MainWindow::SwitchTo(int idx)
{
    QWidget *target = pages_.at(idx);
    QWidget *cur = stack_->currentWidget();

    if (animEnabled_ && transition_ && cur && cur != target && !stack_->size().isEmpty()) {
        QElapsedTimer timer;
        timer.start();
        const QPixmap prevShot = CapturePage(cur);
        const QPixmap nextShot = CapturePage(target);
        const qint64 captureMs = timer.elapsed();
        if (!prevShot.isNull() && !nextShot.isNull()) {
            PageTransition::Mode mode = PageTransition::Mode::Push;
            int duration = Motion::Page;
            if (navMotion_ == NavMotion::Pop) {
                mode = PageTransition::Mode::Pop;
            } else if (navMotion_ == NavMotion::Lateral) {
                // 同级切换也走位移：方向由导航顺序决定，时长与首页分区共用 240ms，
                // 这样"点导航"和"滑分区"看到的是同一条时间轴与同一条曲线。
                mode = lateralForward_ ? PageTransition::Mode::Push : PageTransition::Mode::Pop;
                duration = Motion::Slow;
            }
            transition_->SetDuration(duration);
            if (transition_->Begin(prevShot, nextShot, mode)) {
                pendingIdx_ = idx;
                leavingPage_ = cur;
                animSize_ = stack_->size();
                animStartedMs_ = QDateTime::currentMSecsSinceEpoch();
                qWarning("transition: begin %dx%d capture=%lldms overlay=qml mode=%s\n", animSize_.width(), animSize_.height(), (long long)captureMs,
                             navMotion_ == NavMotion::Pop ? "pop"
                             : (navMotion_ == NavMotion::Lateral ? "lateral" : "push"));
                return;
            }
        }
    }
    ApplyPage(idx);
}

void MainWindow::ApplyPage(int idx)
{
    PageBase *page = pages_.at(idx);
    if (!page)
        return;
    stack_->setCurrentWidget(page);
    // 页面首次进入可能要建表/排版/生成示例数据（阅读页实测 200ms+）。
    // 动画路径上这一步是在覆盖层底下做的，用户看不到卡顿。
    page->OnEnter();
    Touch::InstallScrollGestures(page);
}

void MainWindow::LandPending()
{
    if (pendingIdx_ < 0)
        return;
    const int idx = pendingIdx_;
    pendingIdx_ = -1;
    leavingPage_ = nullptr;
    ApplyPage(idx);
    if (animStartedMs_ > 0) {
        qWarning("transition: landed after %lldms\n", (long long)(QDateTime::currentMSecsSinceEpoch() - animStartedMs_));
        animStartedMs_ = 0;
    }

    // 新页要真的画出一帧再撤覆盖层：setCurrentWidget 之后 Qt 要等下一轮事件循环
    // 才绘制，这时若已经撤了覆盖层，那一两帧就是背景色（用户看到"闪一下"）。
    QTimer::singleShot(48, this, [this] {
        // 48ms 内又开始了下一轮（连点导航）：这次撤层要作废，否则会把新覆盖层一起收掉
        if (pendingIdx_ >= 0)
            return;
        if (transition_ && !transition_->IsRunning())
            transition_->End();
    });
}

QPixmap MainWindow::CapturePage(QWidget *page)
{
    if (!page || !stack_)
        return {};
    const QSize target = stack_->size();
    if (target.isEmpty())
        return {};

    // 隐藏页的布局可能从未激活（QStackedLayout 只在 show/resize 时 activate），
    // 直接 render 会把子控件画在默认位置 —— 动画里就是"一张错位/空白的图"。
    if (page->size() != target)
        page->resize(target);
    if (QLayout *lay = page->layout())
        lay->activate();
    page->ensurePolished();

    const qreal dpr = devicePixelRatioF();
    QPixmap shot(target * dpr);
    shot.setDevicePixelRatio(dpr);
    // 透明区域不能留在未初始化内存里（会渲染成纯黑），先铺窗口底色
    shot.fill(UiTheme::I().Tokens().bg);
    page->render(&shot);
    return shot;
}

void MainWindow::SetAnimationEnabled(bool on)
{
    animEnabled_ = on;
    // 一处开关同时管住"换页覆盖层"与"控件级微动效"：关掉后逻辑路径完全不变，
    // 只是所有时长退化为 0（见 motion.cpp 的 Animate/Fade/CrossFade 分支）。
    Motion::SetEnabled(on);
    if (transition_)
        transition_->SetAnimationEnabled(on);
    if (!on && pendingIdx_ >= 0) {
        LandPending();
        if (transition_)
            transition_->End();
    }
}

void MainWindow::FreezeTransition(qreal progress)
{
    if (transition_)
        transition_->FreezeAt(progress);
}

void MainWindow::DemoTransition(PageId from, PageId to, qreal progress, int durationMs,
                                bool backward, int forceMode)
{
    // 起点必须"无动画"地跳过去，否则会先播一次 from 的动画
    const bool saved = animEnabled_;
    SetAnimationEnabled(false);
    Go(from, false);
    if (QWidget *p = pages_.value(int(from))) {
        if (QLayout *lay = p->layout())
            lay->activate();
    }
    SetAnimationEnabled(true);
    if (durationMs > 0 && transition_)
        transition_->SetDuration(durationMs);
    // 前进/后退/同级由 forceMode 或 backward 决定；这里显式指定，避免被
    // "Go(..., false) = 同级跳转"的默认规则改掉演示方向。
    nextMotion_ = forceMode == 2 ? NavMotion::Lateral
                                 : ((forceMode == 1 || (forceMode < 0 && backward)) ? NavMotion::Pop
                                                                                    : NavMotion::Push);
    hasNextMotion_ = true;
    Go(to, !backward);
    if (progress >= 0.0 && transition_ && transition_->IsRunning())
        FreezeTransition(progress);
    (void)saved;
}

void MainWindow::EnableTouchMode(bool on)
{
    Touch::SetTouchMode(on);
    // Python 的 mobile_ui.IsEnabled() 就是 platform_mobile.IsAndroid()：
    // 手机上导航栏恒为抽屉。桌面用 --touch 模拟 Android，因此同步开关抽屉。
    SetDrawerMode(on);
    if (touch_)
        touch_->Install();          // 幂等：只有第一次真正装
    Touch::InstallScrollGestures(this);
    const int changed = Touch::PolishHitTargets(this);
    qWarning("touch: mode=%d hitTargets<=44dp fixed=%d safeArea=%s\n", int(Touch::IsTouchMode()), changed,
                 qPrintable(Touch::SafeArea().ToString()));
}

void MainWindow::ShowEdgeHint(qreal progress)
{
    if (touch_) {
        touch_->SetActive(true);
        touch_->ShowEdgeHint(progress);
    }
}

void MainWindow::SetSafeAreaOverride(int left, int top, int right, int bottom)
{
    Touch::Insets in;
    in.left = left;
    in.top = top;
    in.right = right;
    in.bottom = bottom;
    Touch::SetSafeAreaOverride(in);
    ApplySafeArea();
}

void MainWindow::ApplySafeArea()
{
    if (!rootLayout_)
        return;
    Touch::Insets in = Touch::SafeArea();
    // 底部恒为 0：手势导航条不再参与根布局留白（原来 Android 上会拿到 78dp，
    // 首页页码条/抽屉底栏下面会多出一条空白带）。左右/顶部仍按刘海安全区收。
    in.bottom = 0;
    rootLayout_->setContentsMargins(in.left, in.top, in.right, in.bottom);
    qWarning("touch: safeArea applied = %s\n", qPrintable(in.ToString()));
}

void MainWindow::DemoReaderTap(qreal fx, qreal fy)
{
    // 等阅读页完成首次布局（OnEnter 里会生成示例页并排版），否则 viewport 还是 0x0
    QTimer::singleShot(350, this, [this, fx, fy] {
        if (reader_)
            reader_->TapDemo(fx, fy);
    });
}

bool MainWindow::SimulateBackSwipe()
{
    // 回归入口：不注入鼠标事件（不同平台合成事件的坐标语义不一致），
    // 直接把"一次完整手势"的语义跑一遍 —— 等价于拖到阈值后松手。
    const bool canGoBack = current_ != PageId::Home;
    if (touch_)
        touch_->ShowEdgeHint(1.0);
    if (canGoBack)
        Back();
    return canGoBack;
}

void MainWindow::Back()
{
    if (history_.isEmpty()) {
        Go(PageId::Home, false);
        return;
    }
    const PageId target = history_.takeLast();
    // 标记"这是返回"：Go 里据此选 Pop（从左侧推回），而不是同级淡入
    backNext_ = true;
    Go(target, false);
}

void MainWindow::UpdateNavState()
{
    // 高亮"当前所在分区"：详情/阅读是从某个分区点进去的，仍然高亮那个入口，
    // 否则一路点进阅读页后侧栏会全空（Python 的 buttonGroup 也是保持入口选中）。
    PageId active = current_;
    if (!SpecFor(active)) {
        for (int i = history_.size() - 1; i >= 0; --i) {
            if (SpecFor(history_.at(i))) {
                active = history_.at(i);
                break;
            }
        }
    }
    QToolButton *checked = nullptr;
    for (const auto &item : navItems_) {
        const bool on = item.first == active;
        item.second->setChecked(on);
        if (on)
            checked = item.second;
    }
    UpdateIcons();
    RebuildCrumbs();
    // 选中项在可视区外时（如"批量超分"），把侧栏滚过去。Python 没做这一步，
    // 表现为点了菜单看不到高亮；这里补上，不影响布局。
    // 抽屉模式下不自动滚动：Python 的抽屉没有这个行为，展开时应从"用户"分组开始。
    if (checked && navScroll_ && !drawerMode_) {
        QScrollArea *area = navScroll_;
        QTimer::singleShot(0, this, [area, checked] {
            area->ensureWidgetVisible(checked, 0, 12);
        });
    }
}

void MainWindow::UpdateIcons()
{
    const int px = UiStyle::Fpx(32);
    for (const auto &item : navItems_) {
        const NavSpec *spec = SpecFor(item.first);
        if (!spec)
            continue;
        const bool on = item.second->isChecked();
        item.second->setIcon(Glyph::Icon(spec->icon, px, on ? T().accent : T().textDim));
    }
    for (QWidget *line : navSeparators_)
        line->setStyleSheet(QStringLiteral("background: %1;").arg(T().border.name()));
    if (crumbMenu_) {
        crumbMenu_->setIcon(Glyph::Icon(Glyph::Menu, UiStyle::Fpx(15), T().textDim));
    }
    // 侧栏滚动条走控件级样式表：应用级 QSS 里 `QScrollArea#NavScroll QScrollBar`
    // 这条后代选择器没能压过基座（res/theme）的通用规则，手柄一直是 @border，
    // 在 #31363b 底上等于隐形。控件级样式表优先级更高且不受基座影响。
    if (navScroll_) {
        navScroll_->verticalScrollBar()->setStyleSheet(
            QStringLiteral("QScrollBar:vertical{background:transparent;width:8px;margin:0px;}"
                           "QScrollBar::handle:vertical{background:%1;min-height:32px;}"
                           "QScrollBar::handle:vertical:hover{background:%2;}"
                           "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0px;}"
                           "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical"
                           "{background:transparent;}")
                .arg(T().textFaint.name(), T().textDim.name()));
    }
}

Qt::Edges MainWindow::EdgeAt(const QPoint &pos) const
{
    const int m = 6;
    Qt::Edges edges;
    if (pos.x() <= m)
        edges |= Qt::LeftEdge;
    if (pos.x() >= width() - m)
        edges |= Qt::RightEdge;
    if (pos.y() <= m)
        edges |= Qt::TopEdge;
    if (pos.y() >= height() - m)
        edges |= Qt::BottomEdge;
    return edges;
}

void MainWindow::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        const Qt::Edges edges = EdgeAt(e->position().toPoint());
        if (edges && windowHandle()) {
            windowHandle()->startSystemResize(edges);
            return;
        }
    }
    QWidget::mousePressEvent(e);
}

void MainWindow::mouseMoveEvent(QMouseEvent *e)
{
    const Qt::Edges edges = EdgeAt(e->position().toPoint());
    if (edges == (Qt::LeftEdge | Qt::TopEdge) || edges == (Qt::RightEdge | Qt::BottomEdge))
        setCursor(Qt::SizeFDiagCursor);
    else if (edges == (Qt::RightEdge | Qt::TopEdge) || edges == (Qt::LeftEdge | Qt::BottomEdge))
        setCursor(Qt::SizeBDiagCursor);
    else if (edges & (Qt::LeftEdge | Qt::RightEdge))
        setCursor(Qt::SizeHorCursor);
    else if (edges & (Qt::TopEdge | Qt::BottomEdge))
        setCursor(Qt::SizeVerCursor);
    else
        unsetCursor();
    QWidget::mouseMoveEvent(e);
}

void MainWindow::UpdateMobileBreakpoint()
{
    if (!Touch::IsTouchMode())
        return;
    // 600dp 是 Android 官方的"平板/折叠屏展开"断点：窄屏用抽屉，宽屏恢复常驻侧栏。
    const bool compact = width() < UiStyle::Fpx(600);
    if (compact == drawerMode_)
        return;
    // SetDrawerMode(false) 内部会把最小尺寸提到 980×660，窗口会先被夹到 980；
    // 记住切换前的尺寸，设完新的下限再还原，否则平板/折叠屏会被"弹"到 980 宽。
    const QSize keep = size();
    SetDrawerMode(compact);
    // SetDrawerMode(false) 会把最小尺寸恢复成桌面值（980×660），手机上必须改回来。
    setMinimumSize(compact ? QSize(UiStyle::Fpx(320), UiStyle::Fpx(320))
                           : QSize(UiStyle::Fpx(600), UiStyle::Fpx(320)));
    if (size() != keep)
        resize(keep);
    // 桌面 --touch 也能验收断点：日志里直接看 compact/rail，不用靠截图猜。
    qWarning("layout: w=%d compact=%d rail=%d\n", width(), int(compact),
                 int(!drawerMode_));
}

void MainWindow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    UpdateMobileBreakpoint();
    if (drawerMode_)
        UpdateDrawerGeometry();

    // 动画期间尺寸变了（软键盘弹出/收起、旋转、分屏）：覆盖层还带着旧几何，
    // 继续播会错位，直接落位到目标页。但必须区分"真尺寸变化"和"布局抖一下"，
    // 否则切页时面包屑重排带来的 1~2px 抖动会把动画在第一帧就掐掉。
    if (pendingIdx_ >= 0 && transition_ && transition_->IsRunning() && stack_) {
        const QSize now = stack_->size();
        if (!animSize_.isEmpty()
            && (qAbs(now.width() - animSize_.width()) > 8
                || qAbs(now.height() - animSize_.height()) > 8)) {
            LandPending();
            transition_->End();
        }
    }
    // 窄窗口时收窄导航栏（对应 Python 版 mobile 布局下的图标模式）
    UpdateNavMetrics();
}

// 导航栏尺寸跟着字号走：图标 20 + 两字标签 11px，9pt 时 70x54 刚好，
// 字号一变就会换行成"首/页"两行，所以这里按同一套缩放算。
void MainWindow::UpdateNavUser()
{
    const UserState u = UserSession::User();
    if (navName_)
        navName_->setText(u.loggedIn ? u.name : QStringLiteral("未登录"));
    if (navTitle_)
        navTitle_->setText(u.loggedIn ? u.title : QStringLiteral("登录后同步收藏与记录"));
    // 未登录时 Python 这几个值是空串；空着会被误读成"加载失败"，用 — 更清楚
    const QString dash = QStringLiteral("—");
    if (navCoins_)
        navCoins_->setText(u.loggedIn ? QString::number(u.coins) : dash);
    if (navLevel_) {
        navLevel_->setText(u.loggedIn ? QStringLiteral("Lv.%1").arg(u.level) : dash);
        // 经验值与收藏上限不占行宽，放 tooltip（与 Python 的 Lv(exp/next) 同源）
        navLevel_->setToolTip(u.loggedIn && u.nextExp > 0
                                  ? QStringLiteral("经验 %1/%2").arg(u.exp).arg(u.nextExp)
                                  : QString());
    }
    if (navFav_) {
        navFav_->setText(u.loggedIn ? QString::number(u.favorites) : dash);
        navFav_->setToolTip(u.loggedIn && u.favoritesMax > 0
                                ? QStringLiteral("收藏上限 %1").arg(u.favoritesMax)
                                : QString());
    }
    if (navProxy_)
        navProxy_->setText(u.proxy);
    if (navProxyImg_)
        navProxyImg_->setText(u.proxyImg);
    if (navHide_)
        navHide_->setText(QStringLiteral("已选 %1 个").arg(u.hideWordList.size()));
    if (navAuthBtn_) {
        // 登录后按钮还写"登录"会让人以为没登上；点它仍然是进账号页（行为不变）
        navAuthBtn_->setText(u.loggedIn ? QStringLiteral("账号") : QStringLiteral("登录"));
        navAuthBtn_->setToolTip(u.loggedIn ? QStringLiteral("查看账号 / 退出登录")
                                           : QStringLiteral("登录"));
    }
    UpdateSignButton();
}

namespace {

// JmApi 的列表项 → UI 的 Comic。cover 是相对路径（media/albums/...），要拼图片 CDN 域名。
Comic RealComic(const JmComicItem &it, const QString &imgDomain)
{
    Comic c;
    c.id = it.id;
    c.title = it.name;
    c.author = it.author;
    if (!it.category.isEmpty())
        c.tags = QStringList{it.category};
    c.status = QStringLiteral("连载中");
    c.updated = QStringLiteral("刚刚");
    c.desc = QStringLiteral("来自禁漫接口（分类：%1）").arg(
        it.category.isEmpty() ? QStringLiteral("未标注") : it.category);
    // 列表接口（/latest、/search、/promote、/favorite）都不返回章节总数：
    // 这里只填"已经通过 /album 确认过"的值，取不到就保持 0（未知）。
    // 卡片渲染据此决定是否显示"· N 话"（见 ComicMetaText），不会画成"0 话"。
    c.chapters = ComicFacts::Chapters(it.id);
    QString cover = it.cover;
    // 列表接口的 image 字段为空（实测 /latest 全是 ""），Python 版是按 id 拼固定命名：
    //   /media/albums/<id>_3x4.jpg
    if (cover.isEmpty() && !it.id.isEmpty())
        cover = QStringLiteral("media/albums/%1_3x4.jpg").arg(it.id);
    if (!cover.startsWith(QLatin1String("http"))) {
        while (cover.startsWith(QLatin1Char('/')))
            cover.remove(0, 1);
        cover = QStringLiteral("https://%1/%2").arg(imgDomain, cover);
    }
    c.coverUrl = cover;
    return c;
}

}  // namespace

void MainWindow::LoadRealData()
{
    EnsureApi();
    // 首屏/重试都先进入加载态：在数据到达前不渲染任何条目。
    if (home_ && !home_->HasSections())
        home_->SetLoading();

    struct FetchState {
        bool latestDone = false;
        bool promoteDone = false;
        QVector<Comic> latest;
        QVector<JmHomeBlock> blocks;
        QString error;
    };
    auto st = QSharedPointer<FetchState>::create();
    auto finish = [this, st] {
        if (!st->latestDone || !st->promoteDone)
            return;
        QVector<HomeSection> secs;
        if (!st->latest.isEmpty()) {
            HomeSection first;
            first.title = QStringLiteral("最新上传");
            first.kind = HomeSectionKind::Latest;
            first.comics = st->latest;
            secs.append(first);
        }
        for (const JmHomeBlock &b : st->blocks) {
            if (b.type == QLatin1String("novels") || b.type == QLatin1String("library"))
                continue;   // 与 Python filterTypes 一致
            HomeSection s;
            s.title = b.title;
            s.kind = HomeSectionKind::Promo;
            for (const JmComicItem &it : b.items)
                s.comics.append(RealComic(it, api_->ImageDomain()));
            if (!s.comics.isEmpty())
                secs.append(s);
        }
        if (home_ && !secs.isEmpty()) {
            home_->SetSections(secs);
            qWarning("real-data: sections=%d latest=%d\n", int(secs.size()),
                     int(st->latest.size()));
        } else {
            // 接口没给内容就如实报错：首页不保留任何本地样例条目。
            if (home_)
                home_->SetFailed(st->error.isEmpty()
                                     ? QStringLiteral("接口没有返回首页内容，请稍后重试。")
                                     : st->error);
            qWarning("real-data: empty (%s)\n", qPrintable(st->error));
        }
    };

    // 重试会再次进入本函数：先断开上一轮连接，否则回调会叠加。
    if (latestConn_)
        disconnect(latestConn_);
    if (promoteConn_)
        disconnect(promoteConn_);

    latestConn_ = connect(api_, &JmApi::LatestReady, this,
                          [this, st, finish](bool ok, const QVector<JmComicItem> &items, int page,
                                             const QString &err) {
                              if (page > 0) {
                                  // "加载更多"的追加请求：不影响首次加载的完成判定
                                  QVector<Comic> list;
                                  if (ok)
                                      for (const JmComicItem &it : items)
                                          list.append(RealComic(it, api_->ImageDomain()));
                                  if (home_)
                                      home_->AppendLatest(list, page);
                                  qWarning("latest-more: page=%d items=%d\n", page,
                                           int(list.size()));
                                  return;
                              }
                              if (page != 0)
                                  return;
                              if (!ok) {
                                  st->error = err;
                              } else {
                                  for (const JmComicItem &it : items)
                                      st->latest.append(RealComic(it, api_->ImageDomain()));
                              }
                              st->latestDone = true;
                              finish();
                          });
    promoteConn_ = connect(api_, &JmApi::PromoteReady, this,
                           [st, finish](bool ok, const QVector<JmHomeBlock> &blocks,
                                        const QString &err) {
                               if (ok)
                                   st->blocks = blocks;
                               else if (st->error.isEmpty())
                                   st->error = err;
                               st->promoteDone = true;
                               finish();
                           });

    // 单通道串行：latest 先到、promote 后到
    api_->FetchLatestPage(0);
    api_->FetchPromote(0);
}

void MainWindow::EnsureApi()
{
    if (api_)
        return;
    net_ = new NetClient(this);
    api_ = new JmApi(net_, this);
    connect(api_, &JmApi::Log, this, [](const QString &l) {
        qWarning("api: %s\n", qPrintable(l));
    });

    // 启动时已由 SessionStore 读进 UserSession；这里把 avs 交给 JmApi，
    // 之后所有请求自动带 Cookie（收藏/书架等需要登录态）。
    const UserState u = UserSession::User();
    if (!u.avs.isEmpty())
        api_->RestoreSession(u.avs.toLatin1(), u.name);
    api_->SetUid(u.uid);

    // 签到（打卡）结果：成功 → 按钮置"已打卡"并落库；失败 → 提示服务端文案
    connect(api_, &JmApi::DailyReady, this,
            [this](bool ok, bool signedToday, int dailyId, const QString &msg,
                   const QString &err) {
                if (!ok) {
                    qWarning("sign: daily fail err=%s", qPrintable(err));
                    return;
                }
                UserSession::Ref().signedToday = signedToday;
                UpdateNavUser();
                qWarning("sign: daily ok signed=%d id=%d msg=%s", int(signedToday), dailyId,
                         qPrintable(msg));
            });
    connect(api_, &JmApi::SignReady, this,
            [this](bool ok, const QString &msg, const QString &err) {
                if (signBtn_)
                    signBtn_->setEnabled(true);
                if (ok) {
                    UserSession::Ref().signedToday = true;
                    SessionStore::Save();
                    UpdateNavUser();
                    Toast::Show(this, msg.isEmpty() ? QStringLiteral("打卡成功") : msg);
                    qWarning("sign: ok msg=%s", qPrintable(msg));
                    return;
                }
                Toast::Show(this, !msg.isEmpty() ? msg
                                                 : QStringLiteral("打卡失败：%1").arg(err));
                qWarning("sign: fail msg=%s err=%s", qPrintable(msg), qPrintable(err));
                // 失败常见原因是"今天已经签过"：刷新一次真实状态，按钮不会一直可点
                if (!err.isEmpty())
                    api_->FetchDailySign();
            });

    connect(api_, &JmApi::AlbumReady, this, [this](bool ok, const JmAlbum &a, const QString &err) {
        // 先判"是不是当前作品"，再判成败：失败的响应现在也带 album id，
        // 否则切作品后旧请求的超时会弹出与当前作品无关的错误。
        if (!detailComicId_.isEmpty() && !a.id.isEmpty() && a.id != detailComicId_)
            return;   // 用户已经切到别的作品
        if (!ok) {
            qWarning("album-fail: %s\n", qPrintable(err));
            Toast::Show(this, QStringLiteral("详情加载失败：%1").arg(err));
            return;
        }
        detail_->SetAlbum(a);
        // /album 的 series 是章节数的唯一权威来源。记进 ComicFacts 并立刻刷新
        // 首页/搜索/收藏里同 id 的卡片（含当前不在前台的页面），
        // 用户从详情返回时看到的就是真实话数，而不是列表接口留下的"未知"。
        ApplyChapterCount(a.id.isEmpty() ? detailComicId_ : a.id, int(a.chapters.size()));
    });

    connect(api_, &JmApi::SearchReady, this,
            [this](bool ok, const QVector<JmComicItem> &items, int total, int page,
                   const QString &err) {
                if (!ok) {
                    if (page <= 1) {
                        // 接口失败如实显示错误 + 重试入口，绝不用本地结果顶替。
                        search_->SetFailed(err);
                    } else {
                        Toast::Show(this, QStringLiteral("没有更多结果"));
                        search_->SetResults({}, search_->Total(), page - 1, true);
                    }
                    qWarning("search-fail: %s\n", qPrintable(err));
                    return;
                }
                QVector<Comic> list;
                list.reserve(items.size());
                for (const JmComicItem &it : items)
                    list.append(RealComic(it, api_->ImageDomain()));
                search_->SetResults(list, total, page, page > 1);
            });

    connect(api_, &JmApi::LoginReady, this,
            [this](bool ok, const JmUserInfo &user, const QString &err) {
                auto *lp = qobject_cast<LoginPage *>(pages_[int(PageId::Login)]);
                if (lp)
                    lp->SetBusy(false);
                if (!ok) {
                    if (lp)
                        lp->SetStatus(QStringLiteral("登录失败：%1").arg(err), false);
                    Toast::Show(this, QStringLiteral("登录失败：%1").arg(err));
                    qWarning("login-fail: %s\n", qPrintable(err));
                    return;
                }
                // 账号资料只认 /login 的响应：里面本来就有 coin / level /
                // album_favorites，之前只取 username 就把其余字段丢了。
                ApplyUserProfile(user, true);
                if (lp)
                    lp->SetLoggedIn(true, user.name);
                Toast::Show(this, QStringLiteral("登录成功"));
                // 登录后立刻取签到信息：打卡按钮要能反映"今天是否已签"
                api_->FetchDailySign();
                if (current_ == PageId::Login)
                    Back();
                qWarning("login-ok: user=%s uid=%lld avs=%s level=%d coin=%d fav=%d",
                         qPrintable(UserSession::User().name),
                         static_cast<long long>(UserSession::User().uid),
                         api_->Avs().isEmpty() ? "no" : "yes", UserSession::User().level,
                         UserSession::User().coins, UserSession::User().favorites);
            });

    ApplyProxySettings();

    connect(api_, &JmApi::FavoriteAddReady, this, [this](bool ok, const QString &err) {
        if (!ok) {
            Toast::Show(this, QStringLiteral("收藏失败：%1").arg(err));
            return;
        }
        detail_->SetFavorite(pendingFavWanted_);
        Toast::Show(this, pendingFavWanted_ ? QStringLiteral("已加入收藏")
                                            : QStringLiteral("已取消收藏"));
        // 收藏数是账号数据：本地先按意图 ±1，随后用 /favorite 的 total 校准
        UserState &u = UserSession::Ref();
        u.favorites = qMax(0, u.favorites + (pendingFavWanted_ ? 1 : -1));
        UpdateNavUser();
        if (UserSession::User().loggedIn)
            api_->FavoriteFolder(1);
    });

    connect(api_, &JmApi::FavoriteReady, this,
            [this](bool ok, const QVector<JmComicItem> &items, int total, const QString &err) {
                auto *fp = qobject_cast<FavoritePage *>(pages_[int(PageId::Favorite)]);
                if (!ok) {
                    qWarning("favorite-list: fail %s (loggedIn=%d)", qPrintable(err),
                             int(UserSession::User().loggedIn));
                    if (!UserSession::User().loggedIn) {
                        // 会话已失效（401 分支先发 SessionExpired）或无会话：
                        // 收藏页要显示"去登录"，不是"网络错误"。
                        if (fp)
                            fp->SetState(FavoritePage::State::NotLoggedIn);
                        return;
                    }
                    if (fp)
                        fp->SetState(FavoritePage::State::Error,
                                     QStringLiteral("加载失败：%1").arg(err));
                    return;
                }
                QVector<Comic> list;
                list.reserve(items.size());
                for (const JmComicItem &it : items)
                    list.append(RealComic(it, api_->ImageDomain()));
                // total 才是服务端权威的收藏数（album_favorites 可能是登录时的快照）
                UserSession::Ref().favorites = total;
                SessionStore::Save();
                UpdateNavUser();
                if (fp)
                    fp->SetItems(list, total);
                qWarning("favorite-list: items=%d total=%d\n", int(list.size()), total);
            });

    // 会话失效（/favorite 返回 code 401 或 HTTP 401）：必须把"已登录 + 上一个
    // 账号的数据"一起收掉，否则用户会继续看到别人的收藏数/称号。
    connect(api_, &JmApi::SessionExpired, this, [this](const QString &msg) {
        if (!UserSession::User().loggedIn && UserSession::User().avs.isEmpty())
            return;   // 本来就没登录
        SessionStore::Clear();   // 清 avs/uid/账号字段（保留屏蔽词等本地偏好）
        if (api_) {
            api_->RestoreSession(QByteArray(), QString());
            api_->SetUid(0);
        }
        if (auto *lp = qobject_cast<LoginPage *>(pages_[int(PageId::Login)]))
            lp->SetLoggedIn(false);
        if (auto *fp = qobject_cast<FavoritePage *>(pages_[int(PageId::Favorite)]))
            fp->SetState(FavoritePage::State::NotLoggedIn);
        UpdateNavUser();
        Toast::Show(this, msg.isEmpty() ? QStringLiteral("登录已失效，请重新登录") : msg);
        qWarning("session: expired (%s)", qPrintable(msg));
    });
}

void MainWindow::EnqueueDownload(const Comic &c, const QVector<int> &chapterIndexes)
{
    EnsureApi();
    if (!downloads_) {
        Toast::Show(this, QStringLiteral("下载队列未初始化"));
        return;
    }
    if (!detail_ || chapterIndexes.isEmpty()) {
        Toast::Show(this, QStringLiteral("章节还没准备好"));
        return;
    }
    QStringList ids;
    QStringList names;
    for (int idx : chapterIndexes) {
        const QString cid = detail_->ChapterIdAt(idx);
        if (cid.isEmpty())
            continue;
        ids << cid;
        names << detail_->ChapterNameAt(idx);
    }
    if (ids.isEmpty()) {
        Toast::Show(this, QStringLiteral("没有可下载的章节"));
        return;
    }
    downloads_->EnqueueChapters(c.id, c.title, ids, names, c.coverUrl);
    Toast::Show(this, QStringLiteral("已加入 %1 个下载任务").arg(ids.size()));
    // 入队后直接落到下载页：进度/暂停/重试都在那一页，否则用户点完看不到任何反馈
    if (current_ != PageId::Download)
        Go(PageId::Download);
}

void MainWindow::SetImageHostsForTest(const QStringList &hosts)
{
    EnsureApi();
    if (!api_ || hosts.isEmpty())
        return;
    // 验收钩子：某些网络下只有个别图床可达，强制把该 host 排到第一位，
    // 避免"下载逻辑没问题、全卡在 CDN 不可达"把结论带偏。
    QStringList clean;
    for (const QString &h : hosts) {
        const QString t = h.trimmed();
        if (t.isEmpty())
            continue;
        const QString host = QUrl(t).host();
        clean << (host.isEmpty() ? t : host);
    }
    if (!clean.isEmpty()) {
        api_->SetImageDomains(clean);
        qWarning("img-host: %s", qPrintable(clean.join(QLatin1Char(','))));
    }
}

QString MainWindow::DownloadReport() const
{
    if (auto *dp = qobject_cast<DownloadPage *>(pages_.value(int(PageId::Download))))
        return dp->Report();
    return QStringLiteral("dl-page: unavailable");
}

bool MainWindow::DownloadSelfTest(const QString &albumId, int chapters, int timeoutMs)
{
    if (albumId.isEmpty())
        return false;
    EnsureApi();
    if (!downloads_) {
        qWarning("dl-selftest: no download manager");
        return false;
    }
    // 走与 UI 完全相同的路径：/album → 详情页章节表 → EnqueueChapters
    OpenAlbumById(albumId);
    QElapsedTimer timer;
    timer.start();
    QEventLoop loop;
    QTimer poll;
    bool enqueued = false;
    poll.setInterval(200);
    QObject::connect(&poll, &QTimer::timeout, &loop, [this, &loop, &poll, &enqueued, albumId,
                                                       chapters, &timer, timeoutMs] {
        auto *profile = detail_;
        if (!profile)
            return;
        const int have = profile->ChapterCount();
        if (have == 0 && timer.elapsed() < timeoutMs)
            return;   // 等 /album
        if (have > 0) {
            const int n = chapters <= 0 ? have : qMin(chapters, have);
            QVector<int> idx;
            idx.reserve(n);
            for (int i = 0; i < n; ++i)
                idx << i;
            Comic c;
            c.id = albumId;
            c.title = QStringLiteral("dl-selftest");
            EnqueueDownload(c, idx);
            enqueued = !downloads_->Tasks().isEmpty();
            qWarning("dl-selftest: enqueued %d chapter(s) of album %s", n, qPrintable(albumId));
            poll.stop();
            loop.quit();
            return;
        }
        loop.quit();
    });
    poll.start();
    loop.exec();
    if (!enqueued) {
        // /album 没回来 / 章节表为空：这是失败，不能报 PASS（否则"下载入口
        // 完全没通"也会被当成通过）。
        qWarning("dl-selftest: no chapter enqueued (album fetch failed or empty)");
        return false;
    }
    const bool idle = downloads_->WaitIdle(timeoutMs);
    const QString report = downloads_->Report();
    qWarning("dl-selftest: idle=%d %s", int(idle), qPrintable(report));
    bool ok = idle;
    for (const DownloadTask &t : downloads_->Tasks()) {
        if (t.state == DownloadTask::Failed || t.state == DownloadTask::Interrupted)
            ok = false;
        qWarning("dl-selftest: task=%s %s %d/%d bytes=%lld dir=%s err=%s",
                 qPrintable(t.chapterName), qPrintable(t.StateName()), t.done, t.total,
                 static_cast<long long>(t.bytes), qPrintable(t.dir), qPrintable(t.error));
    }
    return ok;
}

void MainWindow::ApplyUserProfile(const JmUserInfo &user, bool refreshFavorites)
{
    UserState &u = UserSession::Ref();
    u.loggedIn = true;
    if (!user.name.isEmpty())
        u.name = user.name;
    u.avs = QString::fromLatin1(api_ ? api_->Avs() : QByteArray());
    u.uid = api_ && api_->Uid() != 0 ? api_->Uid() : user.uid;
    u.signedToday = false;
    if (user.hasProfile) {
        u.title = user.levelName.isEmpty() ? QStringLiteral("已登录（在线）") : user.levelName;
        u.coins = user.coins;
        u.level = user.level;
        u.favorites = user.favorites;
        u.favoritesMax = user.favoritesMax;
        u.exp = user.exp;
        u.nextExp = user.nextExp;
    } else if (u.title.isEmpty()) {
        // 服务端没给称号：至少让用户看到"登录成功了"，但数值保留缓存
        u.title = QStringLiteral("已登录（在线）");
    }
    SessionStore::Save();
    UpdateNavUser();
    if (refreshFavorites && api_ && api_->LoggedIn()) {
        // 登录响应里的收藏数可能是快照，拉一次列表校准，同时把收藏页预热
        api_->FavoriteFolder(1);
    }
    qWarning("user-profile: name=%s level=%d coin=%d fav=%d/%d exp=%d/%d avatar=%s",
             qPrintable(u.name), u.level, u.coins, u.favorites, u.favoritesMax, u.exp,
             u.nextExp, user.hasProfile ? "server" : "cached");
}

void MainWindow::ProbeSession()
{
    if (sessionProbed_)
        return;
    sessionProbed_ = true;
    const UserState u = UserSession::User();
    // 没有 AVS 就没有可校验的会话（未登录），别发注定 401 的请求
    if (!u.loggedIn || u.avs.isEmpty() || !api_ || !api_->LoggedIn())
        return;
    qWarning("session: probe start uid=%lld", static_cast<long long>(u.uid));
    api_->FavoriteFolder(1);
}

void MainWindow::OpenDetail(const Comic &c)
{
    detail_->ShowComic(c);
    Go(PageId::Detail);
    if (c.id.isEmpty())
        return;
    detailComicId_ = c.id;
    EnsureApi();
    api_->FetchAlbum(c.id);
    qWarning("album-req: %s\n", qPrintable(c.id));
}

void MainWindow::ApplyChapterCount(const QString &id, int chapters)
{
    if (id.isEmpty() || chapters <= 0)
        return;
    // 记住权威值：之后重新拉列表（RealComic）或重建首页分区都能直接带上话数。
    ComicFacts::RememberChapters(id, chapters);
    if (home_)
        home_->SetChapters(id, chapters);
    if (search_)
        search_->SetChapters(id, chapters);
    if (auto *fp = qobject_cast<FavoritePage *>(pages_.value(int(PageId::Favorite))))
        fp->SetChapters(id, chapters);
}

void MainWindow::OpenAlbumById(const QString &albumId)
{
    Comic c;
    c.id = albumId;
    c.title = QStringLiteral("加载中…");
    // 作者留空：等 /album 回填。以前填 "·" 只是为了让 meta 行不掉字段，
    // 现在作者名是"点它搜索作者"的链接，占位符会变成可点的假关键词。
    c.status = QStringLiteral("连载中");
    c.updated = QStringLiteral("刚刚");
    c.desc = QStringLiteral("正在读取作品信息…");
    OpenDetail(c);
}

void MainWindow::OpenChapterById(const QString &chapterId, const QString &label)
{
    // 继续阅读：同一章重开时跳回上次的页码（与 Python AddHistory/OpenPage(pageIndex) 一致）
    const QString last = UiPrefs::GetString(QStringLiteral("readerLastChapter"));
    resumePage_ = (last == chapterId) ? qMax(1, UiPrefs::GetInt(QStringLiteral("readerLastPage"), 1))
                                      : 1;
    resumePending_ = resumePage_ > 1;
    UiPrefs::Set(QStringLiteral("readerLastChapter"), chapterId);
    if (!resumePending_)
        UiPrefs::Set(QStringLiteral("readerLastPage"), 1);
    currentChapterId_ = chapterId;
    currentChapterLabel_ = label;
    // 章末/章首能否继续翻：按当前章节在详情页章节表里的位置判断
    bool hasPrev = false;
    bool hasNext = false;
    if (detail_) {
        for (int i = 0; i < detail_->ChapterCount(); ++i) {
            if (detail_->ChapterIdAt(i) == chapterId) {
                hasPrev = i > 0;
                hasNext = i + 1 < detail_->ChapterCount();
                break;
            }
        }
    }
    if (reader_)
        reader_->SetChapterNavigation(hasPrev, hasNext);
    EnsureApi();
    if (!pageLoader_) {
        pageLoader_ = new PageLoader(api_, this);
        connect(pageLoader_, &PageLoader::Progress, this, [this](int done, int total) {
            if (!reader_)
                return;
            if (total <= 0) {
                reader_->SetStatus(QStringLiteral("获取章节…"));
                return;
            }
            // 总页数一到就铺占位页：之后每页就绪单独交付，当前页先出图。
            if (!reader_->IsProgressive()) {
                reader_->BeginProgressive(total);
                // 继续阅读：页数一到就跳到上次的位置，那张图到了直接显示，
                // 不用等整章下完再跳（否则用户要先看第 1 页再"闪"到第 N 页）。
                if (resumePending_) {
                    reader_->ResumePage(resumePage_ - 1);
                    resumePending_ = false;
                }
            }
            reader_->SetLoadProgress(done, total);
        });
        // 单页到货：立刻交给阅读器（当前页那张会马上解码显示）
        connect(pageLoader_, &PageLoader::PageReady, this,
                [this](int index, int total, const QString &path) {
                    Q_UNUSED(path);
                    if (!reader_)
                        return;
                    if (!reader_->IsProgressive())
                        reader_->BeginProgressive(qMax(index + 1, 1));
                    reader_->SetPageReady(index, path);
                    // 逐页日志：验收"第 1 页先出图、其余页后续补齐"看的就是这条时间线
                    qWarning("progressive: page %d/%d ready", index + 1, total);
                });
        connect(pageLoader_, &PageLoader::Finished, this,
                [this](bool ok, const QString &title, const QStringList &files,
                       const QString &err) {
                    if (!ok) {
                        // 一页都没落地才是真失败；已经显示出来的页不要清掉
                        if (reader_ && reader_->IsProgressive())
                            reader_->FinishProgressive(files);
                        reader_->SetStatus(QStringLiteral("加载失败"));
                        Toast::Show(this, QStringLiteral("章节加载失败：%1").arg(err));
                        return;
                    }
                    if (reader_ && reader_->IsProgressive()) {
                        // 渐进路径：收尾只把缺页占位摘掉，不重置当前页码
                        reader_->FinishProgressive(files);
                    } else {
                        reader_->ShowImages(title, files);
                    }
                    if (resumePending_ && reader_) {
                        reader_->ResumePage(resumePage_ - 1);
                        Toast::Show(this, QStringLiteral("继续阅读：第 %1 页").arg(resumePage_));
                        // 恢复动作本身会再发一次 PageChanged，等本轮事件跑完再解除保护
                        QTimer::singleShot(0, this, [this] { resumePending_ = false; });
                    }
                    if (!err.isEmpty())
                        Toast::Show(this, err);
                    qWarning("chapter-shown: pages=%d resume=%d\n", int(files.size()), resumePage_);
                });
        connect(pageLoader_, &PageLoader::Log, this, [](const QString &l) {
            qWarning("pages: %s\n", qPrintable(l));
        });
    }
    reader_->OpenRemote(label);
    Go(PageId::Reader);
    pageLoader_->Load(chapterId, label);
    qWarning("chapter-req: %s\n", qPrintable(chapterId));
}

void MainWindow::OpenAdjacentChapter(int delta)
{
    if (!detail_ || currentChapterId_.isEmpty()) {
        Toast::Show(this, QStringLiteral("没有可切换的章节"));
        return;
    }
    int cur = -1;
    for (int i = 0; i < detail_->ChapterCount(); ++i) {
        if (detail_->ChapterIdAt(i) == currentChapterId_) {
            cur = i;
            break;
        }
    }
    if (cur < 0) {
        Toast::Show(this, QStringLiteral("当前章节不在作品章节表里"));
        return;
    }
    const int target = cur + delta;
    if (target < 0) {
        Toast::Show(this, QStringLiteral("已经是第一章"));
        return;
    }
    if (target >= detail_->ChapterCount()) {
        Toast::Show(this, QStringLiteral("已经是最后一章"));
        return;
    }
    const QString cid = detail_->ChapterIdAt(target);
    if (cid.isEmpty()) {
        Toast::Show(this, QStringLiteral("章节信息尚未加载"));
        return;
    }
    const QString name = detail_->ChapterNameAt(target);
    OpenChapterById(cid, name.isEmpty() ? QStringLiteral("第 %1 话").arg(target + 1) : name);
}

void MainWindow::ToggleReaderFullScreen()
{
    readerFullScreen_ = !readerFullScreen_;
#ifdef Q_OS_ANDROID
    // Android 上 showFullScreen() 只改窗口状态，状态栏/手势条仍由系统绘制在上层
    // （真机实测：调用后画面尺寸不变，顶部状态栏照样压着标题条）。
    // 必须同时把 Activity 的 decorView 设成沉浸式，系统栏才会真正让位。
    AndroidSetSystemBarsHidden(readerFullScreen_);
#endif
    if (readerFullScreen_)
        showFullScreen();
    else
        showNormal();
#ifdef Q_OS_ANDROID
    // 系统栏消失/恢复会改变 availableGeometry：安全区要重算一次，
    // 否则全屏下顶部仍留着状态栏高度的空白（或退出后内容又钻到状态栏底下）。
    QTimer::singleShot(150, this, [this] { ApplySafeArea(); });
#endif
    // 全屏只剩图片：阅读器工具条（浮层）一并收起，点按左右上角可再叫出来
    if (reader_)
        reader_->SetFullScreenMode(readerFullScreen_);
    // 用 Toast 而不是 SetStatus：状态行是页码，写"全屏"会把页码顶掉且不会自己恢复
    Toast::Show(this, readerFullScreen_ ? QStringLiteral("全屏：点按左右上角可唤出工具条")
                                        : QStringLiteral("已退出全屏"));
    qWarning("reader-fullscreen: %d", int(readerFullScreen_));
}

void MainWindow::RealSearch(const QString &keyword)
{
    Go(PageId::Search, false);
    search_->Search(keyword);
}

void MainWindow::RealLogin(const QString &user, const QString &pass)
{
    EnsureApi();
    Go(PageId::Login, false);
    api_->Login(user, pass);
}

void MainWindow::LoadMoreLatest()
{
    EnsureApi();
    if (home_)
        home_->SelectSection(0);
    api_->FetchLatestPage(1);
}

void MainWindow::RefreshFavoriteList()
{
    EnsureApi();
    auto *fp = qobject_cast<FavoritePage *>(pages_[int(PageId::Favorite)]);
    if (!UserSession::User().loggedIn || !api_->LoggedIn()) {
        // 没有会话：明确显示"去登录"，不要发注定 401 的请求
        if (fp)
            fp->SetState(FavoritePage::State::NotLoggedIn);
        return;
    }
    if (fp && fp->CurrentState() != FavoritePage::State::Ready
        && fp->CurrentState() != FavoritePage::State::Empty) {
        fp->SetState(FavoritePage::State::Loading);
    }
    api_->FavoriteFolder(1);
}



QString MainWindow::UserReport() const
{
    const UserState u = UserSession::User();
    const auto *fp = qobject_cast<FavoritePage *>(pages_.value(int(PageId::Favorite)));
    QString text = QStringLiteral("loggedIn=%1 name=%2 title=%3 coins=%4 level=%5 "
                                  "fav=%6/%7 exp=%8/%9 navCoins=%10 navLevel=%11 "
                                  "navFav=%12 favState=%13 cards=%14");
    text = text.arg(int(u.loggedIn));
    text = text.arg(u.name);
    text = text.arg(u.title);
    text = text.arg(u.coins);
    text = text.arg(u.level);
    text = text.arg(u.favorites);
    text = text.arg(u.favoritesMax);
    text = text.arg(u.exp);
    text = text.arg(u.nextExp);
    text = text.arg(navCoins_ ? navCoins_->text() : QString());
    text = text.arg(navLevel_ ? navLevel_->text() : QString());
    text = text.arg(navFav_ ? navFav_->text() : QString());
    text = text.arg(fp ? fp->StateName() : QStringLiteral("none"));
    text = text.arg(fp ? fp->CardCount() : 0);
    return text;
}



void MainWindow::PickProxy(bool imageDomain)
{
    Q_UNUSED(imageDomain);
    // 侧栏的“分流”两行都进同一个页面：接口/图床在同一张表里分别单选
    Go(PageId::Proxy);
}

void MainWindow::OpenProxyPage()
{
    // 曾经是模态 QDialog：真机 V2463A 上能显示但按钮点不动（桌面正常）。
    // 改成主窗口内的页面后走同一条导航/触摸事件链，问题从根上消失。
    Go(PageId::Proxy);
}

void MainWindow::DemoProxyTest(const QString &kind)
{
    // 桌面/自动化：直接进分流页并开跑，省掉“点进设置再点测速”的两步
    Go(PageId::Proxy, false);
    auto *page = qobject_cast<ProxyPage *>(pages_[int(PageId::Proxy)]);
    if (!page)
        return;
    if (kind == QLatin1String("adv")) {
        page->ExpandAdvanced();   // 只展开高级抽屉，不跑测速
    } else if (kind.startsWith(QLatin1String("adv"))) {
        // 地区下拉的回归入口，一个进程只做一件事（QMenu 是 popup 窗口，连着弹会互相干扰）：
        //   advmenu=打印菜单项（真实数据），advmenudemo=注入示例地区后打印菜单项，
        //   advapply=点第 2 项验证"选中即应用"，advclick=点按验证弹菜单，
        //   advdrag=按下后拖动验证不误触
        const bool seed = kind != QLatin1String("advmenu");
        if (seed) {
            // 截图用：造几个地区让下拉有内容（只改内存，不落盘）
            page->SeedRegionCandidatesForTest({QStringLiteral("美国·Los Angeles"),
                                               QStringLiteral("日本·Tokyo"),
                                               QStringLiteral("中国香港·Hong Kong")});
        }
        page->ExpandAdvanced();
        QTimer::singleShot(250, page, [page, kind] {
            if (kind == QLatin1String("advapply")) {
                qWarning("proxy-region-menu: applied=%s",
                         qPrintable(page->TriggerRegionMenuForTest(1)));
            } else if (kind == QLatin1String("advclick")) {
                // 点按判定：同点 = 点击；位移 20px = 滑动（不弹菜单）
                const QPoint base(200, 400);
                qWarning("proxy-region-menu: tap=%d drag=%d",
                         int(page->IsRegionTap(base, base)),
                         int(page->IsRegionTap(base, base + QPoint(20, 0))));
            } else {
                const QStringList items = page->PopupRegionMenuForTest();
                qWarning("proxy-region-menu: items=%d [%s]", int(items.size()),
                         qPrintable(items.join(QStringLiteral(" | "))));
            }
        });
    } else if (kind == QLatin1String("nodes")) {
        page->StartNodeTest();
    } else {
        page->StartLineTest();
    }
}

void MainWindow::UseNodeForTest(const QString &ip)
{
    ProxyConfig::SetApiLine(5);   // CDN 线路：接口域名 www.cdngwc.cc
    ProxyConfig::SetImgLine(5);   // 图床域名 cdn-msp.jmapiproxy3.cc
    ProxyConfig::SetSelectedIp(ip);
    ProxyConfig::Save();
    ApplyProxySettings();
    qWarning("use-node: ip=%s apiLine=5 imgLine=5", qPrintable(ip));
}

void MainWindow::ProxySelfTest()
{
    // 人造四种典型节点：完全不通 / 正常低延迟 / 正常低分 / ping 通但图片失败
    auto make = [](const QString &ip, int delay, int download, bool pingFail, bool imgFail) {
        NodeResult r;
        r.ip = ip;
        r.delayMs = delay;
        r.downloadMs = download;
        r.failed = pingFail;
        r.downloadFailed = imgFail;
        return r;
    };
    QVector<NodeResult> rs;
    rs << make(QStringLiteral("10.0.0.9"), 0, 0, true, true);
    rs << make(QStringLiteral("10.0.0.1"), 400, 400, false, false);
    rs << make(QStringLiteral("10.0.0.2"), 100, 900, false, false);
    rs << make(QStringLiteral("10.0.0.3"), 200, 0, false, true);

    QVector<NodeResult> sorted = rs;
    ProxyConfig::SortResults(sorted, QString());
    QStringList order;
    for (const NodeResult &r : sorted)
        order << r.ip;

    const QString best = ProxyConfig::PickBest(rs, QString());
    const QString keepImgFail = ProxyConfig::PickBest(rs, QStringLiteral("10.0.0.3"));
    // Python CheckSelectBox：选中项只要还在列表里就保持，哪怕它 ping 失败
    const QString keepPingFail = ProxyConfig::PickBest(rs, QStringLiteral("10.0.0.9"));
    std::fprintf(stderr, "proxy-selftest: order=%s best=%s keep=%s keepfail=%s\n",
                 qPrintable(order.join(QLatin1Char(','))), qPrintable(best),
                 qPrintable(keepImgFail), qPrintable(keepPingFail));

    // ---- 与 Python 版对齐的确定性事实（套件逐项断言） ----
    const QVector<int> display = ProxyConfig::DisplayOrder();
    QStringList idx, labels;
    for (int i : display) {
        idx << QString::number(i);
        labels << ProxyConfig::LineLabel(i);
    }
    std::fprintf(stderr, "proxy-align: lines=%d order=%s labels=%s\n",
                 int(ProxyConfig::Lines().size()), qPrintable(idx.join(QLatin1Char(','))),
                 qPrintable(labels.join(QLatin1Char(','))));

    const QVector<NodeResult> all = ProxyConfig::AllNodes();
    int nProxy = 0, nCustom = 0, nCf = 0;
    for (const NodeResult &r : all) {
        switch (r.kind) {
        case NodeKind::ProxyIp: ++nProxy; break;
        case NodeKind::Custom: ++nCustom; break;
        default: ++nCf; break;
        }
    }
    std::fprintf(stderr, "proxy-align: nodes=%d proxy=%d custom=%d cf=%d first=%s last=%s\n",
                 int(all.size()), nProxy, nCustom, nCf,
                 qPrintable(all.first().name), qPrintable(all.last().name));
    int nFilterProxy = 0, nFilterCf = 0;
    for (const NodeResult &r : all) {
        if (ProxyConfig::MatchFilter(r.kind, NodeFilter::ProxyGroup))
            ++nFilterProxy;
        if (ProxyConfig::MatchFilter(r.kind, NodeFilter::PreferredGroup))
            ++nFilterCf;
    }
    std::fprintf(stderr, "proxy-align: filter proxy=%d cf=%d\n", nFilterProxy, nFilterCf);

    // ---- 「自定义ProxyIP地区」的纯逻辑：中文名 ↔ 二字代码、候选去重排序 ----
    // 只输出 ASCII（zh-CN 控制台是 GBK，中文会让回归脚本在解码阶段就失配）。
    QVector<NodeResult> regionNodes;
    for (const char *country : {"美国·Los Angeles", "日本·Tokyo", "美国·New York", ""}) {
        NodeResult r;
        r.country = QString::fromUtf8(country);
        regionNodes << r;
    }
    const QStringList regionNames = ProxyConfig::RegionNames(regionNodes);
    QStringList regionCodes;
    for (const QString &n : regionNames)
        regionCodes << RegionCode::ToCode(n);
    std::fprintf(stderr,
                 "proxy-align: region code(us/jp)=%s/%s name(us/hk/cn)=%s/%s/%s "
                 "unknown=%d names=%d codes=%s\n",
                 qPrintable(RegionCode::ToCode(QStringLiteral("美国·Los Angeles"))),
                 qPrintable(RegionCode::ToCode(QStringLiteral("日本"))),
                 qPrintable(RegionCode::ToName(QStringLiteral("us"))),
                 qPrintable(RegionCode::ToName(QStringLiteral("hk"))),
                 qPrintable(RegionCode::ToName(QStringLiteral("cn"))),
                 int(RegionCode::ToCode(QStringLiteral("火星")).isEmpty()),
                 int(regionNames.size()), qPrintable(regionCodes.join(QLatin1Char(','))));

    std::fprintf(stderr, "proxy-align: defaults api=%d img=%d ip=%s mode=%d apiUrl=%s imgUrl=%s\n",
                 ProxyConfig::ApiLine(), ProxyConfig::ImgLine(),
                 qPrintable(ProxyConfig::SelectedIp().isEmpty() ? QStringLiteral("-")
                                                               : ProxyConfig::SelectedIp()),
                 int(ProxyConfig::Mode()), qPrintable(ProxyConfig::GetApiUrl2(1)),
                 qPrintable(ProxyConfig::GetImgUrl2(1)));
    std::fprintf(stderr, "proxy-align: fallback api7=%s img0=%s\n",
                 qPrintable(ProxyConfig::GetApiUrl2(7)),
                 qPrintable(ProxyConfig::GetImgUrl2(0)));
    const RoutePlan route =
        ProxyConfig::ResolveRoute(QUrl(ProxyConfig::GetApiUrl2(1) + QStringLiteral("/latest")),
                                  true);
    std::fprintf(stderr, "proxy-align: route line=%d viaNode=%d reason=%s\n",
                 route.line, int(route.viaNode), qPrintable(route.reason));

    // ---- DOH / ECH / HTTP3 的配置默认值与后端能力 ----
    // 0.90 补录：C++ 壳的 HTTP3 默认开，刻意与 Python ProxySetting 默认关分歧。
    std::fprintf(stderr,
                 "proxy-align: advanced doh=%s active=%d ech=%d http3=%d supported=%d "
                 "echOff=%d http3Off=%d %s\n",
                 qPrintable(ProxyConfig::DohAddress()), int(ProxyConfig::DohEnabled()),
                 int(ProxyConfig::EchEnabled()), int(ProxyConfig::Http3Enabled()),
                 int(ProxyConfig::AdvancedTlsSupported()), int(ProxyConfig::EchOff()),
                 int(ProxyConfig::Http3Off()), qPrintable(NetBackend::Summary()));
    // Q5：本次会话的回退/降级计数（与页面高级区同一份数据源）。
    // 机器解析行一律 ASCII key=value：stderr 在 zh-CN 控制台是 GBK，
    // 中文会让回归脚本的 (\d+) 断言在解码阶段就失配。
    const ProxyConfig::AdvancedCounters cnt = ProxyConfig::Counters();
    std::fprintf(stderr, "proxy-align: counters h3ok=%d h3fail=%d h3fb=%d echok=%d echfail=%d\n",
                 cnt.h3Ok, cnt.h3Fail, cnt.h3Fallback, cnt.echOk, cnt.echFail);

    // ---- DoH DNS wire 解析的确定性自检（不联网）：A 记录 + HTTPS RR 的 ech 参数 ----
    QByteArray hdr(12, '\0');
    hdr[2] = char(0x81);
    hdr[3] = char(0x80);
    hdr[5] = char(1);   // QDCOUNT
    hdr[7] = char(1);   // ANCOUNT
    QByteArray qn;
    qn.append(char(1)).append('a').append(char(4)).append("test", 4).append('\0');
    qn.append(char(0)).append(char(1)).append(char(0)).append(char(1));
    auto answer = [&hdr, &qn](quint16 type, const QByteArray &rdata) {
        QByteArray an;
        an.append(char(0xC0)).append(char(0x0C));                     // name -> question
        an.append(char(type >> 8)).append(char(type & 0xff));
        an.append(char(0)).append(char(1));                           // class IN
        an.append(char(0)).append(char(0)).append(char(1)).append(char(0x2C));  // ttl 300
        an.append(char(rdata.size() >> 8)).append(char(rdata.size() & 0xff));
        an.append(rdata);
        return hdr + qn + an;
    };
    const QByteArray rdataA = QByteArray("\x01\x02\x03\x04", 4);
    const DohResult parsedA = DohClient::ParseWire(answer(1, rdataA), QStringLiteral("a.test"));
    QByteArray rdataH;
    rdataH.append(char(0)).append(char(1));       // priority
    rdataH.append('\0');                          // target = root
    rdataH.append(char(0)).append(char(5));       // SvcParam key 5 = ech
    rdataH.append(char(0)).append(char(3));
    rdataH.append("ech", 3);
    const DohResult parsedH = DohClient::ParseWire(answer(65, rdataH),
                                                   QStringLiteral("cloudflare-ech.com"));
    const QByteArray dnsQuery = DohClient::BuildDnsQuery(QStringLiteral("cloudflare-ech.com"), 65);
    std::fprintf(stderr, "proxy-align: dohquery len=%d a=%s ech=%d wire=%d\n",
                 dnsQuery.size(), qPrintable(parsedA.ips.join(QLatin1Char(','))),
                 int(!parsedH.echConfig.isEmpty()), int(parsedA.fromDoh));
    std::fflush(stderr);
}

void MainWindow::EchProbe(const QString &host)
{
    ProxyConfig::Load();
    ProxyConfig::SyncDohClient();

    // ECHConfigList 来自 DoH 的 HTTPS RR；没拿到就等一会儿（最多 8s）
    if (DohClient::I().CachedEch().isEmpty()) {
        QEventLoop wait;
        QTimer::singleShot(8000, &wait, &QEventLoop::quit);
        QObject::connect(&DohClient::I(), &DohClient::EchReady, &wait, &QEventLoop::quit);
        wait.exec();
    }
    const QString b64 = DohClient::I().CachedEch();
    const QByteArray ech = b64.isEmpty() ? QByteArray() : QByteArray::fromBase64(b64.toLatin1());
    qWarning("ech-probe: host=%s backend=%s echConfig=%dB b64=%s", qPrintable(host),
             qPrintable(EchChannelStatus()), ech.size(),
             qPrintable(QString::fromLatin1(ech.toBase64())));

    TlsChannel *ch = CreateEchChannel(this);
    if (!ch) {
        qWarning("ech-probe: no ECH TLS backend");
        return;
    }
    QEventLoop loop;
    bool done = false;
    QObject::connect(ch, &TlsChannel::Connected, &loop, [&] {
        const TlsInfo i = ch->Info();
        qWarning("ech-probe: host=%s ok ech=%s(%d) version=%s alpn=%s verify=%d/%ld",
                 qPrintable(host), qPrintable(i.echText), i.echStatus, qPrintable(i.version),
                 qPrintable(i.alpn.isEmpty() ? QStringLiteral("-") : i.alpn), int(i.verifyOk),
                 i.verifyResult);
        done = true;
        loop.quit();
    });
    QObject::connect(ch, &TlsChannel::Error, &loop, [&](const QString &e) {
        qWarning("ech-probe: host=%s failed: %s", qPrintable(host), qPrintable(e));
        done = true;
        loop.quit();
    });
    QObject::connect(ch, &TlsChannel::Log, this,
                     [](const QString &l) { qWarning("ech-probe: %s", qPrintable(l)); });
    QTimer::singleShot(15000, &loop, &QEventLoop::quit);

    if (!ch->Connect(host, 443, host, ech,
                     {QStringLiteral("h2"), QStringLiteral("http/1.1")})) {
        qWarning("ech-probe: connect start failed");
        ch->deleteLater();
        return;
    }
    loop.exec();
    if (!done)
        qWarning("ech-probe: timeout");
    ch->Close();
    ch->deleteLater();
}

void MainWindow::H3Probe(const QString &host, const QString &path, bool useEch)
{
    ProxyConfig::Load();
    ProxyConfig::SyncDohClient();

    // DoH 解析出的 IP 直接给 QUIC 用（等价 curl --resolve，证书仍按域名校验）
    QString ip;
    if (useEch || ProxyConfig::DohEnabled()) {
        DohClient::I().Resolve(host);
        QEventLoop wait;
        QTimer::singleShot(6000, &wait, &QEventLoop::quit);
        QObject::connect(&DohClient::I(), &DohClient::Resolved, &wait, &QEventLoop::quit);
        wait.exec();
        const QStringList ips = DohClient::I().CachedIps(host);
        if (!ips.isEmpty())
            ip = ips.first();
    }

    QByteArray ech;
    if (useEch) {
        if (DohClient::I().CachedEch().isEmpty()) {
            QEventLoop wait;
            QTimer::singleShot(8000, &wait, &QEventLoop::quit);
            QObject::connect(&DohClient::I(), &DohClient::EchReady, &wait, &QEventLoop::quit);
            wait.exec();
        }
        const QString b64 = DohClient::I().CachedEch();
        if (!b64.isEmpty())
            ech = QByteArray::fromBase64(b64.toLatin1());
    }

    qWarning("h3-probe: host=%s path=%s ip=%s ech=%dB backend=%s", qPrintable(host),
             qPrintable(path), qPrintable(ip.isEmpty() ? QStringLiteral("-") : ip),
             ech.size(), qPrintable(quic::BackendSummary()));

    quic::Client client(this);
    QObject::connect(&client, &quic::Client::Log, this,
                     [](const QString &l) { qWarning("h3-probe: %s", qPrintable(l)); });

    quic::Request req;
    req.host = host;
    req.connectIp = ip;
    req.path = path;
    req.echConfigList = ech;
    req.headers.append(qMakePair(QByteArrayLiteral("user-agent"),
                                 QByteArrayLiteral("jmcomic-cpp/0.88")));
    req.timeoutMs = 25000;
    req.headers.append(qMakePair(QByteArrayLiteral("accept"), QByteArrayLiteral("*/*")));

    QEventLoop loop;
    bool done = false;
    client.Start(req, [&](const quic::Response &r) {
        qWarning("h3-probe: done ok=%d status=%d bytes=%lld %ldms version=%s alpn=%s "
                 "tls=%s ech=%s echRejected=%d err=%s",
                 int(r.ok), r.status, (long long)r.body.size(), long(r.elapsedMs),
                 qPrintable(r.version), qPrintable(r.alpn), qPrintable(r.tlsVersion),
                 r.echText.toUtf8().constData(), int(r.echRejected),
                 r.error.toUtf8().constData());
        if (!r.body.isEmpty()) {
            const QString head = QString::fromUtf8(r.body.left(120)).simplified();
            qWarning("h3-probe: body[0..120]=%s", qPrintable(head));
        }
        done = true;
        loop.quit();
    });
    QTimer::singleShot(25000, &loop, &QEventLoop::quit);
    loop.exec();
    if (!done)
        qWarning("h3-probe: timeout");
}

void MainWindow::H3Burst(const QString &host, const QString &path, int count)
{
    ProxyConfig::Load();
    ProxyConfig::SyncDohClient();
    count = qBound(1, count, 20);

    quic::Request req;
    req.host = host;
    req.path = path;
    req.timeoutMs = 20000;
    req.headers.append(qMakePair(QByteArrayLiteral("user-agent"),
                                 QByteArrayLiteral("jmcomic-cpp/0.89")));
    req.headers.append(qMakePair(QByteArrayLiteral("accept"), QByteArrayLiteral("*/*")));

    struct Agg
    {
        int ok = 0;
        int done = 0;
        int n = 0;
        qint64 maxMs = 0;
    };
    auto agg = std::make_shared<Agg>();
    agg->n = count;

    auto *client = new quic::Client(this);
    QObject::connect(client, &quic::Client::Log, this,
                     [](const QString &l) { qWarning("h3-burst: %s", qPrintable(l)); });

    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);

    // 全部请求先同步投递（此时连接还没建立）：这正是池化要处理的场景
    for (int i = 0; i < count; ++i) {
        client->Start(req, [&loop, agg](const quic::Response &r) {
            if (r.ok)
                ++agg->ok;
            agg->maxMs = qMax(agg->maxMs, r.elapsedMs);
            if (++agg->done >= agg->n)
                loop.quit();
        });
    }
    deadline.start(45000);
    loop.exec();
    deadline.stop();

    const quic::PoolStats s = quic::Stats();
    // 断言口径：n 个同域请求 → handshakes 应为 1，reused 应为 n-1
    qWarning("h3-burst: n=%d ok=%d handshakes=%d reused=%d conns=%d jobs=%d queued=%d "
             "zr_tried=%d zr_ok=%d zr_rej=%d max=%ldms",
             count, agg->ok, s.handshakes, s.reused, s.connections, s.jobs, s.queued,
             s.zeroRttTried, s.zeroRttAccepted, s.zeroRttRejected, long(agg->maxMs));
    quic::ShutdownPool();
    client->deleteLater();
}

void MainWindow::H3Bench(const QString &host, const QString &path, int count)
{
    ProxyConfig::Load();
    count = qBound(1, count, 50);

    QList<qint64> totals;
    QList<qint64> ttfbs;
    int ok = 0;
    QString csv = QStringLiteral("i,ok,status,total_ms,ttfb_ms,bytes\n");

    for (int i = 1; i <= count; ++i) {
        // 每轮先清池再新建 Client：测的是"完整握手 + 首包"，而不是连接复用。
        // 只有不复用，握手期的 PTO/丢包抖动才会暴露出来（Q3 要的正是这个）。
        quic::ShutdownPool();

        quic::Client client;
        quic::Response got;
        bool done = false;
        QEventLoop loop;
        QObject::connect(&client, &quic::Client::Log, this,
                         [](const QString &l) { qWarning("h3-bench: %s", qPrintable(l)); });

        quic::Request req;
        req.host = host;
        req.path = path;
        req.timeoutMs = 15000;
        req.headers.append(qMakePair(QByteArrayLiteral("user-agent"),
                                     QByteArrayLiteral("jmcomic-cpp/0.90-bench")));

        client.Start(req, [&loop, &got, &done](const quic::Response &r) {
            got = r;
            done = true;
            loop.quit();
        });
        QTimer::singleShot(18000, &loop, &QEventLoop::quit);
        loop.exec();

        if (got.ok)
            ++ok;
        if (got.elapsedMs > 0)
            totals << got.elapsedMs;
        if (got.ttfbMs > 0)
            ttfbs << got.ttfbMs;
        qWarning("h3-bench: i=%d ok=%d status=%d total=%ldms ttfb=%ldms bytes=%d %s",
                 i, got.ok ? 1 : 0, got.status, long(got.elapsedMs), long(got.ttfbMs),
                 int(got.body.size()), done ? "done" : "timeout");
        csv += QStringLiteral("%1,%2,%3,%4,%5,%6\n")
                   .arg(i).arg(got.ok ? 1 : 0).arg(got.status)
                   .arg(got.elapsedMs).arg(got.ttfbMs).arg(got.body.size());
    }
    quic::ShutdownPool();

    auto pct = [](QList<qint64> v, double p) -> qint64 {
        if (v.isEmpty())
            return 0;
        std::sort(v.begin(), v.end());
        const int idx = qBound(0, int(std::ceil(p / 100.0 * v.size())) - 1, int(v.size()) - 1);
        return v.at(idx);
    };
    qint64 mn = 0;
    qint64 mx = 0;
    if (!totals.isEmpty()) {
        const auto mm = std::minmax_element(totals.cbegin(), totals.cend());
        mn = *mm.first;
        mx = *mm.second;
    }
    const qint64 p50 = pct(totals, 50);
    const qint64 p95 = pct(totals, 95);
    const qint64 t50 = pct(ttfbs, 50);
    const qint64 t95 = pct(ttfbs, 95);

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    const QString file = dir + QStringLiteral("/h3-bench-%1.csv")
                                  .arg(QDateTime::currentDateTime()
                                           .toString(QStringLiteral("yyyyMMdd-HHmmss")));
    QFile f(file);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(csv.toUtf8());
    f.close();

    qWarning("h3-bench: n=%d ok=%d p50=%ldms p95=%ldms min=%ldms max=%ldms "
             "ttfb_p50=%ldms ttfb_p95=%ldms csv=%s",
             count, ok, long(p50), long(p95), long(mn), long(mx),
             long(t50), long(t95), qPrintable(file));
}

void MainWindow::ApplyProxySettings()
{
    ProxyConfig::Load();
    ProxyConfig::SyncDohClient();
    const ProxyLine api = ProxyConfig::LineAt(ProxyConfig::ApiLine());
    const ProxyLine img = ProxyConfig::LineAt(ProxyConfig::ImgLine());
    UserState &u = UserSession::Ref();
    u.proxy = api.name;
    u.proxyImg = img.name;
    UpdateNavUser();

    if (!api_) {
        // API 还没建（首屏可能没走网络）：等 EnsureApi 后再应用
        return;
    }
    // 路由优先级集中在 ProxyConfig::ResolveRoute（反代 > 系统代理 > IP 覆盖 > 直连）
    const RoutePlan apiPlan =
        ProxyConfig::ResolveRoute(QUrl(api.apiBase + QStringLiteral("/")), true);
    const RoutePlan imgPlan =
        ProxyConfig::ResolveRoute(QUrl(img.imgBase + QStringLiteral("/")), false);
    api_->SetDomains({QUrl(api.apiBase).host()});
    api_->SetImageDomains({QUrl(img.imgBase).host()});
    if (apiPlan.line == 6)
        api_->SetProxyRoute(ProxyConfig::ProxyApiDomain2(), QUrl(api.apiBase).host());
    else
        api_->SetProxyRoute(QString(), QString());
    // DoH 解析出来的 IP 是按域名变化的，由 JmApi 在每次请求里查缓存；
    // 这里只能固定住"用户选中的节点"，不能把某个域名的 DoH 结果当成全局节点。
    api_->SetNodeOverride(apiPlan.viaDoh ? QString() : apiPlan.connectIp,
                          apiPlan.viaNode && !apiPlan.viaDoh);
    qWarning("proxy-apply: api=%s img=%s node=%s(%s) doh=%d reason=%s | %s",
             qPrintable(api.name), qPrintable(img.name),
             qPrintable(apiPlan.viaDoh ? apiPlan.connectIp : ProxyConfig::SelectedIp()),
             apiPlan.viaNode ? "on" : "off", int(apiPlan.viaDoh), qPrintable(apiPlan.reason),
             qPrintable(imgPlan.reason));
}

void MainWindow::EditHideWords()
{
    const UserState u = UserSession::User();
    const QString text = ModalDialog::Prompt(
        this, QStringLiteral("屏蔽词"),
        QStringLiteral("多个词用逗号分隔；命中分类、标签或标题的作品会被过滤。"),
        u.hideWordList.join(QStringLiteral(",")));
    if (text.isNull())
        return;   // 取消
    QStringList words;
    for (const QString &w : text.split(QRegularExpression(QStringLiteral("[,，]")))) {
        const QString t = w.trimmed();
        if (!t.isEmpty() && !words.contains(t))
            words << t;
    }
    UserSession::Ref().hideWordList = words;
    // 屏蔽词只存在 session.ini 里：不落库的话，退出登录或会话失效后会被
    // Load()/Clear() 当成空值覆盖掉（用户会以为"登录一次把屏蔽词吃了"）。
    SessionStore::Save();
    UpdateNavUser();
    Toast::Show(this, QStringLiteral("屏蔽词已更新：%1 个").arg(words.size()));
}

// ---------------------------------------------------------------- 打卡
void MainWindow::UpdateSignButton()
{
    if (!signBtn_)
        return;
    const UserState u = UserSession::User();
    const bool logged = u.loggedIn && !u.avs.isEmpty();
    signBtn_->setEnabled(true);
    signBtn_->setText(u.signedToday ? QStringLiteral("已打卡") : QStringLiteral("打卡"));
    signBtn_->setToolTip(logged ? (u.signedToday ? QStringLiteral("今天已打卡，第 2 天再来")
                                                 : QStringLiteral("每日打卡领 J Coins"))
                                : QStringLiteral("登录后可打卡"));
}

void MainWindow::SignIn()
{
    EnsureApi();
    const bool logged = api_->LoggedIn() && api_->Uid() != 0;
    if (!logged) {
        qWarning("sign: need-login loggedIn=%d uid=%lld",
                 int(api_->LoggedIn()), static_cast<long long>(api_->Uid()));
        Toast::Show(this, QStringLiteral("打卡需要先登录"));
        Go(PageId::Login);
        return;
    }
    if (UserSession::User().signedToday) {
        Toast::Show(this, QStringLiteral("今天已经打卡了"));
        return;
    }
    if (signBtn_) {
        signBtn_->setEnabled(false);
        signBtn_->setText(QStringLiteral("打卡中…"));
    }
    qWarning("sign: request uid=%lld", static_cast<long long>(api_->Uid()));
    api_->SignDaily();
}

QString MainWindow::SignButtonText() const
{
    return signBtn_ ? signBtn_->text() : QString();
}

QString MainWindow::SignButtonState() const
{
    if (!signBtn_)
        return QStringLiteral("none");
    if (!signBtn_->isEnabled())
        return QStringLiteral("busy");
    return UserSession::User().signedToday ? QStringLiteral("signed") : QStringLiteral("unsigned");
}

int MainWindow::NavActionCount() const
{
    int n = 0;
    for (QWidget *w : navActions_) {
        if (w && w->property("navActionBordered").toBool())
            ++n;
    }
    return n;
}

bool MainWindow::ClickNavAction(const QString &actionId)
{
    for (QWidget *w : navActions_) {
        if (!w || w->property("navAction").toString() != actionId)
            continue;
        const QPoint local = w->rect().center();
        const QPoint global = w->mapToGlobal(local);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(global),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(w, &press);
        QCoreApplication::sendEvent(w, &release);
        qWarning("nav-action: click %s page=%d", qPrintable(actionId), int(current_));
        return true;
    }
    return false;
}

void MainWindow::SetReadingModeForTest(int mode)
{
    if (auto *sv = qobject_cast<SettingsView *>(pages_[int(PageId::Settings)]))
        sv->SetReadingMode(mode);
    if (reader_)
        reader_->SetReadingMode(mode, false);   // 回归入口不污染用户偏好
    qWarning("reader-mode: set=%d actual=%d vertical=%d", mode, ReaderModeForTest(),
             int(reader_ && reader_->IsVerticalReading()));
}

int MainWindow::ReaderModeForTest() const
{
    return reader_ ? reader_->ReadingMode() : 0;
}

void MainWindow::LoadReaderDirForTest(const QString &dir)
{
    Go(PageId::Reader, false);
    if (!reader_)
        return;
    const int n = reader_->LoadDirForTest(dir);
    qWarning("reader-dir: %s pages=%d", qPrintable(dir), n);
}

void MainWindow::RunReaderParityForTest()
{
    if (reader_)
        reader_->RunReaderParitySelfTest();
}

void MainWindow::RunReaderTouchForTest()
{
    if (reader_)
        reader_->RunReaderTouchSelfTest();
}

void MainWindow::RunReaderProgressiveForTest(const QString &dir)
{
    Go(PageId::Reader, false);
    if (reader_)
        reader_->RunReaderProgressiveSelfTest(dir);
}

void MainWindow::ReaderSwipeForTest(int dx, int dy)
{
    if (reader_)
        reader_->SwipeDemo(dx, dy);
    qWarning("reader-swipe: dx=%d dy=%d mode=%d page=%d", dx, dy, ReaderModeForTest(),
             reader_ ? int(reader_->CurrentPage()) : -1);
}

void MainWindow::ReaderPinchForTest(qreal ratio)
{
    if (reader_)
        reader_->PinchDemo(ratio);
    qWarning("reader-pinch: ratio=%.2f", ratio);
}

void MainWindow::ReaderSrForTest()
{
    if (reader_)
        reader_->ToggleSrForTest();
    qWarning("reader-sr: %s", qPrintable(ReaderSrStatusForTest()));
}

QString MainWindow::ReaderSrStatusForTest()
{
    return reader_ ? reader_->SrStatusForTest() : QStringLiteral("no-reader");
}

void MainWindow::ReaderMenuForTest()
{
    if (reader_)
        reader_->OpenMenuForTest();
}

QString MainWindow::HomeGeometryReport() const
{
    return home_ ? home_->GeometryReport() : QStringLiteral("no-home");
}

QString MainWindow::HomeStripState() const
{
    return home_ ? home_->StripStateReport() : QStringLiteral("no-home");
}

void MainWindow::HomeStripDrag(int dx)
{
    if (!home_)
        return;
    const int before = home_->CurrentSection();
    const int off0 = home_->StripOffset();
    home_->StripDrag(dx);
    qWarning("strip: drag=%d offset %d->%d content=%d section %d->%d dragging=%d", dx, off0,
             home_->StripOffset(), home_->StripContentWidth(), before, home_->CurrentSection(),
             int(home_->StripDragging()));
}

void MainWindow::HomeStripTap(int x)
{
    if (home_)
        home_->StripTap(x);
}

// ---------------------------------------------------------------- 防误触回归
//
// 五个方向都要能过（任一不过都说明阈值/复位写歪了）：
//   * 侧栏按钮：位移超阈值 → 不跳页；位移为 0 → 照常跳页；
//   * 回摆：划出去再划回原点（首末差≈0）→ 仍然不跳页（看的是过程峰值位移）；
//   * 下拉框：在框上滑 → 不弹窗、不换选中；点一下 → 弹窗；
//   * 弹窗列表：在列表内滑 → 不提交、列表不关、不断言被误选的项；
//     列表内点选 → 正常提交并关闭；
//   * 二级菜单：按在有子菜单的项上停留后再滑走 → 不 triggered、子菜单不残留、
//     activeAction 归空；正常点击 → 恰好 triggered 一次。
//
// 菜单必须真实弹出才能验（QMenu 是独立 popup 窗口，失活会被系统立刻关掉），
// 所以 --quiet 下这一段跳过（menu_skipped=1），菜单项要单独跑一次带焦点的：
//   jmcomic_ui.exe --tapguard-selftest --shot out.png --shot-delay 4500
namespace {

void TapSendMouse(QWidget *target, QEvent::Type type, const QPoint &global,
                  Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent ev(type, QPointF(target->mapFromGlobal(global)), QPointF(global),
                   button, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(target, &ev);
}

void TapWait(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

void TapOnce(QWidget *target, const QPoint &global)
{
    TapSendMouse(target, QEvent::MouseButtonPress, global, Qt::LeftButton, Qt::LeftButton);
    TapSendMouse(target, QEvent::MouseButtonRelease, global, Qt::LeftButton, Qt::NoButton);
}

// 一次滑动：按下 → 分步移动 → 抬起。
// holdMs 用于复现"先按住（让 hover 子菜单定时器起来）再滑走"的真实姿势。
void TapSwipe(QWidget *pressOn, QWidget *moveOn, const QPoint &from, const QPoint &to,
              int steps = 4, int holdMs = 0)
{
    TapSendMouse(pressOn, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton);
    if (holdMs > 0)
        TapWait(holdMs);
    for (int i = 1; i <= steps; ++i) {
        TapSendMouse(moveOn, QEvent::MouseMove, from + (to - from) * i / steps, Qt::NoButton,
                     Qt::LeftButton);
        TapWait(8);
    }
    TapSendMouse(moveOn, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton);
}

bool ComboPopupVisible(QComboBox *combo)
{
    QAbstractItemView *view = combo ? combo->findChild<QAbstractItemView *>() : nullptr;
    return view && view->isVisible();
}

}  // namespace

bool MainWindow::TapGuardSelfTest()
{
    if (navItems_.isEmpty())
        return false;
    Touch::ResetTapGuardStats();
    bool ok = true;

    // ---- 1) 侧栏按钮：滑动不跳页 ----
    QToolButton *btn = navItems_.first().second;   // "我的收藏"
    const PageId before = current_;
    const QPoint navPt = btn->mapToGlobal(btn->rect().center());

    TapSwipe(btn, btn, navPt, navPt + QPoint(0, 24));
    const bool navDragIgnored = current_ == before;
    const bool navReleased = !btn->isDown();

    // ---- 2) 回摆：划出去再划回原点，首末差为 0，但过程峰值已超阈值 ----
    TapSendMouse(btn, QEvent::MouseButtonPress, navPt, Qt::LeftButton, Qt::LeftButton);
    TapSendMouse(btn, QEvent::MouseMove, navPt + QPoint(0, 40), Qt::NoButton, Qt::LeftButton);
    TapSendMouse(btn, QEvent::MouseMove, navPt, Qt::NoButton, Qt::LeftButton);
    TapSendMouse(btn, QEvent::MouseButtonRelease, navPt, Qt::LeftButton, Qt::NoButton);
    const bool backSwipeIgnored = current_ == before;

    // ---- 3) 正常点击照旧生效 ----
    TapOnce(btn, navPt);
    const bool navClickWorked = current_ != before && current_ == PageId::Favorite;

    // ---- 4) 下拉选项框：框上滑动不弹窗；点一下弹窗；列表内滑动不提交 ----
    QFrame *panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("TapGuardTestPanel"));
    panel->setGeometry(8, 8, 280, 200);
    panel->raise();
    panel->show();

    QComboBox *combo = new QComboBox(panel);
    combo->setGeometry(12, 12, 220, 34);
    for (int i = 0; i < 12; ++i)
        combo->addItem(QStringLiteral("item-%1").arg(i));
    combo->show();
    TapWait(60);   // 等布局/样式就位，命中矩形才不是 0x0

    const QPoint comboPt = combo->mapToGlobal(combo->rect().center());
    // 按下后停 220ms 再滑：这个样式的弹窗是"延时弹出"的（实测 ~150ms），
    // 正好落在手指开始滑动之后 —— 不先等它出现就测不到真实的冲突场景。
    TapSendMouse(combo, QEvent::MouseButtonPress, comboPt, Qt::LeftButton, Qt::LeftButton);
    TapWait(220);
    for (int i = 1; i <= 4; ++i) {
        TapSendMouse(combo, QEvent::MouseMove, comboPt + QPoint(0, 13 * i), Qt::NoButton,
                     Qt::LeftButton);
        TapWait(8);
    }
    TapSendMouse(combo, QEvent::MouseButtonRelease, comboPt + QPoint(0, 54), Qt::LeftButton,
                 Qt::NoButton);
    TapWait(160);
    const bool comboSwipeOk = combo->currentIndex() == 0 && !ComboPopupVisible(combo);

    TapOnce(combo, comboPt);
    TapWait(320);   // 等延时弹窗真的出现
    const bool comboOpened = ComboPopupVisible(combo);

    bool popupSwipeOk = false;
    bool popupTapOk = false;
    if (comboOpened) {
        QAbstractItemView *view = combo->findChild<QAbstractItemView *>();
        QWidget *vp = view->viewport();
        const auto centerOf = [view, vp](int row) {
            const QRect r = view->visualRect(view->model()->index(row, 0));
            return vp->mapToGlobal(r.center());
        };
        // 列表内滑动：按下第 1 项 → 滑 72px → 抬起，不得提交、不得关闭
        TapSwipe(vp, vp, centerOf(1), centerOf(1) + QPoint(0, 72));
        TapWait(160);
        const bool stillOpen = ComboPopupVisible(combo);
        const bool indexKept = combo->currentIndex() == 0;
        // 列表内点选：必须正常提交并关闭
        TapOnce(vp, centerOf(3));
        TapWait(200);
        popupSwipeOk = stillOpen && indexKept;
        popupTapOk = combo->currentIndex() == 3 && !ComboPopupVisible(combo);
        if (!popupTapOk && ComboPopupVisible(combo))
            combo->hidePopup();
    }

    // ---- 5) 二级菜单：按住（触发 hover 子菜单）再滑走，不得 triggered/残留 ----
    QMenu *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("TapGuardTestMenu"));
    QAction *alpha = menu->addAction(QStringLiteral("alpha"));
    menu->addAction(QStringLiteral("beta"));
    QMenu *sub = menu->addMenu(QStringLiteral("more"));
    sub->addAction(QStringLiteral("sub-1"));
    int fired = 0;
    connect(menu, &QMenu::triggered, this, [&fired](QAction *) { ++fired; });
    connect(sub, &QMenu::triggered, this, [&fired](QAction *) { ++fired; });

    menu->popup(mapToGlobal(QPoint(60, 40)));
    TapWait(120);
    bool menuDragOk = false;
    bool menuTapOk = false;
    // --quiet 把窗口挪到屏幕外、不激活：QMenu 弹出后会被系统立刻收回，
    // 这一段没有可断言的对象，跳过（报告里 menu_skipped=1）。
    const bool menuUsable = menu->isVisible();
    if (menuUsable) {
        const QPoint onSub = menu->mapToGlobal(menu->actionGeometry(sub->menuAction()).center());
        const QPoint onBeta = menu->mapToGlobal(menu->actionGeometry(menu->actions().at(1)).center());
        TapSwipe(menu, menu, onSub, onBeta, 3, 280);
        TapWait(160);
        menuDragOk = fired == 0 && menu->activeAction() == nullptr && !sub->isVisible();
        if (!menu->isVisible()) {
            menu->popup(mapToGlobal(QPoint(60, 40)));
            TapWait(120);
        }
        TapOnce(menu, menu->mapToGlobal(menu->actionGeometry(alpha).center()));
        TapWait(120);
        menuTapOk = fired == 1;
    }
    menu->close();
    delete menu;
    delete panel;

    ok = navDragIgnored && navReleased && backSwipeIgnored && navClickWorked && comboSwipeOk
         && comboOpened && popupSwipeOk && popupTapOk
         && (!menuUsable || (menuDragOk && menuTapOk));

    qWarning("tapguard-selftest: nav_drag=%d nav_release=%d back_swipe=%d nav_click=%d "
             "combo_swipe=%d combo_open=%d popup_swipe=%d popup_tap=%d menu_drag=%d "
             "menu_tap=%d menu_skipped=%d swallowed=%d style=%s",
             int(navDragIgnored), int(navReleased), int(backSwipeIgnored), int(navClickWorked),
             int(comboSwipeOk), int(comboOpened), int(popupSwipeOk), int(popupTapOk),
             int(menuDragOk), int(menuTapOk), int(!menuUsable), Touch::TapGuardSwallowed(),
             qPrintable(QString::fromLatin1(QApplication::style()->metaObject()->className())));
    return ok;
}

void MainWindow::ClickReaderFitForTest()
{
    if (!reader_)
        return;
    reader_->ClickFitButtonForTest();
    qWarning("reader-fit: zoom=%d%%", reader_->ZoomPercent());
}

// 抽屉里点一个导航项：等价于手指点 NavItem（Go(id, false)），但必须先真的把
// 抽屉打开 —— 只有 drawerOpen_ 为真才会走到"先收抽屉、再播换页动画"的路径。
void MainWindow::DrawerNavForTest(PageId id)
{
    if (!drawerMode_)
        SetDrawerMode(true);
    OpenDrawer();
    qWarning("drawer-nav: open=1 target=%d", int(id));
    // 抽屉滑入用 Motion::Slow；等它到位再点，和用户的操作节奏一致
    QTimer::singleShot(Motion::Slow + 80, this, [this, id] {
        Go(id, false);
        qWarning("drawer-nav: clicked target=%d page=%d drawer=%d", int(id), int(current_),
                 int(drawerOpen_));
    });
}

PageBase *MainWindow::FindPage(PageId id) const
{
    const int idx = int(id);
    if (idx < 0 || idx >= pages_.size())
        return nullptr;
    return pages_.at(idx);
}

void MainWindow::UpdateNavMetrics()
{
    // 侧栏宽度、行高、图标、头像都跟着字号缩放：不缩放时 12pt 会把
    // "分类与排行"挤成两行（Python 侧栏 260px，是按 9pt 排的）。
    if (navRail_ && !navCollapsed_ && !drawerMode_)
        navRail_->setFixedWidth(UiStyle::Fpx(260));
    if (drawerMode_)
        UpdateDrawerGeometry();   // 抽屉宽度由几何算，绝不能用 setFixedWidth 锁死
    for (const auto &item : navItems_) {
        item.second->setMinimumHeight(UiStyle::Fpx(40));
        item.second->setIconSize(QSize(UiStyle::Fpx(32), UiStyle::Fpx(32)));
    }
    if (navAvatar_)
        navAvatar_->setFixedSize(UiStyle::Fpx(88), UiStyle::Fpx(88));
    if (crumbBar_)
        crumbBar_->setFixedHeight(UiStyle::Fpx(44));
    if (crumbMenu_)
        crumbMenu_->setFixedSize(UiStyle::Fpx(30), UiStyle::Fpx(30));
    UpdateIcons();
}

void MainWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    // 冷启动的会话校验：session.ini 里只有缓存值，服务端是否还认这个 AVS
    // 只能靠一次真实请求确认（/favorite 401 = 失效）。延迟到首屏之后，
    // 不和首页 /promote + /latest 抢单通道。
    if (!UserSession::User().avs.isEmpty()) {
        QTimer::singleShot(1500, this, [this] {
            EnsureApi();
            ProbeSession();
        });
    }
    if (!history_.isEmpty())
        return;
    // 首屏动画：轻微上浮，避免"啪"地出现
    setWindowOpacity(0.0);
    auto *timer = new QTimer(this);
    int step = 0;
    connect(timer, &QTimer::timeout, this, [this, timer, step]() mutable {
        step += 1;
        setWindowOpacity(qMin(1.0, step / 6.0));
        if (step >= 6)
            timer->stop();
    });
    timer->start(16);
}

void MainWindow::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Back || e->key() == Qt::Key_Escape)
        qWarning("jm-key: key=0x%x page=%d drawer=%d", int(e->key()),
                 static_cast<int>(current_), drawerOpen_ ? 1 : 0);
    // Android 的 back 会被投递两次（Qt 的 OnBackInvokedCallback + Activity 默认回调各一次，
    // 实测间隔约 320ms）。不去重会"连退两级"；但第二条正是 Activity 里真正 finish 的那条，
    // 所以首页退出不能再依赖"事件被忽略"，必须自己 quit（见下面）。
    if (e->key() == Qt::Key_Back) {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        // 首页（且抽屉没开、不是刚被返回带回来的）：两发都 ignore，
        // Activity 默认回调收到后才会真正 finish。这一段必须放在去重之前，
        // 否则第二发被 accept 掉，系统就永远不退。
        if (current_ == PageId::Home && !drawerOpen_ && now - lastBackNavMs_ > 700) {
            QWidget::keyPressEvent(e);
            return;
        }
        if (lastBackMs_ > 0 && now - lastBackMs_ < 600) {
            e->accept();
            return;
        }
        lastBackMs_ = now;

        if (drawerOpen_) {          // 抽屉优先于页面回退
            CloseDrawer();
            lastBackNavMs_ = now;
            return;
        }
        if (current_ != PageId::Home) {
            Back();
            lastBackNavMs_ = now;
            return;
        }
        // 刚因返回退到首页（<700ms）：这是同一次按下的重复投递，吃掉。
        // 不要在这里调 QCoreApplication::quit()：Android 上 Qt 的退出流程会在
        // 主线程里等图形栈收尾，实测触发 "Input dispatching timed out" 的 ANR。
        e->accept();
        return;
    }

    // Esc：桌面语义与返回键一致（抽屉 → 回退），但首页不退出
    if (e->key() == Qt::Key_Escape) {
        if (drawerOpen_) {
            CloseDrawer();
            return;
        }
        if (current_ != PageId::Home) {
            Back();
            return;
        }
    }
    QWidget::keyPressEvent(e);
}

bool MainWindow::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == drawerScrim_ && ev->type() == QEvent::MouseButtonPress) {
        CloseDrawer();
        return true;
    }
    // 侧栏三条可交互行：按下记位置，抬起时位移不大才算点击
    // （抽屉里想"滑动浏览"时手指也会从某一行开始，不能一滑就打开面板）
    if (QWidget *w = qobject_cast<QWidget *>(obj)) {
        const QString action = w->property("navAction").toString();
        if (!action.isEmpty()) {
            if (ev->type() == QEvent::MouseButtonPress) {
                auto *me = static_cast<QMouseEvent *>(ev);
                if (me->button() == Qt::LeftButton) {
                    navActionPress_ = me->globalPosition().toPoint();
                    navActionPressed_ = true;
                    navActionPressedId_ = action;
                }
            } else if (ev->type() == QEvent::MouseButtonRelease) {
                auto *me = static_cast<QMouseEvent *>(ev);
                const bool isTap = navActionPressed_ && navActionPressedId_ == action
                                  && (me->globalPosition().toPoint() - navActionPress_)
                                             .manhattanLength()
                                         <= Touch::Dp(12, this);
                navActionPressed_ = false;
                navActionPressedId_.clear();
                if (isTap && me->button() == Qt::LeftButton) {
                    if (action == QLatin1String("proxy")) {
                        PickProxy(false);
                    } else if (action == QLatin1String("proxyImg")) {
                        PickProxy(true);
                    } else if (action == QLatin1String("hide")) {
                        EditHideWords();
                    }
                    return true;
                }
            }
        }
    }
    if (obj == titleBar_) {
        if (ev->type() == QEvent::MouseButtonDblClick) {
            isMaximized() ? showNormal() : showMaximized();
            return true;
        }
        if (ev->type() == QEvent::MouseButtonPress) {
            auto *me = static_cast<QMouseEvent *>(ev);
            if (me->button() == Qt::LeftButton && !isMaximized() && windowHandle()) {
                windowHandle()->startSystemMove();
                return true;
            }
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void MainWindow::ScrollDemo(int times)
{
    // 每次滚到底 → QAbstractItemView 发现 canFetchMore 就调 fetchMore
    for (int i = 0; i < qMax(1, times); ++i)
        QTimer::singleShot(120 + i * 160, this, [this] { home_->ScrollToBottom(); });
}

bool MainWindow::HomeReady() const
{
    return home_ && home_->HasSections();
}

void MainWindow::HomeSwipe(int dx, int dy)
{
    if (home_)
        home_->SwipeDemo(dx, dy);
}

void MainWindow::HomeFreeze(int from, int to, qreal progress)
{
    if (home_)
        home_->FreezeSection(from, to, progress);
}

void MainWindow::SelectHomeSection(int index)
{
    Go(PageId::Home, false);
    if (home_)
        home_->SelectSection(index);
}

// ---------------------------------------------------------------- 抽屉
// 语义严格照 Python 的 tools/mobile_ui_drawer.py：
//   * 宽度 = clamp(win*0.82, 220, 300)；顶部避开菜单栏（否则会盖住菜单按钮）
//   * 遮罩 rgba(0,0,0,110)，点空白关闭
//   * 只做位移动画（x: -width → 0），120ms，开 OutCubic / 关 InCubic
//   * 关闭后 260ms 兜底隐藏，防止动画被打断时抽屉留在屏幕上
void MainWindow::SetDrawerMode(bool on)
{
    if (on == drawerMode_)
        return;
    drawerMode_ = on;

    if (on) {
        // 手机竖屏：放宽窗口最小尺寸，否则 480×900 会被 setMinimumSize(980,660) 卡到 980 宽
        setMinimumSize(UiStyle::Fpx(320), UiStyle::Fpx(480));
        if (navRail_ && bodyLayout_) {
            // 从常驻布局里摘出来，改成浮在主窗口上的覆盖层
            bodyLayout_->removeWidget(navRail_);
            navRail_->setParent(this);
            navRail_->setMinimumWidth(0);
            navRail_->setMaximumWidth(UiStyle::Fpx(300));
            navRail_->hide();
        }
        if (!drawerScrim_) {
            drawerScrim_ = new QWidget(this);
            drawerScrim_->setObjectName(QStringLiteral("DrawerScrim"));
            drawerScrim_->setAttribute(Qt::WA_StyledBackground, true);
            drawerScrim_->setStyleSheet(QStringLiteral("background-color: rgba(0, 0, 0, 110);"));
            drawerScrim_->hide();
            drawerScrim_->installEventFilter(this);
        }
        drawerOpen_ = false;
        UpdateDrawerGeometry();
    } else {
        setMinimumSize(980, 660);
        CloseDrawer();
        if (drawerScrim_) {
            drawerScrim_->deleteLater();
            drawerScrim_ = nullptr;
        }
        if (navRail_ && bodyLayout_) {
            navRail_->setMaximumWidth(QWIDGETSIZE_MAX);
            navRail_->setFixedWidth(UiStyle::Fpx(260));
            bodyLayout_->insertWidget(0, navRail_);
            navRail_->show();
        }
    }
}

void MainWindow::UpdateDrawerGeometry()
{
    if (!drawerMode_ || !navRail_ || !crumbBar_)
        return;
    // 顶部对齐菜单栏下沿：Python 里是 map 一遍 menuButton/label 的底边
    drawerTop_ = crumbBar_->geometry().bottom() + 1;
    drawerWidth_ = qMin(UiStyle::Fpx(300), qMax(UiStyle::Fpx(220), int(width() * 0.82)));
    const int h = qMax(0, height() - drawerTop_);
    if (drawerScrim_)
        drawerScrim_->setGeometry(0, drawerTop_, width(), h);
    if (drawerOpen_)
        navRail_->setGeometry(0, drawerTop_, drawerWidth_, h);
    else
        navRail_->resize(drawerWidth_, h);
}

void MainWindow::OpenDrawer()
{
    if (!drawerMode_ || !navRail_)
        return;
    UpdateDrawerGeometry();
    const int h = qMax(0, height() - drawerTop_);
    if (drawerScrim_) {
        drawerScrim_->show();
        drawerScrim_->raise();
    }
    navRail_->show();
    navRail_->resize(drawerWidth_, h);
    navRail_->move(-drawerWidth_, drawerTop_);
    navRail_->raise();

    if (!drawerAni_)
        drawerAni_ = new QPropertyAnimation(navRail_, "pos", this);
    drawerAni_->stop();
    drawerAni_->setDuration(Motion::Slow);
    drawerAni_->setEasingCurve(Motion::EnterCurve());
    drawerAni_->setStartValue(QPoint(-drawerWidth_, drawerTop_));
    drawerAni_->setEndValue(QPoint(0, drawerTop_));
    drawerAni_->start();
    // 遮罩跟抽屉一起淡入：原来遮罩是"瞬间全黑"，看起来像画面闪了一下
    if (drawerScrim_) {
        drawerScrim_->setGraphicsEffect(nullptr);
        Motion::FadeIn(drawerScrim_, Motion::Base);
    }
    drawerOpen_ = true;

    // 抽屉的视口高度与常驻侧栏不同，桌面侧栏留下的滚动位置会让顶部被裁掉半行。
    // Python 展开抽屉时不滚侧栏（永远从"用户"分组开始），这里同样归零。
    QTimer::singleShot(Motion::Slow + 40, this, [this] {
        if (drawerOpen_ && navScroll_)
            navScroll_->verticalScrollBar()->setValue(0);
    });
}

void MainWindow::CloseDrawer()
{
    if (!drawerMode_)
        return;
    // 遮罩先淡出（180ms），再隐藏；抽屉本体只滑不淡，避免中途看见底下的页面
    if (drawerScrim_) {
        QPointer<QWidget> scrim = drawerScrim_;
        Motion::FadeOut(drawerScrim_, Motion::Base, [this, scrim] {
            // 淡出期间又被打开：此时遮罩必须留着（否则抽屉开着却没有遮罩）
            if (scrim && !drawerOpen_)
                scrim->hide();
        });
    }
    drawerOpen_ = false;
    if (!navRail_ || navRail_->isHidden())
        return;

    if (!drawerAni_)
        drawerAni_ = new QPropertyAnimation(navRail_, "pos", this);
    drawerAni_->stop();
    drawerAni_->setDuration(Motion::Slow);
    drawerAni_->setEasingCurve(Motion::ExitCurve());
    drawerAni_->setStartValue(navRail_->pos());
    drawerAni_->setEndValue(QPoint(-drawerWidth_, drawerTop_));
    drawerAni_->start();
    QTimer::singleShot(Motion::Slow + 60, this, [this] {
        if (!drawerOpen_ && navRail_)
            navRail_->hide();   // 兜底：连点/切页打断动画时也能真正收起
    });
}

void MainWindow::ToggleDrawer()
{
    if (drawerOpen_)
        CloseDrawer();
    else
        OpenDrawer();
}

bool MainWindow::DrawerSelfTest()
{
    auto log = [](const char *fmt, bool a, bool b) {
        std::fprintf(stderr, fmt, int(a), int(b));
        std::fflush(stderr);
    };
    OpenDrawer();
    const bool opened = drawerOpen_;
    log("drawer selftest: open=%d (expect 1)\n", opened, opened);

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &esc);
    const bool closedByEsc = !drawerOpen_;
    log("drawer selftest: esc_closed=%d (expect 1)\n", closedByEsc, closedByEsc);

    OpenDrawer();
    if (drawerScrim_) {
        const QPoint p = drawerScrim_->rect().center();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), QPointF(drawerScrim_->mapToGlobal(p)),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(drawerScrim_, &press);
    }
    const bool closedByScrim = !drawerOpen_;
    log("drawer selftest: scrim_closed=%d (expect 1)\n", closedByScrim, closedByScrim);
    return opened && closedByEsc && closedByScrim;
}

void MainWindow::DemoOpenDrawer()
{
    if (!drawerMode_)
        SetDrawerMode(true);
    OpenDrawer();
}

void MainWindow::ScrollNavToEnd()
{
    if (!navScroll_)
        return;
    QScrollBar *sb = navScroll_->verticalScrollBar();
    sb->setValue(sb->maximum());
    // 回归：内容不足一屏时 maximum=0，此时"滚到底"本来就不该有画面变化。
    qWarning("nav-scroll: max=%d value=%d", sb->maximum(), sb->value());
}

void MainWindow::DemoCollapseNav()
{
    ToggleNav();
}

void MainWindow::ShotTo(const QString &path, int delayMs)
{
    QTimer::singleShot(delayMs, this, [this, path] {
        QElapsedTimer t;
        t.start();
        const QPixmap shot = grab();
        const qint64 grabMs = t.elapsed();
        shot.save(path);
        qWarning("bench: homeRows=%d painted=%d grab=%lldms cache(entries=%d bytes=%lldKB "
                     "hits=%d misses=%d dir=%s)\n", home_->ItemCount(), g_comicBench.painted, (long long)grabMs,
                     CoverCache::I().EntryCount(), (long long)(CoverCache::I().Bytes() / 1024),
                     CoverCache::I().Hits(), CoverCache::I().Misses(),
                     qPrintable(CoverCache::I().Dir()));
        // 截图模式直接结束进程：QQuickWidget 的渲染线程在"窗口被移到屏幕外"时
        // 会在析构/DLL 卸载阶段偶发卡死或堆损坏（Qt 6.12 + MinGW；真机与正常关窗
        // 不受影响）。--shot 是一次性验收进程，截图与日志都已落盘，
        // 用 TerminateProcess 跳过析构与 DLL 卸载最稳（_Exit 仍会走 DLL detach）。
        std::fprintf(stderr, "shot: saved, skip teardown\n");
        std::fflush(nullptr);
#ifdef Q_OS_WIN
        ::TerminateProcess(::GetCurrentProcess(), 0);
#endif
        std::_Exit(0);
    });
}
