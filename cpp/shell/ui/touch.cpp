#include "touch.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QGuiApplication>
#include <QInputDevice>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QSet>
#include <QTextEdit>
#include <QTimer>
#include <QTouchEvent>
#include <QWidget>

#include <cmath>
#include <cstdio>

#include "motion.h"
#include "theme.h"

namespace {

bool g_touchMode =
#ifdef Q_OS_ANDROID
    true;
#else
    false;
#endif

bool g_hasOverride = false;
Touch::Insets g_override;

bool DeviceHasTouchScreen()
{
    // Qt 6.12 的 devices() 返回 const 指针列表
    const QList<const QInputDevice *> devs = QInputDevice::devices();
    for (const QInputDevice *d : devs) {
        if (d->type() == QInputDevice::DeviceType::TouchScreen)
            return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------- 边缘提示
class EdgeHint : public QWidget
{
public:
    explicit EdgeHint(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setAttribute(Qt::WA_NoSystemBackground, true);
        setAttribute(Qt::WA_ShowWithoutActivating, true);
        setFocusPolicy(Qt::NoFocus);
        hide();
    }

    void SetProgress(qreal p)
    {
        p_ = qBound<qreal>(0.0, p, 1.0);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        if (p_ <= 0.001)
            return;
        const UiTokens &t = UiTheme::I().Tokens();
        const qreal k = p_;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);

        // 1) 左边缘竖条：直接映射"拖了多远"
        QColor bar = t.accent;
        bar.setAlphaF(0.85 * k);
        const qreal barW = 2.0 + 3.0 * k;
        p.fillRect(QRectF(0, height() * 0.18, barW, height() * 0.64), bar);

        // 2) 圆片 + 箭头：从左侧滑入，给出"松手就返回"的暗示
        const qreal r = Touch::Dp(18, this);
        // 用 sqrt 让圆片在拖动早期就露出来：线性映射时 75% 进度还只露出一半，
        // 看起来像"一条竖线旁边有个残影"。
        const qreal cx = -r + std::sqrt(k) * (r * 2.4);
        const qreal cy = height() / 2.0;

        QColor chip = t.accent;
        chip.setAlphaF(0.16 + 0.34 * k);
        p.setPen(Qt::NoPen);
        p.setBrush(chip);
        // 直角矩形风格：圆形气泡 → 方形提示块（内部箭头保持不变）
        p.drawRect(QRectF(cx - r, cy - r, r * 2, r * 2));

        QColor fg = t.accent;
        fg.setAlphaF(0.40 + 0.60 * k);
        QPen pen(fg, qMax<qreal>(1.6, r * 0.11), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        QPainterPath arrow;
        const qreal a = r * 0.36;
        arrow.moveTo(cx + a * 0.55, cy - a);
        arrow.lineTo(cx - a * 0.45, cy);
        arrow.lineTo(cx + a * 0.55, cy + a);
        p.drawPath(arrow);
    }

private:
    qreal p_ = 0.0;
};

// ---------------------------------------------------------------- 全局度量
namespace Touch {

bool IsTouchMode()
{
    return g_touchMode;
}

void SetTouchMode(bool on)
{
    g_touchMode = on;
}

bool IsMobile()
{
#ifdef Q_OS_ANDROID
    return true;
#else
    return false;
#endif
}

int Dp(int v, const QWidget *)
{
    // 不要再乘 devicePixelRatio：Qt 的逻辑坐标系本身就与 DPR 无关
    // （Android 上 1 逻辑像素 ≈ 1dp，Windows 200% 缩放下 1 逻辑像素 = 2 物理像素）。
    // 这里乘 DPR 会让高 DPI 设备上的 44dp 命中目标变成 88dp —— 第一版就是这么错的。
    return qMax(1, v);
}

void SetSafeAreaOverride(const Insets &in)
{
    g_hasOverride = true;
    g_override = in;
}

Insets SafeArea()
{
    if (g_hasOverride)
        return g_override;

    Insets in;
#ifdef Q_OS_ANDROID
    // QScreen 的 availableGeometry 在 Android 上会扣掉系统栏；沉浸式（全屏）下
    // 顶部通常为 0，但底部会保留手势导航条的高度（Pixel 上实测 78dp）。
    // 这个值会让整个根布局上浮，首页页码条/抽屉底栏下面留出一条与内容无关的
    // 空白带。这里明确按 edge-to-edge 处理：**底部不再预留安全区**，内容画到
    // 手势条后面；左右/顶部仍保留，避免刘海/挖孔压住标题。
    // （需要临时验证底部 inset 时用 --safe-insets 覆盖，不影响真实布局。）
    if (QScreen *s = QGuiApplication::primaryScreen()) {
        const QRect g = s->geometry();
        const QRect a = s->availableGeometry();
        in.left = qMax(0, a.left() - g.left());
        in.top = qMax(0, a.top() - g.top());
        in.right = qMax(0, (g.left() + g.width()) - (a.left() + a.width()));
        in.bottom = 0;
    }
#endif
    return in;
}

void InstallScrollGestures(QWidget *root)
{
    if (!root || !g_touchMode)
        return;

    const bool physical = DeviceHasTouchScreen();
    const QScroller::ScrollerGestureType type =
        physical ? QScroller::TouchGesture : QScroller::LeftMouseButtonGesture;

    const QList<QAbstractScrollArea *> areas = root->findChildren<QAbstractScrollArea *>();
    for (QAbstractScrollArea *area : areas) {
        if (area->property("jmScroller").toBool())
            continue;
        area->setProperty("jmScroller", true);
        // QAbstractScrollArea 传自身即可，QScroller 内部会挂到 viewport 上
        QScroller::grabGesture(area, type);
        if (area->property("jmHpanLock").toBool()) {
            // 页面级容器：连边界回弹也关掉，否则 range=0 时 QScroller 仍可能
            // 用 overshoot 把 viewport 里的内容整体推开一段
            if (QScroller *sc = QScroller::scroller(area)) {
                QScrollerProperties props = sc->scrollerProperties();
                props.setScrollMetric(QScrollerProperties::HorizontalOvershootPolicy,
                                      QScrollerProperties::OvershootAlwaysOff);
                sc->setScrollerProperties(props);
            }
        }
    }
}

int PolishHitTargets(QWidget *root)
{
    if (!root || !g_touchMode)
        return 0;
    int changed = 0;

    const int min = Dp(44, root);
    const QList<QAbstractButton *> buttons = root->findChildren<QAbstractButton *>();
    for (QAbstractButton *b : buttons) {
        if (b->objectName() == QLatin1String("NavButton"))
            continue;   // 导航栏本来就是 56dp 见方
        // hitGrow=false：这类控件的高度有版式意义（标题行的文字链、52x32 的开关），
        // 强行拉到 44dp 会把整行撑高或把轨道拉成竖条。
        const QVariant grow = b->property("hitGrow");
        if (grow.isValid() && !grow.toBool())
            continue;
        if (b->width() >= min && b->height() >= min)
            continue;
        // 只放大"两者都小于下限"的控件：把一个 30x30 的图标按钮变成 44x44，
        // 高度仍在 44px 的标题栏内，不会撑破布局。
        if (b->minimumWidth() == b->maximumWidth() || b->minimumHeight() == b->maximumHeight()) {
            const int w = qMax(b->width(), min);
            const int h = qMax(b->height(), min);
            b->setFixedSize(w, h);
            ++changed;
        }
    }
    return changed;
}

}  // namespace Touch

// ---------------------------------------------------------------- 横向平移锁
namespace {

// 装在 viewport 上的事件过滤器：吞掉"横向为主"的拖动。
//
// 鼠标与触摸**两条路径都要拦**：
//   * 桌面触控屏/鼠标拖拽：QScroller 用 LeftMouseButtonGesture，横向 MouseMove
//     被吞后拿不到横向位移；
//   * 真机触摸：QScroller 用 TouchGesture，走的是 QTouchEvent。原实现只处理
//     MouseMove，所以手机上这条锁从未生效 —— 纵向页面照样能整体左右平移。
//
// 只处理移动事件，press/begin/end 一律放行：QScroller 要靠按下开始判定，
// 也需要抬起收尾，否则会出现"滑完停在半路"。
class HorizontalPanLock : public QObject
{
public:
    explicit HorizontalPanLock(QAbstractScrollArea *area)
        : QObject(area->viewport()), area_(area) {}

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        switch (ev->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(ev);
            if (me->button() == Qt::LeftButton) {
                press_ = me->position().toPoint();
                tracking_ = true;
                horizontal_ = false;
            }
            break;
        }
        case QEvent::MouseMove: {
            if (!tracking_)
                break;
            auto *me = static_cast<QMouseEvent *>(ev);
            if (!(me->buttons() & Qt::LeftButton))
                break;
            const QPoint d = me->position().toPoint() - press_;
            if (qAbs(d.x()) > 6 && qAbs(d.x()) > qAbs(d.y())) {
                horizontal_ = true;
                StopHorizontalScroll();
                return true;   // 横向拖动：不交给 QScroller
            }
            break;
        }
        case QEvent::MouseButtonRelease:
            if (tracking_ && horizontal_) {
                StopHorizontalScroll();
                tracking_ = false;
                horizontal_ = false;
                return true;   // 吞掉抬起，避免 QScroller 用残留速度收尾
            }
            tracking_ = false;
            horizontal_ = false;
            break;

        case QEvent::TouchBegin: {
            auto *te = static_cast<QTouchEvent *>(ev);
            touchId_ = -1;
            for (const QEventPoint &p : te->points()) {
                if (p.state() == QEventPoint::State::Pressed) {
                    touchId_ = p.id();
                    touchPress_ = p.position();
                    break;
                }
            }
            trackingTouch_ = touchId_ >= 0;
            horizontal_ = false;
            break;
        }
        case QEvent::TouchUpdate: {
            if (!trackingTouch_)
                break;
            auto *te = static_cast<QTouchEvent *>(ev);
            // 多指（捏合/双指）不在这里拦，避免误伤缩放类控件
            if (te->points().size() > 1) {
                trackingTouch_ = false;
                break;
            }
            for (const QEventPoint &p : te->points()) {
                if (p.id() != touchId_)
                    continue;
                const QPointF d = p.position() - touchPress_;
                if (qAbs(d.x()) > 6 && qAbs(d.x()) > qAbs(d.y())) {
                    horizontal_ = true;
                    StopHorizontalScroll();
                    return true;   // 横向触摸拖动：不交给 QScroller
                }
            }
            break;
        }
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            if (trackingTouch_ && horizontal_) {
                StopHorizontalScroll();
                trackingTouch_ = false;
                touchId_ = -1;
                horizontal_ = false;
                return true;   // 吞掉抬起，避免惯性继续横移
            }
            trackingTouch_ = false;
            touchId_ = -1;
            horizontal_ = false;
            break;
        case QEvent::Wheel: {
            // 触控板的横向两指滑动同样不该平移整页；纵向滚轮照常放行。
            // 部分平台只给 pixelDelta（angleDelta 为 0），两个都要看。
            auto *we = static_cast<QWheelEvent *>(ev);
            const int dx = we->angleDelta().x() != 0 ? we->angleDelta().x()
                                                     : we->pixelDelta().x();
            const int dy = we->angleDelta().y() != 0 ? we->angleDelta().y()
                                                     : we->pixelDelta().y();
            if (dx != 0 && qAbs(dx) > qAbs(dy))
                return true;
            break;
        }
        default:
            break;
        }
        return QObject::eventFilter(obj, ev);
    }

private:
    void StopHorizontalScroll()
    {
        if (!area_)
            return;
        // QScroller 的手势识别在 QGestureManager（应用级）里先于对象过滤器，
        // 单靠"吞事件"挡不住已经启动的滚动：必须显式 stop，并把值拉回 0。
        if (QScroller *sc = QScroller::scroller(area_))
            sc->stop();
        if (QScrollBar *bar = area_->horizontalScrollBar())
            bar->setValue(0);
    }

    QPointer<QAbstractScrollArea> area_;
    QPoint press_;
    bool tracking_ = false;
    bool trackingTouch_ = false;
    bool horizontal_ = false;
    int touchId_ = -1;
    QPointF touchPress_;
};

}  // namespace

namespace Touch {

void LockHorizontalPan(QAbstractScrollArea *area)
{
    // 不再依赖 g_touchMode：桌面触控板/触屏同样会把整页横向拖走。
    // 过滤器只作用于页面级容器，内部横向控件（滑块、阅读器视口）不受影响。
    if (!area)
        return;
    if (!area->viewport() || area->property("jmHpanLock").toBool())
        return;
    area->setProperty("jmHpanLock", true);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 双保险：页面级容器不允许有横向可滚区间。
    // QScroller 的 pan 识别发生在 QGestureManager（应用级过滤器），对象过滤器
    // 只能"事后"吞事件；只要 hmax>0，它仍能在事件到达前改变滚动值。
    // 这里把 range 与 value 都钉死，QScroller 即使启动也没有可移动空间。
    if (QScrollBar *bar = area->horizontalScrollBar()) {
        bar->setRange(0, 0);
        // 注意：只把 value 钉在 0，不要连 rangeChanged 强制归零——
        // 实测（真机 V2463A）hbar range=0 会让 QScroller 判定整个 area
        // 不可滚动，纵向也一起失效；value 归零 + stop() 足以挡住横移。
        QObject::connect(bar, &QScrollBar::valueChanged, bar, [bar](int value) {
            if (value != 0)
                bar->setValue(0);
        });
    }
    area->viewport()->installEventFilter(new HorizontalPanLock(area));
}

}  // namespace Touch

// ---------------------------------------------------------------- 点击防误触
//
// 一次"按下→抬起"只有同时满足下面三条才算点击，否则整条手势按滑动处理：
//
//   1. 位移阈值 kTapSlopDp = 8dp，且取**过程峰值**而不是"抬起点 − 按下点"。
//      手指划出去再划回来时首末差≈0，只看首末差会把整段滑动判成点击 —— 这正是
//      "在下拉列表/菜单里滑一下再松手，仍然选中了某一项"的直接原因。
//   2. 长按漂移 kTapHoldDriftMs = 500ms 且 kTapDriftDp = 4dp：按得久、又在慢慢
//      挪动（慢慢拖列表）同样不是点击，长按不动仍然是点击。
//   3. 静默期 kTapIgnoreAfterScrollMs = 250ms：内容刚滚动过（含惯性尚未收尾）
//      期间的点击一律忽略，否则"想停滚动"会点进某一项。
//
// 判成滑动之后要做三件事，缺一不可：
//   * 吞掉抬起 —— 按钮不 clicked、菜单项不 triggered、下拉列表不提交；
//   * 滑动过程中不制造选中态 —— 菜单与下拉列表的 move 直接吞掉（高亮不跟手，
//     QMenu 的 hover 子菜单定时器也不会被续上），需要滚动就自己改 scrollbar；
//   * 结束后复位 —— 菜单 activeAction 归空并收起已展开的子菜单、按钮解除按下态、
//     下拉列表 currentIndex 回到按下时的值。
//
// 为什么必须在应用级过滤器里做：入口分散在侧栏（QToolButton）、QMenu（含二级
// 子菜单）和 QComboBox 弹窗列表（独立 popup 窗口）里，各自的 mouseReleaseEvent
// 语义不同，逐个改必然漏；而且弹窗容器按下后会 grab 鼠标，按下与抬起根本不在
// 同一个控件上，只有应用级过滤器能看到完整序列。
namespace {

enum class GuardScope {
    None,
    Button,      // 侧栏项 / 工具条按钮 / 开关
    Menu,        // QMenu 本体（含二三级子菜单），以及 QWidgetAction 里的子控件
    Combo,       // QComboBox 本体（含可编辑框里的 QLineEdit）
    PopupList,   // 下拉弹窗里的列表（view 或 viewport）
};

struct GestureState {
    bool active = false;
    GuardScope scope = GuardScope::None;
    QPointer<QWidget> pressWidget;    // 抬起时要复位的按钮/菜单
    QPointer<QWidget> scopeWidget;    // 菜单或下拉框本体（清理状态用）
    QPointer<QAbstractItemView> list; // PopupList：被拖动的列表
    QPointer<QAbstractScrollArea> panel;  // Combo：拖动时要手动补滚的外层容器
    QPoint pressGlobal;
    QPoint lastGlobal;
    qint64 pressMs = 0;
    int maxMoved = 0;                 // 过程峰值位移（不是首末差）
    int scrollAtPress = 0;
    bool scrollKnown = false;
    bool dragging = false;
    bool swallowMove = false;         // 菜单/弹窗列表：move 不再往下传
    QModelIndex listIndexAtPress;     // 弹窗列表按下时的选中项（滑动后回滚）
};

// 菜单项 / 下拉项的归属判定：从 w 往上找，**遇到顶层窗口就停**。
// 弹窗容器（QComboBoxPrivateContainer）的 parentWidget() 是 QComboBox，越过窗口
// 边界会把"弹窗里的点击"错判成"点在下拉框本体上"，进而把弹窗藏起来。
template <typename T>
T *AncestorInWindow(QWidget *w)
{
    for (QWidget *p = w; p; p = p->parentWidget()) {
        if (auto *hit = qobject_cast<T *>(p))
            return hit;
        if (p->isWindow())
            break;
    }
    return nullptr;
}

// 判断 w 是否落在"下拉弹窗里的列表"上（QComboBox 的 view / viewport / 弹窗容器）。
QAbstractItemView *PopupViewOf(QWidget *w)
{
    if (!w)
        return nullptr;
    QWidget *win = w->window();
    if (!win)
        return nullptr;
    // 必须比较 windowType()，不能写 windowFlags() & Qt::Popup：
    // Qt::Popup 的枚举值是 "0x8 | Window"，与普通窗口按位与也会成立（实测把
    // 主窗口里的按钮全判成了弹窗列表）。
    const Qt::WindowType type = win->windowType();
    if (type != Qt::Popup && type != Qt::ToolTip)
        return nullptr;
    if (qobject_cast<QMenu *>(win))
        return nullptr;                       // 菜单走 Menu 分支
    if (auto *v = qobject_cast<QAbstractItemView *>(w))
        return v;
    if (auto *v = qobject_cast<QAbstractItemView *>(w->parentWidget()))
        return v;
    // win == w 时也要找：弹窗 grab 鼠标后 move/release 会直接落到容器上。
    return win->findChild<QAbstractItemView *>();
}

QAbstractScrollArea *ScrollAreaAncestor(QWidget *w)
{
    for (QWidget *p = w; p; p = p->parentWidget()) {
        if (auto *area = qobject_cast<QAbstractScrollArea *>(p))
            return area;
    }
    return nullptr;
}

class TapGuard : public QObject
{
public:
    static TapGuard *I()
    {
        static TapGuard inst;
        return &inst;
    }

