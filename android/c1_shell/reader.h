// C1-W3：本地阅读器 —— 双页 / 阅读方向 / 亮度 / 手势（W2 的翻页与缩放在此之上）。
//
// 设计约束：
//   * 只依赖 Qt Widgets；W3 的目标是把"能看"补成"顺手看"：
//       - 双页(spread)与 RTL（日漫从右往左）；
//       - 亮度压暗（夜间阅读，不做背光控制，那是系统权限）；
//       - 触摸手势：双指捏合缩放、左右滑动翻页（QScrollArea 会消费滚动手势，
//         所以翻页手势挂在 viewport 的事件过滤器上）；
//   * 目录来源优先级：QFileDialog（用户手选）> AppLocalDataLocation/reader
//     （便于 adb 冒烟测试，不需要存储权限）> /sdcard/c1_shell（用户手工投放）；
//   * 默认"适宽"显示，缩放系数是相对适宽的倍率，窗口尺寸变化时按当前倍率重渲染。
#pragma once

#include <QImage>
#include <QLabel>
#include <QScrollArea>
#include <QStringList>
#include <QWidget>

class QEvent;
class QKeyEvent;
class QResizeEvent;

class ReaderView : public QWidget
{
    Q_OBJECT
public:
    enum class Direction { LTR, RTL };

    explicit ReaderView(QWidget *parent = nullptr);

    // 返回实际加载的页数；0 表示该目录没有可用图片
    int LoadDir(const QString &dir);
    // 在线阅读：图片已由 BrowsePage 下载/还原到本地，这里只负责显示。
    // 与 LoadDir 的区别是"不按目录扫、不排序"，顺序就是传入顺序（页序必须稳定）。
    int LoadFiles(const QStringList &files);
    // 冒烟/日常入口：默认目录有图就直接用，否则弹目录选择框
    void LoadDefaultOrPick();
    void OpenPicker();
    void NextPage();
    void PrevPage();
    void ZoomIn();
    void ZoomOut();
    void ResetZoom();

    // ---- W3 体验项 ----
    void ToggleTwoPage();            // 单页 <-> 双页(spread)
    void ToggleDirection();          // LTR <-> RTL
    void CycleBrightness();          // 1.0 -> 0.75 -> 0.5 -> 0.25 -> 1.0
    void SetBrightness(double v);    // 0..1，1=不变暗

    // 标题栏要显示 "Reader n/m"，因此外壳需要能查到当前页与总页数。
    // 只暴露只读视图，不把 files_/current_ 的所有权外泄。
    int PageCount() const { return files_.size(); }
    int CurrentPage() const { return current_ >= 0 ? current_ + 1 : 0; }

signals:
    void PageChanged(int current, int total, const QString &path);
    void StatusMessage(const QString &text);
    void SelfTestDone();

public:
    // 冒烟自检：加载默认目录 → 翻页/回翻/缩放/复位 → 双页/方向/亮度，全走日志
    void RunSelfTest();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void ShowPage(int index);
    void RenderImage();
    void ApplyBrightness();
    int SpreadStep() const { return twoPage_ ? 2 : 1; }
    QStringList ImageFilesIn(const QString &dir) const;

    QScrollArea *scroll_ = nullptr;
    QLabel *imageLabel_ = nullptr;
    QWidget *overlay_ = nullptr;      // 亮度压暗层（透明鼠标事件）
    QImage base_;                     // 当前页
    QImage second_;                   // 双页时的第二页（可能为空）
    QStringList files_;
    int current_ = -1;
    double zoom_ = 1.0;               // 相对"适宽"的倍率；1.0 = 适宽
    double brightness_ = 1.0;         // 1=不变暗；0=全黑
    bool twoPage_ = false;
    Direction direction_ = Direction::LTR;
    // 手势状态（viewport 事件过滤器里用）
    QPoint pressPos_;
    bool pressValid_ = false;
};
