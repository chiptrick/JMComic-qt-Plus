#include "theme.h"

#include "app_font.h"

#include <QApplication>
#include <QDir>
#include <QPainter>
#include <QPainterPath>
#include <QStandardPaths>
#include <QFile>
#include <cstdio>

#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QWidget>

namespace {

QString Rgba(const QColor &c, int alpha255)
{
    return QStringLiteral("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(alpha255);
}

// 两个主题色之间插值。分流页的分隔线、列头底色、选中行、输入框底色都是"派生色"：
// 写死灰值在浅色主题下不是变成一条黑线，就是干脆看不见，只能从 bg/text/accent 推。
QColor Blend(const QColor &a, const QColor &b, qreal t)
{
    const qreal k = qBound(0.0, t, 1.0);
    return QColor(qRound(a.red() + (b.red() - a.red()) * k),
                  qRound(a.green() + (b.green() - a.green()) * k),
                  qRound(a.blue() + (b.blue() - a.blue()) * k));
}

// 字体完全交给 AppFont（app_font.h）：
//   跟随系统时 = SystemParametersInfo 读到的 lfMessageFont，并在系统字体变更时自动重设；
//   自定义时   = 用户选的家族 + 字重/字宽/字号（可变字体走 wght/wdth 轴）。
// 主题这一层只负责"按当前字号缩放 qss 里的 px"，不再自己挑字体。

}  // namespace


namespace {

// QSS 里的 image: url() 走的是 QImageReader，而本机 Qt 没加载 svg 插件
// （supportedImageFormats 里没有 svg），所以下拉箭头改成"运行时画一张 PNG"，
// 颜色跟随主题，避免出现"下拉框没有箭头"这种一眼假。
QString EnsureChevron(const QColor &color, bool dark, int dir = 0)
{
    // dir: 0=下（下拉框） 1=左 / 2=右（标签栏滚动按钮） 3=上（SpinBox）
    QPixmap pm(28, 28);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(color, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    const QPointF c(14, 14);
    if (dir == 1)
        p.translate(c), p.rotate(90), p.translate(-c);
    else if (dir == 2)
        p.translate(c), p.rotate(-90), p.translate(-c);
    else if (dir == 3)
        p.translate(c), p.rotate(180), p.translate(-c);
    QPainterPath path;
    path.moveTo(8, 11);
    path.lineTo(14, 17);
    path.lineTo(20, 11);
    p.drawPath(path);
    p.end();
    pm.setDevicePixelRatio(2.0);

    QString dirPath = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dirPath.isEmpty() || !QDir().mkpath(dirPath))
        dirPath = QCoreApplication::applicationDirPath();
    const char *suffix = dir == 1 ? "_left" : dir == 2 ? "_right" : dir == 3 ? "_up" : "";
    const QString file = QDir(dirPath).filePath(
        (dark ? QStringLiteral("chevron_dark") : QStringLiteral("chevron_light"))
        + QString::fromLatin1(suffix) + QStringLiteral(".png"));
    pm.save(file, "PNG");
    return QDir::fromNativeSeparators(file);
}

}  // namespace

namespace {

// 基础 qss 里写死了 `font-family: Roboto;`（QtMaterial 的遗产）。Windows 没有这个字体，
// Qt 会静默换成 Tahoma —— 只要这行还在，应用字体怎么设都没用。直接删掉。
QString StripFontFamily(const QString &css)
{
    static const QRegularExpression re(QStringLiteral(R"(font-family\s*:\s*Roboto\s*;)"));
    QString out = css;
    out.remove(re);
    return out;
}

// 字号缩放：把 qss 里所有 `font-size: Npx` 按比例放大/缩小。
// 只动 px，不动 `font-size: ;`（基础 qss 里那几行是空的）。
QString ScaleFontPx(const QString &css, qreal scale)
{
    if (qFuzzyCompare(scale, 1.0))
        return css;
    static const QRegularExpression re(QStringLiteral(R"(font-size\s*:\s*(\d+)px)"));
    QString out;
    out.reserve(css.size() + 256);
    int last = 0;
    auto it = re.globalMatch(css);
    while (it.hasNext()) {
        const auto m = it.next();
        out += css.mid(last, m.capturedStart() - last);
        const int px = qMax(8, qRound(m.captured(1).toInt() * scale));
        out += QStringLiteral("font-size: %1px").arg(px);
        last = m.capturedEnd();
    }
    out += css.mid(last);
    return out;
}


// 字重缩放：qss 里有一批 `font-weight: 500/600` 的规则（标题、主按钮、导航名）。
// 用户把"字重"调到 700 时，这些规则仍然把字重钉在 500 —— 视觉上就是
// "字重设置不生效"。这里按 (用户字重 / 400) 等比放大这些字面值，并夹回 100..900。
QString ScaleFontWeight(const QString &css, qreal ratio)
{
    if (qFuzzyCompare(ratio, 1.0))
        return css;
    static const QRegularExpression re(QStringLiteral(R"(font-weight\s*:\s*(\d{3}))"));
    QString out;
    out.reserve(css.size() + 128);
    int last = 0;
    auto it = re.globalMatch(css);
    while (it.hasNext()) {
        const auto m = it.next();
        out += css.mid(last, m.capturedStart() - last);
        const int w = qBound(100, qRound(m.captured(1).toInt() * ratio / 25.0) * 25, 900);
        out += QStringLiteral("font-weight: %1").arg(w);
        last = m.capturedEnd();
    }
    out += css.mid(last);
    return out;
}


// C++ 壳没有编译那套资源（303 个 svg、按主题染色），QSS 一旦用到下拉箭头 /
// SpinBox 箭头 / 标签栏滚动箭头，Qt 就会在日志里刷 "qt.svg: Cannot open file"，
// 界面上则直接没有箭头。这里把这几类路径改写成运行时生成的 PNG（颜色跟随主题），
// 其余未被界面用到的图标保持原样（Qt 只在规则真正命中时才加载，不会产生警告）。
QString RewriteLegacyIcons(const QString &css, const QColor &primary, const QColor &disabled,
                           bool dark)
{
    struct Entry { const char *name; int dir; };
    static const Entry entries[] = {
        {"downarrow", 0}, {"downarrow2", 0}, {"uparrow", 3}, {"uparrow2", 3},
        {"leftarrow2", 1}, {"rightarrow2", 2},
    };
    QString out = css;
    for (const Entry &e : entries) {
        const QString file = QString::fromLatin1(e.name) + QStringLiteral(".svg");
        for (int state = 0; state < 2; ++state) {
            const QString stateDir = state == 0 ? QStringLiteral("primary")
                                                : QStringLiteral("disabled");
            const QString path = EnsureChevron(state == 0 ? primary : disabled, dark, e.dir);
            // 六个主题目录名不同，用正则统一匹配
            static const QString prefix = QStringLiteral(":/icon/theme/[a-z_]+/");
            QRegularExpression re(prefix + stateDir + QLatin1Char('/') + file);
            out.replace(re, QDir::fromNativeSeparators(path));
        }
    }
    return out;
}

}  // namespace

UiTheme &UiTheme::I()
{
    static UiTheme inst;
    return inst;
}

void UiTheme::Apply(const QString &name)
{
    name_ = name;

    // 字体变了：缩放比或字重变了才需要重建 qss；单纯换家族时
    // qApp->setFont 已经把 ApplicationFontChange 发下去了，重建纯属浪费。
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        connect(&AppFont::I(), &AppFont::Changed, this, [this] {
            const qreal weightRatio =
                AppFont::I().UsingCustom() ? AppFont::I().Cfg().weight / 400.0 : 1.0;
            if (!qFuzzyCompare(UiStyle::FontScale(), lastScale_)
                || !qFuzzyCompare(weightRatio, lastWeightRatio_))
                Apply(name_);
        });
    }
    const bool dark = name.startsWith(QLatin1String("dark"));
    t_.dark = dark;

