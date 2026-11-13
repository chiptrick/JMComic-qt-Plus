#include "demo_widgets.h"

#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QAbstractAnimation>

#include <QVariantAnimation>
#include <QtMath>

namespace {

const QColor kBg(0x31, 0x36, 0x3b);
const QColor kBgAlt(0x2a, 0x2e, 0x32);
const QColor kSurface(0x3a, 0x40, 0x45);
const QColor kSurfaceHover(0x44, 0x4b, 0x51);
const QColor kBorder(0x23, 0x26, 0x29);
const QColor kText(0xff, 0xff, 0xff);
const QColor kTextDim(0xb6, 0xbe, 0xc2);
const QColor kTextFaint(0x7d, 0x86, 0x8b);
const QColor kAccent(0x1d, 0xe9, 0xb6);

}  // namespace

// ============================================================ DemoStrip

DemoStrip::DemoStrip(QWidget *parent) : QWidget(parent)
{
    setCursor(Qt::PointingHandCursor);
    setFixedHeight(46);
}

void DemoStrip::SetItems(const QStringList &items)
{
    items_ = items;
    cur_ = 0;
    progress_ = 0.0;
    update();
}

void DemoStrip::SetState(int current, qreal progress)
{
    cur_ = qBound(0, current, qMax(0, items_.size() - 1));
    progress_ = progress;
    update();
}

QRect DemoStrip::SlotRect(int index) const
{
    const int n = qMax(1, items_.size());
    const int w = width() / n;
    return QRect(index * w, 0, index == n - 1 ? width() - index * w : w, height());
}

int DemoStrip::IndexAt(int x) const
{
    const int n = qMax(1, items_.size());
    if (n == 0 || width() <= 0)
        return -1;
    return qBound(0, x * n / qMax(1, width()), n - 1);
}

void DemoStrip::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), kBg);
    p.setPen(QPen(kBorder, 1));
    p.drawLine(0, height() - 1, width(), height() - 1);

    QFont f = font();
    f.setPixelSize(13);
    p.setFont(f);

    const qreal t = qAbs(progress_);
    const int dir = progress_ < 0 ? 1 : -1;
    const int glow = qBound(0, cur_ + dir, qMax(0, items_.size() - 1));

    for (int i = 0; i < items_.size(); ++i) {
        const QRect slot = SlotRect(i);
        QColor c = (i == cur_) ? kText : kTextDim;
        if (i == glow && progress_ != 0.0) {
            // 邻页文字随拖动进度点亮：看得出"下一站"是谁
            c = QColor(int(kTextDim.red() + (kText.red() - kTextDim.red()) * t),
                       int(kTextDim.green() + (kText.green() - kTextDim.green()) * t),
                       int(kTextDim.blue() + (kText.blue() - kTextDim.blue()) * t));
        }
        p.setPen(c);
        p.drawText(slot, Qt::AlignCenter, items_.at(i));
    }

    // 下划线：cur 与 neighbor 之间插值，宽度按文字宽度（与工程里的 SectionStrip 同规则）
    auto underlineRect = [&](int index) {
        const QRect slot = SlotRect(index);
        const int textW = p.fontMetrics().horizontalAdvance(items_.value(index));
        const int uw = qMin(slot.width() - 8, textW + 18);
        return QRect(slot.center().x() - uw / 2, height() - 3, uw, 2);
    };
    QRect a = underlineRect(cur_);
    if (progress_ != 0.0 && glow != cur_) {
        const QRect b = underlineRect(glow);
        a = QRect(qRound(a.x() + (b.x() - a.x()) * t), a.y(),
                  qRound(a.width() + (b.width() - a.width()) * t), 2);
    }
    p.fillRect(a, kAccent);
}

void DemoStrip::mouseReleaseEvent(QMouseEvent *e)
{
    const int idx = IndexAt(int(e->position().x()));
    if (idx >= 0 && e->position().y() < height())
        emit Activated(idx);
    QWidget::mouseReleaseEvent(e);
}

// ============================================================ NavRail

NavRail::NavRail(QWidget *parent) : QWidget(parent)
{
    setFixedWidth(78);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
}

void NavRail::SetItems(const QStringList &items)
{
    items_ = items;
    update();
}

void NavRail::SetState(int current, qreal progress)
{
    cur_ = qBound(0, current, qMax(0, items_.size() - 1));
    progress_ = progress;
    update();
}

QRect NavRail::ItemRect(int index) const
{
    const int n = qMax(1, items_.size());
    const int top = 10;
    const int h = qMax(1, (height() - top * 2) / n);
    return QRect(0, top + index * h, width(), h);
}

int NavRail::IndexAt(int y) const
{
    for (int i = 0; i < items_.size(); ++i) {
        if (ItemRect(i).contains(width() / 2, y))
            return i;
    }
    return -1;
}

