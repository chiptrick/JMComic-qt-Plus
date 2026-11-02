#include "reader.h"

#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QGestureEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QPainter>
#include <QPinchGesture>
#include <QPixmap>
#include <QResizeEvent>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "C1Reader", __VA_ARGS__)

namespace {

const char *kImageExts[] = {"jpg", "jpeg", "png", "webp", "bmp", "gif"};

QString DefaultReaderDir()
{
    // AppLocalDataLocation 在 Android 上是 /data/user/0/<pkg>/files —— 不需要存储权限，
    // adb 冒烟用 run-as 投测试图；桌面端是 ~/.local/share/<app>/reader。
    const QString appDir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!appDir.isEmpty())
        return appDir + QStringLiteral("/reader");
    return QStringLiteral("/sdcard/c1_shell");
}

// 亮度压暗层：盖在 scroll 上层、不接鼠标，只按 alpha 画黑
class BrightnessOverlay : public QWidget
{
public:
    explicit BrightnessOverlay(QWidget *parent)
        : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

    void SetLevel(double v)
    {
        level_ = std::min(std::max(v, 0.0), 1.0);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (level_ >= 0.999)
            return;
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0, int((1.0 - level_) * 255)));
    }

private:
    double level_ = 1.0;
};

}  // namespace

ReaderView::ReaderView(QWidget *parent)
    : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);

    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(false);
    scroll_->setAlignment(Qt::AlignCenter);
    imageLabel_ = new QLabel(scroll_);
    imageLabel_->setAlignment(Qt::AlignCenter);
    scroll_->setWidget(imageLabel_);
    lay->addWidget(scroll_, 1);

    overlay_ = new BrightnessOverlay(this);
    overlay_->setGeometry(rect());

    // 手势：捏合缩放挂在 viewport 上；滑动手势在事件过滤器里用鼠标/触摸按下-抬起判定
    scroll_->viewport()->installEventFilter(this);
    scroll_->viewport()->grabGesture(Qt::PinchGesture);
    setFocusPolicy(Qt::StrongFocus);
}

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
    base_ = QImage();
    second_ = QImage();
    if (files_.isEmpty()) {
        imageLabel_->setText(QStringLiteral("No images in\n%1").arg(dir));
        imageLabel_->setPixmap(QPixmap());
        emit StatusMessage(QStringLiteral("reader: no images in %1").arg(dir));
        emit PageChanged(0, 0, QString());
        return 0;
    }
    LOGI("loaded %d pages from %s", files_.size(), qPrintable(dir));
    emit StatusMessage(QStringLiteral("reader: %1 pages from %2").arg(files_.size()).arg(dir));
    ShowPage(current_);
    return files_.size();
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
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Open manga directory"), QString());
    if (!dir.isEmpty())
        LoadDir(dir);
}

void ReaderView::ShowPage(int index)
{
    if (index < 0 || index >= files_.size())
        return;
    current_ = index;
    base_ = QImage(files_.at(index));
    second_ = (twoPage_ && index + 1 < files_.size())
                  ? QImage(files_.at(index + 1))
                  : QImage();
    if (base_.isNull()) {
        imageLabel_->setText(QStringLiteral("Decode failed:\n%1").arg(files_.at(index)));
        imageLabel_->setPixmap(QPixmap());
        emit PageChanged(index + 1, files_.size(), files_.at(index));
        return;
    }
    RenderImage();
    emit PageChanged(index + 1, files_.size(), files_.at(index));
}

