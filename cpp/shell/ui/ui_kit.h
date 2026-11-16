// 组件库：C++ 侧对 Python `src/component/` 的等价实现（只覆盖界面契约，不含业务）。
#pragma once

#include <QAbstractButton>
#include <QColor>
#include <QComboBox>
#include <QIcon>
#include <QLabel>
#include <QLayout>
#include <QPixmap>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QString>
#include <QWidget>

class QPainter;

#include "models.h"

class QLineEdit;
class QToolButton;
class QHBoxLayout;
class QVariantAnimation;

// ---------------------------------------------------------------- 图标
namespace Glyph {
enum Name {
    Home, Grid, Search, Heart, Download, Folder, Clock, Comment, Sparkle, Gear,
    Info, User, Back, Play, Plus, Check, Close, Minimize, Maximize, Refresh,
    Trash, Pause, Sun, Moon, ChevronRight, Star, Speed, Bookmark,
    // 对齐 Python 导航栏时新增（每周必看 / 网络存储 / 批量超分）
    Calendar, Server, Layers, Menu,
};

QPixmap Pixmap(Name n, int px, const QColor &color);
QIcon Icon(Name n, int px, const QColor &color);
}  // namespace Glyph

// ---------------------------------------------------------------- 占位封面
namespace CoverArt {
// 无网络时的封面：按标题哈希生成稳定配色的渐变块 + 首字，避免出现一排灰块。
QPixmap Render(const QString &title, const QSize &size, int seed);
}  // namespace CoverArt

// ---------------------------------------------------------------- 卡片绘制
// 卡片在"独立控件"和"QListView delegate"两种场景下必须长得一模一样，
// 所以绘制逻辑只留这一份，ComicCard 与 delegate 都调它。
namespace CardPaint {
QSize Size(int width);
void Paint(QPainter *p, const QRect &rect, const Comic &c, bool hover, bool selected);
}  // namespace CardPaint

// ---------------------------------------------------------------- 流式布局
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent = nullptr, int spacing = 18);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize minimumSize() const override;
    QSize sizeHint() const override;
    void setGeometry(const QRect &rect) override;

private:
    int DoLayout(const QRect &rect, bool testOnly) const;

    QList<QLayoutItem *> items_;
    int spacing_;
};

// ---------------------------------------------------------------- 开关
class ToggleSwitch : public QAbstractButton
{
    Q_OBJECT
public:
    explicit ToggleSwitch(QWidget *parent = nullptr);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void checkStateSet() override;
    void nextCheckState() override;
    // 命中区域 = 整个控件（默认实现要求点在那条细轨道上，手指几乎点不中）
    bool hitButton(const QPoint &pos) const override;
    // 手指从开关上起手纵向滑动 = 滚页面（否则开关吃掉了 press，页面拖不动）
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    qreal pos_ = 0.0;  // 0=off 1=on
    QVariantAnimation *anim_ = nullptr;
    QPoint pressPos_;
    bool dragged_ = false;
    int scrolled_ = 0;
};