    if (dark) {
        t_.bg = QColor(0x31, 0x36, 0x3b);
        t_.bgAlt = QColor(0x2a, 0x2e, 0x32);
        t_.surface = QColor(0x3a, 0x40, 0x45);
        t_.surfaceHover = QColor(0x44, 0x4b, 0x51);
        t_.border = QColor(0x23, 0x26, 0x29);
        t_.text = QColor(0xff, 0xff, 0xff);
        t_.textDim = QColor(0xb6, 0xbe, 0xc2);
        t_.textFaint = QColor(0x7d, 0x86, 0x8b);
        t_.shadow = QColor(0, 0, 0, 90);
    } else {
        t_.bg = QColor(0xf2, 0xf3, 0xf5);
        t_.bgAlt = QColor(0xe8, 0xea, 0xed);
        t_.surface = QColor(0xff, 0xff, 0xff);
        t_.surfaceHover = QColor(0xf6, 0xf8, 0xf9);
        t_.border = QColor(0xda, 0xdc, 0xe0);
        t_.text = QColor(0x20, 0x21, 0x24);
        t_.textDim = QColor(0x5f, 0x63, 0x68);
        t_.textFaint = QColor(0x9a, 0xa0, 0xa6);
        t_.shadow = QColor(0, 0, 0, 40);
    }

    // 强调色**逐个取自 Python 版 res/theme/*.qss**（那边一个主题文件里同一个值出现 24 次，
    // 就是按钮/进度/选中行在用的主色）：
    //   dark_orange / light_orange → #FF7B00  ← Python 的默认色（ThemeColorIndex = 0）
    //   dark_pink   / light_pink   → #FF4081  ← Python 的第二档
    //   dark_teal   / light_teal   → #1DE9B6（浅色 #00897B）
    // 以前这里写的 #ffab40 / #ea4c89 与 qss 里的值并不一致，导致「同一套主题两套黄」。
    if (name.contains(QLatin1String("pink")))
        t_.accent = QColor(0xff, 0x40, 0x81);
    else if (name.contains(QLatin1String("orange")))
        t_.accent = QColor(0xff, 0x7b, 0x00);
    else
        t_.accent = dark ? QColor(0x1d, 0xe9, 0xb6) : QColor(0x00, 0x89, 0x7b);

    t_.accentSoft = Rgba(t_.accent, dark ? 34 : 30);
    // Python 的 .danger 是 #dc3545，一并统一（危险按钮只有确认框在用）
    t_.danger = QColor(0xdc, 0x35, 0x45);
    t_.warning = QColor(0xff, 0xc1, 0x07);

    // 1) 基础主题：直接吃 Python 版那份 QSS（同名同文件）
    QString base;
    QFile f(QStringLiteral(":/theme/%1.qss").arg(name));
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        base = StripFontFamily(QString::fromUtf8(f.readAll()));

    // 2) 组件覆盖：Python 侧这些是逐个 setStyleSheet 写的，无法逐条搬运，
    //    这里按同一套 token 重写一份，保证观感一致而不是"另一套设计语言"。
    QString css = base + QLatin1Char('\n') + BuildOverlay();