    void Install()
    {
        if (installed_ || !QCoreApplication::instance())
            return;
        installed_ = true;
        qApp->installEventFilter(this);
    }

    int swallowed() const { return swallowed_; }
    void resetStats() { swallowed_ = 0; }

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        // 只关心输入事件与"弹窗出现"：其它（绘制/布局/定时器）直接放行，
        // 不给全局事件循环加判断成本。
        switch (ev->type()) {
        case QEvent::Show:
            // 平台/样式差异：QComboBox 的弹窗可能比"按下"晚 50~100ms 才出现，
            // 正好落在手指已经开始滑动之后（Windows 11 样式实测如此）。
            OnShow(qobject_cast<QWidget *>(obj));
            return QObject::eventFilter(obj, ev);
        case QEvent::MouseButtonPress:
        case QEvent::MouseMove:
        case QEvent::MouseButtonRelease:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            break;
        default:
            return QObject::eventFilter(obj, ev);
        }

        QWidget *w = qobject_cast<QWidget *>(obj);
        if (!w)
            return QObject::eventFilter(obj, ev);

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        switch (ev->type()) {
        case QEvent::MouseButtonPress: {
            auto *me = static_cast<QMouseEvent *>(ev);
            if (me->button() != Qt::LeftButton)
                break;
            if (OnPress(w, me->globalPosition().toPoint(), now))
                return true;
            break;
        }
        case QEvent::MouseMove: {
            auto *me = static_cast<QMouseEvent *>(ev);
            if (OnMove(w, me->globalPosition().toPoint(),
                       (me->buttons() & Qt::LeftButton) != 0, now))
                return true;
            break;
        }
        case QEvent::MouseButtonRelease: {
            auto *me = static_cast<QMouseEvent *>(ev);
            if (me->button() != Qt::LeftButton)
                break;
            if (OnRelease(w, me->globalPosition().toPoint(), now))
                return true;
            break;
        }
        case QEvent::TouchBegin:
            if (OnTouch(w, static_cast<QTouchEvent *>(ev), now))
                return true;
            break;
        default:
            break;
        }
        return QObject::eventFilter(obj, ev);
    }

private:
    TapGuard() = default;

