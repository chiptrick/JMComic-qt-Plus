#include "reader.h"

#include <QApplication>
#include <QClipboard>
#include <QDir>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMovie>
#include <QMouseEvent>
#include <QPainter>
#include <QPointingDevice>
#include <QPropertyAnimation>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QTouchEvent>
#include <QWheelEvent>
#include <QtConcurrent>
#include <QtMath>
#include <algorithm>
#include <cmath>

#include "frame_probe.h"
#include "jm_webp.h"

// 界面动效层只存在于 jmcomic_ui 目标（见 ui/CMakeLists.txt 的 JM_UI_MOTION）。
// c1shell 也编这一份 reader.cpp，所以这里必须允许"没有动效层"的构建。
#if defined(JM_UI_MOTION)
#  include "motion.h"
#endif

// UI 目标（jmcomic_ui）里用 AppPaths 统一配置根（支持 JM_UI_CONFIG 隔离）；
// c1shell 目标没有 app_paths.cpp，退回标准路径，两边都能编。
#if defined(__has_include)
#  if __has_include("app_paths.h")
#    include "app_paths.h"
#    define JM_READER_HAS_APPPATHS 1
#  endif
#endif

#if defined(__ANDROID__)
#include <android/log.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "C1Reader", __VA_ARGS__)
#else
#include <QDebug>
#define LOGI(...) qInfo(__VA_ARGS__)
#endif

namespace {

const char *kImageExts[] = {"jpg", "jpeg", "png", "webp", "bmp", "gif"};

// 与 Python mobile_ui_base 一致：16px 起拖，32px 才算一次滑动翻页
constexpr int kTouchSlop = 16;
constexpr int kTurnSlop = 32;
constexpr int kMaxCachePages = 8;
constexpr qint64 kMaxCacheBytes = 160LL * 1024 * 1024;

QString PrefIniPath()
{
#ifdef JM_READER_HAS_APPPATHS
    const QString p = AppPaths::ConfigFile(QStringLiteral("ui.ini"));
    if (!p.isEmpty())
        return p;
#endif
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        dir = QCoreApplication::applicationDirPath();
    QDir().mkpath(dir);
    return QDir(dir).filePath(QStringLiteral("ui.ini"));
}

QString DefaultReaderDir()
{
    // AppLocalDataLocation 在 Android 上是 /data/user/0/<pkg>/files —— 不需要存储权限，
    // adb 冒烟用 run-as 投测试图；桌面端是 ~/.local/share/<app>/reader。
    const QString appDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!appDir.isEmpty())
        return appDir + QStringLiteral("/reader");
    return QStringLiteral("/sdcard/c1_shell");
}

QImage DecodeFile(const QString &path)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QImage img = reader.read();
    if (!img.isNull())
        return img;
    // Qt 6.12 的安装包不带 qwebp 插件，JM 下发的内页全是 webp
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray data = f.readAll();
        if (JmWebp::IsWebp(data))
            img = JmWebp::Decode(data);
    }
    return img;
}

bool FileIsAnimated(const QString &path)
{
    QImageReader reader(path);
    return reader.supportsAnimation() && reader.imageCount() > 1;
}

QSizeF SourceSize(const QString &path)
{
    QImageReader reader(path);
    const QSize s = reader.size();
    if (s.isValid() && s.width() > 0 && s.height() > 0)
        return QSizeF(s);
    return QSizeF(800, 1200);   // 读不到头部时按 2:3 估
}

bool IsHorizontalScrollValue(int mode)
{
    return mode == 4 || mode == 5 || mode == 8;
}

QStringList ModeNameList()
{
    return {QStringLiteral("上下滚动"), QStringLiteral("左右翻页"),
            QStringLiteral("左右双页"), QStringLiteral("右左双页"),
            QStringLiteral("左右滚动"), QStringLiteral("右左滚动"),
            QStringLiteral("右左双页(滚轮正序)"), QStringLiteral("等宽模式"),
            QStringLiteral("右左滚动(滚轮正序)")};
}

}  // namespace

// ============================================================ 静态能力查询

bool ReaderView::IsDoubleMode(int mode)
{
    return mode == 2 || mode == 3 || mode == 6;
}

bool ReaderView::IsRtlMode(int mode)
{
    return mode == 3 || mode == 5 || mode == 6 || mode == 8;
}

bool ReaderView::IsScrollModeValue(int mode)
{
    return mode == 0 || mode == 4 || mode == 5 || mode == 8;
}

bool ReaderView::IsHorizontalPage(int mode)
{
    // 单页/双页：滑动翻页的手势轴是横向。不能拿 IsVertical 判断 ——
    // Python 的 isUpDown 把左右翻页也算成"纵向"（那是给滚轮留的旧语义），
    // 触摸照抄就会只认上下滑，真机现象"左右怎么滑都不翻"。
    return mode == 1 || mode == 2 || mode == 3 || mode == 6;
}

int ReaderView::ForwardDelta(int mode, int value)
{
    return IsRtlMode(mode) && (mode == 5 || mode == 8) ? -value : value;
}

QString ReaderView::ModeName(int mode)
{
    return ModeNameList().value(qBound(0, mode, 8));
}

QString ReaderView::ModeShortName(int mode)
{
    static const char *const kShort[] = {
        "上下滚动", "左右翻页", "左右双页", "右左双页", "左右滚动",
        "右左滚动", "右左双页·正", "等宽", "右左滚动·正",
    };
    return QString::fromUtf8(kShort[qBound(0, mode, 8)]);
}

QStringList ReaderView::ModeNames()
{
    return ModeNameList();
}

// ============================================================ 偏好

int ReaderView::PrefMode()
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    return qBound(0, s.value(QStringLiteral("reader/mode"), 1).toInt(), 8);
}

void ReaderView::SetPrefMode(int mode)
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("reader/mode"), qBound(0, mode, 8));
    s.sync();
}

int ReaderView::PrefTurnMs()
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    return qBound(500, s.value(QStringLiteral("reader/turnMs"), 5000).toInt(), 120000);
}

void ReaderView::SetPrefTurnMs(int ms)
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("reader/turnMs"), qBound(500, ms, 120000));
    s.sync();
}

int ReaderView::PrefScrollStep()
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    return qBound(1, s.value(QStringLiteral("reader/scrollStep"), 20).toInt(), 400);
}

void ReaderView::SetPrefScrollStep(int px)
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("reader/scrollStep"), qBound(1, px, 400));
    s.sync();
}

int ReaderView::PrefScrollMs()
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    return qBound(16, s.value(QStringLiteral("reader/scrollMs"), 100).toInt(), 2000);
}

void ReaderView::SetPrefScrollMs(int ms)
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("reader/scrollMs"), qBound(16, ms, 2000));
    s.sync();
}

int ReaderView::PrefManualStep()
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    return qBound(20, s.value(QStringLiteral("reader/manualStep"), 400).toInt(), 2000);
}

void ReaderView::SetPrefManualStep(int px)
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("reader/manualStep"), qBound(20, px, 2000));
    s.sync();
}

int ReaderView::PrefZoom()
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    return qBound(20, s.value(QStringLiteral("reader/zoom"), 100).toInt(), 500);
}

void ReaderView::SetPrefZoom(int percent)
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("reader/zoom"), qBound(20, percent, 500));
    s.sync();
}

bool ReaderView::PrefAutoSr()
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    return s.value(QStringLiteral("reader/autoSr"), false).toBool();
}

void ReaderView::SetPrefAutoSr(bool on)
{
    QSettings s(PrefIniPath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("reader/autoSr"), on);
    s.sync();
}

// ============================================================ 构造

ReaderView::ReaderView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setFocusPolicy(Qt::StrongFocus);
    // 触控由本类自己处理：先打标记，Touch::InstallScrollGestures 看到就跳过，
    // 否则 QScroller 会先接走 TouchBegin/Update，双指与滑动都收不到。
    setProperty("jmScroller", true);

    viewport()->setAttribute(Qt::WA_AcceptTouchEvents, true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    viewport()->setMouseTracking(true);
    viewport()->setCursor(Qt::ArrowCursor);

    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    horizontalScrollBar()->setSingleStep(40);
    verticalScrollBar()->setSingleStep(40);

    mode_ = static_cast<ReadMode>(PrefMode());
    zoom_ = PrefZoom() / 100.0;

    connect(horizontalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!scrollAnimating_ && !pinching_)
            UpdateCurrentFromScroll();
        UpdateGif();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!scrollAnimating_ && !pinching_)
            UpdateCurrentFromScroll();
        UpdateGif();
    });

    autoTimer_ = new QTimer(this);
    connect(autoTimer_, &QTimer::timeout, this, &ReaderView::AutoTick);

    autoSr_ = PrefAutoSr();
    srWatcher_ = new QFutureWatcher<SrEngine::Result>(this);
    connect(srWatcher_, &QFutureWatcherBase::finished, this, &ReaderView::OnSrFinished);
}

// ============================================================ 数据源

