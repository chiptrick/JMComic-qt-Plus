// 菜单与下拉弹窗的触摸交互层实现（设计说明见 touch_menu.h）。
#include "touch_menu.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFont>
#include <QKeyEvent>
#include <QHash>
#include <QImage>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QScrollBar>
#include <QScroller>
#include <QTimer>
#include <QWheelEvent>

#include <iterator>
#include <cstdio>

#include "motion.h"
#include "theme.h"
#include "touch.h"

namespace Touch {
namespace {

// 按压反馈层：按住时在按下项上铺一层"薄雾 + 强调色描边"，滑动开始后渐隐消失。
// 放在弹出窗口（QMenu 本体 / QComboBox 弹窗容器）上而不是 viewport 上，
// 避免落在滚动区内部（QScrollArea 里用半透明子控件会被容器重绘盖掉）。
//
// 配色为什么不能直接铺强调色（2026-11-16 修）：
//   QMenu / QComboBox 的项在六套主题里是两种极端配色 —— 深色主题普通项是
//   #ffffff 白字压在 #232629 深底上，选中项却是 #000000 黑字压在 #ff79b0 /
//   #FFA94D / #6effe8 亮底上；浅色主题反过来是 #3c3c3c / #555555 深字亮底。
//   以前固定画"强调色 alpha=56"，等于在底色上压一块深色：黑字的亮底被压暗、
//   黑字就糊进背景（用户实测"黑色文字被黑色背景遮盖"），浅色主题则几乎看不见。
//   这里改成两条互不干扰的通道：
//     1) 底：先 grab 按下瞬间宿主在该矩形里的真实像素当底图（同 RippleLayer 的
//        做法：不依赖"半透明子控件能不能和父控件合成"，避免某些容器把整块画成
//        不透明深色）。底图保证文字原样保留。
//     2) 雾：**只加深、不提亮**，强度按底色明暗分两档 ——
//        · 深底（<128）：黑雾 alpha 64，把深底压得更深，白字对比度反而升高；
//        · 亮底（>=128）：黑雾 alpha 30，只把亮底压成浅灰（白底 → #e1e1e1），
//          深色文字（#202124 / #3c3c3c / 纯黑）对比度仍在 10:1 以上，肉眼清晰。
//        统一用黑色雾是刻意的：任何主题的项底色都是"深底浅字"或"亮底深字"，
//        加深永远把底色推离浅色文字、且对深色文字的削弱极小 —— 不会出现"提亮
//        亮底 = 白雾糊在白底上完全看不见"的反向翻车（浅色主题实测）。
//        真正会把黑字盖死的是**不透明**底色（见下面 setStyleSheet 那段），
//        只要雾是半透明且底图保留文字像素，六套主题都不会糊字。
//     3) 描边：强调色细边，提供品牌色与按压存在感，不覆盖文字像素。
class PressOverlay : public QWidget
{
public:
    PressOverlay(QWidget *host, const QRect &rc, const QColor &accent)
        : QWidget(host), accent_(accent)
    {
        setObjectName(QStringLiteral("TapMenuPressOverlay"));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        // 两层都要断掉，缺一不可：
        //   · WA_NoSystemBackground：别让 Qt 用调色板 Window 色擦背景；
        //   · setStyleSheet(transparent)：菜单若自己 setStyleSheet 过（阅读器菜单
        //     StyleReaderMenu 就是），子树会退回 qApp 全局 QSS 的
        //     "QWidget { background-color: #31363b; }" —— 那是一条不透明深灰，
        //     直接盖在项上，"黑字被黑底遮住"就是这么来的（用户实测截图）。
        setAttribute(Qt::WA_NoSystemBackground, true);
        setStyleSheet(QStringLiteral("background: transparent;"));
        if (host && rc.isValid())
            base_ = host->grab(rc);
        darken_ = PreferDarken();
        setGeometry(rc);
        raise();
        show();
        Motion::FadeIn(this, Motion::Fast);
    }

