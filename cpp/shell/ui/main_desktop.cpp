// 桌面端入口：C++ 版 UI 的独立可执行程序。
//
// 与 android 用的 main.cpp 分开：那个入口带 dl_iterate_phdr / JNI / sr_qnn 直链探测，
// 属于设备侧验收程序；这里是"桌面预览 + UI 验收"入口，不需要 Android 依赖。
//
// 支持参数：
//   --page home|category|search|detail|reader|download|local|favorite|history|
//          comment|sr|settings|about|login
//   --theme dark_teal|dark_pink|dark_orange|light_teal|light_pink|light_orange
//   --shot <png>      渲染后截图并退出（用于视觉回归）
//   --quiet           静默验收：窗口挪到屏幕外、不抢焦点、不进任务栏；
//                     --quiet-minimized 改为最小化启动。两者都不影响截图内容。
#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

#include "../sr_engine.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QEvent>
#include <QLabel>
#include <QListWidget>
#include <QWidget>
#ifdef Q_OS_WIN
#include <windows.h>   // 静默验收：把窗口挪出可视区并摘掉激活/任务栏样式
#endif
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetrics>
#include <QPushButton>
#include <QFileInfo>
#include <QHash>
#include <QMouseEvent>
#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QAbstractScrollArea>
#include <QPointingDevice>
#include <QTouchEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QUrl>

#ifdef Q_OS_ANDROID
#include <QJniObject>
#endif

#include "../doh_client.h"
#include "../jm_webp.h"
#include "../net_client.h"
#include "../node_http.h"
#include "app_font.h"
#include "app_paths.h"
#include "cache_path.h"
#include "comic_view.h"
#include "models.h"
#include "proxy_config.h"
#include "search_history.h"
#include "session.h"
#include "ui_log.h"
#include "ui_prefs.h"
#include "theme.h"
#include "touch.h"
#include "sr_model_sheet.h"
#include "ui_kit.h"
#include "ui_main_window.h"
#include "ui_pages.h"

namespace {

// 合成一次真实的鼠标拖拽：从窗口左边缘按下 → 水平拖 dx → 松手。
// 走的是 QCoreApplication::notify，因此应用级事件过滤器（TouchAdapter）会像
// 处理真实输入一样处理它 —— 这是"手势状态机"能被自动化验收的唯一办法。
void DemoEdgeDrag(MainWindow &w, int dx, bool commit)
{
    const QPoint local(4, w.height() / 2);
    const QPoint global = w.mapToGlobal(local);
    QCoreApplication::sendEvent(
        &w, new QMouseEvent(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
                            Qt::LeftButton, Qt::LeftButton, Qt::NoModifier));
    for (int i = 1; i <= 6; ++i) {
        const QPoint p(local.x() + dx * i / 6, local.y());
        const QPoint g = w.mapToGlobal(p);
        QCoreApplication::sendEvent(
            &w, new QMouseEvent(QEvent::MouseMove, QPointF(p), QPointF(g), Qt::NoButton,
                                Qt::LeftButton, Qt::NoModifier));
    }
    if (commit) {
        const QPoint p(local.x() + dx, local.y());
        const QPoint g = w.mapToGlobal(p);
        QCoreApplication::sendEvent(
            &w, new QMouseEvent(QEvent::MouseButtonRelease, QPointF(p), QPointF(g),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier));
    } else {
        const QPoint p(local.x() + dx, local.y());
        const QPoint g = w.mapToGlobal(p);
        QCoreApplication::sendEvent(
            &w, new QMouseEvent(QEvent::MouseButtonRelease, QPointF(p), QPointF(g),
                                Qt::LeftButton, Qt::NoButton, Qt::NoModifier));
    }
}

PageId PageFromName(const QString &name)
{
    static const QHash<QString, PageId> map = {
        {QStringLiteral("home"), PageId::Home},
        {QStringLiteral("category"), PageId::Category},
        {QStringLiteral("search"), PageId::Search},
        {QStringLiteral("detail"), PageId::Detail},
        {QStringLiteral("reader"), PageId::Reader},
        {QStringLiteral("download"), PageId::Download},
        {QStringLiteral("local"), PageId::Local},
        {QStringLiteral("favorite"), PageId::Favorite},
        {QStringLiteral("history"), PageId::History},
        {QStringLiteral("comment"), PageId::Comment},
        {QStringLiteral("sr"), PageId::Sr},
        {QStringLiteral("settings"), PageId::Settings},
        {QStringLiteral("about"), PageId::About},
        {QStringLiteral("login"), PageId::Login},
        {QStringLiteral("mycomment"), PageId::MyComment},
        {QStringLiteral("remote"), PageId::RemoteHistory},
        {QStringLiteral("localfav"), PageId::LocalFavorite},
        {QStringLiteral("week"), PageId::Week},
        {QStringLiteral("nas"), PageId::Nas},
        {QStringLiteral("batchsr"), PageId::BatchSr},
        {QStringLiteral("help"), PageId::Help},
        {QStringLiteral("proxy"), PageId::Proxy},
    };
    return map.value(name, PageId::Home);
}

// ---- 静默验收模式（回归脚本用）---------------------------------------------
// 目标：窗口"存在但不打扰"——不抢前台焦点、不进任务栏/Alt-Tab、不出现在任何
// 显示器上。之所以不直接用 -platform offscreen：那个插件拿不到系统字体，
// 中文会渲染成方框、DPR 退化成 1，截图就失去视觉基线价值了。
// QWidget::grab() 是把控件树直接 render 进 QPixmap，与窗口是否可见无关，
// 所以窗口挪到屏幕外之后，截图仍是原生路径（真实 DPI / 字体 / QML）。
enum class QuietMode { Off, Hidden, Minimized };

static QuietMode QuietModeFrom(const QCommandLineParser &parser)
{
    if (parser.isSet(QStringLiteral("quiet-minimized")))
        return QuietMode::Minimized;
    if (parser.isSet(QStringLiteral("quiet")))
        return QuietMode::Hidden;
    // 环境变量给外部工具留一条路（同一份 exe，不必改 argv）
    const QString env = qEnvironmentVariable("JM_UI_QUIET").trimmed().toLower();
    if (env.isEmpty() || env == QLatin1String("0") || env == QLatin1String("off"))
        return QuietMode::Off;
    if (env == QLatin1String("minimized"))
        return QuietMode::Minimized;
    return QuietMode::Hidden;
}

static void ShowQuiet(QWidget &w, QuietMode mode)
{
    w.setAttribute(Qt::WA_ShowWithoutActivating, true);
    const bool minimized = (mode == QuietMode::Minimized);
#ifdef Q_OS_WIN
    // 先建原生窗口再改扩展样式：任务栏按钮在首次 show 时按当时的样式建立，
    // 之后再补 WS_EX_TOOLWINDOW 会留下一个"幽灵"按钮。
    HWND hwnd = reinterpret_cast<HWND>(w.winId());
    if (hwnd) {
        const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW);
    }
#endif
    if (minimized) {
        w.showMinimized();
    } else {
        w.move(-32000, -32000);   // Win32 约定的屏幕外坐标：不参与合成、也照不到
        w.show();
    }
#ifdef Q_OS_WIN
    if (hwnd) {
        // Qt 的 show 可能重置位置/激活行为，这里再钉一次
        if (minimized)
            ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
        else
            SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
#endif
}

// 静默模式下的"兜底守卫"：主窗口之外还会有对话框（确认框、屏蔽词输入、搜索建议…），
// 它们都是独立顶层窗口，自带激活逻辑。这里统一补上 WS_EX_NOACTIVATE，
// 避免某个新加的弹窗把用户正在操作的窗口顶到后面。
class QuietGuard : public QObject
{
public:
    explicit QuietGuard(QObject *parent = nullptr) : QObject(parent) {}

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (ev->type() == QEvent::Show) {
            if (auto *w = qobject_cast<QWidget *>(obj)) {
                if (w->isWindow())
                    w->setAttribute(Qt::WA_ShowWithoutActivating, true);
#ifdef Q_OS_WIN
                if (w->isWindow()) {
                    if (HWND h = reinterpret_cast<HWND>(w->winId())) {
                        const LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
                        SetWindowLongPtrW(h, GWL_EXSTYLE, ex | WS_EX_NOACTIVATE);
                    }
                }
#endif
            }
        }
        return QObject::eventFilter(obj, ev);
    }
};

}  // namespace


#ifdef Q_OS_ANDROID
// 手机上没有命令行：用 `am start ... --es jm.args "--open-album 1488544"` 传参，
// 与桌面 CLI 同一套入口，便于在真机上确定性地复现某条链路。
static QString AndroidExtra(const char *key)
{
    // QtNative.activity() 是 Qt Android 的公开入口，比 QNativeInterface 更稳
    // （后者的头文件在 6.12 里需要额外的 platform 头）。
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid())
        return QString();
    QJniObject intent = activity.callObjectMethod("getIntent", "()Landroid/content/Intent;");
    if (!intent.isValid())
        return QString();
    QJniObject name = QJniObject::fromString(QString::fromLatin1(key));
    QJniObject value = intent.callObjectMethod("getStringExtra",
                                               "(Ljava/lang/String;)Ljava/lang/String;",
                                               name.object<jstring>());
    return value.isValid() ? value.toString() : QString();
}
#endif

// 布局体检：把当前页的控件树连同几何打出来（"空白到底是谁占的"这类问题用它定位）。
static void DumpLayoutTree(QWidget *w, int depth, int maxDepth)
{
    if (!w || depth > maxDepth)
        return;
    const QRect g = w->geometry();
    // min 是"布局能给到的最小宽度"：横向溢出（内容比屏幕宽）就是某一层的
    // minimumSizeHint 超过视口宽度，只看 sizeHint 是找不到责任控件的。
    qWarning("layout-dump: %*s%s[%s] %dx%d+%d+%d vis=%d hint=%dx%d min=%dx%d",
             depth * 2, "", w->metaObject()->className(),
             qPrintable(w->objectName().isEmpty() ? QStringLiteral("-") : w->objectName()),
             g.width(), g.height(), g.x(), g.y(), int(w->isVisible()),
             w->sizeHint().width(), w->sizeHint().height(),
             w->minimumSizeHint().width(), w->minimumSizeHint().height());
    for (QObject *child : w->children()) {
        if (auto *cw = qobject_cast<QWidget *>(child)) {
            if (!cw->isWindow())
                DumpLayoutTree(cw, depth + 1, maxDepth);
        }
    }
}

