// 主窗口：无边框 + 自绘标题栏 + 左侧导航栏 + 页面栈 + 返回栈。
//
// 与 Python 版 main_view 的对应关系：
//   navigationWidget（左侧 260px 侧栏，图标 + 文字） → NavRail（84px，图标在上文字在下）
//   subStackWidget（QStackedWidget + subStackList） → pages_ + history_（返回栈）
//   titleBar（QFramelessWindow）                   → TitleBar（自绘，支持拖动/最大化/贴边缩放）
#pragma once

#include <QPixmap>
#include <QSize>
#include <QVector>
#include <QWidget>

#include "ui_pages.h"

class QStackedWidget;
class QToolButton;
class QLabel;
class QHBoxLayout;
class QScrollArea;
class QVBoxLayout;
class PageTransition;
class TouchAdapter;
struct JmUserInfo;

class MainWindow : public QWidget
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void Go(PageId id, bool pushHistory = true);
    void Back();
    void ShotTo(const QString &path, int delayMs = 600);
    void ScrollDemo(int times);
    void DemoCollapseNav();      // 收起侧栏（截图用）
    void ScrollNavToEnd();       // 侧栏滚到底（验证三段导航都在）
    void SelectHomeSection(int index);
    // 回归：首页合成一次横滑 / 冻结在分区 from→to 的进度（--home-swipe / --home-freeze）
    void HomeSwipe(int dx, int dy = 0);
    // 首页分区数据是否已到（截图冻结/合成手势要等它）
    bool HomeReady() const;
    void HomeFreeze(int from, int to, qreal progress);

    // ---- 换页动画（QML 场景图，见 page_transition.h） ----
    void SetAnimationEnabled(bool on);
    bool AnimationEnabled() const { return animEnabled_; }
    void FreezeTransition(qreal progress);
    // 截图/回归：从 from 无动画跳到起点，再动画切到 to；progress >= 0 时冻结在该进度。
    // forceMode: -1 = 按 backward 推语义；0 = 前进；1 = 返回；2 = 同级淡入。
    void DemoTransition(PageId from, PageId to, qreal progress, int durationMs = 0,
                        bool backward = false, int forceMode = -1);

    // ---- 触摸适配层（见 touch.h） ----
    void EnableTouchMode(bool on);
    void ShowEdgeHint(qreal progress);
    void SetSafeAreaOverride(int left, int top, int right, int bottom);
    bool SimulateBackSwipe();   // 回归：把边缘手势的"结果"跑一遍
    PageId CurrentPage() const { return current_; }
    // 回归：在阅读页合成一次点按（fx/fy 是 viewport 的比例坐标）
    void DemoReaderTap(qreal fx, qreal fy);
    // 侧栏屏蔽词（正常路径是点"屏蔽词"那一行）
    void EditHideWords();
    // 打卡（真实 /daily_chk）。未登录时提示并跳登录页。
    void SignIn();
    // 回归：当前打卡按钮文案（"打卡" / "已打卡" / "打卡中…"）
    QString SignButtonText() const;
    // 回归：ASCII 状态（unsigned / signed / busy / none），避免控制台编码干扰断言
    QString SignButtonState() const;
    // 回归：侧栏三条可交互行（分流×2、屏蔽词）中"带边框"的数量
    int NavActionCount() const;
    // 回归：点一次某条侧栏动作行（proxy / proxyImg / hide）
    bool ClickNavAction(const QString &actionId);
    // 回归：阅读方式（0 单页 / 1 双页 / 2 上下滚动），设置页与阅读器同步
    void SetReadingModeForTest(int mode);
    // 阅读器回归：本地目录建场 + 9 模式/手势自检（--reader-dir / --reader-parity）
    void LoadReaderDirForTest(const QString &dir);
    void RunReaderParityForTest();
    void RunReaderTouchForTest();
    void RunReaderProgressiveForTest(const QString &dir);
    void ReaderSwipeForTest(int dx, int dy);
    void ReaderPinchForTest(qreal ratio);
    void ReaderSrForTest();
    QString ReaderSrStatusForTest();
    // 截图用：直接弹开阅读器右上角菜单（验证"标题栏三项 + 菜单首块图片详情"）
    void ReaderMenuForTest();
    int ReaderModeForTest() const;
    // 回归：首页顶部分区栏（横滑 / 点按）
    void HomeStripDrag(int dx);
    QString HomeGeometryReport() const;
    QString HomeStripState() const;
    void HomeStripTap(int x);
    // 回归：菜单栏防误触（拖动超阈值不跳页 / 阈值内的点击照常生效）
    bool TapGuardSelfTest();
    // 回归：抽屉打开状态下点一次导航项（必须走"先收抽屉、再播同级淡入"的路径）
    void DrawerNavForTest(PageId id);
    // 回归：点一次阅读器标题栏的"恢复原尺寸"按钮，打印缩放结果
    void ClickReaderFitForTest();
    // 回归：按 PageId 取页面实例（取设置页做断言）
    PageBase *FindPage(PageId id) const;
    void DemoOpenDrawer();     // 截图：展开抽屉
    // 冒烟：开→Esc 关→再开→点遮罩关，逐条打印状态（返回是否全部通过）
    bool DrawerSelfTest();
    void EnableDrawerMode(bool on) { SetDrawerMode(on); }
    // 接服务端数据：/promote + /latest；失败时首页进入错误态（不保留任何本地样例）。
    void LoadRealData();

    // 下载队列：详情页"下载"把章节下标交到这里，转成任务并入队
    void EnqueueDownload(const Comic &c, const QVector<int> &chapterIndexes);
    class DownloadManager *Downloads() const { return downloads_; }
    // 回归：打印下载队列状态（--dl-report）
    QString DownloadReport() const;
    // 回归：强制图床 host 顺序（--img-host，逗号分隔）
    void SetImageHostsForTest(const QStringList &hosts);
    // 回归：按 album id 拉详情并下载前 N 话，等完成（--dl-selftest）
    bool DownloadSelfTest(const QString &albumId, int chapters, int timeoutMs);

    // ---- 业务闭环入口（CLI/冒烟与交互共用同一条路径） ----
    void OpenAlbumById(const QString &albumId);   // 打开详情并拉 /album
    void OpenChapterById(const QString &chapterId, const QString &label);  // 拉 /chapter 并读图
    // 章末/章首继续翻：按当前章节在详情页章节表里的位置取相邻一章（Python 自动跳章）
    void OpenAdjacentChapter(int delta);
    // 阅读器全屏（F11 / 工具条按钮）
    void ToggleReaderFullScreen();
    void RealSearch(const QString &keyword);      // 走 /search（结果由信号回填）
    void RealLogin(const QString &user, const QString &pass);   // 走 /login
    // 回归：把侧栏用户区 + 收藏页状态打成一行（不依赖截图文字识别）
    QString UserReport() const;
    // 冒烟：模拟点一次首页"加载更多"（等价于按钮的 LoadMoreLatest(1)）
    void LoadMoreLatest();
    // 分流与节点：线路/节点测速、选用（同一份配置同时供 JmApi 与图片下载使用）
    void OpenProxyPage();
    // 截图/验收：打开对话框并立刻跑一次测速（kind = lines|nodes）
    void DemoProxyTest(const QString &kind);
    // 验收/自动化：强制走 CDN 线路并把节点设为该 IP（验证节点真正作用于流量）
    void UseNodeForTest(const QString &ip);
    // 确定性自检：用人造数据验证"排序 + 选优"策略（不依赖网络）
    void ProxySelfTest();
    // HTTP/3 over QUIC 探针（--h3-probe 用）：一个请求、打日志、退出前收口
    void H3Probe(const QString &host, const QString &path, bool useEch);
    // 连接池验收（--h3-burst 用）：并发 N 个同域请求，输出握手/复用/排队计数。
    // 期望 1 次握手、N-1 次复用；有排队说明并发超过单连接流上限。
    void H3Burst(const QString &host, const QString &path, int count);
    // 首包采样（--h3-bench 用）：串行 N 次「清池 + 新 Client」，输出 P50/P95。
    // 与 H3Burst 互补——burst 测复用，bench 测完整握手成本（Q3）。
    void H3Bench(const QString &host, const QString &path, int count);
    // 用支持 ECH 的 OpenSSL 4.x 通道直连 host:443，输出 ECH 状态/TLS 版本/ALPN/证书校验
    void EchProbe(const QString &host);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    void UpdateMobileBreakpoint();
    void showEvent(QShowEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void keyPressEvent(QKeyEvent *e) override;

private:
    // 换页语义：决定用哪种转场（见 page_transition.h 的 Mode）。
    // Push = 进入下一层，Pop = 返回上一层，Lateral = 左侧导航/面包屑同级跳转。
    enum class NavMotion { Push, Pop, Lateral };

    QWidget *BuildTitleBar();
    void SwitchTo(int index);
    void ApplyPage(int index);
    void LandPending();
    void ApplySafeArea();
    void UpdateNavMetrics();
    void UpdateNavUser();
    void UpdateSignButton();
    void PickProxy(bool imageDomain);

    QPixmap CapturePage(QWidget *page);
    QWidget *BuildNavRail();
    QWidget *BuildCrumbBar();
    void RebuildCrumbs();
    void RebuildCrumbsNow();
    void GoToCrumb(int index);
    void ToggleNav();
    // ---- 手机竖屏：导航栏改为浮动抽屉（对应 Python tools/mobile_ui_drawer.py） ----
    void SetDrawerMode(bool on);
    bool IsDrawerMode() const { return drawerMode_; }
    bool IsDrawerOpen() const { return drawerOpen_; }
    void OpenDrawer();
    void CloseDrawer();
    void ToggleDrawer();
    void UpdateDrawerGeometry();
    void BuildPages();
    void WirePage(PageBase *page, PageId id);
    void EnsureApi();
    void ApplyProxySettings();
    // 登录响应（或会话缓存）→ UserSession + 侧栏 + 落库；refreshFavorites=false 时
    // 不触发 /favorite（后台校准用）
    void ApplyUserProfile(const JmUserInfo &user, bool refreshFavorites);
    // 启动后校验一次会话（/favorite 的 401 是唯一可验证的"会话失效"信号）
    void ProbeSession();
    void OpenDetail(const Comic &c);
    void RefreshFavoriteList();
    // /album 的真实章节数 → 首页/搜索/收藏里同 id 的卡片（按 id 局部刷新）
    void ApplyChapterCount(const QString &id, int chapters);
    void UpdateNavState();
    void UpdateIcons();
    Qt::Edges EdgeAt(const QPoint &pos) const;

    QStackedWidget *stack_ = nullptr;
    QVBoxLayout *rootLayout_ = nullptr;

    PageTransition *transition_ = nullptr;
    TouchAdapter *touch_ = nullptr;

    // 动画期间：目标页还没真正切过去（要等动画结束才切，避免新页首次布局卡住动画）
    int pendingIdx_ = -1;
    QWidget *leavingPage_ = nullptr;
    QSize animSize_;
    qint64 animStartedMs_ = 0;
    bool animEnabled_ = true;
    QVector<PageBase *> pages_;
    QVector<PageId> history_;
    PageId current_ = PageId::Home;

    // 本次换页的动效语义（Go 里定，SwitchTo 里消费）
    NavMotion navMotion_ = NavMotion::Push;
    // 同级跳转的方向：true = 目标在导航列表更下方（整页从右滑入）
    bool lateralForward_ = true;
    NavMotion nextMotion_ = NavMotion::Push;
    bool hasNextMotion_ = false;
    bool backNext_ = false;      // Back() 置位：下一次 Go 按"返回"处理

    QWidget *titleBar_ = nullptr;
    QWidget *navRail_ = nullptr;
    QLabel *navBrand_ = nullptr;
    // 导航项：顺序 = kNav（与 Python navigation_widget 的按钮顺序一致）
    QVector<QPair<PageId, QToolButton *>> navItems_;
    QVector<QWidget *> navSeparators_;
    QLabel *navAvatar_ = nullptr;
    QScrollArea *navScroll_ = nullptr;
    // 用户区六行键值 + 用户名/称号：登录态变化时统一刷新
    QLabel *navName_ = nullptr;
    QLabel *navTitle_ = nullptr;
    QLabel *navCoins_ = nullptr;
    QLabel *navLevel_ = nullptr;
    QLabel *navFav_ = nullptr;
    QLabel *navProxy_ = nullptr;
    QLabel *navProxyImg_ = nullptr;
    QLabel *navHide_ = nullptr;
    // 侧栏用户区里"可交互"的三行（分流×2 / 屏蔽词）：整行带边框 + 可点
    QVector<QWidget *> navActions_;
    QPushButton *navAuthBtn_ = nullptr;
    QPushButton *signBtn_ = nullptr;
    QPoint navActionPress_;
    bool navActionPressed_ = false;
    QString navActionPressedId_;

    // 面包屑条（对应 Python main_view 的 menuLayout：menuButton + 各级页面标题）
    QWidget *crumbBar_ = nullptr;
    QHBoxLayout *crumbLayout_ = nullptr;
    QToolButton *crumbMenu_ = nullptr;
    bool navCollapsed_ = false;

    // 抽屉：drawerWidth/Top 与 Python 的 _jmDrawerWidth/_jmDrawerTop 同义
    QHBoxLayout *bodyLayout_ = nullptr;
    bool drawerMode_ = false;
    class NetClient *net_ = nullptr;
    class JmApi *api_ = nullptr;
    class PageLoader *pageLoader_ = nullptr;
    class DownloadManager *downloads_ = nullptr;
    QString detailComicId_;      // 当前详情页的真实 album id（丢弃过期响应）
    QString pendingFavId_;
    bool pendingFavWanted_ = false;
    bool sessionProbed_ = false; // 启动会话校验只跑一次
    // 首页数据请求的连接：重试时先断开旧的，避免重复回调
    QMetaObject::Connection latestConn_;
    QMetaObject::Connection promoteConn_;
    bool drawerOpen_ = false;
    qint64 lastBackMs_ = 0;      // Android back 去重（一次按键会被投递两次）
    qint64 lastBackNavMs_ = 0;   // 上一次"由返回键触发的导航"时间（首页双击退出的判定）
    int drawerWidth_ = 0;
    int drawerTop_ = 0;
    QWidget *drawerScrim_ = nullptr;
    class QPropertyAnimation *drawerAni_ = nullptr;

    // 页面实例（需要跨页传参的那几个）
    DetailPage *detail_ = nullptr;
    ReaderPage *reader_ = nullptr;
    HomePage *home_ = nullptr;
    SearchPage *search_ = nullptr;

    // 阅读器当前章节：章末"下一章"据此定位
    QString currentChapterId_;
    QString currentChapterLabel_;
    int resumePage_ = 1;
    bool resumePending_ = false;
    bool readerFullScreen_ = false;
};
