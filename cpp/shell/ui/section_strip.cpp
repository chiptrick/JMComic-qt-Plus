#include "section_strip.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QtMath>

#include "app_font.h"
#include "motion.h"
#include "theme.h"
#include "touch.h"

namespace {
constexpr int kPadX = 14;      // 左右内边距（与 QSS 的 QTabBar::tab 一致）
constexpr int kUnderline = 2;  // 选中下划线
}  // namespace

SectionStrip::SectionStrip(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("SectionStrip"));
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    // 分区栏自己就有 8dp 滑动判定（moved_ + 首末同项），点击防误触层必须让路：
    // 吞掉它的抬起会让 EmitDragState(false) 与惯性都不执行，停在"拖动中"。
    setProperty("tapGuardSelfManaged", true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(sizeHintHeight());
    setFocusPolicy(Qt::NoFocus);

    // 主题/字号变化：文字宽度与配色都要重算
    connect(&UiTheme::I(), &UiTheme::Changed, this, [this] {
        widths_.clear();
        for (int i = 0; i < titles_.size(); ++i)
            widths_.push_back(TabWidth(i));
        ClampOffset();
        AnimateIndicator(false);
        update();
    });
    connect(&AppFont::I(), &AppFont::Changed, this, [this] {
        widths_.clear();
        for (int i = 0; i < titles_.size(); ++i)
            widths_.push_back(TabWidth(i));
        setFixedHeight(sizeHintHeight());
        ClampOffset();
        AnimateIndicator(false);
        update();
    });
}

int SectionStrip::sizeHintHeight() const
{
    QFont f = font();
    f.setPixelSize(UiStyle::Fpx(13));
    return qMax(UiStyle::Fpx(36), QFontMetrics(f).height() + UiStyle::Fpx(18));
}

void SectionStrip::SetItems(const QStringList &titles)
{
    titles_ = titles;
    widths_.clear();
    for (int i = 0; i < titles_.size(); ++i)
        widths_.push_back(TabWidth(i));
    current_ = titles_.isEmpty() ? 0 : qBound(0, current_, titles_.size() - 1);
    offset_ = 0;
    ClampOffset();
    EnsureVisible(current_, false);
    AnimateIndicator(false);
    update();
}

void SectionStrip::AnimateIndicator(bool animate)
{
    if (titles_.isEmpty() || widths_.isEmpty()) {
        indLeft_ = indRight_ = 0.0;
        return;
    }
    const qreal left = TabLeft(current_) + kPadX / 2.0;
    const qreal right = TabLeft(current_) + widths_.value(current_) - kPadX / 2.0;
    if (!animate || !isVisible()) {
        indLeft_ = left;
        indRight_ = right;
        update();
        return;
    }
    // 下划线在两个分区之间滑过去，而不是瞬间跳 —— 它同时告诉用户"移动了多远"
    const qreal l0 = indLeft_;
    const qreal r0 = indRight_;
    Motion::Animate(this, QStringLiteral("indicator"), 0.0, 1.0, Motion::Base, Motion::MoveCurve(),
                    [this, l0, r0, left, right](qreal t) {
                        indLeft_ = l0 + (left - l0) * t;
                        indRight_ = r0 + (right - r0) * t;
                        update();
                    });
}

int SectionStrip::TabWidth(int index) const
{
    if (index < 0 || index >= titles_.size())
        return 0;
    QFont f = font();
    f.setPixelSize(UiStyle::Fpx(13));
    const QFontMetrics fm(f);
    return fm.horizontalAdvance(titles_.at(index)) + kPadX * 2;
}

int SectionStrip::TabLeft(int index) const
{
    int x = 0;
    for (int i = 0; i < index && i < widths_.size(); ++i)
        x += widths_.at(i);
    return x;
}

int SectionStrip::ContentWidth() const
{
    int w = 0;
    for (int v : widths_)
        w += v;
    return w;
}

int SectionStrip::MaxOffset() const
{
    // 内容比视口窄时不允许滚动（offset 恒 0）
    return qMax(0, ContentWidth() - width());
}

void SectionStrip::ClampOffset()
{
    offset_ = qBound(0, offset_, MaxOffset());
}

