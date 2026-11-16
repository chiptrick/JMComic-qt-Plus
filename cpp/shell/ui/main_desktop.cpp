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
#include "../vk/sr_vk.h"
#include "jm_account.h"
#include "log_export.h"
#include "nas_store.h"
#include "update_checker.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDebug>
#include <QDateTime>
#include <QElapsedTimer>
#include <QEvent>
#include <QLabel>
#include <QListWidget>
#include <QWidget>
#ifdef Q_OS_WIN
#include <windows.h>   // 静默验收：把窗口挪出可视区并摘掉激活/任务栏样式
#endif
#include <QDir>
#include <QFont>
#include <QStandardPaths>
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
#include <QtConcurrent/QtConcurrent>
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
#include "data_migrator.h"
#include "comic_view.h"
#include "models.h"
#include "node_bench_pool.h"
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

#include <QElapsedTimer>
#include <QFile>
#include <cmath>

// ---------------------------------------------------------------- 真机 Vulkan 验收
// 手机没有桌面 CLI：`am start ... --es jm.args "--vk-report"` 就能把设备摘要、
// 端到端探针、卷积/Pad 单算子自检、逐模型扫描与吞吐基准全部写进 logcat
// （统一前缀 vk-report:）与外部私有目录的 vk-report.txt（adb pull 可读）。
// 用 QTimer::singleShot(0) 分片执行，避免同步跑 39 个模型把主线程卡成 ANR。
struct VkReportState
{
    QStringList lines;
    QVector<SrEngine::ModelInfo> models;
    QString modelDir;
    QString only;
    int idx = 0;
    int ok = 0;
    int fail = 0;
    int skipped = 0;
    int tile = 192;
    int benchN = 0;
    bool sweep = false;
    // compare 模式：每个成功模型额外跑一次 ORT CPU（tile 固定 192，ORT 输入
    // 维度写死）并与 Vulkan 输出逐像素 diff——真机 fp16/wino 的精度复测入口
    bool compare = false;
    int cmpPass = 0;
    int cmpTotal = 0;
};

static void VkReportLine(const QString &line, std::shared_ptr<VkReportState> st)
{
    qWarning("vk-report: %s", line.toUtf8().constData());
    st->lines.append(line);
}

// 回归报告落盘：按"adb 能不能 pull"排序试几个目录。
//   1. GenericDataLocation —— Android 上是 /storage/emulated/0/Android/data/<pkg>/files
//   2. 硬编码同一路径（某些 ROM 上 /sdcard 这个 symlink 在 App 的 mount
//      namespace 里不可见，mkpath("/sdcard/...") 直接失败，必须写真实路径）
//   3. AppDataLocation —— /data/user/0/<pkg>/files，兜底保证报告不丢
// 只往一个位置写，返回实际路径；全写不进去时返回空串（调用方打 unavailable）。
static QString WriteRunReport(const QString &name, const QStringList &lines)
{
    QStringList dirs;
    const QString gen = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!gen.isEmpty())
        dirs << gen;
    dirs << QStringLiteral("/storage/emulated/0/Android/data/org.jmcomic.ui/files");
    dirs << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    for (const QString &dir : dirs) {
        if (dir.isEmpty() || !QDir().mkpath(dir))
            continue;
        const QString path = dir + QLatin1Char('/') + name;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            continue;
        f.write(lines.join(QLatin1Char('\n')).toUtf8());
        f.write("\n");
        f.close();
        return path;
    }
    return QString();
}

static void VkReportFinish(std::shared_ptr<VkReportState> st)
{
    VkReportLine(QStringLiteral("summary models=%1 ok=%2 fail=%3 skipped=%4 tile=%5%6")
                     .arg(st->models.size())
                     .arg(st->ok)
                     .arg(st->fail)
                     .arg(st->skipped)
                     .arg(st->tile)
                     .arg(st->cmpTotal > 0
                              ? QStringLiteral(" compare=%1/%2")
                                    .arg(st->cmpPass)
                                    .arg(st->cmpTotal)
                              : QString()),
                 st);
    // logcat 是主通道（一定有权限）；文件只是方便一次性 pull 回来存档。
    const QString dir = QStringLiteral("/sdcard/Android/data/org.jmcomic.ui/files");
    QDir().mkpath(dir);
    QFile f(QDir(dir).filePath(QStringLiteral("vk-report.txt")));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(st->lines.join(QLatin1Char(10)).toUtf8());
        f.write("\n");
        f.close();
        VkReportLine(QStringLiteral("file=%1").arg(f.fileName()), st);
    } else {
        VkReportLine(QStringLiteral("file=unavailable (%1)").arg(f.errorString()), st);
    }
    VkReportLine(QStringLiteral("done"), st);
}

static void VkReportStep(std::shared_ptr<VkReportState> st)
{
    if (st->sweep && st->idx < st->models.size()) {
        const SrEngine::ModelInfo m = st->models.at(st->idx++);
        const QString path = st->modelDir + QLatin1Char('/') + m.file;
        // 设备反复 DEVICE_LOST 后熔断：后续模型全部记 SKIP，不再让驱动继续重置。
        QString availWhy;
        if (!SrVk::Available(&availWhy)) {
            VkReportLine(QStringLiteral("model %1 SKIP vulkan-unavailable %2")
                             .arg(m.file, availWhy),
                         st);
            ++st->skipped;
            QTimer::singleShot(0, qApp, [st] { VkReportStep(st); });
            return;
        }
        QString err;
        SrVk::Session session;
        bool ok = session.load(path, &err) && session.ensureTile(st->tile, &err);
        const int elems = 3 * st->tile * st->tile;
        std::vector<float> input(static_cast<std::size_t>(elems), 0.0f);
        for (int i = 0; i < elems; ++i)
            input[std::size_t(i)] = float((i * 37 + 11) % 251) / 251.0f;
        std::vector<float> output;
        int ow = 0;
        int oh = 0;
        if (ok) {
            QElapsedTimer t;
            t.start();
            ok = session.run(input.data(), &output, &ow, &oh, &err);
            if (ok) {
                int bad = 0;
                for (float v : output)
                    bad += std::isfinite(v) ? 0 : 1;
                ok = bad == 0;
                if (!ok)
                    err = QStringLiteral("non-finite=%1").arg(bad);
                VkReportLine(QStringLiteral("model %1 %2 out=%3x%4 ms=%5 %6")
                                 .arg(m.file, ok ? QStringLiteral("ok") : QStringLiteral("bad"))
                                 .arg(ow)
                                 .arg(oh)
                                 .arg(t.elapsed())
                                 .arg(err),
                             st);
            }
        }
        // compare 模式：同一输入跑 ORT CPU（输入维度写死 192，只有 tile192 能比），
        // 逐像素 diff——fp16 基线 2.5e-2 / fp32 基线 1e-3，与桌面 --vk-compare 同一套
        if (ok && st->compare && st->tile == 192) {
            std::vector<float> ortOut;
            int ortW = 0;
            int ortH = 0;
            QString ortErr;
            if (SrEngine::DebugOrtTile(path, input, &ortOut, &ortW, &ortH, &ortErr)
                && ortW == ow && ortH == oh && ortOut.size() == output.size()) {
                double maxAbs = 0.0;
                int nonFinite = 0;
                for (size_t i = 0; i < output.size(); ++i) {
                    const double e =
                        std::fabs(double(output[i]) - double(ortOut[std::size_t(i)]));
                    if (!std::isfinite(e))
                        ++nonFinite;
                    maxAbs = qMax(maxAbs, e);
                }
                const double tol = SrVk::Fp16Active() ? 2.5e-2 : 1e-3;
                const bool pass = nonFinite == 0 && maxAbs <= tol;
                ++st->cmpTotal;
                st->cmpPass += pass ? 1 : 0;
                VkReportLine(QStringLiteral("compare %1 %2 maxAbs=%3 tol=%4 fp16=%5")
                                 .arg(m.file, pass ? QStringLiteral("PASS")
                                                   : QStringLiteral("FAIL"))
                                 .arg(maxAbs, 0, 'g', 4)
                                 .arg(tol)
                                 .arg(int(SrVk::Fp16Active())),
                             st);
            } else {
                VkReportLine(QStringLiteral("compare %1 SKIP ort-fail %2")
                                 .arg(m.file, ortErr),
                             st);
            }
        }
        // 这三类不是"算错"，而是 Vulkan 明确不可用/不适用；报告里标 SKIP，
        // 并现场验证 ORT CPU 回退真的能出图（真机最关键的兼容性结论）。
        const bool quantized = err.contains(QStringLiteral("量化模型"));
        const bool budget = err.contains(QStringLiteral("激活区"))
                            || err.contains(QStringLiteral("显存"));
        const bool lost = err.contains(QStringLiteral("DEVICE_LOST"))
                          || err.contains(QStringLiteral("熔断"));
        if (!ok && (quantized || budget || lost)) {
            // ORT 的输入维度固定 192，CPU 验证始终按 192 打包，与报告 tile 无关。
            std::vector<float> cpuInput(static_cast<std::size_t>(3 * 192 * 192), 0.0f);
            std::vector<float> cpuOut;
            int cow = 0;
            int coh = 0;
            QString cpuErr;
            const bool cpuOk =
                SrEngine::DebugOrtTile(path, cpuInput, &cpuOut, &cow, &coh, &cpuErr);
            const QString reason = quantized ? QStringLiteral("quantized")
                                             : (budget ? QStringLiteral("vram-budget")
                                                       : QStringLiteral("device-lost"));
            VkReportLine(QStringLiteral("model %1 SKIP %2 vulkan=unavailable ort-cpu=%3 %4")
                             .arg(m.file)
                             .arg(reason)
                             .arg(int(cpuOk))
                             .arg(cpuOk ? QStringLiteral("ok") : cpuErr),
                         st);
            ++st->skipped;
        } else {
            if (!ok && !err.isEmpty())
                VkReportLine(QStringLiteral("model %1 FAIL %2").arg(m.file, err), st);
            st->ok += ok ? 1 : 0;
            st->fail += ok ? 0 : 1;
        }
        // 让事件循环喘一口气；同步连跑会在手机上触发 ANR / GPU watchdog
        QTimer::singleShot(0, qApp, [st] { VkReportStep(st); });
        return;
    }

    if (st->benchN > 0 && !st->models.isEmpty()) {
        QString path = st->modelDir + QLatin1Char('/') + st->models.first().file;
        for (const SrEngine::ModelInfo &m : st->models) {
            if (m.file.contains(QLatin1String("waifu2x_cunet_up2x_denoise3"))) {
                path = st->modelDir + QLatin1Char('/') + m.file;
                break;
            }
        }
        QString rep;
        const SrVk::BenchResult r = SrVk::Benchmark(path, st->tile, st->benchN, &rep);
        VkReportLine(rep, st);
        // ORT 的输入维度固定 192：CPU 对照始终按 192 打包，
        // 不能复用 Vulkan 的自定义 tile 输入（否则报 invalid dimensions）。
        std::vector<float> cpuInput(static_cast<std::size_t>(3 * 192 * 192), 0.0f);
        std::vector<float> output;
        int ow = 0;
        int oh = 0;
        QString err;
        QElapsedTimer t;
        t.start();
        const bool cpuOk = SrEngine::DebugOrtTile(path, cpuInput, &output, &ow, &oh, &err);
        const double cpuMs = double(t.nsecsElapsed()) / 1.0e6;
        VkReportLine(QStringLiteral("vk-bench: ort-cpu ok=%1 ms=%2 out=%3x%4 %5")
                         .arg(int(cpuOk))
                         .arg(cpuMs, 0, 'f', 2)
                         .arg(ow)
                         .arg(oh)
                         .arg(cpuOk ? QString() : err),
                     st);
        if (r.ok && cpuOk)
            VkReportLine(QStringLiteral("vk-bench: speedup=%1x (ort/vk median)")
                             .arg(cpuMs / qMax(0.01, r.medianMs), 0, 'f', 2),
                         st);
    }
    VkReportFinish(st);
}

