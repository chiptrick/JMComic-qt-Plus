// 首页顶部分区栏：自绘 + 横向拖动滚动。
//
// 为什么不用 QTabBar：
//   * QTabBar 在触摸屏上**不支持拖动滚动**——只有滚轮和左右滚动按钮，手指横向
//     滑动时它把 press/release 当点击处理，滑到哪个标签就切到哪个标签
//     （用户报的"滑不动 + 滑动误触其他标签"是同一个根因）；
//   * QTabBar 的内部偏移是私有的，没有公开 API 能做"跟手拖动"。
//
// 因此这里只保留 QTabWidget 的页面容器（tabBar 隐藏），顶部换成这个自绘条：
//   * 按下 → 记录起点与当前偏移；移动超过 8dp 判定为拖动，只改偏移、不选中；
//   * 松手时位移小于阈值才算点击，选中手指下的分区；
//   * 松手有惯性（速度衰减动画），到边界自动回弹；
//   * 选中项自动滚入可视区（SetCurrentIndex / 程序切分区时）。
//
// 视觉与 QSS 里的 QTabBar::tab 保持一致：13px 文字、左右 14px 内边距、
// 选中项主色 + 2px 下划线。颜色/字号全部取主题与 AppFont，主题或字号变化时重绘。
#pragma once

#include <QPoint>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QVariantAnimation;

class SectionStrip : public QWidget
{
    Q_OBJECT
public:
    explicit SectionStrip(QWidget *parent = nullptr);

    void SetItems(const QStringList &titles);
    void SetCurrentIndex(int index);          // 程序设置，不发 CurrentChanged（下划线补间）
    // 同上，但不补间：分区页滑入前要先把选中项定好，否则会看到下划线从 0 滑过去
    void SetCurrentIndexImmediate(int index, bool ensureVisible = true);
    int CurrentIndex() const { return current_; }
    QStringList Items() const { return titles_; }

    // 回归/自检：当前滚动位置（>=0，0 = 最左）、内容总宽、是否正在拖动
    int Offset() const { return offset_; }
    int ContentWidth() const;
    bool IsDragging() const { return dragging_; }

    int sizeHintHeight() const;

    // 回归入口：合成一次"按下 → 横移 dx → 松手"的完整手势
    void DemoDrag(int dx);
    void DemoScrollBy(int dx);
    // 回归入口：合成一次点按（x 为可视坐标）；命中空白时不切换
    void DemoTap(int x);
    int HitTest(int x) const { return IndexAtX(x); }

signals:
    void CurrentChanged(int index);
    // 拖动开始/结束（外壳可据此暂停列表滚动等）
    void DragStateChanged(bool dragging);

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void wheelEvent(QWheelEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    int TabWidth(int index) const;
    int TabLeft(int index) const;      // 未加偏移的累计左边界
    int IndexAtX(int x) const;         // x 为可视坐标
    int MaxOffset() const;
    void ClampOffset();
    void EnsureVisible(int index, bool animate);
    void StartFling(qreal velocity);
    void EmitDragState(bool dragging);
    // 选中下划线：内容坐标（左/右边界），切换分区时在这两个值之间补间
    void AnimateIndicator(bool animate);

    QStringList titles_;
    QVector<int> widths_;
    int current_ = 0;
    int offset_ = 0;   // 滚动位置（内容坐标，0 = 最左）
    int hover_ = -1;
    qreal indLeft_ = 0.0;
    qreal indRight_ = 0.0;

    bool dragging_ = false;
    bool moved_ = false;
    QPoint pressPos_;
    int pressOffset_ = 0;
    int pressIndex_ = -1;
    QVariantAnimation *fling_ = nullptr;
};
