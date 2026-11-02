// C1-W2：本地阅读器骨架 —— 打开本地目录 / 翻页 / 缩放。
//
// 设计约束：
//   * 只依赖 Qt Widgets（与 C1 的壳技术栈一致，后续 W3 在这里补双页/手势/亮度）；
//   * 目录来源优先级：QFileDialog（用户手选）> AppLocalDataLocation/reader
//     （便于 adb 冒烟测试，不需要存储权限）> /sdcard/c1_shell（用户手工投放）；
//   * 默认"适宽"显示，缩放系数是相对适宽的倍率，窗口尺寸变化时按当前倍率重渲染；
//   * 左右方向键翻页、+/- 缩放：桌面直接可用；Android 冒烟用 adb keyevent 驱动。
#pragma once

#include <QImage>
#include <QLabel>
#include <QScrollArea>
#include <QStringList>
#include <QWidget>

class QKeyEvent;
class QResizeEvent;

class ReaderView : public QWidget
{
    Q_OBJECT
public:
    explicit ReaderView(QWidget *parent = nullptr);

    // 返回实际加载的页数；0 表示该目录没有可用图片
    int LoadDir(const QString &dir);
    // 冒烟/日常入口：默认目录有图就直接用，否则弹目录选择框
    void LoadDefaultOrPick();
    void OpenPicker();
    void NextPage();
    void PrevPage();
    void ZoomIn();
    void ZoomOut();
    void ResetZoom();

signals:
    void PageChanged(int current, int total, const QString &path);
    void StatusMessage(const QString &text);
    void SelfTestDone();

public:
    // 冒烟自检：加载默认目录 → 翻页/回翻/缩放/复位，全部走日志；完成后发 SelfTestDone
    void RunSelfTest();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void ShowPage(int index);
    void RenderImage();
    QStringList ImageFilesIn(const QString &dir) const;

    QScrollArea *scroll_ = nullptr;
    QLabel *imageLabel_ = nullptr;
    QImage base_;
    QStringList files_;
    int current_ = -1;
    double zoom_ = 1.0;   // 相对"适宽"的倍率；1.0 = 适宽
};