    static GuardScope ScopeOf(QWidget *w, QWidget **scopeWidget)
    {
        // 顺序有意义：弹窗列表必须排在 QComboBox 之前，否则弹窗里的按下会被
        // 当成"点在下拉框本体"，滑动时把整个弹窗藏掉。
        if (QAbstractItemView *v = PopupViewOf(w)) {
            *scopeWidget = v;
            return GuardScope::PopupList;
        }
        if (QMenu *m = AncestorInWindow<QMenu>(w)) {
            *scopeWidget = m;
            return GuardScope::Menu;
        }
        if (QComboBox *c = AncestorInWindow<QComboBox>(w)) {
            *scopeWidget = c;
            return GuardScope::Combo;
        }
        if (QAbstractButton *b = AncestorInWindow<QAbstractButton>(w)) {
            *scopeWidget = b;
            return GuardScope::Button;
        }
        if (w->property("tapGuardItem").toBool()) {
            *scopeWidget = w;
            return GuardScope::Button;
        }
        *scopeWidget = nullptr;
        return GuardScope::None;
    }

    bool OnPress(QWidget *w, const QPoint &global, qint64 now)
    {
        // 上一次手势没等到抬起（弹窗被销毁 / 窗口切换）：先把状态清干净
        if (g_.active)
            Settle(g_);
        QWidget *scopeWidget = nullptr;
        const GuardScope scope = ScopeOf(w, &scopeWidget);
        if (scope == GuardScope::None)
            return false;

        g_ = GestureState{};
        g_.active = true;
        g_.scope = scope;
        g_.pressWidget = w;
        g_.scopeWidget = scopeWidget;
        g_.list = (scope == GuardScope::PopupList) ? qobject_cast<QAbstractItemView *>(scopeWidget)
                                                   : nullptr;
        if (g_.list)
            g_.listIndexAtPress = g_.list->currentIndex();
        g_.pressGlobal = global;
        g_.lastGlobal = global;
        g_.pressMs = now;
        bool ok = false;
        g_.scrollAtPress = ScrollFingerprint(w, &ok);
        g_.scrollKnown = ok;
        return false;   // 按下必须放行：按下态/高亮/弹窗都要靠它产生
    }