QStringList ReaderView::ImageFilesIn(const QString &dir) const
{
    QDir d(dir);
    if (!d.exists())
        return {};
    QStringList out;
    const QFileInfoList entries = d.entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
    for (const QFileInfo &fi : entries) {
        const QString ext = fi.suffix().toLower();
        for (const char *e : kImageExts) {
            if (ext == QLatin1String(e)) {
                out.append(fi.absoluteFilePath());
                break;
            }
        }
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

int ReaderView::LoadDir(const QString &dir)
{
    files_ = ImageFilesIn(dir);
    current_ = files_.isEmpty() ? -1 : 0;
    zoom_ = 1.0;
    cache_.clear();
    cacheOrder_.clear();
    cacheBytes_ = 0;
    animFlags_.clear();
    srWanted_.clear();
    srCache_.clear();
    srPending_.clear();
    if (files_.isEmpty()) {
        emit StatusMessage(QStringLiteral("reader: no images in %1").arg(dir));
        emit PageChanged(0, 0, QString());
        viewport()->update();
        return 0;
    }
    LOGI("loaded %d pages from %s", files_.size(), qPrintable(dir));
    emit StatusMessage(QStringLiteral("reader: %1 pages from %2").arg(files_.size()).arg(dir));
    Relayout();
    ShowPage(0);
    return files_.size();
}

int ReaderView::LoadFiles(const QStringList &files)
{
    progressive_ = false;
    files_.clear();
    for (const QString &path : files) {
        if (!path.isEmpty() && QFileInfo::exists(path))
            files_ << path;
    }
    current_ = files_.isEmpty() ? -1 : 0;
    zoom_ = 1.0;
    cache_.clear();
    cacheOrder_.clear();
    cacheBytes_ = 0;
    animFlags_.clear();
    srWanted_.clear();
    srCache_.clear();
    srPending_.clear();
    if (files_.isEmpty()) {
        emit StatusMessage(QStringLiteral("reader: online list empty"));
        emit PageChanged(0, 0, QString());
        viewport()->update();
        return 0;
    }
    LOGI("loaded %d online pages", files_.size());
    emit StatusMessage(QStringLiteral("reader: %1 online pages").arg(files_.size()));
    Relayout();
    ShowPage(0);
    return files_.size();
}

// ---------------------------------------------------------------- 渐进加载
//
// files_ 的每个槽位先在 BeginProgressive 里占位（空串），SetProgressivePage
// 逐张补上。空串在 SourceSize/DecodeFile 里都是安全的（读不到尺寸按 2:3 估），
// 所以占位期排版仍然成立，只是画"加载中…"。
void ReaderView::BeginProgressive(int total)
{
    progressive_ = total > 0;
    files_.clear();
    for (int i = 0; i < total; ++i)
        files_ << QString();
    current_ = files_.isEmpty() ? -1 : 0;
    zoom_ = 1.0;
    cache_.clear();
    cacheOrder_.clear();
    cacheBytes_ = 0;
    animFlags_.clear();
    srWanted_.clear();
    srCache_.clear();
    srPending_.clear();
    if (files_.isEmpty()) {
        emit PageChanged(0, 0, QString());
        viewport()->update();
        return;
    }
    LOGI("progressive start pages=%d", total);
    Relayout();
    ShowPage(0);
}

bool ReaderView::SetProgressivePage(int index, const QString &path)
{
    if (index < 0 || index >= files_.size() || path.isEmpty())
        return false;
    if (files_.at(index) == path)
        return false;
    const bool wasPlaceholder = files_.at(index).isEmpty();
    files_[index] = path;

    // 只解码"会出现在屏幕上"的页：翻页模式下是当前页（含双页搭档），滚动模式下
    // 是当前页前后各一页。其余页等翻到它时再按需解码，内存与耗时都可控。
    const bool onScreen = index == current_ || (IsDoublePage() && index == current_ + 1)
                          || (IsScrollMode() && qAbs(index - current_) <= 1);
    if (!onScreen)
        return false;

    if (wasPlaceholder) {
        // 这一页的尺寸从估值变成真实值：先解码，再重排并锚住当前页，
        // 否则内容会在用户眼皮底下跳动（滚动模式下尤其明显）。
        ImageAt(index);
        RelayoutAnchored();
    } else {
        ImageAt(index);
    }
    UpdateGif();
    viewport()->update();
    emit PageChanged(qMax(0, current_) + 1, files_.size(), files_.value(qMax(0, current_)));
    // 自动超分等文件真正到货后再发起（占位期不排任务）
    if (onScreen)
        QTimer::singleShot(0, this, [this, index] {
            if (current_ == index || (IsDoublePage() && index == current_ + 1))
                MaybeAutoSr();
        });
    return true;
}

void ReaderView::EndProgressive()
{
    progressive_ = false;
    if (files_.isEmpty())
        return;
    int missing = 0;
    for (const QString &f : files_) {
        if (f.isEmpty())
            ++missing;
    }
    if (missing == 0)
        return;
    // 下载失败的页在 files_ 里还是空串：收敛时摘掉，免得永远停在"加载中…"。
    // 当前页的新下标 = 它前面"活下来"的页数（缺页被摘掉后整体前移）。
    QStringList kept;
    int newCurrent = 0;
    for (int i = 0; i < files_.size(); ++i) {
        if (files_.at(i).isEmpty())
            continue;
        if (i < current_)
            ++newCurrent;
        kept << files_.at(i);
    }
    if (kept.isEmpty()) {
        files_.clear();
        current_ = -1;
        emit PageChanged(0, 0, QString());
        viewport()->update();
        return;
    }
    files_ = kept;
    current_ = qBound(0, newCurrent, files_.size() - 1);
    Relayout();
    ScrollToCurrent();
    UpdateGif();
    viewport()->update();
    emit PageChanged(current_ + 1, files_.size(), files_.at(current_));
    LOGI("progressive done pages=%d missing=%d", files_.size(), missing);
}

int ReaderView::PendingPages() const
{
    int n = 0;
    for (const QString &f : files_) {
        if (f.isEmpty())
            ++n;
    }
    return n;
}

bool ReaderView::CurrentHasImage() const
{
    return current_ >= 0 && !cache_.value(current_).isNull();
}

void ReaderView::RelayoutAnchored()
{
    if (rects_.isEmpty() || current_ < 0 || current_ >= rects_.size()) {
        Relayout();
        return;
    }
    const QRectF before = rects_.at(current_);
    const int vx = horizontalScrollBar()->value();
    const int vy = verticalScrollBar()->value();
    const qreal offX = vx - before.left();
    const qreal offY = vy - before.top();
    Relayout();
    if (current_ < 0 || current_ >= rects_.size())
        return;
    const QRectF after = rects_.at(current_);
    navGuard_ = true;
    horizontalScrollBar()->setValue(qRound(after.left() + offX));
    verticalScrollBar()->setValue(qRound(after.top() + offY));
    navGuard_ = false;
}

void ReaderView::LoadDefaultOrPick()
{
    const QString def = DefaultReaderDir();
    if (!ImageFilesIn(def).isEmpty()) {
        LoadDir(def);
        return;
    }
    OpenPicker();
}

void ReaderView::OpenPicker()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Open manga directory"), QString());
    if (!dir.isEmpty())
        LoadDir(dir);
}

// ============================================================ 布局

void ReaderView::Relayout()
{
    const double vw = qMax(1, viewport()->width());
    const double vh = qMax(1, viewport()->height());
    rects_.clear();
    rects_.resize(files_.size());
    content_ = QSizeF(vw, vh);
    if (files_.isEmpty())
        return;

    const int mode = int(mode_);
    const double z = zoom_;

    auto sizeOf = [this](int i) -> QSizeF {
        const QImage img = cache_.value(i);
        if (!img.isNull())
            return QSizeF(img.width(), img.height());
        return SourceSize(files_.at(i));
    };
    auto contain = [](const QSizeF &src, const QSizeF &box) -> QSizeF {
        if (src.width() <= 0 || src.height() <= 0)
            return QSizeF(0, 0);
        const double s = qMin(box.width() / src.width(), box.height() / src.height());
        return QSizeF(src.width() * s, src.height() * s);
    };

    if (mode == 0) {   // 上下滚动：整章纵向连续，页宽 = 视口宽 * 缩放
        double y = 0;
        double maxW = vw;
        for (int i = 0; i < files_.size(); ++i) {
            const QSizeF src = sizeOf(i);
            const double w = vw * z;
            const double h = qMax(1.0, w * src.height() / qMax(1.0, src.width()));
            rects_[i] = QRectF(0, y, w, h);
            y += h;
            maxW = qMax(maxW, w);
        }
        content_ = QSizeF(maxW, qMax(vh, y));
        for (int i = 0; i < files_.size(); ++i)
            rects_[i].moveLeft((content_.width() - rects_.at(i).width()) / 2.0);
    } else if (mode == 7) {   // 等宽：单页按宽铺满，纵向滚动
        const QSizeF src = sizeOf(qMax(0, current_));
        const double w = vw * z;
        const double h = qMax(1.0, w * src.height() / qMax(1.0, src.width()));
        content_ = QSizeF(qMax(vw, w), qMax(vh, h));
        if (current_ >= 0)
            rects_[current_] = QRectF((content_.width() - w) / 2.0, 0, w, h);
    } else if (mode == 4 || mode == 5 || mode == 8) {   // 横向连续滚动
        const double h = vh * z;
        double x = 0;
        double maxH = vh;
        if (mode == 4) {
            for (int i = 0; i < files_.size(); ++i) {
                const QSizeF src = sizeOf(i);
                const double w = qMax(1.0, h * src.width() / qMax(1.0, src.height()));
                rects_[i] = QRectF(x, 0, w, h);
                x += w;
                maxH = qMax(maxH, h);
            }
        } else {
            // 右左：页 0 在最右，索引越大越靠左；前进 = 横向值减小
            for (int i = files_.size() - 1; i >= 0; --i) {
                const QSizeF src = sizeOf(i);
                const double w = qMax(1.0, h * src.width() / qMax(1.0, src.height()));
                rects_[i] = QRectF(x, 0, w, h);
                x += w;
                maxH = qMax(maxH, h);
            }
        }
        content_ = QSizeF(qMax(vw, x), maxH);
        for (int i = 0; i < files_.size(); ++i) {
            if (rects_.at(i).isEmpty())
                continue;
            rects_[i].moveTop((content_.height() - rects_.at(i).height()) / 2.0);
        }
    } else {   // 单页 / 双页
        if (!IsDoubleMode(mode)) {
            const QSizeF src = sizeOf(qMax(0, current_));
            const QSizeF t = contain(src, QSizeF(vw * z, vh * z));
            content_ = QSizeF(qMax(vw, t.width()), qMax(vh, t.height()));
            if (current_ >= 0)
                rects_[current_] = QRectF((content_.width() - t.width()) / 2.0,
                                           (content_.height() - t.height()) / 2.0, t.width(),
                                           t.height());
        } else {
            const int a = current_;
            const int b = (current_ >= 0 && current_ + 1 < files_.size()) ? current_ + 1 : -1;
            if (a < 0)
                return;
            const QSizeF box(vw / 2.0 * z, vh * z);
            const QSizeF ta = contain(sizeOf(a), box);
            const QSizeF tb = b >= 0 ? contain(sizeOf(b), box) : QSizeF(0, 0);
            const double half = vw / 2.0;
            // Python：graphicsItem1 在 0，graphicsItem2 在 width/2；
            // 左右双页当前页在左，右左双页当前页在右。
            const bool rtl = IsRtlMode(mode);
            const double ax = rtl ? half : 0.0;
            const double bx = rtl ? 0.0 : half;
            content_ = QSizeF(qMax(vw, qMax(ax + ta.width(), bx + tb.width())),
                              qMax(vh, qMax(ta.height(), tb.height())));
            rects_[a] = QRectF(ax, (content_.height() - ta.height()) / 2.0, ta.width(), ta.height());
            if (b >= 0)
                rects_[b] = QRectF(bx, (content_.height() - tb.height()) / 2.0, tb.width(),
                                   tb.height());
        }
    }

    const int cw = qCeil(content_.width());
    const int ch = qCeil(content_.height());
    const int vwi = int(vw);
    const int vhi = int(vh);
    navGuard_ = true;
    horizontalScrollBar()->setRange(0, qMax(0, cw - vwi));
    verticalScrollBar()->setRange(0, qMax(0, ch - vhi));
    horizontalScrollBar()->setPageStep(vwi);
    verticalScrollBar()->setPageStep(vhi);
    horizontalScrollBar()->setSingleStep(qMax(20, vwi / 8));
    verticalScrollBar()->setSingleStep(qMax(20, vhi / 8));
    navGuard_ = false;
}

// ============================================================ 图片缓存

QImage ReaderView::ImageAt(int index)
{
    if (index < 0 || index >= files_.size())
        return QImage();
    if (cache_.contains(index)) {
        cacheOrder_.removeAll(index);
        cacheOrder_.prepend(index);
        return cache_.value(index);
    }
    QElapsedTimer t;
    t.start();
    const QImage img = DecodeFile(files_.at(index));
    if (!img.isNull()) {
        cache_.insert(index, img);
        cacheOrder_.prepend(index);
        cacheBytes_ += qint64(img.sizeInBytes());
        TrimCache();
        if (qEnvironmentVariableIsSet("JM_READER_PROBE")) {
            std::fprintf(stderr, "reader-probe: decode page=%d %dx%d %lldms bytes=%lld\n",
                         index + 1, img.width(), img.height(), qint64(t.elapsed()),
                         qint64(img.sizeInBytes()));
            std::fflush(stderr);
        }
    }
    return img;
}

void ReaderView::TrimCache()
{
    while ((cacheOrder_.size() > kMaxCachePages || cacheBytes_ > kMaxCacheBytes)
           && !cacheOrder_.isEmpty()) {
        const int victim = cacheOrder_.takeLast();
        if (victim == current_ || (IsDoublePage() && victim == current_ + 1))
            continue;   // 当前屏永远保留
        const QImage img = cache_.take(victim);
        cacheBytes_ -= qint64(img.sizeInBytes());
        if (cacheOrder_.isEmpty())
            break;
    }
}

void ReaderView::EnsureCache(int center)
{
    if (files_.isEmpty())
        return;
    const int radius = IsDoublePage() ? 3 : 2;
    for (int d = 0; d <= radius; ++d) {
        const int a = center + d;
        const int b = center - d;
        if (a >= 0 && a < files_.size())
            ImageAt(a);
        if (b >= 0 && b < files_.size())
            ImageAt(b);
    }
    TrimCache();
}

// ============================================================ 页码/滚动

void ReaderView::BeginViewportFade()
{
#if defined(JM_UI_MOTION)
    if (!Motion::Enabled() || !viewport() || viewport()->width() <= 0 || viewport()->height() <= 0)
        return;
    // 上一帧还在淡：直接换内容，别把两层快照叠起来（那会糊成一片）
    if (fadeAlpha_ > 0.01 || !viewport()->isVisible())
        return;
    const QPixmap shot = viewport()->grab();
    if (shot.isNull())
        return;
    fadeShot_ = shot;
    fadeAlpha_ = 1.0;
    Motion::Animate(this, QStringLiteral("viewportFade"), 1.0, 0.0, Motion::Fast,
                    Motion::ExitCurve(),
                    [this](qreal v) {
                        fadeAlpha_ = v;
                        viewport()->update();
                    },
                    [this] {
                        fadeAlpha_ = 0.0;
                        fadeShot_ = QPixmap();
                        viewport()->update();
                    });
#endif
}

void ReaderView::ShowPage(int index, bool keepScroll)
{
    if (files_.isEmpty()) {
        fadeNextSwap_ = false;
        return;
    }
    const int idx = qBound(0, index, files_.size() - 1);
    // 翻页是"整屏内容被换掉"：抓一张旧画面，换完之后淡出（滚动模式有位移，
    // 由 AnimateScroll 负责，不叠加淡出）。
    if (fadeNextSwap_ && !IsScrollMode())
        BeginViewportFade();
    fadeNextSwap_ = false;
    current_ = idx;
    ImageAt(idx);
    Relayout();
    if (!keepScroll)
        ScrollToCurrent();
    UpdateGif();
    viewport()->update();
    emit PageChanged(idx + 1, files_.size(), files_.at(idx));
    QTimer::singleShot(0, this, [this, idx] {
        EnsureCache(idx);
        if (current_ == idx)
            MaybeAutoSr();
    });
}

void ReaderView::ScrollToCurrent()
{
    if (current_ < 0 || rects_.isEmpty())
        return;
    const QRectF r = rects_.value(current_);
    navGuard_ = true;
    if (IsScrollMode()) {
        if (IsHorizontalScrollValue(int(mode_)))
            horizontalScrollBar()->setValue(qRound(r.x()));
        else
            verticalScrollBar()->setValue(qRound(r.y()));
    } else if (int(mode_) == 7) {   // 等宽：页顶，横向居中
        verticalScrollBar()->setValue(0);
        horizontalScrollBar()->setValue(qRound(r.center().x() - viewport()->width() / 2.0));
    } else {
        // 单页/双页：纵向回顶；放大时横向居中在当前页
        verticalScrollBar()->setValue(0);
        horizontalScrollBar()->setValue(IsZoomed()
                                            ? qRound(r.center().x() - viewport()->width() / 2.0)
                                            : 0);
    }
    navGuard_ = false;
}

int ReaderView::PageAtScrollPos() const
{
    if (rects_.isEmpty())
        return -1;
    if (IsHorizontalScrollValue(int(mode_))) {
        const double vw = viewport()->width();
        const double probe = IsRtlMode(int(mode_))
                                 ? horizontalScrollBar()->value() + vw - 4
                                 : horizontalScrollBar()->value() + 4;
        for (int i = 0; i < rects_.size(); ++i) {
            const QRectF r = rects_.at(i);
            if (r.isEmpty())
                continue;
            if (probe >= r.left() && probe <= r.right())
                return i;
        }
        return current_;
    }
    const double probe = verticalScrollBar()->value() + viewport()->height() / 3.0;
    for (int i = 0; i < rects_.size(); ++i) {
        const QRectF r = rects_.at(i);
        if (r.isEmpty())
            continue;
        if (probe >= r.top() && probe <= r.bottom())
            return i;
    }
    return current_;
}

void ReaderView::UpdateCurrentFromScroll()
{
    if (navGuard_ || scrollAnimating_ || pinching_)
        return;
    if (!IsScrollMode() || files_.isEmpty())
        return;
    const int idx = PageAtScrollPos();
    if (idx < 0 || idx == current_)
        return;
    current_ = idx;
    ImageAt(idx);
    UpdateGif();
    emit PageChanged(idx + 1, files_.size(), files_.at(idx));
    QTimer::singleShot(0, this, [this, idx] { EnsureCache(idx); });
}

void ReaderView::StepPage(int dir, bool semanticNext)
{
    if (files_.isEmpty())
        return;
    if (autoScroll_)
        StopAuto();
    const int step = IsDoublePage() ? 2 : 1;
    const int target = current_ + dir * step;
    if (target < 0) {
        emit StatusMessage(QStringLiteral("已经是第一页"));
        if (!semanticNext)
            emit RequestPrevChapter();
        return;
    }
    if (target >= files_.size()) {
        emit StatusMessage(QStringLiteral("已经是最后一页"));
        if (semanticNext)
            emit RequestNextChapter();
        return;
    }
    fadeNextSwap_ = true;
    ShowPage(target);
}

void ReaderView::NextPage()
{
    // Python qtTool.NextPage：右左双页 / 右左滚动（基础版）把"下一页"映射到
    // 索引减小；滚轮正序的两个变体（6/8）保持索引增大。
    const int m = int(mode_);
    const bool inverted = (m == 3 || m == 5);
    StepPage(inverted ? -1 : +1, true);
}

void ReaderView::PrevPage()
{
    const int m = int(mode_);
    const bool inverted = (m == 3 || m == 5);
    StepPage(inverted ? +1 : -1, false);
}

void ReaderView::FirstPage()
{
    if (!files_.isEmpty())
        ShowPage(0);
}

void ReaderView::LastPage()
{
    if (!files_.isEmpty())
        ShowPage(files_.size() - 1);
}

void ReaderView::ScrollToPage(int index, bool animate)
{
    if (files_.isEmpty())
        return;
    const int idx = qBound(0, index, files_.size() - 1);
    current_ = idx;
    ImageAt(idx);
    Relayout();
    if (IsScrollMode()) {
        QScrollBar *bar = IsHorizontalScrollValue(int(mode_)) ? horizontalScrollBar()
                                                             : verticalScrollBar();
        const int target = qRound(IsHorizontalScrollValue(int(mode_)) ? rects_.at(idx).x()
                                                                     : rects_.at(idx).y());
        if (animate)
            AnimateScroll(bar, target, 220);
        else {
            navGuard_ = true;
            bar->setValue(target);
            navGuard_ = false;
        }
    } else {
        ScrollToCurrent();
    }
    UpdateGif();
    viewport()->update();
    emit PageChanged(idx + 1, files_.size(), files_.at(idx));
    QTimer::singleShot(0, this, [this, idx] { EnsureCache(idx); });
}

void ReaderView::ScrollSemantic(int delta)
{
    if (files_.isEmpty() || delta == 0)
        return;
    if (IsHorizontalScrollValue(int(mode_))) {
        AnimateScroll(horizontalScrollBar(),
                      horizontalScrollBar()->value() + ForwardDelta(int(mode_), delta));
    } else {
        AnimateScroll(verticalScrollBar(), verticalScrollBar()->value() + delta);
    }
}

void ReaderView::ScrollByHalf(int dir)
{
    const int step = (IsHorizontalScrollValue(int(mode_)) ? viewport()->width()
                                                          : viewport()->height())
                     / 2;
    ScrollSemantic(dir * qMax(1, step));
}

// ============================================================ 绘制

void ReaderView::Draw(QPainter &painter)
{
    const QRect vp = viewport()->rect();
    painter.fillRect(vp, Qt::black);
    painter.setRenderHints(QPainter::SmoothPixmapTransform | QPainter::Antialiasing, true);

    const int ox = horizontalScrollBar()->value();
    const int oy = verticalScrollBar()->value();

    painter.save();
    if (pinching_ && std::abs(pinchRatio_ - 1.0) > 0.001) {
        // 手势中只做视图变换：围绕双指中点缩放，不改排版、不重解码
        painter.translate(pinchAnchor_);
        painter.scale(pinchRatio_, pinchRatio_);
        painter.translate(-pinchAnchor_);
    }
    painter.translate(-ox, -oy);

    const QRectF viewRect(ox, oy, vp.width(), vp.height());
    if (IsScrollMode() || int(mode_) == 7) {
        for (int i = 0; i < rects_.size(); ++i) {
            if (rects_.at(i).isEmpty())
                continue;
            if (!rects_.at(i).intersects(viewRect))
                continue;
            DrawPage(painter, i);
        }
    } else {
        DrawPage(painter, current_);
        if (IsDoublePage() && current_ + 1 < files_.size())
            DrawPage(painter, current_ + 1);
    }
    painter.restore();

    if (brightness_ < 0.999) {
        painter.fillRect(vp, QColor(0, 0, 0, int((1.0 - brightness_) * 255)));
    }
}

void ReaderView::DrawPage(QPainter &painter, int index)
{
    if (index < 0 || index >= rects_.size())
        return;
    QImage img = cache_.value(index);
    const bool srReady = srWanted_.contains(index) && srCache_.contains(index);
    if (srReady)
        img = srCache_.value(index);
    if (img.isNull()) {
        DrawPlaceholder(painter, index);
        return;
    }
    painter.drawImage(rects_.at(index), img);
    if (srPending_.contains(index) && !srReady) {
        const QRectF r = rects_.at(index);
        painter.setPen(QColor(255, 123, 0));
        QFont f = painter.font();
        f.setPixelSize(qMax(16, int(qMin(r.width(), r.height()) / 24)));
        painter.setFont(f);
        painter.drawText(r.adjusted(12, 10, -12, -12), Qt::AlignLeft | Qt::AlignTop,
                         QStringLiteral("超分处理中…"));
    }
}

void ReaderView::DrawPlaceholder(QPainter &painter, int index)
{
    const QRectF r = rects_.value(index);
    if (r.isEmpty())
        return;
    painter.fillRect(r, QColor(24, 24, 24));
    painter.setPen(QColor(120, 120, 120));
    QFont f = painter.font();
    f.setPixelSize(qMax(20, int(qMin(r.width(), r.height()) / 6)));
    painter.setFont(f);
    const QString text = (index == current_) ? QStringLiteral("加载中…")
                                             : QString::number(index + 1);
    painter.drawText(r, Qt::AlignCenter, text);
}

void ReaderView::UpdateGif()
{
    const bool allow = !IsScrollMode() && !pinching_ && current_ >= 0 && !files_.isEmpty();
    const int pages[2] = {current_, IsDoublePage() ? current_ + 1 : -1};
    for (int slot = 0; slot < 2; ++slot) {
        const int index = pages[slot];
        bool animated = false;
        if (allow && index >= 0 && index < files_.size()) {
            if (!animFlags_.contains(index))
                animFlags_.insert(index, FileIsAnimated(files_.at(index)));
            animated = animFlags_.value(index);
        }
        if (!animated) {
            if (gifMovie_[slot]) {
                gifMovie_[slot]->stop();
                gifMovie_[slot]->deleteLater();
                gifMovie_[slot] = nullptr;
            }
            if (gifLabel_[slot])
                gifLabel_[slot]->hide();
            gifPage_[slot] = -1;
            continue;
        }
        if (!gifLabel_[slot]) {
            gifLabel_[slot] = new QLabel(viewport());
            gifLabel_[slot]->setObjectName(QStringLiteral("ReaderGif"));
            gifLabel_[slot]->setAttribute(Qt::WA_TransparentForMouseEvents);
            gifLabel_[slot]->setStyleSheet(QStringLiteral("background:#000;"));
            gifLabel_[slot]->setAlignment(Qt::AlignCenter);
        }
        if (gifPage_[slot] != index) {
            if (gifMovie_[slot]) {
                gifMovie_[slot]->stop();
                gifMovie_[slot]->deleteLater();
                gifMovie_[slot] = nullptr;
            }
            gifLabel_[slot]->setMovie(nullptr);
            auto *movie = new QMovie(files_.at(index), QByteArray(), gifLabel_[slot]);
            movie->setCacheMode(QMovie::CacheAll);
            gifLabel_[slot]->setMovie(movie);
            if (movie->isValid())
                movie->start();
            gifMovie_[slot] = movie;
            gifPage_[slot] = index;
        }
        const QRectF r = rects_.value(index);
        QRect geo(qRound(r.x()) - horizontalScrollBar()->value(),
                  qRound(r.y()) - verticalScrollBar()->value(), qCeil(r.width()),
                  qCeil(r.height()));
        gifLabel_[slot]->setGeometry(geo);
        if (gifMovie_[slot] && gifMovie_[slot]->isValid())
            gifMovie_[slot]->setScaledSize(geo.size());
        gifLabel_[slot]->setVisible(!r.isEmpty() && geo.intersects(viewport()->rect()));
        if (gifLabel_[slot]->isVisible())
            gifLabel_[slot]->raise();
    }
}

void ReaderView::ClearGif()
{
    for (int i = 0; i < 2; ++i) {
        if (gifMovie_[i]) {
            gifMovie_[i]->stop();
            gifMovie_[i]->deleteLater();
            gifMovie_[i] = nullptr;
        }
        if (gifLabel_[i])
            gifLabel_[i]->hide();
        gifPage_[i] = -1;
    }
}

// ============================================================ 缩放

void ReaderView::SetZoomPercent(int percent, bool persist)
{
    const int pct = qBound(20, percent, 500);
    if (persist)
        SetPrefZoom(pct);
    if (pct == ZoomPercent()) {
        emit ZoomChanged(pct);
        return;
    }
    const double vw = viewport()->width();
    const double vh = viewport()->height();
    const QPointF center(vw / 2.0, vh / 2.0);
    const QPointF scene(horizontalScrollBar()->value() + center.x(),
                        verticalScrollBar()->value() + center.y());
    const double oldZoom = zoom_;
    zoom_ = pct / 100.0;
    Relayout();
    const double ratio = zoom_ / qMax(0.01, oldZoom);
    navGuard_ = true;
    horizontalScrollBar()->setValue(qRound(scene.x() * ratio - center.x()));
    verticalScrollBar()->setValue(qRound(scene.y() * ratio - center.y()));
    navGuard_ = false;
    UpdateGif();
    viewport()->update();
    emit ZoomChanged(pct);
    emit StatusMessage(QStringLiteral("缩放 %1%").arg(pct));
}

void ReaderView::ZoomIn()
{
    SetZoomPercent(ZoomPercent() + 10);
}

void ReaderView::ZoomOut()
{
    SetZoomPercent(ZoomPercent() - 10);
}

void ReaderView::ResetZoom()
{
    const double oldZoom = zoom_;
    const bool shrunk = oldZoom < 1.0 - 1e-3;

    // 复位必须把跟缩放绑定的一整套运行态一起清掉，否则画面会停在"非 100%"的样子：
    //   1) 双指预览：Draw() 在 pinching_ 为真时还会叠加 pinchRatio_ 的视图变换，
    //      只改 zoom_ 的话看起来完全没反应；残留的 pinchBaseZoom_ 又会让下一次
    //      捏合以旧值为基准；
    //   2) 滚动动画：AnimateScroll 的终点是复位前的旧坐标，会在复位之后把滚动位置
    //      再拉走，表现为"比例对了但位置不对"；
    //   3) 手势翻页锁：同一次手势里 turnFired_ 已置位，复位后不该再判定翻页。
    ResetPinch();
    pinchBaseZoom_ = 1.0;
    pinchAnchor_ = QPointF();
    pinchSceneAnchor_ = QPointF();
    if (scrollAni_) {
        scrollAni_->stop();
        scrollAni_->deleteLater();
        scrollAni_ = nullptr;
    }
    scrollAnimating_ = false;
    touchDragging_ = false;
    turnAccumX_ = 0.0;
    turnAccumY_ = 0.0;
    turnAxis_ = 0;
    turnFired_ = false;

    const double vw = viewport()->width();
    const double vh = viewport()->height();
    const QPointF center(vw / 2.0, vh / 2.0);
    const QPointF scene(horizontalScrollBar()->value() + center.x(),
                        verticalScrollBar()->value() + center.y());

    // 刻意不复用 SetZoomPercent()：那里对"目标值与当前值相同"会提前 return，
    // 而 100% 恰恰是最容易被这个分支吞掉的档位（缩到 100% 附近、或只需要复位
    // 位移时）。这里统一强制落位，保证任意状态下点一次都真的回到默认显示。
    zoom_ = 1.0;
    SetPrefZoom(100);
    Relayout();

    if (shrunk) {
        // 缩小时内容被 qMax(vw/vh) 撑满、滚动条被钳在 0，用旧坐标做比例映射会
        // 落到页面之外；直接回该模式的默认位（翻页模式=左上/居中，滚动模式=
        // 当前页起点），这才是"默认显示尺寸"该有的位移。
        ScrollToCurrent();
    } else {
        // 放大状态保持原有观感：以视口中心为锚缩回去。
        const double ratio = zoom_ / qMax(0.01, oldZoom);
        navGuard_ = true;
        horizontalScrollBar()->setValue(qRound(scene.x() * ratio - center.x()));
        verticalScrollBar()->setValue(qRound(scene.y() * ratio - center.y()));
        navGuard_ = false;
    }

    UpdateGif();
    viewport()->update();
    LOGI("read-zoom reset %d%% -> 100%%", qRound(oldZoom * 100.0));
    emit ZoomChanged(100);
    emit StatusMessage(QStringLiteral("缩放 100%"));
}

// ============================================================ 模式

void ReaderView::SetReadMode(ReadMode m, bool persist)
{
    m = static_cast<ReadMode>(qBound(0, int(m), 8));
    const int keep = current_ >= 0 ? current_ : 0;
    if (persist)
        SetPrefMode(int(m));
    const bool same = (mode_ == m);
    // 换模式是一次整屏重排（页宽/列数都变），不做过渡会像"画面被换了一张图"。
    // 缩放复位也包含在这 120ms 里，观感上是一次连续变化。
    if (!same)
        BeginViewportFade();
    mode_ = m;
    if (!same) {
        // Python ChangeReadMode2：切模式先把缩放复位到 100%
        zoom_ = 1.0;
        SetPrefZoom(100);
    }
    current_ = files_.isEmpty() ? -1 : qBound(0, keep, files_.size() - 1);
    Relayout();
    if (current_ >= 0)
        ScrollToCurrent();
    UpdateGif();
    viewport()->update();
    if (autoScroll_)
        StartAuto();
    LOGI("read-mode %d (%s)", int(m), qPrintable(ModeName(int(m))));
    emit StatusMessage(QStringLiteral("阅读模式：%1").arg(ModeName(int(m))));
    emit ZoomChanged(ZoomPercent());
    emit ModeChanged(int(m));
}

void ReaderView::ToggleTwoPage()
{
    if (IsDoublePage())
        SetReadMode(ReadMode::LeftRight);
    else
        SetReadMode(IsRtl() ? ReadMode::RightLeftDouble : ReadMode::LeftRightDouble);
}

void ReaderView::ToggleVertical()
{
    SetReadMode(IsVertical() ? ReadMode::LeftRight : ReadMode::UpDown);
}

void ReaderView::ToggleDirection()
{
    switch (mode_) {
    case ReadMode::LeftRightDouble:
        SetReadMode(ReadMode::RightLeftDouble);
        break;
    case ReadMode::RightLeftDouble:
    case ReadMode::RightLeftDouble2:
        SetReadMode(ReadMode::LeftRightDouble);
        break;
    case ReadMode::LeftRightScroll:
        SetReadMode(ReadMode::RightLeftScroll);
        break;
    case ReadMode::RightLeftScroll:
    case ReadMode::RightLeftScroll2:
        SetReadMode(ReadMode::LeftRightScroll);
        break;
    default:
        emit StatusMessage(QStringLiteral("当前模式没有左右变体，可在“阅读模式”里切换"));
        break;
    }
}

void ReaderView::SetDirection(bool rtl)
{
    if (rtl == IsRtl()) {
        emit StatusMessage(rtl ? QStringLiteral("当前已是右左方向")
                               : QStringLiteral("当前已是左右方向"));
        return;
    }
    switch (mode_) {
    case ReadMode::LeftRightDouble:
    case ReadMode::RightLeftDouble:
    case ReadMode::RightLeftDouble2:
        SetReadMode(rtl ? ReadMode::RightLeftDouble : ReadMode::LeftRightDouble);
        break;
    case ReadMode::LeftRightScroll:
    case ReadMode::RightLeftScroll:
    case ReadMode::RightLeftScroll2:
        SetReadMode(rtl ? ReadMode::RightLeftScroll : ReadMode::LeftRightScroll);
        break;
    default:
        emit StatusMessage(QStringLiteral("当前模式没有左右变体"));
        break;
    }
}

void ReaderView::ToggleDoublePageAlign()
{
    if (!IsDoublePage()) {
        emit StatusMessage(QStringLiteral("仅双页模式支持调整配对"));
        return;
    }
    if (files_.isEmpty())
        return;
    ShowPage(qMin(current_ + 1, files_.size() - 1));
    emit StatusMessage(QStringLiteral("双页配对已偏移一页"));
}

void ReaderView::SetBrightness(double v)
{
    brightness_ = std::min(std::max(v, 0.0), 1.0);
    viewport()->update();
    LOGI("brightness %.2f", brightness_);
    emit StatusMessage(QStringLiteral("brightness %1%").arg(int(brightness_ * 100 + 0.5)));
}

void ReaderView::CycleBrightness()
{
    static const double kLevels[] = {1.0, 0.75, 0.5, 0.25};
    int idx = 0;
    for (int i = 0; i < 4; ++i) {
        if (std::abs(brightness_ - kLevels[i]) < 0.01) {
            idx = i;
            break;
        }
    }
    SetBrightness(kLevels[(idx + 1) % 4]);
}

// ============================================================ 自动滚动/翻页

void ReaderView::SetAutoScroll(bool on)
{
    if (autoScroll_ == on) {
        if (on)
            StartAuto();
        return;
    }
    autoScroll_ = on;
    if (on) {
        StartAuto();
        emit StatusMessage(QStringLiteral("自动滚动/翻页：开"));
    } else {
        StopAuto();
        emit StatusMessage(QStringLiteral("自动滚动/翻页：关"));
    }
}

void ReaderView::StartAuto()
{
    if (!autoScroll_ || files_.isEmpty())
        return;
    const int interval = IsScrollMode() ? PrefScrollMs() : PrefTurnMs();
    autoTimer_->start(qMax(16, interval));
}

void ReaderView::StopAuto()
{
    autoScroll_ = false;
    if (autoTimer_)
        autoTimer_->stop();
}

void ReaderView::AutoTick()
{
    if (!autoScroll_ || files_.isEmpty()) {
        StopAuto();
        return;
    }
    if (IsScrollMode()) {
        QScrollBar *bar = IsHorizontalScrollValue(int(mode_)) ? horizontalScrollBar()
                                                             : verticalScrollBar();
        const int step = PrefScrollStep();
        const int delta = IsHorizontalScrollValue(int(mode_))
                              ? ForwardDelta(int(mode_), step)
                              : step;
        const bool atEnd = delta > 0 ? bar->value() >= bar->maximum()
                                     : bar->value() <= bar->minimum();
        if (atEnd) {
            StopAuto();
            emit StatusMessage(QStringLiteral("已到末尾，自动滚动停止"));
            return;
        }
        bar->setValue(bar->value() + delta);
        UpdateCurrentFromScroll();
    } else {
        const int before = current_;
        StepPage(+1, true);   // Python TimeOut 调 _NextPage：始终按索引前进
        if (current_ == before) {
            StopAuto();
            emit StatusMessage(QStringLiteral("已到末尾，自动翻页停止"));
        }
    }
}

// ============================================================ 滚动动画

void ReaderView::AnimateScroll(QScrollBar *bar, int target, int ms)
{
    if (!bar)
        return;
    target = qBound(bar->minimum(), target, bar->maximum());
    if (scrollAni_) {
        scrollAni_->stop();
        scrollAni_->deleteLater();
        scrollAni_ = nullptr;
    }
    if (ms <= 0 || qAbs(target - bar->value()) <= 1) {
        navGuard_ = true;
        bar->setValue(target);
        navGuard_ = false;
        UpdateCurrentFromScroll();
        return;
    }
    scrollAnimating_ = true;
    auto *ani = new QPropertyAnimation(bar, "value", this);
    ani->setDuration(ms);
    ani->setStartValue(bar->value());
    ani->setEndValue(target);
    ani->setEasingCurve(QEasingCurve::OutCubic);
    connect(ani, &QPropertyAnimation::finished, this, [this, ani] {
        if (scrollAni_ == ani) {
            scrollAnimating_ = false;
            scrollAni_ = nullptr;
            UpdateCurrentFromScroll();
            UpdateGif();
        }
        ani->deleteLater();
    });
    scrollAni_ = ani;
    ani->start();
}

// ============================================================ 事件

void ReaderView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    Relayout();
    ScrollToCurrent();
    UpdateGif();
    viewport()->update();
}