// ---------------------------------------------------------------- 触摸友好的滑块
// 问题一：QSlider 在按下时就把值跳到按下的位置 —— 手机上想从滑块附近开始上下
// 翻页，手指一落下值就变了。问题二（更致命）：只认手柄 ±10dp 的命中区，而手柄
// 只有 12px、轨道上百 px，手指按在轨道上既不改值也拖不动，看起来就是"滑块坏了"。
// 策略：托盘内任意位置都能按下，但**按下不改值**；位移出来后先判方向：
//   竖向 → 位移转给外层 QScrollArea（页面照常滚，不跳值）；
//   横向 → 抓手柄连续改值，并保留"手指相对手柄中心"的偏移（不会一下跳到手指处）。
class TouchSlider : public QSlider
{
    Q_OBJECT
public:
    explicit TouchSlider(Qt::Orientation o, QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    int ValueAt(int x) const;   // 轨道像素 → 值（已扣掉手柄半宽）
    void ScrollOuter(int dy);   // 竖向手势转交给外层滚动容器

    bool pressed_ = false;
    bool decided_ = false;    // 手势方向是否已判明
    bool vertical_ = false;
    bool dragging_ = false;
    int grabDx_ = 0;          // 按下点相对手柄中心的水平偏移
    QPoint pressPos_;
    int lastY_ = 0;
};

// ---------------------------------------------------------------- 触摸友好的下拉框
// 手势判定统一在 Touch::TapGuard（touch.cpp）里做，本类只保留触摸端的指针样式：
//   * 按在框上滑动 → 不弹窗、不选中，外层页面照常滚；
//   * 弹窗列表内滑动 → 只滚列表、不提交；点选 → 正常提交。
// 这样裸 QComboBox（首页筛选、模型下拉）与这里用的是同一套阈值，不会一边生效
// 一边失灵；也避免"两层实现同时消费位移导致滚动翻倍"。
class TouchCombo : public QComboBox
{
    Q_OBJECT
public:
    explicit TouchCombo(QWidget *parent = nullptr);
};

// ---------------------------------------------------------------- 基础件
// 单行省略标签：QLabel 自己不会省略，定高标题栏里标题一长就把右侧按钮顶出屏幕。
// 这里把完整文本留在 toolTip，显示文本按当前宽度取 elidedText（右侧补 …），
// 横向尺寸策略为 Ignored —— 布局可以把它压到 0，剩下的宽度优先给固定控件。
class ElideLabel : public QLabel
{
public:
    explicit ElideLabel(const QString &text = QString(), QWidget *parent = nullptr);
    // 设置完整文本（等价 setText，但会重新计算省略）
    void SetFullText(const QString &text);
    QString FullText() const { return full_; }
    // 最小宽度给 0：空间不够时布局可以把本标签一路压到"只剩省略号"，
    // 从而保住右侧的页码/按钮不被挤出去；空间够时仍按 sizeHint 占自然宽度。
    QSize minimumSizeHint() const override;
    // sizeHint 永远按**完整文本**计算。QLabel::sizeHint 用的是当前（已被省略的）
    // 文本 —— 一旦参与布局就会形成"越省略越窄、越窄越省略"的自缩循环。
    QSize sizeHint() const override;

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void UpdateElide();
    QString full_;
};

class Chip : public QPushButton
{
    Q_OBJECT
public:
    explicit Chip(const QString &text, bool checkable = true, QWidget *parent = nullptr);
};

class SearchBox : public QWidget
{
    Q_OBJECT
public:
    explicit SearchBox(const QString &placeholder = QStringLiteral("搜索漫画、作者或标签"),
                       QWidget *parent = nullptr);
    QLineEdit *Edit() const { return edit_; }
    QString Text() const;
    void SetText(const QString &text);

signals:
    void Submitted(const QString &text);
    void Changed(const QString &text);

private:
    QLineEdit *edit_ = nullptr;
};

class SectionHeader : public QWidget
{
    Q_OBJECT
public:
    explicit SectionHeader(const QString &title, const QString &hint = QString(),
                           const QString &action = QString(), QWidget *parent = nullptr);
    QPushButton *ActionButton() const { return action_; }

private:
    QPushButton *action_ = nullptr;
};

class SettingRow : public QWidget
{
    Q_OBJECT
public:
    explicit SettingRow(const QString &title, const QString &desc = QString(),
                        QWidget *control = nullptr, QWidget *parent = nullptr);
    void SetControl(QWidget *control);
    QWidget *Control() const { return control_; }

    // 手机/窄屏：宽控件（滑块/下拉/按钮组）标题在上、控件在下，避免 180px
    // 滑块与说明文字横排把内容顶出屏幕（横向滚动范围 > 0 → 页面能左右滑）。
    // 例外：开关/复选框这类只有 52px 宽的二元控件始终与文字同排（见 ApplyDirection）。
    void SetCompact(bool on);
    bool IsCompact() const { return compact_; }

    // 整行可点：开关类行（ToggleSwitch）点标题/说明也能切换，命中区域从
    // 44x24 的一大块扩到整行
    bool IsClickableRow() const;

protected:
    void mouseReleaseEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;

private:
    void ApplyDirection();
    // 切成上下堆叠的宽度门槛：只由字号缩放决定（Fpx(430)）。
    // 不要掺控件/文本度量 —— 那样门槛会随字重变化，拖字重时整行会在
    // 横排/堆叠之间翻转，观感就是"整页被拉长"。
    int CompactThreshold() const;

