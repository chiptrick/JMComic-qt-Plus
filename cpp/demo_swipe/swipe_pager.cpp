#include "swipe_pager.h"

#include <QAbstractAnimation>

#include <QApplication>
#include <QGraphicsOpacityEffect>
#include <QMouseEvent>
#include <QPair>
#include <QResizeEvent>
#include <QVariantAnimation>

#include <cmath>

namespace {

// 离手补间的最短时长：再短就会"啪"地落位，看不出方向
constexpr int kSettleMinMs = 120;

// 手势起始位移的采样窗口（毫秒）：只看最近 100ms，避免"先慢后快"把速度算低
constexpr qint64 kVelocityWindowMs = 100;

// 判定纵向的比值：|dx| 必须大于 |dy| * 1.2 才算横滑。
// 取 1.2 而不是 1.0，是为了对角手势别在两页之间"半开"。
constexpr qreal kAxisRatio = 1.2;

// 从 watched 往上找，看有没有"别的"SlidePager。
// 嵌套场景（首页里再嵌一个分区 pager）必须由内层接管，外层直接放手。
bool HasForeignPager(QWidget *w, const SlidePager *self)
{
    for (QWidget *p = w; p; p = p->parentWidget()) {
        if (auto *sp = qobject_cast<SlidePager *>(p)) {
            if (sp != self)
                return true;
        }
    }
    return false;
}

}  // namespace

SlidePager::SlidePager(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::NoFocus);
    // 应用级过滤器：页面里的子控件（卡片、按钮、输入框）不装过滤器也能被同一套
    // 状态机接管。用 isAncestorOf 过滤，别的窗口/别的 pager 不参与。
    if (qApp)
        qApp->installEventFilter(this);
}

SlidePager::~SlidePager()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

void SlidePager::AddPage(QWidget *page)
{
    if (!page)
        return;
    page->setParent(this);
    page->setGeometry(rect());
    page->hide();
    pages_.push_back(page);
    if (pages_.size() == 1) {
        cur_ = 0;
        page->show();
    }
}

QString SlidePager::PhaseName() const
{
    switch (phase_) {
    case Phase::Idle:     return QStringLiteral("idle");
    case Phase::Tracking: return QStringLiteral("tracking");
    case Phase::Dragging: return QStringLiteral("dragging");
    case Phase::Settling: return QStringLiteral("settling");
    case Phase::Frozen:   return QStringLiteral("frozen");
    }
    return QStringLiteral("?");
}

bool SlidePager::Busy() const
{
    return phase_ == Phase::Tracking || phase_ == Phase::Dragging || phase_ == Phase::Settling;
}

void SlidePager::SetPhase(Phase phase)
{
    if (phase_ == phase)
        return;
    phase_ = phase;
    emit PhaseChanged(PhaseName());
}

// ---------------------------------------------------------------- 页面几何

void SlidePager::SyncPages()
{
    for (int i = 0; i < pages_.size(); ++i) {
        QWidget *p = pages_.at(i);
        p->setGeometry(0, 0, width(), height());
        const bool want = (i == cur_);
        if (p->isVisible() != want)
            p->setVisible(want);
    }
    if (QWidget *c = pages_.value(cur_))
        c->raise();
}

void SlidePager::SetIncoming(int index)
{
    if (index == incoming_)
        return;
    if (incoming_ >= 0) {
        if (QWidget *old = pages_.value(incoming_))
            old->hide();
    }
    incoming_ = index;
    if (incoming_ >= 0) {
        if (QWidget *n = pages_.value(incoming_)) {
            n->setGeometry(rect());
            n->show();
            n->raise();
        }
    }
}

void SlidePager::ApplyOffset()
{
    const int w = qMax(1, width());
    if (QWidget *c = pages_.value(cur_))
        c->setGeometry(offset_, 0, w, height());
    if (incoming_ >= 0) {
        if (QWidget *n = pages_.value(incoming_)) {
            // 与当前页共用同一个 offset，边界值相同 → 接缝严格贴合
            const int x = incoming_ > cur_ ? offset_ + w : offset_ - w;
            n->setGeometry(x, 0, w, height());
        }
    }
    progress_ = offset_ / qreal(w);
    emit ProgressChanged(progress_);
    TickFrame();
}