bool ReaderView::viewportEvent(QEvent *event)
{
    switch (event->type()) {
    case QEvent::Paint: {
        QPainter p(viewport());
        Draw(p);
        // 换图过渡：把"上一张"按 alpha 递减贴回来。放在同一支画笔里做，
        // 不额外开窗口/图层，也不影响滚动条的命中。
        if (fadeAlpha_ > 0.001 && !fadeShot_.isNull()) {
            p.setOpacity(fadeAlpha_);
            p.drawPixmap(0, 0, fadeShot_);
        }
        return true;
    }
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel:
        HandleTouch(static_cast<QTouchEvent *>(event));
        return true;
    case QEvent::MouseButtonPress:
        HandleMousePress(static_cast<QMouseEvent *>(event));
        return true;
    case QEvent::MouseMove:
        HandleMouseMove(static_cast<QMouseEvent *>(event));
        return true;
    case QEvent::MouseButtonRelease:
        HandleMouseRelease(static_cast<QMouseEvent *>(event));
        return true;
    case QEvent::ContextMenu: {
        auto *ce = static_cast<QContextMenuEvent *>(event);
        emit RequestContextMenu(ce->globalPos());
        return true;
    }
    default:
        break;
    }
    return QAbstractScrollArea::viewportEvent(event);
}

