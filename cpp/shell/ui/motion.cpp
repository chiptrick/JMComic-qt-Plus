#include "motion.h"

#include <QAbstractAnimation>
#include <QAbstractButton>
#include <QApplication>
#include <QEvent>
#include <QGraphicsOpacityEffect>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QPropertyAnimation>
#include <QRadialGradient>
#include <QTimer>
#include <QVariantAnimation>
#include <QWidget>

#include <cmath>

#include "theme.h"

namespace {

bool g_enabled = true;

class RippleLayer : public QWidget
{
public:
    RippleLayer(QWidget *parent, const QPoint &center, const QColor &color, const QPixmap &base)
        : QWidget(parent), center_(center), color_(color), base_(base)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setObjectName(QStringLiteral("MotionRipple"));
        setGeometry(parent->rect());
    }

    void Start(int ms)
    {
        auto *a = new QVariantAnimation(this);
        a->setStartValue(0.0);
        a->setEndValue(1.0);
        a->setDuration(ms);
        a->setEasingCurve(QEasingCurve::OutCubic);
        connect(a, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            t_ = v.toReal();
            update();
        });
        connect(a, &QVariantAnimation::finished, this, &QObject::deleteLater);
        a->start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (t_ >= 1.0 || width() <= 0 || height() <= 0)
            return;
        QPainter p(this);
        // 先铺"按下那一刻的按钮像素"，再叠涟漪。
        // 为什么不用 WA_TranslucentBackground：QScrollArea/QAbstractScrollArea 里的
        // 子控件走半透明合成时，在部分平台上会被滚动容器自己的重绘盖掉
        // （实测只剩 1px 亮边）。这里让涟漪层完全不透明，就不依赖任何合成特性。
        if (!base_.isNull())
            p.drawPixmap(0, 0, base_);

        // 半径覆盖到最远角：小按钮上是"整块亮一下"，大按钮上是明显的涟漪
        const qreal far = std::hypot(qMax<qreal>(center_.x(), width() - center_.x()),
                                     qMax<qreal>(center_.y(), height() - center_.y()));
        const qreal r = far * (0.10 + 0.90 * t_);
        QColor c = color_;
        // alpha 用 (1-t²) 收尾：前半程保持亮度，后半程快速褪掉（比线性更像"涟漪"）
        c.setAlphaF(0.26 * (1.0 - t_ * t_));
        QRadialGradient g(center_, r);
        g.setColorAt(0.0, c);
        QColor edge = c;
        edge.setAlpha(0);
        g.setColorAt(1.0, edge);

        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawEllipse(QPointF(center_), r, r);
    }

private:
    QPoint center_;
    QColor color_;
    QPixmap base_;
    qreal t_ = 0.0;
};

// 旧画面的替身：内容完全不透明（就是旧画面本身），靠 QGraphicsOpacityEffect
// 整体降透明度来"淡出"。
//
// 为什么不用 WA_TranslucentBackground 自己画 alpha：QScrollArea 这类容器里的
// 半透明子控件在部分合成路径上会被容器的重绘盖掉（实测只剩 1px 亮边）。
// 而图形效果走的是"离屏渲染 + 按 alpha 合成进父级"，与容器无关，最稳。
class FadeLayer : public QWidget
{
public:
    FadeLayer(QWidget *parent, const QPixmap &shot) : QWidget(parent), shot_(shot)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
    }

    void Start(int ms)
    {
        if (shot_.isNull()) {
            deleteLater();
            return;
        }
        auto *eff = new QGraphicsOpacityEffect(this);
        eff->setOpacity(1.0);
        setGraphicsEffect(eff);
        auto *a = new QPropertyAnimation(eff, "opacity", this);
        a->setDuration(ms);
        a->setStartValue(1.0);
        a->setEndValue(0.0);
        a->setEasingCurve(QEasingCurve::OutCubic);
        connect(a, &QPropertyAnimation::finished, this, [this] { deleteLater(); });
        a->start(QAbstractAnimation::DeleteWhenStopped);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        // 旧画面可能比现在的宿主小（内容变长）：铺一层底色再贴，避免露出下层
        p.fillRect(rect(), UiTheme::I().Tokens().bg);
        if (!shot_.isNull())
            p.drawPixmap(0, 0, shot_);
    }

private:
    QPixmap shot_;
};

