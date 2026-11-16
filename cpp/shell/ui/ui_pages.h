// 页面层：与 Python `src/view/` 一一对应的 C++ 页面。
//
// 覆盖范围见 docs/cpp_ui_parity.md 的对照表；每个类只负责"这一页的张什么样、
// 点了之后去哪"。数据只由 JmApi 的服务端结果（或本地真实扫描）回填，页面本身
// 不持有任何样例条目：未取到数据时显示加载态/空态，取失败时显示错误态。
#pragma once

#include <QImage>
#include <QStringList>
#include <QWidget>

#include "../jm_api.h"
#include "../sr_engine.h"
#include "comic_view.h"
#include "models.h"
#include "nas_store.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QBoxLayout;
class QResizeEvent;
class QScrollArea;
class QStackedWidget;
class QTabWidget;
class QVBoxLayout;
class QComboBox;
class QSpinBox;
class QSlider;
class QMenu;
class QProgressBar;
template <typename T>
class QFutureWatcher;
class ReaderView;
class SectionStrip;
class SectionSwipe;
class EmptyState;
class ElideLabel;
class FlowLayout;
class SectionHeader;

enum class PageId {
    Home = 0,
    Category,
    Search,
    Detail,
    Reader,
    Download,
    Local,
    Favorite,
    History,
    Comment,
    Sr,
    Settings,
    About,
    Login,
    // 以下为对齐 Python 导航栏补齐的页面（顺序与 navigation_widget 的按钮顺序一致）
    MyComment,       // 我的评论
    RemoteHistory,   // 观看记录
    LocalFavorite,   // 本地收藏
    Week,            // 每周必看
    Nas,             // 网络存储
    BatchSr,         // 批量超分
    Help,            // 帮助
    Proxy,           // 分流与节点（原本是模态对话框，真机上点不动，改成页）
};

class PageBase : public QWidget
{
    Q_OBJECT
public:
    explicit PageBase(QWidget *parent = nullptr) : QWidget(parent) {}
    virtual void OnEnter() {}
    virtual void OnLeave() {}

signals:
    void Navigate(PageId id);
    void Toast(const QString &text);
};

// ---------------------------------------------------------------- 首页
class HomePage : public PageBase
{
    Q_OBJECT
public:
    explicit HomePage(QWidget *parent = nullptr);
    void OnEnter() override;
    void ScrollToBottom();   // 冒烟：触发一次 fetchMore
    int ItemCount() const;   // 当前已加载条数（分页验证用）
    int SectionCount() const { return sections_.size(); }
    int CurrentSection() const;
    void SelectSection(int index);   // 截图/冒烟：切到指定分区
    // 服务端数据回填：/promote + /latest 的结果重建全部 tab；空列表视为失败
    void SetSections(const QVector<HomeSection> &sections);
    // /latest 翻页：追加到"最新上传"网格，不重建 tab
    void AppendLatest(const QVector<Comic> &items, int page);
    // 首屏状态：加载中 / 加载失败（带"重试"动作）。两者都不渲染任何条目。
    void SetLoading(const QString &text = QString());
    void SetFailed(const QString &message);
    bool HasSections() const { return !sections_.isEmpty(); }
    // /album 拿到真实章节数后，刷新所有分区里同 id 的卡片（含之后的分区切换）
    void SetChapters(const QString &id, int chapters);

    // 回归/自检：顶部分区栏（自绘、可横向拖动）的状态
    int StripOffset() const;
    int StripContentWidth() const;
    bool StripDragging() const;
    void StripDrag(int dx);          // 合成一次横向拖动（不切换分区）
    void StripTap(int x);            // 合成一次点按（命中才切换）
    int StripHitTest(int x) const;   // 合成点击命中的分区下标（-1 = 空白）
    // 回归：首页几何（页面 / 分区栏 / QTabWidget / 页面区 / 底部状态条）
    QString GeometryReport() const;
    QString StripStateReport() const;
    // 回归：在首页合成一次横滑 / 冻结在 分区 from→to 的指定进度
    void SwipeDemo(int dx, int dy = 0);
    void FreezeSection(int from, int to, qreal progress);

signals:
    void OpenComic(const Comic &c);
    void LoadMoreLatest(int nextPage);
    void RetryRequested();

private:
    // 对应 Python IndexView.AddTab()：一个分区 = 一个 QWidget(grid [+ 翻页行])
    QWidget *BuildSection(const HomeSection &sec);
    void ShowPlaceholder(const QString &title, const QString &text, const QString &action);
    void UpdateLatestStatus();