void SlidePager::TickFrame()
{
    // 只在"真的在动"时统计帧间隔：静止期的 ApplyOffset（resize/切页）间隔是几百 ms，
    // 混进来会把均值拉到没法看。
    if (phase_ != Phase::Dragging && phase_ != Phase::Settling) {
        frameClock_.invalidate();
        return;
    }
    if (frameClock_.isValid()) {
        const qint64 d = frameClock_.restart();
        // 指数平均：单帧抖动不刷屏，持续掉帧能看出来
        frameMs_ = frameMs_ <= 0.0 ? qreal(d) : frameMs_ * 0.7 + qreal(d) * 0.3;
        ++frameCount_;
    } else {
        frameClock_.start();
    }
}

void SlidePager::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    for (QWidget *p : pages_)
        p->setGeometry(0, 0, width(), height());
    if (phase_ == Phase::Dragging && !grabbed_)
        offset_ = qBound(-width(), offset_, width());
    ApplyOffset();
}

// ---------------------------------------------------------------- 手势状态机

bool SlidePager::HandlePress(const QPoint &global, QWidget *target)
{
    if (style_ != Style::Slide || pages_.isEmpty() || phase_ == Phase::Frozen)
        return false;

    if (phase_ == Phase::Settling && anim_) {
        const bool farJump = qAbs(incoming_ - cur_) > 1;
        anim_->stop();
        anim_ = nullptr;
        if (farJump) {
            // 非相邻跳转被打断：直接落定，不再"半途接管"（它会跨好几屏，接管无意义）
            FinishSettle(offset_ * (incoming_ > cur_ ? -1 : 1) > width() / 2);
        }
        // 相邻补间被打断：保留 offset_，从当前位置继续跟手
    }
    if (phase_ != Phase::Idle && phase_ != Phase::Settling && phase_ != Phase::Dragging)
        return false;
    if (phase_ == Phase::Dragging)
        return false;   // 上一次手势没收到 release（丢焦点），忽略新的按下

    dragBase_ = offset_;
    pressGlobal_ = global;
    pressTarget_ = target;
    samples_.clear();
    axisDead_ = false;
    gestureClock_.start();
    samples_.push_back({0, offset_});
    SetPhase(Phase::Tracking);
    return true;
}

void SlidePager::LockHorizontal()
{
    SetPhase(Phase::Dragging);
    // 只有真实指针才抓：合成手势（target == nullptr）没有子控件按下态需要收尾
    if (pressTarget_ && !grabbed_) {
        grabMouse();
        grabbed_ = true;
    }
    CancelChildPress();
}