// 本轮收尾的可量测验收：空白带高度 vs 文本高度、开关是否与文字同排。
// 只打印数字，桌面/AVD 都能用日志断言，不依赖抓屏像素。
static void ReportBlankBands(MainWindow &window)
{
    auto textHeight = [](const QWidget *w) {
        return w ? QFontMetrics(w->font()).height() : -1;
    };

    // 首页底部：只有一行 "1/1 · 18 部" 的页码条
    if (PageBase *home = window.FindPage(PageId::Home)) {
        const QList<QWidget *> bars = home->findChildren<QWidget *>(QStringLiteral("HomeBar"));
        for (QWidget *bar : bars) {
            QLabel *meta = bar->findChild<QLabel *>(QStringLiteral("RowMeta"));
            if (!bar->isVisible() || !meta)
                continue;
            qWarning("blank-report: home pagerH=%d pagerTextH=%d labelH=%d",
                     bar->height(), textHeight(meta), meta->height());
            break;
        }
    }

    // 搜索页搜索框下方：第一条带「清空」文字链的分组标题行
    if (PageBase *search = window.FindPage(PageId::Search)) {
        const QList<QPushButton *> links =
            search->findChildren<QPushButton *>(QStringLiteral("Link"));
        for (QPushButton *link : links) {
            QWidget *header = link->parentWidget();
            QLabel *title = header
                ? header->findChild<QLabel *>(QStringLiteral("SectionTitle")) : nullptr;
            if (!header || !title || !header->isVisible())
                continue;
            qWarning("blank-report: search headerH=%d titleTextH=%d linkH=%d",
                     header->height(), textHeight(title), link->height());
            break;
        }
        // 结果统计行（"「x」的搜索结果 · 共 N 部"）：它在卡片墙之上的独立一行。
        // 这里必须是 hintH == hintTextH，否则说明 QVBoxLayout 又把富余高度塞给了
        // 这一行，界面上就是搜索框与卡片之间那条巨大的空白带。
        const QList<QLabel *> hintLabels =
            search->findChildren<QLabel *>(QStringLiteral("SectionTitle"));
        for (QLabel *hint : hintLabels) {
            QWidget *host = hint->parentWidget();
            if (!host || !hint->isVisible() || !host->findChild<ComicGrid *>())
                continue;
            qWarning("blank-report: search hintH=%d hintTextH=%d",
                     hint->height(), textHeight(hint));
            break;
        }
    }

    // 设置页：所有开关行的控件与标题是否同排（垂直区间重叠）
    if (PageBase *settings = window.FindPage(PageId::Settings)) {
        const QList<SettingRow *> rows = settings->findChildren<SettingRow *>();
        int total = 0, onSameLine = 0, maxCenterDy = -1;
        for (SettingRow *row : rows) {
            if (!row->IsClickableRow())
                continue;
            ToggleSwitch *sw = row->findChild<ToggleSwitch *>();
            QLabel *title = row->findChild<QLabel *>(QStringLiteral("SettingTitle"));
            if (!sw || !title)
                continue;
            ++total;
            const QRect sg = sw->geometry(), tg = title->geometry();
            if (sg.top() < tg.bottom() && tg.top() < sg.bottom())
                ++onSameLine;
            maxCenterDy = qMax(maxCenterDy,
                               qAbs((sg.top() + sg.height() / 2) - (tg.top() + tg.height() / 2)));
        }
        qWarning("blank-report: settings toggleRows=%d sameLine=%d maxCenterDy=%d",
                 total, onSameLine, maxCenterDy);
    }
}