    void FadeAway()
    {
        if (fading_)
            return;
        fading_ = true;
        Motion::FadeOut(this, Motion::Base, [this] { deleteLater(); });
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QRectF inner = QRectF(rect()).adjusted(1, 1, -1, -1);
        QPainterPath clip;
        UiStyle::AddPanel(clip, inner);
        p.setClipPath(clip);
        // grab() 返回的是**设备像素**尺寸的 pixmap（DPR 2 时是控件尺寸的两倍），
        // 直接 drawPixmap(0,0,...) 只会画出左上角四分之一。必须按控件矩形缩放，
        // 否则按压层看起来"只剩一块怪色块"（实测）。
        if (!base_.isNull())
            p.drawPixmap(rect(), base_, base_.rect());
        // 薄雾：只加深不提亮，强度按底色明暗分两档（见类注释）。
        QColor veil(0, 0, 0);
        veil.setAlpha(darken_ ? 64 : 30);
        p.fillRect(inner, veil);
        p.setClipping(false);
        // 强调色描边：按压存在感，全在边缘，不碰文字像素。
        QColor line = accent_;
        line.setAlpha(darken_ ? 170 : 130);
        QPen pen(line);
        pen.setWidthF(qMax(1.0, qreal(UiStyle::Fpx(1))));
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        UiStyle::DrawPanel(&p, inner);
    }

private:
    static qreal Lum(const QColor &c)
    {
        return 0.2126 * c.red() + 0.7152 * c.green() + 0.0722 * c.blue();   // Rec.709，0~255
    }

    // 是否算"深底"。取底图**面积最大**的那一桶（16 桶亮度直方图的众数）——
    // 底色永远占项内面积的大头，文字只占几个百分点，所以众数就是底色。
    // 深底用重雾（alpha 64）把底压得更深；亮底用轻雾（alpha 30）只压成浅灰。
    bool PreferDarken() const
    {
        if (base_.isNull())
            return UiTheme::I().IsDark();   // 无底图：退回主题明暗
        const QImage img = base_.toImage();
        if (img.isNull())
            return UiTheme::I().IsDark();
        int hist[16] = {0};
        for (int y = 0; y < img.height(); y += 2) {
            for (int x = 0; x < img.width(); x += 2) {
                const int b = qBound(0, int(Lum(QColor::fromRgba(img.pixel(x, y))) / 16.0), 15);
                ++hist[b];
            }
        }
        int mode = 0;
        for (int i = 1; i < 16; ++i)
            if (hist[i] > hist[mode])
                mode = i;
        return mode < 8;   // 众数桶落在暗半边 = 深底
    }

    QColor accent_;
    QPixmap base_;
    bool darken_ = true;   // true = 深底（重雾 64）；false = 亮底（轻雾 30）
    bool fading_ = false;
};

// 边界提示条：半透明深色圆角条 + 白字，出现在宿主内侧顶部/底部，约 1s 淡出。
class BoundaryBar : public QWidget
{
public:
    BoundaryBar(QWidget *host, bool top, const QString &text) : QWidget(host)
    {
        setObjectName(QStringLiteral("TapBoundaryHint"));
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        text_ = text.isEmpty() ? (top ? QStringLiteral("已到顶部")
                                      : QStringLiteral("已到底部"))
                               : text;
        const int w = host->width() - UiStyle::Fpx(48);
        const int h = UiStyle::Fpx(30);
        const int x = qMax(UiStyle::Fpx(8), (host->width() - w) / 2);
        const int y = top ? UiStyle::Fpx(10)
                          : host->height() - UiStyle::Fpx(10) - h;
        setGeometry(x, y, w, h);
        raise();
        show();
        Motion::FadeIn(this, Motion::Fast);
        QTimer::singleShot(850, this, [this] {
            Motion::FadeOut(this, Motion::Fast, [this] { deleteLater(); });
        });
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 168));
        p.drawRoundedRect(rect(), UiStyle::Fpx(8), UiStyle::Fpx(8));
        QFont f;
        f.setPixelSize(UiStyle::Fpx(12));
        f.setBold(true);
        p.setFont(f);
        p.setPen(QColor(255, 255, 255, 236));
        p.drawText(rect(), Qt::AlignCenter, text_);
    }

private:
    QString text_;
};