    // 替换顺序必须"长 token 优先"：@text 是 @textDim/@textFaint 的前缀，
    // @accent 是 @accentSoft 的前缀；顺序反了会拼出 "#ffffffDim" 这种非法颜色，
    // Qt 会静默丢弃整条声明（表现为胶囊文字变回主题默认的青色）。
    const QVector<QPair<QString, QString>> tokens = {
        // 派生色（分流页的版式用）：@dividerStrong 是 @divider 的前缀，必须先换长的
        {QStringLiteral("@dividerStrong"), Blend(t_.bg, t_.text, 0.30).name()},
        {QStringLiteral("@divider"), Blend(t_.bg, t_.text, 0.15).name()},
        {QStringLiteral("@rowBand"), Blend(t_.bg, t_.text, 0.07).name()},
        {QStringLiteral("@rowHover"), Blend(t_.bg, t_.text, 0.05).name()},
        {QStringLiteral("@rowSelect"), Blend(t_.bg, t_.accent, 0.32).name()},
        {QStringLiteral("@inset"),
         Blend(t_.bg, dark ? QColor(0, 0, 0) : QColor(255, 255, 255), 0.35).name()},
        {QStringLiteral("@surfaceHover"), t_.surfaceHover.name()},
        {QStringLiteral("@accentSoft"), Rgba(t_.accent, dark ? 34 : 30)},
        {QStringLiteral("@textFaint"), t_.textFaint.name()},
        {QStringLiteral("@textDim"), t_.textDim.name()},
        {QStringLiteral("@surface"), t_.surface.name()},
        {QStringLiteral("@bgAlt"), t_.bgAlt.name()},
        {QStringLiteral("@border"), t_.border.name()},
        {QStringLiteral("@danger"), t_.danger.name()},
        {QStringLiteral("@text"), t_.text.name()},
        {QStringLiteral("@accent"), t_.accent.name()},
        {QStringLiteral("@bg"), t_.bg.name()},
        // 文字链行高：与 SectionHeader 的标题字高同步（14px 字 ≈ 18px 行高 + 2px）
        {QStringLiteral("@linkH"),
         QStringLiteral("%1px").arg(qRound(20 * UiStyle::FontScale()))},
    };
    for (const auto &kv : tokens)
        css.replace(kv.first, kv.second);
    css.replace(QLatin1String("@chevron"), EnsureChevron(t_.textDim, dark));
    css.replace(QLatin1String("@chevronLeft"), EnsureChevron(t_.textDim, dark, 1));
    css.replace(QLatin1String("@chevronRight"), EnsureChevron(t_.textDim, dark, 2));
    // 圆角：全站唯一来源是 UiStyle::Radius（theme.h）。直角风格下注入 0px，
    // 所有 #Primary / #Chip / #SearchBox / #DialogCard / #Toast 等一次生效。
    // 注意顺序：@radius 是 @radiusSm / @radiusOption 的前缀，必须先换长的那个，
    // 否则会拼出 "0pxSm" / "0pxOption"。
    css = RewriteLegacyIcons(css, t_.textDim, t_.textFaint, dark);
    css.replace(QLatin1String("@radiusSm"),
                QStringLiteral("%1px").arg(UiStyle::RadiusSm));
    css.replace(QLatin1String("@radiusOption"),
                QStringLiteral("%1px").arg(UiStyle::RadiusOption));
    css.replace(QLatin1String("@radius"),
                QStringLiteral("%1px").arg(UiStyle::Radius));


    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance())) {
        const qreal weightRatio =
            AppFont::I().UsingCustom() ? AppFont::I().Cfg().weight / 400.0 : 1.0;
        app->setStyleSheet(ScaleFontWeight(ScaleFontPx(css, UiStyle::FontScale()), weightRatio));
        lastScale_ = UiStyle::FontScale();
        lastWeightRatio_ = weightRatio;
    }
    emit Changed();
}

void UiTheme::ToggleDark()
{
    // 只翻明暗，保留强调色家族。原来硬写 teal，在橙色主题下点一次日月图标
    // 就会莫名其妙跳回青色（默认色改成橙色后这个 bug 会天天被看到）。
    const QString family = name_.section(QLatin1Char('_'), 1);
    Apply((t_.dark ? QStringLiteral("light_") : QStringLiteral("dark_"))
          + (family.isEmpty() ? QStringLiteral("orange") : family));
}

QString UiTheme::BuildOverlay() const
{
    return QStringLiteral(R"QSS(
/* 基础主题里有 QWidget{background-color:#31363b}，会让每个裸 QWidget/QLabel
   都在父容器上盖一块底色（表现为文字背后一条深色带）。这里统一置透明，
   再由 #Root / #ListRow / #TitleBar / #NavRail 这些 id 规则各自上色。 */
QWidget { background-color: transparent; }
QLabel { background-color: transparent; border: none; }
QLabel#SettingDesc, QLabel#PageSub, QLabel#PageTitle, QLabel#SectionTitle,
QLabel#SectionHint, QLabel#RowTitle, QLabel#RowMeta, QLabel#SettingTitle,
QLabel#EmptyTitle, QLabel#EmptyText, QLabel#Brand, QLabel#BrandSub {
    background-color: transparent;
}
QWidget#Root { background: @bg; }

QWidget#TitleBar { background: @bgAlt; border-bottom: 1px solid @border; }
QLabel#Brand { color: @text; font-size: 13px; font-weight: 500; }
QLabel#BrandSub { color: @textFaint; font-size: 11px; }
QToolButton#WinBtn, QPushButton#WinBtn {
    background-color: transparent; border: none; border-radius: @radius; padding: 0px;
}
QToolButton#WinBtn:hover, QPushButton#WinBtn:hover { background-color: @surfaceHover; }
QToolButton#WinBtnClose:hover { background: @danger; }

QWidget#NavRail { background: @bgAlt; border-right: 1px solid @border; }
QToolButton#NavItem {
    background: transparent; border: none; border-radius: @radius;
    color: @textDim; font-size: 11px; padding: 8px 2px 6px 2px;
}
QToolButton#NavItem:hover { background: @accentSoft; color: @text; }
QToolButton#NavItem:checked { background: @accentSoft; color: @accent; }
QLabel#NavBadge {
    background: @accent; color: @bg; border-radius: @radius; font-size: 10px; padding: 0px 4px;
}

QWidget#PageRoot { background: @bg; }
QLabel#PageTitle { color: @text; font-size: 20px; font-weight: 500; }
QLabel#PageSub { color: @textFaint; font-size: 12px; }
QLabel#SectionTitle { color: @text; font-size: 14px; font-weight: 500; }
QLabel#SectionHint { color: @textFaint; font-size: 12px; }
QFrame#Divider { background: @border; max-height: 1px; border: none; }

QPushButton#Primary {
    background-color: @accent; color: @bg; border: none; border-radius: @radius;
    padding: 8px 18px; font-size: 13px; font-weight: 500;
}
QPushButton#Primary:hover { background: @accent; }
QPushButton#Primary:pressed { background: @accent; }
/* 搜索页「搜索」按钮：高度必须与输入框完全一致。两层实测约束（search-bar-debug
   取证，2026-11-12）：
   1) 基座 qss 给 QPushButton 钉的 height/min/max-height:34px 作用在内容盒上，
      加上 #Primary 的上下 padding 8px 后按钮实际 = 34+16 = 50px，且 QSS polish
      会重写控件的 min/max——C++ setFixedHeight(Fpx(42)/40) 根本压不过它，
      这就是"按钮比输入框高一大截"的根源；
   2) 输入框（QLineEdit#SearchBox）的自然高度恒为 32px：基座规则 height:18px
      把内容盒钉在 18 + 覆盖层 padding 6×2 + 边框 2（与字号缩放无关，
      FontScale 1.0/1.33 实测一致）。
   因此这里把按钮内容盒钉成 16px：16 + 上下 padding 16 = 32px，与输入框严格
   等高。若日后调整 QLineEdit#SearchBox 的 padding 或基座 height:18px，
   必须同步改这里（和 SearchPage 里 barH 的注释）。 */
