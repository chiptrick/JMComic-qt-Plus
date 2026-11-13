#include "ui_kit.h"

#include <QApplication>
#include <QAbstractItemView>
#include <QCheckBox>
#include <QEasingCurve>
#include <QEvent>
#include <QEventLoop>
#include <QKeyEvent>
#include <QEnterEvent>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QtMath>

#include "cover_cache.h"
#include "motion.h"
#include "theme.h"
#include "touch.h"

// ============================================================ 图标
namespace {

QPainterPath PathFor(Glyph::Name n)
{
    QPainterPath p;
    switch (n) {
    case Glyph::Home:
        p.moveTo(4, 11); p.lineTo(12, 4); p.lineTo(20, 11);
        p.moveTo(6.5, 9.6); p.lineTo(6.5, 20); p.lineTo(17.5, 20); p.lineTo(17.5, 9.6);
        break;
    case Glyph::Grid:
        // 直角风格：图标里的"格子"也去掉圆角（真正的圆仍是圆形：放大镜/时钟/太阳）
        p.addRect(QRectF(4, 4, 7, 7));
        p.addRect(QRectF(13, 4, 7, 7));
        p.addRect(QRectF(4, 13, 7, 7));
        p.addRect(QRectF(13, 13, 7, 7));
        break;
    case Glyph::Search:
        p.addEllipse(QPointF(11, 11), 6.2, 6.2);
        p.moveTo(15.6, 15.6); p.lineTo(20, 20);
        break;
    case Glyph::Heart:
        p.moveTo(12, 20);
        p.cubicTo(4, 14.5, 3, 9.5, 5.6, 6.6);
        p.cubicTo(8, 4, 11, 5, 12, 7.4);
        p.cubicTo(13, 5, 16, 4, 18.4, 6.6);
        p.cubicTo(21, 9.5, 20, 14.5, 12, 20);
        break;
    case Glyph::Download:
        p.moveTo(12, 3.5); p.lineTo(12, 15);
        p.moveTo(7.2, 10.4); p.lineTo(12, 15.4); p.lineTo(16.8, 10.4);
        p.moveTo(4, 19.5); p.lineTo(20, 19.5);
        break;
    case Glyph::Folder:
        p.moveTo(3, 6.5); p.lineTo(10, 6.5); p.lineTo(11.8, 8.8); p.lineTo(21, 8.8);
        p.lineTo(21, 19); p.lineTo(3, 19); p.closeSubpath();
        break;
    case Glyph::Clock:
        p.addEllipse(QPointF(12, 12), 8.2, 8.2);
        p.moveTo(12, 7.4); p.lineTo(12, 12.2); p.lineTo(15.6, 14);
        break;
    case Glyph::Comment:
        p.addRect(QRectF(3.2, 4.2, 17.6, 12.8));
        p.moveTo(8, 17); p.lineTo(8, 20.6); p.lineTo(12.2, 17);
        break;
    case Glyph::Sparkle:
        p.moveTo(12, 3);
        p.cubicTo(12.8, 8.4, 15.6, 11.2, 21, 12);
        p.cubicTo(15.6, 12.8, 12.8, 15.6, 12, 21);
        p.cubicTo(11.2, 15.6, 8.4, 12.8, 3, 12);
        p.cubicTo(8.4, 11.2, 11.2, 8.4, 12, 3);
        p.closeSubpath();
        break;
    case Glyph::Gear:
        p.addEllipse(QPointF(12, 12), 3.4, 3.4);
        p.addEllipse(QPointF(12, 12), 7.4, 7.4);
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4.0;
            p.moveTo(12 + qCos(a) * 7.4, 12 + qSin(a) * 7.4);
            p.lineTo(12 + qCos(a) * 9.6, 12 + qSin(a) * 9.6);
        }
        break;
    case Glyph::Info:
        p.addEllipse(QPointF(12, 12), 8.4, 8.4);
        p.moveTo(12, 10.8); p.lineTo(12, 16.4);
        p.addEllipse(QPointF(12, 7.9), 0.7, 0.7);
        break;
    case Glyph::User:
        p.addEllipse(QPointF(12, 8.4), 3.8, 3.8);
        p.moveTo(4.8, 20);
        p.cubicTo(5.6, 15.4, 8.4, 13.6, 12, 13.6);
        p.cubicTo(15.6, 13.6, 18.4, 15.4, 19.2, 20);
        break;
    case Glyph::Back:
        p.moveTo(14.5, 5); p.lineTo(7.5, 12); p.lineTo(14.5, 19);
        break;
    case Glyph::Play:
        p.moveTo(8.5, 5.5); p.lineTo(19, 12); p.lineTo(8.5, 18.5); p.closeSubpath();
        break;
    case Glyph::Plus:
        p.moveTo(12, 5); p.lineTo(12, 19);
        p.moveTo(5, 12); p.lineTo(19, 12);
        break;
    case Glyph::Check:
        p.moveTo(4.5, 12.5); p.lineTo(9.6, 17.6); p.lineTo(19.5, 6.8);
        break;
    case Glyph::Close:
        p.moveTo(6, 6); p.lineTo(18, 18);
        p.moveTo(18, 6); p.lineTo(6, 18);
        break;
    case Glyph::Minimize:
        p.moveTo(5, 12); p.lineTo(19, 12);
        break;
    case Glyph::Maximize:
        p.addRect(QRectF(5.5, 5.5, 13, 13));
        break;
    case Glyph::Refresh:
        p.arcMoveTo(QRectF(4.5, 4.5, 15, 15), 60);
        p.arcTo(QRectF(4.5, 4.5, 15, 15), 60, 300);
        p.moveTo(18.2, 4.4); p.lineTo(18.2, 9.2); p.lineTo(13.4, 9.2);
        break;
    case Glyph::Trash:
        p.moveTo(4.5, 6.5); p.lineTo(19.5, 6.5);
        p.moveTo(9.4, 6.5); p.lineTo(9.4, 4.2); p.lineTo(14.6, 4.2); p.lineTo(14.6, 6.5);
        p.moveTo(6.6, 6.5); p.lineTo(7.6, 20); p.lineTo(16.4, 20); p.lineTo(17.4, 6.5);
        break;
    case Glyph::Pause:
        p.addRect(QRectF(7.4, 5.2, 3.4, 13.6));
        p.addRect(QRectF(13.2, 5.2, 3.4, 13.6));
        break;
    case Glyph::Sun:
        p.addEllipse(QPointF(12, 12), 4.2, 4.2);
        for (int i = 0; i < 8; ++i) {
            const qreal a = i * M_PI / 4.0;
            p.moveTo(12 + qCos(a) * 6.4, 12 + qSin(a) * 6.4);
            p.lineTo(12 + qCos(a) * 9, 12 + qSin(a) * 9);
        }
        break;
    case Glyph::Moon:
        p.moveTo(15.6, 3.8);
        p.cubicTo(9.2, 5.2, 7.4, 13.4, 12.4, 17.6);
        p.cubicTo(14.4, 19.2, 17.2, 19.4, 19.4, 17.8);
        p.cubicTo(12.8, 17.2, 10.2, 8.6, 15.6, 3.8);
        break;
    case Glyph::ChevronRight:
        p.moveTo(9.5, 5.5); p.lineTo(16, 12); p.lineTo(9.5, 18.5);
        break;
    case Glyph::Star:
        p.moveTo(12, 3.4);
        for (int i = 0; i < 5; ++i) {
            const qreal a = M_PI / 2 + i * 4 * M_PI / 5;
            p.lineTo(12 + qCos(a) * 8.5, 12 - qSin(a) * 8.5);
        }
        p.closeSubpath();
        break;
    case Glyph::Speed:
        p.arcMoveTo(QRectF(3.6, 6.2, 16.8, 16.8), 20);
        p.arcTo(QRectF(3.6, 6.2, 16.8, 16.8), 20, 140);
        p.moveTo(12, 15.6); p.lineTo(15.6, 10.4);
        break;
    case Glyph::Bookmark:
        p.moveTo(6.5, 4.5); p.lineTo(17.5, 4.5); p.lineTo(17.5, 20);
        p.lineTo(12, 16); p.lineTo(6.5, 20); p.closeSubpath();
        break;
    case Glyph::Calendar:
        // 每周必看：外框 + 两个挂耳 + 内部两条分隔线
        p.addRect(QRectF(3.6, 5.4, 16.8, 15));
        p.moveTo(3.6, 9.8); p.lineTo(20.4, 9.8);
        p.moveTo(8.2, 3.4); p.lineTo(8.2, 7);
        p.moveTo(15.8, 3.4); p.lineTo(15.8, 7);
        p.moveTo(8.2, 13.2); p.lineTo(8.2, 16.4);
        p.moveTo(12, 13.2); p.lineTo(12, 16.4);
        break;
    case Glyph::Server:
        // 网络存储：两层机架 + 指示灯
        p.addRect(QRectF(3.6, 4.6, 16.8, 6.2));
        p.addRect(QRectF(3.6, 13.2, 16.8, 6.2));
        p.moveTo(7, 7.7); p.lineTo(7.4, 7.7);
        p.moveTo(7, 16.3); p.lineTo(7.4, 16.3);
        break;
    case Glyph::Menu:
        p.moveTo(4, 7); p.lineTo(20, 7);
        p.moveTo(4, 12); p.lineTo(20, 12);
        p.moveTo(4, 17); p.lineTo(20, 17);
        break;
    case Glyph::Layers:
        // 批量超分：三层堆叠
        p.moveTo(12, 3.4); p.lineTo(20.4, 8); p.lineTo(12, 12.6); p.lineTo(3.6, 8);
        p.closeSubpath();
        p.moveTo(3.6, 12); p.lineTo(12, 16.6); p.lineTo(20.4, 12);
        p.moveTo(3.6, 15.8); p.lineTo(12, 20.4); p.lineTo(20.4, 15.8);
        break;
    }
    return p;
}