// ---------------------------------------------------------------- 鼠标

void ReaderView::HandleMousePress(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    if (event->button() != Qt::LeftButton)
        return;
    if (autoScroll_)
        StopAuto();
    mouseDown_ = true;
    mouseDragging_ = false;
    mousePress_ = event->position();
    mouseLast_ = mousePress_;
    turnAccumX_ = turnAccumY_ = 0;
    turnAxis_ = 0;
    turnFired_ = false;
}

void ReaderView::HandleMouseMove(QMouseEvent *event)
{
    if (!mouseDown_ || !(event->buttons() & Qt::LeftButton))
        return;
    const QPointF pos = event->position();
    if (!mouseDragging_ && (pos - mousePress_).manhattanLength() >= kTouchSlop)
        mouseDragging_ = true;
    if (!mouseDragging_)
        return;
    const QPointF d = pos - mouseLast_;
    mouseLast_ = pos;
    ScrollDrag(d.x(), d.y());
}

void ReaderView::HandleMouseRelease(QMouseEvent *event)
{
    if (!mouseDown_ || event->button() != Qt::LeftButton)
        return;
    mouseDown_ = false;
    const bool wasDragging = mouseDragging_;
    mouseDragging_ = false;
    if (!wasDragging && (event->position() - mousePress_).manhattanLength() <= kTouchSlop)
        EmitTap(event->position());
    viewport()->update();
}