QPushButton#Primary[searchGo="true"] {
    height: 16px; min-height: 16px; max-height: 16px;
}
QPushButton#Ghost {
    background: transparent; color: @text; border: 1px solid @border;
    border-radius: @radius; padding: 7px 14px; font-size: 12px;
}
QPushButton#Ghost:hover { background: @surfaceHover; border-color: @textFaint; }
/* 章节瓷砖：DetailPage 的章节目录网格（ChapterGrid 给每个按钮打 chapTile）。
   旧实现是 C++ 里 setFixedSize(200, 34)，363dp 竖屏一行只放得下 1 个。
   宽度现在由 C++ 按可用宽度反算（一行 ≥4 个），这里只负责高度与内边距：
     内容盒 42 + 上下 padding 0 + 边框 1×2 = 44dp，正好等于触摸目标下限，
     PolishHitTargets 不会再去改它（按钮另有 hitGrow=false 双保险）。
   左右 padding 收到 6px：窄格（~78dp）里也要放得下"第 1234 话"。
   ⚠ 改这里的 padding 必须同步 ui_pages_browse.cpp 的 kChapPadH，
     否则 C++ 算出的省略宽度与实际文字区不一致，末尾会多省/少省几个字。 */
QPushButton#Ghost[chapTile="true"] {
    padding: 0px 6px;
    height: 42px; min-height: 42px; max-height: 42px;
}
/* 已读章节：原来是 C++ 里写死的 #7d868b（只对深色主题成立），改走主题 token，
   浅色主题下也不会退化成一条看不清的灰字。 */
QPushButton#Ghost[chapTile="true"][chapRead="true"] { color: @textFaint; }
/* 基座 qss 给所有 QPushButton 钉了 height/min-height/max-height:34px。
   文字链不是按钮块，34px 会让"最近搜索 / 清空"这一行凭空多出 16px 空白
   （setFixedHeight 顶不过 QSS 的 min/max）。这里按字号解锁成"字高 + 2px"。 */
QPushButton#Link {
    background: transparent; border: none; color: @accent; font-size: 12px;
    height: @linkH; min-height: @linkH; max-height: @linkH; padding: 0px 4px;
}
QPushButton#Link:hover { color: @text; }
/* 分流页的紧凑 chip（筛选条 / 排序 / 高级入口，Python CompactButton 同款度量）。
   基座把按钮内容盒钉在 34px 且 #Ghost 还有 7px 上下 padding，实高 50px ——
   3 个筛选加排序根本排不进一行，只能退回 2×2。这里按 Python 的 24/22 重钉：
   内容盒 + 上下 padding 0 + 1px 边框 = 26/24px。属性选择器特异性高于纯 id 规则，
   才压得住 #Primary / #Ghost 自己的 padding。active 态切成 #Primary 时尺寸不变。 */
QPushButton#Ghost[chip="filter"], QPushButton#Primary[chip="filter"] {
    height: 24px; min-height: 24px; max-height: 24px;
    padding: 0px 8px; font-size: 12px;
}
/* 选中态切成 #Primary（accent 底 + border:none），外高会比 Ghost 少 2px 边框。
   补 1px 上下 padding 把两者对齐成同一个 26px，否则同一行里"全部"会比别的矮一截。 */
QPushButton#Primary[chip="filter"] {
    padding: 1px 8px;
}
QPushButton#Ghost[chip="sort"], QPushButton#Primary[chip="sort"] {
    height: 22px; min-height: 22px; max-height: 22px;
    padding: 0px 6px; font-size: 12px;
}
QPushButton#Primary[chip="sort"] {
    padding: 1px 6px;
}
/* 高级入口按 Python proxyFoot 的紧凑按钮度量（30px，与底部操作行同排居中） */
QPushButton#Ghost[chip="adv"], QPushButton#Primary[chip="adv"] {
    height: 28px; min-height: 28px; max-height: 28px;
    padding: 0px 12px; font-size: 12px;
}
/* ================= 分流页（docs/proxy_advanced_dropdown_demo.html） =================
   整页按 demo 的移动端稿重排：分区小标题 12px、说明/状态 11px、表头 10px、
   行内 11px 单选、26px 下划线输入框、22/24/30px 三档紧凑按钮、58dp 两行节点行。
   全部限定在 QWidget#ProxyPage 子树内 —— 设置页/登录页也用 QRadioButton，
   不能因为这一页把别人的指示器也改成 14px。
   圆角沿用全站 UiStyle::Radius（0px 直角）：demo 的 6~8px 圆角不搬过来。 */
QLabel#ProxyHeadTitle { color: @textDim; font-size: 13px; }
QLabel#ProxySection { color: @text; font-size: 12px; font-weight: 500; }
QLabel#ProxyNote { color: @textFaint; font-size: 11px; }
QLabel#ProxyTableHead { color: @textFaint; font-size: 10px; }
QLabel#ProxyFootNote { color: @textFaint; font-size: 10px; }
QLabel#ProxyFieldLabel { color: @textDim; font-size: 11px; }
QLabel#ProxyEmpty { color: @textFaint; font-size: 11px; }
QLabel#ProxyFootStatus { color: @textDim; font-size: 12px; }
QLabel#ProxyCaret { color: @accent; font-size: 9px; }
/* IP 用等宽字体：一列 IP 只有字宽相同才对得齐（demo 的 ui-monospace） */
QLabel#ProxyNodeIp {
    color: @text; font-size: 12px;
    font-family: "Consolas", "Menlo", "DejaVu Sans Mono", monospace;
}
QLabel#ProxyNodeGeo { color: @textFaint; font-size: 11px; }
QLabel#ProxyNodeSpd { color: @textDim; font-size: 11px; }
QLabel#ProxyTag { color: @accent; background: @accentSoft; font-size: 10px; }

/* 单选/复选：14px 圆形 + 15px 方形。基座把指示器定成 24px、行高 30px，
   4 个代理模式会被挤成两行 —— 这里按 demo 的度量重钉（高度有版式意义）。 */