    // 分区容器：每个分区一整页（分区栏 + 内容），切换时整页横向滑
    class SectionSwipe *deck_ = nullptr;
    class LoadingBar *loading_ = nullptr; // 首屏拉取中的扫描线（3px）
    QPushButton *latestMore_ = nullptr;   // "加载更多"（最新上传）
    QLabel *latestStatus_ = nullptr;      // "第 N 页 · 已加载 M 部"（真实分页状态）
    ComicGrid *latestGrid_ = nullptr;     // /latest 那一格（翻页只追加到它）
    QWidget *emptyHost_ = nullptr;        // 加载中 / 失败占位
    int latestPage_ = 0;
    QVector<ComicGrid *> grids_;
    QVector<HomeSection> sections_;
};

// ---------------------------------------------------------------- 分类
class CategoryPage : public PageBase
{
    Q_OBJECT
public:
    explicit CategoryPage(QWidget *parent = nullptr);

signals:
    void OpenComic(const Comic &c);
    // 题材/排序筛选交给外壳走 /search（页面本身不造结果）
    void FilterRequested(const QString &keyword, const QString &orderBy);
};

// ---------------------------------------------------------------- 搜索
class SearchPage : public PageBase
{
    Q_OBJECT
public:
    explicit SearchPage(QWidget *parent = nullptr);

    // 外部触发一次搜索（截图/冒烟用；正常由输入框回车触发）。
    // 只发 SearchRequested 交给外壳走 /search，页面不生成任何本地结果。
    void Search(const QString &keyword);

    // 服务端结果回填：append=true 表示翻页追加
    void SetResults(const QVector<Comic> &items, int total, int page, bool append);
    void SetSearching(const QString &keyword);
    // 请求失败：显示错误态 + "重试"，不回落演示结果
    void SetFailed(const QString &message);
    int Total() const { return total_; }
    // /album 的真实章节数回填到现有结果卡片（不重新请求 /search）
    void SetChapters(const QString &id, int chapters);

    // 每次进入搜索页都从磁盘重建「最近搜索」：搜索可能发生在别的进程/上次启动。
    void OnEnter() override;
    void ClearHistory();     // 「清空」文字链调用；CLI --history-clear 同义

signals:
    void OpenComic(const Comic &c);
    void SearchRequested(const QString &keyword, int page);

private:
    void RunSearch(const QString &kw);
    void ReloadHistory();
    class SearchBox *box_ = nullptr;
    ComicGrid *grid_ = nullptr;
    class LoadingBar *loading_ = nullptr;
    QWidget *historyBox_ = nullptr;
    QWidget *historyGroup_ = nullptr;   // 「最近搜索」标题 + chips（整组常驻，空时给占位文案）
    QWidget *histChips_ = nullptr;
    QLabel *histEmpty_ = nullptr;
    FlowLayout *histFlow_ = nullptr;
    QWidget *resultBox_ = nullptr;
    QLabel *resultHint_ = nullptr;
    QWidget *tailSpacer_ = nullptr;
    QPushButton *more_ = nullptr;
    QString keyword_;
    int page_ = 0;
    int total_ = 0;
};

// ---------------------------------------------------------------- 详情
struct JmAlbum;

class DetailPage : public PageBase
{
    Q_OBJECT
public:
    explicit DetailPage(QWidget *parent = nullptr);
    void ShowComic(const Comic &c);