// ---------------------------------------------------------------- 触控

void ReaderView::HandleTouch(QTouchEvent *event)
{
    if (event->type() == QEvent::TouchCancel) {
        touchPts_.clear();
        touchPrimary_ = -1;
        touchDragging_ = false;
        swallowTouch_ = false;
        ResetPinch();
        turnAxis_ = 0;
        turnFired_ = false;
        return;
    }

    QList<QPair<int, QPointF>> released;
    TouchMerge(event, &released);

    if (!pinching_ && touchPts_.size() <= 1 && !swallowTouch_)
        TouchSingleMove();

    for (const auto &r : released) {
        if (r.first != touchPrimary_)
            continue;
        if (!touchDragging_ && !swallowTouch_ && !pinching_ && touchPts_.isEmpty()) {
            const QPointF local = viewport()->mapFromGlobal(r.second.toPoint());
            EmitTap(local);
        }
        touchPrimary_ = -1;
        touchDragging_ = false;
        turnAccumX_ = turnAccumY_ = 0;
        turnAxis_ = 0;
        turnFired_ = false;
    }

    TouchSync();
    viewport()->update();
}

void ReaderView::TouchMerge(QTouchEvent *event, QList<QPair<int, QPointF>> *released)
{
    for (const QEventPoint &pt : event->points()) {
        const int id = pt.id();
        if (pt.state() == QEventPoint::State::Released) {
            if (touchPts_.contains(id)) {
                released->append({id, pt.globalPosition()});
                touchPts_.remove(id);
            }
        } else {
            touchPts_.insert(id, pt.globalPosition());
        }
    }
    // TouchEnd 兜底：个别平台最后一个点不带 Released 状态
    if (event->type() == QEvent::TouchEnd) {
        for (const QEventPoint &pt : event->points()) {
            const int id = pt.id();
            if (touchPts_.contains(id)) {
                released->append({id, touchPts_.value(id)});
                touchPts_.remove(id);
            }
        }
    }
}

