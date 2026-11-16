#include "touch.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QStringList>
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
#include "touch_menu.h"

namespace {

// 同一根手指的"手势序列"续接窗口。
//
// 弹窗/子菜单出现时 Qt 会补发一条 MouseButtonPress（mouse replay / popup grab
// 转移），距离上一条 move 通常只有几十毫秒。旧实现把它当成一次全新的点击，
// 把过程峰值与起点一起归零，于是"从选项框上方滑进列表再松手"被判成一次
// 6px 的短点击 —— 正是二级菜单 / 下拉选项框 / 悬浮菜单残留误触的根因。
// 这个窗口只用于"弹出类窗口里的补发按下"，普通控件上的新按下永远开新序列，
// 所以"滑一下马上点另一个按钮"不受影响。
constexpr qint64 kTapSequenceMs = 300;

// 回归对照开关：--tapguard-legacy 复刻修复前"弹窗补发按下会重置手势"的行为，
// 用来在桌面/真机上做同一套用例的 A/B，平时恒为 false。
bool g_tapGuardLegacy = false;

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

    // root 本身是滚动区时 findChildren 扫不到它：模型选择浮层这类打开时才创建的
    // 列表只能把 list 自己传进来（没有父页面可扫），漏了就完全无法拖动（实测）。
    QList<QAbstractScrollArea *> areas = root->findChildren<QAbstractScrollArea *>();
    if (auto *self = qobject_cast<QAbstractScrollArea *>(root)) {
        if (!areas.contains(self))
            areas.prepend(self);
    }
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
// 一次"按下→抬起"只有同时满足下面几条才算点击，否则整条手势按滑动处理：
//
//   1. 位移阈值 kTapSlopDp = 8dp，且取**过程峰值**而不是"抬起点 − 按下点"。
//      手指划出去再划回来时首末差≈0，只看首末差会把整段滑动判成点击 —— 这正是
//      "在下拉列表/菜单里滑一下再松手，仍然选中了某一项"的直接原因。
//   2. 长按漂移 kTapHoldDriftMs = 500ms 且 kTapDriftDp = 4dp：按得久、又在慢慢
//      挪动（慢慢拖列表）同样不是点击，长按不动仍然是点击。
//   3. 静默期 kTapIgnoreAfterScrollMs = 250ms：内容刚滚动过（含惯性尚未收尾）
//      期间的点击一律忽略，否则"想停滚动"会点进某一项。
//   4. 滚动证据 / 多指：本次手势期间任一滚动区动过，或出现过第二根手指，都不是点击。
//   5. **手势序列连续**（本轮补的残留误触根因）：弹窗拿到 grab、二级子菜单由
//      20ms hover 定时器弹出时，Qt 会补发一条 MouseButtonPress。它以前被当成
//      全新点击，pressGlobal / 峰值一起归零：从选项框上方滑进列表的几十像素被
//      洗掉，抬手只剩 5~6px → 下拉框换选项、菜单项 triggered（独立探针实测
//      currentIndex 0→2、QMenu::triggered 两次）。现在这些补发按下沿用同一序列的
//      累计峰值并标成"受污染"，整条手势强制按滑动算，且**连按下一起吞掉**
//      （QComboBox 在 press 就 showPopup、QMenu 在 press 就起子菜单定时器）。
//
// 判成滑动之后要做四件事，缺一不可：
//   * 吞掉抬起 —— 按钮不 clicked、菜单项不 triggered、下拉列表不提交；
//   * 吞掉污染序列里的补发按下 —— 弹窗/子菜单不许在中途被"点开"；
//   * 滑动过程中不制造选中态 —— 菜单与下拉列表的 move 直接吞掉（高亮不跟手），
//     整条菜单链（含二三级子菜单）的 activeAction / hover 定时器一起清掉；
//   * 结束后复位 —— 菜单 activeAction 归空并收起已展开的子菜单、按钮解除按下态、
//     下拉列表 currentIndex 回到按下时的值。
//
// 覆盖范围：Button / Item / Generic 之外，还有 Menu（含子菜单链）、Combo、
// PopupList，以及 Overlay —— 所有 Qt::Popup/Tool/ToolTip 顶层窗口与项目自绘浮层
// （声明 tapGuardOverlay）里的**任何**控件都兜底纳入，不再逐控件列白名单。
// 自绘浮层里"按下即动作"的控件（ModalDialog 遮罩、ProxyPage 悬浮抽屉）同步改成
// 抬起才执行，否则只吞抬起拦不住 press 里的副作用。
//
// 为什么必须在应用级过滤器里做：入口分散在侧栏、QMenu、QComboBox 弹窗、
// 悬浮窗与悬浮菜单里，各自的 mouseReleaseEvent 语义不同，逐个改必然漏；而且
// 弹窗容器按下后会 grab 鼠标，按下与抬起根本不在同一个控件上，只有应用级
// 过滤器能看到完整序列。
namespace {

enum class GuardScope {
    None,
    Generic,     // 自绘可点控件（卡片 / 整行 / 文字链）：只吞"拖动后的抬起"
    Item,        // QAbstractItemView（非弹窗）：行点击同样不许由滑动产生
    Button,      // 侧栏项 / 工具条按钮 / 开关
    Menu,        // QMenu 本体（含二三级子菜单），以及 QWidgetAction 里的子控件
    Combo,       // QComboBox 本体（含可编辑框里的 QLineEdit）
    PopupList,   // 下拉弹窗里的列表（view 或 viewport）
    Overlay,     // 悬浮窗 / 悬浮菜单 / 自绘浮层（遮罩、吸顶条、浮条、模态框）
};

const char *ScopeName(GuardScope s)
{
    switch (s) {
    case GuardScope::Generic:   return "generic";
    case GuardScope::Item:      return "item";
    case GuardScope::Button:    return "button";
    case GuardScope::Menu:      return "menu";
    case GuardScope::Combo:     return "combo";
    case GuardScope::PopupList: return "popup-list";
    case GuardScope::Overlay:   return "overlay";
    case GuardScope::None:      break;
    }
    return "none";
}

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
    bool touchOrigin = false;         // 本次点击手势由触摸合成而来（不是桌面鼠标）
    bool contaminated = false;        // 手势期间出现过弹窗/补发按下：整条按滑动算
    bool menuTaken = false;           // 菜单按下已由 MenuPressController 接管
    QPointer<QAction> pressMenuAction;        // Menu：按下时手指下的菜单项
    QPersistentModelIndex pressListIndex;     // PopupList：按下时手指下的列表行
    QModelIndex listIndexAtPress;     // 弹窗列表按下时的选中项（滑动后回滚）
};

// 一根手指的完整序列。它比"一次 press→release"更长：弹出类窗口会在中途
// 补发按下，序列必须跨过这些按下保持峰值，否则短距离滑动会被洗成点击。
struct GestureSequence {
    bool active = false;
    bool swiping = false;
    int peak = 0;
    qint64 lastMs = 0;
};

// 触摸轨迹。与上一条"点击手势"分开维护，原因见 OnTouch 的注释：
// 真机上 QScroller 吃掉 touch 之后，控件只收到"合成的鼠标按下 + 抬起"，
// 中间没有任何 MouseMove —— 峰值位移只能从原始触摸流里取。
struct TouchTrack {
    bool active = false;
    int id = -1;
    QPoint pressGlobal;
    QPoint lastGlobal;
    int peak = 0;
    bool multi = false;   // 出现过第二根手指
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

// 顶层窗口是"弹出类"（QMenu、QComboBoxPrivateContainer、QMenu 的二级子菜单、
// 悬浮菜单）。注意必须用 windowType() 比较，不能用 windowFlags() & Qt::Popup：
// Qt::Popup 的枚举值含 Window 位，普通窗口按位与也会成立。
bool IsPopupWindow(QWidget *w)
{
    if (!w)
        return false;
    QWidget *win = w->window();
    if (!win)
        return false;
    const Qt::WindowType t = win->windowType();
    return t == Qt::Popup || t == Qt::ToolTip || t == Qt::Tool;
}

// 项目自绘浮层不是顶层窗口，而是叠在主窗口上的全屏遮罩
// （SrModelSheet / ModalDialog / 悬浮筛选面板 / 浮条）。构造处声明
// tapGuardOverlay=true，让浮层里的每一行、每个按钮都参与统一仲裁。
QWidget *OverlayAncestor(QWidget *w)
{
    for (QWidget *p = w; p; p = p->parentWidget()) {
        if (p->property("tapGuardOverlay").toBool())
            return p;
        if (p->isWindow())
            break;
    }
    return nullptr;
}

// 弹窗/子菜单补发的按下会出现在这两类窗口里；普通页面上的按下不受序列续接影响。
bool IsPopupContext(QWidget *w)
{
    return IsPopupWindow(w) || OverlayAncestor(w);
}

// QComboBox 的弹窗列表是否正开着。
bool ComboPopupOpen(QWidget *combo)
{
    auto *c = qobject_cast<QComboBox *>(combo);
    if (!c)
        return false;
    if (QAbstractItemView *v = c->findChild<QAbstractItemView *>())
        return v->isVisible();
    return false;
}

// 文本输入/数值拖拽类控件：它们自己要用整条按下-移动-抬起（选词、拖光标、
// 拖滑块），吞掉抬起会破坏交互，因此永远不参与"点击仲裁"。
bool IsSelfDrivenControl(QWidget *w)
{
    return qobject_cast<QLineEdit *>(w) || qobject_cast<QTextEdit *>(w)
           || qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QAbstractSlider *>(w)
           || qobject_cast<QScrollBar *>(w) || qobject_cast<QAbstractScrollArea *>(w);
}

// "这个控件是否自己声明了可点"。
//
// 判定优先级（从强到弱）：
//   1. tapGuardSelfManaged=true：控件内部已有滑动/点击仲裁（SectionStrip），
//      守卫必须完全让路 —— 吞掉它的抬起会让它停在"拖动中"状态（惯性/状态复位都不走）。
//   2. tapGuardItem / toggleRow / clickable：显式声明（SettingRow 的整行点击）。
//   3. WA_SetCursor + PointingHandCursor：自绘控件的"可点"声明（ComicCard、
//      SrModelField、侧栏行都靠这条，不需要给每个控件加属性）。
//
// 只认 WA_SetCursor 是必要的：QWidget::cursor() 会继承父控件的 cursor，
// 拿 cursor() 判断会把整页所有子控件都判成可点。
bool IsTappable(QWidget *w)
{
    if (!w)
        return false;
    if (w->property("tapGuardSelfManaged").toBool())
        return false;
    if (w->property("tapGuardItem").toBool() || w->property("toggleRow").toBool()
        || w->property("clickable").toBool())
        return true;
    if (w->testAttribute(Qt::WA_SetCursor) && w->cursor().shape() == Qt::PointingHandCursor)
        return true;
    return false;
}

// 从 w 往上找最近的"可点"控件（遇窗口停）。QML 宿主（QQuickWidget）必须排除：
// 卡片点击由 QML 的 MouseArea/Flickable 自己仲裁，在 C++ 侧吞事件会让
// MouseArea 收不到抬起而停在按下态（缩放/按压层不复位）。
QWidget *TappableAncestor(QWidget *w)
{
    for (QWidget *p = w; p; p = p->parentWidget()) {
        if (p->inherits("QQuickWidget"))
            return nullptr;
        if (IsSelfDrivenControl(p))
            return nullptr;
        if (IsTappable(p))
            return p;
        if (p->isWindow())
            break;
    }
    return nullptr;
}

// 冻结过滤器：手势一旦被判定为滑动（BeginDrag），就把该弹出窗口收到的
// 鼠标 move 与补发按下全部拦在 QMenu::event() 之前。
//
// 为什么要装对象级过滤器：应用级过滤器只能拦"事件第一次进入应用"的那一遍，
// 但 Qt 内部还会转发（QMenuPrivate::mouseEventTaken 把事件转给 caused popup）、
// 重放（popup 打开时补发按下），Qt 6.12 起 QMenu::mouseReleaseEvent 在
// mouseDown 为空时不再提前返回、而是继续激活 currentAction —— 高亮跟手的
// move 会把 currentAction 改成手指下的项，抬起一旦有任何路径漏吞就是误选。
// 对象级过滤器直接挡在 QMenu::event() 前面，move 与补发按下都到不了菜单，
// currentAction 停在按下时的项上，Qt 的激活条件就不可能再成立。
//
// 只吞 move：高亮跟手 / 子菜单定时器 / combo currentIndex 全部失效。
// 按下与抬起不能在这里吞 —— 吞抬起后冻结静默期（250ms）内的任何新按下都
// 必须**先**过应用级守卫（OnPress 建立新手势并解冻冻结层），冻结层若把按下
// 吞掉，快速连续操作时按下永远到不了守卫，菜单/选项框会整体失灵（实测：
// 自检与真机全部按下消失）。skipFirstPress/armSkipFirstPress 机制随按下
// 分支一并移除 —— 应用级 OnPress 的接管/续接判定就是放行与否的唯一裁决者。
class PopupFreeze : public QObject
{
public:
    explicit PopupFreeze(QObject *parent, bool skipFirstPress)
        : QObject(parent), skipFirstPress_(skipFirstPress)
    {
        Q_UNUSED(skipFirstPress_);
    }