    // 真实 /album 回填：标题/作者/简介/标签/章节列表/封面。
    // 章节的 id 就是 JM 的 photo id，阅读器据此走 /chapter 拉真实图片。
    void SetAlbum(const JmAlbum &a);
    void SetFavorite(bool on);   // 异步收藏成功后回填按钮状态
    int ChapterCount() const;
    QString ChapterIdAt(int index) const;   // 越界返回空
    QString ChapterNameAt(int index) const; // 越界返回空
    // 当前"本话"：最近一次点开的章节（没点过就是第 1 话）
    int CurrentChapterIndex() const { return currentChapter_; }

signals:
    void StartReading(const Comic &c, int chapterIndex);
    // 下载：一次可带多章（"下载全部"）；chapterIndexes 是 chapters_ 下标
    void DownloadRequested(const Comic &c, const QVector<int> &chapterIndexes);
    void FavoriteRequested(const Comic &c, bool wanted);
    // 点了标签 / 作者名：带关键词跳到搜索页并立刻发起第 1 页搜索。
    // 对齐 Python BookInfoView.ClickTagsItem / ClickAutorItem → QtOwner().OpenSearch2*()：
    // 关键词原样传入（作者名不拼前缀、不再追加"作品"之类的修饰），
    // 落到搜索页即自动搜索，用户不需要再点一次"搜索"。
    void SearchRequested(const QString &keyword);
    // 详情页"评论"：带着作品进入评论区（Python BookInfoView 的评论入口）
    void CommentsRequested(const Comic &c);

private:
    void BuildChapters();
    void BuildTags();      // 标签行：真实标签 → 可点 chip（点击 = 按标签名搜索）
    void UpdateCover();
    // 顶部元信息：章节数未到（/=0 未知）时不写"更新至 0 话"；作者名做成可点链接
    void UpdateMeta();
    // 顶部"封面 + 信息列"的横排/堆叠必须在**页面实际宽度**上算，
    // 且在窗口 size 变化时重算（见 .cpp 里 ApplyTopLayout 的注释）。
    void ApplyTopLayout();

protected:
    void resizeEvent(QResizeEvent *e) override;

private:
    Comic c_;
    QVector<Chapter> chapters_;   // 只来自 /album；空 = 显示"暂无章节"
    QString coverUrl_;
    QString lastCoverShown_;      // 已显示的封面（id|url），只有真变化才做淡入
    class SectionHeader *chapterHeader_ = nullptr;
    QLabel *cover_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *meta_ = nullptr;
    QLabel *desc_ = nullptr;
    QWidget *tagRow_ = nullptr;
    FlowLayout *tagFlow_ = nullptr;   // 标签多时自动换行（对齐 Python 版 FlowLayout）
    QWidget *chapterHost_ = nullptr;
    QPushButton *favBtn_ = nullptr;
    QBoxLayout *topLayout_ = nullptr;  // 封面 + 信息列的容器布局，运行时切方向（横排/堆叠）
    class QScrollArea *scroll_ = nullptr;   // 详情页的纵向滚动区（量可用宽度用）
    QWidget *body_ = nullptr;         // 滚动内容（页边距从这里读，避免与构造期口径不一致）
    bool topSide_ = false;            // 当前是否"左图右文"
    bool topApplied_ = false;         // 是否已按真实宽度算过一次
    bool topCheckPending_ = false;    // 已排了一次"布局稳定后再算"
    int currentChapter_ = 0;
};

// ---------------------------------------------------------------- 阅读器
class ReaderPage : public PageBase
{
    Q_OBJECT
public:
    explicit ReaderPage(QWidget *parent = nullptr);
    void OnEnter() override;

    // 在线章节：外壳先把章节图片下载/还原到本地，再交进来按顺序显示。
    // OpenRemote 只清屏并进入"加载中"状态。
    void OpenRemote(const QString &title);
    void ShowImages(const QString &title, const QStringList &files);
    void SetStatus(const QString &text);
    bool HasImages() const;

    // ---- 渐进加载（在线章节）----
    // 总页数一到就按页数铺占位页，之后每张图片就绪即补上；当前页一到立刻显示，
    // 不再等整章下载完成（见 ReaderView::BeginProgressive）。
    void BeginProgressive(int total);
    void SetPageReady(int index, const QString &path);
    void FinishProgressive(const QStringList &files);
    bool IsProgressive() const { return progressive_; }
    void SetLoadProgress(int done, int total);
    // 全屏：进入时收起工具条（只留图片），退出时恢复
    void SetFullScreenMode(bool on);

    // 阅读方式：0~8，数值与 Python ReadMode 一致（设置页与工具条共用）
    void SetReadingMode(int mode, bool persist = true);
    int ReadingMode() const;
    bool IsVerticalReading() const;
    void SetDirection(bool rtl);
    void SetZoomPercent(int percent);
    void SetAutoSr(bool on);
    int CurrentPage() const;
    int PageCount() const;
    // 章末/章首继续翻：外壳据此进入相邻章节（Python 的自动跳章）
    void SetChapterNavigation(bool hasPrev, bool hasNext);

    // 触摸点按分区（左/中/右 × 上/下）：回归入口，合成一次真实点按
    void TapDemo(qreal fx, qreal fy);

    // 继续阅读：外壳记住"章节 + 页码"，重开同章时跳回上次位置
    void ResumePage(int index);