int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("JMComic"));
    QApplication::setOrganizationName(QStringLiteral("JMComic"));
    QApplication::setApplicationVersion(QStringLiteral("0.93"));

    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOption({QStringLiteral("page"), QStringLiteral("启动页"), QStringLiteral("name"),
                      QStringLiteral("home")});
    parser.addOption({QStringLiteral("theme"), QStringLiteral("主题"), QStringLiteral("name"),
                      QStringLiteral("dark_orange")});
    parser.addOption({QStringLiteral("shot"), QStringLiteral("截图输出路径"), QStringLiteral("png")});
    parser.addOption({QStringLiteral("shot-delay"), QStringLiteral("截图前等待毫秒数（等网络/动画）"),
                      QStringLiteral("ms"), QStringLiteral("600")});
    parser.addOption({QStringLiteral("demo-dialog"), QStringLiteral("截图用：显示一个确认框")});
    parser.addOption({QStringLiteral("sr-sheet"), QStringLiteral("截图用：打开超分模型选择浮层")});
    parser.addOption({QStringLiteral("sr-pick"),
                      QStringLiteral("回归：在图片超分页选中指定模型（等价于点选择栏→确定）"),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("sr-page-image"),
                      QStringLiteral("回归：把图片塞进图片超分页的预览区（等价于点打开图片…）"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("sr-page-run"),
                      QStringLiteral("回归：配合 --sr-page-image，直接跑一次超分（等价于点开始超分）")});
    parser.addOption({QStringLiteral("sr-sheet-scroll"),
                      QStringLiteral("配合 --sr-sheet：把浮层列表滚到指定像素（验证吸顶分组条）"),
                      QStringLiteral("px")});
    parser.addOption({QStringLiteral("scroll-end"), QStringLiteral("滚到底 N 次以触发分页"),
                      QStringLiteral("n"), QStringLiteral("1")});
    // ---- 换页动画（QML）----
    parser.addOption({QStringLiteral("no-anim"), QStringLiteral("关闭换页动画")});
    parser.addOption({QStringLiteral("win-size"), QStringLiteral("窗口尺寸 WxH（默认 1280x840）"),
                      QStringLiteral("wxh")});
    parser.addOption({QStringLiteral("quiet"),
                      QStringLiteral("静默验收：窗口不上屏、不抢焦点、不进任务栏（截图照常）")});
    parser.addOption({QStringLiteral("quiet-minimized"),
                      QStringLiteral("静默验收（最小化）：以最小化方式启动，不抢焦点")});
    parser.addOption({QStringLiteral("anim-from"), QStringLiteral("动画演示的起点页"),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("anim-progress"),
                      QStringLiteral("把动画冻结在 0..1 的进度（截图用）"), QStringLiteral("p")});
    parser.addOption({QStringLiteral("anim-ms"), QStringLiteral("动画时长（毫秒，慢放便于截图）"),
                      QStringLiteral("n")});
    parser.addOption({QStringLiteral("anim-back"), QStringLiteral("按后退方向播（旧页右移）")});
    parser.addOption({QStringLiteral("anim-fade"),
                      QStringLiteral("按同级语义播（交叉淡入，无位移；截图/回归用）")});
    // ---- 触摸适配层 ----
    parser.addOption({QStringLiteral("touch"), QStringLiteral("强制触摸模式（桌面验收用）")});
    parser.addOption({QStringLiteral("drawer"), QStringLiteral("导航改为抽屉（手机竖屏）")});
    parser.addOption({QStringLiteral("demo-drawer"), QStringLiteral("展开抽屉后截图")});
    parser.addOption({QStringLiteral("drawer-selftest"), QStringLiteral("抽屉开关自检（Esc/遮罩）")});
    parser.addOption({QStringLiteral("safe-insets"), QStringLiteral("安全区 l,t,r,b（像素）"),
                      QStringLiteral("ltrb")});
    parser.addOption({QStringLiteral("demo-edge"), QStringLiteral("把边缘手势提示画在指定进度"),
                      QStringLiteral("p")});
    parser.addOption({QStringLiteral("demo-back-swipe"), QStringLiteral("跑一次边缘返回手势")});
    // ---- 字体 ----
    parser.addOption({QStringLiteral("font-family"), QStringLiteral("自定义字体家族"),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("font-weight"), QStringLiteral("字重 100..900"),
                      QStringLiteral("w")});
    parser.addOption({QStringLiteral("font-width"), QStringLiteral("字宽 50..200（%）"),
                      QStringLiteral("pct")});
    parser.addOption({QStringLiteral("font-size"), QStringLiteral("字号 8..16（pt）"),
                      QStringLiteral("pt")});
    parser.addOption({QStringLiteral("font-reset"), QStringLiteral("恢复为跟随系统字体")});
    parser.addOption({QStringLiteral("demo-sysfont"),
                      QStringLiteral("模拟系统字体变更：Family,pt（走与真实变更相同的流程）"),
                      QStringLiteral("spec")});
    parser.addOption({QStringLiteral("open-album"), QStringLiteral("按 album id 打开详情（拉 /album）"),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("open-chapter"),
                      QStringLiteral("按 photo id 打开章节（拉 /chapter + 下载还原）"),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("real-search"), QStringLiteral("走 /search 搜关键词"),
                      QStringLiteral("kw")});
    parser.addOption({QStringLiteral("login"), QStringLiteral("真实登录：用户名:密码"),
                      QStringLiteral("u:p")});
    parser.addOption({QStringLiteral("proxy-page"), QStringLiteral("打开分流与节点页（旧名 --proxy-dialog）")});
    parser.addOption({QStringLiteral("proxy-test"),
                      QStringLiteral("打开设置并跑测速：lines|nodes；adv=展开高级抽屉；"
                                     "advmenu/advmenudemo/advapply/advclick=地区下拉回归"),
                      QStringLiteral("kind")});
    parser.addOption({QStringLiteral("proxy-selftest"),
                      QStringLiteral("排序/选优策略自检（不联网）")});
    parser.addOption({QStringLiteral("doh-test"),
                      QStringLiteral("用 DoH 解析一次域名并打印结果"), QStringLiteral("host")});
    parser.addOption({QStringLiteral("doh-on"),
                      QStringLiteral("开启 DoH 接管请求并预热当前接口域名（验收用）")});
    parser.addOption({QStringLiteral("http3-on"),
                      QStringLiteral("本次会话开启 HTTP/3（验收/回归用，不写回配置）")});
    parser.addOption({QStringLiteral("advanced-status"),
                      QStringLiteral("打印 DOH/ECH/HTTP3 页面状态文案（不联网）")});
    parser.addOption({QStringLiteral("ech-probe"),
                      QStringLiteral("用 OpenSSL 4.x 通道做一次 ECH 握手并打印结果"),
                      QStringLiteral("host")});
    parser.addOption({QStringLiteral("h3-probe"),
                      QStringLiteral("用 QUIC/HTTP3 发一个请求并打印结果"), QStringLiteral("host")});
    // 注意：下面还有一个既有的 --net-probe（同步 QEventLoop + return 0，桌面回归
    // c1_ui_desktop_suite.py 依赖它）。0.90 的直连 H3 验收入口必须用另一个名字，
    // 否则 QCommandLineParser 会报 "option already added" 且两套实现互相覆盖。
    parser.addOption({QStringLiteral("direct-h3-probe"),
                      QStringLiteral("直连验收入口：NetClient 请求，开启 HTTP/3 时先试 H3、失败回 HTTP/2"),
                      QStringLiteral("url")});
    parser.addOption({QStringLiteral("h3-path"),
                      QStringLiteral("--h3-probe 的请求路径"), QStringLiteral("/")});
    parser.addOption({QStringLiteral("h3-ech"),
                      QStringLiteral("--h3-probe 同时注入 ECHConfigList（ECH + HTTP/3）")});
    parser.addOption({QStringLiteral("h3-burst"),
                      QStringLiteral("并发 N 个同域 HTTP/3 请求，验证连接池复用"),
                      QStringLiteral("count")});
    parser.addOption({QStringLiteral("h3-bench"),
                      QStringLiteral("串行 N 次全新握手，输出 P50/P95（Q3 采样）"),
                      QStringLiteral("count")});
    parser.addOption({QStringLiteral("h3-fail-inject"),
                      QStringLiteral("H3 失败注入：bad-port / drop-first / ech-reject"
                                     "（ech-reject 模拟服务端拒绝 ECH-over-QUIC）"),
                      QStringLiteral("mode")});
    parser.addOption({QStringLiteral("ech-h3-selftest"),
                      QStringLiteral("自检 Q4 的进程内抑制：按 (host,节点) 生效、"
                                     "换节点/重置后重新尝试 H3+ECH 组合")});
    parser.addOption({QStringLiteral("ech-h3-probe"),
                      QStringLiteral("走 NodeHttp 业务路径连发两次 GET，验证 ECH/H3 "
                                     "被拒后回退 TCP+ECH 且本进程不再重试组合"),
                      QStringLiteral("url")});
    parser.addOption({QStringLiteral("scroll-bottom"),
                      QStringLiteral("截图前把当前页所有可见滚动区滚到底（长页面验收）")});
    parser.addOption({QStringLiteral("use-node"),
                      QStringLiteral("强制走 CDN 线路并选中该节点 IP（验证真实流量走节点）"),
                      QStringLiteral("ip")});
    parser.addOption({QStringLiteral("latest-more"),
                      QStringLiteral("真实首页加载第 2 页（/latest?page=1）")});
    // ---- 本轮修复的验收入口（见 docs/cpp_ui_fix_round_2026-11-08.md）----
    parser.addOption({QStringLiteral("strip-drag"),
                      QStringLiteral("首页分区栏：合成一次横向拖动（像素）"),
                      QStringLiteral("dx")});
    parser.addOption({QStringLiteral("strip-tap"),
                      QStringLiteral("首页分区栏：合成一次点按（可视 x 坐标）"),
                      QStringLiteral("x")});
    parser.addOption({QStringLiteral("home-swipe"),
                      QStringLiteral("首页：合成一次整页横滑（dx，负=下一个分区）"),
                      QStringLiteral("dx")});
    parser.addOption({QStringLiteral("home-freeze"),
                      QStringLiteral("首页：冻结在分区 from→to 的进度 0..1（截图用）"),
                      QStringLiteral("p")});
    parser.addOption({QStringLiteral("home-freeze-from"), QStringLiteral("冻结起点分区（0 起）"),
                      QStringLiteral("n")});
    parser.addOption({QStringLiteral("home-freeze-to"), QStringLiteral("冻结目标分区（0 起）"),
                      QStringLiteral("n")});
    parser.addOption({QStringLiteral("sign-demo"), QStringLiteral("点一次侧栏打卡")});
    parser.addOption({QStringLiteral("nav-action"),
                      QStringLiteral("点一条侧栏动作行：proxy|proxyImg|hide"),
                      QStringLiteral("id")});
    parser.addOption({QStringLiteral("nav-report"),
                      QStringLiteral("打印侧栏可交互行数量")});
    // ---- 账号数据（登录后侧栏 / 我的收藏）----
    parser.addOption({QStringLiteral("user-report"),
                      QStringLiteral("打印侧栏账号数据与收藏页状态（一行，ASCII 键名）")});
    parser.addOption({QStringLiteral("cache-dir"), QStringLiteral("设置自定义缓存目录"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("cache-reset"), QStringLiteral("缓存目录恢复默认")});
    parser.addOption({QStringLiteral("cache-status"), QStringLiteral("打印缓存目录解析结果")});
    parser.addOption({QStringLiteral("reader-mode"),
                      QStringLiteral("阅读模式 0~8（与 Python ReadMode 同序）"),
                      QStringLiteral("n")});
    parser.addOption({QStringLiteral("reader-dir"),
                      QStringLiteral("把本地目录加载到阅读器（回归用）"), QStringLiteral("path")});
    parser.addOption({QStringLiteral("reader-parity"),
                      QStringLiteral("跑阅读器 9 模式 + 滑动/双指自检")});
    parser.addOption({QStringLiteral("reader-touch-selftest"),
                      QStringLiteral("用真实 QTouchEvent 验证滑动/双指")});
    parser.addOption({QStringLiteral("reader-swipe"),
                      QStringLiteral("合成一次滑动：dx[,dy]"), QStringLiteral("dx[,dy]")});
    parser.addOption({QStringLiteral("reader-pinch"),
                      QStringLiteral("合成一次双指缩放：目标倍率"), QStringLiteral("ratio")});
    parser.addOption({QStringLiteral("reader-fit"),
                      QStringLiteral("缩放后点一次标题栏\"1:1\"按钮（验证恢复原尺寸）")});
    parser.addOption({QStringLiteral("reader-progressive"),
                      QStringLiteral("渐进加载自检：用该目录扮演逐页到达"), QStringLiteral("dir")});
    parser.addOption({QStringLiteral("reader-fullscreen"),
                      QStringLiteral("阅读器全屏（移动端验收：系统栏应隐藏）")});
    parser.addOption({QStringLiteral("tapguard-selftest"),
                      QStringLiteral("菜单栏防误触自检：滑动不跳页 / 点击照常生效")});
    parser.addOption({QStringLiteral("drawer-nav"),
                      QStringLiteral("抽屉打开后点一次导航项（验证换页动画）"), QStringLiteral("page")});
    parser.addOption({QStringLiteral("sr-selftest"),
                      QStringLiteral("超分自检（可选输出 png）"), QStringLiteral("png"),
                      QStringLiteral("")});
    // ---- 超分模型目录（本轮修复：模型不再只有三个）----
    parser.addOption({QStringLiteral("sr-models"),
                      QStringLiteral("打印超分模型目录（含未安装项）并退出")});
    parser.addOption({QStringLiteral("sr-model-test"),
                      QStringLiteral("对每个已安装模型各跑一次超分（输出目录）"),
                      QStringLiteral("dir"), QStringLiteral("")});
    parser.addOption({QStringLiteral("sr-model"),
                      QStringLiteral("指定超分模型（显示名，可与 --sr-model-test 组合）"),
                      QStringLiteral("name")});
    parser.addOption({QStringLiteral("sr-probe"),
                      QStringLiteral("--sr-model-test 的探针图片尺寸 WxH（默认 96x96）"),
                      QStringLiteral("wxh"), QStringLiteral("96x96")});
    // ---- 下载队列（本轮修复：入口/进度/中断恢复）----
    parser.addOption({QStringLiteral("img-host"),
                      QStringLiteral("强制图床 host 顺序（逗号分隔，验服用）"),
                      QStringLiteral("hosts")});
    parser.addOption({QStringLiteral("dl-report"),
                      QStringLiteral("打印下载队列状态并继续（配合 --shot 截图）")});
    parser.addOption({QStringLiteral("dl-selftest"),
                      QStringLiteral("按 album id 拉详情并把前 N 话下完（无人值守验收入口）"),
                      QStringLiteral("albumId")});
    parser.addOption({QStringLiteral("dl-chapters"),
                      QStringLiteral("--dl-selftest 下载前 N 话（默认 1）"),
                      QStringLiteral("n"), QStringLiteral("1")});
    parser.addOption({QStringLiteral("dl-timeout"),
                      QStringLiteral("--dl-selftest 等待超时（秒，默认 180）"),
                      QStringLiteral("sec"), QStringLiteral("180")});
    parser.addOption({QStringLiteral("reader-sr"),
                      QStringLiteral("对当前页发起超分（配合 --shot-delay 等结果）")});
    parser.addOption({QStringLiteral("sr-upscale"),
                      QStringLiteral("直接超分一张图：in.png,out.png"), QStringLiteral("in,out")});
    parser.addOption({QStringLiteral("reader-sr-status"),
                      QStringLiteral("打印当前页超分状态")});
    parser.addOption({QStringLiteral("font-status"), QStringLiteral("打印当前字体解析结果与 qss 缩放")});
    parser.addOption({QStringLiteral("settings-cache"),
                      QStringLiteral("在设置页直接切换缓存目录（等价于点“选择…”）"),
                      QStringLiteral("path")});
    parser.addOption({QStringLiteral("webp-probe"),
                      QStringLiteral("解码一个 webp/png 文件并打印结果（验证内嵌解码器）"),
                      QStringLiteral("file")});
    parser.addOption({QStringLiteral("home-geom"), QStringLiteral("打印首页几何")});
    parser.addOption({QStringLiteral("layout-dump"),
                      QStringLiteral("打印当前页控件树的几何（可选深度，默认 3）"),
                      QStringLiteral("depth"), QStringLiteral("3")});
    parser.addOption({QStringLiteral("blank-report"),
                      QStringLiteral("打印首页/搜索页留白高度与设置页开关排布（量化验收）")});
    parser.addOption({QStringLiteral("path-report"),
                      QStringLiteral("打印配置/数据/缓存目录与 session.ini、ui.ini 的实际落盘位置")});
    parser.addOption({QStringLiteral("data-report"),
                      QStringLiteral("打印登录态与个性化配置是否已从磁盘恢复（升级验服用）")});
    parser.addOption({QStringLiteral("history-clear"),
                      QStringLiteral("清空最近搜索（等价设置页「清空」文字链，验服用）")});
    parser.addOption({QStringLiteral("scroll-report"),
                      QStringLiteral("打印当前页可见滚动区的横/纵向范围（手机端应为 hmax=0）")});
    parser.addOption({QStringLiteral("toggle-test"),
                      QStringLiteral("回归：点设置页某一行的标题区，验证整行可切换开关"),
                      QStringLiteral("title")});
    // 兼容旧名（原来只在 isSet 里用，QCommandLineParser 会对未注册的名字报警）
    parser.addOption({QStringLiteral("proxy-dialog"), QStringLiteral("--proxy-page 的旧名")});
    parser.addOption({QStringLiteral("demo-collapse"), QStringLiteral("收起侧栏后截图")});
    parser.addOption({QStringLiteral("demo-nav-end"), QStringLiteral("侧栏滚到底后截图")});
    parser.addOption({QStringLiteral("home-section"), QStringLiteral("首页切到第 N 个分区（0 起）"),
                      QStringLiteral("n"), QStringLiteral("0")});
    parser.addOption({QStringLiteral("demo-reader-tap"),
                      QStringLiteral("阅读页合成一次点按 fx,fy（比例坐标）"),
                      QStringLiteral("xy")});
    parser.addOption({QStringLiteral("demo-reader-menu"),
                      QStringLiteral("阅读页弹开右上角菜单后截图")});
    parser.addOption({QStringLiteral("demo-drag"),
                      QStringLiteral("合成一次边缘拖拽（像素），验证手势状态机"),
                      QStringLiteral("dx")});
    parser.addOption({QStringLiteral("net-probe"), QStringLiteral("发一个 GET 并打印结果"),
                      QStringLiteral("url")});
    parser.process(app);

    if (parser.isSet(QStringLiteral("sr-models"))) {
        const QVector<SrEngine::ModelInfo> all = SrEngine::Catalog();
        int installed = 0;
        for (const SrEngine::ModelInfo &m : all) {
            installed += m.installed ? 1 : 0;
            std::fprintf(stderr,
                         "sr-model-item: name=%s file=%s arch=%s scale=%d denoise=%d tile=%d "
                         "installed=%d bytes=%lld\n",
                         qPrintable(m.name), qPrintable(m.file), qPrintable(m.arch), m.scale,
                         m.denoise, m.tile, int(m.installed), static_cast<long long>(m.bytes));
        }
        std::fprintf(stderr, "sr-models: total=%d installed=%d dir=%s search=%s runtime=%s\n",
                     int(all.size()), installed, qPrintable(SrEngine::ModelsDir()),
                     qPrintable(SrEngine::ModelSearchPath()),
                     qPrintable(SrEngine::RuntimePath()));
        std::fprintf(stderr, "sr-models: status=%s\n", qPrintable(SrEngine::Status()));
        const bool ok = SrEngine::Available();
        SrEngine::Shutdown();
        return ok ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("sr-model"))) {
        const QString want = parser.value(QStringLiteral("sr-model"));
        const bool ok = SrEngine::SetModel(want);
        std::fprintf(stderr, "sr-model: set %s ok=%d status=%s\n", qPrintable(want), int(ok),
                     qPrintable(SrEngine::Status()));
    }

    if (parser.isSet(QStringLiteral("sr-model-test"))) {
        const QStringList wh = parser.value(QStringLiteral("sr-probe")).split(QLatin1Char('x'));
        const QSize probe(qMax(32, wh.value(0).toInt()), qMax(32, wh.value(1).toInt()));
        QStringList only;
        if (parser.isSet(QStringLiteral("sr-model")))
            only = parser.value(QStringLiteral("sr-model")).split(QLatin1Char(','),
                                                                  Qt::SkipEmptyParts);
        QString report;
        const bool ok = SrEngine::ModelSweep(only, probe, parser.value(QStringLiteral("sr-model-test")),
                                             &report);
        SrEngine::Shutdown();
        return ok ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("sr-upscale"))) {
        const QStringList io = parser.value(QStringLiteral("sr-upscale")).split(QLatin1Char(','));
        QImage in(io.value(0));
        if (in.isNull()) {
            std::fprintf(stderr, "sr-upscale: cannot read %s\n", qPrintable(io.value(0)));
            return 2;
        }
        const SrEngine::Result r = SrEngine::Upscale(in);
        if (r.image.isNull()) {
            std::fprintf(stderr, "sr-upscale: FAIL %s\n", qPrintable(r.error));
            SrEngine::Shutdown();
            return 2;
        }
        r.image.save(io.value(1));
        std::fprintf(stderr, "sr-upscale: %dx%d -> %dx%d %.0fms -> %s\n", in.width(), in.height(),
                     r.image.width(), r.image.height(), r.ms, qPrintable(io.value(1)));
        SrEngine::Shutdown();
        return 0;
    }

    if (parser.isSet(QStringLiteral("sr-selftest"))) {
        QString report;
        QImage out;
        const bool ok = SrEngine::SelfTest(&report, &out);
        std::fprintf(stderr, "%s\n", qPrintable(report));
        std::fflush(stderr);
        const QString path = parser.value(QStringLiteral("sr-selftest"));
        if (!path.isEmpty() && !out.isNull()) {
            out.save(path);
            std::fprintf(stderr, "sr-selftest: saved %s\n", qPrintable(path));
        }
        return ok ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("net-probe"))) {
        QNetworkAccessManager nam;
        QNetworkRequest req{QUrl(parser.value(QStringLiteral("net-probe")))};
        QNetworkReply *reply = nam.get(req);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        QTimer::singleShot(6000, &loop, &QEventLoop::quit);
        loop.exec();
        std::fprintf(stderr, "net-probe: finished=%d error=%d (%s) bytes=%lld\n",
                     int(reply->isFinished()), int(reply->error()),
                     qPrintable(reply->errorString()), (long long)reply->bytesAvailable());
        return 0;
    }

    JmLog::Init();   // 桌面无控制台：先把 qWarning 落到文件

    // 验收入口必须在 data-report 之前执行，否则报告读到的是清空前的旧值。
    if (parser.isSet(QStringLiteral("history-clear")))
        SearchHistory::Clear();

    // 落盘路径诊断：Android 上这一步能直接看出配置目录是否可写。
    if (parser.isSet(QStringLiteral("path-report"))) {
        const QStringList lines = AppPaths::Report().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines)
            qWarning("%s", line.toUtf8().constData());
    }

    // 字体先就位：主题要按当前字号算 qss 缩放比
    AppFont::I().Init();
    // 会话先恢复：侧栏用户区在构造时就会读 UserSession
    SessionStore::Load();

    // 主题：命令行显式指定优先；否则用上次在设置页选的（原来每次启动都回到默认，
    // 用户感知就是"升级/重启后个性化设置没了"）。
    const QString themeName =
        parser.isSet(QStringLiteral("theme"))
            ? parser.value(QStringLiteral("theme"))
            : UiPrefs::GetString(QStringLiteral("theme"), parser.value(QStringLiteral("theme")));
    UiTheme::I().Apply(themeName);

    // 升级验收：登录态 / 个性化设置是否真的从磁盘恢复（而不是本次运行的默认值）。
    if (parser.isSet(QStringLiteral("data-report"))) {
        const UserState &u = UserSession::User();
        qWarning("data-report: loggedIn=%d name=%s uid=%lld avsLen=%d proxy=%s hideWords=%d",
                 int(u.loggedIn), qPrintable(u.name), (long long)u.uid, int(u.avs.size()),
                 qPrintable(u.proxy), int(u.hideWordList.size()));
        qWarning("data-report: fontCustom=%d family=%s pt=%.1f cacheRoot=%s",
                 int(AppFont::I().UsingCustom()), qPrintable(AppFont::I().ResolvedFamily()),
                 AppFont::I().EffectivePointSize(), qPrintable(CachePath::Root()));
        qWarning("data-report: sessionFile=%s exists=%d uiIni=%s exists=%d",
                 qPrintable(SessionStore::Path()),
                 QFileInfo::exists(SessionStore::Path()) ? 1 : 0,
                 qPrintable(CachePath::UiIniPath()),
                 QFileInfo::exists(CachePath::UiIniPath()) ? 1 : 0);
        qWarning("data-report: prefs %s themeNow=%s", qPrintable(UiPrefs::Report()),
                 qPrintable(UiTheme::I().Name()));
        qWarning("data-report: %s", qPrintable(SearchHistory::Report()));
    }

    if (parser.isSet(QStringLiteral("font-reset")))
        AppFont::I().ResetToSystem();

    if (parser.isSet(QStringLiteral("font-family")) || parser.isSet(QStringLiteral("font-weight"))
        || parser.isSet(QStringLiteral("font-width")) || parser.isSet(QStringLiteral("font-size"))) {
        AppFont::I().SetSource(AppFont::Source::Custom);
        if (parser.isSet(QStringLiteral("font-family")))
            AppFont::I().SetFamily(parser.value(QStringLiteral("font-family")));
        if (parser.isSet(QStringLiteral("font-weight")))
            AppFont::I().SetWeight(parser.value(QStringLiteral("font-weight")).toInt());
        if (parser.isSet(QStringLiteral("font-width")))
            AppFont::I().SetWidth(parser.value(QStringLiteral("font-width")).toInt());
        if (parser.isSet(QStringLiteral("font-size")))
            AppFont::I().SetPointSize(parser.value(QStringLiteral("font-size")).toInt());
    }

    // 主题已在上面的 data-report 之前应用（那里要打印生效值），此处不再重复。

    // --touch 必须在建页之前生效：页面在构造期就按手机形态排版
    // （页边距/页头、详情页的上下堆叠都只在构造期判定一次）。
    if (parser.isSet(QStringLiteral("touch")))
        Touch::SetTouchMode(true);

    MainWindow window;
    if (parser.isSet(QStringLiteral("win-size"))) {
        const QStringList wh = parser.value(QStringLiteral("win-size")).split(QLatin1Char('x'));
        if (wh.size() == 2)
            window.resize(qMax(320, wh[0].toInt()), qMax(480, wh[1].toInt()));
    }
    const QuietMode quietMode = QuietModeFrom(parser);
    QuietGuard quietGuard;      // 栈上对象：事件循环期间一直有效
    if (quietMode != QuietMode::Off)
        app.installEventFilter(&quietGuard);
    if (quietMode == QuietMode::Off) {
        window.show();
    } else {
        // 静默模式：窗口不上屏也不抢焦点，但布局/绘制/截图全部照常
        ShowQuiet(window, quietMode);
        std::fprintf(stderr, "quiet: mode=%s hwnd_offscreen=%d\n",
                     quietMode == QuietMode::Minimized ? "minimized" : "hidden",
                     quietMode == QuietMode::Minimized ? 0 : 1);
    }

    // 触摸层：Android 上默认开；桌面用 --touch 强制打开，这样桌面就能验收
    // 边缘手势、手势滚动、44dp 命中目标这些"只有手指才会触发"的行为。
    if (parser.isSet(QStringLiteral("touch")))
        window.EnableTouchMode(true);
    // --demo-drawer 也要先切抽屉模式：抽屉模式会把窗口下限降到 320×480，
    // 否则后面的 --win-size 420x860 先被桌面模式的 980 下限夹掉。
    if (parser.isSet(QStringLiteral("drawer")) || parser.isSet(QStringLiteral("demo-drawer"))
        || parser.isSet(QStringLiteral("drawer-selftest")))
        window.EnableDrawerMode(true);
    // 抽屉模式会把最小尺寸改成 320×480；窗口尺寸必须在这之后再应用一次，
    // 否则 --win-size 420x860 会先被桌面模式的 980 下限夹掉。
    if (parser.isSet(QStringLiteral("win-size"))) {
        const QStringList wh = parser.value(QStringLiteral("win-size")).split(QLatin1Char('x'));
        if (wh.size() == 2)
            window.resize(qMax(320, wh[0].toInt()), qMax(480, wh[1].toInt()));
    }
    if (parser.isSet(QStringLiteral("safe-insets"))) {
        const QStringList parts = parser.value(QStringLiteral("safe-insets")).split(QLatin1Char(','));
        if (parts.size() == 4) {
            window.SetSafeAreaOverride(parts[0].toInt(), parts[1].toInt(), parts[2].toInt(),
                                       parts[3].toInt());
        }
    }

    const PageId page = PageFromName(parser.value(QStringLiteral("page")));
    const bool demoAnim = parser.isSet(QStringLiteral("anim-from"))
                          || parser.isSet(QStringLiteral("anim-progress"));
    if (demoAnim) {
        const PageId from = parser.isSet(QStringLiteral("anim-from"))
                                ? PageFromName(parser.value(QStringLiteral("anim-from")))
                                : PageId::Home;
        const qreal p = parser.isSet(QStringLiteral("anim-progress"))
                            ? parser.value(QStringLiteral("anim-progress")).toDouble()
                            : -1.0;
        const int forceMode = parser.isSet(QStringLiteral("anim-fade"))
                                  ? 2
                                  : (parser.isSet(QStringLiteral("anim-back")) ? 1 : 0);
        window.DemoTransition(from, page, p, parser.value(QStringLiteral("anim-ms")).toInt(),
                              parser.isSet(QStringLiteral("anim-back")), forceMode);
    } else {
        // 启动落页不该播动画（用户没"操作"，播了反而像卡顿）
        window.SetAnimationEnabled(false);
        if (page != PageId::Home)
            window.Go(page, false);
        window.SetAnimationEnabled(!parser.isSet(QStringLiteral("no-anim")));
    }

    // 展开抽屉放在落页之后：几何基于最终窗口尺寸，也能拍到"某个页面 + 抽屉展开"
    if (parser.isSet(QStringLiteral("drawer-selftest"))) {
        const bool ok = window.DrawerSelfTest();
        return ok ? 0 : 1;
    }
    if (parser.isSet(QStringLiteral("demo-drawer")))
        window.DemoOpenDrawer();

    if (parser.isSet(QStringLiteral("demo-edge")))
        window.ShowEdgeHint(parser.value(QStringLiteral("demo-edge")).toDouble());

    // 直连 H3 验收入口：不经 JmApi 的节点/DoH 计划，创建一个干净的 NetClient，
    // 这样 --http3-on 时无论设备配置如何都会走"先 H3、失败回 QNAM/HTTP2"的新路径。
    auto runNetProbe = [&window](const QString &urlText) {
        auto *client = new NetClient(&window);
        QObject::connect(client, &NetClient::Finished, client, [client](const NetResult &r) {
            qWarning("direct-h3-probe: %s", qPrintable(r.Summary()));
            client->deleteLater();
        });
        client->Get(QUrl(urlText), 12000);
    };

    // Q4 纯逻辑自检：不碰网络，验证「抑制按 (host, 节点) 生效」与
    // 「换节点 / 重置后重新尝试」。冷启动复位由 --advanced-status 里的
    // echH3Blocked=0 断言（状态只存内存，不落盘）。
    auto runEchH3SelfTest = [] {
        const QString host = QStringLiteral("selftest.example");
        const QString nodeA = QStringLiteral("1.1.1.1");
        const QString nodeB = QStringLiteral("2.2.2.2");
        const QString savedNode = ProxyConfig::SelectedIp();
        ProxyConfig::ResetEchH3Blocks();
        const int initial = ProxyConfig::EchH3BlockCount();
        const bool before = ProxyConfig::EchH3Blocked(host, nodeA);
        ProxyConfig::NoteEchH3Rejected(host, nodeA);
        const bool after = ProxyConfig::EchH3Blocked(host, nodeA);
        const bool otherNode = ProxyConfig::EchH3Blocked(host, nodeB);
        const int count = ProxyConfig::EchH3BlockCount();
        ProxyConfig::SetSelectedIp(nodeB);   // 换节点 = 显式重试
        const bool afterNodeChange = ProxyConfig::EchH3Blocked(host, nodeA);
        const int countAfter = ProxyConfig::EchH3BlockCount();
        ProxyConfig::SetSelectedIp(savedNode);
        const bool pass = initial == 0 && !before && after && !otherNode && count == 1
                          && !afterNodeChange && countAfter == 0;
        qWarning("ech-h3-selftest: initial=%d blockedBefore=%d blockedAfter=%d "
                 "otherNode=%d count=%d afterNodeChange=%d countAfter=%d -> %s",
                 initial, int(before), int(after), int(otherNode), count,
                 int(afterNodeChange), countAfter, pass ? "PASS" : "FAIL");
    };

    // Q4 业务路径探针：故意走 NodeHttp（非 quicOnly），因为只有这条链路
    // 有 TCP+ECH 可回退。连发两次：第一次验证「策略拒绝 → TCP+ECH」，
    // 第二次验证「本进程内暂时不再尝试组合」。配合 --h3-fail-inject ech-reject
    // 可完全离线、确定性地复跑。
    auto runEchH3Probe = [&window](const QString &urlText) {
        auto *nh = new NodeHttp(&window);
        QObject::connect(nh, &NodeHttp::Log, nh, [](const QString &l) {
            qWarning("ech-h3-probe: %s", qPrintable(l));
        });
        auto step = std::make_shared<std::function<void(int)>>();
        *step = [nh, urlText, step](int pass) {
            if (pass > 2) {
                qWarning("ech-h3-probe: finished echH3Blocked=%d",
                         ProxyConfig::EchH3BlockCount());
                nh->deleteLater();
                return;
            }
            NodeHttpRequest hr;
            hr.url = QUrl(urlText);
            hr.timeoutMs = 12000;
            nh->Send(hr, [nh, pass, step](const NodeHttpResponse &r) {
                qWarning("ech-h3-probe: pass=%d ok=%d status=%d bytes=%lld err=%s "
                         "echH3Blocked=%d",
                         pass, int(r.ok), r.status,
                         static_cast<long long>(r.body.size()),
                         r.error.toUtf8().constData(),
                         ProxyConfig::EchH3BlockCount());
                (*step)(pass + 1);
            });
        };
        (*step)(1);
    };