    bool eventFilter(QObject *, QEvent *ev) override
    {
        switch (ev->type()) {
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:              // 高亮跟手 / 子菜单定时器 / combo currentIndex
            return true;
        default:
            return false;                    // 按下/抬起交给应用级守卫统一判定
        }
    }

private:
    bool skipFirstPress_;
};

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
        if (!w) {
            // 触摸事件在真机上投给 QWidgetWindow（不是 QWidget），这里只取轨迹，
            // 与接收者无关。旧实现要求 obj 是 QWidget，于是整条触摸流被丢掉，
            // "过程峰值"在真机上永远取不到 —— 这是误触修复失效的一半原因。
            switch (ev->type()) {
            case QEvent::TouchBegin:
            case QEvent::TouchUpdate:
            case QEvent::TouchEnd:
            case QEvent::TouchCancel:
                OnTouch(static_cast<QTouchEvent *>(ev));
                break;
            default:
                break;
            }
            return QObject::eventFilter(obj, ev);
        }

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
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            // 只记录轨迹，永不吞事件：吞 touch 会直接掐掉滚动与捏合。
            OnTouch(static_cast<QTouchEvent *>(ev));
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
        // 文本输入 / 滑块 / 滚动区自己要用完整的按下-移动-抬起，不参与仲裁
        if (IsSelfDrivenControl(w)) {
            *scopeWidget = nullptr;
            return GuardScope::None;
        }
        // 列表行：Qt 自己在 mouseReleaseEvent 里发 clicked，同样不许由滑动产生
        if (QAbstractItemView *v = AncestorInWindow<QAbstractItemView>(w)) {
            *scopeWidget = v;
            return GuardScope::Item;
        }
        // 自绘可点控件：ComicCard / SettingRow / SrModelField / 文字链……
        if (QWidget *t = TappableAncestor(w)) {
            *scopeWidget = t;
            return GuardScope::Generic;
        }
        // 浮层兜底：弹出类窗口 / 自绘浮层里的**任何**控件都参与仲裁。
        // QComboBox 弹窗容器、QMenu 的 QWidgetAction 宿主、吸顶分组条、遮罩层
        // 本身都不是 Qt 标准可点控件，但"滑过它们再抬手"同样会触发关闭浮层、
        // 选中项或提交弹窗；只列白名单必然漏。
        if (QWidget *overlay = OverlayAncestor(w)) {
            *scopeWidget = overlay;
            return GuardScope::Overlay;
        }
        if (IsPopupWindow(w)) {
            *scopeWidget = w->window();
            return GuardScope::Overlay;
        }
        *scopeWidget = nullptr;
        return GuardScope::None;
    }