void ReaderView::TouchSingleMove()
{
    if (touchPrimary_ < 0 && !touchPts_.isEmpty()) {
        auto it = touchPts_.constBegin();
        touchPrimary_ = it.key();
        touchPress_ = touchPts_.value(touchPrimary_);
        touchLast_ = touchPress_;
        touchDragging_ = false;
        turnAccumX_ = turnAccumY_ = 0;
        turnAxis_ = 0;
        turnFired_ = false;
    }
    if (touchPrimary_ < 0 || !touchPts_.contains(touchPrimary_))
        return;
    const QPointF g = touchPts_.value(touchPrimary_);
    if (!touchDragging_ && (g - touchPress_).manhattanLength() >= kTouchSlop)
        touchDragging_ = true;
    if (!touchDragging_)
        return;
    const QPointF d = g - touchLast_;
    touchLast_ = g;
    ScrollDrag(d.x(), d.y());
}

void ReaderView::TouchSync()
{
    const int n = touchPts_.size();
    if (n >= 2) {
        if (!pinching_)
            StartPinch();
        else
            ApplyPinch();
        return;
    }
    if (pinching_) {
        FinishPinch();
        swallowTouch_ = n > 0;   // 缩放后剩下的手指不许变成点按
        touchPrimary_ = -1;
        touchDragging_ = false;
        return;
    }
    if (n == 0) {
        touchPrimary_ = -1;
        touchDragging_ = false;
        swallowTouch_ = false;
        turnAccumX_ = turnAccumY_ = 0;
        turnAxis_ = 0;
        turnFired_ = false;
    }
}

void ReaderView::ScrollDrag(qreal dx, qreal dy)
{
    if (files_.isEmpty())
        return;
    const double vw = viewport()->width();
    const double vh = viewport()->height();
    const bool canPanX = content_.width() > vw + 0.5;
    const bool canPanY = content_.height() > vh + 0.5;

    bool panned = false;
    if (canPanX) {
        const int v = horizontalScrollBar()->value() - qRound(dx);
        horizontalScrollBar()->setValue(v);
        panned = true;
    }
    if (canPanY) {
        const int v = verticalScrollBar()->value() - qRound(dy);
        verticalScrollBar()->setValue(v);
        panned = true;
    }
    if (panned) {
        UpdateCurrentFromScroll();
        return;
    }

    // 没放大/无可平移空间：翻页模式下一次手势只翻一页
    if (!IsHorizontalPage(int(mode_)))
        return;
    if (turnFired_)
        return;
    turnAccumX_ += -dx;
    turnAccumY_ += -dy;
    if (turnAxis_ == 0) {
        const double ax = std::abs(turnAccumX_);
        const double ay = std::abs(turnAccumY_);
        if (ax >= kTurnSlop && ax >= ay)
            turnAxis_ = 1;
        else if (ay >= kTurnSlop)
            turnAxis_ = 2;
        else
            return;
    }
    QScrollBar *bar = (turnAxis_ == 1) ? horizontalScrollBar() : verticalScrollBar();
    const double accum = (turnAxis_ == 1) ? turnAccumX_ : turnAccumY_;
    if (accum > 0) {
        if (qAbs(bar->value() - bar->maximum()) > 5)
            return;
        NextPage();
    } else {
        if (bar->value() > 5)
            return;
        PrevPage();
    }
    turnFired_ = true;
}

void ReaderView::StartPinch()
{
    pinching_ = true;
    touchDragging_ = false;
    touchPrimary_ = -1;
    pinchStartDist_ = 0.0;
    const auto pts = touchPts_.values();
    if (pts.size() >= 2) {
        const QPointF d = pts.at(1) - pts.at(0);
        pinchStartDist_ = std::hypot(d.x(), d.y());
        const QPointF mid = (pts.at(0) + pts.at(1)) / 2.0;
        pinchAnchor_ = viewport()->mapFromGlobal(mid.toPoint());
    }
    pinchBaseZoom_ = zoom_;
    pinchRatio_ = 1.0;
    pinchSceneAnchor_ = QPointF(horizontalScrollBar()->value() + pinchAnchor_.x(),
                                verticalScrollBar()->value() + pinchAnchor_.y());
    ClearGif();
    LOGI("pinch start base=%d%% dist=%.0f mode=%d", ZoomPercent(), pinchStartDist_, int(mode_));
}

void ReaderView::ApplyPinch()
{
    const auto pts = touchPts_.values();
    if (pts.size() < 2)
        return;
    const QPointF d = pts.at(1) - pts.at(0);
    const double dist = std::hypot(d.x(), d.y());
    if (pinchStartDist_ < 1.0) {
        pinchStartDist_ = dist;
        return;
    }
    if (dist < 1.0)
        return;
    const double target = qBound(0.2, pinchBaseZoom_ * (dist / pinchStartDist_), 5.0);
    pinchRatio_ = target / pinchBaseZoom_;
    viewport()->update();
}

void ReaderView::FinishPinch()
{
    if (!pinching_)
        return;
    pinching_ = false;
    const double newZoom = qBound(0.2, pinchBaseZoom_ * pinchRatio_, 5.0);
    pinchRatio_ = 1.0;
    const int pct = qRound(newZoom * 100.0);
    const int oldPct = ZoomPercent();
    if (std::abs(pct - oldPct) < 5) {
        UpdateGif();
        viewport()->update();
        return;
    }
    const double scale = newZoom / qMax(0.01, pinchBaseZoom_);
    zoom_ = pct / 100.0;
    SetPrefZoom(pct);
    Relayout();
    navGuard_ = true;
    horizontalScrollBar()->setValue(
        qRound(pinchSceneAnchor_.x() * scale - pinchAnchor_.x()));
    verticalScrollBar()->setValue(qRound(pinchSceneAnchor_.y() * scale - pinchAnchor_.y()));
    navGuard_ = false;
    UpdateGif();
    viewport()->update();
    emit ZoomChanged(pct);
    emit StatusMessage(QStringLiteral("缩放 %1%").arg(pct));
    LOGI("pinch apply %d%% -> %d%%", oldPct, pct);
}

void ReaderView::ResetPinch()
{
    pinching_ = false;
    pinchRatio_ = 1.0;
    pinchStartDist_ = 0.0;
    UpdateGif();
}

void ReaderView::EmitTap(const QPointF &local)
{
    if (autoScroll_)
        StopAuto();
    const int w = qMax(1, viewport()->width());
    const int h = qMax(1, viewport()->height());
    const int x = int(local.x());
    const int y = int(local.y());
    QString zone;
    if (y <= h / 2)
        zone = x <= w / 3 ? QStringLiteral("left-top")
                          : (x <= w * 2 / 3 ? QStringLiteral("mid-top")
                                            : QStringLiteral("right-top"));
    else
        zone = x <= w / 3 ? QStringLiteral("left-bottom")
                          : (x <= w * 2 / 3 ? QStringLiteral("mid-bottom")
                                            : QStringLiteral("right-bottom"));
    emit TapZone(zone, QPoint(x, y));
}

// ---------------------------------------------------------------- 滚轮/键盘

void ReaderView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        SetZoomPercent(ZoomPercent() + (event->angleDelta().y() > 0 ? 10 : -10));
        event->accept();
        return;
    }
    if (autoScroll_)
        StopAuto();
    const int dy = event->angleDelta().y();
    if (dy == 0)
        return;
    const int manual = PrefManualStep();

    if (!IsScrollMode() && int(mode_) != 7) {
        if (dy < 0 && verticalScrollBar()->value() >= verticalScrollBar()->maximum() - 5) {
            NextPage();
            event->accept();
            return;
        }
        if (dy > 0 && verticalScrollBar()->value() <= 5) {
            PrevPage();
            event->accept();
            return;
        }
        AnimateScroll(verticalScrollBar(),
                      verticalScrollBar()->value() + (dy < 0 ? manual : -manual));
        event->accept();
        return;
    }

    if (IsHorizontalScrollValue(int(mode_))) {
        int v = dy < 0 ? manual : -manual;
        if (int(mode_) == 8)
            v = -v;   // 滚轮正序：与左右滚动同手感
        AnimateScroll(horizontalScrollBar(), horizontalScrollBar()->value() + v);
    } else {
        AnimateScroll(verticalScrollBar(),
                      verticalScrollBar()->value() + (dy < 0 ? manual : -manual));
    }
    event->accept();
}