    // 回归入口：本地目录建场 / 9 模式 + 手势自检
    int LoadDirForTest(const QString &dir);
    void RunReaderParitySelfTest();
    void RunReaderTouchSelfTest();
    void RunReaderProgressiveSelfTest(const QString &dir);
    void SwipeDemo(int dx, int dy);
    void PinchDemo(qreal ratio);
    void ToggleSrForTest();
    QString SrStatusForTest() const;
    void OpenMenuForTest();   // 截图用：弹开右上角菜单
    int ZoomPercent() const;
    void ClickFitButtonForTest();   // 回归：真正点一次标题栏"1:1"按钮

signals:
    void RequestNextChapter();
    void RequestPrevChapter();
    void RequestFullScreen();
    void RequestBack();
    void ProgressChanged(int page, int total);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void ShowEmptyState();
    void HandleTap(const QString &zone, const QPoint &pos);
    void ScrollHalf(int dir);
    void ToggleChrome(bool forceShow = false);
    // 阅读器工具条是**浮层**（不在布局里）：显隐时上下滑出，内容区不被重排，
    // 因此既没有跳帧也不会让当前页重新布局。
    void ShowChrome(bool show, bool animate);
    void LayoutChrome();
    // 标题栏只留"返回 / 标题 / 加载进度 / 菜单"四项：其余控件（阅读模式、缩放、
    // 自动滚动、全屏、图片信息…）全部收进菜单，图片详情直接作为菜单首块显示。
    void ShowReaderMenu(const QPoint &globalPos);
    QMenu *BuildReaderMenu();
    QWidget *BuildInfoHeader(QWidget *parent);
    void SaveCurrentAs();
    void SyncChromeButtons();
    void UpdatePageWidgets(int cur, int total);

    ReaderView *view_ = nullptr;
    ElideLabel *title_ = nullptr;  // 超长自动省略，窄窗口不挤走菜单按钮
    QLabel *pageInfo_ = nullptr;   // 加载进度 / 页码
    QPushButton *fitBtn_ = nullptr; // 恢复原尺寸（缩放后一键回 100%）
    QWidget *bar_ = nullptr;       // 顶部工具条（点按左右上角显隐）
    QWidget *bottom_ = nullptr;    // 底部翻页条（页码滑块）
    QPushButton *menuBtn_ = nullptr;
    QSlider *pageSlider_ = nullptr;
    QLabel *pageLabel_ = nullptr;
    QLabel *help_ = nullptr;       // 首次进入的分区/手势提示（点一下关闭）
    QWidget *emptyHost_ = nullptr; // 没有章节图片时的空态
    bool pendingRemote_ = false;   // 正在等真实章节，OnEnter 不要覆盖空态
    bool progressive_ = false;     // 正在渐进加载（占位页 + 逐张补图）
    int loadDone_ = 0;
    int loadTotal_ = 0;
    bool fullScreen_ = false;
    bool hasPrevChapter_ = false;
    bool hasNextChapter_ = false;
    bool chromeShown_ = true;      // 工具条是否在屏内（浮层位置上移/下移，不是 setVisible）
};

// ---------------------------------------------------------------- 下载
class DownloadPage : public PageBase
{
    Q_OBJECT
public:
    explicit DownloadPage(QWidget *parent = nullptr);
    void OnEnter() override;

    // MainWindow 在构造期注入下载队列（页面不自己 new，避免两份队列）
    void SetManager(class DownloadManager *m);
    QString Report() const;   // 一行 ASCII 状态（--dl-report 用）

signals:
    void OpenDownloaded(const QString &dir);

private:
    void Rebuild();
    class DownloadManager *mgr_ = nullptr;
    QVBoxLayout *list_ = nullptr;
    QLabel *summary_ = nullptr;
    class QTimer *refresh_ = nullptr;
    QString filter_;
};

// ---------------------------------------------------------------- 本地
class LocalPage : public PageBase
{
    Q_OBJECT
public:
    explicit LocalPage(QWidget *parent = nullptr);

    // 外部（下载页"打开"）指定目录并刷新：不弹目录选择框
    void OpenDir(const QString &path);
    QString CurrentDir() const { return root_; }

signals:
    void OpenLocal(const QString &path);

private:
    void Scan(const QString &root);
    QVBoxLayout *list_ = nullptr;
    QLabel *path_ = nullptr;
    QString root_;
};