void ReaderView::RenderImage()
{
    if (base_.isNull() || scroll_->viewport()->width() <= 0)
        return;
    // 组合显示图：单页=base；双页=base+second（RTL 时视觉顺序对调：右页先看）
    QImage show = base_;
    if (twoPage_ && !second_.isNull()) {
        const int gap = 8;
        QImage combined(base_.width() + gap + second_.width(),
                        std::max(base_.height(), second_.height()),
                        QImage::Format_RGB32);
        combined.fill(Qt::black);
        QPainter p(&combined);
        const bool rtl = (direction_ == Direction::RTL);
        const QImage &left = rtl ? second_ : base_;
        const QImage &right = rtl ? base_ : second_;
        p.drawImage(0, 0, left);
        p.drawImage(left.width() + gap, 0, right);
        p.end();
        show = combined;
    }
    // 1.0 = 适宽；不做上采样超过 4 倍，避免极端小图糊成巨图
    const double fit = double(scroll_->viewport()->width()) / double(show.width());
    const double scale = std::min(std::max(fit * zoom_, 0.05), fit * 4.0);
    const QSize target(int(show.width() * scale), int(show.height() * scale));
    imageLabel_->setPixmap(QPixmap::fromImage(
        show.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    imageLabel_->resize(target);
}

void ReaderView::NextPage()
{
    const int step = SpreadStep();
    if (current_ + step < files_.size()) {
        LOGI("page -> %d/%d%s", current_ + step + 1, files_.size(),
             twoPage_ ? " (spread)" : "");
        ShowPage(current_ + step);
    }
}

void ReaderView::PrevPage()
{
    const int step = SpreadStep();
    if (current_ - step >= 0) {
        LOGI("page -> %d/%d%s", current_ - step + 1, files_.size(),
             twoPage_ ? " (spread)" : "");
        ShowPage(current_ - step);
    }
}

void ReaderView::ZoomIn()
{
    zoom_ = std::min(zoom_ * 1.25, 8.0);
    RenderImage();
    emit StatusMessage(QStringLiteral("zoom x%1").arg(zoom_, 0, 'f', 2));
}

void ReaderView::ZoomOut()
{
    zoom_ = std::max(zoom_ / 1.25, 0.2);
    RenderImage();
    emit StatusMessage(QStringLiteral("zoom x%1").arg(zoom_, 0, 'f', 2));
}

void ReaderView::ResetZoom()
{
    zoom_ = 1.0;
    RenderImage();
    emit StatusMessage(QStringLiteral("zoom fit-width"));
}

void ReaderView::ToggleTwoPage()
{
    twoPage_ = !twoPage_;
    LOGI("two-page %s", twoPage_ ? "on" : "off");
    if (current_ >= 0)
        ShowPage(current_);
    emit StatusMessage(twoPage_ ? QStringLiteral("two-page on")
                                : QStringLiteral("two-page off"));
}

void ReaderView::ToggleDirection()
{
    direction_ = (direction_ == Direction::LTR) ? Direction::RTL : Direction::LTR;
    LOGI("direction %s", direction_ == Direction::RTL ? "RTL" : "LTR");
    if (current_ >= 0)
        ShowPage(current_);
    emit StatusMessage(direction_ == Direction::RTL ? QStringLiteral("direction RTL")
                                                    : QStringLiteral("direction LTR"));
}

void ReaderView::SetBrightness(double v)
{
    brightness_ = std::min(std::max(v, 0.0), 1.0);
    ApplyBrightness();
    LOGI("brightness %.2f", brightness_);
    emit StatusMessage(QStringLiteral("brightness %1%")
                           .arg(int(brightness_ * 100 + 0.5)));
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

void ReaderView::ApplyBrightness()
{
    static_cast<BrightnessOverlay *>(overlay_)->SetLevel(brightness_);
    overlay_->raise();
}

void ReaderView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Right:
        // RTL：右键是"上一页"（日漫从右往左读）
        if (direction_ == Direction::RTL)
            PrevPage();
        else
            NextPage();
        return;
    case Qt::Key_Left:
        if (direction_ == Direction::RTL)
            NextPage();
        else
            PrevPage();
        return;
    case Qt::Key_PageDown:
        NextPage();
        return;
    case Qt::Key_PageUp:
        PrevPage();
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
    case Qt::Key_D:
        ToggleTwoPage();
        return;
    case Qt::Key_R:
        ToggleDirection();
        return;
    case Qt::Key_B:
        CycleBrightness();
        return;
    default:
        QWidget::keyPressEvent(event);
    }
}

void ReaderView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    overlay_->setGeometry(rect());
    RenderImage();
}

bool ReaderView::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == scroll_->viewport()) {
        switch (event->type()) {
        case QEvent::Gesture: {
            auto *ge = static_cast<QGestureEvent *>(event);
            if (auto *pinch = static_cast<QPinchGesture *>(
                    ge->gesture(Qt::PinchGesture))) {
                static double lastScale = 1.0;
                if (pinch->state() == Qt::GestureStarted) {
                    lastScale = 1.0;
                } else if (pinch->state() == Qt::GestureUpdated) {
                    const double delta = pinch->scaleFactor() / lastScale;
                    if (delta > 1.05) {
                        ZoomIn();
                        lastScale = pinch->scaleFactor();
                    } else if (delta < 0.95) {
                        ZoomOut();
                        lastScale = pinch->scaleFactor();
                    }
                }
                ge->accept(Qt::PinchGesture);
                return true;
            }
            break;
        }
        case QEvent::MouseButtonPress:
            if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
                pressPos_ = static_cast<QMouseEvent *>(event)->pos();
                pressValid_ = true;
            }
            break;
        case QEvent::MouseButtonRelease: {
            if (!pressValid_)
                break;
            pressValid_ = false;
            const QPoint rel = static_cast<QMouseEvent *>(event)->pos() - pressPos_;
            if (std::abs(rel.x()) > 80 && std::abs(rel.x()) > std::abs(rel.y())) {
                const bool toPrev = (direction_ == Direction::RTL)
                                        ? (rel.x() > 0)
                                        : (rel.x() < 0);
                LOGI("swipe dx=%d -> %s", rel.x(), toPrev ? "prev" : "next");
                if (toPrev)
                    PrevPage();
                else
                    NextPage();
                return true;
            }
            break;
        }
        default:
            break;
        }
    }
    return QWidget::eventFilter(obj, event);
}

void ReaderView::RunSelfTest()
{
    LOGI("selftest begin");
    LoadDefaultOrPick();
    QTimer::singleShot(300, this, [this] { NextPage(); });
    QTimer::singleShot(600, this, [this] { NextPage(); });
    QTimer::singleShot(900, this, [this] { PrevPage(); });
    QTimer::singleShot(1200, this, [this] { ZoomIn(); });
    QTimer::singleShot(1500, this, [this] { ResetZoom(); });
    // W3：双页 -> RTL -> 亮度
    QTimer::singleShot(1800, this, [this] { ToggleTwoPage(); });
    QTimer::singleShot(2100, this, [this] { ToggleDirection(); });
    QTimer::singleShot(2400, this, [this] { CycleBrightness(); });
    QTimer::singleShot(2700, this, [this] {
        LOGI("selftest done pages=%d twoPage=%d rtl=%d brightness=%.2f",
             files_.size(), int(twoPage_), int(direction_ == Direction::RTL), brightness_);
        emit SelfTestDone();
    });
}
