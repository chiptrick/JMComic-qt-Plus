// 触摸适配层（Android / 触屏设备）。
//
// 目标：让同一套 Widget 界面在"鼠标"和"手指"两种输入下都成立，而不需要维护两份
// 页面代码。Python 版把这件事写成了 8,309 行 mobile_*.py（返回键、抽屉、分屏、
// 输入法、点击热区、横滑拦截…），这里按"组件化 + 可测"重做，只保留真正需要的：
//
//   1. 边缘右滑返回（含过程反馈）
//   2. 滚动容器的手势滚动（QScroller，触摸模式才接管）
//   3. 命中目标下限（44dp）
//   4. 输入法弹出时把焦点控件滚进可视区
//   5. 安全区（状态栏 / 刘海 / 手势条）内边距
//   6. 返回键（Qt::Key_Back）
//
// 可测性：桌面没有触摸屏，用 --touch 强制打开触摸模式；边缘手势用鼠标也能走
// 同一条代码路径（只在触摸模式下生效，避免和桌面拖拽冲突），因此桌面就能验收。
#pragma once

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QString>

class QWidget;
class QAbstractScrollArea;

namespace Touch {

// 触摸模式：桌面默认关闭（保留 hover / 滚轮 / 精确点击语义），
// Android 或 --touch 时打开。
bool IsTouchMode();
void SetTouchMode(bool on);

// 是否运行在手机平台（编译期判定，不受 --touch 影响）。
// 主窗口用它决定"上机即手机形态"：隐藏自绘标题栏 + 抽屉导航 + 全屏尺寸。
bool IsMobile();

// dp → 逻辑像素（按窗口 DPR 取整，最小 1）
int Dp(int v, const QWidget *ctx = nullptr);

// 安全区。桌面恒为 0；Android 只取左右/顶部（刘海、挖孔），
// **底部手势导航条不再预留**（内容 edge-to-edge，避免多出一条 78dp 空白带）。
struct Insets {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool IsZero() const { return left == 0 && top == 0 && right == 0 && bottom == 0; }
    QString ToString() const
    {
        return QStringLiteral("%1,%2,%3,%4").arg(left).arg(top).arg(right).arg(bottom);
    }
};

Insets SafeArea();
// 测试与厂商 ROM 兜底：JM_SAFE_INSETS="l,t,r,b"
void SetSafeAreaOverride(const Insets &in);

// 递归给所有 QAbstractScrollArea 装手势滚动（触摸模式才装）
void InstallScrollGestures(QWidget *root);

// 关掉某个滚动区的横向平移（纵向页面专用）。
//
// 为什么需要：QScrollArea 即使把横向滚动条策略设为 AlwaysOff，只要内容比视口宽，
// 横向滚动范围仍然非 0，QScroller 照样能左右拖动 —— 表现为手机上"页面能左右滑"，
// 而且滑完不会回位（内容被推偏）。设置页/详情页这类纵向页面必须关掉。
void LockHorizontalPan(QAbstractScrollArea *area);

// 命中目标下限：把小于 44dp 的按钮放大（只在触摸模式下动，桌面布局不受影响）
// 返回被调整的控件数量（0 表示本来就是合规的）
int PolishHitTargets(QWidget *root);

// ---- 点击防误触（滑动 vs 点击）----
//
// 现象：手指在任意可交互区域（侧栏、卡片、设置整行、下拉选项、菜单项、列表行）
// 上或附近滑动时，松手会被当成点击 —— 跳页、拨开关、换选项、执行菜单项。
//
// 根因（真机 + 桌面实测）：
//   * 控件的点击语义只有一条规则"抬起点落在我的矩形内 → clicked"，完全不看
//     按下到抬起之间发生过什么；
//   * QScroller 的 DragStartDistance ≈ 10mm（Android 上约 40 逻辑像素）。位移
//     小于它时既不滚动、也不算滑动，抬手就是一次"合法点击"；
//   * 位移够了以后 QScroller 会吃掉 touch 手势，但**仍把一条合成鼠标抬起按
//     最后位置发给控件**，且中间没有 MouseMove（实测）。所以控件看到的
//     press(起点) + release(终点) 依然像一次点击。
//
// 判定：一次"按下→抬起"只有同时满足下面四条才算点击，否则整条手势按滑动处理
// （吞掉抬起）。四条互相独立，任一成立即为滑动：
//   * 位移阈值 kTapSlopDp = 8dp。取**过程峰值**（鼠标位移与原始触摸位移取大），
//     不是"抬起点 − 按下点"：划出去再划回来（首末差≈0）同样判为滑动。
//   * 长按漂移 kTapHoldDriftMs = 500ms 且 kTapDriftDp = 4dp：按得久又在缓慢
//     挪动（慢慢拖列表）不算点击；长按不动仍然是点击。
//   * 滚动证据：本次手势期间任一滚动区发生过位移（含 QScroller 拖出来的、
//     含惯性未收尾）。内容动过就不是点击。
//   * 忽略阈值 kTapIgnoreAfterScrollMs = 250ms：内容在 250ms 内刚滚动过
//     （含惯性尚未收尾），期间的点击一律忽略。
//   * 多指：手势期间出现过第二根手指，一律不算点击（本应用无双指点按语义）。
//   * 序列污染：手势期间弹出窗口补发的按下（mouse replay）沿用累计峰值，
//     整条手势强制按滑动算，且连按下一起吞（kTapSequenceMs = 300ms 续接窗口，
//     只作用于弹出类窗口；普通控件上的新按下永远开新序列）。
//
// 覆盖范围（GuardScope）：按钮/菜单/下拉框/下拉弹窗列表之外，还包括
//   * Overlay —— 所有 Qt::Popup/Tool/ToolTip 顶层窗口，以及声明
//     tapGuardOverlay 的项目自绘浮层（SrModelSheet / ModalDialog /
//     ProxyPage 悬浮抽屉）里的任何控件；悬浮菜单同样走整条菜单链清理。
//   * Item —— QAbstractItemView（列表行，Qt 自己在抬起时发 clicked）；
//   * Generic —— 自绘可点控件：声明了 tapGuardItem / toggleRow / clickable
//     属性，或自带 PointingHandCursor（WA_SetCursor）的控件，例如 ComicCard、
//     SettingRow、SrModelField、文字链。
// 显式声明 tapGuardSelfManaged=true 的控件（SectionStrip）除外：它内部已有
// 滑动/点击仲裁，守卫吞掉抬起会让它停在"拖动中"。
//
// 冲突消解：QScroller 正在拖同一个容器、且本次手势来自桌面鼠标时，抬起交还
// 给它（它靠这条事件收尾并起惯性；点击已由它自己的假释放取消）。触摸路径相反：
// QScroller 只吃 touch，合成抬起与它无关，必须吞。
constexpr int kTapSlopDp = 8;
constexpr int kTapDriftDp = 4;
constexpr int kTapHoldDriftMs = 500;
constexpr int kTapIgnoreAfterScrollMs = 250;

void InstallTapGuard();        // 幂等：装一次应用级过滤器
int TapGuardSwallowed();       // 回归：拦截次数（吞掉的抬起 + 污染序列的补发按下）
void ResetTapGuardStats();     // 回归：清零计数（截图/自检前调用）
void SetTapGuardLegacy(bool on);  // 回归对照：复刻修复前的手势重置行为
bool TapGuardLegacy();            // 回归：当前是否处于对照档
QString TapGuardTrace();       // 回归：最近 24 条手势判定（真机落盘排障用）
int TapGuardScopeOf(QWidget *w);  // 回归：控件落入的 GuardScope 编号（0=None）

}  // namespace Touch