// ---------------------------------------------------------------- 收藏
class FavoritePage : public PageBase
{
    Q_OBJECT
public:
    // 账号页的"没内容"有四种原因，必须分开显示：未登录 / 加载中 / 空收藏 /
    // 加载失败。全都画成同一张空白（或同一批演示卡片）就是用户报的
    // "登录了看不到自己的收藏，还显示别人的东西"。
    enum class State { NotLoggedIn, Loading, Empty, Error, Ready };
    explicit FavoritePage(QWidget *parent = nullptr);
    void OnEnter() override;
    // 真实收藏列表（/favorite）。空列表 = Empty，绝不回落到演示卡片。
    void SetItems(const QVector<Comic> &items, int total = -1);
    void SetState(State state, const QString &detail = QString());
    State CurrentState() const { return state_; }
    // 回归/冒烟：ASCII 状态名 + 当前卡片数（不读文案，避免编码干扰断言）
    QString StateName() const;
    int CardCount() const;
    // /album 的真实章节数回填到已显示的收藏卡片
    void SetChapters(const QString &id, int chapters);

signals:
    void OpenComic(const Comic &c);
    // 进入页面时请求刷新（外壳决定要不要带 Cookie 去拉 /favorite）
    void RefreshRequested();
    // 空状态里的"去登录"按钮：外壳负责跳登录页
    void LoginRequested();

private:
    void ApplyState();
    ComicGrid *grid_ = nullptr;
    class LoadingBar *loading_ = nullptr;
    QWidget *content_ = nullptr;   // 卡片墙 / 空态的公共宿主（状态切换时做交叉淡出）
    QLabel *hint_ = nullptr;
    QWidget *emptyHost_ = nullptr;
    EmptyState *empty_ = nullptr;
    State state_ = State::NotLoggedIn;
    QString stateDetail_;
    int total_ = 0;
};

// ---------------------------------------------------------------- 历史
class HistoryPage : public PageBase
{
    Q_OBJECT
public:
    explicit HistoryPage(QWidget *parent = nullptr);

private:
    QVBoxLayout *list_ = nullptr;
};

// ---------------------------------------------------------------- 评论（作品评论）
class CommentPage : public PageBase
{
    Q_OBJECT
public:
    enum class State { NoAlbum, Loading, Empty, Error, Ready };
    explicit CommentPage(QWidget *parent = nullptr);
    // 进入某本书的评论区（详情页"评论"按钮 / 回归 CLI 共用）
    void SetAlbum(const QString &albumId, const QString &title);
    void SetItems(const QVector<JmComment> &items, int total, int page, const QString &albumId);
    void SetState(State state, const QString &detail = QString());
    void SetSendResult(bool ok, const QString &message);
    QString StateName() const;
    int RowCount() const;
    QString AlbumId() const { return albumId_; }

signals:
    void RefreshRequested(const QString &albumId, int page);
    void SendRequested(const QString &albumId, const QString &text);
    void LoginRequested();

private:
    void ApplyState();
    void Submit();

    QVBoxLayout *list_ = nullptr;
    QLineEdit *edit_ = nullptr;
    QPushButton *send_ = nullptr;
    QLabel *hint_ = nullptr;
    QLabel *sendStatus_ = nullptr;
    class LoadingBar *loading_ = nullptr;
    class EmptyState *empty_ = nullptr;
    QString albumId_;
    QString albumTitle_;
    State state_ = State::NoAlbum;
    QString stateDetail_;
    int total_ = 0;
    int page_ = 1;
    int rows_ = 0;
};

// ---------------------------------------------------------------- 超分
class SrModelField;

class SrPage : public PageBase
{
    Q_OBJECT
public:
    explicit SrPage(QWidget *parent = nullptr);
    void OnEnter() override;

    // 重新扫描模型目录（安装/推送模型后不必重启）
    void ReloadModels();
    // 回归/自动化：选中某个模型（等价于点该行"使用"）
    bool SelectModel(const QString &name);
    // 回归/自动化：直接指定待超分的图片
    void SetSourceForTest(const QString &path);
    // 回归/自动化：开始超分（不弹文件框），返回是否真的排了任务
    bool StartUpscaleForTest();
    // 一行 ASCII 状态（--sr-page-report）
    QString Report() const;
    QString CurrentModelName() const;

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void UpdateStatusLine();
    void UpdateModelField();
    void UpdateInfoCard();
    void UpdatePreviewImage();
    void RelayoutStage();
    void StartUpscale(const QString &path);
    void SaveResult();

