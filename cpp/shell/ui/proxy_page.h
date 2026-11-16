// 分流与节点设置页：版式对齐 docs/proxy_advanced_dropdown_demo.html（移动端稿）。
//
// 页面结构（主体在一个滚动区里，底栏固定）：
//   页头「代理通道 + 说明」 → 代理模式单选（4 档）→ 模式说明一行
//   → 「线路测速」表（表头 + 6 行，行间 1px 分割线，右上是 测速/自动选择）
//   → 「节点列表」工具条（标题 + 计数 + 全部/ProxyIP/优选 + 排序）
//   → 列头（节点/地区 · 延迟/评分）→ 节点行（单选 + 两行：IP/延迟评分、标签/地区）
//   → 固定底栏（节点状态 + 高级/收起 + 节点测速）
// 高级抽屉（DOH / ECH / HTTP3 / 地区 / 自定义 IP）是**悬浮层**：挂在页面自身、
// 贴在底栏上沿，高度动画从 0 长上来；不占列表版面，也不触发页面滚动。
//
// 与 Python 版 ui_login_new.ui tab_4 的差异只在"排布"，语义一一对应：
//   分区顺序、文案、单选语义（接口/图床各自一组）、选中即写 Setting.ProxyIpValue
//   均保持不变；"自动选择"是 C++ 侧新增能力（按测速结果 + IP 配置推荐线路）。
// 控件度量走 demo：14px 圆形单选、15px 方形复选、26px 下划线输入框、
// 22/24/30px 三档紧凑按钮；圆角沿用全站 UiStyle::Radius（0px 直角）。
#pragma once

#include <QHash>
#include <QVector>

#include "proxy_config.h"
#include "ui_pages.h"

class QButtonGroup;
class QCheckBox;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QVBoxLayout;
class QWidget;
class QResizeEvent;
class QScrollArea;
class ElideLabel;
class NodeTester;

class ProxyPage : public PageBase
{
    Q_OBJECT
public:
    explicit ProxyPage(QWidget *parent = nullptr);

    void OnEnter() override;
    // 离开页面时把悬浮抽屉收起：否则下次进来它还挂着，
    // 常驻的"点面板外收起"过滤器也会跟着去别的页面判事件
    void OnLeave() override;

    // 测速入口（按钮与自动化共用，外部接口不变）
    void StartLineTest();
    void StartNodeTest();
    // 按测速结果与 IP 配置自动选线路
    void AutoSelectLines();
    // 截图/回归用：直接展开高级抽屉（等价于点一次"高级"，不做动画）
    void ExpandAdvanced();
    // 截图/回归用：展开抽屉并弹出「自定义ProxyIP地区」下拉（返回菜单项文案，
    // 供 --proxy-test advmenu 打印断言：展示全部已获取地区）
    QStringList PopupRegionMenuForTest();
    // 截图/回归用：走菜单的真实 triggered 链路，返回 "写入的代码|文本框文案"
    QString TriggerRegionMenuForTest(int index);
    // 截图/回归用：点按判定（与 eventFilter 同一条规则），验证"滑动不误触"
    bool IsRegionTap(const QPoint &pressGlobal, const QPoint &releaseGlobal) const;
    // 截图/回归用：造几个带地区的节点（只改内存、不落盘），
    // 让地区下拉能演示"列出全部已获取地区"，不影响真实配置
    void SeedRegionCandidatesForTest(const QStringList &countries);
    // 截图/回归用：打印"面板内 / 底栏按钮 / 面板外"三点的命中判定
    // （期望 in=0 btn=0 out=1），验证"点面板外收起"的几何口径
    QString AdvancedOutsideProbeForTest();
    // 截图/回归用：抽屉当前是否展开（验证"离开页面自动收起"）
    bool IsAdvancedOpenForTest() const { return advOpen_; }

signals:
    void Applied();

protected:
    void resizeEvent(QResizeEvent *e) override;
    // 只读的地区框：按下/抬起位移在阈值内才算"点击"，滑动经过时不弹菜单
    bool eventFilter(QObject *obj, QEvent *ev) override;

private:
    void BuildUi();

    void RefreshLineRows();
    void SetLineBusy(bool busy);
    void OnLinePing(int index, bool ok, int ms, const QString &error);
    void OnLineSpeed(int index, bool ok, const QString &text);
    void OnLineResult(int index, const LineTestResult &result);

