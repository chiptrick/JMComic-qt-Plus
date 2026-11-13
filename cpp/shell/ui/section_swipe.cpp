#include "section_swipe.h"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPair>
#include <QQuickWidget>
#include <QResizeEvent>
#include <QTimer>
#include <QTouchEvent>
#include <QVBoxLayout>
#include <QVariantAnimation>

#include "comic_view.h"
#include "motion.h"
#include "section_strip.h"
#include "theme.h"
#include "touch.h"

namespace {

// 离手补间的最短时长：再短就"啪"地落位，看不出方向
constexpr int kSettleMinMs = 120;
// 速度采样窗口：只看最近 100ms，避免"先慢后快"把速度算低
constexpr qint64 kVelocityWindowMs = 100;
// 判横滑的比值：|dx| 必须大于 |dy| × 1.2
constexpr qreal kAxisRatio = 1.2;

// 触摸模式下贴左边缘的一窄条留给系统"返回"手势，不抢来切分区
int EdgeGuardPx()
{
    return Touch::Dp(24);
}

}  // namespace

SectionSwipe::SectionSwipe(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::NoFocus);
    // 应用级过滤器：卡片墙是 QQuickWidget（事件不会冒泡到父控件），
    // 只装在容器上收不到拖动；这里用 isAncestorOf 过滤，只处理自己的子树。
    if (qApp)
        qApp->installEventFilter(this);
}

SectionSwipe::~SectionSwipe()
{
    if (qApp)
        qApp->removeEventFilter(this);
}

// ---------------------------------------------------------------- 页面

void SectionSwipe::ClearPanes()
{
    Interrupt();
    for (const Pane &p : panes_) {
        if (p.host) {
            p.host->hide();
            p.host->deleteLater();
        }
    }
    panes_.clear();
    grids_.clear();
    cur_ = 0;
    incoming_ = -1;
    offset_ = 0;
    SetPhase(Phase::Idle);
}

void SectionSwipe::SetPanes(const QStringList &titles, const QVector<QWidget *> &contents)
{
    ClearPanes();
    const int n = qMin(titles.size(), contents.size());
    if (n <= 0)
        return;

    for (int i = 0; i < n; ++i) {
        auto *host = new QWidget(this);
        auto *v = new QVBoxLayout(host);
        v->setContentsMargins(0, 0, 0, 0);
        v->setSpacing(0);

        auto *strip = new SectionStrip(host);
        strip->SetItems(titles);
        // 每一页自带一份分区栏：切换时它跟着页面一起滑走/滑入。
        // 建页时直接定位到本页选中项，不做补间（此时还没显示）。
        strip->SetCurrentIndexImmediate(i, true);
        v->addWidget(strip);

        QWidget *content = contents.at(i);
        content->setParent(host);
        v->addWidget(content, 1);

        connect(strip, &SectionStrip::CurrentChanged, this, [this, i, strip](int idx) {
            // 点到"当前页"的分区栏才切换；否则把选中还原，页面不动标签不跑
            if (i != cur_ || idx == cur_ || phase_ != Phase::Idle || !isVisible()) {
                if (idx != i)
                    strip->SetCurrentIndexImmediate(i, true);
                return;
            }
            strip->SetCurrentIndexImmediate(i, true);
            SetCurrent(idx, true);
        });

        panes_.push_back({host, strip, content});
        const auto found = content->findChildren<ComicGrid *>();
        for (ComicGrid *g : found)
            grids_.push_back(g);
    }

    cur_ = 0;
    incoming_ = -1;
    offset_ = 0;
    for (int i = 0; i < panes_.size(); ++i) {
        panes_[i].host->setGeometry(0, 0, width(), height());
        panes_[i].host->setVisible(i == 0);
    }
}

SectionStrip *SectionSwipe::CurrentStrip() const
{
    return panes_.value(cur_).strip;
}

QWidget *SectionSwipe::CurrentPane() const
{
    return panes_.value(cur_).host;
}

int SectionSwipe::StripOffset() const
{
    SectionStrip *s = CurrentStrip();
    return s ? s->Offset() : 0;
}

int SectionSwipe::StripContentWidth() const
{
    SectionStrip *s = CurrentStrip();
    return s ? s->ContentWidth() : 0;
}

bool SectionSwipe::StripDragging() const
{
    SectionStrip *s = CurrentStrip();
    return s && s->IsDragging();
}

void SectionSwipe::StripDrag(int dx)
{
    if (SectionStrip *s = CurrentStrip())
        s->DemoDrag(dx);
}

void SectionSwipe::StripTap(int x)
{
    if (SectionStrip *s = CurrentStrip())
        s->DemoTap(x);
}

bool SectionSwipe::IsBusy() const
{
    return phase_ != Phase::Idle;
}