    bool OnPress(QWidget *w, const QPoint &global, qint64 now)
    {
        QWidget *scopeWidget = nullptr;
        const GuardScope scope = ScopeOf(w, &scopeWidget);
        const int slop = Touch::Dp(Touch::kTapSlopDp, w);
        const bool inPopup = IsPopupContext(w);

        if (g_tapGuardLegacy) {
            // A/B 对照：修复前的任何新按下都把整条手势（含过程峰值）清零
            if (g_.active)
                Settle(g_);
            g_ = GestureState{};
            seq_ = GestureSequence{};
            pendingPopup_ = nullptr;
        }

        // ---- 手势序列续接：弹窗 / 二级子菜单补发按下 ----
        //
        // Qt 在 popup 拿到 grab、子菜单由 hover 定时器弹出时，会再补一条
        // MouseButtonPress（mouse replay）。旧实现把它当"一次全新的点击"，
        // pressGlobal / pressMs / maxMoved 全部归零：前面几十像素的滑动被洗掉，
        // 抬手只剩 5~6px 位移 → 判成点击 → 下拉框换选项 / 菜单项执行。
        // 这里不重置任何轨迹，并把整条手势标成"受污染"（必为滑动）。
        const bool midGesture = g_.active
                                && (g_.dragging || g_.contaminated || g_.maxMoved > slop
                                    || touch_.peak > slop || touch_.multi);
        // 注意：不按"距上次滑动的时间窗"续接补发按下。手势结束后的迟到 replay
        // 由冻结层（吞抬起后保持 250ms）和 OnRelease 的"无按下抬起"拦截兜住；
        // 若在这里按时间窗吞掉用户的正常按下，6.12 的 QMenu 会用残留
        // currentAction 激活那条无按下抬起 —— 表现为"按下就触发"。
        if (midGesture) {
            seq_.active = true;
            seq_.swiping = true;
            seq_.peak = qMax(seq_.peak, qMax(g_.maxMoved, touch_.peak));
            seq_.lastMs = now;
            g_.contaminated = true;
            Trace(QStringLiteral("press+ %1 scope=%2 seq=%3 popup=%4")
                      .arg(QLatin1String(w->metaObject()->className()))
                      .arg(QLatin1String(ScopeName(scope)))
                      .arg(seq_.peak).arg(int(inPopup)));
            // 弹窗里的"按下即动作"必须连按下一起吞：QComboBox 在 press 就
            // showPopup，QMenu 在 press 就 setCurrentAction + 起 20ms 子菜单
            // 定时器，自绘遮罩在 press 就关闭浮层。只吞抬起已经来不及。
            if (inPopup) {
                ++swallowed_;
                return true;
            }
            return false;
        }

        // 走到这里说明是"新的一根手指"：续接情况已在上面返回。
        // 无条件开新序列，滑完立刻点按钮 / 点菜单项才不会被上一次滑动污染。
        ThawAll();   // 上一条手势若没等到抬起，冻结层在这里兜底解开
        seq_ = GestureSequence{};
        seq_.active = true;
        seq_.lastMs = now;
        pendingPopup_ = nullptr;

        if (scope == GuardScope::None && g_.active) {
            // 绝不能复位手势状态：鼠标事件被 ignore 后会**向父控件传播**
            // （卡片不 accept → 同一条按下还会到达 QFrame、MainWindow），
            // 复位会把手心正在进行的可点手势清掉，于是抬手变成一次"没有守卫"的点击。
            return false;
        }
        // 上一条手势没等到抬起（弹窗被销毁 / 窗口切换）：先把状态清干净
        if (g_.active)
            Settle(g_);
        if (scope == GuardScope::None) {
            // 从空白处起手也要记一条"空手势"：ComicCard / SettingRow /
            // SrModelField 这类自绘控件只看 mouseReleaseEvent（不检查有没有按在自己
            // 身上），手指从空白划到它们身上再抬起，仍会 emit Activated/clicked。
            // 这条手势不复位任何视觉态，抬起时只在落点是可点控件时才吞（见 OnRelease）。
            g_ = GestureState{};
            g_.active = true;
            g_.scope = GuardScope::None;
            g_.pressWidget = w;
            g_.pressGlobal = global;
            g_.lastGlobal = global;
            g_.pressMs = now;
            g_.touchOrigin = touch_.active;
            if (!g_.touchOrigin)
                touch_ = TouchTrack{};
            bool ok = false;
            g_.scrollAtPress = ScrollFingerprint(w, &ok);
            g_.scrollKnown = ok;
            return false;
        }

        g_ = GestureState{};
        g_.active = true;
        g_.scope = scope;
        g_.pressWidget = w;
        g_.scopeWidget = scopeWidget;
        g_.list = (scope == GuardScope::PopupList) ? qobject_cast<QAbstractItemView *>(scopeWidget)
                                                   : nullptr;
        if (g_.list) {
            g_.listIndexAtPress = g_.list->currentIndex();
            g_.pressListIndex =
                g_.list->indexAt(g_.list->viewport()->mapFromGlobal(global));
        }
        if (scope == GuardScope::Menu) {
            auto *m = qobject_cast<QMenu *>(scopeWidget);
            g_.pressMenuAction = m ? m->actionAt(m->mapFromGlobal(global)) : nullptr;
        }
        // 按下即冻结弹出窗口的鼠标流入。freeze 只拦"之后"到达该窗口的事件，
        // 不影响当前这条 press；死区内的 move 不再让菜单高亮跟手/起子菜单
        // 定时器，Qt 6.12 的 release 激活条件（currentAction）就不会被改写。
        if (scope == GuardScope::Menu || scope == GuardScope::PopupList
            || scope == GuardScope::Overlay)
            FreezePopup(scopeWidget, /*skipFirstPress=*/true);
        // 触摸模式下的菜单接管：按下被吞，控制器负责按压视觉 / 松手才触发 /
        // 滑动跟手滚动与边界提示（见 touch_menu.h）。
        Touch::MenuPressController::DismissOutside(global);
        if (g_.active && g_.menuTaken) {
            // 菜单接管手势进行中出现的任何新按下（Qt popup replay、触摸合成的
            // 重复按下等）都不属于新的用户点击：吞掉，保证接管手势完整，
            // 否则这条按下会替换掉 menuTaken 手势，导致后续 move/release 全部
            // 脱离控制器（菜单交互整体失灵，实测）。
            return true;
        }
        if (scope == GuardScope::Menu && Touch::IsTouchMode()) {
            auto *m = qobject_cast<QMenu *>(scopeWidget);
            if (m && !Touch::TapGuardLegacy()
                && Touch::MenuPressController::BeginMenuPress(m, global)) {
                // 按下必须被吞：放行给 QMenu 就等于让它执行 mousePressEvent ——
                // 设 currentAction + 起 20ms 子菜单定时器，"按下即弹二级菜单"、
                // "滑动一下弹出又立刻收起"的闪现全部来自这里。吞下之后按压视觉、
                // 松手触发、滑动滚动都由控制器接管（与 Combo 分支同一写法）。
                g_.menuTaken = true;
                return true;
            }
        }
        if (scope == GuardScope::Combo && Touch::IsTouchMode()) {
            auto *cb = qobject_cast<QComboBox *>(scopeWidget);
            QAbstractItemView *openView = cb ? cb->findChild<QAbstractItemView *>() : nullptr;
            if (cb && !(openView && openView->isVisible())
                && Touch::MenuPressController::BeginComboPress(cb, global)) {
                // 按下被吞：选项框不再"按下即弹出"，松手才 showPopup，
                // 按压层画在框上，滑动放弃打开并渐隐
                g_.menuTaken = true;
                return true;
            }
            if (openView && openView->isVisible()) {
                Touch::MenuPressController::BeginListPress(openView, global);
                Touch::InstallBoundaryHints(openView);
            }
        }
        if (scope == GuardScope::PopupList && Touch::IsTouchMode()) {
            if (auto *v = qobject_cast<QAbstractItemView *>(scopeWidget)) {
                Touch::MenuPressController::BeginListPress(v, global);
                Touch::InstallBoundaryHints(v);
            }
        }
        g_.pressGlobal = global;
        g_.lastGlobal = global;
        g_.pressMs = now;
        // 触摸合成的鼠标按下：记住来源。触摸路径上 QScroller 只吃 touch，
        // 这条合成抬起可以安全吞掉；桌面鼠标路径的抬起还要给 QScroller 收尾。
        g_.touchOrigin = touch_.active;
        // 纯鼠标手势不能继承上一次触摸留下的峰值/多指标记，否则"先滑一下再点"
        // 会因为陈旧峰值把正常点击吞掉。
        if (!g_.touchOrigin)
            touch_ = TouchTrack{};
        bool ok = false;
        g_.scrollAtPress = ScrollFingerprint(w, &ok);
        g_.scrollKnown = ok;
        if (qEnvironmentVariableIsSet("JM_TAPGUARD_LOG")) {
            std::fprintf(stderr, "tapguard: press %s[%s] scope=%d touch=%d\n",
                         w->metaObject()->className(),
                         qPrintable(w->objectName().isEmpty() ? QStringLiteral("-")
                                                              : w->objectName()),
                         int(scope), int(g_.touchOrigin));
            std::fflush(stderr);
        }
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

        // 菜单/选项框接管中的手势：滑动滚动/按压渐隐由控制器处理。
        // 用 HasState 判定（menuTaken 标志可能被中间的 replay 按下清除，而
        // 控制器的按压状态仍在），双保险保证抬起/移动始终进控制器。
        if (g_.menuTaken || Touch::MenuPressController::HasState(g_.scopeWidget)) {
            const int dy = global.y() - prev.y();
            return Touch::MenuPressController::FeedMove(g_.scopeWidget, global, dy);
        }

        // 峰值取四者的最大：本帧位移、历次鼠标位移、原始触摸位移，以及
        // **本序列此前累计的峰值**（弹窗/子菜单补发按下之前的那一半滑动）。
        // 真机上 QScroller 拿下手势后控件只收到"按下 + 抬起"两条合成事件，
        // 中间没有 MouseMove —— 只看鼠标峰值会漏掉"划出去再划回来"。
        const int slop = Touch::Dp(Touch::kTapSlopDp, w);
        // 弹出类作用域的位移阈值必须对齐 QMenu 自己的激活窗口：
        // hasMouseMoved 用 QApplication::startDragDistance()（默认 10px）判定，
        // 真机上它小于 8dp（DPR>=1.25 时），滑动落在这段窗口里的抬起会直接
        // 激活 currentAction —— "按下就触发、但不是每次"的量化来源。
        const int popupSlop = qMax(QApplication::startDragDistance(), Touch::Dp(4, w));
        const int effSlop = (g_.scope == GuardScope::Menu || g_.scope == GuardScope::PopupList
                             || g_.scope == GuardScope::Combo || g_.scope == GuardScope::Overlay)
                                ? qMin(slop, popupSlop) : slop;
        const int peak = qMax(qMax(g_.maxMoved, touch_.peak), seq_.active ? seq_.peak : 0);
        if (seq_.active) {
            seq_.peak = peak;
            seq_.lastMs = now;
            if (peak > effSlop)
                seq_.swiping = true;
        }
        // Combo 弹窗打开后的任何 move 都按滑动处理：死区内的 move 会让
        // QComboBoxPrivateContainer 的过滤器把 currentIndex 跟手改写，抬起落在
        // blockMouseReleaseTimer 过期之后就直接提交 —— 这正是选项框"概率性误触"
        // 的来源（弹窗刚打开的防误提交窗口一过就必中）。弹窗没开时 move 仍留给
        // 页面补滚。
        if (!g_.dragging
            && (peak > effSlop
                || (!g_tapGuardLegacy && g_.scope == GuardScope::Combo
                    && ComboPopupOpen(g_.scopeWidget))))
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
        if (g_.dragging)
            return;
        g_.dragging = true;
        switch (g_.scope) {
        case GuardScope::Menu:
            // QMenu 的 mouseMoveEvent 会 setCurrentAction + 启动 20ms 子菜单定时器，
            // 光"清一下 activeAction"再放行等于没清：它立刻又设回来。从这里开始
            // move 一律吞掉，菜单的高亮与子菜单都不再跟着手指走。
            FreezePopup(g_.scopeWidget, /*skipFirstPress=*/false);
            ClearMenu(g_.scopeWidget);
            g_.swallowMove = true;
            break;
        case GuardScope::PopupList:
            FreezePopup(g_.list ? g_.list->window() : nullptr, false);
            // 弹窗列表自己滚（见 DragTo），move 不外泄 → 列表高亮不跟手
            g_.swallowMove = true;
            break;
        case GuardScope::Overlay:
            // 悬浮窗里的列表还要滚动，不能吞 move；但滑动过程中由 hover 定时器
            // 弹出的悬浮菜单必须立刻收起（QMenu 的 20ms 定时器不依赖新 move）。
            FreezePopup(g_.scopeWidget, false);
            ClearMenuChain();
            g_.swallowMove = false;
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
        case GuardScope::Item:
        case GuardScope::Generic:
        case GuardScope::None:
            // 不拦 move：这两类的滚动完全靠 QScroller 转发位移，
            // 吞一条 move 就从"滑动"退化成"点了不动"。
            g_.swallowMove = false;
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
        case GuardScope::Overlay:
            // 悬浮菜单可能在"按下 → 判定为滑动"的间隙里已经弹出，每次 move 收一遍
            ClearMenuChain();
            break;
        case GuardScope::Item:
        case GuardScope::Generic:
        case GuardScope::None:
            // 行/卡片不自己滚：位移交给 QScroller（它拿得到 move 就够）
            break;
        }
    }