bool IsFilled(Glyph::Name n)
{
    return n == Glyph::Play || n == Glyph::Star || n == Glyph::Sparkle
        || n == Glyph::Pause || n == Glyph::Bookmark;
}

}  // namespace

namespace Glyph {

QPixmap Pixmap(Name n, int px, const QColor &color)
{
    const int ss = qMax(px, 8) * 2;
    QPixmap pm(ss, ss);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.scale(ss / 24.0, ss / 24.0);
    QPen pen(color, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    p.setPen(pen);
    const QPainterPath path = PathFor(n);
    if (IsFilled(n)) {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawPath(path);
    } else {
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }
    pm.setDevicePixelRatio(2.0);
    return pm;
}

QIcon Icon(Name n, int px, const QColor &color)
{
    return QIcon(Pixmap(n, px, color));
}

}  // namespace Glyph

// ============================================================ 封面
namespace CoverArt {

QPixmap Render(const QString &title, const QSize &size, int seed)
{
    const int w = qMax(40, size.width());
    const int h = qMax(40, size.height());
    QPixmap pm(w, h);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    const int hue = (qAbs(seed) * 47 + qAbs(qHash(title)) % 360) % 360;
    QColor a = QColor::fromHsv(hue, 58, 152);
    QColor b = QColor::fromHsv((hue + 26) % 360, 96, 74);
    QLinearGradient g(0, 0, w * 0.85, h);
    g.setColorAt(0.0, a);
    g.setColorAt(1.0, b);
    p.fillRect(QRect(0, 0, w, h), g);

    p.setPen(QPen(QColor(255, 255, 255, 10), 1.0));
    for (int x = -h; x < w; x += 16)
        p.drawLine(x, 0, x + h, h);

    QFont f = p.font();
    f.setPixelSize(int(h * 0.44));
    f.setWeight(QFont::DemiBold);
    p.setFont(f);
    p.setPen(QColor(255, 255, 255, 30));
    p.drawText(QRect(0, 0, w, h), Qt::AlignCenter, title.left(1));

    QLinearGradient scrim(0, h * 0.62, 0, h);
    scrim.setColorAt(0.0, QColor(0, 0, 0, 0));
    scrim.setColorAt(1.0, QColor(0, 0, 0, 120));
    p.fillRect(QRect(0, int(h * 0.6), w, h - int(h * 0.6)), scrim);
    return pm;
}

}  // namespace CoverArt

// ============================================================ FlowLayout
FlowLayout::FlowLayout(QWidget *parent, int spacing)
    : QLayout(parent), spacing_(spacing)
{
    setContentsMargins(0, 0, 0, 0);
}

FlowLayout::~FlowLayout()
{
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem *item) { items_.append(item); }
int FlowLayout::count() const { return items_.size(); }
QLayoutItem *FlowLayout::itemAt(int index) const
{
    return (index >= 0 && index < items_.size()) ? items_.at(index) : nullptr;
}
QLayoutItem *FlowLayout::takeAt(int index)
{
    if (index < 0 || index >= items_.size())
        return nullptr;
    return items_.takeAt(index);
}
Qt::Orientations FlowLayout::expandingDirections() const { return {}; }
bool FlowLayout::hasHeightForWidth() const { return true; }

int FlowLayout::heightForWidth(int width) const
{
    return DoLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect &rect)
{
    QLayout::setGeometry(rect);
    DoLayout(rect, false);
}

QSize FlowLayout::sizeHint() const { return minimumSize(); }

QSize FlowLayout::minimumSize() const
{
    QSize size;
    for (QLayoutItem *item : items_)
        size = size.expandedTo(item->minimumSize());
    return size;
}

int FlowLayout::DoLayout(const QRect &rect, bool testOnly) const
{
    int x = rect.x();
    int y = rect.y();
    int lineHeight = 0;
    const QRect eff = rect.adjusted(contentsMargins().left(), contentsMargins().top(),
                                    -contentsMargins().right(), -contentsMargins().bottom());
    for (QLayoutItem *item : items_) {
        const QSize hint = item->sizeHint();
        int nextX = x + hint.width() + spacing_;
        if (nextX - spacing_ > eff.right() + 1 && lineHeight > 0) {
            x = eff.x();
            y = y + lineHeight + spacing_;
            nextX = x + hint.width() + spacing_;
            lineHeight = 0;
        }
        if (!testOnly)
            item->setGeometry(QRect(QPoint(x, y), hint));
        x = nextX;
        lineHeight = qMax(lineHeight, hint.height());
    }
    return y + lineHeight - rect.y() + contentsMargins().bottom();
}

// ============================================================ ToggleSwitch
ToggleSwitch::ToggleSwitch(QWidget *parent) : QAbstractButton(parent)
{
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    // 44x24 在手机上太窄（手指命中区建议 44dp 见方）：轨道视觉尺寸不变，
    // 但控件本身给到 52x32，配合整行可点，实际命中区远大于原来的 24px 高。
    setFixedSize(52, 32);
    // 别让 Touch::PolishHitTargets 把它拉成 52x44：轨道高度会从 25 变 37，
    // 看起来像一根竖条；命中区已经由 hitButton(整控件) + 整行可点覆盖。
    setProperty("hitGrow", false);
    // 开关按下后可能被判成"纵向滑动页面"，出涟漪会误导（以为已经切换了）；
    // 它的状态反馈由轨道滑块自己的 140ms 动画承担。
    setProperty("noRipple", true);
}

QSize ToggleSwitch::sizeHint() const { return QSize(52, 32); }

bool ToggleSwitch::hitButton(const QPoint &pos) const
{
    // 默认实现只判定"轨道"那一小块；这里整个控件都算命中
    return rect().contains(pos);
}

void ToggleSwitch::checkStateSet()
{
    const qreal target = isChecked() ? 1.0 : 0.0;
    if (!Motion::Enabled()) {
        // 关掉动效时直接到位（逻辑必须与有动画时完全一致）
        pos_ = target;
        update();
        return;
    }
    if (!anim_) {
        anim_ = new QVariantAnimation(this);
        // 时长/曲线取统一表：开关、涟漪、提示消失都是同一个"微反馈"档
        anim_->setDuration(Motion::Fast);
        anim_->setEasingCurve(Motion::EnterCurve());
        connect(anim_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            pos_ = v.toReal();
            update();
        });
    }
    anim_->stop();
    anim_->setStartValue(pos_);
    anim_->setEndValue(target);
    anim_->start();
}