    QWidget *stage_ = nullptr;
    QLabel *preview_ = nullptr;
    QLabel *badge_ = nullptr;
    QLabel *status_ = nullptr;
    QLabel *scaleLabel_ = nullptr;
    ElideLabel *detail_ = nullptr;    // 图片信息 · 状态
    ElideLabel *infoRes_ = nullptr;   // 图片信息 · 分辨率
    ElideLabel *infoSize_ = nullptr;  // 图片信息 · 大小
    ElideLabel *infoMs_ = nullptr;    // 图片信息 · 耗时
    QProgressBar *progress_ = nullptr;
    QPushButton *runBtn_ = nullptr;
    QPushButton *saveBtn_ = nullptr;
    QPushButton *pickBtn_ = nullptr;
    SrModelField *modelField_ = nullptr;
    QString model_;
    QImage source_;
    QImage result_;
    QString sourcePath_;
    double resultMs_ = 0.0;
    bool busy_ = false;
    QFutureWatcher<SrEngine::Result> *watcher_ = nullptr;
};

// ---------------------------------------------------------------- 设置
class SettingsView : public PageBase
{
    Q_OBJECT
public:
    explicit SettingsView(QWidget *parent = nullptr);
    // 进页时对齐引擎当前模型：在超分页换过模型后回到设置页，
    // 选择栏不能还显示旧值（下拉框时代这里是靠重建页面才对上的）
    void OnEnter() override;

    // 阅读模式：0~8，与 ReaderView/Python ReadMode 同序
    void SetReadingMode(int mode, bool persist = true);
    int ReadingMode() const;

    // 回归：当前显示的缓存目录文案 + 直接改缓存目录（等价于点"选择…"）
    QString CacheDirLabel() const;
    bool ApplyCacheDir(const QString &path);

signals:
    void ReadingModeChanged(int mode);
    void ReadingDirectionChanged(bool rtl);
    void ReadingZoomChanged(int percent);
    void ReadingAutoSrChanged(bool on);
    void CachePathChanged(const QString &path);

private:
    QWidget *BuildGroup(const QString &title);
    void UpdateCacheLabel();
    QVBoxLayout *body_ = nullptr;
    QString filter_;
    class QComboBox *readMode_ = nullptr;
    class QLabel *cacheLabel_ = nullptr;
    SrModelField *modelField_ = nullptr;
};

// ---------------------------------------------------------------- 我的评论
class MyCommentPage : public PageBase
{
    Q_OBJECT
public:
    // 与收藏页同一套四态：未登录 / 加载中 / 空 / 失败 / 就绪
    enum class State { NotLoggedIn, Loading, Empty, Error, Ready };
    explicit MyCommentPage(QWidget *parent = nullptr);
    void OnEnter() override;
    // page == 1 时替换整列，> 1 时追加（翻页）
    void SetItems(const QVector<JmComment> &items, int total, int page);
    void SetState(State state, const QString &detail = QString());
    QString StateName() const;
    int RowCount() const;

signals:
    void RefreshRequested(int page);
    void LoginRequested();
    void OpenAlbum(const QString &albumId);

private:
    void ApplyState();
    QVBoxLayout *list_ = nullptr;
    QScrollArea *area_ = nullptr;
    class LoadingBar *loading_ = nullptr;
    QLabel *hint_ = nullptr;
    EmptyState *empty_ = nullptr;
    QPushButton *refresh_ = nullptr;
    State state_ = State::NotLoggedIn;
    QString stateDetail_;
    int total_ = 0;
    int page_ = 1;
    int rows_ = 0;
};

// ---------------------------------------------------------------- 观看记录（云端）
class RemoteHistoryPage : public PageBase
{
    Q_OBJECT
public:
    enum class State { NotLoggedIn, Loading, Empty, Error, Ready };
    static constexpr int kPageSize = 80;   // 服务端每页条数（实测 80）
    explicit RemoteHistoryPage(QWidget *parent = nullptr);
    void OnEnter() override;
    void SetItems(const QVector<Comic> &items, int total, int page);
    void SetState(State state, const QString &detail = QString());
    QString StateName() const;
    int CardCount() const;

signals:
    void RefreshRequested(int page);
    void OpenComic(const Comic &c);
    void LoginRequested();

private:
    void ApplyState();
    ComicGrid *grid_ = nullptr;
    class LoadingBar *loading_ = nullptr;
    QWidget *content_ = nullptr;
    QWidget *emptyHost_ = nullptr;
    QWidget *moreHost_ = nullptr;
    EmptyState *empty_ = nullptr;
    QLabel *hint_ = nullptr;
    QPushButton *sync_ = nullptr;
    QPushButton *more_ = nullptr;
    State state_ = State::NotLoggedIn;
    QString stateDetail_;
    int total_ = 0;
    int page_ = 1;
};