// 每个弹出窗口一份手势状态。
enum class Kind { Menu, Combo, List };

struct MenuState
{
    Kind kind = Kind::Menu;
    QPointer<PressOverlay> overlay;
    QPointer<QAction> pressAction;     // QMenu：按下时手指下的项
    QPointer<QComboBox> combo;   // QComboBox 按下接管
    QPersistentModelIndex pressIndex;  // 下拉弹窗：按下时手指下的行
    QPoint pressGlobal;
    qint64 pressMs = 0;
    int wheelAcc = 0;                  // 未结算的滚轮位移（QMenu 跟手滚动）
    bool swiping = false;
    QPointer<QWidget> popup;
};

QHash<QWidget *, MenuState> &States()
{
    static QHash<QWidget *, MenuState> s;
    return s;
}

void CleanupStates()
{
    auto &s = States();
    for (auto it = s.begin(); it != s.end();) {
        if (!it->popup)
            it = s.erase(it);
        else
            ++it;
    }
}

MenuState *StateOf(QWidget *popup)
{
    auto it = States().find(popup);
    return it == States().end() ? nullptr : &it.value();
}

QColor AccentColor()
{
    const QColor c = UiTheme::I().Tokens().accent;
    return c.isValid() ? c : QColor(0xd0, 0x60, 0x70);
}

QFont HintFont()
{
    QFont f;
    f.setPixelSize(UiStyle::Fpx(12));
    f.setBold(true);
    return f;
}

void CloseMenuChain(QMenu *menu)
{
    const QList<QWidget *> tops = QApplication::topLevelWidgets();
    for (QWidget *top : tops) {
        if (auto *m = qobject_cast<QMenu *>(top)) {
            if (m->isVisible()) {
                // 关闭时清 activeAction：残留的 currentAction 会让下一次
                // QMenu::event(Show) 执行 popupAction，菜单打开 250ms 后
                // 自己弹出子菜单（凭空弹出）。
                m->setActiveAction(nullptr);
                m->close();
            }
        }
    }
    if (menu) {
        menu->setActiveAction(nullptr);
        menu->close();
    }
}

// QMenu 滚动后的顶/底检测：第一项完整可见=已到顶，末项完整可见=已到底。
// dy 是本帧手指 y 位移：上滑（dy<0）看上面 → 顶部提示；下滑（dy>0）→ 底部提示。
void CheckMenuBounds(QMenu *menu, int dy)
{
    const QList<QAction *> acts = menu->actions();
    if (acts.isEmpty())
        return;
    const QRect first = menu->actionGeometry(acts.first());
    const QRect lastG = menu->actionGeometry(acts.last());
    if (dy < 0 && first.top() >= 0)
        ShowBoundaryHint(menu, true);
    else if (dy > 0 && lastG.bottom() <= menu->height())
        ShowBoundaryHint(menu, false);
}

// 下拉弹窗滚动的顶/底检测（语义与上面一致）。
void CheckListBounds(QAbstractItemView *view, int dy)
{
    QScrollBar *sb = view->verticalScrollBar();
    if (!sb)
        return;
    if (dy < 0 && sb->value() <= sb->minimum())
        ShowBoundaryHint(view->window(), true);
    else if (dy > 0 && sb->value() >= sb->maximum())
        ShowBoundaryHint(view->window(), false);
}