void ToggleSwitch::nextCheckState()
{
    QAbstractButton::nextCheckState();
    checkStateSet();
}

void ToggleSwitch::mousePressEvent(QMouseEvent *e)
{
    pressPos_ = e->position().toPoint();
    dragged_ = false;
    scrolled_ = 0;
    QAbstractButton::mousePressEvent(e);
}

void ToggleSwitch::mouseMoveEvent(QMouseEvent *e)
{
    if (!Touch::IsTouchMode()) {
        QAbstractButton::mouseMoveEvent(e);
        return;
    }
    const QPoint d = e->position().toPoint() - pressPos_;
    if (!dragged_ && qAbs(d.y()) > Touch::Dp(8, this) && qAbs(d.y()) > qAbs(d.x()))
        dragged_ = true;
    if (!dragged_) {
        QAbstractButton::mouseMoveEvent(e);
        return;
    }
    // 自己滚外层页面：press 被按钮吃掉了，QScroller 看不到
    const int dy = d.y() - scrolled_;
    if (dy != 0) {
        for (QWidget *p = parentWidget(); p; p = p->parentWidget()) {
            if (auto *area = qobject_cast<QScrollArea *>(p)) {
                QScrollBar *sb = area->verticalScrollBar();
                sb->setValue(sb->value() - dy);
                scrolled_ = d.y();
                break;
            }
        }
    }
    e->accept();
}

void ToggleSwitch::mouseReleaseEvent(QMouseEvent *e)
{
    if (dragged_) {
        // 拖动结束：不切换（否则"想滚页面"会顺手把开关拨了）
        dragged_ = false;
        e->accept();
        return;
    }
    QAbstractButton::mouseReleaseEvent(e);
}

void ToggleSwitch::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const UiTokens &t = UiTheme::I().Tokens();
    // 轨道在 32 高的控件里居中，视觉厚度仍是 18（与原来 24 高时一致）
    const QRectF track(0.5, (height() - 18.0) / 2.0, width() - 1.0, 18.0);
    QColor bg = t.dark ? t.border : QColor(0xd0, 0xd4, 0xd8);
    if (isChecked())
        bg = t.accent;
    else if (underMouse())
        bg = t.dark ? t.surfaceHover : QColor(0xbf, 0xc4, 0xc9);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    // 直角矩形风格：胶囊轨道 → 直角轨道
    UiStyle::DrawPanel(&p, track);

    const qreal d = track.height() - 4.0;
    const qreal x = track.left() + 2.0 + pos_ * (track.width() - d - 4.0);
    // 开：深色主题用深色钮（压在青色轨道上对比强）；浅色主题统一白钮
    p.setBrush(isChecked() ? (t.dark ? QColor(0x1b, 0x1d, 0x21) : QColor(0xff, 0xff, 0xff))
                           : QColor(0xff, 0xff, 0xff));
    // 圆形滑块 → 方形滑块（直角风格不保留圆弧）
    p.drawRect(QRectF(x, track.top() + 2.0, d, d));
}

// ============================================================ TouchSlider
TouchSlider::TouchSlider(Qt::Orientation o, QWidget *parent) : QSlider(o, parent)
{
    setCursor(Qt::PointingHandCursor);
    if (orientation() == Qt::Horizontal)
        setMinimumHeight(32);
}

// 轨道像素 → 值。手柄中心能走的行程是 groove 宽度减掉手柄宽度，
// 这样"手指落在手柄中心"和"滑块画出来的位置"用的是同一套几何，不会在两端差半格。
int TouchSlider::ValueAt(int centerX) const
{
    if (maximum() == minimum())
        return value();
    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect groove =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
    const int span = qMax(1, groove.width() - handle.width());
    const int pos = qBound(0, centerX - (groove.left() + handle.width() / 2), span);
    return minimum() + qRound(qreal(pos) / span * (maximum() - minimum()));
}

// 竖滑交给外层滚动容器：按下被本控件接管后，QScroller 收不到这次手势，只能自己补。
void TouchSlider::ScrollOuter(int dy)
{
    for (QWidget *p = parentWidget(); p; p = p->parentWidget()) {
        if (auto *area = qobject_cast<QScrollArea *>(p)) {
            QScrollBar *sb = area->verticalScrollBar();
            sb->setValue(sb->value() - dy);
            return;
        }
    }
}

void TouchSlider::mousePressEvent(QMouseEvent *e)
{
    if (!Touch::IsTouchMode() || e->button() != Qt::LeftButton) {
        QSlider::mousePressEvent(e);
        return;
    }
    const QPoint p = e->position().toPoint();
    if (!rect().contains(p)) {
        e->ignore();
        return;
    }
    // 接管按下：不接管就收不到后续 move，"按在轨道上拖不动"就是这么来的。
    // 但这里**不改值**——先等方向判明，竖滑要留给页面滚动。
    pressed_ = true;
    decided_ = false;
    vertical_ = false;
    dragging_ = false;
    pressPos_ = p;
    lastY_ = p.y();

    QStyleOptionSlider opt;
    initStyleOption(&opt);
    const QRect handle =
        style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
    grabDx_ = p.x() - handle.center().x();   // 按在轨道上时为负/正偏移，横拖时保持
    e->accept();
}

