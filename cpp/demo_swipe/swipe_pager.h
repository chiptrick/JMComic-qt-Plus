// 整页横向滑动容器（DEMO 用，独立于 cpp/shell，不引用工程内任何文件）。
//
// 设计目标与工程内已有实现对齐：
//   * 位移语义抄 PageSwipe.qml：同一份 signed offset 同时给"当前页"和"目标页"，
//     两页严格相邻（x 与 x+W），接缝不会露 1px。
//   * 时间轴抄 Motion：位移只用 OutCubic，时长取 Motion::Slow(240)。
//   * 手势进度是"外部可写"的（FreezeAt / 手势逐帧），不是只能从 0 播到 1 ——
//     PageTransition 现在缺的就是这一点。
//
// 状态机（一次手势只走一遍，不会连翻）：
//   Idle → Tracking(按下, 等轴锁定) → Dragging(跟手) → Settling(离手补间) → Idle
//   纵向手势在 Tracking 阶段就被判死（axisDead_），整段手势不再参与横滑。
//
// 关键取舍：
//   1. 不做"多页连滑"：一次手势最多翻一页（drag(x) 被夹在 [-W, W]），
//      避免长距离甩动一次跳过 3 个分区导致落点不可预期。
//   2. 边界用橡皮筋（edgeResistance_）而不是硬停：位移 = dx * 0.32，
//      且**绝不**把手势转交给外层 pager（分区首尾 ≠ 模块切换）。
//   3. 离手判定同时看距离与速度：p >= 28% 或 |v| >= 0.35 px/ms 即提交。
//   4. 补间时长按剩余距离线性缩放并夹到 [120, 240]，程序触发恒为 240 ——
//      曲线始终是 OutCubic，所以"跟手释放"和"点导航"观感一致。
#pragma once

#include <QEasingCurve>
#include <QElapsedTimer>
#include <QPoint>
#include <QVector>
#include <QWidget>

class QVariantAnimation;

class SlidePager : public QWidget
{
    Q_OBJECT
public:
    // Slide = 本次要验证的方案；CrossFade = 现状对照（分区 120ms / 模块 180ms）
    enum class Style { Slide, CrossFade };
    enum class Phase { Idle, Tracking, Dragging, Settling, Frozen };

    explicit SlidePager(QWidget *parent = nullptr);
    ~SlidePager() override;

    void AddPage(QWidget *page);
    int Count() const { return pages_.size(); }
    int Current() const { return cur_; }
    QWidget *PageAt(int index) const { return pages_.value(index); }
    int Incoming() const { return incoming_; }

    void SetStyle(Style style);
    Style CurrentStyle() const { return style_; }
    void SetSlideDuration(int ms) { slideMs_ = qMax(0, ms); }
    int SlideDuration() const { return slideMs_; }
    void SetCrossFadeDuration(int ms) { fadeMs_ = qMax(0, ms); }
    // 阈值：离手时位移达到容器宽度的多少比例就提交
    void SetCommitDistance(qreal ratio) { commitRatio_ = qBound(0.05, ratio, 0.9); }
    qreal CommitDistance() const { return commitRatio_; }
    // 甩动速度阈值（px/ms），与位移同号表示同向
    void SetCommitVelocity(qreal pxPerMs) { commitVel_ = qMax(0.0, pxPerMs); }
    qreal CommitVelocity() const { return commitVel_; }
    // 轴锁定阈值（px）：超过这个位移才开始判方向
    void SetAxisLock(int px) { axisLock_ = qMax(2, px); }
    int AxisLock() const { return axisLock_; }
    // 边界橡皮筋系数：0 = 完全不动，1 = 完全跟手
    void SetEdgeResistance(qreal r) { edgeResist_ = qBound(0.0, r, 1.0); }
    qreal EdgeResistance() const { return edgeResist_; }

    // 程序切换：animate=false 或 slideMs=0 时瞬切
    void SetCurrent(int index, bool animate = true);
    bool Busy() const;

    // -1..1：负 = 正朝"下一个"位移，正 = 朝"上一个"。分区栏下划线按它插值。
    qreal Progress() const { return progress_; }
    Phase CurrentPhase() const { return phase_; }
    QString PhaseName() const;
    qreal LastFrameMs() const { return frameMs_; }   // 相邻两帧间隔的指数平均

    // ---- 合成输入（与真实指针共用同一个状态机，回归/自动演示用） ----
    void DemoSwipe(int dx, int dy, int steps = 10);         // 同步吐事件
    void StartAutoSwipe(int dx, int dy, int ms);            // 按时间轴吐事件
    void CancelGesture();
    // 截图/回归：冻结在 from→to 的指定进度（不触发 CurrentChanged）
    void FreezeAt(int from, int to, qreal progress);
    void ClearFreeze();

    // 回归专用：覆盖速度采样（0 位移的合成手势算不出速度）
    void SetVelocityOverride(qreal v) { velocityOverride_ = v; }

signals:
    void CurrentChanged(int index);
    void ProgressChanged(qreal progress);
    void PhaseChanged(const QString &name);
    void SwipeRejected(const QString &why);   // 边界 / 纵向手势，状态条显示用

protected:
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool HandlePress(const QPoint &global, QWidget *target);
    void HandleMove(const QPoint &global, Qt::MouseButtons buttons);
    void HandleRelease();
    void LockHorizontal();
    void CancelChildPress();
    void ReleasePointer();
    void ApplyDrag(int rawOffset);
    qreal SampleVelocity() const;
    void Settle();
    void AnimateOffsetTo(int target, bool commit);
    void FinishSettle(bool commit);
    void AbortIncoming();
    void CommitIncoming();
    void ApplyOffset();
    void SetIncoming(int index);
    void SyncPages();
    void SetPhase(Phase phase);
    void TickFrame();
    void StartCrossFade(int target);
    void Interrupt();

    QVector<QWidget *> pages_;
    int cur_ = 0;
    int incoming_ = -1;
    int offset_ = 0;
    int dragBase_ = 0;
    qreal progress_ = 0.0;

    Phase phase_ = Phase::Idle;
    bool axisDead_ = false;
    bool grabbed_ = false;
    QPoint pressGlobal_;
    QWidget *pressTarget_ = nullptr;
    QVector<QPair<qint64, int>> samples_;   // (时间, 位移)

    QVariantAnimation *anim_ = nullptr;
    QElapsedTimer frameClock_;
    QElapsedTimer gestureClock_;
    qreal frameMs_ = 0.0;
    int frameCount_ = 0;

    Style style_ = Style::Slide;
    int slideMs_ = 240;
    int fadeMs_ = 180;
    qreal commitRatio_ = 0.28;
    qreal commitVel_ = 0.35;
    int axisLock_ = 12;
    qreal edgeResist_ = 0.32;
    qreal velocityOverride_ = 0.0;
};