QPainterPath NavRail::GlyphPath(int index, const QRectF &b) const
{
    QPainterPath path;
    const qreal s = b.width() / 20.0;
    switch (index % 5) {
    case 0: {   // 首页：屋顶 + 屋身
        path.moveTo(b.left() + 2 * s, b.top() + 9 * s);
        path.lineTo(b.center().x(), b.top() + 2 * s);
        path.lineTo(b.right() - 2 * s, b.top() + 9 * s);
        path.moveTo(b.left() + 4 * s, b.top() + 8 * s);
        path.lineTo(b.left() + 4 * s, b.bottom() - 2 * s);
        path.lineTo(b.right() - 4 * s, b.bottom() - 2 * s);
        path.lineTo(b.right() - 4 * s, b.top() + 8 * s);
        break;
    }
    case 1: {   // 搜索：圆 + 手柄
        path.addEllipse(QRectF(b.left() + 2 * s, b.top() + 2 * s, 11 * s, 11 * s));
        path.moveTo(b.left() + 12 * s, b.top() + 12 * s);
        path.lineTo(b.right() - 2 * s, b.bottom() - 2 * s);
        break;
    }
    case 2: {   // 分类：四个方块
        const qreal g = 2 * s;
        const qreal w = 7 * s;
        path.addRect(QRectF(b.left() + 2 * s, b.top() + 2 * s, w, w));
        path.addRect(QRectF(b.right() - 2 * s - w, b.top() + 2 * s, w, w));
        path.addRect(QRectF(b.left() + 2 * s, b.bottom() - 2 * s - w, w, w));
        path.addRect(QRectF(b.right() - 2 * s - w, b.bottom() - 2 * s - w, w, w));
        Q_UNUSED(g);
        break;
    }
    case 3: {   // 收藏：心形
        const QPointF c = b.center();
        path.moveTo(c.x(), b.bottom() - 2 * s);
        path.cubicTo(b.left() - 1 * s, c.y() - 1 * s, b.left() + 4 * s, b.top() + 1 * s,
                     c.x(), b.top() + 6 * s);
        path.cubicTo(b.right() - 4 * s, b.top() + 1 * s, b.right() + 1 * s, c.y() - 1 * s,
                     c.x(), b.bottom() - 2 * s);
        break;
    }
    default: {   // 设置：圆 + 八根齿
        path.addEllipse(QRectF(b.left() + 6 * s, b.top() + 6 * s, 8 * s, 8 * s));
        const QPointF c = b.center();
        for (int i = 0; i < 8; ++i) {
            const qreal ang = i * M_PI / 4.0;
            path.moveTo(c.x() + qCos(ang) * 6 * s, c.y() + qSin(ang) * 6 * s);
            path.lineTo(c.x() + qCos(ang) * 9 * s, c.y() + qSin(ang) * 9 * s);
        }
        break;
    }
    }
    return path;
}

void NavRail::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), kBgAlt);
    p.setPen(QPen(kBorder, 1));
    p.drawLine(width() - 1, 0, width() - 1, height());

    QFont f = font();
    f.setPixelSize(10);
    p.setFont(f);

    for (int i = 0; i < items_.size(); ++i) {
        const QRect r = ItemRect(i);
        const bool active = (i == cur_);
        QColor c = active ? kAccent : ((i == hover_) ? kText : kTextFaint);
        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::NoBrush);
        const QRectF box(r.center().x() - 10, r.top() + 11, 20, 20);
        p.drawPath(GlyphPath(i, box));

        p.setPen(active ? kAccent : kTextDim);
        p.drawText(QRect(r.left(), r.top() + 36, r.width(), 16), Qt::AlignHCenter | Qt::AlignTop,
                   items_.at(i));
    }

    // 选中条：cur 与 neighbor 之间按 progress 插值（模块切换时它跟着页面一起走）
    const int n = qMax(1, items_.size());
    const int dir = progress_ < 0 ? 1 : -1;
    const int glow = qBound(0, cur_ + dir, n - 1);
    qreal y = ItemRect(cur_).center().y();
    if (progress_ != 0.0 && glow != cur_) {
        const qreal y2 = ItemRect(glow).center().y();
        y += (y2 - y) * qAbs(progress_);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(kAccent);
    p.drawRect(QRectF(0, y - 12, 3, 24));
}

void NavRail::mouseReleaseEvent(QMouseEvent *e)
{
    const int idx = IndexAt(int(e->position().y()));
    if (idx >= 0)
        emit Activated(idx);
    QWidget::mouseReleaseEvent(e);
}

void NavRail::mouseMoveEvent(QMouseEvent *e)
{
    const int idx = IndexAt(int(e->position().y()));
    if (idx != hover_) {
        hover_ = idx;
        update();
    }
    QWidget::mouseMoveEvent(e);
}

void NavRail::leaveEvent(QEvent *e)
{
    hover_ = -1;
    update();
    QWidget::leaveEvent(e);
}

// ============================================================ DemoCard

DemoCard::DemoCard(const QString &code, const QString &title, int hue, Variant variant,
                   QWidget *parent)
    : QWidget(parent), code_(code), title_(title), hue_(hue), variant_(variant)
{
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
}