// 全局按下反馈。只认"按钮类"，并且排除那些按下会被解释成别的手势的控件
// （开关/滑块/下拉框按下后要判断方向，出涟漪反而误导）。
class TapFilter : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (ev->type() != QEvent::MouseButtonPress)
            return false;
        auto *w = qobject_cast<QWidget *>(obj);
        if (!w)
            return false;

        QAbstractButton *btn = qobject_cast<QAbstractButton *>(w);
        for (QWidget *p = w; !btn && p; p = p->parentWidget())
            btn = qobject_cast<QAbstractButton *>(p);
        if (!btn || !btn->isEnabled() || !btn->isVisible())
            return false;
        if (btn->property("noRipple").toBool())
            return false;

        auto *me = static_cast<QMouseEvent *>(ev);
        QPoint local = btn->mapFromGlobal(me->globalPosition().toPoint());
        if (!btn->rect().contains(local))
            local = btn->rect().center();
        Motion::Ripple(btn, local);
        return false;
    }
};

TapFilter *g_tapFilter = nullptr;

// 抓到的旧画面是否"整片同色"。
// 用途：QQuickWidget 的快照在个别合成后端上可能抓成空白（FBO 未就绪），
// 这种图淡出等于"先盖一层底色再消失"，比不淡更难看；此时宁可直接换内容。
bool LooksFlat(const QPixmap &pm)
{
    if (pm.isNull() || pm.width() < 8 || pm.height() < 8)
        return true;
    const QImage img = pm.toImage();
    int lo = 255;
    int hi = 0;
    for (int y = 0; y < 32; ++y) {
        for (int x = 0; x < 32; ++x) {
            const QRgb c = img.pixel(img.width() * x / 32, img.height() * y / 32);
            const int lum = qGray(c);
            lo = qMin(lo, lum);
            hi = qMax(hi, lum);
        }
    }
    return hi - lo < 4;
}

}  // namespace

