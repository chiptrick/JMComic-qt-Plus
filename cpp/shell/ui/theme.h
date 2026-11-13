// UI 主题层：加载 Python 版同一套 QSS（res/theme/*.qss），再叠加 C++ 侧的组件覆盖规则。
//
// 为什么复用同一份 QSS：
//   Python 版的外观由 QtMaterial 主题（6 套：dark/light × teal/pink/orange）决定，
//   直接复用可以把"基础控件长什么样"这件事变成零差异；C++ 侧只需要补
//   nav/card/chip 这类 Python 用内联 setStyleSheet 写的部分（那边无法逐条搬过来）。
#pragma once

#include <QColor>
#include <QObject>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QString>

class QWidget;

struct UiTokens {
    QColor bg;
    QColor bgAlt;
    QColor surface;
    QColor surfaceHover;
    QColor border;
    QColor text;
    QColor textDim;
    QColor textFaint;
    QColor accent;
    QColor accentSoft;
    QColor danger;
    QColor warning;
    QColor shadow;
    bool dark = true;
};

// ---------------------------------------------------------------- 造型
// 直角矩形风格（2026-11-03）：全站圆角 = 0，去掉胶囊 / 圆形气泡等弧线造型。
//
// 这里是 **C++ 自绘代码的唯一圆角来源**；QSS 侧由 theme.cpp 的 @radius token 注入，
// 值就取 UiStyle::Radius。res/theme/*.qss 是 Python 版共用的、不支持 token，
// 那边的 0px 是字面量 —— 要整体改回圆角，这两处必须一起改（文档见
// docs/cpp_ui_parity.md §9）。
namespace UiStyle {
constexpr int Radius = 0;

// 近似直角：登录 / 打卡 / 确定 / 取消 / 开始阅读 / 收藏 这六类按钮统一 3px 圆角
// —— 接近直角、边缘略微软化，和全站 0px 的"硬直角"区分开，但不会变成胶囊。
// 只影响边框造型：尺寸、配色、hover/pressed/disabled 全部沿用原有
// #Primary / #Ghost / #DangerBtn 规则（theme.cpp 里用属性选择器只覆盖 border-radius）。
constexpr int RadiusSm = 3;

// 带边框的选项框（下拉框 #Filter / #SettingCombo）：同样是 3px 近似直角。
// 与 RadiusSm 分开命名，是为了以后能单独调"框"而不牵动按钮；两者当前取同一
// 数值，保证按钮与选项框的边缘观感一致。无边框选项框（frame=false）不受影响。
constexpr int RadiusOption = 3;

// 给按钮打上 soft 标记（QSS 侧对应 [btnShape="soft"]）。
// 必须在控件首次 polish（show）之前调用；本项目的按钮都在构造期打标记。
void MarkSoftButton(QWidget *w);

// 字号缩放：用户把字号调大/调小时，qss 里的 px 与自绘代码里的 setPixelSize
// 必须一起缩放，否则"只放大一部分文字"比不放大还难看。
// 基准 9pt（设计稿标注时的系统字体大小），夹在 0.85~1.5 之间：
// 上界是照着实测得出的——超过 1.5 后标题栏/阅读器工具条这些定高容器会开始裁字。
qreal FontScale();
int Fpx(int designPx);   // 设计稿像素 → 当前像素（随字号缩放）

// 圆角矩形 → 直角矩形。用 if constexpr，Radius = 0 时不会生成圆角调用。
inline void DrawPanel(QPainter *p, const QRectF &rect)
{
    if constexpr (Radius > 0)
        p->drawRoundedRect(rect, Radius, Radius);
    else
        p->drawRect(rect);
}

inline void AddPanel(QPainterPath &path, const QRectF &rect)
{
    if constexpr (Radius > 0)
        path.addRoundedRect(rect, Radius, Radius);
    else
        path.addRect(rect);
}
}  // namespace UiStyle

class UiTheme : public QObject
{
    Q_OBJECT
public:
    static UiTheme &I();

    // name 形如 dark_teal / light_pink，与 res/theme 下文件名一致
    void Apply(const QString &name);
    void ToggleDark();

    const UiTokens &Tokens() const { return t_; }
    const QString &Name() const { return name_; }
    bool IsDark() const { return t_.dark; }

signals:
    void Changed();

private:
    UiTheme() = default;
    QString BuildOverlay() const;

    // 默认跟随 Python 版：ThemeColorIndex=0 → #FF7B00（设置项里叫"橙色"）
    QString name_ = QStringLiteral("dark_orange");
    qreal lastScale_ = 1.0;   // 上次构建 qss 时的字号缩放比
    qreal lastWeightRatio_ = 1.0;   // 上次构建 qss 时的字重比例（用户字重 / 400）
    UiTokens t_;
};
