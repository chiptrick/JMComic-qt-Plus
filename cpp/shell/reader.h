// 本地/在线阅读器：9 种阅读模式 + 双指缩放 + 滑动翻页 + 拖动滚动/平移。
//
// 对齐对象：python/src/view/read/read_enum.py（ReadMode 9 种）、
//           python/src/view/read/read_graphics.py（布局/滚轮/缩放）、
//           python/src/view/read/read_tool.py（翻页方向/自动滚动）、
//           python/src/tools/mobile_ui_reader.py（触控状态机）。
//
// 设计约束：
//   * 只用 Qt Widgets，单视口自绘（没有子 QLabel），这样滚动模式下相邻页不会
//     把两根手指拆成两条触摸序列 —— Python 版为此专门按触点 id 合并，这里
//     结构上就不存在该问题；
//   * 触控完全基于 QTouchEvent：1 指拖动 = 滚动/平移，1 指点按 = 三分区，
//     2 指 = 缩放（手势中只做视图预览，抬手才重排），与 Python 语义一致；
//   * 翻页模式下滑动**一次手势只翻一页**：翻页轴在本手势第一次越过阈值时
//     锁定，翻过即作废，避免一滑跳过十几页；
//   * 阅读偏好落 ui.ini [reader]，与设置页共用。
#pragma once

#include <QAbstractScrollArea>
#include <QFutureWatcher>
#include <QHash>
#include <QImage>
#include <QPixmap>
#include <QPointF>
#include <QRectF>
#include <QSet>
#include <QSizeF>
#include <QStringList>
#include <QVector>

#include "sr_engine.h"

class QLabel;
class QMovie;
class QScrollBar;
class QTimer;
class QPropertyAnimation;
class QKeyEvent;
class QWheelEvent;
class QTouchEvent;

class ReaderView : public QAbstractScrollArea
{
    Q_OBJECT
public:
    // 数值必须与 Python ReadMode 一致（prefs / 设置页 / 回归参数共用）
    enum class ReadMode : int {
        UpDown = 0,           // 上下滚动（整章纵向连续）
        LeftRight = 1,        // 左右翻页（单页，默认）
        LeftRightDouble = 2,  // 左右双页：当前页在左
        RightLeftDouble = 3,  // 右左双页：当前页在右（滚轮/翻页方向按右左语义）
        LeftRightScroll = 4,  // 左右滚动（整章横向连续）
        RightLeftScroll = 5,  // 右左滚动（从右往左排）
        RightLeftDouble2 = 6, // 右左双页（滚轮正序）
        Samewight = 7,        // 等宽模式（单页，按宽度铺满，纵向滚动）
        RightLeftScroll2 = 8, // 右左滚动（滚轮正序）
    };
    // 兼容 C1 shell / 旧代码的三个名字
    static constexpr ReadMode Single = ReadMode::LeftRight;
    static constexpr ReadMode Spread = ReadMode::LeftRightDouble;
    static constexpr ReadMode Vertical = ReadMode::UpDown;

    explicit ReaderView(QWidget *parent = nullptr);

    // ---- 数据源 ----
    int LoadDir(const QString &dir);              // 扫目录（按文件名排序）
    int LoadFiles(const QStringList &files);      // 在线章节（顺序即页序）
    void LoadDefaultOrPick();
    void OpenPicker();

    // ---- 渐进加载（在线章节）----
    // 整章几十张图，等全部下完才显示会让首页那几十秒都是空的。
    // 这里先按总页数建占位页（当前页画"加载中…"），每页就绪即补一张；
    // 当前页/当前屏的文件一到就立刻解码重排 —— 用户先看到第 1 页，其余页在后面补齐。
    void BeginProgressive(int total);
    bool SetProgressivePage(int index, const QString &path);
    // 收敛：整章下载结束后把仍然缺页的占位摘掉（失败页不留"加载中…"）
    void EndProgressive();
    bool IsProgressive() const { return progressive_; }
    int PendingPages() const;                     // 还没到货的页数
    bool CurrentHasImage() const;                 // 当前页是否已经解码完成

    // ---- 翻页 ----
    void NextPage();
    void PrevPage();
    void FirstPage();
    void LastPage();
    void ScrollToPage(int index, bool animate = true);

    // ---- 缩放（百分比，20%~500%，与 Python 滑块同范围）----
    void ZoomIn();
    void ZoomOut();
    void ResetZoom();
    void SetZoomPercent(int percent, bool persist = true);
    int ZoomPercent() const { return qRound(zoom_ * 100.0); }
    bool IsZoomed() const { return zoom_ > 1.001; }
    // "恢复原尺寸"是否还有可还原的东西：放大**或缩小**都算偏离 100%。
    // 不要拿 IsZoomed() 当这个判据 —— 它只覆盖放大（>100%），缩小状态下会返回
    // false，按钮因此被禁用，点击（含回归里的 click()）直接被 Qt 吞掉。
    bool CanResetZoom() const { return zoom_ < 0.999 || zoom_ > 1.001 || pinching_; }