void TouchSlider::mouseMoveEvent(QMouseEvent *e)
{
    if (!Touch::IsTouchMode() || !pressed_) {
        QSlider::mouseMoveEvent(e);
        return;
    }
    const QPoint p = e->position().toPoint();
    if (!decided_) {
        const QPoint d = p - pressPos_;
        const int slop = Touch::Dp(6, this);
        if (qAbs(d.x()) < slop && qAbs(d.y()) < slop) {
            e->accept();
            return;   // 还在死区：什么都不做，避免手抖就改值
        }
        decided_ = true;
        vertical_ = qAbs(d.y()) > qAbs(d.x());
        lastY_ = pressPos_.y();
    }
    if (vertical_) {
        const int dy = p.y() - lastY_;
        if (dy != 0) {
            ScrollOuter(dy);
            lastY_ = p.y();
        }
        e->accept();
        return;
    }
    dragging_ = true;
    setValue(ValueAt(p.x() - grabDx_));
    e->accept();
}

void TouchSlider::mouseReleaseEvent(QMouseEvent *e)
{
    if (!Touch::IsTouchMode() || !pressed_) {
        QSlider::mouseReleaseEvent(e);
        return;
    }
    // 没拖动就松手（点在轨道上）不改值：保留"按轨道不跳值"的原意，防误触。
    pressed_ = false;
    decided_ = false;
    vertical_ = false;
    dragging_ = false;
    e->accept();
}

// ============================================================ TouchCombo
// 下拉框的滑动/点选判定统一由 Touch::TapGuard（touch.cpp）负责：
//   * 手指按在框上滑动 → 不弹窗、不选中，外层页面照常滚；
//   * 弹窗列表内滑动 → 只滚列表，不提交、不改 currentIndex；
//   * 弹窗列表内点选 → 正常提交并关闭。
// 这里只保留"触摸端指针样式"，不再各写一套事件处理：两套实现并存时，
// 位移会同时被两边消费（滚动速度翻倍），判定阈值也容易走偏。
TouchCombo::TouchCombo(QWidget *parent) : QComboBox(parent)
{
    setCursor(Qt::PointingHandCursor);
}

// ============================================================ ElideLabel
ElideLabel::ElideLabel(const QString &text, QWidget *parent) : QLabel(parent)
{
    // Preferred + 自定义 minimumSizeHint：空间够时按文字自然宽度占位（标题紧挨页码），
    // 空间不够时可以被压到 0（否则 QLabel 的最小宽 = 整行文字宽，窄窗口下会把
    // 兄弟控件挤出可视区）。
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    setMinimumWidth(0);
    setWordWrap(false);
    SetFullText(text);
}

QSize ElideLabel::minimumSizeHint() const
{
    QSize s = QLabel::minimumSizeHint();
    s.setWidth(0);
    return s;
}

QSize ElideLabel::sizeHint() const
{
    const QFontMetrics fm(font());
    const QMargins m = contentsMargins();
    return QSize(fm.horizontalAdvance(full_) + m.left() + m.right(),
                 QLabel::sizeHint().height());
}

void ElideLabel::SetFullText(const QString &text)
{
    full_ = text;
    // 完整文本进 tooltip：桌面端悬停可看全，触摸端不影响排版
    setToolTip(text);
    UpdateElide();
}

void ElideLabel::resizeEvent(QResizeEvent *event)
{
    QLabel::resizeEvent(event);
    UpdateElide();
}

void ElideLabel::changeEvent(QEvent *event)
{
    QLabel::changeEvent(event);
    // 字号/字体变化后省略位置必须重算，否则会出现"字变大但省略号跟不上"
    if (event->type() == QEvent::FontChange)
        UpdateElide();
}

void ElideLabel::UpdateElide()
{
    const int avail = contentsRect().width();
    if (avail <= 0 || full_.isEmpty()) {
        QLabel::setText(full_);
        return;
    }
    // 只在真的放不下时省略：elidedText 在放得下时原样返回
    QLabel::setText(fontMetrics().elidedText(full_, Qt::ElideRight, avail));
}

// ============================================================ Chip
Chip::Chip(const QString &text, bool checkable, QWidget *parent) : QPushButton(text, parent)
{
    setObjectName(QStringLiteral("Chip"));
    setCheckable(checkable);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
}

// ============================================================ SearchBox
SearchBox::SearchBox(const QString &placeholder, QWidget *parent) : QWidget(parent)
{
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(10, 0, 6, 0);
    lay->setSpacing(8);

    auto *icon = new QLabel(this);
    icon->setPixmap(Glyph::Pixmap(Glyph::Search, 16, UiTheme::I().Tokens().textFaint));
    icon->setFixedSize(16, 16);
    lay->addWidget(icon);

    edit_ = new QLineEdit(this);
    edit_->setObjectName(QStringLiteral("SearchBox"));
    edit_->setPlaceholderText(placeholder);
    edit_->setFrame(false);
    edit_->setClearButtonEnabled(false);
    lay->addWidget(edit_, 1);

    auto *clear = new QToolButton(this);
    clear->setObjectName(QStringLiteral("WinBtn"));
    clear->setIcon(Glyph::Icon(Glyph::Close, 12, UiTheme::I().Tokens().textFaint));
    clear->setIconSize(QSize(12, 12));
    clear->setFixedSize(22, 22);
    clear->setCursor(Qt::PointingHandCursor);
    clear->hide();
    lay->addWidget(clear);

    setStyleSheet(QStringLiteral("QWidget { background: transparent; }"));

    connect(edit_, &QLineEdit::textChanged, this, [this, clear](const QString &txt) {
        clear->setVisible(!txt.isEmpty());
        emit Changed(txt);
    });
    connect(edit_, &QLineEdit::returnPressed, this, [this] { emit Submitted(edit_->text()); });
    connect(clear, &QToolButton::clicked, this, [this] {
        edit_->clear();
        emit Submitted(QString());
    });
}

QString SearchBox::Text() const { return edit_ ? edit_->text() : QString(); }
void SearchBox::SetText(const QString &text) { edit_->setText(text); }