    bool OnRelease(QWidget *w, const QPoint &global, qint64 now)
    {
        if (!g_.active) {
            // 没有对应按下的抬起落在弹出类窗口（QMenu/QComboBox 弹窗/浮层）：
            // Qt 6.12 的 QMenu 在 mouseDown 为空时不再提前返回，而是继续用
            // currentAction 激活 —— 按下被守卫吞掉（滑动中/冻结静默期内）之后，
            // 这条"无按下抬起"就是"按下就触发"的直接来源，必须吞掉。
            // 普通控件上的无按下抬起照常放行（拖拽/悬停收尾要用）。
            if (!g_tapGuardLegacy && IsPopupContext(w)) {
                ++swallowed_;
                ThawAll();
                return true;
            }
            ThawAll();
            return false;
        }
        const GestureState gs = g_;
        g_ = GestureState{};
        if (gs.menuTaken || Touch::MenuPressController::HasState(gs.scopeWidget)) {
            // 菜单/选项框接管手势：触发/弹子菜单/保持打开由控制器统一决定
            ThawAll();
            return Touch::MenuPressController::FeedRelease(gs.scopeWidget, global);
        }

        // 手势结束时触摸流可能已经结束（真机上是"合成抬起 → TouchEnd"），
        // 因此 touchOrigin 在按下时就记下来，这里不再看 touch_.active。
        const bool fromTouch = gs.touchOrigin;
        // 序列峰值必须并进来：弹窗/子菜单补发按下之前的那一半滑动，
        // 在 gs.maxMoved 里已经被清零了（gs 是补发按下之前的老状态，或
        // 补发按下之后的新状态，两者都只覆盖半段）。
        const int peak = qMax(qMax(gs.maxMoved, touch_.peak), seq_.active ? seq_.peak : 0);
        const int moved = (global - gs.pressGlobal).manhattanLength();
        const int slop = Touch::Dp(Touch::kTapSlopDp, w);
        // 弹出类作用域与 OnMove 同用 startDragDistance 阈值（见 OnMove 注释）：
        // 抬起位移一旦越过 QMenu 的激活窗口就必须按滑动处理。
        const int popupSlop = qMax(QApplication::startDragDistance(), Touch::Dp(4, w));
        const int effSlop = (gs.scope == GuardScope::Menu || gs.scope == GuardScope::PopupList
                             || gs.scope == GuardScope::Combo || gs.scope == GuardScope::Overlay)
                                ? qMin(slop, popupSlop) : slop;

        bool scrollNow = false;
        if (gs.scrollKnown && gs.pressWidget) {
            bool ok = false;
            const int v = ScrollFingerprint(gs.pressWidget, &ok);
            scrollNow = ok && v != gs.scrollAtPress;
        }
        // 本次手势期间任何滚动区发生过位移（含惯性、含 QScroller 拖出来的），
        // 都说明手指在"滑"而不是"点"。比滚动条指纹更可靠：滑出去再滑回原位时
        // 首末指纹相同，但这一条仍然是真。
        const bool scrolled = gs.pressMs > 0 && lastScrollMs_ >= gs.pressMs;
        // 惯性滚动尚未收尾时的点击一律不算数（"想停滚动"不该点进某一项）
        const bool quiet = lastScrollMs_ > 0
                           && (now - lastScrollMs_) < Touch::kTapIgnoreAfterScrollMs;
        // 长按 + 缓慢漂移同样不是点击
        const bool holdDrift = (now - gs.pressMs) > Touch::kTapHoldDriftMs
                               && peak > Touch::Dp(Touch::kTapDriftDp, w);
        // 位移判据取"过程峰值"：划出去再划回来（首末差≈0）也必须算滑动。
        // 多指（捏合/双指）永远不是点击：本应用没有双指点按语义。
        // 跨项/跨行判据：菜单与下拉列表的命中是离散的 —— 按下与抬起落在不同
        // 项上就是滑动，哪怕位移小到峰值死区里（菜单行紧凑或真机短滑时会发生，
        // 这正是"按下不触发、一开始滑动就触发菜单项"的窗口）。反过来，同一项
        // 内的 1~3px 手指抖动不算，正常点击手感不变。
        bool crossedItem = false;
        if (gs.scope == GuardScope::Menu) {
            if (auto *m = qobject_cast<QMenu *>(gs.scopeWidget))
                crossedItem = m->actionAt(m->mapFromGlobal(global)) != gs.pressMenuAction;
        } else if (gs.scope == GuardScope::PopupList && gs.list) {
            crossedItem = gs.list->indexAt(gs.list->viewport()->mapFromGlobal(global))
                          != gs.pressListIndex;
        }
        // Combo 手势（按下在框上）的抬起落在弹窗列表里：正常点选的按下就落在
        // 列表上（作用域是 PopupList），按框滑进列表再抬手必然经过滑动。
        const bool comboPopupLanded = !g_tapGuardLegacy && gs.scope == GuardScope::Combo
                                      && PopupViewOf(w) != nullptr;
        // 受污染的手势（期间弹窗出现 / 补发按下）一律按滑动算：那不是一次
        // 新的用户点击，而是 Qt 的 popup replay，不能让它提交任何选项。
        const bool drag = gs.dragging || gs.contaminated || moved > effSlop || peak > effSlop
                          || holdDrift || touch_.multi || seq_.swiping || crossedItem
                          || comboPopupLanded;

        if (!drag && !scrollNow && !scrolled && !quiet) {
            seq_ = GestureSequence{};   // 真点击：序列干净结束
            pendingPopup_ = nullptr;
            ThawAll();                  // 立即解冻：release 要原样到达菜单/控件
            return false;              // 原样交给控件，clicked/triggered 照常
        }
        seq_.active = true;
        seq_.swiping = true;
        seq_.peak = qMax(seq_.peak, peak);
        seq_.lastMs = now;

        // 空白处起手的手势：只有"抬手落在可点控件上"才有误触风险（自绘控件
        // 不看按下态）。落点也是空白时放行，免得把 QScroller 的收尾抬起吞掉，
        // 也免得把普通滚动计入 swallowed 统计。
        if (gs.scope == GuardScope::None && !gs.contaminated) {
            QWidget *releaseTarget = nullptr;
            if (ScopeOf(w, &releaseTarget) == GuardScope::None) {
                ThawAll();
                return false;
            }
        }

        // 判成滑动之后的复位：按钮解除按下态、菜单收起高亮/子菜单、
        // 下拉列表把选中项回滚。Generic/Item 没有需要复位的视觉态。
        Settle(gs);
        Trace(QStringLiteral("release %1 scope=%2 drag=%3 peak=%4 contam=%5 -> %6")
                  .arg(QLatin1String(w->metaObject()->className()))
                  .arg(QLatin1String(ScopeName(gs.scope)))
                  .arg(int(drag)).arg(peak).arg(int(gs.contaminated))
                  .arg(QLatin1String("swallow")));
        if (qEnvironmentVariableIsSet("JM_TAPGUARD_LOG")) {
            std::fprintf(stderr,
                         "tapguard: ignored %s[%s] scope=%d drag=%d peak=%d end=%d "
                         "hold=%lldms quiet=%d scroll=%d touch=%d\n",
                         w->metaObject()->className(),
                         qPrintable(w->objectName().isEmpty() ? QStringLiteral("-")
                                                              : w->objectName()),
                         int(gs.scope), int(drag), peak, moved, (long long)(now - gs.pressMs),
                         int(quiet), int(scrollNow), int(fromTouch));
            std::fflush(stderr);
        }

        // 冲突消解：QScroller 正在拖同一个容器时，桌面鼠标路径上的这条抬起
        // 是它收尾/起惯性的唯一输入 —— 吞掉会让它停在 Dragging（实测：页面
        // 只走到手指位移的一半且不再回弹）。这条路 QScroller 自己会发一条
        // 假释放把按下态取消，所以点击本来就不会发生，交还给它即可。
        // 触摸路径相反：QScroller 只吃 touch，合成抬起与它无关，必须吞。
        // 但弹窗路径不能交还：popup 里没有需要收尾的 QScroller，而 Qt 的
        // QComboBoxPrivateContainer / QMenu 会在 release 上直接提交选项。
        // 受污染的手势（弹窗 replay）尤其必须吞干净。
        const bool inPopupRelease = gs.contaminated || gs.scope == GuardScope::PopupList
                                    || gs.scope == GuardScope::Menu
                                    || gs.scope == GuardScope::Combo
                                    || gs.scope == GuardScope::Overlay;
        if (!fromTouch && !inPopupRelease && ScrollerEngaged(gs.pressWidget)) {
            ThawAll();                  // 交还 QScroller 收尾，同样要立即解冻
            return false;
        }
        ++swallowed_;
        // 吞掉抬起后冻结层再保持 250ms：Qt 对 popup 的内部转发/重放可能紧随
        // 其后（Qt 6.12 的 mouseReleaseEvent 在 mouseDown 为空时仍会激活
        // currentAction），立即解冻会让同轮的漏网事件直达菜单。
        QTimer::singleShot(250, this, [this] { ThawAll(); });
        return true;
    }

