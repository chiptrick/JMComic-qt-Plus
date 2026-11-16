// 菜单与下拉弹窗的触摸交互层（按压反馈 / 抬起才响应 / 滑动滚动与边界提示）。
//
// 背景：QMenu 原生交互在触摸端有三套别扭行为 —— 按下即起 20ms 子菜单定时器
// （表现为"按下触发"、滑动开始又立刻收起）、滑动中高亮跟手并在抬手后关闭子菜单、
// 没有按压视觉。本层在触摸模式下接管：
//   * 按下：QMenu / QComboBox 本体的按下都被吞（不弹子菜单、不设 currentAction、
//     不 showPopup），手指下的项显示半透明按压层；QComboBox 已经展开的弹窗列表
//     的按下放行（QListView 需要 setCurrentIndex 才能提交点选），只叠加按压层。
//   * 滑动：按压层渐隐消失；QMenu 用合成滚轮跟手滚动，滚动到顶/底（或弹窗列表
//     滚到边界）时按滑动方向弹出"已到顶部 / 已到底部"提示条（BoundaryHint）。
//   * 抬起：无滑动 → 普通项 trigger 并关闭菜单链、子菜单项弹出子菜单（松手才响应
//     按下请求）；有滑动 → 不触发，菜单保持打开（不再"高亮后立刻关闭"）。
// 桌面鼠标路径不受影响（接管只在触摸模式启用）。
#pragma once

#include <QPoint>
#include <QString>

class QWidget;
class QComboBox;
class QMenu;
class QAbstractItemView;
class QAbstractScrollArea;

namespace Touch {

// 边界提示条：在宿主内侧顶部/底部弹一条渐隐的"已到顶部 / 已到底部"。
// 同一宿主、同一方向 800ms 节流；显示约 1s 后自动淡出销毁。
void ShowBoundaryHint(QWidget *host, bool top, const QString &text = QString());

// 给滚动容器装"到达顶/底提示"：QScroller 拖动中滚动值越过边界时触发。
// 幂等（同一 area 只装一次）。
void InstallBoundaryHints(class QAbstractScrollArea *area);

// 菜单/下拉弹窗的按压与滑动接管。所有方法按"弹出窗口"寻址（QMenu 自身或
// QComboBox 弹窗容器）。
class MenuPressController {
public:
    // QMenu 按下：接管并吞掉这次按下 —— QMenu::mousePressEvent 不执行，
    // setCurrentAction 与 20ms 子菜单定时器都不会启动，"按下弹二级菜单/滑动时
    // 闪一下"从源头消失。按压层由控制器自绘，松手（无滑动）才 trigger 或弹子菜单。
    // **返回值必须被调用方使用**：true = 已接管，应用层要 return true 吞掉按下；
    // false = 未接管（菜单不可见 / legacy 对照），按下照常放行。
    static bool BeginMenuPress(QMenu *menu, const QPoint &global);

    // QComboBox 按下：接管并吞掉这次按下 —— 选项框不再"按下即弹出"，
    // 按压层画在框上，松手（无滑动）才真正 showPopup()，滑动则不弹。
    // 返回 true 表示按下已由控制器处理，事件应被吞。
    static bool BeginComboPress(QComboBox *combo, const QPoint &global);

    // QComboBox 弹窗列表按下：只叠加按压视觉，返回 false 表示按下仍要放行给
    // 视图（QListView 需要 setCurrentIndex 才能提交点选）。
    static bool BeginListPress(QAbstractItemView *view, const QPoint &global);

    // 手势推进：global 为当前全局位置，dy 为本次全局 y 位移。
    // 返回 true 表示事件应被吞（QMenu 始终吞；下拉弹窗放行给 DragTo 滚动）。
    static bool FeedMove(QWidget *popup, const QPoint &global, int dy);

    // 手势结束：控制器决定触发/弹子菜单/保持打开。返回 true 表示事件应被吞。
    static bool FeedRelease(QWidget *popup, const QPoint &global);

    // 按下落在可见菜单矩形之外：关闭那条菜单链（接管后 Qt 原生的
    // "点外面关闭"不再工作，由这里补齐）。在每个新按下时调用。
    static void DismissOutside(const QPoint &global);

    // 该弹出窗口是否处于接管状态（有按压手势）。
    static bool HasState(QWidget *popup);

    // 手势取消（弹窗销毁/窗口切换）：清理按压层与状态。
    static void Cancel(QWidget *popup);
};

}  // namespace Touch