// ============================================================ SectionHeader
SectionHeader::SectionHeader(const QString &title, const QString &hint,
                             const QString &action, QWidget *parent)
    : QWidget(parent)
{
    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(10);

    auto *t = new QLabel(title, this);
    t->setObjectName(QStringLiteral("SectionTitle"));
    t->setWordWrap(true);
    lay->addWidget(t, 0);

    if (!hint.isEmpty()) {
        auto *h = new QLabel(hint, this);
        h->setObjectName(QStringLiteral("SectionHint"));
        // 区块说明经常很长（字体组那条 30+ 字）。不换行时 QLabel 的
        // minimumSizeHint 会把整页撑到 780px —— 手机上就是"页面能左右滑"的根因。
        // 换行 + 吃掉剩余宽度即可；**不能**用 Ignored（会把文字压成一列单字）。
        h->setWordWrap(true);
        h->setMinimumWidth(0);
        lay->addWidget(h, 1);
    } else {
        lay->addStretch(1);
    }

    if (!action.isEmpty()) {
        action_ = new QPushButton(action, this);
        action_->setObjectName(QStringLiteral("Link"));
        action_->setCursor(Qt::PointingHandCursor);
        // 文字链不是按钮块：触摸模式下的 44dp 命中下限（Touch::PolishHitTargets）
        // 会把整行标题撑到 44px 高，标题与下方内容之间就多出两块空白。
        // 这里退出"命中区放大"，高度按字体收。
        action_->setProperty("hitGrow", false);
        action_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        lay->addWidget(action_, 0, Qt::AlignVCenter);
        action_->ensurePolished();
        // 高度跟标题文字走：标题 14px 的行高 + 2px。不能用 UiStyle::Fpx()
        // （它有 8px 下限），也不能用按钮自己的字体度量（未套 QSS 时量到的是
        // 应用默认 9pt，读出来 26px），否则整行会被文字链撑到 34px。
        const int textH = QFontMetrics(t->font()).height();
        action_->setFixedHeight(textH + qMax(2, qRound(2 * UiStyle::FontScale())));
    }
}

// ============================================================ SettingRow
SettingRow::SettingRow(const QString &title, const QString &desc,
                       QWidget *control, QWidget *parent)
    : QWidget(parent)
{
    outer_ = new QHBoxLayout(this);
    QHBoxLayout *outer = outer_;
    outer->setContentsMargins(0, 8, 0, 8);
    outer->setSpacing(16);

    auto *texts = new QVBoxLayout();
    texts->setSpacing(2);
    auto *t = new QLabel(title, this);
    t->setObjectName(QStringLiteral("SettingTitle"));
    t->setWordWrap(true);
    texts->addWidget(t);
    if (!desc.isEmpty()) {
        auto *d = new QLabel(desc, this);
        d->setObjectName(QStringLiteral("SettingDesc"));
        d->setWordWrap(true);
        // 说明文字是换行文本：不给它"按最长行撑宽"的机会，否则窄屏上整行会被
        // 顶得比屏幕还宽（横向滚动范围 > 0 → 页面能左右滑）
        d->setMinimumWidth(0);
        // Ignored 让 minimumSizeHint 不再按文本宽度算（长路径/长单词也不会撑宽）
        texts->addWidget(d);
    }
    outer->addLayout(texts, 1);

    if (control) {
        control_ = control;
        control_->setParent(this);
        outer->addWidget(control_, 0, Qt::AlignRight | Qt::AlignVCenter);
    }
    setMinimumHeight(52);
    setAttribute(Qt::WA_Hover, true);
    setObjectName(QStringLiteral("SettingRow"));
    setProperty("toggleRow", IsClickableRow());
}

void SettingRow::SetControl(QWidget *control)
{
    if (control_) {
        control_->hide();
        control_->deleteLater();
    }
    control_ = control;
    if (!control_)
        return;
    control_->setParent(this);
    if (outer_)
        outer_->addWidget(control_, 0, Qt::AlignRight | Qt::AlignVCenter);
    control_->show();
    setProperty("toggleRow", IsClickableRow());
    style()->unpolish(this);
    style()->polish(this);
    ApplyDirection();
}

bool SettingRow::IsClickableRow() const
{
    // 开关/复选框这类"二元"控件，整行点都应该是安全的；滑块/下拉/按钮不行
    return qobject_cast<ToggleSwitch *>(control_) || qobject_cast<QCheckBox *>(control_);
}

void SettingRow::SetCompact(bool on)
{
    if (compact_ == on)
        return;
    compact_ = on;
    // 这行日志是"整页被拉长"类问题的定位锚点：横排/堆叠翻转只看宽度门槛，
    // 宽度、门槛、控件 hint 三个数一起打出来才能判断是谁把行挤窄了。
    qWarning("setting-row: compact=%d w=%d thr=%d controlHint=%d", int(on), width(),
             CompactThreshold(), control_ ? control_->sizeHint().width() : 0);
    ApplyDirection();
}

void SettingRow::ApplyDirection()
{
    if (!outer_)
        return;
    // 开关/复选框只有 52x32：它和一行文字放得下，没必要再占第二行。
    // 之前的"紧凑模式一律上下堆叠"把开关挤到说明文字下面，行高从 48 涨到 101，
    // 手机上要滚很久才能看完一屏。
    const bool sameLine = !compact_ || IsClickableRow();
    outer_->setDirection(sameLine ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    outer_->setSpacing(sameLine ? (compact_ ? 12 : 16) : 8);
    if (control_) {
        if (sameLine) {
            outer_->setAlignment(control_, Qt::AlignRight | Qt::AlignVCenter);
        } else {
            // 紧凑模式**不能**给控件设对齐标志：QBoxLayout 一旦见到对齐标志就只给
            // sizeHint 宽度，控件里的"路径 + 两个按钮"会被压到每个只剩一个字。
            // 不设标志 = 撑满整行；下拉框自身有 maximumWidth，仍然靠左。
            outer_->setAlignment(control_, Qt::Alignment());
        }
    }
    // 紧凑模式下行高由内容决定；同排的开关行给一个 44dp 的触摸下限
    setMinimumHeight(compact_ ? (sameLine ? UiStyle::Fpx(44) : 0) : 52);
    setProperty("compact", compact_);
    setProperty("inline", sameLine);
    style()->unpolish(this);
    style()->polish(this);
}

void SettingRow::resizeEvent(QResizeEvent *e)
{
    QWidget::resizeEvent(e);
    if (width() <= 0)
        return;
    // 横排/堆叠的判定必须用**最终宽度**。
    //
    // 造期与字体轴（字重 900）变化时，QSS 会整体重建、页面重新布局，同一行会先被
    // 摆成 268px 再回到 390px；在 268 时判成"堆叠"之后就再也不会翻回来 —— 因为
    // 最终 390px 落在旧迟滞带（382±12）的死区里，页面于是永久停在堆叠态，
    // 整页被拉长（用户报的"字重调最大页面被拉伸"）。
    // 这里统一延后到本轮布局结束、按稳定宽度判一次，且只用一个门槛、不再留死区：
    // 堆叠会让内容更高（滚动条出现、行更窄），横排则相反，反馈方向本身就是收敛的。
    if (dirCheckPending_)
        return;
    dirCheckPending_ = true;
    QTimer::singleShot(0, this, [this] {
        dirCheckPending_ = false;
        if (width() <= 0)
            return;
        const bool narrow = width() < CompactThreshold();
        if (narrow != compact_)
            SetCompact(narrow);
    });
}

// 切成上下堆叠的宽度门槛。
//
// 只用字号缩放这一个输入，**不掺控件/文本度量**：阈值一旦跟着 sizeHint /
// minimumSizeHint 走，就会随字重、字体家族、文案长度变化（实测两个组合框在
// 400 与 900 下相差 11px），结果是"拖字重时整行在横排/堆叠之间翻转"——
// 那正是"调字重整页被拉伸"的另一半原因。宽度门槛只回答"这行够不够宽"，
// 堆叠后的观感问题在滑块那一侧解决（见 MakeSliderField）。
int SettingRow::CompactThreshold() const
{
    return UiStyle::Fpx(430);
}

void SettingRow::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && IsClickableRow() && rect().contains(e->position().toPoint())) {
        // 点在控件自己身上时由控件处理，避免一次点击切换两次
        if (control_ && control_->geometry().contains(e->position().toPoint())) {
            QWidget::mouseReleaseEvent(e);
            return;
        }
        if (auto *btn = qobject_cast<QAbstractButton *>(control_)) {
            btn->click();   // click() 自带 toggle + clicked/animation
            e->accept();
            return;
        }
    }
    QWidget::mouseReleaseEvent(e);
}