    bool OnMove(QWidget *w, const QPoint &global, bool leftDown, qint64 now)
    {
        if (!g_.active || !leftDown)
            return false;
        const int moved = (global - g_.pressGlobal).manhattanLength();
        if (moved > g_.maxMoved)
            g_.maxMoved = moved;
        const QPoint prev = g_.lastGlobal;
        g_.lastGlobal = global;

        if (!g_.dragging && moved > Touch::Dp(Touch::kTapSlopDp, w))
            BeginDrag();
        if (!g_.dragging) {
            // 还在死区内：留给控件自己做悬停/高亮，点击手感不变
            return false;
        }

        DragTo(global, prev);
        return g_.swallowMove;
    }

    void BeginDrag()
    {
        g_.dragging = true;
        switch (g_.scope) {
        case GuardScope::Menu:
            // QMenu 的 mouseMoveEvent 会 setCurrentAction + 启动 20ms 子菜单定时器，
            // 光"清一下 activeAction"再放行等于没清：它立刻又设回来。从这里开始
            // move 一律吞掉，菜单的高亮与子菜单都不再跟着手指走。
            ClearMenu(g_.scopeWidget);
            g_.swallowMove = true;
            break;
        case GuardScope::PopupList:
            // 弹窗列表自己滚（见 DragTo），move 不外泄 → 列表高亮不跟手
            g_.swallowMove = true;
            break;
        case GuardScope::Combo:
            // 有的样式在**按下**就弹窗（QComboBox::mousePressEvent → showPopup）。
            // 手指一开始滑就必须把它收回：否则后续 move/release 全被弹窗容器
            // grab 走，一松手就变成"选中手指下面那一项"。
            if (auto *c = qobject_cast<QComboBox *>(g_.scopeWidget)) {
                if (QAbstractItemView *v = c->findChild<QAbstractItemView *>()) {
                    if (v->isVisible() && v->window() && v->window()->isVisible())
                        c->hidePopup();
                }
                // 这个 press 被下拉框吃掉了，QScroller 拿不到，外层页面只能自己滚
                g_.panel = ScrollAreaAncestor(c->parentWidget());
            }
            g_.swallowMove = false;   // 页面还要继续滚，move 不能吞
            break;
        case GuardScope::Button:
            // 按钮不拦 move：页面滚动靠 QScroller 转发的位移，吞了就滚不动了。
            if (auto *b = qobject_cast<QAbstractButton *>(g_.pressWidget))
                b->setDown(false);
            g_.swallowMove = false;
            break;
        case GuardScope::None:
            break;
        }
    }