QWidget#ProxyPage QRadioButton {
    spacing: 5px; color: @text; font-size: 11px;
    height: 19px; min-height: 19px; max-height: 19px; background: transparent;
}
QWidget#ProxyPage QRadioButton::indicator {
    width: 14px; height: 14px; border-radius: 7px;
    border: 1px solid @dividerStrong; background: transparent;
}
QWidget#ProxyPage QRadioButton::indicator:checked {
    background: @accent; border: 1px solid @accent;
}
QWidget#ProxyPage QCheckBox {
    spacing: 5px; color: @text; font-size: 11px;
    height: 22px; min-height: 22px; max-height: 22px; background: transparent;
}
QWidget#ProxyPage QCheckBox::indicator {
    width: 15px; height: 15px; border: 1px solid @accent; background: transparent;
}
QWidget#ProxyPage QCheckBox::indicator:checked {
    background: @accent; border: 1px solid @accent;
}

/* 下划线输入框（demo .fld）：内容盒 21px + 1/2px 边框 ≈ 26px 高 */
QLineEdit#ProxyField {
    background: @inset; color: @text;
    border: 1px solid @divider; border-bottom: 2px solid @divider;
    padding: 0px 6px; font-size: 11px;
    height: 21px; min-height: 21px; max-height: 21px;
}
QLineEdit#ProxyField[accent="true"] { border-bottom-color: @accent; }
QLineEdit#ProxyField:focus { border-color: @accent; }
QLineEdit#RegionField {
    background: transparent; color: @accent; border: none;
    padding: 0px; font-size: 11px;
}
QWidget#ProxyFieldBox {
    background: @inset;
    border: 1px solid @divider; border-bottom: 2px solid @accent;
}
/* 页头的"说明"文字链：手机没有 hover，虚线底边是唯一的可点暗示 */
QPushButton#ProxyDocLink {
    background: transparent; border: none; border-bottom: 1px dashed @textFaint;
    color: @textDim; font-size: 11px; padding: 0px 1px;
    height: 15px; min-height: 15px; max-height: 15px;
}
QPushButton#ProxyDocLink:hover { color: @text; border-bottom-color: @text; }

/* 结构块：列头 / 底栏 / 节点行 / 分割线。颜色全部是主题派生色，
   浅色主题下自动变成"深一档的灰"，不会出现白色分隔线。 */
QFrame#ProxyRule { background: @divider; border: none; }
QWidget#ProxyColHead {
    background: @rowBand;
    border-top: 1px solid @divider; border-bottom: 1px solid @divider;
}
QWidget#ProxyFoot { background: @bgAlt; border-top: 1px solid @divider; }
QWidget#ProxyNodeRow { background: transparent; border-bottom: 1px solid @divider; }
QWidget#ProxyNodeRow:hover { background: @rowHover; }
QWidget#ProxyNodeRow[sel="true"] { background: @rowSelect; }

/* 分流页的三档紧凑按钮：工具条 22px / 字段旁 24px / 底栏 30px。
   基座把按钮内容盒钉在 34px 且 #Ghost 另有 7px padding，不重钉就排不进一行。
   text-transform:none —— 基座对 QPushButton 统一转大写，会把 ProxyIP 写成
   "PROXYIP"（demo 与 Python 侧都是原样大小写）。 */
QPushButton#Ghost[chip="tool"], QPushButton#Primary[chip="tool"] {
    height: 22px; min-height: 22px; max-height: 22px;
    padding: 0px 7px; font-size: 10px; text-transform: none;
}
QPushButton#Primary[chip="tool"] {
    padding: 1px 7px;
}
QPushButton#Ghost[chip="field"], QPushButton#Primary[chip="field"] {
    height: 24px; min-height: 24px; max-height: 24px;
    padding: 0px 8px; font-size: 11px; text-transform: none;
}
QPushButton#Primary[chip="field"] {
    padding: 1px 8px;
}
QPushButton#Ghost[chip="foot"], QPushButton#Primary[chip="foot"] {
    height: 30px; min-height: 30px; max-height: 30px;
    padding: 0px 14px; font-size: 12px; text-transform: none;
}
QPushButton#Primary[chip="foot"] {
    padding: 1px 14px;
}
/* 高级抽屉的描边容器（Python proxyAdvFrame）：展开时 1px 主题色，
   收起时退回普通面板色，不留亮线。 */
QWidget#AdvFrame {
    background-color: @surface;
    border: 1px solid @border;
}
QWidget#AdvFrame[open="true"] {
    background-color: @surface;
    border: 1px solid @accent;
}
QPushButton#Chip {
    background-color: @surface; color: @textDim; border: 1px solid @border;
    border-radius: @radius; padding: 4px 14px; font-size: 12px;
}
QPushButton#Chip:hover { color: @text; border-color: @textFaint; }
QPushButton#Chip:checked { background: @accentSoft; color: @accent; border-color: @accent; }

/* 近似直角按钮：登录 / 打卡 / 确定 / 取消 / 开始阅读 / 收藏（UiStyle::MarkSoftButton）。
   这里**只覆盖 border-radius**，padding / 配色 / :hover / :pressed / :disabled 全部沿用
   上面的 #Primary / #Ghost / #DangerBtn 规则，所以尺寸和交互状态不会有任何变化。
   属性选择器的特异性高于纯 id 选择器，才压得住前面那行 border-radius: @radius。 */
QPushButton#Primary[btnShape="soft"],
QPushButton#Ghost[btnShape="soft"],
QPushButton#DangerBtn[btnShape="soft"] {
    border-radius: @radiusSm;
}

QLineEdit#SearchBox, QLineEdit#Input, QComboBox#Filter, QSpinBox#Filter {
    background: @surface; color: @text; border: 1px solid @border;
    border-radius: @radius; padding: 6px 10px; font-size: 13px;
    selection-background-color: @accent; selection-color: @bg;
}
QLineEdit#SearchBox:focus, QLineEdit#Input:focus { border-color: @accent; }
QComboBox#Filter::drop-down { border: none; width: 20px; }
QComboBox#Filter::down-arrow { image: url(@chevron); width: 10px; height: 10px; }
QComboBox#Filter QAbstractItemView {
    background: @surface; color: @text; border: 1px solid @border;
    selection-background-color: @accentSoft; outline: none;
}