    // 从 w 往上找滚动区，看它的 QScroller 是否正在拖（Dragging/Scrolling）。
    static bool ScrollerEngaged(QWidget *w)
    {
        for (QWidget *p = w; p; p = p->parentWidget()) {
            auto *area = qobject_cast<QAbstractScrollArea *>(p);
            if (!area)
                continue;
            if (QScroller *sc = QScroller::scroller(area)) {
                if (sc->state() != QScroller::Inactive)
                    return true;
            }
        }
        return false;
    }

    // 触摸流：只维护轨迹（峰值 / 多指 / 来源），不参与"点击"判定，也从不吞事件。
    //
    // 为什么必须单独记：真机上 QScroller 拿下手势后，手指下的控件只收到两条
    // 合成鼠标事件（按下在最开始、抬起在**最后位置**），中间一条 MouseMove
    // 都没有。此时若只看鼠标位移，"划出去再划回来"会被判成点击 —— 而它正是
    // 用户最容易做出来的手势（先滑一半发现不对再滑回来）。
    //
    // 也不能在这里自己判点击：触摸事件的目标是窗口/滚动区而不是手指下的控件
    // （控件没开 WA_AcceptTouchEvents），在这里初始化手势会把作用域判成"不可点"。
    bool OnTouch(QTouchEvent *te)
    {
        const QList<QEventPoint> pts = te->points();
        switch (te->type()) {
        case QEvent::TouchBegin: {
            touch_ = TouchTrack{};
            for (const QEventPoint &p : pts) {
                if (p.state() == QEventPoint::State::Pressed) {
                    touch_.id = p.id();
                    touch_.pressGlobal = p.globalPosition().toPoint();
                    touch_.lastGlobal = touch_.pressGlobal;
                    break;
                }
            }
            if (touch_.id < 0 && !pts.isEmpty()) {
                touch_.id = pts.first().id();
                touch_.pressGlobal = pts.first().globalPosition().toPoint();
                touch_.lastGlobal = touch_.pressGlobal;
            }
            touch_.active = touch_.id >= 0;
            break;
        }
        case QEvent::TouchUpdate: {
            if (!touch_.active)
                break;
            // 多指：捏合/双指划动一律按"非点击"处理
            if (pts.size() > 1)
                touch_.multi = true;
            for (const QEventPoint &p : pts) {
                if (p.id() != touch_.id)
                    continue;
                touch_.lastGlobal = p.globalPosition().toPoint();
                touch_.peak = qMax(touch_.peak,
                                   (touch_.lastGlobal - touch_.pressGlobal).manhattanLength());
            }
            break;
        }
        case QEvent::TouchEnd:
        case QEvent::TouchCancel:
            // 不清 multi/peak：紧随其后的合成抬起还要用它们判定。
            // 状态在下一根手指按下时整体重置。
            touch_.active = false;
            break;
        default:
            break;
        }
        return false;
    }