#ifdef Q_OS_ANDROID
    // 启动即拉线上首页（/latest + /promote）；失败时首页进入错误态，可点重试。
    // 桌面端在 #else 分支同样默认加载，自动化截图靠 --shot-delay 等网络。
    window.LoadRealData();

    // 调试/自动化：intent extra 里带参数时按同一套 CLI 语义执行
    const QString jmArgs = AndroidExtra("jm.args");
    if (!jmArgs.isEmpty()) {
        // 先收集再执行。0.89 是"边扫边执行"，所以 --h3-burst 写在 --h3-probe
        // 之前时拿不到 host（静默退化成默认域名），--h3-path 同理。改成两段式后
        // 参数顺序不再影响结果。
        const QStringList parts = jmArgs.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        QHash<QString, QString> opt;
        QStringList flags;
        const QStringList takesValue = {
            QStringLiteral("--page"),
            QStringLiteral("--shot"),          QStringLiteral("--shot-delay"),
            QStringLiteral("--open-album"),     QStringLiteral("--open-chapter"),
            QStringLiteral("--real-search"),    QStringLiteral("--login"),
            QStringLiteral("--h3-probe"),       QStringLiteral("--h3-path"),
            QStringLiteral("--h3-burst"),       QStringLiteral("--h3-bench"),
            QStringLiteral("--h3-fail-inject"), QStringLiteral("--use-node"),
            QStringLiteral("--ech-probe"),      QStringLiteral("--direct-h3-probe"),
            QStringLiteral("--ech-h3-probe"),   QStringLiteral("--font-size"),
            QStringLiteral("--font-weight"),    QStringLiteral("--font-family"),
            QStringLiteral("--font-width"),     QStringLiteral("--reader-mode"),
            QStringLiteral("--reader-dir"),     QStringLiteral("--reader-progressive"),
            QStringLiteral("--layout-dump"),    QStringLiteral("--drawer-nav"),
        };
        for (int i = 0; i < parts.size(); ++i) {
            const QString &a = parts.at(i);
            if (takesValue.contains(a)) {
                if (i + 1 < parts.size())
                    opt.insert(a, parts.at(i + 1));
                ++i;
            } else {
                flags << a;
            }
        }

        // UI 收尾回归：页面直达 + 与桌面同义的布局量测（日志里读三行 blank-report）
        if (opt.contains(QStringLiteral("--page")))
            window.Go(PageFromName(opt.value(QStringLiteral("--page"))), false);
        // 真机截图：手机上 screencap 会被 FLAG_SECURE 拦掉，只能用应用自己的 grab。
        // 路径建议写 /sdcard/Android/data/<pkg>/files/x.png（外部私有目录，adb pull 可读）。
        if (opt.contains(QStringLiteral("--shot")))
            window.ShotTo(opt.value(QStringLiteral("--shot")),
                          qMax(0, opt.value(QStringLiteral("--shot-delay")).toInt()));
        // 真机验收：抽屉里点导航项（换页动画）/ 阅读器全屏（系统栏是否让位）
        if (opt.contains(QStringLiteral("--drawer-nav"))) {
            const PageId target = PageFromName(opt.value(QStringLiteral("--drawer-nav")));
            QTimer::singleShot(400, &window,
                               [&window, target] { window.DrawerNavForTest(target); });
        }
        if (flags.contains(QStringLiteral("--reader-fullscreen")))
            QTimer::singleShot(900, &window, [&window] { window.ToggleReaderFullScreen(); });
        if (flags.contains(QStringLiteral("--blank-report")))
            QTimer::singleShot(1200, &window, [&window] { ReportBlankBands(window); });
        // 真机布局体检：与桌面同一条 --layout-dump，宽度/最小宽度一起打
        if (opt.contains(QStringLiteral("--layout-dump"))) {
            const int depth = qBound(1, opt.value(QStringLiteral("--layout-dump")).toInt(), 8);
            QTimer::singleShot(900, &window, [&window, depth] {
                if (PageBase *p = window.FindPage(window.CurrentPage()))
                    DumpLayoutTree(p, 0, depth);
            });
        }

        // 升级/落盘验收：与桌面同名参数，手机端也能直接量测。
        // 这三个开关存在的意义：把"看起来登录了"和"真的写进磁盘了"区分开。
        if (flags.contains(QStringLiteral("--history-clear")))
            SearchHistory::Clear();
        if (flags.contains(QStringLiteral("--path-report"))) {
            const QStringList lines = AppPaths::Report().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &line : lines)
                qWarning("%s", line.toUtf8().constData());
        }
        if (flags.contains(QStringLiteral("--data-report"))) {
            const UserState &u = UserSession::User();
            qWarning("data-report: loggedIn=%d name=%s uid=%lld avsLen=%d proxy=%s hideWords=%d",
                     int(u.loggedIn), qPrintable(u.name), (long long)u.uid, int(u.avs.size()),
                     qPrintable(u.proxy), int(u.hideWordList.size()));
            qWarning("data-report: fontCustom=%d family=%s pt=%.1f cacheRoot=%s",
                     int(AppFont::I().UsingCustom()), qPrintable(AppFont::I().ResolvedFamily()),
                     AppFont::I().EffectivePointSize(), qPrintable(CachePath::Root()));
            qWarning("data-report: sessionFile=%s exists=%d",
                     qPrintable(SessionStore::Path()),
                     QFileInfo::exists(SessionStore::Path()) ? 1 : 0);
            qWarning("data-report: prefs %s themeNow=%s", qPrintable(UiPrefs::Report()),
                     qPrintable(UiTheme::I().Name()));
            qWarning("data-report: %s", qPrintable(SearchHistory::Report()));
        }
        // 字体三轴在真机上的 A/B 验收：语义与桌面完全一致——只要给了任一字体参数，
        // 就切到"自定义"来源（跟随系统模式下 Build() 会整段忽略 weight/width）。
        if (opt.contains(QStringLiteral("--font-family")) || opt.contains(QStringLiteral("--font-weight"))
            || opt.contains(QStringLiteral("--font-width"))
            || opt.contains(QStringLiteral("--font-size"))) {
            AppFont::I().SetSource(AppFont::Source::Custom);
        }
        if (opt.contains(QStringLiteral("--font-family")))
            AppFont::I().SetFamily(opt.value(QStringLiteral("--font-family")));
        if (opt.contains(QStringLiteral("--font-width")))
            AppFont::I().SetWidth(opt.value(QStringLiteral("--font-width")).toInt());
        if (opt.contains(QStringLiteral("--font-weight")))
            AppFont::I().SetWeight(opt.value(QStringLiteral("--font-weight")).toInt());
        if (opt.contains(QStringLiteral("--font-size")))
            AppFont::I().SetPointSize(opt.value(QStringLiteral("--font-size")).toInt());

        const QString h3Host = opt.value(QStringLiteral("--h3-probe"));
        const QString h3Path = opt.value(QStringLiteral("--h3-path"), QStringLiteral("/"));

        // 会话级开关先落地：后面的探针与业务请求才看得到
        if (flags.contains(QStringLiteral("--http3-on")))
            ProxyConfig::SetHttp3Enabled(true);   // 仅本会话
        if (flags.contains(QStringLiteral("--doh-on"))) {
            // 与桌面 --doh-on 同义，但**不 Save**：验收不该污染用户配置
            ProxyConfig::Load();
            ProxyConfig::SetDohEnabled(true);
            ProxyConfig::SyncDohClient();
        }
        if (opt.contains(QStringLiteral("--h3-fail-inject")))
            ProxyConfig::SetH3FailInject(opt.value(QStringLiteral("--h3-fail-inject")));

        if (opt.contains(QStringLiteral("--direct-h3-probe")))
            runNetProbe(opt.value(QStringLiteral("--direct-h3-probe")));
        if (opt.contains(QStringLiteral("--ech-h3-probe")))
            runEchH3Probe(opt.value(QStringLiteral("--ech-h3-probe")));

        if (opt.contains(QStringLiteral("--use-node")))
            window.UseNodeForTest(opt.value(QStringLiteral("--use-node")));
        if (opt.contains(QStringLiteral("--open-album")))
            window.OpenAlbumById(opt.value(QStringLiteral("--open-album")));
        if (opt.contains(QStringLiteral("--open-chapter"))) {
            const QString cid = opt.value(QStringLiteral("--open-chapter"));
            window.OpenChapterById(cid, QStringLiteral("在线章节 %1").arg(cid));
        }
        if (opt.contains(QStringLiteral("--real-search")))
            window.RealSearch(opt.value(QStringLiteral("--real-search")));
        if (opt.contains(QStringLiteral("--login"))) {
            // 与桌面路径同一套语义：user:pass，冒号前是账号。原来这里把
            // "user:pass" 整串当用户名、密码传空，Android 上永远登不进去。
            const QString spec = opt.value(QStringLiteral("--login"));
            const int colon = spec.indexOf(QLatin1Char(':'));
            if (colon > 0)
                window.RealLogin(spec.left(colon), spec.mid(colon + 1));
            else
                window.RealLogin(spec, QString());
        }
        if (flags.contains(QStringLiteral("--user-report")))
            QTimer::singleShot(1500, &window,
                               [&window] { qWarning("user-report: %s", qPrintable(window.UserReport())); });
        if (flags.contains(QStringLiteral("--latest-more")))
            QTimer::singleShot(1500, &window, [&window] { window.LoadMoreLatest(); });
        // --proxy-selftest 不在 Android 上接：它是同步自检，启动期跑会把 UI 初始化
        // 堵住（实测进程活着但一帧日志都没有）。桌面用它做离网回归，手机不需要。
        if (flags.contains(QStringLiteral("--advanced-status")))
            qWarning("advanced-status: %s", qPrintable(ProxyConfig::AdvancedStatus()));
        if (flags.contains(QStringLiteral("--ech-h3-selftest")))
            runEchH3SelfTest();

        // 探针类互斥，按信息量从大到小只跑一个：同时开会抢同一个事件循环，
        // 结果互相干扰（0.89 的 burst+probe 同跑就是这样）。
        const QString benchHost =
            h3Host.isEmpty() ? QStringLiteral("cloudflare.com") : h3Host;
        if (opt.contains(QStringLiteral("--h3-bench"))) {
            bool okCount = false;
            const int n = opt.value(QStringLiteral("--h3-bench")).toInt(&okCount);
            window.H3Bench(benchHost, h3Path, okCount ? n : 10);
        } else if (opt.contains(QStringLiteral("--h3-burst"))) {
            bool okCount = false;
            const int n = opt.value(QStringLiteral("--h3-burst")).toInt(&okCount);
            window.H3Burst(benchHost, h3Path, okCount ? n : 10);
        } else if (!h3Host.isEmpty()) {
            window.H3Probe(h3Host, h3Path, flags.contains(QStringLiteral("--h3-ech")));
        } else if (opt.contains(QStringLiteral("--ech-probe"))) {
            window.EchProbe(opt.value(QStringLiteral("--ech-probe")));
        }
    }