    // ---- 阅读模式 ----
    void SetReadMode(ReadMode m, bool persist = true);
    void SetReadMode(int m, bool persist = true)
    {
        SetReadMode(static_cast<ReadMode>(qBound(0, m, 8)), persist);
    }
    ReadMode Mode() const { return mode_; }
    int ModeValue() const { return int(mode_); }
    bool IsVertical() const { return mode_ == ReadMode::UpDown || mode_ == ReadMode::Samewight; }
    bool IsScrollMode() const { return IsScrollModeValue(int(mode_)); }
    bool IsDoublePage() const { return IsDoubleMode(int(mode_)); }
    bool IsRtl() const { return IsRtlMode(int(mode_)); }
    void ToggleTwoPage();        // 单页 <-> 左右双页（兼容入口）
    void ToggleVertical();       // 上下滚动 <-> 左右翻页（兼容入口）
    void ToggleDirection();      // 在左右/右左变体之间切换（无变体时提示）
    void SetDirection(bool rtl); // 显式设置方向（设置页用）
    void ToggleDoublePageAlign();// F10：双页配对偏移一页（封面单独一页）

    static bool IsDoubleMode(int mode);
    static bool IsRtlMode(int mode);
    static bool IsScrollModeValue(int mode);
    static bool IsHorizontalPage(int mode);   // 横向翻页模式（滑动轴 = 横向）
    static int ForwardDelta(int mode, int value); // 语义"下一屏为正" -> 滚动条增量
    static QString ModeName(int mode);
    static QString ModeShortName(int mode);   // 工具条窄按钮用
    static QStringList ModeNames();

    // ---- 亮度（C++ 版保留的额外能力）----
    void SetBrightness(double v);
    void CycleBrightness();

    // ---- 触控/滚动辅助 ----
    QWidget *Viewport() const { return viewport(); }
    void ScrollByHalf(int dir);      // dir: +1 下/右半屏，-1 上/左半屏
    void ScrollSemantic(int delta);  // 正数 = 下一屏（自动按模式换算轴向/方向）

    // ---- 自动滚动 / 自动翻页 ----
    void SetAutoScroll(bool on);
    bool IsAutoScroll() const { return autoScroll_; }

    // ---- 超分（waifu2x 2x，后台线程）----
    void ToggleSrCurrent();          // F2 / 菜单：本张开/关
    void SetAutoSr(bool on);
    bool IsAutoSr() const { return autoSr_; }
    bool CurrentHasSr() const;
    bool CurrentSrPending() const;
    QString SrStatusText() const;     // 不可用原因 / 处理中 / 已应用 / 未开启
    static bool PrefAutoSr();
    static void SetPrefAutoSr(bool on);

    // ---- 当前页信息 / 复制 ----
    QSize CurrentImageSize() const;
    qint64 CurrentFileSize() const;
    QString CurrentFilePath() const;
    bool CurrentIsAnimated() const;
    QByteArray CurrentRawBytes() const;
    bool CopyCurrentToClipboard();
    bool SaveCurrentTo(const QString &path);
    bool SaveCurrentToLastDir();   // F4：记住上次另存目录，不再弹框

    // ---- 偏好（ui.ini [reader]）----
    static int PrefMode();
    static void SetPrefMode(int mode);
    static int PrefTurnMs();
    static void SetPrefTurnMs(int ms);
    static int PrefScrollStep();
    static void SetPrefScrollStep(int px);
    static int PrefScrollMs();
    static void SetPrefScrollMs(int ms);
    static int PrefManualStep();
    static void SetPrefManualStep(int px);
    static int PrefZoom();
    static void SetPrefZoom(int percent);

    // ---- 回归入口（合成手势，走与真实事件相同的状态机）----
    void SimulateTap(qreal fx, qreal fy);
    void SimulateSwipe(int dx, int dy, int steps = 8);
    void SimulatePinch(qreal ratio);
    // 走真实 QTouchEvent 投递路径（TouchMerge/TouchSync/TouchSingleMove）
    void RunTouchEventSelfTest();
    void RunParitySelfTest();
    // 渐进加载自检（不依赖网络）：用本地目录扮演"第 1 页先到、后面页陆续到、末尾缺页"
    void RunProgressiveSelfTest(const QString &dir);