QWidget#Card { background: transparent; }
QWidget#ListRow {
    background-color: @surface; border: 1px solid @border; border-radius: @radius;
}
QWidget#ListRow:hover { background: @surfaceHover; }
QLabel#RowTitle { color: @text; font-size: 13px; font-weight: 500; }
QLabel#RowMeta { color: @textFaint; font-size: 11px; }

QWidget#SettingRow { background: transparent; }
QLabel#SettingTitle { color: @text; font-size: 13px; }
QLabel#SettingDesc { color: @textDim; font-size: 12px; }
QComboBox#SettingCombo {
    background: @surface; color: @text; border: 1px solid @border;
    border-radius: @radius; padding: 4px 10px; min-width: 108px; font-size: 12px;
}
QComboBox#SettingCombo::drop-down { border: none; width: 22px; }
QComboBox#SettingCombo::down-arrow { image: url(@chevron); width: 10px; height: 10px; }

/* 带边框的选项框：#Filter（首页分区 / 类别页 / 每周必看的筛选下拉）与
   #SettingCombo（设置、下载页的下拉）。统一 3px 近似直角 —— 视觉上仍接近
   直角，只把四个角轻微软化，和 #Primary/#Ghost[btnShape="soft"] 的按钮一致。

   这里**只覆盖 border-radius**：边框颜色/粗细、padding、min-width、字号、
   背景与文字配色全部沿用上面的原有规则。默认 / :focus / :on（列表弹开）/
   :disabled 逐态列出，保证四种状态取到同一个圆角，也不会因为某天给某个状态
   补了 border-radius 而让圆角在切换时忽然消失。同组选择器里的 QLineEdit /
   QSpinBox 以及无边框下拉（frame=false，透明边框）都不在此列，保持原样。 */
QComboBox#Filter,
QComboBox#Filter:focus,
QComboBox#Filter:on,
QComboBox#Filter:disabled,
QComboBox#SettingCombo,
QComboBox#SettingCombo:focus,
QComboBox#SettingCombo:on,
QComboBox#SettingCombo:disabled {
    border-radius: @radiusOption;
}

QProgressBar#RowProgress {
    background: @border; border: none; border-radius: @radius; max-height: 6px; text-align: center;
}
QProgressBar#RowProgress::chunk { background: @accent; border-radius: @radius; }

/* 滑块（设置页的字重/字宽/字号）。直角风格：轨道与手柄都是矩形，无圆角。 */
QSlider { min-height: 26px; background: transparent; }
QSlider::groove:horizontal { height: 4px; background: @border; }
QSlider::sub-page:horizontal { background: @accent; }
QSlider::add-page:horizontal { background: @border; }
QSlider::handle:horizontal {
    background: @text; width: 12px; margin: -6px 0; border: none;
}
QSlider::handle:horizontal:hover { background: @accent; }
QSlider::handle:horizontal:pressed { background: @accent; }
/* 禁用态：跟随系统模式下这几个控件不可改，必须一眼看出来 */
QSlider::handle:horizontal:disabled { background: @textFaint; }
QSlider::sub-page:horizontal:disabled { background: @border; }
QComboBox#SettingCombo:disabled { color: @textFaint; border-color: @border; }
QLabel#SettingTitle:disabled, QLabel#SettingDesc:disabled { color: @textFaint; }

QScrollArea { background: transparent; border: none; }
QScrollArea > QWidget > QWidget { background: transparent; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @border; border-radius: @radius; min-height: 30px; }
QScrollBar::handle:vertical:hover { background: @textFaint; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0px; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QLabel#Toast {
    background: @bgAlt; color: @text; border: 1px solid @border;
    border-radius: @radius; padding: 8px 18px; font-size: 12px;
}
QLabel#Tag {
    background-color: @accentSoft; color: @accent; border-radius: @radius;
    padding: 1px 6px; font-size: 10px;
}
QLabel#TagDim {
    background-color: @accentSoft; color: @textDim; border-radius: @radius;
    padding: 1px 7px; font-size: 10px;
}
QWidget#DialogCard {
    background-color: @surface; border: 1px solid @border; border-radius: @radius;
}
QLabel#DialogTitle { color: @text; font-size: 15px; font-weight: 500; }
QLabel#DialogText { color: @textDim; font-size: 13px; }
QLineEdit#DialogInput {
    background-color: @bgAlt; border: 1px solid @border;
    border-radius: @radius; color: @text;
    padding: 4px 8px; font-size: 13px;
}
QPushButton#DangerBtn {
    background-color: @danger; color: #ffffff; border: none;
    border-radius: @radius; padding: 8px 18px; font-size: 13px; font-weight: 500;
}
QPushButton#DangerBtn:hover { background-color: #b02a37; }
QLabel#EmptyTitle { color: @text; font-size: 14px; }
QLabel#EmptyText { color: @textFaint; font-size: 12px; }

/* ================================================================ 侧栏
   Python 的 navigationWidget：260px、底色与窗口一致、btn 之间 6px、
   选中项是 #4f5b62 底 + 5px 主色左边框（res/theme/*.qss 的 QToolButton:checked）。 */
QWidget#NavRail { background: @bg; border-right: 1px solid @border; }
QWidget#NavTop { background: transparent; border-bottom: 1px solid @border; }
QWidget#NavTail { background: transparent; border-top: 1px solid @border; }
QScrollArea#NavScroll, QWidget#NavContent { background: transparent; border: none; }
/* 侧栏在 840px 高的窗口里本来就要滚动（15 项 × 46px + 顶区 ≈ 1000px），
   基座的滚动条太宽会压掉文字，这里单独收窄到 6px。 */
QScrollArea#NavScroll QScrollBar:vertical {
    background: transparent; width: 8px; margin: 0px; padding: 0px;
}
/* 手柄不能再用 @border：它比侧栏底色还深（#232629 on #31363b），等于看不见，
   用户会以为"导航"只有一项。 */
QScrollArea#NavScroll QScrollBar::handle:vertical {
    background: @textFaint; min-height: 32px; border-radius: @radius;
}
QScrollArea#NavScroll QScrollBar::handle:vertical:hover { background: @textFaint; }
QScrollArea#NavScroll QScrollBar::add-line:vertical,
QScrollArea#NavScroll QScrollBar::sub-line:vertical { height: 0px; }
QScrollArea#NavScroll QScrollBar::add-page:vertical,
QScrollArea#NavScroll QScrollBar::sub-page:vertical { background: transparent; }