// 把"被我们抢走的那次按下"在孩子那边收尾：发一个落在控件之外的 release，
// 子控件（QAbstractButton / QScroller 的 viewport）收到后自己复位，不会误触发点击。
void SlidePager::CancelChildPress()
{
    if (!pressTarget_)
        return;
    QWidget *t = pressTarget_;
    pressTarget_ = nullptr;
    const QPointF far(-10000.0, -10000.0);
    const QPointF globalFar(t->mapToGlobal(far.toPoint()));
    QMouseEvent release(QEvent::MouseButtonRelease, far, globalFar, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(t, &release);
}

void SlidePager::ReleasePointer()
{
    if (grabbed_) {
        releaseMouse();
        grabbed_ = false;
    }
    pressTarget_ = nullptr;
}

void SlidePager::HandleMove(const QPoint &global, Qt::MouseButtons buttons)
{
    if (phase_ != Phase::Tracking && phase_ != Phase::Dragging)
        return;
    if (!(buttons & Qt::LeftButton))
        return;

    const QPoint d = global - pressGlobal_;

    if (phase_ == Phase::Tracking) {
        if (axisDead_)
            return;
        if (qMax(qAbs(d.x()), qAbs(d.y())) < axisLock_)
            return;   // 还没到锁定阈值：什么都不做，交给子控件
        if (qAbs(d.x()) <= qAbs(d.y()) * kAxisRatio) {
            axisDead_ = true;   // 纵向手势：整段手势不再参与横滑
            emit SwipeRejected(QStringLiteral("纵向手势 → 交给内容滚动"));
            return;
        }
        LockHorizontal();
    }

    samples_.push_back({gestureClock_.elapsed(), dragBase_ + d.x()});
    while (samples_.size() > 10)
        samples_.removeFirst();

    ApplyDrag(dragBase_ + d.x());
}

void SlidePager::ApplyDrag(int rawOffset)
{
    const int w = qMax(1, width());
    // 一次手势最多一页：位移上限就是一屏
    rawOffset = qBound(-w, rawOffset, w);

    if (rawOffset == 0) {
        SetIncoming(-1);
        offset_ = 0;
        ApplyOffset();
        return;
    }

    const int step = rawOffset < 0 ? 1 : -1;
    const int candidate = cur_ + step;

    if (candidate < 0 || candidate >= pages_.size()) {
        // 边界：橡皮筋，且**不**转交外层（分区首尾不等于模块切换）
        SetIncoming(-1);
        offset_ = qRound(rawOffset * edgeResist_);
        ApplyOffset();
        return;
    }

    if (incoming_ < 0 || (qAbs(incoming_ - cur_) == 1 && incoming_ != candidate))
        SetIncoming(candidate);

    offset_ = rawOffset;
    ApplyOffset();
}

qreal SlidePager::SampleVelocity() const
{
    if (velocityOverride_ != 0.0)
        return velocityOverride_;
    if (samples_.size() < 2)
        return 0.0;
    const auto &last = samples_.last();
    const auto &first = samples_.first();
    const qint64 dt = last.first - first.first;
    if (dt < kVelocityWindowMs / 4 || dt <= 0)
        return 0.0;
    return (last.second - first.second) / qreal(dt);
}

void SlidePager::HandleRelease()
{
    if (phase_ == Phase::Dragging)
        Settle();
    else if (phase_ == Phase::Tracking || phase_ == Phase::Settling)
        SetPhase(Phase::Idle);
}

void SlidePager::Settle()
{
    const int w = qMax(1, width());
    const qreal p = qAbs(offset_) / qreal(w);
    const qreal v = SampleVelocity();

    bool commit = false;
    if (incoming_ >= 0) {
        if (p >= commitRatio_)
            commit = true;
        else if (qAbs(v) >= commitVel_ && (v < 0) == (offset_ < 0))
            commit = true;
    }
    ReleasePointer();
    AnimateOffsetTo(commit ? (offset_ < 0 ? -w : w) : 0, commit);
}

void SlidePager::AnimateOffsetTo(int target, bool commit)
{
    const int w = qMax(1, width());
    const int dist = qAbs(target - offset_);
    int ms = slideMs_ > 0 ? int(slideMs_ * qreal(dist) / qreal(w)) : 0;
    if (ms > 0)
        ms = qMax(kSettleMinMs, ms);

    if (ms <= 0) {
        offset_ = target;
        ApplyOffset();
        FinishSettle(commit);
        return;
    }

    auto *a = new QVariantAnimation(this);
    a->setStartValue(offset_);
    a->setEndValue(target);
    a->setDuration(ms);
    // 位移只有一条曲线（与 PageSwipe.qml 的 OutCubic 一致）
    a->setEasingCurve(QEasingCurve::OutCubic);
    connect(a, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        offset_ = qRound(v.toReal());
        ApplyOffset();
    });
    connect(a, &QVariantAnimation::finished, this, [this, commit] {
        anim_ = nullptr;
        FinishSettle(commit);
    });
    anim_ = a;
    SetPhase(Phase::Settling);
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void SlidePager::CommitIncoming()
{
    const int old = cur_;
    if (incoming_ < 0)
        return;
    cur_ = incoming_;
    incoming_ = -1;
    offset_ = 0;
    progress_ = 0.0;
    if (QWidget *w = pages_.value(old))
        w->hide();
    SyncPages();
    emit ProgressChanged(0.0);
    emit CurrentChanged(cur_);
}

void SlidePager::AbortIncoming()
{
    if (incoming_ >= 0) {
        if (QWidget *n = pages_.value(incoming_))
            n->hide();
    }
    incoming_ = -1;
    offset_ = 0;
    progress_ = 0.0;
    SyncPages();
    ApplyOffset();
}

void SlidePager::FinishSettle(bool commit)
{
    if (commit && incoming_ >= 0)
        CommitIncoming();
    else
        AbortIncoming();
    SetPhase(Phase::Idle);
}

void SlidePager::Interrupt()
{
    if (anim_) {
        anim_->stop();
        anim_ = nullptr;
    }
    ReleasePointer();
    if (phase_ == Phase::Frozen)
        phase_ = Phase::Idle;
    if (incoming_ >= 0 || offset_ != 0)
        AbortIncoming();
    SetPhase(Phase::Idle);
}

void SlidePager::CancelGesture()
{
    if (phase_ == Phase::Tracking || phase_ == Phase::Dragging) {
        ReleasePointer();
        AbortIncoming();
        SetPhase(Phase::Idle);
    }
}

// ---------------------------------------------------------------- 程序切换

void SlidePager::SetCurrent(int index, bool animate)
{
    if (index < 0 || index >= pages_.size() || index == cur_)
        return;
    Interrupt();

    const bool instant = !animate || slideMs_ == 0 || (style_ == Style::CrossFade && fadeMs_ == 0);
    if (instant) {
        cur_ = index;
        offset_ = 0;
        progress_ = 0.0;
        SyncPages();
        emit ProgressChanged(0.0);
        emit CurrentChanged(cur_);
        return;
    }
    if (style_ == Style::CrossFade) {
        StartCrossFade(index);
        return;
    }

    // 相邻或跳转都按"整页滑一屏"处理：方向可读，但不做多屏连滑
    SetIncoming(index);
    const int dir = index > cur_ ? -1 : 1;
    offset_ = 0;
    ApplyOffset();
    AnimateOffsetTo(dir * qMax(1, width()), true);
}

void SlidePager::StartCrossFade(int target)
{
    QWidget *from = pages_.value(cur_);
    QWidget *to = pages_.value(target);
    if (!from || !to)
        return;

    to->setGeometry(rect());
    to->show();
    to->raise();

    auto *effFrom = new QGraphicsOpacityEffect(from);
    effFrom->setOpacity(1.0);
    from->setGraphicsEffect(effFrom);
    auto *effTo = new QGraphicsOpacityEffect(to);
    effTo->setOpacity(0.0);
    to->setGraphicsEffect(effTo);

    incoming_ = target;
    SetPhase(Phase::Settling);

    auto *a = new QVariantAnimation(this);
    a->setStartValue(0.0);
    a->setEndValue(1.0);
    a->setDuration(fadeMs_);
    a->setEasingCurve(QEasingCurve::InOutQuad);
    connect(a, &QVariantAnimation::valueChanged, this, [this, effFrom, effTo, target](const QVariant &v) {
        const qreal t = v.toReal();
        effFrom->setOpacity(1.0 - t);
        effTo->setOpacity(t);
        progress_ = (target > cur_ ? -1.0 : 1.0) * t;
        emit ProgressChanged(progress_);
    });
    connect(a, &QVariantAnimation::finished, this, [this, from, to] {
        // 摘掉 effect 会顺带 delete 掉它，之后不能再碰
        from->setGraphicsEffect(nullptr);
        to->setGraphicsEffect(nullptr);
        const int old = cur_;
        cur_ = incoming_;
        incoming_ = -1;
        offset_ = 0;
        if (old != cur_)
            pages_.value(old)->hide();
        SyncPages();
        progress_ = 0.0;
        emit ProgressChanged(0.0);
        emit CurrentChanged(cur_);
        SetPhase(Phase::Idle);
    });
    anim_ = a;
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void SlidePager::SetStyle(Style style)
{
    if (style_ == style)
        return;
    Interrupt();
    style_ = style;
}

// ---------------------------------------------------------------- 指针入口

void SlidePager::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        HandlePress(event->globalPosition().toPoint(), nullptr);
    event->accept();
}

void SlidePager::mouseMoveEvent(QMouseEvent *event)
{
    HandleMove(event->globalPosition().toPoint(), event->buttons());
    event->accept();
}

void SlidePager::mouseReleaseEvent(QMouseEvent *event)
{
    HandleRelease();
    event->accept();
}

bool SlidePager::eventFilter(QObject *watched, QEvent *event)
{
    auto *w = qobject_cast<QWidget *>(watched);
    if (!w || w == this || pages_.isEmpty())
        return false;
    if (!isAncestorOf(w) || HasForeignPager(w, this))
        return false;

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() != Qt::LeftButton)
            return false;
        HandlePress(me->globalPosition().toPoint(), w);
        return false;   // 按下必须放行：普通点击（卡片/按钮/输入框）照常工作
    }
    case QEvent::MouseMove: {
        if (phase_ != Phase::Tracking && phase_ != Phase::Dragging)
            return false;
        auto *me = static_cast<QMouseEvent *>(event);
        if (!(me->buttons() & Qt::LeftButton))
            return false;
        HandleMove(me->globalPosition().toPoint(), me->buttons());
        // 拖起来之后吞掉移动：子控件（滚动区）不要再跟着动
        return phase_ == Phase::Dragging;
    }
    case QEvent::MouseButtonRelease: {
        if (phase_ == Phase::Dragging) {
            HandleRelease();
            return true;
        }
        if (phase_ == Phase::Tracking) {
            SetPhase(Phase::Idle);
            return false;   // 纯点击：放给子控件
        }
        return false;
    }
    case QEvent::UngrabMouse:
    case QEvent::WindowDeactivate: {
        if (phase_ == Phase::Dragging)
            Settle();
        return false;
    }
    default:
        break;
    }
    return false;
}