static void RunAndroidVkReport(bool sweep, int benchN, int tile, const QString &only,
                               bool compare)
{
    auto st = std::make_shared<VkReportState>();
    st->tile = tile;
    st->benchN = benchN;
    st->sweep = sweep;
    st->only = only;
    st->compare = compare;
    st->modelDir = SrEngine::ModelsDir();
    for (const SrEngine::ModelInfo &m : SrEngine::InstalledModels()) {
        if (!only.isEmpty() && !m.file.contains(only, Qt::CaseInsensitive))
            continue;
        st->models.append(m);
    }
    VkReportLine(QStringLiteral("start dir=%1 models=%2 tile=%3 sweep=%4 bench=%5 compare=%6")
                     .arg(st->modelDir)
                     .arg(st->models.size())
                     .arg(tile)
                     .arg(int(sweep))
                     .arg(benchN)
                     .arg(int(compare)),
                 st);
    QString why;
    const bool avail = SrVk::Available(&why);
    VkReportLine(QStringLiteral("available=%1 info=%2 %3")
                     .arg(int(avail))
                     .arg(SrVk::Info())
                     .arg(why),
                 st);
    QString rep;
    const bool probeOk = SrVk::Probe(&rep);
    VkReportLine(QStringLiteral("probe pass=%1 %2")
                     .arg(int(probeOk))
                     .arg(rep.replace(QLatin1Char(10), QLatin1Char('|'))),
                 st);
    const bool convOk = SrVk::ConvSelfTest(&rep);
    VkReportLine(QStringLiteral("conv pass=%1 %2")
                     .arg(int(convOk))
                     .arg(rep.replace(QLatin1Char(10), QLatin1Char('|'))),
                 st);
    const bool padOk = SrVk::PadSelfTest(&rep);
    VkReportLine(QStringLiteral("pad pass=%1 %2")
                     .arg(int(padOk))
                     .arg(rep.replace(QLatin1Char(10), QLatin1Char('|'))),
                 st);
    if (!avail) {
        VkReportFinish(st);
        return;
    }
    QTimer::singleShot(0, qApp, [st] { VkReportStep(st); });
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
            const int dy = qAbs((sg.top() + sg.height() / 2) - (tg.top() + tg.height() / 2));
            // 逐行打出来：只报一个 max 时无法定位是哪一行（尤其是"说明文字换行"的行）。
            // 注意用 toUtf8()：qPrintable 走本地编码，中文在日志里会变成 U+FFFD。
            qWarning("blank-report: toggleRow dy=%d title=%s descH=%d rowH=%d", dy,
                     title->text().toUtf8().constData(),
                     row->findChild<QLabel *>(QStringLiteral("SettingDesc"))
                         ? row->findChild<QLabel *>(QStringLiteral("SettingDesc"))->height()
                         : 0,
                     row->height());
            maxCenterDy = qMax(maxCenterDy, dy);
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
    QApplication::setApplicationVersion(QStringLiteral("0.95"));

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
    parser.addOption({QStringLiteral("sr-sheet-bench"),
                      QStringLiteral("回归：打开浮层并点测速按钮（sel=选中 / all=全部）"),
                      QStringLiteral("mode")});
    parser.addOption({QStringLiteral("sr-sheet-filter"),
                      QStringLiteral("配合 --sr-sheet-bench：先点名字包含该词的筛选 chip"),
                      QStringLiteral("kw")});
    parser.addOption({QStringLiteral("sr-sheet-cancel"),
                      QStringLiteral("配合 --sr-sheet-bench：N 毫秒后点「停止」"), QStringLiteral("ms")});
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
    parser.addOption({QStringLiteral("node-threads"),
                      QStringLiteral("本次运行的节点测速线程数 1..16（1=兼容单线程；不写回配置）"),
                      QStringLiteral("n")});
    parser.addOption({QStringLiteral("node-bench-selftest"),
                      QStringLiteral("节点测速线程池离线自检（夹紧/并发度/汇总/异常/超时/取消）")});
    parser.addOption({QStringLiteral("node-probe"),
                      QStringLiteral("用工作线程的阻塞探测单测一个节点 IP（排障用）"),
                      QStringLiteral("ip")});
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
                      QStringLiteral("全控件防误触自检：弹窗/二级菜单/浮层滑动不误触、点击照常")});
    parser.addOption({QStringLiteral("tapguard-legacy"),
                      QStringLiteral("回归对照：复刻修复前弹窗补发按下重置手势的行为")});
    parser.addOption({QStringLiteral("drawer-nav"),
                      QStringLiteral("抽屉打开后点一次导航项（验证换页动画）"), QStringLiteral("page")});
    parser.addOption({QStringLiteral("sr-selftest"),
                      QStringLiteral("超分自检（可选输出 png）"), QStringLiteral("png"),
                      QStringLiteral("")});
    // ---- 超分模型目录（本轮修复：模型不再只有三个）----
    parser.addOption({QStringLiteral("sr-bench"),
                      QStringLiteral("回归：按硬件测一遍模型（all 或逗号分隔的模型名）"),
                      QStringLiteral("sel")});
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
    parser.addOption({QStringLiteral("settings-search"),
                      QStringLiteral("截图/回归：在设置页搜索框输入关键词（会触发过滤）"),
                      QStringLiteral("kw")});
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
    // ---- 本轮补齐能力的验收入口（都是真实动作，不是桩）----
    parser.addOption({QStringLiteral("export-data"),
                      QStringLiteral("把 session.ini/ui.ini(+.bak) 导出到指定目录（卸载重装/换设备前用）"),
                      QStringLiteral("dir")});
    parser.addOption({QStringLiteral("import-data"),
                      QStringLiteral("从 --export-data 的目录导入配置（导入前的现场滚动进 .bak）"),
                      QStringLiteral("dir")});
    parser.addOption({QStringLiteral("export-logs"),
                      QStringLiteral("导出诊断日志 zip 到指定目录并退出"), QStringLiteral("dir")});
    parser.addOption({QStringLiteral("vk-info"),
                      QStringLiteral("打印 Vulkan 设备摘要（loader/设备/显存/预算）")});
    parser.addOption({QStringLiteral("vk-probe"),
                      QStringLiteral("Vulkan 计算端到端自检（设备/arena/描述符/dispatch/回读）")});
    parser.addOption({QStringLiteral("vk-model-test"),
                      QStringLiteral("对模型目录下每个非量化 onnx 跑一次 Vulkan 推理"), QStringLiteral("dir"),
                      QStringLiteral("")});
    parser.addOption({QStringLiteral("vk-only"),
                      QStringLiteral("--vk-model-test 的过滤词（逗号分隔，匹配文件名）"),
                      QStringLiteral("list")});
    parser.addOption({QStringLiteral("vk-pad-test"),
                      QStringLiteral("Vulkan Pad（reflect）单算子自检")});
    parser.addOption({QStringLiteral("vk-conv-test"),
                      QStringLiteral("Vulkan 3x3 卷积单算子自检（对比 CPU，含 Winograd）")});
    parser.addOption({QStringLiteral("vk-stream-test"),
                      QStringLiteral("整图 tile 流水验收（参数：合成图边长，默认 512）"),
                      QStringLiteral("n"), QStringLiteral("512")});
    parser.addOption({QStringLiteral("sr-perf-test"),
                      QStringLiteral("设置页性能测试行的底层回归（当前模型分档基准 + 推荐）")});
    parser.addOption({QStringLiteral("sr-npu-compare"),
                      QStringLiteral("量化模型数值验收：同一 tile 对比 NPU 与 CPU 输出"),
                      QStringLiteral("model"), QStringLiteral("")});
    parser.addOption({QStringLiteral("vk-compare"),
                      QStringLiteral("同 tile 上对比 Vulkan 与 ORT 输出（参数：模型目录，可空）"),
                      QStringLiteral("dir"), QStringLiteral("")});
    parser.addOption({QStringLiteral("vk-tile"),
                      QStringLiteral("--vk-model-test / --vk-bench 的 tile 尺寸（默认 192）"),
                      QStringLiteral("n"), QStringLiteral("192")});
    parser.addOption({QStringLiteral("vk-bench"),
                      QStringLiteral("单模型吞吐基准（参数：模型文件名子串，可空取默认模型）"),
                      QStringLiteral("model"), QStringLiteral("")});
    parser.addOption({QStringLiteral("vk-iter"),
                      QStringLiteral("--vk-bench 的稳态迭代次数（默认 5）"),
                      QStringLiteral("n"), QStringLiteral("5")});
    parser.addOption({QStringLiteral("vk-quant-test"),
                      QStringLiteral("量化模型兼容性：ORT CPU 能否跑 + Vulkan 是否明确拒绝"),
                      QStringLiteral("dir"), QStringLiteral("")});
    parser.addOption({QStringLiteral("sr-backend"),
                      QStringLiteral("打印超分后端探测结果（pref/chosen/QNN 就绪情况）")});
    parser.addOption({QStringLiteral("nas-selftest"),
                      QStringLiteral("网络存储节点表增删校验自检（不污染既有配置）")});
    parser.addOption({QStringLiteral("account-selftest"),
                      QStringLiteral("注册/找回密码的表单编码与 toastr 解析自检（离线）")});
    parser.addOption({QStringLiteral("update-check"),
                      QStringLiteral("检查更新 + 拉更新配置（阻塞到结果或超时）")});
    parser.addOption({QStringLiteral("account-flow"),
                      QStringLiteral("账号网页流程验收：register:u:e:p:code / reset:email / "
                                     "confirm:u:p / captcha"),
                      QStringLiteral("spec")});
    parser.addOption({QStringLiteral("update-base"),
                      QStringLiteral("覆盖更新端点（指向本地 mock 服务，离线回归用）"),
                      QStringLiteral("url")});
    // 页面级回归（需要窗口/页面实例，异步结果靠 --shot-delay 等）
    parser.addOption({QStringLiteral("watchlist-report"),
                      QStringLiteral("打开观看记录页并请求 /watch_list")});
    parser.addOption({QStringLiteral("mycomment-report"),
                      QStringLiteral("打开我的评论页并请求 /forum?uid=")});
    parser.addOption({QStringLiteral("week-report"),
                      QStringLiteral("打开每周必看页并请求 /serialization")});
    parser.addOption({QStringLiteral("about-report"),
                      QStringLiteral("打开关于页、执行检查更新并把状态行打到日志")});
    parser.addOption({QStringLiteral("nas-ui-selftest"),
                      QStringLiteral("在真实 NasPage 上跑增/校验/删全链路")});
    parser.addOption({QStringLiteral("comment-report"),
                      QStringLiteral("打开某作品的评论区并请求 /forum?aid=（参数：作品 id）"),
                      QStringLiteral("albumId")});
    parser.addOption({QStringLiteral("login-panel"),
                      QStringLiteral("登录页展开注册/找回密码面板（register|forgot）后截图"),
                      QStringLiteral("kind")});
    parser.process(app);
    // 日志 handler 要尽早装：CLI 分支（--export-logs/--sr-backend/--nas-selftest…）
    // 也是"需要留证据"的运行，装晚了导出的 zip 里只有 summary.txt。
    // Init 内部有 once 保护，下面原本那处调用照旧。
    JmLog::Init();

    if (parser.isSet(QStringLiteral("sr-bench"))) {
        const QVector<SrEngine::BenchHw> hws = SrEngine::BenchHardware();
        QStringList hwIds;
        bool hadGpu = false;
        for (const SrEngine::BenchHw &h : hws) {
            std::fprintf(stderr, "sr-bench-hw: id=%s short=%s available=%d label=%s note=%s\n",
                         qPrintable(h.id), qPrintable(h.shortLabel), int(h.available),
                         qPrintable(h.label), qPrintable(h.note));
            if (h.available)
                hwIds << h.id;
            if (h.id.startsWith(QLatin1String("gpu")))
                hadGpu = true;
        }
        const QString sel = parser.value(QStringLiteral("sr-bench"));
        QStringList models;
        if (sel == QLatin1String("all")) {
            for (const SrEngine::ModelInfo &m : SrEngine::InstalledModels())
                models << m.name;
        } else {
            models = sel.split(QLatin1Char(','), Qt::SkipEmptyParts);
        }
        const QString original = SrEngine::CurrentModel();
        for (const QString &name : models) {
            const QVector<SrEngine::BenchSample> rows =
                SrEngine::BenchModel(name, hwIds, 192, 3);
            for (const SrEngine::BenchSample &r : rows) {
                std::fprintf(stderr, "sr-bench: model=%s hw=%s ok=%d ms=%.1f note=%s\n",
                             qPrintable(name), qPrintable(r.hwId), int(r.ok), r.ms,
                             qPrintable(r.note));
            }
            std::fflush(stderr);
        }
        SrEngine::BenchFinish(original, hadGpu);
        std::fprintf(stderr, "sr-bench: restored=%s (was %s)\n",
                     qPrintable(SrEngine::CurrentModel()), qPrintable(original));
        return 0;
    }
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

    // ---- 本轮补齐能力的验收入口 ----
    // 注意：这些分支都在窗口创建之前返回，跑的是"模块级"真实逻辑
    // （日志打包、后端探测、节点表读写、表单编码、更新检查），不依赖 GUI 状态。

    if (parser.isSet(QStringLiteral("vk-info"))) {
        qWarning("vk-info: %s", SrVk::Info().toUtf8().constData());
        return SrVk::Available() ? 0 : 2;
    }
    if (parser.isSet(QStringLiteral("vk-model-test"))) {
        QString dir = parser.value(QStringLiteral("vk-model-test"));
        if (dir.isEmpty())
            dir = SrEngine::ModelsDir();
        QStringList only;
        if (parser.isSet(QStringLiteral("vk-only"))) {
            only = parser.value(QStringLiteral("vk-only")).split(QLatin1Char(','),
                                                                 Qt::SkipEmptyParts);
        }
        const int tile = qMax(32, parser.value(QStringLiteral("vk-tile")).toInt());
        QString report;
        const QVector<SrVk::ModelResult> res = SrVk::Sweep(dir, only, tile, &report);
        std::fprintf(stderr, "%s\n", report.toUtf8().constData());
        int ok = 0;
        int tested = 0;
        int skipped = 0;
        qint64 ms = 0;
        for (const SrVk::ModelResult &r : res) {
            if (r.skipped) {
                ++skipped;
                continue;
            }
            ++tested;
            ok += r.ok ? 1 : 0;
            ms += r.elapsedMs;
        }
        std::fprintf(stderr,
                     "vk-model-test: total=%d tested=%d ok=%d fail=%d skipped=%d totalMs=%lld\n",
                     int(res.size()), tested, ok, tested - ok, skipped, (long long)ms);
        return (tested > 0 && ok == tested) ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("vk-bench"))) {
        const QString want = parser.value(QStringLiteral("vk-bench"));
        const QString dir = SrEngine::ModelsDir();
        QString path;
        for (const SrEngine::ModelInfo &m : SrEngine::InstalledModels()) {
            if (want.isEmpty() || m.file.contains(want, Qt::CaseInsensitive)) {
                path = QDir(dir).filePath(m.file);
                break;
            }
        }
        if (path.isEmpty()) {
            qWarning("vk-bench: no model matching [%s] under %s", qPrintable(want),
                     qPrintable(dir));
            return 2;
        }
        const int tile = qBound(32, parser.value(QStringLiteral("vk-tile")).toInt(), 512);
        const int n = qBound(1, parser.value(QStringLiteral("vk-iter")).toInt(), 100);
        QString rep;
        const SrVk::BenchResult r = SrVk::Benchmark(path, tile, n, &rep);
        qWarning("%s", rep.toUtf8().constData());
        std::vector<float> input(static_cast<std::size_t>(3 * tile * tile), 0.0f);
        std::vector<float> output;
        int ow = 0;
        int oh = 0;
        QString err;
        QElapsedTimer timer;
        timer.start();
        const bool cpuOk = SrEngine::DebugOrtTile(path, input, &output, &ow, &oh, &err);
        const double cpuMs = double(timer.nsecsElapsed()) / 1.0e6;
        qWarning("vk-bench: ort-cpu ok=%d ms=%g out=%dx%d %s", int(cpuOk), cpuMs, ow, oh,
                 qPrintable(cpuOk ? QString() : err));
        if (r.ok && cpuOk)
            qWarning("vk-bench: speedup=%gx (ort/vk median)", cpuMs / qMax(0.01, r.medianMs));
        SrEngine::Shutdown();
        return r.ok ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("vk-quant-test"))) {
        QString dir = parser.value(QStringLiteral("vk-quant-test"));
        if (dir.isEmpty())
            dir = qEnvironmentVariable("JM_VK_QUANT_DIR");
        if (dir.isEmpty())
            dir = SrEngine::ModelsDir();
        const QDir d(dir);
        const QStringList files = d.entryList(
            {QStringLiteral("*q16*.onnx"), QStringLiteral("*lowq*.onnx")}, QDir::Files,
            QDir::Name);
        if (files.isEmpty()) {
            qWarning("vk-quant-test: no quantized onnx under %s", qPrintable(dir));
            return 0;
        }
        int ortOk = 0;
        int vkRejected = 0;
        const int tile = qBound(32, parser.value(QStringLiteral("vk-tile")).toInt(), 512);
        for (const QString &file : files) {
            const QString path = d.filePath(file);
            std::vector<float> input(static_cast<std::size_t>(3 * tile * tile), 0.0f);
            std::vector<float> output;
            int ow = 0;
            int oh = 0;
            QString err;
            const bool cpu = SrEngine::DebugOrtTile(path, input, &output, &ow, &oh, &err);
            SrVk::Session vk;
            QString vkErr;
            const bool loaded = vk.load(path, &vkErr);
            const bool rejected = !loaded && vkErr.contains(QStringLiteral("量化模型"));
            ortOk += cpu ? 1 : 0;
            vkRejected += rejected ? 1 : 0;
            qWarning("vk-quant-test: %s ort=%d %s vulkan=%s %s", qPrintable(file), int(cpu),
                     cpu ? qPrintable(QStringLiteral("ok")) : qPrintable(err),
                     rejected ? "rejected" : (loaded ? "ACCEPTED(BUG)" : "failed"),
                     qPrintable(vkErr));
        }
        qWarning("vk-quant-test: total=%d ortOk=%d vulkanRejected=%d", int(files.size()), ortOk,
                 vkRejected);
        SrEngine::Shutdown();
        return vkRejected == int(files.size()) ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("vk-pad-test"))) {
        QString report;
        const bool ok = SrVk::PadSelfTest(&report);
        std::fprintf(stderr, "%s\n", report.toUtf8().constData());
        return ok ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("vk-conv-test"))) {
        QString report;
        const bool ok = SrVk::ConvSelfTest(&report);
        std::fprintf(stderr, "%s\n", report.toUtf8().constData());
        return ok ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("vk-compare"))) {
        QString dir = parser.value(QStringLiteral("vk-compare"));
        if (dir.isEmpty())
            dir = SrEngine::ModelsDir();
        QStringList only;
        if (parser.isSet(QStringLiteral("vk-only"))) {
            only = parser.value(QStringLiteral("vk-only")).split(QLatin1Char(','),
                                                                 Qt::SkipEmptyParts);
        }
        const int tile = qMax(32, parser.value(QStringLiteral("vk-tile")).toInt());
        const QStringList files = QDir(dir).entryList({QStringLiteral("*.onnx")}, QDir::Files,
                                                     QDir::Name);
        int compared = 0;
        int same = 0;
        for (const QString &file : files) {
            if (file.contains(QLatin1String("_q16")) || file.contains(QLatin1String("lowq")))
                continue;
            if (!only.isEmpty()) {
                bool hit = false;
                for (const QString &k : only) {
                    if (file.contains(k, Qt::CaseInsensitive)) {
                        hit = true;
                        break;
                    }
                }
                if (!hit)
                    continue;
            }
            const QString path = QDir(dir).filePath(file);
            const int inElems = 3 * tile * tile;
            std::vector<float> input(static_cast<std::size_t>(inElems), 0.0f);
            // 固定的伪随机图案：可复现，且覆盖 0..1 全量程
            for (int i = 0; i < inElems; ++i)
                input[std::size_t(i)] = float((i * 37 + 11) % 251) / 251.0f;

            std::vector<float> ortOut;
            int ortW = 0;
            int ortH = 0;
            QString err;
            const bool ortOk =
                SrEngine::DebugOrtTile(path, input, &ortOut, &ortW, &ortH, &err);

            SrVk::Session session;
            std::vector<float> vkOut;
            int vkW = 0;
            int vkH = 0;
            bool vkOk = session.load(path, &err) && session.ensureTile(tile, &err);
            if (vkOk)
                vkOk = session.run(input.data(), &vkOut, &vkW, &vkH, &err);
            ++compared;
            if (!ortOk || !vkOk) {
                std::fprintf(stderr, "vk-compare: %s ort=%d vk=%d err=%s\n", qPrintable(file),
                             int(ortOk), int(vkOk), qPrintable(err));
                continue;
            }
            if (ortOut.size() != vkOut.size() || ortW != vkW || ortH != vkH) {
                std::fprintf(stderr,
                             "vk-compare: %s SHAPE ort=%dx%d(%lld) vk=%dx%d(%lld)\n",
                             qPrintable(file), ortW, ortH, (long long)ortOut.size(), vkW, vkH,
                             (long long)vkOut.size());
                continue;
            }
            if (qEnvironmentVariableIsSet("JM_VK_COMPARE_HEAD")) {
                QStringList av, bv;
                for (int k = 0; k < 4; ++k) {
                    av << QString::number(double(ortOut[size_t(k)]), 'g', 6);
                    bv << QString::number(double(vkOut[size_t(k)]), 'g', 6);
                }
                std::fprintf(stderr, "vk-compare: %s head ort=[%s] vk=[%s]\n", qPrintable(file),
                             qPrintable(av.join(QLatin1String(","))),
                             qPrintable(bv.join(QLatin1String(","))));
            }
            double maxAbs = 0.0;
            double sumAbs = 0.0;
            double refMax = 0.0;
            int nonFinite = 0;
            qint64 worst = -1;
            for (size_t i = 0; i < ortOut.size(); ++i) {
                const double e = std::fabs(double(ortOut[i]) - double(vkOut[i]));
                if (!std::isfinite(e))
                    ++nonFinite;
                sumAbs += e;
                refMax = qMax(refMax, std::fabs(double(ortOut[i])));
                if (e > maxAbs) {
                    maxAbs = e;
                    worst = qint64(i);
                }
            }
            const double meanAbs = sumAbs / double(ortOut.size());
            // 判据：绝对误差或相对量程，二者取大。
            //   fp32 存储：1e-3（fp32 多 pass 累加允许这点漂移）
            //   fp16 存储：2.5e-2（用户定夺的 fp16 基线：half 存储舍入在
            //   30~90 层深网络上累积的实测量级，NCNN fp16 同等行为、视觉无差；
            //   与 Winograd 无关——JM_VK_WINO=0 对照 FAIL 集一致）。
            //   显式关掉 JM_VK_FP16 后可用 1e-3 基线复核数值路径。
            const double tol = SrVk::Fp16Active() ? 2.5e-2 : 1e-3;
            const bool pass = nonFinite == 0 && maxAbs <= qMax(tol, refMax * tol);
            same += pass ? 1 : 0;
            std::fprintf(stderr,
                         "vk-compare: %s %s out=%dx%d maxAbs=%g meanAbs=%g refMax=%g "
                         "nonFinite=%d worst=%lld tol=%g fp16=%d\n",
                         qPrintable(file), pass ? "PASS" : "FAIL", vkW, vkH, maxAbs, meanAbs,
                         refMax, nonFinite, (long long)worst, tol, int(SrVk::Fp16Active()));
        }
        std::fprintf(stderr, "vk-compare: compared=%d pass=%d fail=%d\n", compared, same,
                     compared - same);
        SrEngine::Shutdown();
        return (compared > 0 && same == compared) ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("vk-probe"))) {
        QString report;
        const bool ok = SrVk::Probe(&report);
        qWarning("%s", report.toUtf8().constData());
        qWarning("vk-probe: pass=%d", int(ok));
        return ok ? 0 : 2;
    }

    // 设置页"性能测试"行的底层回归：对当前模型跑一组建档基准并打印推荐
    if (parser.isSet(QStringLiteral("sr-perf-test"))) {
        const QString srModel = parser.value(QStringLiteral("sr-model"));
        const QVector<SrEngine::SrPerfRow> rows =
            SrEngine::RunPerfProbe(QVector<int>(), 1, srModel);
        for (const SrEngine::SrPerfRow &r : rows) {
            std::fprintf(stderr, "sr-perf: %s ok=%d ms=%.0f actMb=%lld memMb=%lld note=%s\n",
                         qPrintable(r.label), int(r.ok), r.ms, (long long)r.actMb,
                         (long long)r.memDeltaMb, qPrintable(r.note));
        }
        std::fprintf(stderr, "sr-perf: recommend %s\n",
                     qPrintable(SrEngine::PerfRecommendation(rows)));
        std::fprintf(stderr, "sr-perf: tilePref=%d\n", SrEngine::TilePreference());
        SrEngine::Shutdown();
        return 0;
    }

    // 量化模型数值验收：同一 tile 上 NPU 与 CPU 的输出差（桌面无 QNN EP，
    // 会打印 compare=skip(npu=not-htp...)；真机才是有效入口）
    if (parser.isSet(QStringLiteral("sr-npu-compare"))) {
        int npuMs = 0;
        int cpuMs = 0;
        double mx = -1.0;
        double ps = -1.0;
        const QString rep = SrEngine::CompareNpuCpu(
            parser.value(QStringLiteral("sr-npu-compare")), &npuMs, &cpuMs, &mx, &ps);
        std::fprintf(stderr, "sr-compare: %s\n", qPrintable(rep));
        std::fprintf(stderr, "sr-compare: npu=%dms cpu=%dms maxAbs=%.6g psnr=%.2f\n", npuMs,
                     cpuMs, mx, ps);
        SrEngine::Shutdown();
        return 0;
    }

    if (parser.isSet(QStringLiteral("vk-stream-test"))) {        // 整图 tile 流水验收：合成 <size>x<size> 渐变图走 Upscale（auto 后端，
        // tile 数 >=3 时进入 Session::runTiles 双缓冲流水），打印后端/耗时/
        // 像素哈希。与 JM_VK_NO_STREAM=1 的运行对比哈希应完全一致
        //（流水只是提交节奏不同，数值路径与逐 tile run() 相同）。
        const int size = qMax(64, parser.value(QStringLiteral("vk-stream-test")).toInt());
        QImage img(size, size, QImage::Format_RGB888);
        for (int y = 0; y < size; ++y) {
            uchar *line = img.scanLine(y);
            for (int x = 0; x < size; ++x) {
                line[x * 3 + 0] = uchar((x * 7 + y * 3) % 256);
                line[x * 3 + 1] = uchar((x * 2 + y * 11) % 256);
                line[x * 3 + 2] = uchar((x + y * 5) % 256);
            }
        }
        QElapsedTimer t;
        t.start();
        const SrEngine::Result r = SrEngine::Upscale(img);
        quint32 hash = 2166136261u;
        if (!r.image.isNull()) {
            const QImage argb = r.image.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < argb.height(); ++y) {
                const uint *p = reinterpret_cast<const uint *>(argb.constScanLine(y));
                for (int x = 0; x < argb.width(); ++x)
                    hash = (hash ^ uint(p[x])) * 16777619u;
            }
        }
        std::fprintf(stderr,
                     "vk-stream-test: size=%d out=%dx%d ms=%.0f pixelHash=%08x "
                     "model=%s backend=%s err=%s\n",
                     size, r.image.width(), r.image.height(), r.ms, hash,
                     qPrintable(r.model), qPrintable(SrEngine::Status()),
                     qPrintable(r.error));
        SrEngine::Shutdown();
        return r.image.isNull() ? 2 : 0;
    }

    if (parser.isSet(QStringLiteral("sr-backend"))) {
        // 先触发会话建立，BackendReport() 里的 chosen 才是真实生效的后端：
        // 之前先打印报告，chosen 还是初始 cpu，容易被误读成"没跑在 NPU 上"。
        const QString srStatus = SrEngine::Status();
        std::fprintf(stderr, "%s\n", qPrintable(SrEngine::BackendReport()));
        std::fprintf(stderr, "sr-backend: status=%s model=%s\n", qPrintable(srStatus),
                     qPrintable(SrEngine::CurrentModel()));
        SrEngine::Shutdown();
        return 0;
    }

    if (parser.isSet(QStringLiteral("node-bench-selftest"))) {
        // 纯离线：不建窗口、不联网、不读写 ui.ini，退出码即可作为 CI 断言。
        QString report;
        const bool pass = NodeBench::SelfTest(&report);
        std::fprintf(stderr, "%s\n", qPrintable(report));
        std::fflush(stderr);
        return pass ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("nas-selftest"))) {
        const QString report = NasStore::SelfTest();
        std::fprintf(stderr, "%s\n", qPrintable(report));
        const bool pass = report.contains(QStringLiteral("validate-local=1"))
                          && report.contains(QStringLiteral("probe-local=1"))
                          && report.contains(QStringLiteral("probe-missing=0"))
                          && report.contains(QStringLiteral("validate-bad-dav=0"));
        std::fprintf(stderr, "nas-selftest: pass=%d\n", int(pass));
        return pass ? 0 : 2;
    }

    if (parser.isSet(QStringLiteral("account-selftest"))) {
        // 离线可判定的部分：字段编码与站点文案解析（不打网络）
        const QByteArray form = JmAccount::EncodeForm(
            {{QStringLiteral("username"), QStringLiteral("u ser")},
             {QStringLiteral("password"), QStringLiteral("p&w=d")},
             {QStringLiteral("submit_signup"), QString()}});
        std::fprintf(stderr, "account-selftest: form=%s\n", form.constData());
        const auto err = JmAccount::ParseToastr(
            QByteArray("<script>toastr['error'](\"账号已被注册\")</script>"));
        std::fprintf(stderr, "account-selftest: toastr-error ok=%d msg=%s\n", int(err.first),
                     qPrintable(err.second));
        const auto okMsg = JmAccount::ParseToastr(
            QByteArray("<script>toastr['success'](\"注册成功\")</script>"));
        std::fprintf(stderr, "account-selftest: toastr-success ok=%d msg=%s\n", int(okMsg.first),
                     qPrintable(okMsg.second));
        const auto empty = JmAccount::ParseToastr(QByteArray("<html></html>"));
        std::fprintf(stderr, "account-selftest: toastr-none ok=%d msgEmpty=%d\n",
                     int(empty.first), int(empty.second.isEmpty()));
        const QStringList bases = JmAccount::DefaultWebBases();
        std::fprintf(stderr, "account-selftest: web-bases=%d first=%s\n", int(bases.size()),
                     qPrintable(bases.value(0)));
        const bool pass = !err.first && err.second == QStringLiteral("账号已被注册") && okMsg.first
                          && okMsg.second == QStringLiteral("注册成功") && !empty.first
                          && form.contains("p%26w%3Dd");
        return pass ? 0 : 2;
    }


    // ---- 账号网页流程验收入口（指向 JM_WEB_BASE，默认 mock 服务）----
    // 用法：--account-flow register:user:email:pass:code | reset:email | confirm:user:pass | captcha
    if (parser.isSet(QStringLiteral("account-flow"))) {
        const QString spec = parser.value(QStringLiteral("account-flow"));
        const QStringList parts = spec.split(QLatin1Char(':'));
        const QString kind = parts.value(0);
        JmAccount acc;
        QEventLoop loop;
        int exitCode = 2;
        QObject::connect(&acc, &JmAccount::Log, &loop,
                         [](const QString &l) { qWarning("account: %s", qPrintable(l)); });

        if (kind == QStringLiteral("register")) {
            QObject::connect(&acc, &JmAccount::RegisterFinished, &loop,
                             [&](bool ok, const QString &msg) {
                                 std::fprintf(stderr, "account-flow: register ok=%d msg=%s\n",
                                              int(ok), qPrintable(msg));
                                 exitCode = ok ? 0 : 2;
                                 loop.quit();
                             });
            acc.Register(parts.value(1), parts.value(2), parts.value(3), parts.value(3),
                         QStringLiteral("Male"), parts.value(4));
        } else if (kind == QStringLiteral("reset")) {
            QObject::connect(&acc, &JmAccount::ResetPasswordFinished, &loop,
                             [&](bool ok, const QString &msg) {
                                 std::fprintf(stderr, "account-flow: reset ok=%d msg=%s\n",
                                              int(ok), qPrintable(msg));
                                 exitCode = ok ? 0 : 2;
                                 loop.quit();
                             });
            acc.ResetPassword(parts.value(1));
        } else if (kind == QStringLiteral("confirm")) {
            QObject::connect(&acc, &JmAccount::VerifyMailFinished, &loop,
                             [&](bool ok, const QString &msg) {
                                 std::fprintf(stderr, "account-flow: confirm ok=%d msg=%s\n",
                                              int(ok), qPrintable(msg));
                                 exitCode = ok ? 0 : 2;
                                 loop.quit();
                             });
            acc.ResendVerifyMail(parts.value(1), parts.value(2));
        } else if (kind == QStringLiteral("captcha")) {
            QObject::connect(&acc, &JmAccount::CaptchaReady, &loop,
                             [&](bool ok, const QByteArray &img, const QString &type,
                                 const QString &err) {
                                 const QImage decoded = QImage::fromData(img);
                                 std::fprintf(stderr,
                                              "account-flow: captcha ok=%d bytes=%lld type=%s "
                                              "decoded=%dx%d err=%s\n",
                                              int(ok), static_cast<long long>(img.size()),
                                              qPrintable(type), decoded.width(), decoded.height(),
                                              qPrintable(err));
                                 exitCode = (ok && !decoded.isNull()) ? 0 : 2;
                                 loop.quit();
                             });
            acc.FetchCaptcha();
        } else {
            std::fprintf(stderr, "account-flow: unknown kind %s\n", qPrintable(kind));
            return 2;
        }
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
        std::fprintf(stderr, "account-flow: base=%s exit=%d\n", qPrintable(acc.CurrentBase()),
                     exitCode);
        return exitCode;
    }

    // 升级迁移 + 损坏隔离：必须先于任何读写 ui.ini / session.ini 的分支
    //（下面的 update-check / export-logs 之后的 AppFont/SessionStore 初始化都会碰它们）。
    DataMigrator::Run();

    if (parser.isSet(QStringLiteral("node-probe"))) {
        // 单节点排障：把工作线程里的阻塞探测单独拎出来跑一次，便于和
        // --use-node（主线程 NodeHttp 通道）逐项对照，不启动任何界面。
        ProxyConfig::Load();
        NodeBench::Task t;
        t.id = 0;
        t.url = QUrl(ProxyConfig::GetApiUrl2(5) + QStringLiteral("/latest/?page=0"));
        t.connectIp = parser.value(QStringLiteral("node-probe"));
        t.sniHost = t.url.host();
        // JM_BENCH_TIMEOUT 只给排障用：观察慢节点在更长截止下的真实耗时分布
        t.timeoutMs = qBound(500, qEnvironmentVariableIntValue("JM_BENCH_TIMEOUT") > 0
                                       ? qEnvironmentVariableIntValue("JM_BENCH_TIMEOUT")
                                       : 3000,
                             20000);
        t.maxBytes = 2 * 1024 * 1024;
        QAtomicInteger<int> cancel(0);
        const NodeBench::Outcome r = NodeBench::ProbeBlocking(t, 0, cancel);
        std::fprintf(stderr,
                     "node-probe: ip=%s host=%s ok=%d timedOut=%d ms=%lld bytes=%lld err=%s\n",
                     qPrintable(t.connectIp), qPrintable(t.sniHost), int(r.ok),
                     int(r.timedOut), (long long)r.elapsedMs, (long long)r.bytes,
                     qPrintable(r.error));
        std::fflush(stderr);
        return r.ok ? 0 : 2;
    }

    // --export-data / --import-data：登录态与个性化设置的目录级导出/导入。
    // 用途：卸载重装、跨设备、签名变更后的迁移（docs/data_persistence_upgrade_analysis.md §3.3）。
    if (parser.isSet(QStringLiteral("export-data"))) {
        const QString err = DataMigrator::ExportData(parser.value(QStringLiteral("export-data")));
        if (err.isEmpty())
            std::fprintf(stderr, "export-data: ok\n");
        else {
            std::fprintf(stderr, "export-data: FAIL %s\n", qPrintable(err));
            return 2;
        }
        return 0;
    }
    if (parser.isSet(QStringLiteral("import-data"))) {
        const QString err = DataMigrator::ImportData(parser.value(QStringLiteral("import-data")));
        if (err.isEmpty())
            std::fprintf(stderr, "import-data: ok（导入的配置立即生效）\n");
        else {
            std::fprintf(stderr, "import-data: FAIL %s\n", qPrintable(err));
            return 2;
        }
        return 0;
    }

    if (parser.isSet(QStringLiteral("export-logs"))) {
        const QString dir = parser.value(QStringLiteral("export-logs"));
        QString error;
        const QStringList extra = {QStringLiteral("来源: --export-logs（CLI）")};
        const QString path = LogExport::Export(dir, extra, &error);
        if (path.isEmpty()) {
            std::fprintf(stderr, "export-logs: FAIL %s\n", qPrintable(error));
            return 2;
        }
        std::fprintf(stderr, "export-logs: ok path=%s bytes=%lld dir=%s\n", qPrintable(path),
                     static_cast<long long>(QFileInfo(path).size()),
                     qPrintable(LogExport::LogDir()));
        return 0;
    }

    if (parser.isSet(QStringLiteral("update-check"))) {
        if (parser.isSet(QStringLiteral("update-base")))
            qputenv("JM_UPDATE_BASE", parser.value(QStringLiteral("update-base")).toUtf8());
        UpdateChecker checker;
        QEventLoop loop;
        bool hasUpdate = false;
        bool checkOk = false;
        QString version;
        QString error;
        QString changelog;
        int configVer = -1;
        QString configTime;
        QObject::connect(&checker, &UpdateChecker::CheckFinished, &loop,
                         [&](bool ok, bool has, const QString &v, const QString &e) {
                             checkOk = ok;
                             hasUpdate = has;
                             version = v;
                             error = e;
                             if (!ok || !has)
                                 loop.quit();   // 无更新/失败：不必再等日志
                         });
        QObject::connect(&checker, &UpdateChecker::ChangelogReady, &loop,
                         [&](bool ok, const QString &, const QString &text, const QString &e) {
                             changelog = ok ? text : QString();
                             if (!ok)
                                 error = e;
                             loop.quit();
                         });
        QObject::connect(&checker, &UpdateChecker::ConfigReady, &loop,
                         [&](bool ok, const UpdateChecker::Config &cfg, const QString &) {
                             if (ok) {
                                 configVer = cfg.ver;
                                 configTime = cfg.verTime;
                             }
                         });
        QObject::connect(&checker, &UpdateChecker::Log, &loop,
                         [](const QString &l) { qWarning("update: %s", qPrintable(l)); });
        checker.FetchConfig();
        checker.Check();
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
        std::fprintf(stderr,
                     "update-check: ok=%d hasUpdate=%d version=%s local=%s err=%s changelog=%d\n",
                     int(checkOk), int(hasUpdate), qPrintable(version),
                     qPrintable(UpdateChecker::ReleaseLine()), qPrintable(error),
                     int(!changelog.isEmpty()));
        std::fprintf(stderr, "update-check: config ver=%d time=%s base=%s\n", configVer,
                     qPrintable(configTime), qPrintable(checker.Bases().value(0)));
        return checkOk ? 0 : 2;
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
        qWarning("path-report: schemaVersion=%d (current=%d)", DataMigrator::SchemaVersion(),
                 DataMigrator::kCurrentSchema);
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
    }    if (parser.isSet(QStringLiteral("demo-drawer")))
        window.DemoOpenDrawer();

    // ---- 本轮补齐能力的页面级验收（都在真实页面上跑同一条业务路径）----
    // 只切页、不额外发请求：OnEnter 就是真实入口（否则同一次运行会拉两遍）。
    if (parser.isSet(QStringLiteral("week-report")))
        window.Go(PageId::Week, false);
    if (parser.isSet(QStringLiteral("watchlist-report")))
        window.Go(PageId::RemoteHistory, false);
    if (parser.isSet(QStringLiteral("mycomment-report")))
        window.Go(PageId::MyComment, false);
    if (parser.isSet(QStringLiteral("about-report"))) {
        window.Go(PageId::About, false);
        window.CheckUpdates();
        QTimer::singleShot(14000, &window,
                           [&window] { qWarning("%s", qPrintable(window.AboutReport())); });
    }
    if (parser.isSet(QStringLiteral("nas-ui-selftest"))) {
        window.Go(PageId::Nas, false);
        qWarning("%s", qPrintable(window.NasSelfTest()));
    }
    if (parser.isSet(QStringLiteral("comment-report"))) {
        if (auto *cp = qobject_cast<CommentPage *>(window.FindPage(PageId::Comment))) {
            const QString aid = parser.value(QStringLiteral("comment-report"));
            window.Go(PageId::Comment, false);
            cp->SetAlbum(aid, QStringLiteral("作品 %1").arg(aid));
        }
    }
    if (parser.isSet(QStringLiteral("login-panel"))) {
        window.Go(PageId::Login, false);
        if (auto *lp = qobject_cast<LoginPage *>(window.FindPage(PageId::Login))) {
            const QString kind = parser.value(QStringLiteral("login-panel"));
            lp->ShowPanel(kind == QLatin1String("forgot") ? LoginPage::Panel::Forgot
                                                          : LoginPage::Panel::Register);
            qWarning("login-panel: mode=%s status=%s", qPrintable(kind),
                     qPrintable(lp->PanelStatusText()));
        }
    }


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
            QStringLiteral("--vk-bench"),       QStringLiteral("--vk-tile"),
            QStringLiteral("--vk-only"),        QStringLiteral("--vk-fp16"),
            QStringLiteral("--vk-wino"),        QStringLiteral("--qnn-probe"),
            QStringLiteral("--qnn-debug"),
            QStringLiteral("--sr-model"),     QStringLiteral("--sr-bench"),
            QStringLiteral("--sr-npu-compare"),
            QStringLiteral("--sr-sheet-scroll"),
            QStringLiteral("--export-data"),    QStringLiteral("--import-data"),
            QStringLiteral("--tapguard-watch"),  // 秒数：真机手势验收时持续落盘状态
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
        // 触摸防误触回归：手机上没有 stdout，逐项结果同时写进私有目录，
        // adb pull 即可读（部分 ROM 会整段丢 logcat）。
        // A/B 对照：同一套手势在"修复前判定"下必须 fail，用来证明用例测的就是本轮补的链路
        Touch::SetTapGuardLegacy(flags.contains(QStringLiteral("--tapguard-legacy")));
        if (flags.contains(QStringLiteral("--tapguard-selftest"))) {
            QTimer::singleShot(1200, &window, [&window, jmArgs] {
                QString detail;
                const bool ok = window.TapGuardSelfTest(&detail);
                const QString out = WriteRunReport(
                    QStringLiteral("tapguard-selftest.txt"),
                    QStringList{QStringLiteral("jm.args=%1").arg(jmArgs),
                     ok ? QStringLiteral("result=pass") : QStringLiteral("result=fail"),
                     detail});
                qWarning("tapguard-selftest: %s file=%s", ok ? "pass" : "fail",
                         out.isEmpty() ? "unavailable" : qPrintable(out));
            });
        }
        // 真机/模拟器注入真实 touch 手势时，持续把「当前页 + 被吞掉的抬起次数」
        // 落盘：划过可点区域后页号不变、swallowed 递增即判定防误触生效。
        if (opt.contains(QStringLiteral("--tapguard-watch"))) {
            const int secs = qBound(1, opt.value(QStringLiteral("--tapguard-watch")).toInt(), 120);
            const qint64 t0 = QDateTime::currentMSecsSinceEpoch();
            auto *watch = new QTimer(&window);
            QObject::connect(watch, &QTimer::timeout, &window,
                             [&window, watch, t0, secs, jmArgs] {
                const qint64 ms = QDateTime::currentMSecsSinceEpoch() - t0;
                const int page = int(window.CurrentPage());
                const int swallowed = Touch::TapGuardSwallowed();
                const QString out = WriteRunReport(
                    QStringLiteral("tapguard-watch.txt"),
                    QStringList{QStringLiteral("jm.args=%1").arg(jmArgs),
                     QStringLiteral("elapsed=%1ms").arg(ms),
                     QStringLiteral("page=%1").arg(page),
                     QStringLiteral("swallowed=%1").arg(swallowed),
                     QStringLiteral("trace:"),
                     Touch::TapGuardTrace()});
                qWarning("tapguard-watch: t=%lldms page=%d swallowed=%d file=%s",
                         (long long)ms, page, swallowed,
                         out.isEmpty() ? "unavailable" : qPrintable(out));
                if (ms >= qint64(secs) * 1000) {
                    watch->stop();
                    watch->deleteLater();
                }
            });
            watch->start(400);
        }
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
        // 模型选择浮层（真机）：桌面同义。滚动值周期上报到 logcat —— 真机外部分目录
        // 对 shell 不可见（pull 不到应用截图），滑动验证只能靠日志里的 scrollbar 值。
        if (flags.contains(QStringLiteral("--sr-sheet"))) {
            const int scrollTo = opt.value(QStringLiteral("--sr-sheet-scroll")).toInt();
            QTimer::singleShot(400, &window, [&window, scrollTo] {
                QWidget *page = window.FindPage(window.CurrentPage());
                if (!page)
                    page = window.FindPage(PageId::Sr);
                if (!page)
                    return;
                QWidget *sheet = SrModelSheet::Show(page, SrEngine::CurrentModel());
                if (!sheet)
                    return;
                if (scrollTo > 0) {
                    QTimer::singleShot(200, sheet, [sheet, scrollTo] {
                        if (auto *list = sheet->findChild<QListWidget *>(
                                QStringLiteral("SrModelList")))
                            list->verticalScrollBar()->setValue(scrollTo);
                    });
                }
                auto *timer = new QTimer(sheet);
                QObject::connect(timer, &QTimer::timeout, sheet, [sheet] {
                    auto *list = sheet->findChild<QListWidget *>(
                        QStringLiteral("SrModelList"));
                    if (list)
                        qWarning("sr-sheet-scroll-now: %d/%d", list->verticalScrollBar()->value(),
                                 list->verticalScrollBar()->maximum());
                });
                timer->start(400);
                QTimer::singleShot(10000, sheet, [timer] { timer->stop(); });
            });
        }
        // 测速回归（真机与桌面同义）：--sr-bench all|<模型名>[,<模型名>...]
        // 后台线程跑，避免 ANR；逐行 qWarning 进 logcat。
        if (opt.contains(QStringLiteral("--sr-bench"))) {
            const QString sel = opt.value(QStringLiteral("--sr-bench"));
            const QVector<SrEngine::BenchHw> hws = SrEngine::BenchHardware();
            QStringList hwIds;
            bool hadGpu = false;
            for (const SrEngine::BenchHw &h : hws) {
                qWarning("sr-bench-hw: id=%s short=%s available=%d label=%s note=%s",
                         qPrintable(h.id), qPrintable(h.shortLabel), int(h.available),
                         qPrintable(h.label), qPrintable(h.note));
                if (h.available)
                    hwIds << h.id;
                if (h.id.startsWith(QLatin1String("gpu")))
                    hadGpu = true;
            }
            QStringList models;
            if (sel == QLatin1String("all")) {
                for (const SrEngine::ModelInfo &mi : SrEngine::InstalledModels())
                    models << mi.name;
            } else {
                models = sel.split(QLatin1Char(','), Qt::SkipEmptyParts);
            }
            const QString original = SrEngine::CurrentModel();
            qWarning("sr-bench-start: models=%d hw=%s original=%s", int(models.size()),
                     qPrintable(hwIds.join(QLatin1Char(','))), qPrintable(original));
            QtConcurrent::run([models, hwIds, hadGpu, original] {
                QElapsedTimer timer;
                timer.start();
                for (const QString &name : models) {
                    const QVector<SrEngine::BenchSample> rows =
                        SrEngine::BenchModel(name, hwIds, 192, 3);
                    for (const SrEngine::BenchSample &r : rows)
                        qWarning("sr-bench: model=%s hw=%s ok=%d ms=%.1f note=%s",
                                 qPrintable(name), qPrintable(r.hwId), int(r.ok), r.ms,
                                 qPrintable(r.note));
                }
                SrEngine::BenchFinish(original, hadGpu);
                qWarning("sr-bench-done: %.1fs restored=%s", timer.elapsed() / 1000.0,
                         qPrintable(SrEngine::CurrentModel()));
            });
        }
        if (flags.contains(QStringLiteral("--history-clear")))
            SearchHistory::Clear();
        if (flags.contains(QStringLiteral("--path-report"))) {
            const QStringList lines = AppPaths::Report().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
            for (const QString &line : lines)
                qWarning("%s", line.toUtf8().constData());
            qWarning("path-report: schemaVersion=%d (current=%d)", DataMigrator::SchemaVersion(),
                     DataMigrator::kCurrentSchema);
        }
        // 手机上的导出/导入：目录写 /sdcard/Android/data/<pkg>/files/ 下，
        // adb pull / adb push 可及。导入在本会话立即生效（下次启动读盘）。
        if (opt.contains(QStringLiteral("--export-data"))) {
            const QString err = DataMigrator::ExportData(opt.value(QStringLiteral("--export-data")));
            qWarning("export-data: %s", err.isEmpty() ? "ok" : qPrintable(err));
        }
        if (opt.contains(QStringLiteral("--import-data"))) {
            const QString err = DataMigrator::ImportData(opt.value(QStringLiteral("--import-data")));
            qWarning("import-data: %s%s", err.isEmpty() ? "ok" : qPrintable(err),
                     err.isEmpty() ? "（下次启动读盘生效）" : "");
            if (err.isEmpty()) {
                // 导入后立即重读：验收参数（--data-report）看到的是导入结果
                SessionStore::Load();
                ProxyConfig::Load();
            }
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

        // 真机 Vulkan 验收：--vk-report 一键跑设备/算子自检 + 全量模型扫描 + 吞吐；
        // --vk-sweep 只扫模型，--vk-bench N 只测吞吐，--vk-only 过滤文件名。
        // 分片执行在函数里用 QTimer(0) 串起来，这里只负责延后到 UI 建好之后启动。
        // fp16 / winograd / QNN 探测开关：am start 无法给应用传环境变量，用
        // CLI 参数落地 env（后续 singleShot 的测试都晚于这里的 setenv）。
        // 放在 vk-report 分支之外：--sr-perf-test 单独跑时同样生效。
        if (opt.contains(QStringLiteral("--vk-fp16")))
            qputenv("JM_VK_FP16", opt.value(QStringLiteral("--vk-fp16")).toUtf8());
        if (opt.contains(QStringLiteral("--vk-wino")))
            qputenv("JM_VK_WINO", opt.value(QStringLiteral("--vk-wino")).toUtf8());
        if (opt.contains(QStringLiteral("--qnn-probe")))
            qputenv("JM_QNN_PROBE", opt.value(QStringLiteral("--qnn-probe")).toUtf8());
        if (opt.contains(QStringLiteral("--qnn-debug")))
            qputenv("JM_QNN_DEBUG", opt.value(QStringLiteral("--qnn-debug")).toUtf8());
        // 性能测试（真机入口）：对当前模型跑 GPU/NPU/CPU 三档并落盘
        // /sdcard/Android/data/org.jmcomic.ui/files/sr-perf.txt（adb pull 可读）。
        // NPU 档在 QNN 运行库随包时自动实测，缺失时输出缺失原因。
        if (flags.contains(QStringLiteral("--sr-perf-test"))) {
            const QString srModel = opt.value(QStringLiteral("--sr-model"));
            QTimer::singleShot(1500, qApp, [srModel] {
                const QString jmArgsEcho = AndroidExtra("jm.args");
                const QVector<SrEngine::SrPerfRow> rows =
                    SrEngine::RunPerfProbe(QVector<int>(), 1, srModel);
                QStringList lines;
                lines << QStringLiteral("jm.args=%1").arg(jmArgsEcho);
                lines << QStringLiteral("env JM_QNN_PROBE=%1")
                          .arg(qgetenv("JM_QNN_PROBE").constData());
                lines << QStringLiteral("backend=%1").arg(SrEngine::BackendReport());
                lines << QStringLiteral("qnn=%1").arg(SrEngine::QnnProcStatus());
                lines << QStringLiteral("qnnlog=%1").arg(SrEngine::QnnProcLogTail());
                for (const SrEngine::SrPerfRow &r : rows) {
                    const QString line =
                        QStringLiteral("%1 ok=%2 ms=%3 actMb=%4 memMb=%5 note=%6")
                            .arg(r.label)
                            .arg(int(r.ok))
                            .arg(r.ms, 0, 'f', 0)
                            .arg(r.actMb)
                            .arg(r.memDeltaMb)
                            .arg(r.note);
                    lines << line;
                    qWarning("vk-report: sr-perf %s", line.toUtf8().constData());
                }
                const QString rec = SrEngine::PerfRecommendation(rows);
                lines << QStringLiteral("recommend %1").arg(rec);
                qWarning("vk-report: sr-perf recommend %s", qPrintable(rec));
                const QString out = WriteRunReport(QStringLiteral("sr-perf.txt"), lines);
                qWarning("vk-report: sr-perf file=%s",
                         out.isEmpty() ? "unavailable" : qPrintable(out));
            });
        }
        // 量化模型数值验收：同一 tile 上 NPU vs CPU。QDQ 图"跑通"不等于"算对"
        //（量化参数没解析、tile 尺寸不匹配时耗时正常但画面是错的），真机回归用它。
        if (opt.contains(QStringLiteral("--sr-npu-compare"))) {
            const QString cmpModel = opt.value(QStringLiteral("--sr-npu-compare"));
            QTimer::singleShot(1500, qApp, [cmpModel] {
                int npuMs = 0;
                int cpuMs = 0;
                double mx = -1.0;
                double ps = -1.0;
                const QString rep = SrEngine::CompareNpuCpu(cmpModel, &npuMs, &cpuMs, &mx, &ps);
                QStringList lines;
                lines << QStringLiteral("jm.args=%1").arg(AndroidExtra("jm.args"));
                lines << QStringLiteral("model=%1").arg(cmpModel);
                lines << QStringLiteral("backend=%1").arg(SrEngine::BackendReport());
                lines << QStringLiteral("qnn=%1").arg(SrEngine::QnnProcStatus());
                lines << rep;
                for (const QString &l : lines)
                    qWarning("vk-report: sr-compare %s", qPrintable(l));
                const QString out =
                    WriteRunReport(QStringLiteral("sr-compare.txt"), lines);
                qWarning("vk-report: sr-compare file=%s",
                         out.isEmpty() ? "unavailable" : qPrintable(out));
            });
        }
        if (flags.contains(QStringLiteral("--vk-report"))
            || flags.contains(QStringLiteral("--vk-sweep"))
            || opt.contains(QStringLiteral("--vk-bench"))) {
            bool tileOk = false;
            const int tileArg = opt.value(QStringLiteral("--vk-tile")).toInt(&tileOk);
            // 默认 tile 跟生产链路一致（TileForCurrentModel 含 watchdog 降档），
            // 否则 --vk-report 的吞吐数据与真实推理对不上
            const int tileAuto = SrEngine::TileForCurrentModel();
            const int tile =
                tileOk ? qBound(32, tileArg, 512)
                       : (tileAuto > 0 ? qBound(32, tileAuto, 512) : 192);
            bool benchOk = false;
            const int benchArg = opt.value(QStringLiteral("--vk-bench")).toInt(&benchOk);
            const bool report = flags.contains(QStringLiteral("--vk-report"));
            const int benchN = benchOk ? qMax(1, benchArg) : (report ? 5 : 0);
            const bool sweep = report || flags.contains(QStringLiteral("--vk-sweep"));
            const QString only = opt.value(QStringLiteral("--vk-only"));
            const bool compare = flags.contains(QStringLiteral("--vk-compare"));
            QTimer::singleShot(1500, qApp, [tile, benchN, sweep, only, compare] {
                RunAndroidVkReport(sweep, benchN, tile, only, compare);
            });
        }
    }