// ---------------------------------------------------------------- 本地收藏
class LocalFavoritePage : public PageBase
{
    Q_OBJECT
public:
    explicit LocalFavoritePage(QWidget *parent = nullptr);
    void OnEnter() override;
    void Rescan();
    // 回归：ASCII 一行（root=... items=N）
    QString Report() const;

signals:
    // 打开某个本地归档目录（id 存的是绝对路径，走本地页浏览图片）
    void OpenLocal(const QString &path);

private:
    ComicGrid *grid_ = nullptr;
    EmptyState *empty_ = nullptr;
    QLabel *hint_ = nullptr;
    QString root_;
};

// ---------------------------------------------------------------- 每周必看 / 连载更新
class WeekPage : public PageBase
{
    Q_OBJECT
public:
    enum class State { Loading, Empty, Error, Ready };
    explicit WeekPage(QWidget *parent = nullptr);
    void OnEnter() override;
    void SetItems(const QVector<Comic> &items, int total, int page);
    void SetState(State state, const QString &detail = QString());
    QString StateName() const;
    int CardCount() const;
    // 回归：直接切换筛选并请求（等价于点下拉框）
    void SelectFilter(int typeIndex, int dayIndex);

signals:
    // type: all/manga/hanman；day: 0=全部，1~7=周一~周日
    void RefreshRequested(const QString &type, int day, int page);
    void OpenComic(const Comic &c);

private:
    void ApplyState();
    void Request(int page);
    ComicGrid *grid_ = nullptr;
    class LoadingBar *loading_ = nullptr;
    QWidget *content_ = nullptr;
    QWidget *emptyHost_ = nullptr;
    QWidget *pager_ = nullptr;
    EmptyState *empty_ = nullptr;
    QLabel *hint_ = nullptr;
    QPushButton *more_ = nullptr;
    QComboBox *type_ = nullptr;
    QComboBox *day_ = nullptr;
    State state_ = State::Loading;
    QString stateDetail_;
    int total_ = 0;
    int page_ = 1;
};

// ---------------------------------------------------------------- 网络存储
class NasPage : public PageBase
{
    Q_OBJECT
public:
    explicit NasPage(QWidget *parent = nullptr);
    void OnEnter() override;
    // 重新读 ui.ini 并重建行（添加/删除/校验完成后都会走这里）
    void Reload();
    // 回归：直接增删（不经过表单），成功返回节点 id
    QString AddNodeForTest(const QString &name, const QString &type, const QString &target,
                           const QString &user, const QString &pass, QString *error);
    bool RemoveNodeForTest(const QString &id);
    bool ProbeNodeForTest(const QString &id);   // 同步校验 local/smb；webdav 返回 false
    // 回归：ASCII 一行（nodes=N ready=M form=0/1）
    QString Report() const;

signals:
    // 本地/SMB 归档目录交给本地页浏览
    void OpenLocal(const QString &path);

private:
    void BuildForm(QVBoxLayout *lay, QWidget *body);
    void SetFormOpen(bool open);
    void RebuildRows();
    void Persist();
    void ProbeOne(int index);
    void AddFromForm();

    QVBoxLayout *list_ = nullptr;
    QWidget *form_ = nullptr;
    QLineEdit *nameEdit_ = nullptr;
    QLineEdit *targetEdit_ = nullptr;
    QLineEdit *userEdit_ = nullptr;
    QLineEdit *passEdit_ = nullptr;
    QComboBox *typeCombo_ = nullptr;
    QPushButton *addBtn_ = nullptr;
    QLabel *hint_ = nullptr;
    QVector<NasNode> nodes_;
    bool formOpen_ = false;
    bool busy_ = false;
};

// ---------------------------------------------------------------- 批量超分
class BatchSrPage : public PageBase
{
    Q_OBJECT
public:
    explicit BatchSrPage(QWidget *parent = nullptr);
    void OnEnter() override;

private:
    void StartBatch();
    void UpdateOutDir();

    ElideLabel *dir_ = nullptr;
    QLabel *hint_ = nullptr;
    ElideLabel *scaleVal_ = nullptr;
    ElideLabel *outVal_ = nullptr;
    QProgressBar *progress_ = nullptr;
    SrModelField *modelField_ = nullptr;
    QComboBox *fmt_ = nullptr;
    QPushButton *start_ = nullptr;
    QString dirPath_;
    QString modelName_;
    bool busy_ = false;
    QFutureWatcher<QString> *watcher_ = nullptr;
};