// ============================================================ EmptyState
EmptyState::EmptyState(const QString &title, const QString &text,
                       const QString &action, QWidget *parent)
    : QWidget(parent)
{
    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(24, 40, 24, 40);
    lay->setSpacing(10);
    lay->setAlignment(Qt::AlignCenter);

    auto *icon = new QLabel(this);
    icon->setPixmap(Glyph::Pixmap(Glyph::Info, 40, UiTheme::I().Tokens().textFaint));
    icon->setAlignment(Qt::AlignCenter);
    lay->addWidget(icon, 0, Qt::AlignHCenter);

    auto *t = new QLabel(title, this);
    t->setObjectName(QStringLiteral("EmptyTitle"));
    t->setAlignment(Qt::AlignCenter);
    lay->addWidget(t);

    auto *d = new QLabel(text, this);
    d->setObjectName(QStringLiteral("EmptyText"));
    d->setAlignment(Qt::AlignCenter);
    d->setWordWrap(true);
    lay->addWidget(d);

    if (!action.isEmpty()) {
        action_ = new QPushButton(action, this);
        action_->setObjectName(QStringLiteral("Ghost"));
        action_->setCursor(Qt::PointingHandCursor);
        lay->addWidget(action_, 0, Qt::AlignHCenter);
    }
}

// ============================================================ Toast
Toast::Toast(QWidget *parent) : QLabel(parent)
{
    setObjectName(QStringLiteral("Toast"));
    setAlignment(Qt::AlignCenter);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    hide();
}

void Toast::Show(QWidget *parent, const QString &text, int ms)
{
    if (!parent || text.trimmed().isEmpty())
        return;
    // 同一时刻只留一条：连续操作时旧提示直接替换，避免文字叠在一起
    for (Toast *old : parent->findChildren<Toast *>())
        delete old;
    auto *toast = new Toast(parent);
    toast->setText(text);
    toast->adjustSize();
    const int x = parent->width() / 2 - toast->width() / 2;
    const int y = parent->height() - toast->height() - 32;
    toast->home_ = QPoint(qMax(12, x), qMax(12, y));
    // 从下方 10px 处淡入并落位：提示是"从底部冒出来"的，不是凭空出现
    toast->move(toast->home_ + QPoint(0, UiStyle::Fpx(10)));
    toast->show();
    toast->raise();
    if (Motion::Enabled()) {
        Motion::SlideTo(toast, toast->home_, Motion::Base, Motion::EnterCurve());
        Motion::FadeIn(toast, Motion::Base);
    } else {
        toast->move(toast->home_);
    }
    // 退场也算在总时长里，保证"显示 ms 毫秒"的总感观不变
    const int hold = qMax(400, ms - (Motion::Enabled() ? Motion::Fast + 20 : 0));
    QTimer::singleShot(hold, toast, &Toast::PopOut);
}

void Toast::PopOut()
{
    if (leaving_)
        return;
    leaving_ = true;
    if (!Motion::Enabled()) {
        deleteLater();
        return;
    }
    Motion::SlideTo(this, home_ + QPoint(0, UiStyle::Fpx(6)), Motion::Fast, Motion::ExitCurve());
    Motion::FadeOut(this, Motion::Fast, [this] { deleteLater(); });
}

// ============================================================ 加载条
LoadingBar::LoadingBar(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("LoadingBar"));
    setFixedHeight(UiStyle::Fpx(3));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    // 常驻（不 hide）：隐藏会让布局收回 3px，再次加载时整页内容往下跳一下。
    // 空闲时只画一条页面底色，肉眼等于不存在。
}

void LoadingBar::Start()
{
    if (running_)
        return;
    running_ = true;
    show();
    // 上一轮可能还在淡出：先停掉，否则 fade_ 会被它继续往下拉（亮线闪一下）
    Motion::Stop(this, QStringLiteral("fade"));
    fade_ = 1.0;
    if (!Motion::Enabled()) {
        // 动效关闭：只画一条静止的整宽亮线（状态还是"正在加载"，但不跑动画）
        update();
        return;
    }
    if (!loop_) {
        loop_ = new QVariantAnimation(this);
        loop_->setStartValue(0.0);
        loop_->setEndValue(1.0);
        loop_->setDuration(1100);
        loop_->setEasingCurve(QEasingCurve::Linear);
        loop_->setLoopCount(-1);
        connect(loop_, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
            phase_ = v.toReal();
            update();
        });
    }
    loop_->start();
    update();
}

void LoadingBar::Stop()
{
    if (!running_)
        return;
    running_ = false;
    if (!Motion::Enabled()) {
        if (loop_)
            loop_->stop();
        fade_ = 0.0;
        update();
        return;
    }
    // 数据到位后先淡出再停：直接停掉会留下一条"突然消失"的亮线
    Motion::Animate(this, QStringLiteral("fade"), fade_, 0.0, Motion::Fast, Motion::ExitCurve(),
                    [this](qreal v) {
                        fade_ = v;
                        update();
                    },
                    [this] {
                        if (loop_)
                            loop_->stop();
                        update();
                    });
}

void LoadingBar::paintEvent(QPaintEvent *)
{
    const UiTokens &t = UiTheme::I().Tokens();
    QPainter p(this);
    if (fade_ <= 0.001) {
        // 空闲：铺一层页面底色，避免这块 3px 区域保留上一帧的残影
        p.fillRect(rect(), t.bg);
        return;
    }
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(t.border.red(), t.border.green(), t.border.blue(),
                              int(70 * fade_)));

    // 30% 宽的亮段匀速扫过；两侧渐隐，避免出现"硬边小方块"。
    // 动效关闭时退化成一条静态整宽亮线。
    const qreal w = qMax<qreal>(1.0, width());
    const qreal bandW = Motion::Enabled() ? qMax<qreal>(48.0, w * 0.30) : w;
    const qreal x = Motion::Enabled() ? (-bandW + (w + bandW) * phase_) : 0.0;
    QLinearGradient g(x, 0, x + bandW, 0);
    QColor c = t.accent;
    QColor edge = t.accent;
    edge.setAlpha(0);
    c.setAlpha(int(235 * fade_));
    edge.setAlpha(0);
    g.setColorAt(0.0, edge);
    g.setColorAt(0.55, c);
    g.setColorAt(1.0, edge);
    p.fillRect(QRectF(x, 0, bandW, height()), g);
}