    void OnShow(QWidget *w)
    {
        if (!w || g_tapGuardLegacy)
            return;
        // 菜单接管手势期间弹出的子菜单（按下时 20ms 定时器触发）立即收起：
        // "按下不触发子菜单弹出"，弹出动作延迟到松手（见 FeedRelease）。
        if (g_.menuTaken) {
            if (auto *m = qobject_cast<QMenu *>(w))
                if (qobject_cast<QMenu *>(m->parentWidget()))
                    QTimer::singleShot(0, m, [m] { if (m) m->hide(); });
            return;
        }
        // Combo 弹窗无论怎么打开都冻结 move：死区滑动会让
        // QComboBoxPrivateContainer 的过滤器把 currentIndex 跟手改写，抬起落在
        // blockMouseReleaseTimer 过期后直接提交 —— 选项框"概率性误触"的来源。
        // 用户随后点列表项的按下由 PopupList 作用域的 armSkip 放行。
        if (!g_tapGuardLegacy && g_.scope == GuardScope::Combo && w->isWindow()
            && w->parentWidget()
            && qobject_cast<QComboBox *>(w->parentWidget()))
            FreezePopup(w, /*skipFirstPress=*/false);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        // 只在"手势还没抬手"时算滑动期间。手势结束之后用户主动点开的弹窗
        // （滑完立刻点下拉框选一项）必须正常打开，不能被当成误触收起。
        const bool swiping = g_.active
                             && (g_.dragging || g_.contaminated || seq_.swiping);
        if (!swiping)
            return;

        // 滑动期间"新出现"的弹窗/浮层不是用户主动打开的：标成污染，后续
        // 它补发的按下按同一序列处理（OnPress 的续接分支）。
        const bool popup = IsPopupWindow(w) || w->property("tapGuardOverlay").toBool();
        if (popup && w->isWindow()) {
            g_.contaminated = true;
            seq_.active = true;
            seq_.swiping = true;
            seq_.lastMs = now;
            pendingPopup_ = w->window();
            FreezePopup(w, /*skipFirstPress=*/false);
            Trace(QStringLiteral("show %1 seq=%2 -> contaminated")
                      .arg(QLatin1String(w->metaObject()->className())).arg(seq_.peak));
        }

        // 二级子菜单：QMenu 的 20ms hover 定时器可能在"按下 → 判明滑动"的
        // 间隙里弹出。它不依赖新的 move，只能在这里收。
        if (auto *m = qobject_cast<QMenu *>(w)) {
            if (qobject_cast<QMenu *>(m->parentWidget()))
                QTimer::singleShot(0, m, [m] { if (m) m->hide(); });
            ClearMenuChain();
        }

        // QComboBox 弹窗可能比"按下"晚 50~150ms 才出现，正好落在手指已经
        // 开始滑动之后。旧逻辑只在 scope==Combo 时收；现在覆盖"从框上方滑进来"
        // 的情形：那时的起手作用域是 None，弹窗同样要收。
        QComboBox *c = qobject_cast<QComboBox *>(w->parentWidget());
        if (!c && g_.scope == GuardScope::Combo)
            c = qobject_cast<QComboBox *>(g_.scopeWidget);
        if (c && w->isWindow()) {
            QPointer<QComboBox> keep = c;
            QTimer::singleShot(0, keep, [keep] {
                if (keep)
                    keep->hidePopup();
            });
        }
    }