    QWidget *control_ = nullptr;
    QHBoxLayout *outer_ = nullptr;
    bool compact_ = false;
    bool dirCheckPending_ = false;   // resize 后只在本轮布局末尾判一次方向
};

class EmptyState : public QWidget
{
    Q_OBJECT
public:
    explicit EmptyState(const QString &title, const QString &text,
                        const QString &action = QString(), QWidget *parent = nullptr);
    QPushButton *ActionButton() const { return action_; }

private:
    QPushButton *action_ = nullptr;
};

class Toast : public QLabel
{
    Q_OBJECT
public:
    static void Show(QWidget *parent, const QString &text, int ms = 2400);

private:
    explicit Toast(QWidget *parent);
    void PopOut();

    QPoint home_;
    bool leaving_ = false;
};

// ---------------------------------------------------------------- 加载条
// 页面级"正在取数据"的指示：3px 细线 + 一段主色扫光。
// 为什么不用 QProgressBar：接口不返回进度，转圈/填块都会暗示"有一个确定的比例"；
// 扫光只表达"还在跑"，并且整条只有 3px，不挤占内容。
class LoadingBar : public QWidget
{
    Q_OBJECT
public:
    explicit LoadingBar(QWidget *parent = nullptr);

    void Start();
    void Stop();
    bool IsRunning() const { return running_; }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    QVariantAnimation *loop_ = nullptr;
    qreal phase_ = 0.0;
    qreal fade_ = 0.0;
    bool running_ = false;
};

// ---------------------------------------------------------------- 模态框
// Python 版的确认框是自绘的（跟随主题），C++ 侧不能直接用 QMessageBox：
// 系统皮肤在深色主题里会突兀地弹出一块浅色对话框。
class ModalDialog : public QWidget
{
    Q_OBJECT
public:
    enum Kind { Info, Warning, Danger };

    static bool Confirm(QWidget *parent, const QString &title, const QString &text,
                        const QString &okText = QStringLiteral("确定"),
                        const QString &cancelText = QStringLiteral("取消"),
                        Kind kind = Warning);
    static void Message(QWidget *parent, const QString &title, const QString &text,
                        Kind kind = Info);
    // 只显示、不阻塞（截图/演示用；业务代码请用 Confirm）
    static void Show(QWidget *parent, const QString &title, const QString &text,
                     const QString &okText = QStringLiteral("确定"),
                     const QString &cancelText = QStringLiteral("取消"),
                     Kind kind = Warning);
    // 单行输入框的模态框（屏蔽词这类"改一个值"的场景）；取消返回空串
    static QString Prompt(QWidget *parent, const QString &title, const QString &text,
                          const QString &initial = QString());

protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    ModalDialog(QWidget *host, const QString &title, const QString &text,
                const QString &okText, const QString &cancelText, Kind kind, bool confirm,
                const QString *initialValue = nullptr);
    void CenterCard();
    void Finish(bool ok);

    class QEventLoop *loop_ = nullptr;
    QWidget *card_ = nullptr;
    class QLineEdit *edit_ = nullptr;
    bool accepted_ = false;
    // 进出场动效：scrim 是遮罩浓度（0..1），intro_ 保证只播一次
    qreal scrim_ = 0.0;
    bool intro_ = false;
    bool finished_ = false;
    // 点遮罩关闭必须等抬起：按下即关闭会让"从遮罩上滑过去"被当成一次点击，
    // 而且 TapGuard 只吞抬起、拦不住 press 里的副作用。
    bool outsideArmed_ = false;
    QPoint outsidePress_;
};

// ---------------------------------------------------------------- 卡片
class ComicCard : public QWidget
{
    Q_OBJECT
public:
    explicit ComicCard(const Comic &c, QWidget *parent = nullptr);

    void SetComic(const Comic &c);
    const Comic &Data() const { return c_; }
    void SetCardWidth(int w);
    QSize sizeHint() const override;

signals:
    void Activated(const Comic &c);
    void SecondaryClicked(const Comic &c, const QPoint &globalPos);

protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseReleaseEvent(QMouseEvent *) override;

private:
    int CoverHeight() const;

    Comic c_;
    QPixmap cover_;
    bool hover_ = false;
    int width_ = 168;
};