    int PageCount() const { return files_.size(); }
    int CurrentPage() const { return current_ >= 0 ? current_ + 1 : 0; }

signals:
    void PageChanged(int current, int total, const QString &path);
    void StatusMessage(const QString &text);
    void SelfTestDone();
    void ZoomChanged(int percent);
    void ModeChanged(int mode);
    void SrStateChanged(int index, bool ready);
    // 点按三分区：left-top/right-top（显隐工具条）、mid-top/mid-bottom（半屏）、
    // left-bottom/right-bottom（上一页/下一页）
    void TapZone(const QString &zone, const QPoint &pos);
    // 翻到章末/章首：由外壳决定是否进入相邻章节（Python 版会自动跳）
    void RequestNextChapter();
    void RequestPrevChapter();
    void RequestToggleChrome();
    void RequestFullScreen();
    void RequestBack();
    void RequestContextMenu(const QPoint &globalPos);
    void RequestSaveAs();

protected:
    void keyPressEvent(QKeyEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    bool viewportEvent(QEvent *event) override;

private:
    // ---- 布局与绘制 ----
    void Relayout();
    void ScrollToCurrent();
    void UpdateCurrentFromScroll();
    int PageAtScrollPos() const;
    void Draw(QPainter &painter);
    // 换图前的视口快照淡出：翻页（非滚动模式）、切阅读模式、切超分都用它，
    // 让"内容被替换"这件事有一次可读的过渡，而不是整屏瞬间变掉。
    void BeginViewportFade();
    void DrawPage(QPainter &painter, int index);
    void DrawPlaceholder(QPainter &painter, int index);
    void UpdateGif();
    void ClearGif();

    // ---- 图片缓存 ----
    QImage ImageAt(int index);
    void EnsureCache(int center);
    void TrimCache();

    // ---- 触控 ----
    void HandleTouch(QTouchEvent *event);
    void TouchMerge(QTouchEvent *event, QList<QPair<int, QPointF>> *released);
    void TouchSingleMove();
    void TouchSync();
    void StartPinch();
    void ApplyPinch();
    void FinishPinch();
    void ResetPinch();
    void EmitTap(const QPointF &local);

    void HandleMousePress(QMouseEvent *event);
    void HandleMouseMove(QMouseEvent *event);
    void HandleMouseRelease(QMouseEvent *event);
    void ScrollDrag(qreal dx, qreal dy);

    // ---- 自动滚动 ----
    void StartAuto();
    void StopAuto();
    void AutoTick();

    // ---- 超分 ----
    void StartSr(int index);
    void OnSrFinished();
    void MaybeAutoSr();
    void TrimSrCache();

    void AnimateScroll(QScrollBar *bar, int target, int ms = 200);
    QStringList ImageFilesIn(const QString &dir) const;
    void ShowPage(int index, bool keepScroll = false);
    void StepPage(int dir, bool semanticNext);
    void AtEdge(bool forward);
    // 重排后把视口锚回"当前页的同一个相对位置"：渐进加载时后续页的真实尺寸
    // 会替换估值，直接 Relayout 会让内容在手指底下跳走。
    void RelayoutAnchored();

    QStringList files_;
    int current_ = -1;
    double zoom_ = 1.0;
    double brightness_ = 1.0;
    ReadMode mode_ = ReadMode::LeftRight;
    // 渐进加载：files_ 里允许有空串（该页还没下完）。progressive_ 为真时
    // EndProgressive() 才会把缺页占位摘掉。
    bool progressive_ = false;

    QVector<QRectF> rects_;     // 每页在内容坐标系里的矩形
    QSizeF content_;            // 内容总尺寸（>= 视口）
    bool navGuard_ = false;     // 程序化滚动时抑制"滚动 -> 换页"
    QPropertyAnimation *scrollAni_ = nullptr;
    bool scrollAnimating_ = false;

    QHash<int, QImage> cache_;
    QList<int> cacheOrder_;     // MRU 在前
    qint64 cacheBytes_ = 0;
    QHash<int, bool> animFlags_;

    QLabel *gifLabel_[2] = {nullptr, nullptr};
    QMovie *gifMovie_[2] = {nullptr, nullptr};
    int gifPage_[2] = {-1, -1};

    QTimer *autoTimer_ = nullptr;
    bool autoScroll_ = false;
    QString lastSaveDir_;

    // 视口过渡（见 BeginViewportFade）
    QPixmap fadeShot_;
    qreal fadeAlpha_ = 0.0;
    bool fadeNextSwap_ = false;

    QSet<int> srWanted_;
    QSet<int> srPending_;
    QHash<int, QImage> srCache_;
    QFutureWatcher<SrEngine::Result> *srWatcher_ = nullptr;
    int srJobPage_ = -1;
    int srQueued_ = -1;
    bool autoSr_ = false;

    // 触控状态机
    QHash<int, QPointF> touchPts_;   // id -> 全局坐标
    int touchPrimary_ = -1;
    QPointF touchPress_;
    QPointF touchLast_;
    bool touchDragging_ = false;
    bool pinching_ = false;
    bool swallowTouch_ = false;
    double pinchStartDist_ = 0.0;
    double pinchBaseZoom_ = 1.0;
    double pinchRatio_ = 1.0;
    QPointF pinchAnchor_;
    QPointF pinchSceneAnchor_;
    double turnAccumX_ = 0.0;
    double turnAccumY_ = 0.0;
    int turnAxis_ = 0;         // 0 未锁定 / 1 横向 / 2 纵向
    bool turnFired_ = false;

    // 鼠标（桌面等价于单指）
    QPointF mousePress_;
    QPointF mouseLast_;
    bool mouseDown_ = false;
    bool mouseDragging_ = false;
};
