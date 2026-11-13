#include "app_font.h"

#include "app_paths.h"

#include <QAbstractNativeEventFilter>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>
#include <functional>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

constexpr double kBasePoint = 9.0;   // 设计基准字号（主题 qss 里的 px 都按它标定）
// 界面是按 9pt 排的：标题栏 44px、导航按钮 70x54、卡片文字区 40px 都是定值。
// 字号超过 12pt（=1.33 倍）后，这些容器开始裁字/挤压，所以跟随系统时也夹到 12pt。
// 想放开上限：把这里调大，并用 screenshots 逐页复核 ui_pages_* 的定高容器。
constexpr int kMinPoint = 8;
constexpr int kMaxPoint = 12;

QString ConfigPath()
{
    // 与 ui.ini 同源（AppPaths 统一解析）；Android 上不会写进只读的 appdir。
    static const QString path = [] {
        AppPaths::MigrateLegacyFile(QStringLiteral("ui.ini"));
        return AppPaths::ConfigFile(QStringLiteral("ui.ini"));
    }();
    return path;
}

// 系统 UI 字体。Windows 直接问 Win32——这是唯一能保证"刚改完就能读到新值"的途径：
// QFontDatabase::systemFont() 依赖 Qt 平台插件先处理完 WM_SETTINGCHANGE，
// 而我们的原生事件过滤器跑在它前面，立刻读会拿到旧缓存。
QFont QuerySystemFont()
{
#ifdef Q_OS_WIN
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        const QString family = QString::fromWCharArray(ncm.lfMessageFont.lfFaceName);
        if (!family.isEmpty()) {
            QFont f;
            f.setFamily(family);
            // lfHeight < 0 表示"字符高度"，单位是系统 DPI 下的像素；换算成 pt 与 Qt 对齐。
            int sysDpi = 96;
            if (HDC hdc = ::GetDC(nullptr)) {
                sysDpi = ::GetDeviceCaps(hdc, LOGPIXELSY);
                ::ReleaseDC(nullptr, hdc);
            }
            const int h = ncm.lfMessageFont.lfHeight;
            const double pt = h < 0 ? (-h * 72.0 / qMax(1, sysDpi))
                                    : (h * 72.0 / qMax(1, sysDpi) * 0.75);
            f.setPointSizeF(qBound(6.0, pt, 24.0));
            if (ncm.lfMessageFont.lfWeight > 0)
                f.setWeight(QFont::Weight(qBound(100, int(ncm.lfMessageFont.lfWeight), 900)));
            return f;
        }
    }
#endif
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont);
}

bool SameFont(const QFont &a, const QFont &b)
{
    return a.family() == b.family() && qAbs(a.pointSizeF() - b.pointSizeF()) < 0.05;
}

}  // namespace

// ---------------------------------------------------------------- 系统字体监听
namespace {

class SystemFontWatcher : public QObject, public QAbstractNativeEventFilter
{
public:
    explicit SystemFontWatcher(std::function<void()> onChanged)
        : onChanged_(std::move(onChanged))
    {
        // 防抖：WM_SETTINGCHANGE 一次系统设置变更会连着来好几条
        debounce_.setSingleShot(true);
        debounce_.setInterval(250);
        connect(&debounce_, &QTimer::timeout, this, [this] {
            if (onChanged_)
                onChanged_();
        });

        // 兜底轮询：原生消息漏了（或被别的窗口吃掉）也能在 2 秒内发现。
        // 只比 family + 字号，代价是两次 SystemParametersInfo，微秒级。
        poll_.setInterval(2000);
        connect(&poll_, &QTimer::timeout, this, [this] {
            const QFont now = QuerySystemFont();
            if (!SameFont(now, last_)) {
                last_ = now;
                schedule();
            }
        });
    }

    void start()
    {
        last_ = QuerySystemFont();
        qApp->installNativeEventFilter(this);
        // 运行期安装/删除字体（fontDatabaseChanged）也可能影响回退链
        connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, this, [this] { schedule(); });
        poll_.start();
    }