    // 清整条菜单链：主菜单的 activeAction + 所有可见的二级/三级子菜单。
    // 只清"按下时那一个 QMenu"是不够的 —— 抬手可能落在刚弹出的子菜单上，
    // 它自己的 activeAction / hover 定时器还在，照样会 triggered。
    static void ClearMenuChain()
    {
        const QList<QWidget *> tops = QApplication::topLevelWidgets();
        for (QWidget *top : tops) {
            auto *m = qobject_cast<QMenu *>(top);
            if (!m || !m->isVisible())
                continue;
            m->setActiveAction(nullptr);
            if (qobject_cast<QMenu *>(m->parentWidget()))
                m->hide();      // 只收子菜单，用户主动打开的主菜单保持可见
        }
    }

    static void ClearMenu(QWidget *w)
    {
        ClearMenuChain();
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
        if (gs.menuTaken)
            return;   // 接管手势没有按钮按下态/菜单高亮需要复位，子菜单保持打开
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
                    Touch::InstallBoundaryHints(area);
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

    // 最近若干条手势判定：--tapguard-watch 落盘，真机/模拟器上可以直接看到
    // "哪条按下被吞、峰值多少、抬起落在哪个 scope"，不用依赖会整段丢的 logcat。
    void Trace(const QString &line)
    {
        if (trace_.size() >= 24)
            trace_.removeFirst();
        trace_ << line;
    }

public:
    QString traceText() const { return trace_.join(QLatin1String("\n")); }
    static int ScopeValue(QWidget *w)
    {
        QWidget *scopeWidget = nullptr;
        return int(ScopeOf(w, &scopeWidget));
    }

private:
    bool installed_ = false;
    int swallowed_ = 0;
    GestureState g_;
    GestureSequence seq_;
    TouchTrack touch_;
    QPointer<QWidget> pendingPopup_;
    QHash<QWidget *, QPointer<PopupFreeze>> frozen_;
    QStringList trace_;

    // 给弹出窗口（QMenu / QComboBox 弹窗容器 / 自绘浮层）及其全部子控件
    // 装对象级冻结过滤器。对象级过滤器在 QWidget::event() 之前执行，且
    // installEventFilter 是 prepend（后装先跑），所以能挡在 Qt 自己的
    // QComboBoxPrivateContainer::eventFilter 与 QMenu 内部转发之前。
    void FreezePopup(QWidget *w, bool skipFirstPress)
    {
        if (!w)
            return;
        QWidget *win = w->window();
        if (!win)
            return;
        QWidgetList targets;
        targets << win << w;
        const QWidgetList kids = win->findChildren<QWidget *>();
        for (QWidget *k : kids)
            targets << k;
        for (QWidget *t : targets) {
            if (!t)
                continue;
            auto it = frozen_.find(t);
            if (it != frozen_.end())
                continue;   // 已有冻结层（只吞 move），无需重复安装
            auto *f = new PopupFreeze(t, skipFirstPress);
            t->installEventFilter(f);
            frozen_.insert(t, f);
            // 兜底：手势没等到抬起（弹窗被销毁 / 窗口切换）也要解冻；
            // 4s 覆盖最慢的滑动序列，正常手势结束后 ThawAll 会提前解开。
            QPointer<QWidget> keep = t;
            QTimer::singleShot(4000, this, [this, keep] {
                if (keep)
                    ThawOne(keep);
            });
        }
    }

    void ThawOne(QWidget *t)
    {
        auto it = frozen_.find(t);
        if (it == frozen_.end())
            return;
        if (it.value())
            t->removeEventFilter(it.value());
        if (it.value())
            it.value()->deleteLater();
        frozen_.erase(it);
    }

    void ThawAll()
    {
        const QList<QWidget *> keys = frozen_.keys();
        for (QWidget *t : keys)
            ThawOne(t);
    }
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

void SetTapGuardLegacy(bool on)
{
    g_tapGuardLegacy = on;
}

bool TapGuardLegacy()
{
    return g_tapGuardLegacy;
}

QString TapGuardTrace()
{
    return TapGuard::I()->traceText();
}

int TapGuardScopeOf(QWidget *w)
{
    return w ? TapGuard::I()->ScopeValue(w) : 0;
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
        // 起手点落在左边缘的可点控件上（返回按钮、抽屉首项）：这一串事件
        // 从判定为返回手势起就被本适配器吞掉，控件收不到抬起。这里主动解除
        // 按下态，否则按钮会一直停在"按下"高亮上，看起来像卡住。
        if (auto *btn = qobject_cast<QAbstractButton *>(pressWidget_))
            btn->setDown(false);
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
        pressWidget_ = w;
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
        pressWidget_ = w;
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
