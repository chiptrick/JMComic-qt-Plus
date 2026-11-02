#include "reader.h"

#include <QDir>
#include <QFileDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLoggingCategory>
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
    // 1.0 = 适宽；不做上采样超过 4 倍，避免极端小图糊成巨图
    const double fit = double(scroll_->viewport()->width()) / double(base_.width());
    const double scale = std::min(std::max(fit * zoom_, 0.05), fit * 4.0);
    const QSize target(int(base_.width() * scale), int(base_.height() * scale));
    imageLabel_->setPixmap(QPixmap::fromImage(
        base_.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    imageLabel_->resize(target);
    imageLabel_->setPixmap(imageLabel_->pixmap());
}

void ReaderView::NextPage()
{
    if (current_ + 1 < files_.size()) {
        LOGI("page -> %d/%d", current_ + 2, files_.size());
        ShowPage(current_ + 1);
    }
}

void ReaderView::PrevPage()
{
    if (current_ > 0) {
        LOGI("page -> %d/%d", current_, files_.size());
        ShowPage(current_ - 1);
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

void ReaderView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Right:
    case Qt::Key_PageDown:
        NextPage();
        return;
    case Qt::Key_Left:
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
    default:
        QWidget::keyPressEvent(event);
    }
}

void ReaderView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    RenderImage();
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
    QTimer::singleShot(1800, this, [this] {
        LOGI("selftest done pages=%d", files_.size());
        emit SelfTestDone();
    });
}