void SectionStrip::SetCurrentIndex(int index)
{
    if (titles_.isEmpty())
        return;
    const int idx = qBound(0, index, titles_.size() - 1);
    if (idx == current_)
        return;
    current_ = idx;
    EnsureVisible(idx, true);
    AnimateIndicator(true);
    update();
}

void SectionStrip::SetCurrentIndexImmediate(int index, bool ensureVisible)
{
    if (titles_.isEmpty())
        return;
    current_ = qBound(0, index, titles_.size() - 1);
    if (ensureVisible)
        EnsureVisible(current_, false);
    AnimateIndicator(false);
    update();
}

int SectionStrip::IndexAtX(int x) const
{
    int left = -offset_;
    for (int i = 0; i < widths_.size(); ++i) {
        const int w = widths_.at(i);
        if (x >= left && x < left + w)
            return i;
        left += w;
    }
    return -1;
}

void SectionStrip::EnsureVisible(int index, bool animate)
{
    if (index < 0 || index >= widths_.size() || width() <= 0)
        return;
    const int left = TabLeft(index) - offset_;
    const int w = widths_.at(index);
    int target = offset_;
    if (left < 0)
        target = TabLeft(index);
    else if (left + w > width())
        target = TabLeft(index) + w - width();
    target = qBound(0, target, MaxOffset());
    if (target == offset_)
        return;
    if (!animate) {
        offset_ = target;
        update();
        return;
    }
    if (!Motion::Enabled()) {
        // 动效关闭：直接对齐（选区仍然可见，只是不做滑动过程）
        offset_ = target;
        update();
        return;
    }
    StartFling(0);   // 打断上一次惯性
    auto *anim = new QVariantAnimation(this);
    anim->setDuration(160);
    anim->setEasingCurve(QEasingCurve::OutCubic);
    anim->setStartValue(offset_);
    anim->setEndValue(target);
    connect(anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        offset_ = v.toInt();
        update();
    });
    connect(anim, &QVariantAnimation::finished, anim, &QObject::deleteLater);
    anim->start();
}

void SectionStrip::StartFling(qreal velocity)
{
    if (fling_)
        fling_->stop();
    if (qAbs(velocity) < 0.05)
        return;
    // 惯性位移：速度(px/ms) × 160ms，方向与拖动一致（offset 跟 dx 同向），
    // 触底由 ClampOffset 夹住（表现为回弹）
    // 手指向左拖（velocity<0）→ 内容继续向左 → 滚动位置增大
    const int delta = int(-velocity * 160.0);
    const int target = qBound(0, offset_ + delta, MaxOffset());
    if (target == offset_)
        return;
    if (!fling_) {
        fling_ = new QVariantAnimation(this);
        fling_->setDuration(240);
        fling_->setEasingCurve(QEasingCurve::OutCubic);
        connect(fling_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            offset_ = v.toInt();
            update();
        });
    }
    fling_->stop();
    fling_->setStartValue(offset_);
    fling_->setEndValue(target);
    fling_->start();
}

void SectionStrip::EmitDragState(bool dragging)
{
    if (dragging_ == dragging)
        return;
    dragging_ = dragging;
    emit DragStateChanged(dragging);
}

void SectionStrip::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);
        return;
    }
    if (fling_)
        fling_->stop();
    pressPos_ = e->position().toPoint();
    pressOffset_ = offset_;
    pressIndex_ = IndexAtX(pressPos_.x());
    moved_ = false;
    EmitDragState(true);
    e->accept();
}

void SectionStrip::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (!(e->buttons() & Qt::LeftButton)) {
        // hover 高亮（桌面）
        const int idx = IndexAtX(pos.x());
        if (idx != hover_) {
            hover_ = idx;
            update();
        }
        QWidget::mouseMoveEvent(e);
        return;
    }
    const int dx = pos.x() - pressPos_.x();
    const int dy = pos.y() - pressPos_.y();
    // 8dp 判定阈值：小于它仍算"点击"，避免点标签时手抖变成拖动
    if (!moved_ && qAbs(dx) < Touch::Dp(8) && qAbs(dy) < Touch::Dp(8))
        return;
    // 纵向为主的滑动不算拖标签栏（交给外层滚动）
    if (!moved_ && qAbs(dy) > qAbs(dx))
        return;
    moved_ = true;
    // 手指向左（dx<0）→ 内容向左 → 滚动位置增大
    offset_ = pressOffset_ - dx;
    ClampOffset();
    update();
    e->accept();
}