    void schedule() { debounce_.start(); }

    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override
    {
#ifdef Q_OS_WIN
        if (type == "windows_generic_MSG" || type == "windows_dispatcher_MSG") {
            auto *msg = static_cast<MSG *>(message);
            if (msg->message == WM_SETTINGCHANGE) {
                // SPI_SETNONCLIENTMETRICS = 0x0029：系统字体/字号/标题栏字号变更都走它
                if (msg->wParam == SPI_SETNONCLIENTMETRICS)
                    schedule();
            } else if (msg->message == WM_FONTCHANGE || msg->message == WM_THEMECHANGED) {
                schedule();
            }
        }
#else
        Q_UNUSED(type);
        Q_UNUSED(message);
#endif
        return false;
    }

private:
    std::function<void()> onChanged_;
    QTimer debounce_;
    QTimer poll_;
    QFont last_;
};

}  // namespace

// ---------------------------------------------------------------- AppFont
AppFont &AppFont::I()
{
    static AppFont inst;
    return inst;
}

namespace {
QStringList g_families;   // 缓存：系统里装/卸字体时由 Init() 里的 fontDatabaseChanged 清空
}

QStringList AppFont::Families()
{
    if (g_families.isEmpty()) {
        g_families = QFontDatabase::families();
        g_families.sort(Qt::CaseInsensitive);
    }
    return g_families;
}

void AppFont::Init()
{
    if (!applyTimer_) {
        applyTimer_ = new QTimer(this);
        applyTimer_->setSingleShot(true);
        applyTimer_->setInterval(40);   // 拖动滑块时合并刷新，不会每步重刷整个界面
        connect(applyTimer_, &QTimer::timeout, this, &AppFont::ApplyNow);
    }

    Load();
    ReadSystemFont();
    ApplyNow();

    // 监听系统字体变更（单例，装一次）
    static SystemFontWatcher *watcher = nullptr;
    if (!watcher) {
        watcher = new SystemFontWatcher([] { AppFont::I().RefreshFromSystem(); });
        watcher->start();
        QObject::connect(qGuiApp, &QGuiApplication::fontDatabaseChanged, qGuiApp,
                         [] { g_families.clear(); });
    }
}

void AppFont::Load()
{
    QSettings s(ConfigPath(), QSettings::IniFormat);
    s.beginGroup(QStringLiteral("font"));
    const QString src = s.value(QStringLiteral("source"), QStringLiteral("system")).toString();
    cfg_.source = (src == QLatin1String("custom")) ? Source::Custom : Source::System;
    cfg_.family = s.value(QStringLiteral("family")).toString();
    cfg_.weight = qBound(100, s.value(QStringLiteral("weight"), 400).toInt(), 900);
    cfg_.widthPct = qBound(50, s.value(QStringLiteral("width"), 100).toInt(), 200);
    cfg_.pointSize = qBound(kMinPoint, s.value(QStringLiteral("size"), 9).toInt(), kMaxPoint);
    s.endGroup();
    if (cfg_.source == Source::Custom && cfg_.family.isEmpty()) {
        cfg_.source = Source::System;   // 配置残缺：退回跟随系统，别把界面搞成默认字体
    }
}

void AppFont::Persist() const
{
    const QString path = ConfigPath();
    if (path.isEmpty()) {
        qWarning("app-font: 没有可写的配置目录，字体设置未落库");
        return;
    }
    QSettings s(path, QSettings::IniFormat);
    s.beginGroup(QStringLiteral("font"));
    s.setValue(QStringLiteral("source"),
               cfg_.source == Source::Custom ? QStringLiteral("custom") : QStringLiteral("system"));
    s.setValue(QStringLiteral("family"), cfg_.family);
    s.setValue(QStringLiteral("weight"), cfg_.weight);
    s.setValue(QStringLiteral("width"), cfg_.widthPct);
    s.setValue(QStringLiteral("size"), cfg_.pointSize);
    s.endGroup();
    s.sync();
    if (s.status() != QSettings::NoError)
        qWarning("app-font: 写入失败 status=%d path=%s", int(s.status()), qPrintable(path));
}

void AppFont::ReadSystemFont()
{
    if (!sysFromTest_)
        sysFont_ = QuerySystemFont();
    if (sysFont_.family().isEmpty()) {
        // 极端兜底：系统字体读不出来时，别让界面变成方框
        sysFont_.setFamily(QFontDatabase::systemFont(QFontDatabase::GeneralFont).family());
        if (sysFont_.pointSizeF() <= 0)
            sysFont_.setPointSizeF(kBasePoint);
    }
    // 跟随系统模式下，字号滑块要显示系统当前值
    if (cfg_.source == Source::System)
        cfg_.pointSize = qBound(kMinPoint, qRound(sysFont_.pointSizeF()), kMaxPoint);
}

