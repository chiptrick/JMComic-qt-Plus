#include "page_transition.h"

#include <QCoreApplication>
#include <QEvent>
#include <QMetaObject>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWidget>
#include <QQuickWindow>
#include <QTimer>
#include <QWidget>

#include <cstdio>

#include "theme.h"

// ---------------------------------------------------------------- 纹理来源
PageImageProvider::PageImageProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage PageImageProvider::requestImage(const QString &id, QSize *size, const QSize &)
{
    QMutexLocker lock(&mutex_);
    const QImage img = images_.value(id);
    if (size)
        *size = img.size();
    return img;
}

void PageImageProvider::Put(const QString &id, const QImage &image)
{
    QMutexLocker lock(&mutex_);
    images_.insert(id, image);
    order_.insert(id, ++seq_);

    // 只留最近 6 张：一次换页 2 张，留点余量给"上一轮还没释放"的情况。
    // 不清理的话每切一次页就多两张 2560x1680 的图，几分钟就吃掉几个 GB。
    while (order_.size() > 6) {
        QString oldest;
        qint64 oldestSeq = std::numeric_limits<qint64>::max();
        for (auto it = order_.cbegin(); it != order_.cend(); ++it) {
            if (it.value() < oldestSeq) {
                oldestSeq = it.value();
                oldest = it.key();
            }
        }
        order_.remove(oldest);
        images_.remove(oldest);
    }
}

// ---------------------------------------------------------------- 覆盖层
PageTransition::PageTransition(QWidget *host) : QObject(host), host_(host)
{
    if (host_)
        host_->installEventFilter(this);
}

PageTransition::~PageTransition() = default;

bool PageTransition::EnsureScene()
{
    if (ready_)
        return true;
    if (softFails_ >= 2) {
        // 连续两次连帧都画不出来：本进程不再尝试，避免每次切页都卡 120ms
        return false;
    }

    if (!view_) {
        QWidget *win = host_ ? host_->window() : nullptr;
        if (!win)
            return false;

        view_ = new QQuickWidget(win);
        view_->setObjectName(QStringLiteral("PageSwipeOverlay"));
        view_->setResizeMode(QQuickWidget::SizeRootObjectToView);
        view_->setClearColor(UiTheme::I().Tokens().bg);
        view_->setFocusPolicy(Qt::NoFocus);
        view_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        view_->setAttribute(Qt::WA_ShowWithoutActivating, true);
        view_->installEventFilter(this);

        provider_ = new PageImageProvider;   // engine 接管所有权
        view_->engine()->addImageProvider(QStringLiteral("pagecache"), provider_);

        connect(view_, &QQuickWidget::sceneGraphError, this,
                [this](QQuickWindow::SceneGraphError, const QString &msg) {
                    ready_ = false;
                    softFails_ = 2;
                    std::fprintf(stderr, "page-transition: scene graph error: %s\n",
                                 qPrintable(msg));
                });

        view_->setSource(QUrl(QStringLiteral("qrc:/qml/PageSwipe.qml")));
        if (view_->status() != QQuickWidget::Ready) {
            std::fprintf(stderr, "page-transition: QML load failed (status=%d)\n",
                         int(view_->status()));
            softFails_ = 2;
            return false;
        }
        root_ = view_->rootObject();
        if (!root_) {
            softFails_ = 2;
            return false;
        }
        connect(root_, SIGNAL(finished()), this, SLOT(OnSceneFinished()));

        // 有的后端不经过 QQuickWidget 的 paintEvent 就直接出帧，
        // 两条路都接上，谁先来算谁（firstFrame_ 自己去重）。
        if (QQuickWindow *qw = view_->quickWindow())
            connect(qw, &QQuickWindow::frameSwapped, this, &PageTransition::OnFrameSwapped);

        watchdog_ = new QTimer(this);
        watchdog_->setSingleShot(true);
        connect(watchdog_, &QTimer::timeout, this, &PageTransition::OnWatchdog);

        view_->hide();
    }

    ready_ = true;
    return true;
}

void PageTransition::SyncGeometry()
{
    if (!view_ || !host_)
        return;
    QWidget *win = host_->window();
    if (!win)
        return;
    const QPoint topLeft = host_->mapTo(win, QPoint(0, 0));
    view_->setGeometry(QRect(topLeft, host_->size()));
}

bool PageTransition::Begin(const QPixmap &prev, const QPixmap &next, Mode mode)
{
    if (!animEnabled_ || !host_ || prev.isNull() || next.isNull())
        return false;
    if (!EnsureScene())
        return false;

    ++generation_;
    prevId_ = QStringLiteral("prev_%1").arg(generation_);
    nextId_ = QStringLiteral("next_%1").arg(generation_);
    provider_->Put(prevId_, prev.toImage());
    provider_->Put(nextId_, next.toImage());

    root_->setProperty("prevSource", QUrl(QStringLiteral("image://pagecache/") + prevId_));
    root_->setProperty("nextSource", QUrl(QStringLiteral("image://pagecache/") + nextId_));
    // 0 = push（旧左移、新从右入）1 = pop（旧右移、新从左入）2 = fade（无位移）
    root_->setProperty("mode", mode == Mode::Push ? 0 : (mode == Mode::Pop ? 1 : 2));
    root_->setProperty("durationMs", durationMs_);
    root_->setProperty("dpr", host_->devicePixelRatioF());

    running_ = true;
    frozen_ = false;
    firstFrame_ = false;

    SyncGeometry();
    view_->show();
    view_->raise();
    QMetaObject::invokeMethod(root_, "start");
    watchdog_->start(120);
    return true;
}

void PageTransition::End()
{
    running_ = false;
    frozen_ = false;
    if (watchdog_)
        watchdog_->stop();
    if (root_)
        QMetaObject::invokeMethod(root_, "stop");
    if (view_)
        view_->hide();
}

void PageTransition::FreezeAt(qreal progress)
{
    if (!root_)
        return;
    frozen_ = true;
    running_ = false;
    if (watchdog_)
        watchdog_->stop();
    QMetaObject::invokeMethod(root_, "freeze", Q_ARG(QVariant, QVariant(progress)));
}

void PageTransition::OnFrameSwapped()
{
    if (!running_ || firstFrame_)
        return;
    firstFrame_ = true;
    if (watchdog_)
        watchdog_->stop();
    emit FirstFrame();
}

void PageTransition::OnSceneFinished()
{
    if (!running_ || frozen_)
        return;
    running_ = false;
    emit Finished();
}

void PageTransition::OnWatchdog()
{
    if (!running_ || firstFrame_ || frozen_)
        return;

    // 没有拿到 frameSwapped：强制渲染一次，能出图就当作首帧（只是报得晚一点）
    if (view_) {
        const QImage frame = view_->grabFramebuffer();
        if (!frame.isNull()) {
            firstFrame_ = true;
            emit FirstFrame();
            return;
        }
    }

    ++softFails_;
    running_ = false;
    std::fprintf(stderr, "page-transition: no frame in 120ms (softFail=%d)\n", softFails_);
    emit Failed(QStringLiteral("no frame in 120ms"));
}

bool PageTransition::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == view_ && ev->type() == QEvent::Paint && running_ && !firstFrame_) {
        // paintEvent 还没执行完，此时报"有内容"会让调用方提前藏旧页（闪一帧背景）。
        // 排到本轮事件之后再报。
        QTimer::singleShot(0, this, [this] { OnFrameSwapped(); });
    }
    if (obj == host_ && (ev->type() == QEvent::Resize || ev->type() == QEvent::Move))
        SyncGeometry();
    return QObject::eventFilter(obj, ev);
}