// QMenu 跟手滚动：把累计位移换算成滚轮格（QMenu::wheelEvent → 内部 scrollMenu，
// 每格滚动一行）。手指下滑 = 看更下面的内容 = 滚轮向下（angleDelta.y()<0）。
void FeedMenuWheel(QMenu *menu, int dy, const QPoint &global)
{
    auto it = States().find(menu->window());
    if (it == States().end())
        return;
    MenuState &st = it.value();
    st.wheelAcc += dy;
    const int step = qMax(UiStyle::Fpx(14), 10);
    while (qAbs(st.wheelAcc) >= step) {
        const int dir = st.wheelAcc > 0 ? -1 : +1;
        const QPoint pos = menu->mapFromGlobal(global);
        QWheelEvent we(pos, QPointF(global), QPoint(), QPoint(0, dir * 120),
                       Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(menu, &we);
        st.wheelAcc -= (st.wheelAcc > 0 ? step : -step);
    }
}

}  // namespace

void ShowBoundaryHint(QWidget *host, bool top, const QString &text)
{
    if (!host)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const char *key = top ? "bhintTopMs" : "bhintBottomMs";
    const qint64 last = host->property(key).toLongLong();
    if (now - last < 800)
        return;
    host->setProperty(key, now);
    new BoundaryBar(host, top, text);
}

void InstallBoundaryHints(QAbstractScrollArea *area)
{
    if (!area || area->property("bhintInstalled").toBool())
        return;
    area->setProperty("bhintInstalled", true);
    QScrollBar *vbar = area->verticalScrollBar();
    if (!vbar)
        return;
    QObject::connect(vbar, &QScrollBar::valueChanged, vbar, [area, vbar](int v) {
        QScroller *sc = QScroller::scroller(area);
        if (!sc || sc->state() == QScroller::Inactive) {
            vbar->setProperty("bhintLastV", v);
            return;
        }
        const int last = vbar->property("bhintLastV").toInt();
        if (v <= vbar->minimum() && last > v)
            ShowBoundaryHint(area->viewport(), true);
        else if (v >= vbar->maximum() && last < v)
            ShowBoundaryHint(area->viewport(), false);
        vbar->setProperty("bhintLastV", v);
    });
}

bool MenuPressController::BeginMenuPress(QMenu *menu, const QPoint &global)
{
    if (!menu || !menu->isVisible() || TapGuardLegacy())
        return false;
    CleanupStates();
    MenuState &st = States()[menu];
    st.kind = Kind::Menu;
    st.popup = menu;
    st.pressGlobal = global;
    st.pressMs = QDateTime::currentMSecsSinceEpoch();
    st.wheelAcc = 0;
    st.swiping = false;
    st.pressIndex = QPersistentModelIndex();
    QAction *a = menu->actionAt(menu->mapFromGlobal(global));
    st.pressAction = a;
    // **吞下这次按下**：QMenu 收不到 press 就不会起 20ms 子菜单定时器、不会
    // 高亮跟手 —— "按下触发二级菜单弹出"从源头消失，且按压层成为唯一的
    // 按压视觉。顺带清掉上一次手势遗留的 activeAction：QMenu::event(Show) 会
    // 对残留的 currentAction 执行 popupAction（250ms 延迟弹子菜单），不清会让
    // 上一轮的子菜单在本次按下后凭空弹出。
    menu->setActiveAction(nullptr);
    if (!a || !a->isEnabled() || a->isSeparator())
        return true;   // 无可按压项：不画按压层，按下仍被吞（抬起统一关菜单）
    delete st.overlay;
    st.overlay = new PressOverlay(menu, menu->actionGeometry(a), AccentColor());
    return true;
}