namespace Motion {

bool Enabled()
{
    return g_enabled;
}

void SetEnabled(bool on)
{
    g_enabled = on;
}

QEasingCurve EnterCurve()
{
    return QEasingCurve(QEasingCurve::OutCubic);
}

QEasingCurve ExitCurve()
{
    return QEasingCurve(QEasingCurve::InCubic);
}

QEasingCurve MoveCurve()
{
    return QEasingCurve(QEasingCurve::InOutCubic);
}

void Animate(QObject *owner, const QString &tag, qreal from, qreal to, int ms,
             const QEasingCurve &curve, const std::function<void(qreal)> &step,
             const std::function<void()> &done)
{
    if (!owner || !step)
        return;
    if (!g_enabled || ms <= Instant) {
        step(to);
        if (done)
            done();
        return;
    }

    const QString name = QStringLiteral("motion:") + tag;
    const auto siblings = owner->children();
    for (QObject *child : siblings) {
        auto *old = qobject_cast<QVariantAnimation *>(child);
        if (old && old->objectName() == name) {
            old->stop();
            old->deleteLater();
        }
    }

    auto *a = new QVariantAnimation(owner);
    a->setObjectName(name);
    a->setStartValue(from);
    a->setEndValue(to);
    a->setDuration(ms);
    a->setEasingCurve(curve);
    QObject::connect(a, &QVariantAnimation::valueChanged, owner, [step](const QVariant &v) {
        step(v.toReal());
    });
    QObject::connect(a, &QVariantAnimation::finished, owner, [a, done] {
        a->deleteLater();
        if (done)
            done();
    });
    a->start();
}

void Stop(QObject *owner, const QString &tag)
{
    if (!owner)
        return;
    const QString name = QStringLiteral("motion:") + tag;
    const auto kids = owner->children();
    for (QObject *child : kids) {
        auto *a = qobject_cast<QVariantAnimation *>(child);
        if (a && a->objectName() == name) {
            a->stop();
            a->deleteLater();
        }
    }
}

void Ripple(QWidget *w, const QPoint &pos)
{
    if (!w || !g_enabled || !w->isVisible() || w->width() <= 0 || w->height() <= 0)
        return;
    // 同一控件只保留一层涟漪：连点时旧的直接收掉，避免叠出深色圆斑
    const auto stale = w->findChildren<QWidget *>(QStringLiteral("MotionRipple"));
    for (QWidget *old : stale)
        old->deleteLater();

    const QPoint center = pos.x() < 0 ? w->rect().center() : pos;
    const QColor accent = UiTheme::I().Tokens().accent;
    QPointer<QWidget> guard(w);
    // 排到本轮事件之后：按钮先处理按下、画出 :pressed 态，涟漪再把这份像素当底，
    // 否则会把按下高亮盖回未按下时的样子。
    QTimer::singleShot(0, w, [guard, center, accent] {
        if (!guard || !guard->isVisible() || guard->width() <= 0)
            return;
        auto *layer = new RippleLayer(guard, center, accent, guard->grab());
        layer->show();
        layer->raise();
        layer->Start(Fast * 2 + 80);
    });
}

void InstallTapFeedback()
{
    if (g_tapFilter)
        return;
    g_tapFilter = new TapFilter(qApp);
    qApp->installEventFilter(g_tapFilter);
}

void UninstallTapFeedback()
{
    if (!g_tapFilter)
        return;
    qApp->removeEventFilter(g_tapFilter);
    delete g_tapFilter;
    g_tapFilter = nullptr;
}

void FadeIn(QWidget *w, int ms)
{
    if (!w || !w->isVisible())
        return;
    if (!g_enabled || ms <= Instant) {
        if (w->graphicsEffect())
            w->setGraphicsEffect(nullptr);
        return;
    }
    auto *eff = new QGraphicsOpacityEffect(w);
    eff->setOpacity(0.0);
    w->setGraphicsEffect(eff);
    auto *a = new QPropertyAnimation(eff, "opacity", w);
    a->setDuration(ms);
    a->setStartValue(0.0);
    a->setEndValue(1.0);
    a->setEasingCurve(EnterCurve());
    QObject::connect(a, &QPropertyAnimation::finished, w, [w, a] {
        a->deleteLater();
        // 动画结束必须摘掉 effect：留着会让这棵子树永久走离屏合成
        QTimer::singleShot(0, w, [w] {
            if (w)
                w->setGraphicsEffect(nullptr);
        });
    });
    a->start();
}

void FadeOut(QWidget *w, int ms, const std::function<void()> &done)
{
    if (!w) {
        if (done)
            done();
        return;
    }
    if (!g_enabled || ms <= Instant || !w->isVisible()) {
        if (done)
            done();
        return;
    }
    auto *eff = new QGraphicsOpacityEffect(w);
    eff->setOpacity(1.0);
    w->setGraphicsEffect(eff);
    auto *a = new QPropertyAnimation(eff, "opacity", w);
    a->setDuration(ms);
    a->setStartValue(1.0);
    a->setEndValue(0.0);
    a->setEasingCurve(ExitCurve());
    QObject::connect(a, &QPropertyAnimation::finished, w, [w, a, done] {
        a->deleteLater();
        if (done)
            done();
        QTimer::singleShot(0, w, [w] {
            if (w)
                w->setGraphicsEffect(nullptr);
        });
    });
    a->start();
}

void CrossFade(QWidget *host, const std::function<void()> &mutate, int ms)
{
    if (!host || !mutate)
        return;
    if (!g_enabled || ms <= Instant || !host->isVisible() || host->width() <= 0
        || host->height() <= 0) {
        mutate();
        return;
    }

    // 上一轮还没淡完（连点/连续切分区）：先收掉，否则两层叠着会压暗画面
    const auto stale = host->findChildren<QWidget *>(QStringLiteral("MotionFadeOverlay"));
    for (QWidget *w : stale)
        w->deleteLater();

    const QPixmap before = host->grab();
    if (before.isNull() || LooksFlat(before)) {
        // 抓不到有内容的旧画面：直接改，不留"底色渐变"这种假动效
        mutate();
        return;
    }

    // 先盖旧画面，再改内容：改的过程中（换主题这种几十上百毫秒的重排）用户
    // 看到的是稳定的旧画面，而不是半新半旧的中间态。
    auto *layer = new FadeLayer(host, before);
    layer->setObjectName(QStringLiteral("MotionFadeOverlay"));
    layer->setGeometry(host->rect());
    layer->show();
    layer->raise();

    mutate();

    layer->setGeometry(host->rect());
    layer->raise();
    layer->Start(ms);
}

void SlideTo(QWidget *w, const QPoint &to, int ms, const QEasingCurve &curve,
             const std::function<void()> &done)
{
    if (!w)
        return;
    const QPoint from = w->pos();
    if (!g_enabled || ms <= Instant || from == to) {
        w->move(to);
        if (done)
            done();
        return;
    }
    Animate(w, QStringLiteral("slide"), 0.0, 1.0, ms, curve,
            [w, from, to](qreal t) {
                w->move(qRound(from.x() + (to.x() - from.x()) * t),
                        qRound(from.y() + (to.y() - from.y()) * t));
            },
            done);
}

}  // namespace Motion