QString SectionSwipe::StateReport() const
{
    return QStringLiteral("sections cur=%1/%2 offset=%3 incoming=%4 phase=%5 w=%6")
        .arg(cur_)
        .arg(panes_.size())
        .arg(offset_)
        .arg(incoming_)
        .arg(phase_ == Phase::Idle        ? QStringLiteral("idle")
             : phase_ == Phase::Tracking  ? QStringLiteral("tracking")
             : phase_ == Phase::Dragging  ? QStringLiteral("dragging")
                                          : QStringLiteral("settling"))
        .arg(width());
}

void SectionSwipe::DemoSwipe(int dx, int dy, int steps)
{
    if (panes_.isEmpty() || steps < 2)
        return;
    QWidget *target = panes_.at(cur_).content ? panes_.at(cur_).content : panes_.at(cur_).host;
    if (!target)
        return;
    const QPoint origin = target->rect().center();
    auto send = [target](QEvent::Type type, const QPoint &local, Qt::MouseButton button,
                         Qt::MouseButtons buttons) {
        QMouseEvent ev(type, QPointF(local), QPointF(target->mapToGlobal(local)), button, buttons,
                       Qt::NoModifier);
        QCoreApplication::sendEvent(target, &ev);
    };
    send(QEvent::MouseButtonPress, origin, Qt::LeftButton, Qt::LeftButton);
    for (int i = 1; i <= steps; ++i)
        send(QEvent::MouseMove, origin + QPoint(dx * i / steps, dy * i / steps), Qt::NoButton,
             Qt::LeftButton);
    send(QEvent::MouseButtonRelease, origin + QPoint(dx, dy), Qt::LeftButton, Qt::NoButton);
}

void SectionSwipe::FreezeAt(int from, int to, qreal progress)
{
    if (panes_.isEmpty())
        return;
    Interrupt();
    cur_ = qBound(0, from, panes_.size() - 1);
    if (QWidget *h = panes_.value(cur_).host) {
        h->setGeometry(0, 0, width(), height());
        h->show();
        h->raise();
    }
    if (to < 0 || to >= panes_.size() || to == cur_) {
        offset_ = 0;
        ApplyOffset();
        return;
    }
    SetIncoming(to);
    const int dir = to > cur_ ? -1 : 1;
    offset_ = qRound(dir * qBound(0.0, progress, 1.0) * qMax(1, width()));
    ApplyOffset();
}

// ---------------------------------------------------------------- 几何

void SectionSwipe::SetIncoming(int index)
{
    if (index == incoming_)
        return;
    if (incoming_ >= 0) {
        if (QWidget *h = panes_.value(incoming_).host)
            h->hide();
    }
    incoming_ = index;
    if (incoming_ >= 0) {
        if (QWidget *h = panes_.value(incoming_).host) {
            h->setGeometry(0, 0, width(), height());
            h->show();
            h->raise();
        }
    }
}

void SectionSwipe::ApplyOffset()
{
    const int w = qMax(1, width());
    if (QWidget *h = panes_.value(cur_).host)
        h->setGeometry(offset_, 0, w, height());
    if (incoming_ >= 0) {
        if (QWidget *h = panes_.value(incoming_).host) {
            const int x = incoming_ > cur_ ? offset_ + w : offset_ - w;
            h->setGeometry(x, 0, w, height());
        }
    }
}

void SectionSwipe::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    for (const Pane &p : panes_) {
        if (p.host)
            p.host->setGeometry(0, 0, width(), height());
    }
    if (phase_ == Phase::Dragging)
        offset_ = qBound(-width(), offset_, width());
    ApplyOffset();
}

// ---------------------------------------------------------------- 切换

void SectionSwipe::Interrupt()
{
    if (anim_) {
        anim_->stop();
        anim_ = nullptr;
    }
    if (incoming_ >= 0)
        AbortIncoming();
    SetPhase(Phase::Idle);
}

void SectionSwipe::SetCurrent(int index, bool animate)
{
    if (index < 0 || index >= panes_.size() || index == cur_)
        return;
    Interrupt();

    const bool instant = !animate || slideMs_ <= 0 || !Motion::Enabled();
    if (instant) {
        const int old = cur_;
        cur_ = index;
        offset_ = 0;
        if (QWidget *h = panes_.value(old).host)
            h->hide();
        if (QWidget *h = panes_.value(cur_).host) {
            h->setGeometry(0, 0, width(), height());
            h->show();
            h->raise();
        }
        emit CurrentChanged(cur_);
        return;
    }

    // 相邻或跳转都按"整页滑一屏"处理：方向可读，不做多屏连滑
    SetIncoming(index);
    const int dir = index > cur_ ? -1 : 1;
    offset_ = 0;
    ApplyOffset();
    AnimateTo(dir * qMax(1, width()), true);
}