QLabel#NavGroup {
    color: @textFaint; font-size: 11px; padding: 6px 0 2px 13px;
}
QToolButton#NavItem {
    background-color: transparent; border: none;
    border-left: 4px solid transparent;
    border-radius: @radius;
    color: @text; font-size: 13px;
    padding: 0px 8px 0px 9px;
    text-align: left;
}
QToolButton#NavItem:hover { background-color: @surfaceHover; }
QToolButton#NavItem:checked {
    background-color: @surfaceHover;
    border-left: 4px solid @accent;
    color: @accent;
}
QFrame#NavLine { background: @border; border: none; }

QLabel#NavAvatar {
    background-color: @accentSoft; color: @accent;
    border: 1px solid @border;
    border-radius: @radius;
    font-size: 34px; font-weight: 600;
}
QLabel#NavName { color: @text; font-size: 13px; font-weight: 600; }
QLabel#NavTitle { color: @textFaint; font-size: 11px; }
QLabel#NavKey { color: @textFaint; font-size: 11px; }
QLabel#NavValue { color: @textDim; font-size: 11px; }
QLabel#NavValueAccent { color: @accent; font-size: 11px; }

/* 侧栏里"可交互"的三行（分流×2 / 屏蔽词）：只读行是纯文字，这三行必须有
   可见边界，否则用户不知道能点。整行是命中区（Label 只是行内文字）。 */
QWidget#NavAction {
    background: transparent;
    border: 1px solid @border;
    border-radius: @radius;
}
QWidget#NavAction:hover { background: @surfaceHover; border-color: @textFaint; }
QWidget#NavAction:pressed { background: @accentSoft; border-color: @accent; }
QWidget#NavAction QLabel#NavValue { color: @text; }

/* 开关类设置行：整行可点，hover 给一点底色反馈 */
QWidget#SettingRow[toggleRow="true"]:hover { background: @surfaceHover; }

/* ================================================================ 面包屑条 */
QWidget#CrumbBar { background: @bg; border-bottom: 1px solid @border; }
QToolButton#Crumb {
    background-color: transparent; border: none; border-radius: @radius;
    color: @textDim; font-size: 12px; padding: 3px 8px;
}
QToolButton#Crumb:hover { color: @text; background-color: @surfaceHover; }
QToolButton#Crumb:checked { color: @accent; font-weight: 500; }
QLabel#CrumbSep { color: @textFaint; font-size: 12px; }
QToolButton#CrumbMenu {
    background-color: transparent; border: none; border-radius: @radius;
}
QToolButton#CrumbMenu:hover { background-color: @surfaceHover; }

/* ================================================================ 首页分区
   Python 首页是 tabWidget：tab 在顶部、选中项主色 + 2px 下划线（见基座 qss 的
   QTabBar::tab:top:selected），这里把继承来的 uppercase/bold 去掉。 */
QTabWidget#HomeTabs::pane { border: none; background: transparent; }
QTabWidget#HomeTabs > QTabBar { text-transform: none; font-weight: normal; }
QTabWidget#HomeTabs > QTabBar::tab {
    background: transparent; border: none;
    color: @textDim; font-size: 13px;
    padding: 6px 14px; height: 24px;
    border-bottom: 2px solid transparent;
}
QTabWidget#HomeTabs > QTabBar::tab:hover { color: @text; }
QTabWidget#HomeTabs > QTabBar::tab:selected {
    color: @accent; border-bottom: 2px solid @accent;
}
/* 分区多 / 窗口窄时 QTabBar 会出现左右滚动按钮；基座里那两条 rule 指向
   res 主题目录下的 svg（本目标没打包这些文件），箭头会是空的，这里补上。 */
QTabWidget#HomeTabs > QTabBar QToolButton {
    background-color: transparent; border: none;
    width: 18px; margin: 0px; padding: 0px;
}
QTabWidget#HomeTabs > QTabBar QToolButton:hover { background-color: @surfaceHover; }
QTabWidget#HomeTabs > QTabBar QToolButton::left-arrow {
    image: url(@chevronLeft); width: 9px; height: 9px;
}
QTabWidget#HomeTabs > QTabBar QToolButton::right-arrow {
    image: url(@chevronRight); width: 9px; height: 9px;
}
QWidget#HomeBar { background: transparent; }
QSpinBox#PageSpin {
    background-color: @surface; border: 1px solid @border;
    border-radius: @radius; color: @text;
    padding: 2px 6px; height: 22px; font-size: 12px;
}

/* ================= 超分模型选择浮层（docs/waifu2x_model_list_demo.html） =================
   浮层 = 整窗遮罩（C++ 自绘）+ 底部/居中卡片。尺寸与吸顶分组条都在 C++ 里算，
   这里只定"长什么样"：
     * 卡片 surface 底 + 1px 边框（全站直角，demo 的 16px 圆角不搬过来）；
     * 列头/底栏用 bgAlt，与列表行拉开层次；
     * 列表自身透明：行底色由 delegate 按"当前使用/已选/hover"逐行画，
       统一背景会在选中行下面透出第二层颜色；
     * 行内高度有版式意义（两行文本），这里不解锁按钮高度；只有 chip 与底栏按钮
       按 demo 的紧凑度量重钉 —— 基座 QPushButton 默认 34px 内容盒 + padding，
       五个 chip 会占掉浮层三分之一高度。 */