// ---------------------------------------------------------------- 帮助
class HelpPage : public PageBase
{
    Q_OBJECT
public:
    explicit HelpPage(QWidget *parent = nullptr);
};

// ---------------------------------------------------------------- 关于
class AboutPage : public PageBase
{
    Q_OBJECT
public:
    explicit AboutPage(QWidget *parent = nullptr);
    // 版本行：构造号 + 参与更新比较的发行线（两条线不同，必须都写出来）
    void SetVersionInfo(const QString &appVersion, const QString &releaseLine);
    // 更新检查的状态文案（"正在检查…" / "已是最新 v1.3.7" / "有更新：v1.3.8"）
    void SetUpdateStatus(const QString &text, bool ok);
    // 有新版本时的更新日志预览 + 下载入口
    void SetUpdateResult(const QString &newVersion, const QString &changelog);
    void SetConfigLine(const QString &text);
    // 日志导出结果（成功给路径，失败给原因）
    void SetExportResult(bool ok, const QString &detail);
    QString UpdateStatusText() const;
    QString ConfigLineText() const;

signals:
    void ExportLogsRequested();
    void CheckUpdateRequested();
    void OpenLogDirRequested();
    void OpenUrlRequested(const QString &url);

private:
    QLabel *version_ = nullptr;
    QLabel *updateStatus_ = nullptr;
    QLabel *configLine_ = nullptr;
    QLabel *changelog_ = nullptr;
    QPushButton *downloadBtn_ = nullptr;
    QPushButton *logBtn_ = nullptr;
    QPushButton *checkBtn_ = nullptr;
    QString downloadUrl_ = QStringLiteral("https://github.com/tonquer/JMComic-qt/");
};

// ---------------------------------------------------------------- 登录 / 注册 / 找回密码
class LoginPage : public PageBase
{
    Q_OBJECT
public:
    enum class Panel { None, Register, Forgot };
    explicit LoginPage(QWidget *parent = nullptr);
    void SetStatus(const QString &text, bool ok);
    void SetBusy(bool busy);
    // 登录成功后状态行 + "退出登录"按钮要立刻跟着变（原来只在构造时读一次
    // 会话，登录完还得切页/重启才看到退出入口）。
    void SetLoggedIn(bool on, const QString &name = QString());

    // ---- 注册 / 找回密码面板 ----
    void ShowPanel(Panel panel);
    Panel CurrentPanel() const { return panelMode_; }
    void SetPanelStatus(const QString &text, bool ok);
    void SetPanelBusy(bool busy);
    void SetCaptcha(const QImage &image);
    void SetCaptchaError(const QString &why);
    QString PanelStatusText() const;

signals:
    // 真实登录：外壳调 /login，成功后把 avs 落库并刷新侧栏
    void LoginRequested(const QString &user, const QString &pass);
    // 退出登录：页面已清 UI，外壳还要把 JmApi 里的 avs 一起清掉
    void LogoutRequested();
    // 注册（字段与 Python RegisterReq 一致）
    void RegisterRequested(const QString &user, const QString &email, const QString &pass,
                           const QString &pass2, const QString &gender, const QString &captcha);
    void ResetPasswordRequested(const QString &email);
    // 重发注册验证邮件（Python RegisterVerifyMailReq）
    void ResendVerifyRequested(const QString &user, const QString &pass);
    void CaptchaRefreshRequested();

private:
    void BuildAuthPanel(QVBoxLayout *colLay, QWidget *column);
    void ApplyPanelMode();
    void SubmitPanel();

    QLabel *status_ = nullptr;
    QPushButton *loginBtn_ = nullptr;
    QPushButton *logoutBtn_ = nullptr;
    QPushButton *regLink_ = nullptr;
    QPushButton *forgotLink_ = nullptr;

    QWidget *panel_ = nullptr;
    QLabel *panelTitle_ = nullptr;
    QLabel *panelStatus_ = nullptr;
    QLineEdit *regUser_ = nullptr;
    QLineEdit *regEmail_ = nullptr;
    QLineEdit *regPass_ = nullptr;
    QLineEdit *regPass2_ = nullptr;
    QLineEdit *regCaptcha_ = nullptr;
    QComboBox *regGender_ = nullptr;
    QLabel *captchaView_ = nullptr;
    QPushButton *captchaBtn_ = nullptr;
    QPushButton *panelSubmit_ = nullptr;
    QPushButton *panelCancel_ = nullptr;
    QPushButton *resendBtn_ = nullptr;
    Panel panelMode_ = Panel::None;
    bool panelBusy_ = false;
};
