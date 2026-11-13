// 虚拟化网格：QListView + 自定义 delegate，替代原来的 widget-per-item。
//
// 为什么换：
//   原来的 ComicGrid 给每部漫画建一个 QWidget（含封面 pixmap），1000 条就是 1000 个
//   控件树节点；QListView 只绘制可见区域的 item，滚动时复用 delegate，
//   数据量从"几十条"放宽到"几万条"。
// 与卡片外观保持一致：
//   绘制逻辑统一走 CardPaint::Paint()，独立控件 ComicCard 与 delegate 共用一份代码。
#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QListView>
#include <QStyledItemDelegate>
#include <QWidget>

#include <functional>

#include "models.h"
#include "touch.h"

// 轻量压测数据：--shot 时打印，用来证明"只画可见区域"
struct ComicBench {
    int rows = 0;
    qint64 setMs = 0;
    int painted = 0;   // delegate paint 调用次数（含重绘）
};
extern ComicBench g_comicBench;

class ComicListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    // QML delegate 用的角色（QQuick 列表逐字段取，见 PageCards.qml）。
    // ComicRole 保留：widgets delegate（ComicDelegate）仍按 row 取整条数据。
    enum Role {
        ComicRole = Qt::UserRole + 1,
        IdRole,
        TitleRole,
        AuthorRole,
        StatusRole,
        ChaptersRole,
        // 封面下方第二行的成稿文案（作者 · N 话 / 仅作者）。
        // 由 C++ 的 ComicMetaText() 统一拼装：QML 不再自己做 " + 0 + 话"，
        // 否则"章节数未知"会被渲染成"0 话"。
        MetaRole,
        ReadChapterRole,
        UpdatedRole,
        CoverUrlRole,
        // "b64(id)|b64(url)|b64(title)|w|h"——封面 image provider 的请求串，
        // 宽高用当前卡宽，避免 QML 里做 base64（中文 title 会炸 Qt.btoa）。
        CoverReqRole,
        // 封面就绪代次：provider 预取到图 / 网络封面到货时 ++，拼进
        // image source 让 QML 重取。0 表示还没有任何刷新。
        CoverRevRole,
    };

    explicit ComicListModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void SetComics(const QVector<Comic> &comics);
    // 追加（搜索结果翻页用）：不重置模型，保留当前滚动位置
    void AddComics(const QVector<Comic> &more);
    // /album 带回真实章节数：按 id 定位到行做精确 dataChanged。
    // 返回是否命中（分页源里尚未加载的行返回 false，等下次取数时由
    // RealComic() 从 ComicFacts 补齐）。
    bool SetChapters(const QString &id, int chapters);
    // 分页数据源：滚动到底部时按需拉取，适合接真实接口（避免一次性拿几千条）
    void SetPagedSource(std::function<QVector<Comic>(int offset, int limit)> loader, int total);

    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

    const Comic &At(int row) const;
    int Count() const { return items_.size(); }
    bool HasMore() const { return hasMore_; }

    // 卡片封面宽（QML 端列数变化时由 ComicGrid 同步），用于组装 CoverReqRole
    void SetCoverWidth(int w);
    int CoverWidth() const { return coverW_; }

public slots:
    // 封面预取/到货：定位到行做精确 dataChanged，避免全列刷新
    //（ComicGrid 把 CoverProvider::CoverReady 接到这里）
    void OnCoverReady(const QString &key);

private:
    int RowOfKey(const QString &key) const;
    void BumpRow(int row);

    QVector<Comic> items_;
    std::function<QVector<Comic>(int, int)> loader_;
    int total_ = 0;
    bool hasMore_ = false;
    int coverW_ = 168;
    // 封面就绪代次按 comic id 记：只让到货的那一行 URL 变化重取，
    // 全局代次会让整屏 Image 一起重新加载（异步间隙会闪占位色）
    QHash<QString, int> revById_;
};

class ComicDelegate : public QStyledItemDelegate
{
    Q_OBJECT
public:
    explicit ComicDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    void SetCardWidth(int w);
    int CardWidth() const { return cardWidth_; }

private:
    int cardWidth_ = 168;
};

class ComicListView : public QListView
{
    Q_OBJECT
public:
    explicit ComicListView(QWidget *parent = nullptr);

signals:
    // 长按/右键：交给页面决定弹什么菜单（与 Python 版的长按行为对齐）
    void ContextRequested(const QModelIndex &index, const QPoint &globalPos);

protected:
    void mousePressEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    // 滚动 = 内容纯平移：用 backing-store blit 代替全视口重绘（见实现注释）
    void scrollContentsBy(int dx, int dy) override;

private:
    void ArmLongPress(const QPoint &pos, const QModelIndex &index);
    void CancelLongPress();

    class QTimer *pressTimer_ = nullptr;
    QPoint pressPos_;
    QPersistentModelIndex pressIndex_;
};

class ComicGrid : public QWidget
{
    Q_OBJECT
    // QML 端决定是否启用 hover（触摸拖动时 hover 无意义）
    Q_PROPERTY(bool touchMode READ TouchMode CONSTANT)
public:
    explicit ComicGrid(QWidget *parent = nullptr);
    ~ComicGrid() override;

    void SetComics(const QVector<Comic> &comics);
    // 追加（搜索结果翻页用）：不重置模型，保留当前滚动位置
    void AddComics(const QVector<Comic> &more);
    // /album 的真实章节数回填到已显示的同 id 卡片（见 ComicListModel::SetChapters）
    void SetChapters(const QString &id, int chapters);
    void SetPagedSource(std::function<QVector<Comic>(int, int)> loader, int total);
    void SetCardWidth(int w);
    int Count() const;
    void ScrollToBottom();

    bool TouchMode() const;
    // 首页分区横滑期间锁住卡片点击：拖动也会让 QML 的 MouseArea 收到 release，
    // 不锁的话"滑一下"会顺手点开一张卡片。
    void SetSwipeLock(bool on) { swipeLock_ = on; }
    bool SwipeLocked() const { return swipeLock_; }
    // 回归/自检：QML 根对象（读推下去的字体/几何属性）
    QObject *QmlRootForTest() const;

protected:
    void resizeEvent(QResizeEvent *ev) override;

signals:
    void Activated(const Comic &c);
    void ContextAction(const Comic &c, const QString &action);

public slots:
    // QML PageCards.qml 的 MouseArea 经 cardBridge 调用
    void cardClicked(int row);
    void cardLongPress(int row);

private:
    // C++ 端算好几何（复刻 Relayout 的列数规则），QML 只负责渲染
    void ApplyGeometry(int viewWidth);
    // 把应用字体（家族 / 字重 / 字号缩放）推给 QML：卡片文字必须跟着设置页变
    void SyncFont();
    void SyncTheme();
    // 长按菜单（与 Python 版一致：详情 / 收藏 / 下载 三项）
    void ShowCardMenu(int row, const QPoint &globalPos);

    ComicListModel *model_ = nullptr;
    class QQuickWidget *quick_ = nullptr;
    int cardWidth_ = 168;
    int coverW_ = 168;
    int lastViewportW_ = 0;
    bool swipeLock_ = false;
};