#else
    // 桌面与手机一致：启动即拉线上首页；失败进入错误态（不保留任何样例数据）。
    window.LoadRealData();
#endif
    // 运行时覆盖必须在任何测速入口之前落地；它只在内存里，不写 ui.ini。
    if (parser.isSet(QStringLiteral("node-threads"))) {
        const int raw = parser.value(QStringLiteral("node-threads")).toInt();
        NodeBench::SetThreadOverride(raw);
        std::fprintf(stderr, "node-threads: request=%d effective=%d (max=%d, 1=legacy)\n",
                     raw, NodeBench::ThreadOverride(), NodeBench::kMaxThreads);
    }
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
    if (parser.isSet(QStringLiteral("tapguard-legacy")))
        Touch::SetTapGuardLegacy(true);
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
        // 报告时机跟 --shot-delay 走（同一套策略见下面的 blank-report）：
        // 详情页的标题/标签/章节来自 /album，首页分区来自 /promote+/latest，
        // 固定 600ms 只能量到"加载中"的骨架；内容回来后才被撑宽的页面会被漏掉
        // （11-13 详情页 363dp：600ms 已经 hmax=87，但章节 16s 才回）。
        const int shotDelay = parser.value(QStringLiteral("shot-delay")).toInt();
        const int delayMs = parser.isSet(QStringLiteral("shot")) ? qMax(600, shotDelay - 1200) : 600;
        QTimer::singleShot(delayMs, &window, [&window] {
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
        // 异步内容（/album 的章节、封面、标签）可能在快照之后才把页面撑宽：
        // 横向范围一旦从 0 变成 >0 就补一条 late 记录，回归日志能直接定位"何时被撑宽"。
        for (QScrollArea *sa : window.findChildren<QScrollArea *>()) {
            auto fired = std::make_shared<bool>(false);
            QObject::connect(sa->horizontalScrollBar(), &QScrollBar::rangeChanged, sa,
                             [sa, fired](int, int max) {
                                 if (*fired || max <= 0 || !sa->isVisible())
                                     return;
                                 *fired = true;
                                 QWidget *inner = sa->widget();
                                 qWarning("scroll-report: late obj=%s hmax=%d viewport=%dx%d "
                                          "widget=%dx%d",
                                          qPrintable(sa->objectName().isEmpty()
                                                         ? QStringLiteral("-")
                                                         : sa->objectName()),
                                          max, sa->viewport()->width(), sa->viewport()->height(),
                                          inner ? inner->width() : -1,
                                          inner ? inner->height() : -1);
                             });
        }
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
    if (parser.isSet(QStringLiteral("settings-search"))) {
        const QString kw = parser.value(QStringLiteral("settings-search"));
        QTimer::singleShot(200, &window, [&window, kw] {
            if (auto *page = window.FindPage(PageId::Settings)) {
                if (auto *box = page->findChild<SearchBox *>())
                    box->SetText(kw);
            }
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
        const QString benchMode = parser.value(QStringLiteral("sr-sheet-bench"));
        const int cancelMs = parser.value(QStringLiteral("sr-sheet-cancel")).toInt();
        const QString filterKw = parser.value(QStringLiteral("sr-sheet-filter"));
        QTimer::singleShot(220, &window, [&window, scrollTo, benchMode, cancelMs, filterKw] {
            // 从"当前页"打开：设置页/图片超分页都能用同一个钩子截浮层
            QWidget *page = window.FindPage(window.CurrentPage());
            if (!page)
                page = window.FindPage(PageId::Sr);
            if (!page)
                return;
            auto *sheet = SrModelSheet::Show(page, SrEngine::CurrentModel());
            if (sheet && !filterKw.isEmpty()) {
                for (auto *chip : sheet->findChildren<Chip *>()) {
                    if (chip->text().contains(filterKw)) {
                        chip->click();
                        break;
                    }
                }
            }
            if (sheet && !benchMode.isEmpty()) {
                QString name = QStringLiteral("SrBenchSel");
                if (benchMode == QLatin1String("all"))
                    name = QStringLiteral("SrBenchAll");
                else if (benchMode == QLatin1String("group"))
                    name = QStringLiteral("SrBenchGroup");
                QPushButton *btn = sheet->findChild<QPushButton *>(name);
                if (btn)
                    btn->click();
                if (cancelMs > 0) {
                    QTimer::singleShot(cancelMs, sheet, [sheet] {
                        if (auto *stop = sheet->findChild<QPushButton *>(
                                QStringLiteral("SrBenchStop")))
                            stop->click();
                    });
                }
            }
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