QWidget#SrSheetCard {
    background-color: @surface;
    border: 1px solid @border;
}
QWidget#SrSheetGrip { background: transparent; }
QWidget#SrSheetGripBar { background: @textFaint; }
QLabel#SrSheetTitle { color: @text; font-size: 16px; font-weight: 500; }
QLabel#SrSheetSub { color: @textFaint; font-size: 11px; }
QPushButton#SrSheetClose {
    background: transparent; color: @textDim; border: 1px solid @border;
    border-radius: @radius; font-size: 13px; padding: 0px;
    height: 26px; min-height: 26px; max-height: 26px;
    width: 26px; min-width: 26px; max-width: 26px;
}
QPushButton#SrSheetClose:hover { background: @surfaceHover; color: @text; }
QWidget#SrSheetCard QPushButton#Chip {
    /* demo 的 chip 是"两行（名称 + 计数）+ 5px padding"≈35px 高；这里是单行、
       计数跟在名称后面，用 28px 内容盒（+ 1px 边框 = 30px 实高）折中：
       再矮就不像可点的筛选项了 */
    height: 28px; min-height: 28px; max-height: 28px;
    padding: 0px 10px; font-size: 11px;
    /* 基座 qss 给 QPushButton 加了 text-transform: uppercase，
       "Real-CUGAN" 会变成 "REAL-CUGAN"，和列表里的大小写对不上 */
    text-transform: none;
}
QWidget#SrSheetColHead {
    background-color: @bgAlt;
    border-top: 1px solid @border;
    border-bottom: 1px solid @border;
}
QLabel#SrSheetCol { color: @textFaint; font-size: 10px; }
QLabel#SrSheetColCount {
    color: @textFaint; font-size: 10px; margin-left: 6px;
    font-family: "Consolas", "Menlo", "DejaVu Sans Mono", monospace;
}
QListWidget#SrModelList {
    background: transparent; border: none; outline: none;
}
QListWidget#SrModelList QScrollBar:vertical {
    background: transparent; width: 4px; margin: 0px;
}
QListWidget#SrModelList QScrollBar::handle:vertical {
    background: @textFaint; min-height: 28px; border-radius: @radius;
}
QWidget#SrSheetFoot { background-color: @bgAlt; border-top: 1px solid @border; }
/* 测速工具条（demo v2）：第 1 行按钮 30px 实高（内容盒 18 + 上下 padding 5 + 边框 2），
   第 2 行状态 10px 等宽 + 2px 进度条。基座把 QPushButton 钉在 34px，
   这三个按钮必须用属性选择器解锁高度。 */
QWidget#SrSheetCard QPushButton[benchBtn="true"] {
    /* 对象名不再是 #Ghost，必须把中性态写全，否则会落回基座的橙色描边按钮 */
    background: transparent; color: @textDim; border: 1px solid @border;
    height: 18px; min-height: 18px; max-height: 18px;
    /* 11px + 左右 10px：340dp 下三个按钮合计约 291dp，稳放进 314dp；
       12px 时实测刚好溢出 8dp，"仅"字会被裁掉 */
    padding: 5px 10px; font-size: 11px; border-radius: @radius;
}
QWidget#SrSheetCard QPushButton[benchBtn="true"]:hover {
    border-color: @textFaint; color: @text;
}
QWidget#SrSheetCard QPushButton[benchBtn="true"]:disabled {
    color: @textFaint; border-color: @border;
}
QPushButton[benchBtn="true"][benchPrimary="true"] {
    border-color: @accent; color: @accent; background: @accentSoft;
}
QPushButton[benchBtn="true"][benchStop="true"] {
    border-color: @danger; color: @danger;
}
QLabel#SrBenchStat {
    color: @textFaint; font-size: 10px;
    font-family: "Consolas", "Menlo", "DejaVu Sans Mono", monospace;
}
QLabel#SrBenchStat[run="true"] { color: @accent; }
QLabel#SrBenchLegend { color: @textFaint; font-size: 10px; }
/* 取消后再次打开时的"继续测试"悬浮条 */
QWidget#SrBenchResume { background-color: @bgAlt; border: 1px solid @accent; }
QLabel#SrBenchResumeText { color: @text; font-size: 11px; }
QLabel#SrSheetSelKey { color: @textFaint; font-size: 10px; }
QLabel#SrSheetSelVal { color: @accent; font-size: 10px; }
QWidget#SrSheetCard QPushButton#Ghost,
QWidget#SrSheetCard QPushButton#Primary {
    height: 22px; min-height: 22px; max-height: 22px;
    padding: 7px 14px; font-size: 13px;
}

/* 整行「模型选择栏」：主行显示名 + 副行 onnx 完整 ID。
   基座把 QPushButton 内容盒钉在 34px，两行文本放不下，这里按 40px 解锁
   （40 + 上下 padding 6×2 + 边框 2 = 54dp 实高）。 */
QWidget#SrModelField {
    background-color: @bgAlt;
    border: 1px solid @border;
    border-radius: @radiusOption;
}
QWidget#SrModelField:hover { border-color: @textFaint; background-color: @surfaceHover; }
QLabel#SrFieldMain { color: @text; font-size: 13px; }
QLabel#SrFieldMain[installed="false"] { color: @textFaint; }
QLabel#SrFieldId { color: @textFaint; font-size: 10px; }

/* 超分页 / 批量超分页的卡片与信息行（demo 的 .card + .kv） */
QWidget#SrCard {
    background-color: @surface; border: 1px solid @border; border-radius: @radius;
}
QLabel#SrCardTitle { color: @textFaint; font-size: 11px; }
QLabel#SrFieldLabel { color: @textDim; font-size: 11px; }
QLabel#SrKvKey { color: @textFaint; font-size: 11px; }
QLabel#SrPreview { color: @textFaint; font-size: 12px; background: transparent; }
QLabel#SrKvVal { color: @text; font-size: 11px; }
QWidget#SrStage { background-color: @bgAlt; border: 1px solid @border; }
QLabel#SrBadge {
    background-color: rgba(0, 0, 0, 96); color: @text;
    border: 1px solid @textFaint; border-radius: @radius;
    padding: 1px 5px; font-size: 9px;
}
QWidget#SrPathBox {
    background-color: @bgAlt; border: 1px solid @border; border-radius: @radius;
}
QLabel#SrPathText { color: @textDim; font-size: 11px; }
)QSS");
}

// ---------------------------------------------------------------- 字号缩放
namespace UiStyle {

qreal FontScale()
{
    // 以 9pt 为设计基准；上下界见 theme.h 的说明。
    return qBound(0.85, AppFont::I().EffectivePointSize() / 9.0, 1.5);
}

int Fpx(int designPx)
{
    return qMax(8, qRound(designPx * FontScale()));
}

void MarkSoftButton(QWidget *w)
{
    // 动态属性只读取一次：控件首次 polish 时样式表引擎按属性选择器匹配。
    // 本项目的按钮都在构造期、show 之前打标记，所以不需要 unpolish/polish。
    if (w)
        w->setProperty("btnShape", "soft");
}

}  // namespace UiStyle
