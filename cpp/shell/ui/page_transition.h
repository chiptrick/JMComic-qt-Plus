// 换页动画：QML 场景图实现（替代 QWidget 侧的手写 GPU 覆盖层）。
//
// 分层：
//   PageSwipe.qml        场景（两张 Image + 位移）与时间轴（NumberAnimation）
//   PageTransition       QQuickWidget 宿主 + 快照投递 + 首帧/失败处理
//   MainWindow::SwitchTo 快照抓取、页面栈切换时机、失败回退
//
// 为什么用 QQuickWidget 而不是 QQuickView + createWindowContainer：
//   * 覆盖层是**不透明**的（两张整页快照铺满页面区），不需要透明合成，
//     但需要跟 QStackedWidget 严格同几何、同 z 序，QQuickWidget 是普通子控件，
//     这两点自然满足；native window container 在部分平台会和 widget 抢 z 序。
//   * 代价是每帧多一次全屏 FBO 合成（QQuickWidget 先渲到纹理再合成）。
//     桌面实测可接受；设备侧若成为瓶颈，可换成 QQuickView + 原生窗口，
//     因为覆盖层不透明，这条路不需要 alpha 合成。
//
// 失败降级：
//   1. QML 场景加载失败（缺 Qt Quick 插件）→ IsReady() 为 false，调用方直接切页；
//   2. 首帧 120ms 内没画出来 → 先用 grabFramebuffer() 强制渲染一次，
//      仍失败则本次动画放弃（连续两次后本进程不再尝试）。
//   与 Python 版的差别：那边还有一套 raster 覆盖层兜底；这里不重复实现，
//   因为 Qt Quick 缺失时 raster 覆盖层同样只能靠 CPU 逐帧重绘整页（实测 30ms/帧），
//   体验还不如直接切页。
#pragma once

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPixmap>
#include <QQuickImageProvider>
#include <QString>
#include <QUrl>

class QQuickItem;
class QQuickWidget;
class QTimer;
class QWidget;

// 页面快照的纹理来源。每次 Begin 用新的 id，避免 QML 端命中旧纹理。
class PageImageProvider : public QQuickImageProvider
{
public:
    PageImageProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
    void Put(const QString &id, const QImage &image);

private:
    QMutex mutex_;
    QHash<QString, QImage> images_;
    QHash<QString, qint64> order_;   // id → 序号，用于淘汰
    qint64 seq_ = 0;
};

class PageTransition : public QObject
{
    Q_OBJECT
public:
    // 三种语义（C++ 版按自己的页面栈模型定义，不是照搬 Python 的两向位移）：
    //   Push  进入下一层（详情、阅读）—— 新页从右侧推入
    //   Pop   返回上一层（返回键 / 边缘手势 / 面包屑回退）—— 新页从左侧推入
    //   Fade  无位移交叉淡入。同级导航已改用位移（Lateral 语义 = 按方向选
    //         Push/Pop，时长 Motion::Slow），Fade 只留给截图/回归对照。
    // 同级跳转的方向由"导航列表顺序"决定（见 MainWindow::NavRank）：目标在下方
    // 就从右侧滑入，避免用户失去"我去了哪一屏"的空间感。
    enum class Mode { Push, Pop, Fade };
    enum class Direction { Forward, Backward };

    // host 是"要被覆盖的那块区域"（页面栈）。覆盖层挂到 host 所在窗口上，
    // 几何跟随 host，这样它不会被 QStackedWidget 的切页影响。
    explicit PageTransition(QWidget *host);
    ~PageTransition() override;

    bool IsReady() const { return ready_; }
    void SetAnimationEnabled(bool on) { animEnabled_ = on; }
    bool AnimationEnabled() const { return animEnabled_; }

    void SetDuration(int ms) { durationMs_ = ms; }
    int Duration() const { return durationMs_; }

    bool IsRunning() const { return running_; }

    // 返回 false：这次没有动画（场景不可用 / 快照为空），调用方直接切页
    bool Begin(const QPixmap &prev, const QPixmap &next, Mode mode);
    bool Begin(const QPixmap &prev, const QPixmap &next, Direction dir)
    {
        return Begin(prev, next, dir == Direction::Forward ? Mode::Push : Mode::Pop);
    }
    void End();

    // 截图/回归：冻结在指定进度，不触发 Finished
    void FreezeAt(qreal progress);
    bool IsFrozen() const { return frozen_; }

signals:
    void FirstFrame();   // 覆盖层已有内容，调用方此刻才可以藏旧页
    void Finished();     // 动画播完，调用方此时切页并撤覆盖层
    void Failed(const QString &why);

protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;

private slots:
    void OnFrameSwapped();
    void OnSceneFinished();
    void OnWatchdog();

private:
    bool EnsureScene();
    void SyncGeometry();

    QWidget *host_ = nullptr;
    QQuickWidget *view_ = nullptr;
    PageImageProvider *provider_ = nullptr;
    QQuickItem *root_ = nullptr;
    QTimer *watchdog_ = nullptr;

    QString prevId_;
    QString nextId_;
    quint64 generation_ = 0;
    int durationMs_ = 250;

    bool animEnabled_ = true;
    bool ready_ = false;      // QML 场景可用
    bool running_ = false;
    bool frozen_ = false;
    bool firstFrame_ = false;
    int softFails_ = 0;       // 连续"没画出帧"的次数，到 2 次就不再尝试
};
