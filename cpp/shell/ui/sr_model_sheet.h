// 超分模型选择浮层 + 整行「模型选择栏」（对应 docs/waifu2x_model_list_demo.html）。
//
// 为什么单独一层：
//   * 模型有 39 项，原先在「图片超分」页里排成一堵内联列表，页面还要放预览和三个
//     按钮，360dp 视口下模型行的「使用」按钮会被裁到屏幕外（真机实测 hmax=387）；
//   * 内联列表没有回显：当前生效的模型和"点一下试试"的行长得一模一样；
//   * 模型名原本走 ElideLabel，同名族（9 个 CUNET 变体）压成同一个前缀后无法区分。
//
// 现在的形态：
//   * 页面里只留一行「选择栏」（模型显示名 + onnx 完整文件名），点开浮层再选；
//   * 浮层按算法分组，分组条**吸顶**（滚动时始终知道自己在哪一组，下一组会把它顶走）；
//   * 每行两行文本：显示名 + onnx 完整 ID，都不做省略；
//   * 当前生效的模型整行高亮 + 左侧主色条 + 「当前使用」角标；本次点选但还没确认的
//     行单独标「已选」——两者必须能区分，否则点完看到还是"当前使用"会以为没选中。
#pragma once

#include <QString>
#include <QWidget>

class ElideLabel;
class QLabel;

// 整行选择栏：主行 = 模型显示名，副行 = onnx 完整文件名（等宽小字，不省略）。
// 点击行为由调用方接（一般打开 SrModelSheet::Pick）。
//
// 为什么是 QWidget 而不是 QPushButton：基座 qss 把 QPushButton 的内容盒钉死在
// 34px，两行文本在字号放大后会被裁掉一半；而 QSS 的 min/max-height 会覆盖
// C++ 的 setFixedHeight，改不回来。QWidget + 布局的高度由字体自然决定。
class SrModelField : public QWidget
{
    Q_OBJECT
public:
    explicit SrModelField(QWidget *parent = nullptr);

    // 设置当前模型名；名字为空/不在目录里时显示「未选择模型」占位
    void SetModel(const QString &name);
    QString Model() const { return name_; }

signals:
    void clicked();

protected:
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void UpdateTexts();

    ElideLabel *main_ = nullptr;
    ElideLabel *id_ = nullptr;
    QLabel *go_ = nullptr;
    QString name_;
};

namespace SrModelSheet {

// 打开「选择超分模型」浮层并同步等待确认。
//   current：标记为「当前使用」的模型名（一般传 SrEngine::CurrentModel()）
// 返回用户确认的模型名；取消/关闭返回空串。**不改引擎状态**——是否生效由调用方决定
// （图片超分页要写 ui.ini，批量超分页只把选择用在这一次批处理上）。
QString Pick(QWidget *host, const QString &current);

// 回归/截图：只打开浮层，不阻塞（关闭后自删）。--sr-sheet 用。
// 返回浮层控件（截图时可以按 objectName 找列表并滚到指定位置，验证吸顶条）。
QWidget *Show(QWidget *host, const QString &current);

}  // namespace SrModelSheet