void AppFont::RefreshFromSystem()
{
    const QFont before = sysFont_;
    ReadSystemFont();
    const bool changed = !SameFont(before, sysFont_);
    // 自定义模式下系统字体也可能影响"回退家族"，所以一律重新应用；
    // 是否真的需要重建 qss 由 UiTheme 比较缩放比决定。
    ApplyNow();
    if (changed) {
        std::fprintf(stderr, "app-font: system font changed -> %s %.1fpt\n",
                     qPrintable(sysFont_.family()), sysFont_.pointSizeF());
        std::fflush(stderr);
    }
}

QFont AppFont::Build() const
{
    QFont f;
    if (cfg_.source == Source::System) {
        f = sysFont_;
        // 调试口子：JM_UI_FONT=<family> 可强制指定（A/B 对比、缺字体的机器）
        const QString env = qEnvironmentVariable("JM_UI_FONT");
        if (!env.isEmpty())
            f.setFamily(env);
        f.setHintingPreference(QFont::PreferFullHinting);
        return f;
    }

    QString family = cfg_.family;
    if (!QFontDatabase::families().contains(family, Qt::CaseInsensitive))
        family = sysFont_.family();          // 回退 1：字体不存在
    // 回退 2：自定义字体缺中文字形时，由 Qt 逐字落到系统字体，不会出现豆腐块
    f.setFamilies({family, sysFont_.family()});
    // 与 qss 的缩放比保持一致：基础字号和 px 缩放必须是同一个夹取结果
    f.setPointSize(qBound(kMinPoint, cfg_.pointSize, kMaxPoint));
    f.setWeight(QFont::Weight(qBound(100, cfg_.weight, 900)));
    f.setStretch(qBound(50, cfg_.widthPct, 200));
    // 可变字体：显式给 wght / wdth 轴。字体不是可变字体时 Qt 会忽略这两个值，
    // 退回上面的 weight/stretch 就近匹配 —— 两条路都留着，才能既支持变体又不炸普通字体。
    f.setVariableAxis(QFont::Tag("wght"), float(cfg_.weight));
    f.setVariableAxis(QFont::Tag("wdth"), float(cfg_.widthPct));
    f.setHintingPreference(QFont::PreferFullHinting);
    return f;
}

QString AppFont::ResolvedFamily() const
{
    return QFontInfo(Build()).family();
}

QString AppFont::SystemFamily() const
{
    return sysFont_.family();
}

double AppFont::EffectivePointSize() const
{
    return Build().pointSizeF();
}

bool AppFont::CustomFamilyAvailable() const
{
    return QFontDatabase::families().contains(cfg_.family, Qt::CaseInsensitive);
}

QString AppFont::StatusText() const
{
    if (cfg_.source == Source::System) {
        // 系统字体在 Android 上是按像素定义的，pointSizeF() 会是 -1：
        // 直接打 "−1.0pt" 会让人以为字体坏了，这种情况改报 px。
        const QFontInfo info(Build());
        const qreal pt = info.pointSizeF();
        const QString size = pt > 0 ? QStringLiteral("%1pt").arg(pt, 0, 'f', 1)
                                    : QStringLiteral("%1px").arg(info.pixelSize());
        return QStringLiteral("跟随系统：%1 · %2").arg(info.family(), size);
    }

    // 显示"实际解析到"的值而不是滑块值：可变字体的轴有范围（Segoe UI Variable 的
    // wght 只到 700），滑块拉到 800 时如实写成 700，比拍胸脯说"可变轴生效"有用。
    const QFontInfo info(Build());
    QString text = QStringLiteral("%1 · %2pt · 字重 %3 · 字宽 %4%")
                       .arg(info.family())
                       .arg(cfg_.pointSize)
                       .arg(int(info.weight()))
                       .arg(cfg_.widthPct);
    if (!CustomFamilyAvailable())
        text += QStringLiteral(" · 字体不可用，已回退系统字体");
    if (qAbs(int(info.weight()) - cfg_.weight) > 30) {
        // 静态字体通常只有 Regular/Bold 两档：滑块 500~900 全都会落到 700，
        // 看起来像"改字重没反应"。这里如实说清楚，而不是暗示可变轴在生效。
        text += QStringLiteral("（请求 %1：该字体没有这一档，实际按 %2 渲染）")
                    .arg(cfg_.weight)
                    .arg(int(info.weight()));
    }
    // 字宽：QFont::setStretch 只有在字体族能匹配/合成对应字面时才真正改变字形。
    // 用同一串文字量一下"当前字宽"与"100% 字宽"的排版宽度，差得太少就说明没生效。
    if (qAbs(cfg_.widthPct - 100) > 10) {
        const QString probe = QStringLiteral("字体宽度 Width 100%");
        QFont base = Build();
        base.setStretch(100);
        const qreal now = QFontMetricsF(Build()).horizontalAdvance(probe);
        const qreal ref = QFontMetricsF(base).horizontalAdvance(probe);
        if (ref > 1.0 && qAbs(now - ref) < ref * 0.03)
            text += QStringLiteral(" · 该字体不支持字宽变化，实际未生效");
    }
    return text;
}