    void RenderNodes();
    void RefreshNodeRow(int row);
    void UpdateCount();
    void UpdateStatus();
    void UpdateEffectiveHint();
    // 测速状态行（页头右侧）与地区候选计数（高级抽屉内）都只有一行，
    // 必须走 ElideLabel 才不会把同一行的按钮顶出可视区
    void SetLineStatus(const QString &text);
    void RefreshRegionStat();
    void SetFilter(NodeFilter filter);
    void CycleSort();
    // 高级抽屉：与 Python proxyAdvPanel 同一套语义（按钮文案 高级/收起、
    // 高度动画展开、展开时 1px 主题色描边），但挂在页面自身做悬浮层
    void ToggleAdvanced();
    void SetAdvancedOpen(bool open, bool animate);
    void ApplyAdvancedHeight(qreal value);
    void LayoutAdvancedOverlay();
    int AdvancedContentHeight();
    int AdvancedMaxHeight() const;
    void ApplyCustomIps();
    void SetProxyMode(int mode);
    void SaveAdvanced();
    void CheckDoh();
    void RefreshAdvancedStatus();
    // 自定义 ProxyIP 地区：候选来自"已获取到的节点"（Python RegionNames）
    QMenu *ShowRegionMenu(const QPoint &globalPos);   // 返回菜单便于自检读取菜单项
    void FillRegionMenu(QMenu *menu);                 // 菜单项（真实弹出与离屏截图共用）
    void ApplyRegionChoice(const QString &name);
    void RefreshRegionField();
    // 地区下拉的锚点：弹层要贴住"带下划线的整框"，而不是里面那截只读输入框
    QPoint RegionAnchor() const;
    // 悬浮抽屉：这个全局坐标是否落在"面板与底栏按钮之外"（是 → 收起）
    bool IsAdvancedOutside(const QPoint &globalPos) const;

    NodeTester *tester_ = nullptr;
    QScrollArea *scroll_ = nullptr;   // 整页滚动容器（展开高级抽屉后要把它滚进视口）

    // 线路网格
    QGridLayout *lineGrid_ = nullptr;
    // Python 的 radioApiGroup / radioImgGroup：两组各自单选，不能因同一父控件
    // 的 autoExclusive 变成"12 个里只能选一个"
    QButtonGroup *apiGroup_ = nullptr;
    QButtonGroup *imgGroup_ = nullptr;
    // Python 分流设置：无代理 / HTTP / Socks5 / 系统（IsHttpProxy 0..3）
    QButtonGroup *modeGroup_ = nullptr;
    QLabel *modeStatus_ = nullptr;
    ElideLabel *lineStatus_ = nullptr;
    QPushButton *lineBtn_ = nullptr;
    QPushButton *autoBtn_ = nullptr;
    QHash<int, QLabel *> lineDelay_;
    QHash<int, QLabel *> lineSpeed_;
    QVector<LineTestResult> lineResults_;

    // 节点区
    QVBoxLayout *nodeHost_ = nullptr;
    ElideLabel *countLabel_ = nullptr;
    ElideLabel *nodeStatus_ = nullptr;   // 底栏左侧状态
    QPushButton *nodeBtn_ = nullptr;
    // 筛选条：Python proxyFilterBar 的单行 3 chip（不换行、无计数后缀，
    // 计数只在工具条 countLabel_ 里出现一次）
    QPushButton *filterAll_ = nullptr;
    QPushButton *filterProxy_ = nullptr;
    QPushButton *filterPreferred_ = nullptr;
    QPushButton *sortBtn_ = nullptr;   // 工具条右侧的排序按钮
    QLineEdit *customEdit_ = nullptr;
    QLineEdit *regionEdit_ = nullptr;
    QWidget *regionBox_ = nullptr;     // 地区"带下划线的框"（含右端 ▾）
    QLabel *regionStat_ = nullptr;     // 已获取 N 个节点
    QLabel *effective_ = nullptr;      // 页脚诊断：接口/图片实际走的线路与原因

    // 高级抽屉（Python proxyAdvPanel + proxyAdvFrame）：悬浮层，不入滚动内容
    QWidget *advOverlay_ = nullptr;
    QScrollArea *advScroll_ = nullptr;
    QWidget *advFrame_ = nullptr;
    QPushButton *advBtn_ = nullptr;
    QLabel *advNote_ = nullptr;
    QWidget *foot_ = nullptr;      // 悬浮层要贴住它的上沿
    int advHeight_ = 0;            // 当前动画高度（0 = 收起）
    bool advOpen_ = false;
    // 「点面板外收起」延后到抬起判定：按下即收起会把滑过面板外的手势
    // 当成点击（悬浮窗误触），而且全局防误触层只吞抬起、拦不住 press。
    bool advOutsideArmed_ = false;
    QPoint advOutsidePress_;

    // 高级（Python tab_4 高级面板：DOH 地址 / 启用 ECH / 启用 HTTP3）
    QLineEdit *dohEdit_ = nullptr;
    QCheckBox *dohEnable_ = nullptr;
    QCheckBox *echBox_ = nullptr;
    QCheckBox *http3Box_ = nullptr;
    // 节点测速并发线程数（1..16；1 = 兼容单线程事件循环）
    QSpinBox *threadSpin_ = nullptr;
    QLabel *dohStatus_ = nullptr;
    QLabel *advancedStatus_ = nullptr;
    QPushButton *dohCheckBtn_ = nullptr;
    QHash<int, QLabel *> nodeSpd_;   // 行 -> "延迟 / 评分"

    QVector<NodeResult> nodes_;
    QVector<int> shownRows_;      // row -> nodes_ 下标
    NodeFilter filter_ = NodeFilter::All;
    NodeSortMode sortMode_ = NodeSortMode::Smart;
    int apiLine_ = 1;
    int imgLine_ = 1;
    QString selectedIp_;
    bool applyingRadio_ = false;
    bool narrow_ = false;
    // 地区框的点击判定（滑动经过时不算点击，与 Touch::TapGuard 同一阈值语义）
    QPoint regionPressPos_;
    bool regionPressed_ = false;
};