// ============================================================ 卡片绘制
namespace CardPaint {

QSize Size(int width)
{
    const int w = qMax(96, width);
    // 文字区高度跟着字号缩放，否则放大字号后卡片标题/作者会被裁掉
    return QSize(w, w * 4 / 3 + UiStyle::Fpx(40));
}

void Paint(QPainter *p, const QRect &rect, const Comic &c, bool hover, bool selected)
{
    const UiTokens &t = UiTheme::I().Tokens();
    const int w = rect.width();
    const int h = rect.height();
    const int coverH = w * 4 / 3;

    p->save();
    p->translate(rect.topLeft());
    p->setRenderHint(QPainter::Antialiasing, true);

    if (hover || selected) {
        QColor bg = t.surfaceHover;
        bg.setAlpha(hover ? 90 : 60);
        p->setPen(Qt::NoPen);
        p->setBrush(bg);
        UiStyle::DrawPanel(p, QRectF(0, 0, w, h));
    }

    const QRect coverRect(0, 0, w, coverH);
    const QPixmap cover = CoverCache::I().Get(c.id, c.title, c.coverUrl, QSize(w, coverH));

    // Radius=0（直角风格）时 addRect 的 setClipPath 是纯开销；将来若开圆角
    // 风格，应在缓存层预烘焙圆角位图（参考 ui_pages_browse.cpp 的
    // RoundedPixmap），不要把路径裁剪放进滚动热路径。
    if constexpr (UiStyle::Radius > 0) {
        QPainterPath clip;
        UiStyle::AddPanel(clip, QRectF(coverRect));
        p->save();
        p->setClipPath(clip);
        p->drawPixmap(coverRect, cover);
        p->restore();
    } else {
        p->drawPixmap(coverRect, cover);
    }

    p->setPen(QPen(hover ? t.accent : QColor(255, 255, 255, 18), 1.0));
    p->setBrush(Qt::NoBrush);
    UiStyle::DrawPanel(p, QRectF(coverRect).adjusted(0.5, 0.5, -0.5, -0.5));

    // 角标
    const QString badge = c.status == QStringLiteral("已完结") ? QStringLiteral("完结")
                                                               : QStringLiteral("更新");
    QFont bf = p->font();
    bf.setPixelSize(UiStyle::Fpx(10));
    p->setFont(bf);
    const QFontMetrics bfm(bf);
    const int bw = bfm.horizontalAdvance(badge) + 12;
    const QRectF badgeRect(coverRect.right() - bw - 8, 8, bw, UiStyle::Fpx(18));
    p->setPen(Qt::NoPen);
    p->setBrush(QColor(0, 0, 0, 150));
    UiStyle::DrawPanel(p, badgeRect);
    p->setPen(c.status == QStringLiteral("已完结") ? t.textDim : t.accent);
    p->drawText(badgeRect, Qt::AlignCenter, badge);

    // 阅读进度
    if (c.chapters > 0 && c.readChapter > 0) {
        const qreal ratio = qBound(0.0, qreal(c.readChapter) / qreal(c.chapters), 1.0);
        p->fillRect(QRectF(0, coverH - 3, w, 3), QColor(0, 0, 0, 110));
        p->fillRect(QRectF(0, coverH - 3, w * ratio, 3), t.accent);
    }

    QFont tf = p->font();
    tf.setPixelSize(UiStyle::Fpx(13));
    tf.setWeight(QFont::Medium);
    p->setFont(tf);
    p->setPen(hover ? t.accent : t.text);
    const QFontMetrics tfm(tf);
    p->drawText(QRect(1, coverH + UiStyle::Fpx(8), w - 2, UiStyle::Fpx(18)),
                Qt::AlignLeft | Qt::AlignVCenter,
                tfm.elidedText(c.title, Qt::ElideRight, w - 2));

    QFont mf = p->font();
    mf.setPixelSize(UiStyle::Fpx(11));
    p->setFont(mf);
    p->setPen(t.textDim);
    const QFontMetrics mfm(mf);
    // 与 QML 的 model.metaText 同一份文案（models.h: ComicMetaText）：
    // 章节数未知时只画作者，不能画成"作者 · 0 话"。
    const QString meta = ComicMetaText(c);
    p->drawText(QRect(1, coverH + UiStyle::Fpx(26), w - 2, UiStyle::Fpx(14)),
                Qt::AlignLeft | Qt::AlignVCenter,
                mfm.elidedText(meta, Qt::ElideRight, w - 2));

    p->restore();
}

}  // namespace CardPaint

// ============================================================ 模态框
ModalDialog::ModalDialog(QWidget *host, const QString &title, const QString &text,
                         const QString &okText, const QString &cancelText, Kind kind,
                         bool confirm, const QString *initialValue)
    : QWidget(host)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setGeometry(host->rect());
    setFocusPolicy(Qt::StrongFocus);
    host->installEventFilter(this);

    card_ = new QWidget(this);
    card_->setObjectName(QStringLiteral("DialogCard"));
    card_->setFixedWidth(420);

    auto *lay = new QVBoxLayout(card_);
    lay->setContentsMargins(22, 20, 22, 18);
    lay->setSpacing(10);

    auto *t = new QLabel(title, card_);
    t->setObjectName(QStringLiteral("DialogTitle"));
    t->setWordWrap(true);
    lay->addWidget(t);

    auto *d = new QLabel(text, card_);
    d->setObjectName(QStringLiteral("DialogText"));
    d->setWordWrap(true);
    d->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lay->addWidget(d);

    if (initialValue) {
        edit_ = new QLineEdit(card_);
        edit_->setObjectName(QStringLiteral("DialogInput"));
        edit_->setText(*initialValue);
        edit_->setFixedHeight(UiStyle::Fpx(34));
        lay->addWidget(edit_);
    }

    auto *btns = new QHBoxLayout();
    btns->setSpacing(10);
    btns->addStretch(1);

    if (confirm) {
        auto *cancel = new QPushButton(cancelText, card_);
        cancel->setObjectName(QStringLiteral("Ghost"));
        // 确认框的「取消 / 确定」是一组：两个都走近似直角（3px），
        // 避免同一行出现一个圆角一个直角（危险操作的"删除/清空"也跟随这一组）。
        UiStyle::MarkSoftButton(cancel);
        cancel->setCursor(Qt::PointingHandCursor);
        btns->addWidget(cancel);
        connect(cancel, &QPushButton::clicked, this, [this] { Finish(false); });
    }

    auto *ok = new QPushButton(okText, card_);
    ok->setObjectName(kind == Danger ? QStringLiteral("DangerBtn") : QStringLiteral("Primary"));
    UiStyle::MarkSoftButton(ok);
    ok->setCursor(Qt::PointingHandCursor);
    ok->setDefault(true);
    btns->addWidget(ok);
    connect(ok, &QPushButton::clicked, this, [this] { Finish(true); });
    lay->addLayout(btns);

    // 居中：构造期就算好，进场动画才有确定的落点（原来放在 singleShot(0) 里，
    // 动画会先看到卡片在左上角，再被"搬"到中间）
    CenterCard();

    if (confirm)
        loop_ = new QEventLoop(this);

    // Message：点击确定或 2.5 秒后自动关闭，避免挡住后续操作
    if (!confirm)
        QTimer::singleShot(2500, this, [this] { Finish(true); });
}