bool MenuPressController::BeginComboPress(QComboBox *combo, const QPoint &global)
{
    if (!combo || !combo->isVisible() || TapGuardLegacy())
        return false;
    QAbstractItemView *openView = combo->findChild<QAbstractItemView *>();
    if (openView && openView->isVisible())
        return false;   // 弹窗已开（不应发生），走 PopupList 逻辑
    CleanupStates();
    MenuState &st = States()[combo];
    st.kind = Kind::Combo;
    st.popup = combo;
    st.combo = combo;
    st.pressGlobal = global;
    st.pressMs = QDateTime::currentMSecsSinceEpoch();
    st.wheelAcc = 0;
    st.swiping = false;
    st.pressAction = nullptr;
    st.pressIndex = QPersistentModelIndex();
    // 按压层画在框本体上：按住即显示半透明按压效果
    delete st.overlay;
    st.overlay = new PressOverlay(combo, combo->rect().adjusted(0, 0, -1, -1),
                                  AccentColor());
    return true;   // 按下被吞：QComboBox::mousePressEvent 不执行，选项框不再按下即弹
}

void MenuPressController::DismissOutside(const QPoint &global)
{
    if (TapGuardLegacy())
        return;
    CleanupStates();
    auto &s = States();
    for (auto it = s.begin(); it != s.end();) {
        if (!it->popup) {
            it = s.erase(it);
            continue;
        }
        if (auto *menu = qobject_cast<QMenu *>(it->popup)) {
            if (menu->isVisible()
                && !menu->rect().contains(menu->mapFromGlobal(global))) {
                if (it->overlay)
                    it->overlay->FadeAway();
                it = s.erase(it);
                menu->close();
                continue;
            }
        }
        ++it;
    }
}

bool MenuPressController::BeginListPress(QAbstractItemView *view, const QPoint &global)
{
    if (!view || !view->isVisible() || TapGuardLegacy())
        return false;
    CleanupStates();
    MenuState &st = States()[view->window()];
    st.kind = Kind::List;
    st.popup = view->window();
    st.pressGlobal = global;
    st.pressMs = QDateTime::currentMSecsSinceEpoch();
    st.wheelAcc = 0;
    st.swiping = false;
    st.pressAction = nullptr;
    const QModelIndex idx = view->indexAt(view->viewport()->mapFromGlobal(global));
    st.pressIndex = idx;
    if (idx.isValid() && (view->model()->flags(idx) & Qt::ItemIsEnabled)) {
        delete st.overlay;
        const QRect rc = view->visualRect(idx);
        const QPoint tl = view->viewport()->mapTo(st.popup, rc.topLeft());
        st.overlay = new PressOverlay(st.popup, QRect(tl, rc.size()), AccentColor());
    }
    return false;   // 按下放行：QListView 需要 setCurrentIndex 才能提交点选
}

bool MenuPressController::FeedMove(QWidget *popup, const QPoint &global, int dy)
{
    MenuState *stp = StateOf(popup);
    if (!stp || !stp->popup)
        return false;
    MenuState &st = *stp;
    if (st.kind == Kind::Combo) {
        // 选项框按下接管：按压中移动 = 放弃打开，按压层渐隐；弹窗未开，
        // 无滚动/边界诉求，move 一律吞（防穿透到框下方内容）。
        if (!st.swiping && (global - st.pressGlobal).manhattanLength()
            > QApplication::startDragDistance()) {
            st.swiping = true;
            if (st.overlay)
                st.overlay->FadeAway();
        }
        return true;
    }
    if (!st.swiping) {
        const int moved = (global - st.pressGlobal).manhattanLength();
        if (moved <= QApplication::startDragDistance())
            return st.kind == Kind::Menu;   // 死区内：菜单吞（无副作用），弹窗放行
        st.swiping = true;
        if (st.overlay)
            st.overlay->FadeAway();   // 按压效果随滑动逐渐消失
    }
    if (st.kind == Kind::List) {
        // 下拉弹窗：滚动由 DragTo 负责，这里只做边界提示
        if (auto *v = qobject_cast<QAbstractItemView *>(popup))
            CheckListBounds(v, dy);
        return false;
    }
    auto *menu = qobject_cast<QMenu *>(st.popup);
    if (!menu)
        return true;
    FeedMenuWheel(menu, dy, global);
    CheckMenuBounds(menu, dy);
    return true;
}