void AppFont::SetSource(Source s)
{
    if (cfg_.source == s)
        return;
    if (s == Source::Custom && cfg_.family.isEmpty())
        cfg_.family = sysFont_.family();     // 从系统字体起步，避免一下子跳成默认字体
    cfg_.source = s;
    Persist();
    ScheduleApply();
}

void AppFont::SetFamily(const QString &family)
{
    if (cfg_.family == family)
        return;
    cfg_.family = family;
    Persist();
    ScheduleApply();
}

void AppFont::SetWeight(int w)
{
    w = qBound(100, w, 900);
    if (cfg_.weight == w)
        return;
    cfg_.weight = w;
    Persist();
    ScheduleApply();
}

void AppFont::SetWidth(int pct)
{
    pct = qBound(50, pct, 200);
    if (cfg_.widthPct == pct)
        return;
    cfg_.widthPct = pct;
    Persist();
    ScheduleApply();
}

void AppFont::SetPointSize(int pt)
{
    pt = qBound(kMinPoint, pt, kMaxPoint);
    if (cfg_.pointSize == pt)
        return;
    cfg_.pointSize = pt;
    Persist();
    ScheduleApply();
}

void AppFont::ResetToSystem()
{
    cfg_.source = Source::System;
    cfg_.family.clear();
    cfg_.weight = 400;
    cfg_.widthPct = 100;
    Persist();
    ScheduleApply();
}

void AppFont::SetSystemFontProviderForTest(const QFont &font)
{
    sysFromTest_ = true;
    sysFont_ = font;
    RefreshFromSystem();
}

void AppFont::ClearSystemFontProviderForTest()
{
    sysFromTest_ = false;
    ReadSystemFont();
    ApplyNow();
}

void AppFont::ScheduleApply()
{
    if (applyTimer_)
        applyTimer_->start();
    else
        ApplyNow();
}

void AppFont::ApplyNow()
{
    if (applying_)
        return;
    applying_ = true;

    const QFont font = Build();
    // 去重必须带上可变轴与来源：QFont::key() 只覆盖 family/size/weight/stretch，
    // 有些平台（Android 的 FreeType 引擎）只认可变轴，光比 key 会把
    // "只改字重/字宽"的请求判成没变化（设置页滑块拖了但界面不动）。
    QString sig = font.key();
    sig += QStringLiteral("|wght:%1|wdth:%2|src:%3")
               .arg(double(font.variableAxisValue(QFont::Tag("wght"))), 0, 'f', 2)
               .arg(double(font.variableAxisValue(QFont::Tag("wdth"))), 0, 'f', 2)
               .arg(int(cfg_.source));
    // 首次必须应用一次（默认 QFont 恰好就是系统字体，key 相同，不能因此跳过）
    if (hasApplied_ && appliedKey_ == sig) {
        applying_ = false;
        return;   // 没有任何变化：不重刷界面，也不发信号
    }
    // QFont 变化会触发 ApplicationFontChange，各控件自行重排；这里只负责"设进去"。
    if (auto *app = qobject_cast<QApplication *>(QCoreApplication::instance()))
        app->setFont(font);
    applied_ = font;
    appliedKey_ = sig;
    hasApplied_ = true;

    std::fprintf(stderr, "app-font: source=%s family=%s -> resolved=%s weight=%d size=%.1fpt\n",
                 cfg_.source == Source::Custom ? "custom" : "system", qPrintable(font.family()),
                 qPrintable(QFontInfo(font).family()), int(QFontInfo(font).weight()),
                 font.pointSizeF());
    std::fflush(stderr);

    applying_ = false;
    emit Changed();
}