void ReaderView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Right:
        NextPage();
        return;
    case Qt::Key_Left:
        PrevPage();
        return;
    case Qt::Key_Down:
        ScrollSemantic(viewport()->height() / 2);
        return;
    case Qt::Key_Up:
        ScrollSemantic(-viewport()->height() / 2);
        return;
    case Qt::Key_PageDown:
        NextPage();
        return;
    case Qt::Key_PageUp:
        PrevPage();
        return;
    case Qt::Key_Space:
        ScrollSemantic(viewport()->height());
        return;
    case Qt::Key_Home:
        FirstPage();
        return;
    case Qt::Key_End:
        LastPage();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        ZoomIn();
        return;
    case Qt::Key_Minus:
        ZoomOut();
        return;
    case Qt::Key_0:
        ResetZoom();
        return;
    case Qt::Key_F1:
        CopyCurrentToClipboard();
        return;
    case Qt::Key_F2:
        ToggleSrCurrent();
        return;
    case Qt::Key_F3:
        emit RequestSaveAs();
        return;
    case Qt::Key_F4:
        SaveCurrentToLastDir();
        return;
    case Qt::Key_F5:
        SetAutoScroll(!IsAutoScroll());
        return;
    case Qt::Key_F10:
        ToggleDoublePageAlign();
        return;
    case Qt::Key_F11:
        emit RequestFullScreen();
        return;
    case Qt::Key_F12:
        emit RequestToggleChrome();
        return;
    case Qt::Key_Escape:
        emit RequestBack();
        return;
    case Qt::Key_B:
        CycleBrightness();
        return;
    case Qt::Key_D:
        ToggleTwoPage();
        return;
    case Qt::Key_R:
        ToggleDirection();
        return;
    default:
        break;
    }
    if (event->modifiers() & Qt::ShiftModifier) {
        if (event->key() == Qt::Key_Left) {
            emit RequestPrevChapter();
            return;
        }
        if (event->key() == Qt::Key_Right) {
            emit RequestNextChapter();
            return;
        }
    }
    QAbstractScrollArea::keyPressEvent(event);
}

// ============================================================ 当前页信息

QSize ReaderView::CurrentImageSize() const
{
    if (current_ < 0 || current_ >= files_.size())
        return QSize();
    const QImage img = cache_.value(current_);
    if (!img.isNull())
        return img.size();
    const QSize s = SourceSize(files_.at(current_)).toSize();
    return s;
}

qint64 ReaderView::CurrentFileSize() const
{
    if (current_ < 0 || current_ >= files_.size())
        return 0;
    return QFileInfo(files_.at(current_)).size();
}

QString ReaderView::CurrentFilePath() const
{
    if (current_ < 0 || current_ >= files_.size())
        return QString();
    return files_.at(current_);
}

bool ReaderView::CurrentIsAnimated() const
{
    if (current_ < 0 || current_ >= files_.size())
        return false;
    return FileIsAnimated(files_.at(current_));
}

QByteArray ReaderView::CurrentRawBytes() const
{
    if (current_ < 0 || current_ >= files_.size())
        return {};
    QFile f(files_.at(current_));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.readAll();
}

bool ReaderView::CopyCurrentToClipboard()
{
    const QImage img = ImageAt(current_);
    if (img.isNull()) {
        emit StatusMessage(QStringLiteral("当前页还没有图片"));
        return false;
    }
    QGuiApplication::clipboard()->setImage(img);
    emit StatusMessage(QStringLiteral("已复制当前图片"));
    return true;
}

bool ReaderView::SaveCurrentTo(const QString &path)
{
    const QByteArray data = CurrentRawBytes();
    if (data.isEmpty()) {
        emit StatusMessage(QStringLiteral("当前页还没有图片"));
        return false;
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        emit StatusMessage(QStringLiteral("保存失败：%1").arg(f.errorString()));
        return false;
    }
    f.write(data);
    lastSaveDir_ = QFileInfo(path).absolutePath();
    emit StatusMessage(QStringLiteral("已保存到 %1").arg(path));
    return true;
}

bool ReaderView::SaveCurrentToLastDir()
{
    if (lastSaveDir_.isEmpty()) {
        emit RequestSaveAs();
        return false;
    }
    const QString name = QFileInfo(CurrentFilePath()).fileName();
    if (name.isEmpty())
        return false;
    return SaveCurrentTo(QDir(lastSaveDir_).filePath(name));
}

// ============================================================ 超分

void ReaderView::ToggleSrCurrent()
{
    if (current_ < 0 || files_.isEmpty())
        return;
    if (!SrEngine::Available()) {
        emit StatusMessage(QStringLiteral("超分不可用：%1").arg(SrEngine::Status()));
        return;
    }
    if (srWanted_.contains(current_)) {
        srWanted_.remove(current_);
        srCache_.remove(current_);
        BeginViewportFade();
        viewport()->update();
        emit SrStateChanged(current_, false);
        emit StatusMessage(QStringLiteral("本张超分已关闭"));
        return;
    }
    srWanted_.insert(current_);
    if (srCache_.contains(current_)) {
        BeginViewportFade();
        viewport()->update();
        emit SrStateChanged(current_, true);
        emit StatusMessage(QStringLiteral("已应用超分（缓存）"));
        return;
    }
    StartSr(current_);
}

void ReaderView::SetAutoSr(bool on)
{
    autoSr_ = on;
    SetPrefAutoSr(on);
    emit StatusMessage(on ? QStringLiteral("自动超分：开") : QStringLiteral("自动超分：关"));
    if (on)
        MaybeAutoSr();
}

void ReaderView::MaybeAutoSr()
{
    if (!autoSr_ || current_ < 0 || files_.isEmpty())
        return;
    // 渐进加载期间当前页可能还是占位（空串）：等文件到货后再发起，
    // 否则会对空路径起一次注定失败的超分任务（srPending_ 还会把它锁死）。
    if (files_.value(current_).isEmpty())
        return;
    if (srCache_.contains(current_) || srPending_.contains(current_))
        return;
    if (!SrEngine::Available())
        return;
    srWanted_.insert(current_);
    StartSr(current_);
}

void ReaderView::StartSr(int index)
{
    if (index < 0 || index >= files_.size())
        return;
    if (srPending_.contains(index))
        return;
    if (srWatcher_->isRunning()) {
        srQueued_ = index;   // 同一时间只跑一块，避免 CPU 线程互相抢
        return;
    }
    const QImage img = ImageAt(index);
    if (img.isNull()) {
        emit StatusMessage(QStringLiteral("超分失败：当前页还没有图片"));
        return;
    }
    srPending_.insert(index);
    srJobPage_ = index;
    emit SrStateChanged(index, false);
    emit StatusMessage(QStringLiteral("超分处理中…（%1x%2）").arg(img.width()).arg(img.height()));
    srWatcher_->setFuture(QtConcurrent::run([img] { return SrEngine::Upscale(img); }));
}

void ReaderView::OnSrFinished()
{
    const SrEngine::Result res = srWatcher_->result();
    const int page = srJobPage_;
    srJobPage_ = -1;
    if (page >= 0)
        srPending_.remove(page);
    if (res.image.isNull()) {
        if (page >= 0)
            srWanted_.remove(page);
        emit StatusMessage(QStringLiteral("超分失败：%1").arg(res.error));
        emit SrStateChanged(page, false);
    } else {
        srCache_.insert(page, res.image);
        TrimSrCache();
        qInfo("sr: applied page=%d %dx%d %.0fms", page + 1, res.image.width(),
              res.image.height(), res.ms);
        emit StatusMessage(QStringLiteral("超分完成：%1 → %2x%3，%4ms")
                               .arg(QFileInfo(files_.value(page)).fileName())
                               .arg(res.image.width())
                               .arg(res.image.height())
                               .arg(res.ms, 0, 'f', 0));
        emit SrStateChanged(page, true);
    }
    // 超分是"同一页换成更清晰的一张"：淡一下能让清晰度的变化被看见，而不是闪一下
    BeginViewportFade();
    viewport()->update();
    if (srQueued_ >= 0) {
        const int q = srQueued_;
        srQueued_ = -1;
        StartSr(q);
    }
}

void ReaderView::TrimSrCache()
{
    while (srCache_.size() > 4) {
        const QList<int> keys = srCache_.keys();
        bool removed = false;
        for (int k : keys) {
            if (k != current_ && k != current_ + 1) {
                srCache_.remove(k);
                removed = true;
                break;
            }
        }
        if (!removed) {
            srCache_.remove(keys.first());
        }
    }
}

bool ReaderView::CurrentHasSr() const
{
    return current_ >= 0 && srWanted_.contains(current_) && srCache_.contains(current_);
}

bool ReaderView::CurrentSrPending() const
{
    return current_ >= 0 && srPending_.contains(current_);
}

QString ReaderView::SrStatusText() const
{
    if (!SrEngine::Available())
        return QStringLiteral("不可用：%1").arg(SrEngine::Status());
    if (CurrentSrPending())
        return QStringLiteral("处理中…");
    if (CurrentHasSr())
        return QStringLiteral("已应用 2x");
    return QStringLiteral("未开启");
}

// ============================================================ 回归入口

void ReaderView::SimulateTap(qreal fx, qreal fy)
{
    EmitTap(QPointF(viewport()->width() * qBound<qreal>(0.02, fx, 0.98),
                    viewport()->height() * qBound<qreal>(0.02, fy, 0.98)));
}

void ReaderView::SimulateSwipe(int dx, int dy, int steps)
{
    if (steps <= 0)
        steps = 1;
    touchPts_.clear();
    touchPts_.insert(1, viewport()->mapToGlobal(QPoint(viewport()->width() / 2,
                                                       viewport()->height() / 2)));
    touchPrimary_ = 1;
    touchPress_ = touchPts_.value(1);
    touchLast_ = touchPress_;
    touchDragging_ = true;
    turnAccumX_ = turnAccumY_ = 0;
    turnAxis_ = 0;
    turnFired_ = false;
    for (int i = 0; i < steps; ++i)
        ScrollDrag(qreal(dx) / steps, qreal(dy) / steps);
    touchPts_.clear();
    touchPrimary_ = -1;
    touchDragging_ = false;
    turnAxis_ = 0;
    turnFired_ = false;
}

void ReaderView::SimulatePinch(qreal ratio)
{
    const QPointF center(viewport()->width() / 2.0, viewport()->height() / 2.0);
    const QPointF gcenter = viewport()->mapToGlobal(center.toPoint());
    touchPts_.clear();
    touchPts_.insert(1, gcenter - QPointF(50, 0));
    touchPts_.insert(2, gcenter + QPointF(50, 0));
    StartPinch();
    touchPts_.insert(1, gcenter - QPointF(50 * ratio, 0));
    touchPts_.insert(2, gcenter + QPointF(50 * ratio, 0));
    ApplyPinch();
    touchPts_.clear();
    TouchSync();
}