#else
    // 桌面与手机一致：启动即拉线上首页；失败进入错误态（不保留任何样例数据）。
    window.LoadRealData();
#endif
    if (parser.isSet(QStringLiteral("proxy-page")) || parser.isSet(QStringLiteral("proxy-dialog")))
        window.OpenProxyPage();
    if (parser.isSet(QStringLiteral("proxy-test")))
        window.DemoProxyTest(parser.value(QStringLiteral("proxy-test")));
    if (parser.isSet(QStringLiteral("proxy-selftest")))
        window.ProxySelfTest();
    if (parser.isSet(QStringLiteral("advanced-status")))
        qWarning("advanced-status: %s", qPrintable(ProxyConfig::AdvancedStatus()));
    if (parser.isSet(QStringLiteral("ech-h3-selftest")))
        runEchH3SelfTest();
    // 注入必须先于探针落地：0.90 时这块写在 --direct-h3-probe 之后，
    // 桌面 CLI 下 --h3-fail-inject 对直连探针完全不起作用。
    if (parser.isSet(QStringLiteral("h3-fail-inject")))
        ProxyConfig::SetH3FailInject(parser.value(QStringLiteral("h3-fail-inject")));
    if (parser.isSet(QStringLiteral("direct-h3-probe")))
        runNetProbe(parser.value(QStringLiteral("direct-h3-probe")));
    if (parser.isSet(QStringLiteral("ech-h3-probe")))
        runEchH3Probe(parser.value(QStringLiteral("ech-h3-probe")));
    if (parser.isSet(QStringLiteral("h3-probe"))) {
        window.H3Probe(parser.value(QStringLiteral("h3-probe")).isEmpty()
                           ? QStringLiteral("www.cdnhjk.net")
                           : parser.value(QStringLiteral("h3-probe")),
                       parser.value(QStringLiteral("h3-path")).isEmpty()
                           ? QStringLiteral("/")
                           : parser.value(QStringLiteral("h3-path")),
                       parser.isSet(QStringLiteral("h3-ech")));
    }
    if (parser.isSet(QStringLiteral("h3-bench"))) {
        bool okCount = false;
        const int n = parser.value(QStringLiteral("h3-bench")).toInt(&okCount);
        window.H3Bench(parser.value(QStringLiteral("h3-probe")).isEmpty()
                           ? QStringLiteral("cloudflare.com")
                           : parser.value(QStringLiteral("h3-probe")),
                       parser.value(QStringLiteral("h3-path")).isEmpty()
                           ? QStringLiteral("/")
                           : parser.value(QStringLiteral("h3-path")),
                       okCount ? n : 10);
    }
    if (parser.isSet(QStringLiteral("h3-burst"))) {
        bool okCount = false;
        const int n = parser.value(QStringLiteral("h3-burst")).toInt(&okCount);
        window.H3Burst(parser.value(QStringLiteral("h3-probe")).isEmpty()
                           ? QStringLiteral("cloudflare.com")
                           : parser.value(QStringLiteral("h3-probe")),
                       parser.value(QStringLiteral("h3-path")).isEmpty()
                           ? QStringLiteral("/")
                           : parser.value(QStringLiteral("h3-path")),
                       okCount ? n : 10);
    }
    if (parser.isSet(QStringLiteral("ech-probe")))
        window.EchProbe(parser.value(QStringLiteral("ech-probe")).isEmpty()
                            ? QStringLiteral("www.cdnhjk.net")
                            : parser.value(QStringLiteral("ech-probe")));
    if (parser.isSet(QStringLiteral("scroll-bottom"))) {
        QTimer::singleShot(600, &window, [&window] {
            for (QScrollArea *sa : window.findChildren<QScrollArea *>()) {
                if (sa->isVisible())
                    sa->verticalScrollBar()->setValue(sa->verticalScrollBar()->maximum());
            }
        });
    }
    if (parser.isSet(QStringLiteral("doh-test"))) {
        const QString host = parser.value(QStringLiteral("doh-test")).isEmpty()
                                 ? QStringLiteral("www.cdnhjk.net")
                                 : parser.value(QStringLiteral("doh-test"));
        ProxyConfig::Load();
        DohClient::I().Configure(ProxyConfig::DohAddress(), ProxyConfig::EchEnabled());
        QObject::connect(&DohClient::I(), &DohClient::Resolved, &window,
                         [host](const QString &h, const QStringList &ips, bool viaDoh,
                                const QString &error) {
                             if (h != host)
                                 return;
                             qWarning("doh-test: host=%s viaDoh=%d ips=%s err=%s",
                                      qPrintable(h), int(viaDoh),
                                      qPrintable(ips.join(QLatin1Char(','))), qPrintable(error));
                         });
        DohClient::I().Resolve(host);
    }
    if (parser.isSet(QStringLiteral("http3-on")))
        ProxyConfig::SetHttp3Enabled(true);   // 只在内存里改，不 Save，避免污染用户配置
    if (parser.isSet(QStringLiteral("doh-on"))) {
        ProxyConfig::Load();
        ProxyConfig::SetDohEnabled(true);
        ProxyConfig::Save();
        ProxyConfig::SyncDohClient();
        const QString host = QUrl(ProxyConfig::GetApiUrl2(ProxyConfig::ApiLine())).host();
        // 同步预热：保证后面 --open-album / --real-search 的请求能用上 DoH 解析结果，
        // 而不是"第一次直连、异步解析还没回来"。
        QEventLoop loop;
        QTimer::singleShot(6000, &loop, &QEventLoop::quit);
        QObject::connect(&DohClient::I(), &DohClient::Resolved, &loop,
                         [&loop, host](const QString &h, const QStringList &, bool,
                                       const QString &) {
                             if (h == host)
                                 loop.quit();
                         });
        DohClient::I().Resolve(host);
        loop.exec();
        // ECH 配置与 DoH A 记录是两条查询；等它到齐，后续请求才走 ECH 通道
        if (DohClient::I().CachedEch().isEmpty()) {
            QEventLoop echLoop;
            QTimer::singleShot(6000, &echLoop, &QEventLoop::quit);
            QObject::connect(&DohClient::I(), &DohClient::EchReady, &echLoop,
                             &QEventLoop::quit);
            echLoop.exec();
        }
        qWarning("doh-on: host=%s ips=%s ech=%dB", qPrintable(host),
                 qPrintable(DohClient::I().CachedIps(host).join(QLatin1Char(','))),
                 int(QByteArray::fromBase64(DohClient::I().CachedEch().toLatin1()).size()));
    }
    if (parser.isSet(QStringLiteral("use-node")))
        window.UseNodeForTest(parser.value(QStringLiteral("use-node")));
    if (parser.isSet(QStringLiteral("latest-more")))
        QTimer::singleShot(16000, &window, [&window] { window.LoadMoreLatest(); });
    if (parser.isSet(QStringLiteral("open-album")))
        window.OpenAlbumById(parser.value(QStringLiteral("open-album")));
    if (parser.isSet(QStringLiteral("open-chapter")))
        window.OpenChapterById(parser.value(QStringLiteral("open-chapter")),
                               QStringLiteral("在线章节 %1")
                                   .arg(parser.value(QStringLiteral("open-chapter"))));
    if (parser.isSet(QStringLiteral("real-search")))
        window.RealSearch(parser.value(QStringLiteral("real-search")));
    if (parser.isSet(QStringLiteral("login"))) {
        const QString spec = parser.value(QStringLiteral("login"));
        const int colon = spec.indexOf(QLatin1Char(':'));
        if (colon > 0)
            window.RealLogin(spec.left(colon), spec.mid(colon + 1));
        else
            window.RealLogin(spec, QString());
    }
    // ---- 本轮修复的验收入口 ----
    if (parser.isSet(QStringLiteral("cache-dir"))) {
        const bool ok = CachePath::SetRoot(parser.value(QStringLiteral("cache-dir")));
        qWarning("cache-path: set ok=%d %s", int(ok), qPrintable(CachePath::StatusLine()));
    }
    if (parser.isSet(QStringLiteral("cache-reset"))) {
        CachePath::SetRoot(QString());
        qWarning("cache-path: reset %s", qPrintable(CachePath::StatusLine()));
    }
    if (parser.isSet(QStringLiteral("cache-status")))
        qWarning("%s", qPrintable(CachePath::StatusLine()));
    if (parser.isSet(QStringLiteral("settings-cache"))) {
        window.Go(PageId::Settings, false);
        auto *sv = qobject_cast<SettingsView *>(window.FindPage(PageId::Settings));
        const bool ok = sv && sv->ApplyCacheDir(parser.value(QStringLiteral("settings-cache")));
        qWarning("cache-path: settings-apply ok=%d label=%s %s", int(ok),
                 qPrintable(sv ? sv->CacheDirLabel() : QString()),
                 qPrintable(CachePath::StatusLine()));
    }

    if (parser.isSet(QStringLiteral("nav-report")))
        qWarning("nav-action: count=%d", window.NavActionCount());
    if (parser.isSet(QStringLiteral("user-report"))) {
        // 延时一拍：登录/注入可能还在事件队列里，侧栏文字要等布局刷新后才准
        QTimer::singleShot(600, &window,
                           [&window] { qWarning("user-report: %s", qPrintable(window.UserReport())); });
    }
    if (parser.isSet(QStringLiteral("nav-action"))) {
        const QString id = parser.value(QStringLiteral("nav-action"));
        QTimer::singleShot(260, &window, [&window, id] { window.ClickNavAction(id); });
    }

    if (parser.isSet(QStringLiteral("sign-demo"))) {
        QTimer::singleShot(320, &window, [&window] {
            window.SignIn();
            qWarning("sign: state=%s", qPrintable(window.SignButtonState()));
        });
        // 注入模式下是同步应答，再补一条延时日志（等 UI 更新）
        QTimer::singleShot(520, &window, [&window] {
            qWarning("sign: state-after=%s", qPrintable(window.SignButtonState()));
        });
    }

    if (parser.isSet(QStringLiteral("home-geom"))) {
        QTimer::singleShot(500, &window, [&window] {
            qWarning("home-geom: %s", qPrintable(window.HomeGeometryReport()));
        });
    }

    if (parser.isSet(QStringLiteral("layout-dump"))) {
        const int depth = qBound(1, parser.value(QStringLiteral("layout-dump")).toInt(), 8);
        QTimer::singleShot(600, &window, [&window, depth] {
            if (PageBase *p = window.FindPage(window.CurrentPage()))
                DumpLayoutTree(p, 0, depth);
        });
    }
    if (parser.isSet(QStringLiteral("tapguard-selftest"))) {
        // 必须在窗口已经布局/显示之后合成事件，否则按钮的命中矩形还是 0x0
        QTimer::singleShot(720, &window, [&window] {
            const bool ok = window.TapGuardSelfTest();
            qWarning("tapguard-selftest: %s", ok ? "pass" : "fail");
        });
    }
    if (parser.isSet(QStringLiteral("drawer-nav"))) {
        const PageId target = PageFromName(parser.value(QStringLiteral("drawer-nav")));
        QTimer::singleShot(320, &window, [&window, target] { window.DrawerNavForTest(target); });
    }
    if (parser.isSet(QStringLiteral("reader-progressive"))) {
        const QString dir = parser.value(QStringLiteral("reader-progressive"));
        QTimer::singleShot(420, &window, [&window, dir] {
            window.RunReaderProgressiveForTest(dir);
        });
    }
    if (parser.isSet(QStringLiteral("reader-fullscreen"))) {
        // 桌面/真机同一条路径：等价于点菜单里的"全屏"
        QTimer::singleShot(900, &window, [&window] { window.ToggleReaderFullScreen(); });
    }
    if (parser.isSet(QStringLiteral("blank-report"))) {
        // 带截图时把量测推迟到截图前：首页数据来自网络，400ms 时还没到，
        // 量到的是加载态（pagerH=-1），回归会误判。离线页面仍按 400ms 走。
        const int shotDelay = parser.value(QStringLiteral("shot-delay")).toInt();
        const int delayMs = parser.isSet(QStringLiteral("shot"))
                                ? qMax(400, shotDelay - 1200)
                                : 400;
        QTimer::singleShot(delayMs, &window, [&window] { ReportBlankBands(window); });
    }

    // 首页分区页要等 /promote + /latest 回来才存在：下面几个入口都先等"有分区"，
    // 否则量到的是加载态（deck 还没布局，viewport/content 都是 -1）。
    // 等待预算跟着 --shot-delay 走：接口慢的时候（H3 超时→TCP 回退，实测 14s+）
    // 固定 12s 会在数据到达前就放弃。
    const int homeWaitMs = parser.isSet(QStringLiteral("shot"))
                               ? qMax(12000, parser.value(QStringLiteral("shot-delay")).toInt() - 600)
                               : 12000;
    auto whenHomeReady = [&window, homeWaitMs](const std::function<void()> &fn) {
        auto *wait = new QTimer(&window);
        wait->setInterval(120);
        auto *clock = new QElapsedTimer;
        clock->start();
        QObject::connect(wait, &QTimer::timeout, &window, [&window, wait, clock, fn, homeWaitMs] {
            if (!window.HomeReady()) {
                if (clock->elapsed() > homeWaitMs) {
                    std::fprintf(stderr, "home: no sections after 12s, skipped");
                    wait->stop();
                }
                return;
            }
            wait->stop();
            fn();
        });
        wait->start();
    };

    if (parser.isSet(QStringLiteral("strip-drag"))) {
        const int dx = parser.value(QStringLiteral("strip-drag")).toInt();
        window.Go(PageId::Home, false);
        whenHomeReady([&window, dx] {
            window.HomeStripDrag(dx);
            QTimer::singleShot(200, &window, [&window] {
                qWarning("%s", qPrintable(window.HomeStripState()));
            });
        });
    }
    if (parser.isSet(QStringLiteral("strip-tap"))) {
        const int x = parser.value(QStringLiteral("strip-tap")).toInt();
        window.Go(PageId::Home, false);
        whenHomeReady([&window, x] {
            window.HomeStripTap(x);
            QTimer::singleShot(320, &window, [&window] {
                qWarning("%s", qPrintable(window.HomeStripState()));
            });
        });
    }

    if (parser.isSet(QStringLiteral("home-swipe")) || parser.isSet(QStringLiteral("home-freeze"))) {
        const bool freeze = parser.isSet(QStringLiteral("home-freeze"));
        const int dx = parser.value(QStringLiteral("home-swipe")).toInt();
        const qreal p = freeze ? parser.value(QStringLiteral("home-freeze")).toDouble() : 0.0;
        const int from = parser.isSet(QStringLiteral("home-freeze-from"))
                             ? parser.value(QStringLiteral("home-freeze-from")).toInt()
                             : 0;
        const int to = parser.isSet(QStringLiteral("home-freeze-to"))
                           ? parser.value(QStringLiteral("home-freeze-to")).toInt()
                           : from + 1;
        whenHomeReady([&window, freeze, dx, p, from, to] {
            if (freeze)
                window.HomeFreeze(from, to, p);
            else
                window.HomeSwipe(dx, 0);
            qWarning("%s", qPrintable(window.HomeStripState()));
        });
    }

    const QString readerDir = parser.value(QStringLiteral("reader-dir"));
    if (!readerDir.isEmpty()) {
        window.LoadReaderDirForTest(readerDir);
        // 等布局完成后再跑模式/手势，否则视口尺寸还是构造时的 100px
        if (parser.isSet(QStringLiteral("reader-mode"))) {
            const int mode = parser.value(QStringLiteral("reader-mode")).toInt();
            QTimer::singleShot(320, &window, [&window, mode] {
                window.SetReadingModeForTest(mode);
            });
        }
        if (parser.isSet(QStringLiteral("reader-swipe"))) {
            const QStringList d = parser.value(QStringLiteral("reader-swipe"))
                                      .split(QLatin1Char(','));
            const int dx = d.value(0).toInt();
            const int dy = d.value(1).toInt();
            QTimer::singleShot(420, &window, [&window, dx, dy] {
                window.ReaderSwipeForTest(dx, dy);
            });
        }
        if (parser.isSet(QStringLiteral("reader-pinch"))) {
            const qreal ratio = parser.value(QStringLiteral("reader-pinch")).toDouble();
            QTimer::singleShot(460, &window, [&window, ratio] {
                window.ReaderPinchForTest(ratio);
            });
        }
        // 缩放后点一次"1:1"：验证标题栏按钮真的把缩放还原成 100%
        if (parser.isSet(QStringLiteral("reader-fit"))) {
            QTimer::singleShot(620, &window, [&window] { window.ClickReaderFitForTest(); });
        }
        if (parser.isSet(QStringLiteral("reader-parity"))) {
            QTimer::singleShot(520, &window, [&window] { window.RunReaderParityForTest(); });
        }
        if (parser.isSet(QStringLiteral("reader-touch-selftest"))) {
            QTimer::singleShot(560, &window, [&window] { window.RunReaderTouchForTest(); });
        }
        if (parser.isSet(QStringLiteral("reader-sr"))) {
            QTimer::singleShot(700, &window, [&window] { window.ReaderSrForTest(); });
        }
        if (parser.isSet(QStringLiteral("reader-sr-status"))) {
            QTimer::singleShot(1500, &window, [&window] {
                qWarning("reader-sr-status: %s", qPrintable(window.ReaderSrStatusForTest()));
            });
        }
    } else if (parser.isSet(QStringLiteral("reader-mode"))) {
        const int mode = parser.value(QStringLiteral("reader-mode")).toInt();
        window.Go(PageId::Reader, false);
        QTimer::singleShot(300, &window, [&window, mode] { window.SetReadingModeForTest(mode); });
    }

    if (parser.isSet(QStringLiteral("toggle-test"))) {
        window.Go(PageId::Settings, false);
        const QString want = parser.value(QStringLiteral("toggle-test"));
        QTimer::singleShot(300, &window, [&window, want] {
            auto *page = window.FindPage(PageId::Settings);
            if (!page)
                return;
            for (SettingRow *row : page->findChildren<SettingRow *>()) {
                auto *title = row->findChild<QLabel *>(QStringLiteral("SettingTitle"));
                if (!title || title->text() != want)
                    continue;
                auto *toggle = qobject_cast<ToggleSwitch *>(row->Control());
                if (!toggle)
                    continue;
                const bool before = toggle->isChecked();
                // 点标题区（行内左侧），不是开关本身 —— 验证"整行可点"
                const QPoint local(UiStyle::Fpx(24), row->height() / 2);
                const QPoint global = row->mapToGlobal(local);
                QMouseEvent press(QEvent::MouseButtonPress, QPointF(local), QPointF(global),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, QPointF(local), QPointF(global),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QCoreApplication::sendEvent(row, &press);
                QCoreApplication::sendEvent(row, &release);
                qWarning("toggle-test: row=%s hitHeight=%d toggleSize=%dx%d before=%d after=%d",
                         qPrintable(want), row->height(), toggle->width(), toggle->height(),
                         int(before), int(toggle->isChecked()));
                return;
            }
            qWarning("toggle-test: row not found: %s", qPrintable(want));
        });
    }

    if (parser.isSet(QStringLiteral("webp-probe"))) {
        const QString f = parser.value(QStringLiteral("webp-probe"));
        QFile file(f);
        const bool opened = file.open(QIODevice::ReadOnly);
        const QByteArray data = opened ? file.readAll() : QByteArray();
        const QImage byQt(f);
        const QImage byJm = JmWebp::IsWebp(data) ? JmWebp::Decode(data) : QImage();
        qWarning("webp-probe: file=%s bytes=%d qt=%dx%d jm=%dx%d isWebp=%d",
                 qPrintable(f), int(data.size()), byQt.width(), byQt.height(),
                 byJm.width(), byJm.height(), int(JmWebp::IsWebp(data)));
    }

    if (parser.isSet(QStringLiteral("scroll-report"))) {
        // 延迟到布局稳定后再报告（窗口 resize / 页面 OnEnter 都在事件循环里）
        QTimer::singleShot(600, &window, [&window] {
        qWarning("scroll-report: window=%dx%d", window.width(), window.height());
        // 手机端纵向页面不该能左右滑：hmax 必须为 0（否则 QScroller 会平移内容）
        for (QScrollArea *sa : window.findChildren<QScrollArea *>()) {
            if (!sa->isVisible())
                continue;
            QWidget *inner = sa->widget();
            qWarning("scroll-report: obj=%s hmax=%d vmax=%d viewport=%dx%d widget=%dx%d",
                     qPrintable(sa->objectName().isEmpty() ? QStringLiteral("-")
                                                           : sa->objectName()),
                     sa->horizontalScrollBar()->maximum(), sa->verticalScrollBar()->maximum(),
                     sa->viewport()->width(), sa->viewport()->height(),
                     inner ? inner->width() : -1, inner ? inner->height() : -1);
            if (inner && inner->width() > sa->viewport()->width()) {
                // 定位"把页面撑宽"的元凶：按最小宽排序打印前 5 个设置行
                QVector<QPair<int, QString>> rows;
                for (SettingRow *row : inner->findChildren<SettingRow *>()) {
                    auto *title = row->findChild<QLabel *>(QStringLiteral("SettingTitle"));
                    rows.push_back({row->minimumSizeHint().width(),
                                    title ? title->text() : QStringLiteral("?")});
                }
                std::sort(rows.begin(), rows.end(),
                          [](const QPair<int, QString> &a, const QPair<int, QString> &b) {
                              return a.first > b.first;
                          });
                for (int i = 0; i < qMin(5, rows.size()); ++i)
                    qWarning("scroll-report:   wide-row min=%d title=%s", rows.at(i).first,
                             qPrintable(rows.at(i).second));
                // 再找出所有"最小宽 > 视口"的子控件（含非 SettingRow 的容器）
                for (QWidget *w : inner->findChildren<QWidget *>()) {
                    if (w->minimumSizeHint().width() > sa->viewport()->width() - 24) {
                        qWarning("scroll-report:   wide-widget class=%s name=%s min=%d w=%d",
                                 w->metaObject()->className(),
                                 qPrintable(w->objectName().isEmpty() ? QStringLiteral("-")
                                                                      : w->objectName()),
                                 w->minimumSizeHint().width(), w->width());
                    }
                }
            }
        }
        // QListView / 自绘滚动视图（ComicListView 等）不是 QScrollArea，
        // 上面的循环扫不到；它们同样会被 QScroller 横向拖动，单独报一遍。
        for (QAbstractScrollArea *aa : window.findChildren<QAbstractScrollArea *>()) {
            if (!aa->isVisible() || qobject_cast<QScrollArea *>(aa))
                continue;
            qWarning("scroll-report: area=%s hmax=%d vmax=%d viewport=%dx%d",
                     aa->metaObject()->className(), aa->horizontalScrollBar()->maximum(),
                     aa->verticalScrollBar()->maximum(), aa->viewport()->width(),
                     aa->viewport()->height());
        }
        });
    }

    if (parser.isSet(QStringLiteral("font-status"))) {
        const AppFont::Config &cfg = AppFont::I().Cfg();
        const QFont f = AppFont::I().Build();
        qWarning("font-status: source=%s family=%s resolved=%s weight=%d/%d stretch=%d "
                 "size=%.1f/%.1f scale=%.3f familyOk=%d",
                 cfg.source == AppFont::Source::Custom ? "custom" : "system",
                 qPrintable(cfg.family), qPrintable(QFontInfo(f).family()),
                 int(QFontInfo(f).weight()), cfg.weight, f.stretch(), f.pointSizeF(),
                 double(cfg.pointSize), double(UiStyle::FontScale()),
                 int(QFontDatabase::families().contains(cfg.family, Qt::CaseInsensitive)));
        qWarning("font-status: text=%s", qPrintable(AppFont::I().StatusText()));
        // 卡片墙（QML）侧：把推给 QML 的三个属性打出来，证明 QML 真的收到了
        if (auto *grid = window.findChild<ComicGrid *>()) {
            if (QObject *root = grid->QmlRootForTest()) {
                qWarning("font-status: qml family=%s weight=%d scale=%.3f cellH=%d",
                         qPrintable(root->property("uiFontFamily").toString()),
                         root->property("uiFontWeight").toInt(),
                         root->property("uiScale").toReal(),
                         root->property("cellH").toInt());
            }
        }
    }

    if (parser.isSet(QStringLiteral("demo-collapse")))
        window.DemoCollapseNav();
    if (parser.isSet(QStringLiteral("demo-nav-end")))
        window.ScrollNavToEnd();
    if (parser.isSet(QStringLiteral("home-section")))
        window.SelectHomeSection(parser.value(QStringLiteral("home-section")).toInt());

    if (parser.isSet(QStringLiteral("demo-reader-tap"))) {
        const QStringList xy = parser.value(QStringLiteral("demo-reader-tap")).split(QLatin1Char(','));
        if (xy.size() == 2)
            window.DemoReaderTap(xy[0].toDouble(), xy[1].toDouble());
    }

    if (parser.isSet(QStringLiteral("demo-reader-menu"))) {
        QTimer::singleShot(320, &window, [&window] { window.ReaderMenuForTest(); });
    }

    if (parser.isSet(QStringLiteral("demo-drag"))) {
        const int dx = parser.value(QStringLiteral("demo-drag")).toInt();
        window.EnableTouchMode(true);
        QTimer::singleShot(200, &window, [&window, dx] {
            DemoEdgeDrag(window, dx, true);
            std::fprintf(stderr, "touch: edge-drag dx=%d -> page=%d\n", dx, int(window.CurrentPage()));
            std::fflush(stderr);
        });
    }

    if (parser.isSet(QStringLiteral("demo-sysfont"))) {
        // 只伪造"操作系统给出的答案"，防抖/比对/重设/刷新走真实路径
        const QStringList parts =
            parser.value(QStringLiteral("demo-sysfont")).split(QLatin1Char(','));
        const QString fam = parts.value(0, QStringLiteral("Segoe UI"));
        const double pt = parts.size() > 1 ? parts.value(1).toDouble() : 9.0;
        QTimer::singleShot(600, &window, [fam, pt] {
            QFont f;
            f.setFamily(fam);
            f.setPointSizeF(pt);
            AppFont::I().SetSystemFontProviderForTest(f);
            std::fprintf(stderr, "demo: forced system font -> %s %.1fpt\n", qPrintable(fam), pt);
            std::fflush(stderr);
        });
    }

    if (parser.isSet(QStringLiteral("demo-back-swipe"))) {
        QTimer::singleShot(150, &window, [&window] {
            const bool ok = window.SimulateBackSwipe();
            std::fprintf(stderr, "touch: back-swipe -> %s\n", ok ? "navigated back" : "at root");
            std::fflush(stderr);
        });
    }

    if (parser.isSet(QStringLiteral("scroll-end")))
        window.ScrollDemo(parser.value(QStringLiteral("scroll-end")).toInt());

    if (parser.isSet(QStringLiteral("sr-pick"))) {
        const QString want = parser.value(QStringLiteral("sr-pick"));
        QTimer::singleShot(240, &window, [&window, want] {
            auto *page = qobject_cast<SrPage *>(window.FindPage(PageId::Sr));
            if (!page) {
                qWarning("sr-pick: no sr page");
                return;
            }
            const bool ok = page->SelectModel(want);
            qWarning("sr-pick: %s ok=%d %s", qPrintable(want), int(ok),
                     qPrintable(page->Report()));
        });
    }
    if (parser.isSet(QStringLiteral("sr-page-image"))) {
        const QString path = parser.value(QStringLiteral("sr-page-image"));
        const bool runNow = parser.isSet(QStringLiteral("sr-page-run"));
        QTimer::singleShot(240, &window, [&window, path, runNow] {
            auto *page = qobject_cast<SrPage *>(window.FindPage(PageId::Sr));
            if (!page)
                return;
            page->SetSourceForTest(path);
            if (runNow && !page->StartUpscaleForTest())
                qWarning("sr-page-run: not started");
        });
    }
    if (parser.isSet(QStringLiteral("sr-sheet"))) {
        const int scrollTo = parser.value(QStringLiteral("sr-sheet-scroll")).toInt();
        QTimer::singleShot(220, &window, [&window, scrollTo] {
            auto *page = window.FindPage(PageId::Sr);
            if (!page)
                return;
            auto *sheet = SrModelSheet::Show(page, SrEngine::CurrentModel());
            if (!sheet || scrollTo <= 0)
                return;
            // 浮层自己会在下一轮事件循环把列表滚到"当前使用"那一行，
            // 截图用的滚动必须再晚一拍，否则会被它覆盖回顶部
            QTimer::singleShot(80, sheet, [sheet, scrollTo] {
                if (auto *list = sheet->findChild<QListWidget *>(QStringLiteral("SrModelList")))
                    list->verticalScrollBar()->setValue(scrollTo);
            });
        });
    }
    if (parser.isSet(QStringLiteral("demo-dialog"))) {
        QTimer::singleShot(120, &window, [&window] {
            ModalDialog::Show(&window, QStringLiteral("清空历史"),
                              QStringLiteral("将删除全部本地阅读记录，且无法恢复。"),
                              QStringLiteral("清空"), QStringLiteral("取消"), ModalDialog::Danger);
        });
    }

    if (parser.isSet(QStringLiteral("img-host")))
        window.SetImageHostsForTest(
            parser.value(QStringLiteral("img-host")).split(QLatin1Char(','), Qt::SkipEmptyParts));
    if (parser.isSet(QStringLiteral("dl-report"))) {
        QTimer::singleShot(500, &window,
                           [&window] { qWarning("%s", qPrintable(window.DownloadReport())); });
    }
    if (parser.isSet(QStringLiteral("dl-selftest"))) {
        const int n = parser.value(QStringLiteral("dl-chapters")).toInt();
        const int secs = parser.value(QStringLiteral("dl-timeout")).toInt();
        const bool ok = window.DownloadSelfTest(parser.value(QStringLiteral("dl-selftest")),
                                                n > 0 ? n : 1,
                                                (secs > 0 ? secs : 180) * 1000);
        std::fprintf(stderr, "dl-selftest: %s\n", ok ? "PASS" : "FAIL");
        std::fflush(stderr);
        return ok ? 0 : 2;
    }

    const QString shot = parser.value(QStringLiteral("shot"));
    if (!shot.isEmpty()) {
        QDir().mkpath(QFileInfo(shot).absolutePath());
        window.ShotTo(shot, qMax(0, parser.value(QStringLiteral("shot-delay")).toInt()));
    }
    const int rc = app.exec();
    // ORT 的 session/env 在静态析构里释放会和推理线程撞上（实测进程卡住不退），
    // 显式收口：先释放 ORT 对象，再让 QApplication 正常销毁。
    SrEngine::Shutdown();
    return rc;
}