class EdgeHint;

class TouchAdapter : public QObject
{
    Q_OBJECT
public:
    explicit TouchAdapter(QWidget *host);
    ~TouchAdapter() override;

    void Install();
    void SetActive(bool on);
    bool IsActive() const { return active_; }

    // 演示/回归：把边缘提示画到指定进度（0..1），不触发返回
    void ShowEdgeHint(qreal progress);

    int EdgeWidth() const;
    int Threshold() const;

signals:
    void BackRequested();
    void EdgeProgress(qreal p);

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    bool AtEdge(const QPoint &globalPos) const;
    void BeginDrag(const QPoint &globalPos);
    void UpdateDrag(const QPoint &globalPos);
    void FinishDrag(bool commit);
    void ShowHint(qreal p);

    QWidget *host_ = nullptr;
    EdgeHint *hint_ = nullptr;
    bool active_ = true;
    bool installed_ = false;

    bool dragging_ = false;
    bool decided_ = false;     // 是否已经判明"横滑还是竖滑"
    bool horizontal_ = false;  // 判明后：是否接管（横滑才接管）
    bool fromTouch_ = false;
    QPointer<QWidget> pressWidget_;   // 起手控件：接管时解除它的按下态
    QPoint startGlobal_;
    qreal progress_ = 0.0;
};