    void DragTo(const QPoint &global, const QPoint &prev)
    {
        const int dy = global.y() - prev.y();
        switch (g_.scope) {
        case GuardScope::PopupList: {
            if (!g_.list)
                break;
            if (QScrollBar *sb = g_.list->verticalScrollBar()) {
                if (dy != 0) {
                    scrolling_ = true;
                    sb->setValue(sb->value() - dy);
                    scrolling_ = false;
                }
            }
            break;
        }
        case GuardScope::Menu:
            // 子菜单可能在"按下 → 判定为滑动"的间隙里已经弹出（QMenu 的 hover
            // 定时器），这里每次 move 都收一遍，保证滑动过程中/结束后都不残留。
            ClearMenu(g_.scopeWidget);
            break;
        case GuardScope::Combo: {
            if (!g_.panel)
                break;
            // QScroller 已经在拖这个容器时不要叠加：两边各改一次滚动值会变成双倍速度
            if (QScroller *sc = QScroller::scroller(g_.panel)) {
                if (sc->state() != QScroller::Inactive)
                    break;
            }
            if (QScrollBar *sb = g_.panel->verticalScrollBar()) {
                if (dy != 0) {
                    scrolling_ = true;
                    sb->setValue(sb->value() - dy);
                    scrolling_ = false;
                }
            }
            break;
        }
        case GuardScope::Button:
            if (auto *b = qobject_cast<QAbstractButton *>(g_.pressWidget))
                b->setDown(false);
            break;
        case GuardScope::None:
            break;
        }
    }

