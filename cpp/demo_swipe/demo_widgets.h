// DEMO 里的几个自绘小部件：分区栏、左侧导航、卡片、开关。
// 只服务于"看手势与转场"，视觉按工程主题色（#31363b / #1de9b6）对齐。
#pragma once

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

class QVariantAnimation;
class QPainterPath;

// 顶部分区栏：文字 + 2px 下划线。下划线位置由 SlidePager 的 signed progress 驱动，
// 所以"手指还在拖"的时候，栏上的选中项就已经跟着走了 —— 转场不是切完才发生的事。
class DemoStrip : public QWidget
{
    Q_OBJECT
public:
    explicit DemoStrip(QWidget *parent = nullptr);

    void SetItems(const QStringList &items);
    void SetState(int current, qreal progress);   // progress: -1..1
    int Current() const { return cur_; }
    int Count() const { return items_.size(); }
    QSize sizeHint() const override { return {100, 46}; }

signals:
    void Activated(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    QRect SlotRect(int index) const;
    int IndexAt(int x) const;

    QStringList items_;
    int cur_ = 0;
    qreal progress_ = 0.0;
};

// 左侧导航：图标 + 文字。选中条的位置同样按 progress 插值（模块切换也是"整页滑"）
class NavRail : public QWidget
{
    Q_OBJECT
public:
    explicit NavRail(QWidget *parent = nullptr);

    void SetItems(const QStringList &items);
    void SetState(int current, qreal progress);

signals:
    void Activated(int index);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;

private:
    QRect ItemRect(int index) const;
    int IndexAt(int y) const;
    QPainterPath GlyphPath(int index, const QRectF &box) const;

    QStringList items_;
    int cur_ = 0;
    qreal progress_ = 0.0;
    int hover_ = -1;
};

// 卡片：封面用两条同色系渐变 + 编号，"哪一页在动"一眼能看清
class DemoCard : public QWidget
{
    Q_OBJECT
public:
    enum Variant { Wide = 0, Narrow = 1 };

    DemoCard(const QString &code, const QString &title, int hue, Variant variant,
             QWidget *parent = nullptr);

    QSize sizeHint() const override;

signals:
    void Activated(const QString &title);

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    QString code_;
    QString title_;
    int hue_ = 200;
    Variant variant_ = Wide;
    bool hover_ = false;
    bool down_ = false;
};

// 设置页开关（顺带验证"拖拽不会误点开关"）
class ToggleSwitch : public QWidget
{
    Q_OBJECT
public:
    explicit ToggleSwitch(bool on, QWidget *parent = nullptr);
    bool IsOn() const { return on_; }
    void SetOn(bool on, bool animate = true);
    QSize sizeHint() const override { return {44, 24}; }

signals:
    void Toggled(bool on);

protected:
    void paintEvent(QPaintEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    bool on_ = false;
    qreal t_ = 0.0;
    QVariantAnimation *anim_ = nullptr;
};