void SectionSwipe::AnimateTo(int target, bool commit)
{
    const int w = qMax(1, width());
    const int dist = qAbs(target - offset_);
    int ms = (slideMs_ > 0 && Motion::Enabled()) ? int(slideMs_ * qreal(dist) / qreal(w)) : 0;
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
    // 位移只有一条曲线（与 PageSwipe.qml / 模块切换一致）
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

void SectionSwipe::CommitIncoming()
{
    if (incoming_ < 0)
        return;
    const int old = cur_;
    cur_ = incoming_;
    incoming_ = -1;
    offset_ = 0;
    if (QWidget *h = panes_.value(old).host)
        h->hide();
    if (QWidget *h = panes_.value(cur_).host) {
        h->setGeometry(0, 0, width(), height());
        h->show();
        h->raise();
    }
    SetPhase(Phase::Idle);
    emit CurrentChanged(cur_);
}

void SectionSwipe::AbortIncoming()
{
    if (incoming_ >= 0) {
        if (QWidget *h = panes_.value(incoming_).host)
            h->hide();
    }
    incoming_ = -1;
    offset_ = 0;
    if (QWidget *h = panes_.value(cur_).host) {
        h->setGeometry(0, 0, width(), height());
        h->show();
    }
    SetPhase(Phase::Idle);
}

void SectionSwipe::FinishSettle(bool commit)
{
    if (commit && incoming_ >= 0)
        CommitIncoming();
    else
        AbortIncoming();
    UnlockCardsSoon();
}

void SectionSwipe::SetPhase(Phase phase)
{
    phase_ = phase;
    const bool dragging = (phase == Phase::Dragging);
    setCursor(dragging ? Qt::ClosedHandCursor : Qt::ArrowCursor);
}

// ---------------------------------------------------------------- 手势

bool SectionSwipe::IsStripTarget(QWidget *w) const
{
    for (QWidget *p = w; p && p != this; p = p->parentWidget()) {
        if (qobject_cast<SectionStrip *>(p))
            return true;
    }
    return false;
}

bool SectionSwipe::InsideQuickWidget(QWidget *w)
{
    for (QWidget *p = w; p; p = p->parentWidget()) {
        if (qobject_cast<QQuickWidget *>(p))
            return true;
    }
    return false;
}

bool SectionSwipe::HandlePress(const QPoint &global, QWidget *target)
{
    // 注意：这里不能在关动效时直接返回 —— 关动效只是"时长归 0"，
    // 手势仍然要能切换（AnimateTo 会走 ms<=0 的瞬切分支）。
    if (panes_.isEmpty())
        return false;

    // 触摸模式下贴左边缘留给系统返回手势
    if (Touch::IsTouchMode() && target && target->window()) {
        const QPoint winLeft = target->window()->mapToGlobal(QPoint(0, 0));
        if (global.x() < winLeft.x() + EdgeGuardPx())
            return false;
    }

    if (phase_ == Phase::Settling && anim_) {
        const bool farJump = qAbs(incoming_ - cur_) > 1;
        anim_->stop();
        anim_ = nullptr;
        if (farJump)
            FinishSettle(qAbs(offset_) > width() / 2);
    }
    if (phase_ != Phase::Idle && phase_ != Phase::Settling)
        return false;

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

void SectionSwipe::LockHorizontal()
{
    SetPhase(Phase::Dragging);
    // 卡片墙的点击由 QML 侧统一锁掉（MouseArea 的 onClicked 会先看这个标记）
    SetCardLock(true);
    // 普通控件（按钮等）：补一次界外 release，让按下态复位且不触发点击
    if (pressTarget_ && !InsideQuickWidget(pressTarget_))
        CancelChildPress(pressTarget_);
    pressTarget_ = nullptr;
}

void SectionSwipe::CancelChildPress(QWidget *target)
{
    if (!target)
        return;
    const QPointF far(-10000.0, -10000.0);
    QMouseEvent release(QEvent::MouseButtonRelease, far,
                        QPointF(target->mapToGlobal(far.toPoint())), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    // 应用级过滤器会收到我们自己补发的这次 release：不挡住的话它会被当成
    // "手势结束"，在还没开始位移时就结算（实测 offset 停在 0）。
    sendingCancel_ = true;
    QCoreApplication::sendEvent(target, &release);
    sendingCancel_ = false;
}

void SectionSwipe::HandleMove(const QPoint &global)
{
    if (phase_ != Phase::Tracking && phase_ != Phase::Dragging)
        return;
    const QPoint d = global - pressGlobal_;

    if (phase_ == Phase::Tracking) {
        if (axisDead_)
            return;
        if (qMax(qAbs(d.x()), qAbs(d.y())) < axisLock_)
            return;
        if (qAbs(d.x()) <= qAbs(d.y()) * kAxisRatio) {
            axisDead_ = true;   // 纵向手势：整段不再参与横滑
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

void SectionSwipe::ApplyDrag(int rawOffset)
{
    const int w = qMax(1, width());
    rawOffset = qBound(-w, rawOffset, w);   // 一次手势最多一页

    if (rawOffset == 0) {
        SetIncoming(-1);
        offset_ = 0;
        ApplyOffset();
        return;
    }

    const int candidate = cur_ + (rawOffset < 0 ? 1 : -1);
    if (candidate < 0 || candidate >= panes_.size()) {
        // 首/尾：橡皮筋，且不转交给模块层
        SetIncoming(-1);
        offset_ = qRound(rawOffset * edgeResist_);
        ApplyOffset();
        return;
    }
    if (incoming_ < 0 || incoming_ != candidate)
        SetIncoming(candidate);
    offset_ = rawOffset;
    ApplyOffset();
}

qreal SectionSwipe::SampleVelocity() const
{
    if (samples_.size() < 2)
        return 0.0;
    const auto &last = samples_.last();
    const auto &first = samples_.first();
    const qint64 dt = last.first - first.first;
    if (dt < kVelocityWindowMs / 4 || dt <= 0)
        return 0.0;
    return (last.second - first.second) / qreal(dt);
}

void SectionSwipe::Settle()
{
    const int w = qMax(1, width());
    const qreal p = qAbs(offset_) / qreal(w);
    const qreal v = SampleVelocity();

    bool commit = false;
    if (incoming_ >= 0) {
        if (p >= commitRatio_)
            commit = true;
        else if (qAbs(v) >= commitVelocity_ && (v < 0) == (offset_ < 0))
            commit = true;   // 小幅快甩也翻页
    }
    pressTarget_ = nullptr;
    AnimateTo(commit ? (offset_ < 0 ? -w : w) : 0, commit);
}

void SectionSwipe::HandleRelease()
{
    if (phase_ == Phase::Dragging)
        Settle();
    else if (phase_ == Phase::Tracking)
        SetPhase(Phase::Idle);
}

void SectionSwipe::SetCardLock(bool on)
{
    for (ComicGrid *g : grids_) {
        if (g)
            g->SetSwipeLock(on);
    }
}

void SectionSwipe::UnlockCardsSoon()
{
    // 松手事件还要下发给 QML（我们只观察、不抢），点击判定就在同一次派发里；
    // 排到下一轮再解锁，才能保证"拖完那一下"不算点击。
    QTimer::singleShot(0, this, [this] { SetCardLock(false); });
}

bool SectionSwipe::eventFilter(QObject *watched, QEvent *event)
{
    auto *w = qobject_cast<QWidget *>(watched);
    if (!w || w == this || panes_.isEmpty())
        return false;
    if (!isAncestorOf(w) || IsStripTarget(w))
        return false;

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton)
            HandlePress(me->globalPosition().toPoint(), w);
        return false;   // 按下必须放行：点击/长按照常
    }
    case QEvent::MouseMove: {
        if (phase_ != Phase::Tracking && phase_ != Phase::Dragging)
            return false;
        auto *me = static_cast<QMouseEvent *>(event);
        if (!(me->buttons() & Qt::LeftButton))
            return false;
        HandleMove(me->globalPosition().toPoint());
        return false;   // 不吞：QML 的 Flickable 自己判断纵向滚动
    }
    case QEvent::MouseButtonRelease: {
        if (sendingCancel_)
            return false;   // 自己补发的界外 release，不参与状态机
        if (phase_ == Phase::Dragging || phase_ == Phase::Tracking)
            HandleRelease();
        return false;
    }
    case QEvent::TouchBegin: {
        auto *te = static_cast<QTouchEvent *>(event);
        if (te->points().size() != 1)
            return false;
        const QEventPoint &p = te->points().first();
        if (p.state() != QEventPoint::State::Pressed)
            return false;
        touchId_ = p.id();
        HandlePress(p.globalPosition().toPoint(), w);
        return false;
    }
    case QEvent::TouchUpdate: {
        if (phase_ != Phase::Tracking && phase_ != Phase::Dragging)
            return false;
        auto *te = static_cast<QTouchEvent *>(event);
        if (te->points().size() > 1) {
            // 多指（捏合等）：整段手势作废，不当横滑
            if (phase_ == Phase::Dragging)
                AnimateTo(0, false);
            else
                SetPhase(Phase::Idle);
            touchId_ = -1;
            return false;
        }
        for (const QEventPoint &p : te->points()) {
            if (p.id() == touchId_)
                HandleMove(p.globalPosition().toPoint());
        }
        return false;
    }
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        if (phase_ == Phase::Dragging || phase_ == Phase::Tracking)
            HandleRelease();
        touchId_ = -1;
        return false;
    }
    default:
        break;
    }
    return false;
}