    bool OnRelease(QWidget *w, const QPoint &global, qint64 now)
    {
        if (!g_.active)
            return false;
        const GestureState gs = g_;
        g_ = GestureState{};

        const int moved = (global - gs.pressGlobal).manhattanLength();
        bool scrollNow = false;
        if (gs.scrollKnown && gs.pressWidget) {
            bool ok = false;
            const int v = ScrollFingerprint(gs.pressWidget, &ok);
            scrollNow = ok && v != gs.scrollAtPress;
        }
        // 惯性滚动尚未收尾时的点击一律不算数（"想停滚动"不该点进某一项）
        const bool quiet = lastScrollMs_ > 0
                           && (now - lastScrollMs_) < Touch::kTapIgnoreAfterScrollMs;
        // 长按 + 缓慢漂移同样不是点击
        const bool holdDrift = (now - gs.pressMs) > Touch::kTapHoldDriftMs
                               && gs.maxMoved > Touch::Dp(Touch::kTapDriftDp, w);
        // 位移判据取"过程峰值"：划出去再划回来（首末差≈0）也必须算滑动
        const bool drag = gs.dragging || moved > Touch::Dp(Touch::kTapSlopDp, w)
                          || gs.maxMoved > Touch::Dp(Touch::kTapSlopDp, w) || holdDrift;

        if (!drag && !scrollNow && !quiet)
            return false;   // 真点击：原样交给控件，clicked/triggered 照常

        // 吞掉这次抬起：按钮不会 clicked()、菜单项不会 triggered()、
        // 下拉列表不会提交，同时把中途产生的选中态复位。
        Settle(gs);
        ++swallowed_;
        if (qEnvironmentVariableIsSet("JM_TAPGUARD_LOG")) {
            std::fprintf(stderr,
                         "tapguard: ignored %s[%s] scope=%d drag=%d peak=%d end=%d "
                         "hold=%lldms quiet=%d scroll=%d\n",
                         w->metaObject()->className(),
                         qPrintable(w->objectName().isEmpty() ? QStringLiteral("-")
                                                              : w->objectName()),
                         int(gs.scope), int(drag), gs.maxMoved, moved,
                         (long long)(now - gs.pressMs), int(quiet), int(scrollNow));
            std::fflush(stderr);
        }
        return true;
    }

    // 触摸路径：只有"自己接受触摸事件"的控件才收得到（其余控件由 Qt 合成鼠标
    // 事件，走上面的鼠标分支）。菜单/弹窗列表属于前者时同样要守，否则真机上
    // 这条链路完全没有防护。
    bool OnTouch(QWidget *w, QTouchEvent *te, qint64 now)
    {
        if (te->points().isEmpty())
            return false;
        const QPoint global = te->points().first().globalPosition().toPoint();
        switch (te->type()) {
        case QEvent::TouchBegin:
            return OnPress(w, global, now);
        case QEvent::TouchUpdate:
            return OnMove(w, global, true, now);
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            return OnRelease(w, global, now);
        default:
            return false;
        }
    }

    void OnShow(QWidget *w)
    {
        if (!w || !g_.active || !g_.dragging || g_.scope != GuardScope::Combo)
            return;
        QWidget *comboW = g_.scopeWidget;
        if (!comboW || !w->isWindow() || w->parentWidget() != comboW)
            return;
        // 手指已经在滑，弹窗这时才出现：立刻收回。否则它 grab 鼠标，后续
        // move/release 全部落到列表上 —— 一松手就是"选中手指下面那一项"。
        QPointer<QComboBox> c = qobject_cast<QComboBox *>(comboW);
        if (!c)
            return;
        QTimer::singleShot(0, c, [c] {
            if (c)
                c->hidePopup();
        });
    }

    static void ClearMenu(QWidget *w)
    {
        auto *m = qobject_cast<QMenu *>(w);
        if (!m)
            return;
        // setActiveAction(nullptr) 会收起"当前项对应的子菜单"；再把仍然可见的
        // 子菜单显式 hide 一遍，覆盖 hover 定时器刚弹出、状态还没来得及同步的情况。
        m->setActiveAction(nullptr);
        const QList<QAction *> acts = m->actions();
        for (QAction *a : acts) {
            if (QMenu *sub = a->menu()) {
                if (sub->isVisible())
                    sub->hide();
            }
        }
    }

