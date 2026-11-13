// 交互动效层（C++ 版独立设计，不复用 Python 的动画覆盖范围）。
//
// 设计依据是 C++ 版自己的交互模型，而不是"Python 有哪几个动画"：
//   1. 桌面/触摸共用一套 Widget 树，动画只能动**便宜属性**（透明度、位置、自绘参数），
//      绝不能动布局几何 —— 每帧 relayout 会把滚动容器、QQuickWidget 一起拖进重排，
//      144Hz 上就是肉眼可见的掉帧。
//   2. 层级关系明确：换页 = 空间位移（横向），同级切换 = 淡入淡出，浮层 = 淡入 + 位移。
//      三种语义各自只有一条曲线，避免"同一个动作在不同页面观感不同"。
//   3. 动效必须能被一键关掉（--no-anim），并且关掉后逻辑完全等价（无残留状态）。
//
// 三类原语：
//   * Animate()    通用数值补间（同名 tag 自动打断，避免连点时动画叠加）
//   * Ripple()     点击反馈：按在哪个点，涟漪就从哪个点扩散
//   * CrossFade()  内容原地替换的交叉淡出：抓一张旧画面盖上去，改完再让它淡掉
//
// 为什么 CrossFade 用"自绘 QPixmap + setOpacity"，而不是 QGraphicsOpacityEffect：
//   QGraphicsOpacityEffect 会让整棵子树走离屏渲染（含 QQuickWidget 的 FBO），
//   一帧多一次全尺寸合成；而这里只是把旧画面按 alpha 递减贴一次，代价固定且很低。
#pragma once

#include <QEasingCurve>
#include <QPoint>
#include <QString>

#include <functional>

class QObject;
class QWidget;

namespace Motion {

// 统一时长表。所有动效只能取这几个值，避免出现 85/90/95 这种"各写各的"。
enum Duration {
    Instant = 0,
    Fast = 120,    // 微反馈：涟漪、提示消失、文字块切换
    Base = 180,    // 内容替换：交叉淡出、浮层进出
    Slow = 240,    // 大幅位移：抽屉、工具条、页面
    Page = 250,    // 换页（与 PageTransition 的默认时长一致）
};

bool Enabled();
void SetEnabled(bool on);

// 三条语义曲线：进场减速、退场加速、位移两端缓。
QEasingCurve EnterCurve();
QEasingCurve ExitCurve();
QEasingCurve MoveCurve();

// 通用补间。owner 用来承载动画对象与打断标记：同一个 (owner, tag) 只会有一个动画在跑。
void Animate(QObject *owner, const QString &tag, qreal from, qreal to, int ms,
             const QEasingCurve &curve, const std::function<void(qreal)> &step,
             const std::function<void()> &done = {});

// 立刻停掉某个 (owner, tag) 的补间（不会触发 done）。用于"反向操作打断正向动画"，
// 例如加载条正在淡出时又开始了新一轮加载。
void Stop(QObject *owner, const QString &tag);

// 点击涟漪。pos 为控件内坐标；默认取几何中心。
void Ripple(QWidget *w, const QPoint &pos = QPoint(-1, -1));

// 全局点击反馈：按钮按下即出涟漪。可重复调用（只装一次）。
void InstallTapFeedback();
void UninstallTapFeedback();

// 淡入/淡出（QGraphicsOpacityEffect，动画结束即摘掉，不留常驻离屏合成）。
// 不要用在 QQuickWidget 上（它自带 FBO，套 effect 会额外多一层离屏）。
void FadeIn(QWidget *w, int ms = Fast);
void FadeOut(QWidget *w, int ms = Fast, const std::function<void()> &done = {});

// 原地内容替换：抓旧画面 → mutate() → 旧画面在 ms 内淡出。
// 用于分区切换、搜索结果到达、空态/列表互斥这类"同一块区域换内容"的场合。
void CrossFade(QWidget *host, const std::function<void()> &mutate, int ms = Base);

// 浮层位移（Toast/工具条这类不在布局里的控件）。从当前位置移动到 to。
void SlideTo(QWidget *w, const QPoint &to, int ms = Slow,
             const QEasingCurve &curve = ExitCurve(),
             const std::function<void()> &done = {});

}  // namespace Motion
