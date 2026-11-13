// 首页分区容器：每个分区是一整页（分区栏 + 内容），切换时整页横向滑。
//
// 为什么不用 QTabWidget：
//   * QTabWidget 一次只显示一页，做不到"上一页滑出、下一页滑入"；
//   * 分区栏要跟着页面一起滑，所以分区栏必须属于"页"，不能是页外的常驻控件。
// 因此这里每个 pane = [SectionStrip + 内容]，切换就是移动两个 pane。
//
// 手势与动画参数（与模块切换共用同一套，见 demo_swipe/README.md）：
//   跟手 1:1；12px 轴锁（|dx| > 1.2·|dy|）；离手 28% 或 0.35px/ms 提交；
//   补间 OutCubic，时长 240ms（按剩余距离缩到 [120,240]）；一次手势最多一页；
//   首尾分区橡皮筋 ×0.32，绝不把手势转交给模块层。
//
// 事件策略：**只观察、不抢事件**。分区内容包括 QQuickWidget（卡片墙），
// 抢走它的 press/release 会让 QML 的 MouseArea 停在按下态；因此这里
//   * 鼠标/触摸序列照常下发；
//   * 判定横滑后，把卡片墙的点击锁上（ComicGrid::SetSwipeLock），松手即解；
//   * 落在普通控件（按钮）上的拖动，用一次"界外 release"收掉按下态（防误触）。
#pragma once

#include <QElapsedTimer>
#include <QPoint>
#include <QVector>
#include <QWidget>

class ComicGrid;
class SectionStrip;
class QVariantAnimation;

class SectionSwipe : public QWidget
{
    Q_OBJECT
public:
    explicit SectionSwipe(QWidget *parent = nullptr);
    ~SectionSwipe() override;

    // titles 与 contents 一一对应；contents 的所有权交给本容器
    void SetPanes(const QStringList &titles, const QVector<QWidget *> &contents);
    void ClearPanes();

    int Count() const { return panes_.size(); }
    int Current() const { return cur_; }
    // animate=false 或动效关闭时瞬切
    void SetCurrent(int index, bool animate = true);
    bool IsBusy() const;

    // 回归/兼容：当前页的分区栏与页容器
    SectionStrip *CurrentStrip() const;
    QWidget *CurrentPane() const;
    int StripOffset() const;
    int StripContentWidth() const;
    bool StripDragging() const;
    void StripDrag(int dx);
    void StripTap(int x);

    // 回归：容器内部状态（一行 ASCII）
    QString StateReport() const;
    // 回归：合成一次横滑（走与真实指针相同的状态机）
    void DemoSwipe(int dx, int dy = 0, int steps = 8);
    // 截图：冻结在 from→to 的指定进度（不播动画）
    void FreezeAt(int from, int to, qreal progress);
    int OffsetPx() const { return offset_; }

signals:
    void CurrentChanged(int index);
    void SwipeRejected(const QString &why);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    struct Pane {
        QWidget *host = nullptr;
        SectionStrip *strip = nullptr;
        QWidget *content = nullptr;
    };
    enum class Phase { Idle, Tracking, Dragging, Settling };

    bool HandlePress(const QPoint &global, QWidget *target);
    void HandleMove(const QPoint &global);
    void HandleRelease();
    void ApplyDrag(int rawOffset);
    void Interrupt();
    void UnlockCardsSoon();
    void LockHorizontal();
    void Settle();
    void AnimateTo(int target, bool commit);
    void FinishSettle(bool commit);
    void CommitIncoming();
    void AbortIncoming();
    void SetIncoming(int index);
    void ApplyOffset();
    void SetPhase(Phase phase);
    void SetCardLock(bool on);
    void CancelChildPress(QWidget *target);
    qreal SampleVelocity() const;
    bool IsStripTarget(QWidget *w) const;
    static bool InsideQuickWidget(QWidget *w);

    QVector<Pane> panes_;
    QVector<ComicGrid *> grids_;
    int cur_ = 0;
    int incoming_ = -1;
    int offset_ = 0;
    int dragBase_ = 0;
    Phase phase_ = Phase::Idle;

    QPoint pressGlobal_;
    QWidget *pressTarget_ = nullptr;
    bool axisDead_ = false;
    // 正在补发"界外 release"：这次事件不能再进自己的状态机
    bool sendingCancel_ = false;
    int touchId_ = -1;
    QVector<QPair<qint64, int>> samples_;
    QElapsedTimer gestureClock_;

    QVariantAnimation *anim_ = nullptr;
    int slideMs_ = 240;
    qreal commitRatio_ = 0.28;
    qreal commitVelocity_ = 0.35;
    int axisLock_ = 12;
    qreal edgeResist_ = 0.32;
};