    void Settle(const GestureState &gs)
    {
        if (auto *b = qobject_cast<QAbstractButton *>(gs.pressWidget))
            b->setDown(false);
        ClearMenu(gs.scopeWidget);
        // 下拉框：滑动期间被延时弹出的列表要在收尾时确认关掉（正常点击不经过这里）
        if (gs.scope == GuardScope::Combo) {
            if (auto *c = qobject_cast<QComboBox *>(gs.scopeWidget)) {
                if (QAbstractItemView *v = c->findChild<QAbstractItemView *>()) {
                    if (v->isVisible())
                        c->hidePopup();
                }
            }
        }
        // 弹窗列表：滑动期间没让高亮跟手，这里再把按下时动过的选中项收回原位，
        // 避免"滑动结束后列表停在被误选的那一项上"。
        if (gs.list) {
            if (gs.listIndexAtPress.isValid()
                && gs.list->currentIndex() != gs.listIndexAtPress) {
                // 回滚选中项可能带动列表滚动：同样属于"守卫自己造成的位移"，
                // 不该让随后的正常点选落进 250ms 静默期。
                scrolling_ = true;
                gs.list->setCurrentIndex(gs.listIndexAtPress);
                scrolling_ = false;
            }
        }
    }

    // 手指所在滚动区的"滚动位置指纹"：抬起时和按下时不一致 = 这一次手势滚过内容。
    // 只取最近的祖先滚动区，足够判断，也避免遍历整棵树。
    int ScrollFingerprint(QWidget *w, bool *ok)
    {
        for (QWidget *p = w; p; p = p->parentWidget()) {
            if (auto *area = qobject_cast<QAbstractScrollArea *>(p)) {
                if (!area->horizontalScrollBar() || !area->verticalScrollBar())
                    continue;
                // 顺带把"最近一次滚动时间"接上：惯性滚动期间用户再点一下，
                // 这一下不该算点击（Android 上就是"停滚动反倒点进了某一项"）。
                if (!hooked_.contains(area)) {
                    hooked_.insert(area);
                    for (QScrollBar *bar : {area->horizontalScrollBar(),
                                            area->verticalScrollBar()}) {
                        connect(bar, &QScrollBar::valueChanged, this,
                                [this](int) {
                                    // 守卫自己拖出来的位移没有惯性，紧接着的点击不该被
                                    // "防惯性"静默期误伤（弹窗列表里点选就属于这种）。
                                    if (!scrolling_)
                                        lastScrollMs_ = QDateTime::currentMSecsSinceEpoch();
                                },
                                Qt::UniqueConnection);
                    }
                }
                *ok = true;
                return area->verticalScrollBar()->value() * 100003
                       + area->horizontalScrollBar()->value();
            }
        }
        *ok = false;
        return 0;
    }

    bool installed_ = false;
    int swallowed_ = 0;
    GestureState g_;
    qint64 lastScrollMs_ = 0;
    bool scrolling_ = false;   // 守卫自己在拖内容（没有惯性），不参与静默期
    QSet<QAbstractScrollArea *> hooked_;
};

}  // namespace

namespace Touch {

void InstallTapGuard()
{
    TapGuard::I()->Install();
}

int TapGuardSwallowed()
{
    return TapGuard::I()->swallowed();
}

void ResetTapGuardStats()
{
    TapGuard::I()->resetStats();
}

}  // namespace Touch

// ---------------------------------------------------------------- 适配器
TouchAdapter::TouchAdapter(QWidget *host) : QObject(host), host_(host) {}

TouchAdapter::~TouchAdapter() = default;

void TouchAdapter::Install()
{
    if (installed_ || !host_)
        return;
    installed_ = true;

    // 应用级过滤器：页面里的子控件（列表、按钮、阅读器）也要能拦到。
    // 只装到 host_ 上的话，触摸事件被最内层控件接走，host_ 根本看不到。
    qApp->installEventFilter(this);

    Touch::InstallScrollGestures(host_);
    Touch::PolishHitTargets(host_);
}

void TouchAdapter::SetActive(bool on)
{
    active_ = on;
    if (!on)
        ShowHint(0);
}

int TouchAdapter::EdgeWidth() const
{
    return Touch::Dp(24, host_);
}

int TouchAdapter::Threshold() const
{
    return Touch::Dp(72, host_);
}

bool TouchAdapter::AtEdge(const QPoint &globalPos) const
{
    if (!host_)
        return false;
    const QPoint local = host_->mapFromGlobal(globalPos);
    if (local.y() < 0 || local.y() > host_->height())
        return false;
    return local.x() >= 0 && local.x() <= EdgeWidth();
}

void TouchAdapter::BeginDrag(const QPoint &globalPos)
{
    dragging_ = true;
    decided_ = false;
    horizontal_ = false;
    startGlobal_ = globalPos;
    progress_ = 0.0;
}