void ReaderView::RunTouchEventSelfTest()
{
    if (files_.isEmpty() || viewport()->width() < 50) {
        std::fprintf(stderr, "reader-touch: SKIP no pages / tiny viewport\n");
        std::fflush(stderr);
        return;
    }
    auto makePoint = [this](int id, const QPointF &local, QEventPoint::State state) {
        // 公开构造：scenePosition / globalPosition。触摸处理只读 globalPosition，
        // 本地坐标由 viewport()->mapFromGlobal 反算。
        return QEventPoint(id, state, local, QPointF(viewport()->mapToGlobal(local.toPoint())));
    };
    auto send = [this](QEvent::Type type, const QList<QEventPoint> &pts) {
        QTouchEvent ev(type, QPointingDevice::primaryPointingDevice(), Qt::NoModifier, pts);
        QCoreApplication::sendEvent(viewport(), &ev);
    };

    int pass = 0;
    int fail = 0;
    auto check = [&](bool ok, const QString &what) {
        ok ? ++pass : ++fail;
        std::fprintf(stderr, "reader-touch: %s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
        std::fflush(stderr);
    };

    // 1) 真实 Touch 事件：单指横滑 -> 下一页
    SetReadMode(1, false);
    SetZoomPercent(100, false);
    ShowPage(0);
    const int before = current_;
    QPointF start(viewport()->width() * 0.8, viewport()->height() * 0.5);
    send(QEvent::TouchBegin, {makePoint(0, start, QEventPoint::State::Pressed)});
    send(QEvent::TouchUpdate,
         {makePoint(0, start - QPointF(120, 0), QEventPoint::State::Updated)});
    send(QEvent::TouchEnd,
         {makePoint(0, start - QPointF(120, 0), QEventPoint::State::Released)});
    check(current_ == before + 1, QStringLiteral("QTouchEvent 横滑翻页"));

    // 2) 真实 Touch 事件：双指放大 2 倍
    const QPointF c(viewport()->width() / 2.0, viewport()->height() / 2.0);
    send(QEvent::TouchBegin, {makePoint(0, c - QPointF(50, 0), QEventPoint::State::Pressed)});
    send(QEvent::TouchUpdate,
         {makePoint(0, c - QPointF(50, 0), QEventPoint::State::Updated),
          makePoint(1, c + QPointF(50, 0), QEventPoint::State::Pressed)});
    send(QEvent::TouchUpdate,
         {makePoint(0, c - QPointF(100, 0), QEventPoint::State::Updated),
          makePoint(1, c + QPointF(100, 0), QEventPoint::State::Updated)});
    send(QEvent::TouchEnd,
         {makePoint(0, c - QPointF(100, 0), QEventPoint::State::Released),
          makePoint(1, c + QPointF(100, 0), QEventPoint::State::Released)});
    check(ZoomPercent() >= 195 && ZoomPercent() <= 205,
          QStringLiteral("QTouchEvent 双指放大到 200%"));

    // 3) 真实 Touch 事件：点按右下三分区 -> 下一页
    SetZoomPercent(100, false);
    ShowPage(0);
    const int tapBefore = current_;
    const QPointF tapPoint(viewport()->width() * 0.82, viewport()->height() * 0.82);
    send(QEvent::TouchBegin, {makePoint(0, tapPoint, QEventPoint::State::Pressed)});
    send(QEvent::TouchEnd, {makePoint(0, tapPoint, QEventPoint::State::Released)});
    check(current_ == tapBefore + 1, QStringLiteral("QTouchEvent 点按右下翻页"));

    // 4) 滚轮：页尾再往下 -> 下一页
    ShowPage(0);
    const int wheelBefore = current_;
    QWheelEvent wheel(QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0),
                      QPointF(viewport()->mapToGlobal(QPoint(viewport()->width() / 2,
                                                             viewport()->height() / 2))),
                      QPoint(0, 0), QPoint(0, -120), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(viewport(), &wheel);
    check(current_ == wheelBefore + 1, QStringLiteral("滚轮翻页"));

    // 5) 键盘：右方向键 -> 下一页
    ShowPage(0);
    const int keyBefore = current_;
    QKeyEvent key(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &key);
    check(current_ == keyBefore + 1, QStringLiteral("右方向键翻页"));

    std::fprintf(stderr, "reader-touch done pass=%d fail=%d zoom=%d%%\n", pass, fail,
                 ZoomPercent());
    std::fflush(stderr);
}

// 渐进加载自检：不依赖网络，用本地目录扮演"第 1 页先到、其余页陆续到、末尾缺页"。
// 断言口径与用户看到的一致：当前页那张一到就出图，总页数不变，收尾只摘缺页。
void ReaderView::RunProgressiveSelfTest(const QString &dir)
{
    const QStringList all = ImageFilesIn(dir);
    if (all.size() < 2) {
        std::fprintf(stderr, "progressive-selftest: SKIP need >=2 images in %s\n",
                     qPrintable(dir));
        std::fflush(stderr);
        return;
    }
    int pass = 0;
    int fail = 0;
    auto check = [&](bool ok, const char *what) {
        if (ok)
            ++pass;
        else
            ++fail;
        std::fprintf(stderr, "progressive-selftest: %s %s\n", ok ? "PASS" : "FAIL", what);
        std::fflush(stderr);
    };

    const int ready = qMin(3, all.size());
    const int total = ready + 2;   // 末尾两页当作"还没下完/最终失败"
    BeginProgressive(total);
    check(files_.size() == total, "按总页数铺占位页");
    check(current_ == 0, "当前页停在第一页");
    check(cache_.isEmpty(), "占位期不预解码");

    QElapsedTimer t;
    t.start();
    SetProgressivePage(0, all.at(0));
    const qint64 firstMs = t.elapsed();
    check(CurrentHasImage(), "第 1 页到货即出图");
    check(PageCount() == total, "单页到货不改变总页数");

    for (int i = 1; i < ready; ++i)
        SetProgressivePage(i, all.at(i));
    check(PendingPages() == total - ready, "未到货页计入待加载");

    // 翻到第 3 页：已到货的页立刻可见
    ShowPage(ready - 1);
    check(current_ == ready - 1 && CurrentHasImage(), "已到货页翻到即显示");

    EndProgressive();
    check(files_.size() == ready, "收尾摘掉缺页占位");
    check(current_ == ready - 1, "收尾不改变当前页");

    std::fprintf(stderr,
                 "progressive-selftest: pass=%d fail=%d first_ms=%lld pages=%d\n", pass, fail,
                 (long long)firstMs, files_.size());
    std::fflush(stderr);
}

void ReaderView::RunParitySelfTest()
{
    int pass = 0;
    int fail = 0;
    auto check = [&](bool ok, const QString &what) {
        if (ok) {
            ++pass;
        } else {
            ++fail;
        }
        std::fprintf(stderr, "reader-parity: %s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
        std::fflush(stderr);
    };

    if (files_.isEmpty()) {
        std::fprintf(stderr, "reader-parity: SKIP no pages\n");
        std::fflush(stderr);
        emit SelfTestDone();
        return;
    }

    const int savedMode = int(mode_);
    const int savedZoom = ZoomPercent();

    // 1) 9 种模式都能建场，且内容尺寸/页矩形有效
    for (int m = 0; m <= 8; ++m) {
        SetReadMode(m, false);
        SetZoomPercent(100, false);
        current_ = qBound(0, current_, files_.size() - 1);
        Relayout();
        ScrollToCurrent();
        const QRectF r = rects_.value(current_);
        const bool ok = r.width() > 0 && r.height() > 0 && content_.width() >= viewport()->width()
                        && content_.height() >= viewport()->height();
        std::fprintf(stderr,
                     "reader-parity: mode=%d (%s) page=%d/%d rect=%.0fx%.0f content=%.0fx%.0f "
                     "scroll=%dx%d\n",
                     m, qPrintable(ModeName(m)), CurrentPage(), PageCount(), r.width(), r.height(),
                     content_.width(), content_.height(), horizontalScrollBar()->maximum(),
                     verticalScrollBar()->maximum());
        std::fflush(stderr);
        check(ok, QStringLiteral("mode %1 布局有效").arg(m));
    }

    // 2) 左右翻页：向左滑 80px 只翻一页，继续滑不再翻
    SetReadMode(1, false);
    SetZoomPercent(100, false);
    ShowPage(0);
    const int beforeSwipe = current_;
    SimulateSwipe(-80, 0);
    check(current_ == beforeSwipe + 1, QStringLiteral("左滑翻下一页"));
    const int afterFirst = current_;
    SimulateSwipe(-80, 0, 1);   // 新手势：从当前页再翻一页
    check(current_ == afterFirst + 1, QStringLiteral("再次左滑再翻一页"));
    SimulateSwipe(80, 0);
    check(current_ == afterFirst, QStringLiteral("右滑回上一页"));
    std::fprintf(stderr, "reader-parity: swipe page=%d\n", CurrentPage());
    std::fflush(stderr);

    // 3) 双指缩放：200% 后 50% -> 100%
    SetReadMode(1, false);
    SetZoomPercent(100, false);
    SimulatePinch(2.0);
    check(ZoomPercent() == 200, QStringLiteral("双指放大到 200%"));
    SimulatePinch(0.5);
    check(ZoomPercent() == 100, QStringLiteral("双指缩小回 100%"));
    std::fprintf(stderr, "reader-parity: pinch zoom=%d%%\n", ZoomPercent());
    std::fflush(stderr);

    // 4) 上下滚动：半屏滚动会移动滚动条并更新当前页
    SetReadMode(0, false);
    SetZoomPercent(100, false);
    ShowPage(0);
    const int v0 = verticalScrollBar()->value();
    const int p0 = current_;
    const int target = qMin(verticalScrollBar()->maximum(),
                            v0 + qMax(1, viewport()->height() / 2));
    check(target > v0 && verticalScrollBar()->maximum() > 0,
          QStringLiteral("上下滚动可移动"));
    verticalScrollBar()->setValue(target);
    UpdateCurrentFromScroll();
    check(CurrentPage() >= p0, QStringLiteral("滚动后当前页前进"));
    std::fprintf(stderr, "reader-parity: vertical v=%d->%d page=%d->%d max=%d\n", v0,
                 verticalScrollBar()->value(), p0 + 1, CurrentPage(),
                 verticalScrollBar()->maximum());
    std::fflush(stderr);

    SetReadMode(savedMode, false);
    SetZoomPercent(savedZoom, false);
    ShowPage(0);
    std::fprintf(stderr, "reader-parity done pass=%d fail=%d\n", pass, fail);
    std::fflush(stderr);
    emit SelfTestDone();
}