// ---------------------------------------------------------------- 合成输入

void SlidePager::DemoSwipe(int dx, int dy, int steps)
{
    if (steps < 2)
        steps = 2;
    const QPoint origin = rect().center();
    const QPointF globalOrigin = QPointF(mapToGlobal(origin));

    QMouseEvent press(QEvent::MouseButtonPress, QPointF(origin), globalOrigin, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &press);
    for (int i = 1; i <= steps; ++i) {
        const QPoint p = origin + QPoint(dx * i / steps, dy * i / steps);
        QMouseEvent move(QEvent::MouseMove, QPointF(p), QPointF(mapToGlobal(p)), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(this, &move);
    }
    const QPoint end = origin + QPoint(dx, dy);
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(end), QPointF(mapToGlobal(end)),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &release);
}

void SlidePager::StartAutoSwipe(int dx, int dy, int ms)
{
    if (phase_ != Phase::Idle)
        Interrupt();
    const QPoint origin = rect().center();
    HandlePress(mapToGlobal(origin), nullptr);

    auto *a = new QVariantAnimation(this);
    a->setStartValue(0.0);
    a->setEndValue(1.0);
    a->setDuration(qMax(1, ms));
    // "手指"用匀速：这样离手速度是真实的，不会被曲线美化
    a->setEasingCurve(QEasingCurve::Linear);
    connect(a, &QVariantAnimation::valueChanged, this, [this, origin, dx, dy](const QVariant &v) {
        const qreal t = v.toReal();
        const QPoint p = origin + QPoint(qRound(dx * t), qRound(dy * t));
        HandleMove(mapToGlobal(p), Qt::LeftButton);
    });
    connect(a, &QVariantAnimation::finished, this, [this] { HandleRelease(); });
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void SlidePager::FreezeAt(int from, int to, qreal progress)
{
    if (pages_.isEmpty())
        return;
    Interrupt();
    cur_ = qBound(0, from, pages_.size() - 1);
    SyncPages();
    if (to < 0 || to >= pages_.size() || to == cur_) {
        incoming_ = -1;
        offset_ = 0;
        ApplyOffset();
        SetPhase(Phase::Frozen);
        return;
    }
    SetIncoming(to);
    const int dir = to > cur_ ? -1 : 1;
    offset_ = qRound(dir * qBound(0.0, progress, 1.0) * width());
    ApplyOffset();
    SetPhase(Phase::Frozen);
}

void SlidePager::ClearFreeze()
{
    if (phase_ != Phase::Frozen)
        return;
    AbortIncoming();
    SetPhase(Phase::Idle);
}