void TouchAdapter::UpdateDrag(const QPoint &globalPos)
{
    if (!dragging_)
        return;

    const int dx = globalPos.x() - startGlobal_.x();
    const int dy = globalPos.y() - startGlobal_.y();

    if (!decided_) {
        const int slop = Touch::Dp(8, host_);
        if (qAbs(dx) < slop && qAbs(dy) < slop)
            return;
        decided_ = true;
        // 横滑判据用 1:1 而非"角度"：Android 的返回手势同样是水平优先，
        // 并且要求向右（dx>0），这样竖滑列表永远不会被误吞。
        horizontal_ = (dx > 0) && (qAbs(dx) > qAbs(dy));
        if (!horizontal_) {
            dragging_ = false;
            ShowHint(0);
            return;
        }
    }

    progress_ = qBound<qreal>(0.0, qreal(dx) / qreal(Threshold()), 1.0);
    ShowHint(progress_);
    emit EdgeProgress(progress_);
}

void TouchAdapter::FinishDrag(bool commit)
{
    const bool fire = commit && horizontal_ && progress_ >= 1.0;
    dragging_ = false;
    decided_ = false;
    horizontal_ = false;
    progress_ = 0.0;
    ShowHint(0);
    if (fire)
        emit BackRequested();
}

void TouchAdapter::ShowHint(qreal p)
{
    if (!host_)
        return;
    if (p <= 0.001) {
        // 松手/取消：提示淡出而不是"啪"地消失（边缘返回的手势反馈要收得住）
        if (hint_ && hint_->isVisible()) {
            QPointer<EdgeHint> hint = hint_;
            Motion::FadeOut(hint_, Motion::Fast, [hint] {
                if (hint)
                    hint->hide();
            });
        }
        return;
    }
    if (!hint_) {
        hint_ = new EdgeHint(host_);
        hint_->resize(Touch::Dp(64, host_), Touch::Dp(132, host_));
    }
    // 上一次的淡出还没走完就又按下了：立刻摘掉 effect（否则会从半透明开始）
    if (hint_->graphicsEffect())
        hint_->setGraphicsEffect(nullptr);
    const QSize hs = hint_->size();
    hint_->move(0, (host_->height() - hs.height()) / 2);
    hint_->SetProgress(p);
    hint_->show();
    hint_->raise();
}

void TouchAdapter::ShowEdgeHint(qreal progress)
{
    ShowHint(progress);
}

bool TouchAdapter::eventFilter(QObject *obj, QEvent *ev)
{
    if (!active_ || !host_)
        return QObject::eventFilter(obj, ev);

    QWidget *w = qobject_cast<QWidget *>(obj);
    if (!w || w->window() != host_)
        return QObject::eventFilter(obj, ev);

    // 输入法弹出时把焦点控件滚进可视区（Android 上软键盘会盖住下半屏）
    if (ev->type() == QEvent::FocusIn && Touch::IsTouchMode()) {
        const bool textInput = qobject_cast<QLineEdit *>(w) || qobject_cast<QTextEdit *>(w);
        if (textInput) {
            QPointer<QWidget> field = w;
            QTimer::singleShot(320, this, [field] {
                if (!field)
                    return;
                for (QWidget *p = field->parentWidget(); p; p = p->parentWidget()) {
                    if (auto *area = qobject_cast<QScrollArea *>(p)) {
                        area->ensureWidgetVisible(field, 0, Touch::Dp(96, area));
                        break;
                    }
                }
            });
        }
    }

    switch (ev->type()) {
    case QEvent::TouchBegin: {
        auto *te = static_cast<QTouchEvent *>(ev);
        if (te->points().isEmpty())
            break;
        // 有控件接了触摸事件时走这条路；否则 Qt 会合成鼠标事件，走下一条分支。
        const QPoint gp = te->points().first().globalPosition().toPoint();
        if (!AtEdge(gp))
            break;
        BeginDrag(gp);
        break;
    }
    case QEvent::TouchUpdate: {
        if (!dragging_)
            break;
        auto *te = static_cast<QTouchEvent *>(ev);
        if (te->points().isEmpty())
            break;
        UpdateDrag(te->points().first().globalPosition().toPoint());
        if (horizontal_)
            return true;   // 已判定为返回手势：不再让页面滚动
        break;
    }
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        if (!dragging_)
            break;
        const bool wasHorizontal = horizontal_;
        FinishDrag(ev->type() == QEvent::TouchEnd);
        if (wasHorizontal)
            return true;
        break;
    }
    case QEvent::MouseButtonPress: {
        if (!Touch::IsTouchMode())
            break;
        auto *me = static_cast<QMouseEvent *>(ev);
        if (me->button() != Qt::LeftButton)
            break;
        if (!AtEdge(me->globalPosition().toPoint()))
            break;
        BeginDrag(me->globalPosition().toPoint());
        // 按下先不吞：万一是竖滑，页面还要正常滚。方向判明后再接管。
        break;
    }
    case QEvent::MouseMove: {
        if (!dragging_)
            break;
        auto *me = static_cast<QMouseEvent *>(ev);
        UpdateDrag(me->globalPosition().toPoint());
        if (horizontal_)
            return true;
        break;
    }
    case QEvent::MouseButtonRelease: {
        if (!dragging_)
            break;
        const bool wasHorizontal = horizontal_;
        FinishDrag(true);
        if (wasHorizontal)
            return true;
        break;
    }
    default:
        break;
    }

    return QObject::eventFilter(obj, ev);
}