bool MenuPressController::FeedRelease(QWidget *popup, const QPoint &global)
{
    MenuState *stp = StateOf(popup);
    if (!stp || !stp->popup)
        return false;
    // 先把需要的字段拷出来：erase 会让 st 引用失效，之后一律不碰 st
    const QPointer<PressOverlay> overlay = stp->overlay;
    const bool wasSwiping = stp->swiping;
    const Kind kind = stp->kind;
    const QPointer<QAction> pressed = stp->pressAction;
    QPointer<QWidget> popupKeep = stp->popup;
    QPointer<QComboBox> comboKeep = stp->combo;
    const QPoint pressGlobal = stp->pressGlobal;
    const qint64 pressMs = stp->pressMs;
    States().erase(States().find(popup));
    // 长按 + 缓慢漂移（位移在 startDragDistance 死区内、按住超过 500ms）与
    // OnRelease 的 holdDrift 判据同源：这是"慢慢拖列表"不是点击。
    const qint64 held = QDateTime::currentMSecsSinceEpoch() - pressMs;
    const bool holdDrift = held > Touch::kTapHoldDriftMs
                           && (global - pressGlobal).manhattanLength()
                                  > Touch::kTapDriftDp;
    const bool swipeLike = wasSwiping || holdDrift;
    if (overlay)
        overlay->FadeAway();
    if (kind == Kind::Combo) {
        // 选项框：松手（无滑动）才弹出；滑动了就不弹（按压层已渐隐）
        if (comboKeep && !swipeLike)
            comboKeep->showPopup();
        return true;
    }
    if (kind == Kind::List)
        return false;   // 弹窗列表：抬起交给 Qt/守卫
    auto *menu = qobject_cast<QMenu *>(popupKeep);
    if (!menu)
        return false;
    if (swipeLike)
        return true;    // 滑动/长按漂移：不触发任何项，菜单保持打开
    if (!menu->isVisible())
        return true;
    // 抬起后不再保留高亮：接管模式下 QMenu 的 activeAction 由控制器维护，
    // 残留会让下一次 QMenu::event(Show) 自动弹出子菜单（凭空弹出）
    menu->setActiveAction(nullptr);
    // 抬起落在菜单矩形外：关闭整条菜单链（接管后 Qt 的"rect 外按下关闭"失效，
    // 这里补齐）
    if (!menu->rect().contains(menu->mapFromGlobal(global))) {
        CloseMenuChain(menu);
        return true;
    }
    if (!pressed)
        return true;    // 空白处按下：抬起不激活（与 QMenu 原语义一致）
    QAction *at = menu->actionAt(menu->mapFromGlobal(global));
    if (at != pressed)
        return true;    // 抬手不在按下项上：不触发
    if (!pressed->isEnabled() || pressed->isSeparator())
        return true;
    if (pressed->menu()) {
        // 子菜单项：松手才弹出。不手动 popup —— 用 Enter 键事件走 Qt 内部的
        // popupAction/showSubMenu 路径，causedPopup 链与滚动状态都保持一致
        //（手动 sub->popup 在"收起后立即重弹"时会崩，实测）。
        menu->setActiveAction(pressed);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Enter, Qt::NoModifier);
        QCoreApplication::sendEvent(menu, &enter);
        return true;
    }
    // 普通项：先关菜单链再触发（trigger 可能同步打开页面/弹对话框）
    CloseMenuChain(menu);
    pressed->trigger();
    return true;
}

bool MenuPressController::HasState(QWidget *popup)
{
    return popup && States().contains(popup);
}

void MenuPressController::Cancel(QWidget *popup)
{
    auto it = States().find(popup);
    if (it == States().end())
        return;
    if (it->overlay)
        it->overlay->FadeAway();
    States().erase(it);
}

}  // namespace Touch