void SectionStrip::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(e);
        return;
    }
    const QPoint pos = e->position().toPoint();
    const bool wasDragging = moved_;
    EmitDragState(false);
    if (!wasDragging) {
        // 纯点击：选中手指下的分区（松开位置与按下位置都要在同一项内）
        const int idx = IndexAtX(pos.x());
        if (idx >= 0 && idx == pressIndex_ && idx != current_) {
            current_ = idx;
            EnsureVisible(idx, true);
            AnimateIndicator(true);
            update();
            emit CurrentChanged(idx);
        }
        e->accept();
        return;
    }
    // 拖动后松手：按最后 40ms 的位移估速度做惯性，绝不触发选中
    moved_ = false;
    const int dx = pos.x() - pressPos_.x();
    StartFling(qreal(dx) / 6.0 / 40.0);
    e->accept();
}

void SectionStrip::wheelEvent(QWheelEvent *e)
{
    const int dy = e->angleDelta().y();
    const int dx = e->angleDelta().x();
    const int delta = qAbs(dx) > qAbs(dy) ? dx : dy;
    if (delta == 0) {
        QWidget::wheelEvent(e);
        return;
    }
    offset_ -= delta / 2;   // 滚轮向下/向右 = 往后翻
    ClampOffset();
    update();
    e->accept();
}

void SectionStrip::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    ClampOffset();
    EnsureVisible(current_, false);
}

void SectionStrip::paintEvent(QPaintEvent *)
{
    const UiTokens &t = UiTheme::I().Tokens();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), Qt::transparent);

    QFont f = font();
    f.setPixelSize(UiStyle::Fpx(13));
    p.setFont(f);

    const int h = height();
    int left = -offset_;
    for (int i = 0; i < titles_.size(); ++i) {
        const int w = widths_.value(i);
        const QRect r(left, 0, w, h - kUnderline);
        const bool sel = (i == current_);
        QColor fg = sel ? t.accent : (i == hover_ ? t.text : t.textDim);
        p.setPen(fg);
        p.drawText(r, Qt::AlignCenter, titles_.at(i));
        left += w;
    }

    // 选中下划线单独绘制：位置是补间值（切换时从旧分区滑到新分区），
    // 不跟随某个分区做即时计算，这样"移动过程"本身可读。
    if (current_ >= 0 && current_ < titles_.size() && indRight_ > indLeft_) {
        const QRectF line(indLeft_ - offset_, h - kUnderline,
                          qMax<qreal>(1.0, indRight_ - indLeft_), kUnderline);
        p.fillRect(line, t.accent);
    }

    // 右侧渐隐：提示"还有更多分区"（内容未滚到底时）
    if (offset_ < MaxOffset()) {
        QLinearGradient g(width() - 28, 0, width(), 0);
        g.setColorAt(0.0, QColor(t.bg.red(), t.bg.green(), t.bg.blue(), 0));
        g.setColorAt(1.0, t.bg);
        p.fillRect(QRect(width() - 28, 0, 28, h), g);
    }
}

void SectionStrip::DemoDrag(int dx)
{
    const QPoint start(width() / 2, height() / 2);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(start), QPointF(mapToGlobal(start)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    mousePressEvent(&press);
    for (int i = 1; i <= 6; ++i) {
        const QPoint p = start + QPoint(dx * i / 6, 0);
        QMouseEvent mv(QEvent::MouseMove, QPointF(p), QPointF(mapToGlobal(p)), Qt::NoButton,
                       Qt::LeftButton, Qt::NoModifier);
        mouseMoveEvent(&mv);
    }
    const QPoint end = start + QPoint(dx, 0);
    QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(end), QPointF(mapToGlobal(end)),
                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    mouseReleaseEvent(&rel);
}

void SectionStrip::DemoScrollBy(int dx)
{
    offset_ += dx;   // 正数 = 往后翻
    ClampOffset();
    update();
}

void SectionStrip::DemoTap(int x)
{
    const QPoint p(x, height() / 2);
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), QPointF(mapToGlobal(p)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    mousePressEvent(&press);
    QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(p), QPointF(mapToGlobal(p)),
                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    mouseReleaseEvent(&rel);
}