void ModalDialog::Show(QWidget *parent, const QString &title, const QString &text,
                       const QString &okText, const QString &cancelText, Kind kind)
{
    if (!parent)
        return;
    auto *dlg = new ModalDialog(parent->window(), title, text, okText, cancelText, kind, true);
    dlg->loop_ = nullptr;   // 不阻塞：按钮只负责关闭
    dlg->show();
    dlg->raise();
    dlg->setFocus();
}

QString ModalDialog::Prompt(QWidget *parent, const QString &title, const QString &text,
                            const QString &initial)
{
    if (!parent)
        return QString();
    auto *dlg = new ModalDialog(parent->window(), title, text, QStringLiteral("确定"),
                                QStringLiteral("取消"), Info, true, &initial);
    dlg->show();
    dlg->raise();
    dlg->setFocus();
    if (dlg->edit_) {
        dlg->edit_->setFocus();
        dlg->edit_->selectAll();
    }
    dlg->loop_->exec();
    const bool ok = dlg->accepted_;
    const QString out = (ok && dlg->edit_) ? dlg->edit_->text() : QString();
    dlg->deleteLater();
    return out;
}

bool ModalDialog::Confirm(QWidget *parent, const QString &title, const QString &text,
                          const QString &okText, const QString &cancelText, Kind kind)
{
    if (!parent)
        return false;
    auto *dlg = new ModalDialog(parent->window(), title, text, okText, cancelText, kind, true);
    dlg->show();
    dlg->raise();
    dlg->setFocus();
    dlg->loop_->exec();
    const bool ok = dlg->accepted_;
    dlg->deleteLater();
    return ok;
}

void ModalDialog::Message(QWidget *parent, const QString &title, const QString &text, Kind kind)
{
    if (!parent)
        return;
    auto *dlg = new ModalDialog(parent->window(), title, text, QStringLiteral("知道了"),
                                QString(), kind, false);
    dlg->show();
    dlg->raise();
}

void ModalDialog::CenterCard()
{
    if (!card_)
        return;
    card_->adjustSize();
    card_->move((width() - card_->width()) / 2, (height() - card_->height()) / 2);
}

void ModalDialog::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    if (intro_)
        return;
    intro_ = true;
    const QPoint home = card_->pos();
    if (!Motion::Enabled()) {
        scrim_ = 1.0;
        return;
    }
    // 遮罩淡入 + 卡片从下方 10px 落位：确认框是"停下来做决定"的场景，
    // 需要一个明确的"出现"过程，而不是一块黑幕瞬间盖上来。
    Motion::Animate(this, QStringLiteral("scrim"), 0.0, 1.0, Motion::Base, Motion::EnterCurve(),
                    [this](qreal v) {
                        scrim_ = v;
                        update();
                    });
    card_->move(home + QPoint(0, UiStyle::Fpx(10)));
    Motion::SlideTo(card_, home, Motion::Base, Motion::EnterCurve());
    Motion::FadeIn(card_, Motion::Base);
}

void ModalDialog::Finish(bool ok)
{
    if (finished_)
        return;
    finished_ = true;
    accepted_ = ok;
    if (!Motion::Enabled()) {
        if (loop_ && loop_->isRunning())
            loop_->quit();
        hide();
        return;
    }
    // 退场：遮罩与卡片同时收，等动画结束再 quit/hide —— 直接 hide 会让确认结果
    // 和画面消失同时发生，看起来像"点了一下却什么都没发生"。
    Motion::Animate(this, QStringLiteral("scrim"), scrim_, 0.0, Motion::Fast, Motion::ExitCurve(),
                    [this](qreal v) {
                        scrim_ = v;
                        update();
                    });
    Motion::FadeOut(card_, Motion::Fast);
    Motion::SlideTo(card_, card_->pos() + QPoint(0, UiStyle::Fpx(6)), Motion::Fast,
                    Motion::ExitCurve());
    if (loop_ && loop_->isRunning())
        QTimer::singleShot(Motion::Fast + 10, loop_, [this] {
            if (loop_ && loop_->isRunning())
                loop_->quit();
        });
    QTimer::singleShot(Motion::Fast + 20, this, [this] { hide(); });
}

void ModalDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(0, 0, 0, int(130 * scrim_)));
}

void ModalDialog::mousePressEvent(QMouseEvent *e)
{
    if (!card_->geometry().contains(e->position().toPoint()))
        Finish(false);
    QWidget::mousePressEvent(e);
}

void ModalDialog::keyPressEvent(QKeyEvent *e)
{
    if (e->key() == Qt::Key_Escape) {
        Finish(false);
        return;
    }
    if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
        Finish(true);
        return;
    }
    QWidget::keyPressEvent(e);
}

bool ModalDialog::eventFilter(QObject *obj, QEvent *ev)
{
    if (obj == parentWidget() && ev->type() == QEvent::Resize) {
        setGeometry(parentWidget()->rect());
        CenterCard();
    }
    return QWidget::eventFilter(obj, ev);
}

// ============================================================ ComicCard
ComicCard::ComicCard(const Comic &c, QWidget *parent) : QWidget(parent), c_(c)
{
    setCursor(Qt::PointingHandCursor);
    setAttribute(Qt::WA_Hover, true);
    cover_ = CoverArt::Render(c.title, QSize(width_, CoverHeight()), int(qHash(c.title) & 0x7fffffff));
    setFixedSize(width_, CoverHeight() + 40);
}

int ComicCard::CoverHeight() const { return width_ * 4 / 3; }

QSize ComicCard::sizeHint() const { return QSize(width_, CoverHeight() + 40); }

void ComicCard::SetCardWidth(int w)
{
    width_ = qMax(96, w);
    cover_ = CoverArt::Render(c_.title, QSize(width_, CoverHeight()), int(qHash(c_.title) & 0x7fffffff));
    setFixedSize(width_, CoverHeight() + 40);
    update();
}

void ComicCard::SetComic(const Comic &c)
{
    c_ = c;
    cover_ = CoverArt::Render(c.title, QSize(width_, CoverHeight()), int(qHash(c.title) & 0x7fffffff));
    update();
}

void ComicCard::enterEvent(QEnterEvent *) { hover_ = true; update(); }
void ComicCard::leaveEvent(QEvent *) { hover_ = false; update(); }

void ComicCard::mousePressEvent(QMouseEvent *e)
{
    if (e->button() == Qt::RightButton) {
        emit SecondaryClicked(c_, e->globalPosition().toPoint());
        e->accept();
        return;
    }
    QWidget::mousePressEvent(e);
}

void ComicCard::mouseReleaseEvent(QMouseEvent *e)
{
    if (e->button() == Qt::LeftButton && rect().contains(e->position().toPoint()))
        emit Activated(c_);
    QWidget::mouseReleaseEvent(e);
}

void ComicCard::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    CardPaint::Paint(&p, rect(), c_, hover_, false);
}