QSize DemoCard::sizeHint() const
{
    return variant_ == Wide ? QSize(150, 188) : QSize(102, 146);
}

void DemoCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRect r = rect().adjusted(0, 0, -1, -1);

    p.fillRect(rect(), hover_ && !down_ ? kSurfaceHover : kSurface);

    const int coverH = variant_ == Wide ? height() - 54 : height() - 40;
    QLinearGradient g(0, 0, width(), coverH);
    QColor c1 = QColor::fromHsl(hue_ % 360, 96, 76);
    QColor c2 = QColor::fromHsl((hue_ + 26) % 360, 110, 52);
    g.setColorAt(0.0, c1);
    g.setColorAt(1.0, c2);
    p.fillRect(QRect(0, 0, width(), coverH), g);

    // 斜纹：让"整块在动"比纯色更容易看出来
    p.setPen(QPen(QColor(255, 255, 255, 16), 1));
    for (int x = -coverH; x < width(); x += 14)
        p.drawLine(x, coverH, x + coverH, 0);

    // 编号
    QFont f = font();
    f.setPixelSize(10);
    p.setFont(f);
    const QRect chip(8, 8, p.fontMetrics().horizontalAdvance(code_) + 12, 18);
    p.fillRect(chip, QColor(0, 0, 0, 110));
    p.setPen(QColor(255, 255, 255, 220));
    p.drawText(chip, Qt::AlignCenter, code_);

    // 标题
    f.setPixelSize(variant_ == Wide ? 12 : 11);
    f.setBold(false);
    p.setFont(f);
    p.setPen(kText);
    const QRect titleRect(9, coverH + 8, width() - 18, 18);
    const QString elided = p.fontMetrics().elidedText(title_, Qt::ElideRight, titleRect.width());
    p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, elided);

    f.setPixelSize(10);
    p.setFont(f);
    p.setPen(kTextFaint);
    p.drawText(QRect(9, coverH + (variant_ == Wide ? 26 : 24), width() - 18, 14),
               Qt::AlignLeft | Qt::AlignVCenter,
               variant_ == Wide ? QStringLiteral("全 24 话 · 昨日更新") : QStringLiteral("24 话"));

    p.setPen(QPen(hover_ ? kAccent : kBorder, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
}

void DemoCard::enterEvent(QEnterEvent *e)
{
    hover_ = true;
    update();
    QWidget::enterEvent(e);
}

void DemoCard::leaveEvent(QEvent *e)
{
    hover_ = false;
    down_ = false;
    update();
    QWidget::leaveEvent(e);
}

void DemoCard::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        down_ = true;
        update();
    }
    QWidget::mousePressEvent(e);
}

void DemoCard::mouseReleaseEvent(QMouseEvent *e)
{
    const bool inside = rect().contains(e->position().toPoint());
    if (down_ && inside)
        emit Activated(title_);
    down_ = false;
    update();
    QWidget::mouseReleaseEvent(e);
}

// ============================================================ ToggleSwitch

ToggleSwitch::ToggleSwitch(bool on, QWidget *parent) : QWidget(parent), on_(on), t_(on ? 1.0 : 0.0)
{
    setCursor(Qt::PointingHandCursor);
}

void ToggleSwitch::SetOn(bool on, bool animate)
{
    if (on_ == on)
        return;
    on_ = on;
    if (!animate) {
        t_ = on ? 1.0 : 0.0;
        update();
        return;
    }
    if (anim_)
        anim_->stop();
    auto *a = new QVariantAnimation(this);
    a->setStartValue(t_);
    a->setEndValue(on ? 1.0 : 0.0);
    a->setDuration(120);   // 微反馈，对齐 Motion::Fast
    a->setEasingCurve(QEasingCurve::OutCubic);
    connect(a, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        t_ = v.toReal();
        update();
    });
    connect(a, &QVariantAnimation::finished, this, [this] { anim_ = nullptr; });
    anim_ = a;
    a->start(QAbstractAnimation::DeleteWhenStopped);
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF track(0, (height() - 22) / 2.0, width(), 22);
    QColor fill = kSurfaceHover;
    if (t_ > 0.0) {
        fill = QColor(int(kSurfaceHover.red() + (kAccent.red() - kSurfaceHover.red()) * t_),
                      int(kSurfaceHover.green() + (kAccent.green() - kSurfaceHover.green()) * t_),
                      int(kSurfaceHover.blue() + (kAccent.blue() - kSurfaceHover.blue()) * t_));
    }
    p.setPen(QPen(kBorder, 1));
    p.setBrush(fill);
    p.drawRoundedRect(track, 3, 3);

    const qreal x = 3 + t_ * (width() - 3 - 16 - 3);
    p.setPen(Qt::NoPen);
    p.setBrush(on_ ? QColor(0x0f, 0x14, 0x16) : QColor(0xd8, 0xdd, 0xe1));
    p.drawRect(QRectF(x, track.top() + 3, 16, 16));
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton) {
        SetOn(!on_);
        emit Toggled(on_);
    }
    QWidget::mouseReleaseEvent(e);
}
