#include "proxy_page.h"

#include "../doh_client.h"
#include "../net_backend.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QRadioButton>
#include <QResizeEvent>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QScrollBar>
#include <QScroller>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "motion.h"
#include "node_tester.h"
#include "region_code.h"
#include "theme.h"
#include "touch.h"
#include "ui_kit.h"

namespace {

// Python 卡片配色：成功 #7fb80e，失败用主题 danger
const QColor kOk(0x7f, 0xb8, 0x0e);

QLabel *Cell(const QString &text, int width, QWidget *parent, const QColor &color)
{
    auto *l = new QLabel(text, parent);
    if (width > 0)
        l->setFixedWidth(width);
    l->setAlignment(Qt::AlignCenter);
    l->setStyleSheet(QStringLiteral("color:%1;").arg(color.name()));
    return l;
}

// 1px 分割线：颜色交给 qss（#ProxyRule 用主题派生的 @divider），
// 这里只负责"高度固定 1px + 不参与拉伸"。
QFrame *Rule(QWidget *parent)
{
    auto *f = new QFrame(parent);
    f->setObjectName(QStringLiteral("ProxyRule"));
    f->setFixedHeight(1);
    f->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    return f;
}

void ClearLayout(QLayout *lay)
{
    if (!lay)
        return;
    while (QLayoutItem *it = lay->takeAt(0)) {
        if (QWidget *w = it->widget())
            w->deleteLater();
        delete it;
    }
}

void MarkButton(QPushButton *b, bool on)
{
    if (!b)
        return;
    b->setObjectName(on ? QStringLiteral("Primary") : QStringLiteral("Ghost"));
    b->style()->unpolish(b);
    b->style()->polish(b);
}

// Python SyncFilterText：排序按钮文案恒定带 ▾，三种模式只在文字上区分。
// 文案保持与 Python 侧一致（智能排序/按延迟/按下载），宽度靠 chip 的 10px 字号
// 与 7px 内边距压下来 —— 360dp 宽仍能和计数、3 个筛选排在同一行。
QString SortButtonText(NodeSortMode mode)
{
    return mode == NodeSortMode::Smart       ? QStringLiteral("智能排序 ▾")
           : mode == NodeSortMode::Latency   ? QStringLiteral("按延迟 ▾")
                                             : QStringLiteral("按下载 ▾");
}

QString ModeName(int mode)
{
    return mode == 0   ? QStringLiteral("无代理")
           : mode == 1 ? QStringLiteral("HTTP")
           : mode == 2 ? QStringLiteral("Socks5")
                       : QStringLiteral("系统");
}

// 节点行右侧的"延迟 / 评分"：与改版前的两列语义完全一致（延迟 + 0.8/0.2 综合分），
// 只是合成一格，好让列头按 demo 写成"延迟 · 评分"。
QString NodeSpeedText(const NodeResult &r)
{
    if (r.failed)
        return QStringLiteral("失败 / —");
    const bool untested = r.delayMs <= 0 && r.downloadMs <= 0;
    if (untested)
        return QStringLiteral("— / —");
    const QString delay = r.delayMs > 0 ? QStringLiteral("%1ms").arg(r.delayMs) : QStringLiteral("—");
    const QString score = r.IsFail() ? QStringLiteral("—") : QString::number(r.Score());
    return delay + QStringLiteral(" / ") + score;
}

QColor NodeSpeedColor(const NodeResult &r)
{
    const UiTokens &tk = UiTheme::I().Tokens();
    if (r.IsFail())
        return tk.danger;
    if (r.delayMs > 0)
        return kOk;
    return tk.textFaint;
}

}  // namespace

ProxyPage::ProxyPage(QWidget *parent) : PageBase(parent)
{
    // qss 里整页的控件度量都写在 #ProxyPage 子树下，必须点名前设好 id
    setObjectName(QStringLiteral("ProxyPage"));
    ProxyConfig::Load();
    apiLine_ = ProxyConfig::ApiLine();
    imgLine_ = ProxyConfig::ImgLine();
    selectedIp_ = ProxyConfig::SelectedIp();
    narrow_ = Touch::IsTouchMode();   // 桌面 --touch 回归也走手机窄屏布局
    lineResults_ = ProxyConfig::LineResults();

    tester_ = new NodeTester(this);
    BuildUi();
    RefreshLineRows();
    nodes_ = ProxyConfig::AllNodes();
    RenderNodes();

    connect(tester_, &NodeTester::LinePing, this, &ProxyPage::OnLinePing);
    connect(tester_, &NodeTester::LineSpeed, this, &ProxyPage::OnLineSpeed);
    connect(tester_, &NodeTester::LineResult, this, &ProxyPage::OnLineResult);
    connect(tester_, &NodeTester::LinesFinished, this, [this] {
        SetLineBusy(false);
        SetLineStatus(QStringLiteral("测速完成 · 每线 2 次采样取最优"));
        RefreshLineRows();
    });
    connect(tester_, &NodeTester::LineResultsReady, this,
            [this](const QVector<LineTestResult> &results) { lineResults_ = results; });
    connect(tester_, &NodeTester::NodeListReady, this,
            [this](const QVector<NodeResult> &nodes) {
                nodes_ = nodes;
                RenderNodes();
            });
    connect(tester_, &NodeTester::NodeUpdated, this, [this](const NodeResult &r) {
        for (int i = 0; i < nodes_.size(); ++i) {
            if (nodes_.at(i).ip == r.ip) {
                nodes_[i] = r;
                break;
            }
        }
        for (int row = 0; row < shownRows_.size(); ++row)
            RefreshNodeRow(row);
        UpdateCount();
    });
    connect(tester_, &NodeTester::NodesFinished, this,
            [this](const QVector<NodeResult> &sorted, const QString &selected) {
                nodes_ = sorted;
                selectedIp_ = selected;
                if (!selected.isEmpty())
                    ProxyConfig::SetSelectedIp(selected);
                nodeBtn_->setEnabled(true);
                nodeBtn_->setText(QStringLiteral("节点测速"));
                nodeStatus_->SetFullText(selected.isEmpty()
                                             ? QStringLiteral("节点测速完成 · 没有可达节点")
                                             : QStringLiteral("节点测速完成 · 已选 %1").arg(selected));
                RenderNodes();
                UpdateEffectiveHint();
                emit Applied();
            });
    connect(tester_, &NodeTester::Log, this,
            [](const QString &line) { qWarning("proxy-test: %s", qPrintable(line)); });

    connect(&DohClient::I(), &DohClient::Checked, this,
            [this](const QString &, bool ok, const QString &detail) {
                if (!dohStatus_)
                    return;
                dohStatus_->setText(ok ? QStringLiteral("检查通过：%1").arg(detail)
                                       : QStringLiteral("检查失败：%1").arg(detail));
                // 校验结果只有一行小字，靠颜色区分成败（demo 的 .stats-txt 同款语义）
                dohStatus_->setStyleSheet(QStringLiteral("color:%1;").arg(
                    (ok ? kOk : UiTheme::I().Tokens().danger).name()));
            });
    connect(&DohClient::I(), &DohClient::EchReady, this,
            [this](const QString &, bool, const QString &) { RefreshAdvancedStatus(); });
    connect(&DohClient::I(), &DohClient::Log, this,
            [](const QString &line) { qWarning("doh: %s", qPrintable(line)); });
}

void ProxyPage::BuildUi()
{
    apiGroup_ = new QButtonGroup(this);
    imgGroup_ = new QButtonGroup(this);
    modeGroup_ = new QButtonGroup(this);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *scroll = new QScrollArea(this);
    scroll_ = scroll;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget(scroll);
    auto *lay = new QVBoxLayout(body);
    // demo 的每一段各自带 12px 左右内缩，而不是整页一个大边距：
    // 只有这样列头底色、行分割线才能整条通到屏幕边缘。
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    scroll->setWidget(body);
    // 分流页是自建 QScrollArea（没走 WrapScroll），必须显式上锁：
    // 否则触摸/触控板的横向拖动会让整页左右平移（hmax 实测 53px @360 宽）。
    Touch::LockHorizontalPan(scroll);
    root->addWidget(scroll, 1);
    // 展开抽屉后的 2.5s 内，内容高度一变（节点行重排/字体落定）就重新贴底，
    // 否则会停在"展开那一刻的滚动范围"上，看起来像点了没反应。
    connect(scroll->verticalScrollBar(), &QScrollBar::rangeChanged, this,
            [this](int, int max) {
                if (advOpen_ && advScrollClock_.isValid() && advScrollClock_.elapsed() < 2500)
                    scroll_->verticalScrollBar()->setValue(max);
            });

    // ---------------- 页头：代理通道 + 说明（demo .phead） ----------------
    auto *head = new QWidget(body);
    auto *headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(12, 10, 12, 2);
    headLay->setSpacing(8);
    auto *headTitle = new QLabel(QStringLiteral("代理通道"), head);
    headTitle->setObjectName(QStringLiteral("ProxyHeadTitle"));
    headLay->addWidget(headTitle);
    headLay->addStretch(1);
    // 手机上没有 hover，所以"说明"必须做成可点文字链（demo 里同样是虚线文字）
    auto *docBtn = new QPushButton(QStringLiteral("说明"), head);
    docBtn->setObjectName(QStringLiteral("ProxyDocLink"));
    docBtn->setProperty("hitGrow", false);
    docBtn->setCursor(Qt::PointingHandCursor);
    connect(docBtn, &QPushButton::clicked, this, [this] {
        ModalDialog::Message(
            this, QStringLiteral("分流说明"),
            QStringLiteral("· 接口与图片可分别选线路：分流 1~4 / IP 分流 / US 反代分流。\n"
                           "· IP 分流只在「无代理」时生效；US 反代分流会把请求改写到反代域名，"
                           "同时自动关闭节点 IP 覆盖。\n"
                           "· 节点列表里选中的 IP 会立即应用到接口、封面与页图请求。"));
    });
    headLay->addWidget(docBtn);
    lay->addWidget(head);

    // ---------------- 代理模式（demo .modes：4 个紧凑单选一行） ----------------
    auto *modeRow = new QHBoxLayout();
    modeRow->setContentsMargins(12, 6, 12, 0);
    modeRow->setSpacing(12);
    const QStringList modeNames = {QStringLiteral("无代理"), QStringLiteral("HTTP"),
                                   QStringLiteral("Socks5"), QStringLiteral("系统")};
    const int currentMode = int(ProxyConfig::Mode());
    for (int i = 0; i < modeNames.size(); ++i) {
        auto *r = new QRadioButton(modeNames.at(i), body);
        r->setAutoExclusive(false);
        // 19px 行高有版式意义：触摸模式下不能被 PolishHitTargets 拉成 44dp 方块
        r->setProperty("hitGrow", false);
        modeGroup_->addButton(r, i);
        r->setChecked(i == currentMode);
        connect(r, &QRadioButton::clicked, this, [this, i] { SetProxyMode(i); });
        modeRow->addWidget(r);
    }
    modeRow->addStretch(1);
    lay->addLayout(modeRow);

    modeStatus_ = new QLabel(QStringLiteral("未启用代理，直连访问"), body);
    modeStatus_->setObjectName(QStringLiteral("ProxyNote"));
    modeStatus_->setContentsMargins(12, 5, 12, 0);
    lay->addWidget(modeStatus_);

    // ---------------- 线路测速（demo .speed：标题 + 右上按钮 + 4 列表格） ----------------
    auto *speedHead = new QHBoxLayout();
    speedHead->setContentsMargins(12, 8, 12, 4);
    speedHead->setSpacing(6);
    auto *speedTitle = new QLabel(QStringLiteral("线路测速"), body);
    speedTitle->setObjectName(QStringLiteral("ProxySection"));
    speedHead->addWidget(speedTitle);
    lineStatus_ = new ElideLabel(QStringLiteral("尚未测速"), body);
    lineStatus_->setObjectName(QStringLiteral("ProxyNote"));
    speedHead->addWidget(lineStatus_, 1);
    lineBtn_ = new QPushButton(QStringLiteral("测速"), body);
    lineBtn_->setObjectName(QStringLiteral("Primary"));
    lineBtn_->setProperty("chip", QStringLiteral("tool"));
    lineBtn_->setProperty("hitGrow", false);
    lineBtn_->setCursor(Qt::PointingHandCursor);
    connect(lineBtn_, &QPushButton::clicked, this, &ProxyPage::StartLineTest);
    speedHead->addWidget(lineBtn_);
    autoBtn_ = new QPushButton(QStringLiteral("自动选择"), body);
    autoBtn_->setObjectName(QStringLiteral("Ghost"));
    autoBtn_->setProperty("chip", QStringLiteral("tool"));
    autoBtn_->setProperty("hitGrow", false);
    autoBtn_->setCursor(Qt::PointingHandCursor);
    connect(autoBtn_, &QPushButton::clicked, this, &ProxyPage::AutoSelectLines);
    speedHead->addWidget(autoBtn_);
    lay->addLayout(speedHead);

    // 表格不再套卡片：行间只有 1px 分割线（demo .sgrid 的 .srow + .srow）
    auto *lineCard = new QWidget(body);
    lineGrid_ = new QGridLayout(lineCard);
    lineGrid_->setContentsMargins(12, 0, 12, 6);
    lineGrid_->setHorizontalSpacing(6);
    lineGrid_->setVerticalSpacing(0);
    lineGrid_->setColumnStretch(0, 1);
    lineGrid_->setColumnStretch(2, 1);
    lay->addWidget(lineCard);

    // ---------------- 节点工具条（demo .tools：标题 + 计数 + 3 筛选 + 排序） ----------------
    // demo 在这里有一条通栏细线（.tools{border-top}）把测速表和节点区分开
    lay->addWidget(Rule(body));
    auto *tools = new QHBoxLayout();
    tools->setContentsMargins(12, 8, 12, 6);
    tools->setSpacing(6);
    auto *listTitle = new QLabel(QStringLiteral("节点列表"), body);
    listTitle->setObjectName(QStringLiteral("ProxySection"));
    tools->addWidget(listTitle);
    countLabel_ = new ElideLabel(QString(), body);
    countLabel_->setObjectName(QStringLiteral("ProxyNote"));
    tools->addWidget(countLabel_);
    tools->addStretch(1);
    filterAll_ = new QPushButton(QStringLiteral("全部"), body);
    filterProxy_ = new QPushButton(QStringLiteral("ProxyIP"), body);
    filterPreferred_ = new QPushButton(QStringLiteral("优选"), body);
    for (QPushButton *b : {filterAll_, filterProxy_, filterPreferred_}) {
        b->setObjectName(QStringLiteral("Ghost"));
        b->setProperty("chip", QStringLiteral("tool"));
        // 22px 高的 chip 在触摸模式下不能被 PolishHitTargets 拉成 44dp 方块
        b->setProperty("hitGrow", false);
        b->setCursor(Qt::PointingHandCursor);
        tools->addWidget(b);
    }
    connect(filterAll_, &QPushButton::clicked, this, [this] { SetFilter(NodeFilter::All); });
    connect(filterProxy_, &QPushButton::clicked, this,
            [this] { SetFilter(NodeFilter::ProxyGroup); });
    connect(filterPreferred_, &QPushButton::clicked, this,
            [this] { SetFilter(NodeFilter::PreferredGroup); });
    sortBtn_ = new QPushButton(SortButtonText(sortMode_), body);
    sortBtn_->setObjectName(QStringLiteral("Ghost"));
    sortBtn_->setProperty("chip", QStringLiteral("tool"));
    sortBtn_->setProperty("hitGrow", false);
    sortBtn_->setCursor(Qt::PointingHandCursor);
    connect(sortBtn_, &QPushButton::clicked, this, &ProxyPage::CycleSort);
    tools->addWidget(sortBtn_);
    lay->addLayout(tools);

    // ---------------- 列头（demo .colhead：通栏底色 + 上下 1px 边线） ----------------
    auto *colHead = new QWidget(body);
    colHead->setObjectName(QStringLiteral("ProxyColHead"));
    colHead->setAttribute(Qt::WA_StyledBackground, true);
    colHead->setFixedHeight(UiStyle::Fpx(22));
    auto *chLay = new QHBoxLayout(colHead);
    chLay->setContentsMargins(12, 0, 12, 0);
    chLay->setSpacing(8);
    auto *chPad = new QWidget(colHead);          // 与行内单选列同宽
    chPad->setFixedWidth(UiStyle::Fpx(18));
    chLay->addWidget(chPad);
    auto *chLeft = new QLabel(QStringLiteral("节点 / 地区"), colHead);
    chLeft->setObjectName(QStringLiteral("ProxyTableHead"));
    chLay->addWidget(chLeft);
    chLay->addStretch(1);
    auto *chRight = new QLabel(QStringLiteral("延迟 · 评分"), colHead);
    chRight->setObjectName(QStringLiteral("ProxyTableHead"));
    chLay->addWidget(chRight);
    lay->addWidget(colHead);

    // ---------------- 节点行（单选 + 两行文字，分隔线由 qss 画） ----------------
    auto *rows = new QWidget(body);
    nodeHost_ = new QVBoxLayout(rows);
    nodeHost_->setContentsMargins(12, 0, 12, 0);
    nodeHost_->setSpacing(0);
    lay->addWidget(rows);

    // ---------------- 高级抽屉（Python tab_4 的 proxyAdvPanel） ----------------
    auto *advWrap = new QWidget(body);
    auto *advWrapLay = new QVBoxLayout(advWrap);
    advWrapLay->setContentsMargins(10, 8, 10, 2);
    advWrapLay->setSpacing(0);
    advPanel_ = new QWidget(advWrap);
    auto *panelLay = new QVBoxLayout(advPanel_);
    panelLay->setContentsMargins(0, 0, 0, 0);
    panelLay->setSpacing(0);
    // 描边必须画在独立子控件上：面板本身高度在 0→h 之间变化，自绘边框会跟着抖；
    // 且普通 QWidget 默认不画 qss 背景/边框，必须开 WA_StyledBackground。
    advFrame_ = new QWidget(advPanel_);
    advFrame_->setObjectName(QStringLiteral("AdvFrame"));
    advFrame_->setAttribute(Qt::WA_StyledBackground, true);
    auto *av = new QVBoxLayout(advFrame_);
    av->setContentsMargins(10, 8, 10, 8);
    av->setSpacing(6);
    panelLay->addWidget(advFrame_);
    advPanel_->setMaximumHeight(0);
    advPanel_->setVisible(false);
    advWrapLay->addWidget(advPanel_);
    lay->addWidget(advWrap);

    auto fieldLabel = [this](const QString &text) {
        auto *l = new QLabel(text, advFrame_);
        l->setObjectName(QStringLiteral("ProxyFieldLabel"));
        return l;
    };

    auto *dohRow = new QHBoxLayout();
    dohRow->setSpacing(6);
    dohRow->addWidget(fieldLabel(QStringLiteral("DOH")));
    dohEdit_ = new QLineEdit(advFrame_);
    dohEdit_->setObjectName(QStringLiteral("ProxyField"));
    dohEdit_->setProperty("accent", true);   // 下划线用主题色（demo .fld）
    dohEdit_->setPlaceholderText(QStringLiteral("https://parse.jpacg.cc/parse"));
    dohEdit_->setText(ProxyConfig::DohAddress());
    dohRow->addWidget(dohEdit_, 1);
    dohCheckBtn_ = new QPushButton(QStringLiteral("检查"), advFrame_);
    dohCheckBtn_->setObjectName(QStringLiteral("Ghost"));
    dohCheckBtn_->setProperty("chip", QStringLiteral("field"));
    dohCheckBtn_->setProperty("hitGrow", false);
    dohCheckBtn_->setCursor(Qt::PointingHandCursor);
    connect(dohCheckBtn_, &QPushButton::clicked, this, &ProxyPage::CheckDoh);
    dohRow->addWidget(dohCheckBtn_);
    av->addLayout(dohRow);

    dohStatus_ = new QLabel(QStringLiteral("DOH 未检查"), advFrame_);
    dohStatus_->setObjectName(QStringLiteral("ProxyNote"));
    dohStatus_->setWordWrap(true);
    av->addWidget(dohStatus_);

    // 窄屏：25 字说明会把抽屉顶到 314px（视口可用 310），锁横向后右侧被裁
    dohEnable_ = new QCheckBox(narrow_ ? QStringLiteral("用 DOH 结果接管请求")
                                       : QStringLiteral("用 DOH 解析结果接管请求（连接 IP + 域名 SNI）"),
                               advFrame_);
    // 三个复选都只有一行文字：不能进命中区放大流程（否则宽度会被冻在默认 100px，
    // 文案从中间被裁掉 —— 触摸模式实测）
    dohEnable_->setProperty("hitGrow", false);
    dohEnable_->setToolTip(QStringLiteral("用 DOH 解析结果接管请求（连接 IP + 域名 SNI）"));
    dohEnable_->setChecked(ProxyConfig::DohEnabled());
    av->addWidget(dohEnable_);

    auto *switches = new QHBoxLayout();
    switches->setSpacing(12);
    echBox_ = new QCheckBox(QStringLiteral("启用 ECH"), advFrame_);
    echBox_->setProperty("hitGrow", false);
    echBox_->setChecked(ProxyConfig::EchEnabled());
    http3Box_ = new QCheckBox(QStringLiteral("启用 HTTP3"), advFrame_);
    http3Box_->setProperty("hitGrow", false);
    http3Box_->setChecked(ProxyConfig::Http3Enabled());
    switches->addWidget(echBox_);
    switches->addWidget(http3Box_);
    switches->addStretch(1);
    av->addLayout(switches);

    // 地区 / 自定义 IP：Python 的 proxyRegionRow + proxyIpListRow 同样在抽屉里。
    // 地区不再手打代码：只读框点击弹出「已获取到的地区」下拉（见 ShowRegionMenu）。
    auto *regionRow = new QHBoxLayout();
    regionRow->setSpacing(6);
    regionRow->addWidget(fieldLabel(QStringLiteral("地区")));
    regionBox_ = new QWidget(advFrame_);
    regionBox_->setObjectName(QStringLiteral("ProxyFieldBox"));
    regionBox_->setAttribute(Qt::WA_StyledBackground, true);
    regionBox_->setFixedSize(UiStyle::Fpx(110), UiStyle::Fpx(26));
    regionBox_->setCursor(Qt::PointingHandCursor);
    regionBox_->installEventFilter(this);      // 点整框（含右端 ▾）都算点击
    auto *rb = new QHBoxLayout(regionBox_);
    rb->setContentsMargins(6, 0, 5, 0);
    rb->setSpacing(3);
    regionEdit_ = new QLineEdit(regionBox_);
    regionEdit_->setObjectName(QStringLiteral("RegionField"));
    regionEdit_->setReadOnly(true);                 // 只能选，不能改出无效子域
    regionEdit_->setFocusPolicy(Qt::NoFocus);       // 只读框不要闪光标、不抢焦点
    regionEdit_->setCursor(Qt::PointingHandCursor);
    regionEdit_->setPlaceholderText(QStringLiteral("全部地区"));
    regionEdit_->setToolTip(QStringLiteral("点击选择已获取到的地区（留空 = 全部地区）"));
    regionEdit_->installEventFilter(this);          // 点击判定自己做（滑动不算点击）
    regionEdit_->setMinimumWidth(0);
    rb->addWidget(regionEdit_, 1);
    auto *caret = new QLabel(QStringLiteral("▾"), regionBox_);
    caret->setObjectName(QStringLiteral("ProxyCaret"));
    caret->setAttribute(Qt::WA_TransparentForMouseEvents, true);   // 点箭头 = 点框
    rb->addWidget(caret);
    regionRow->addWidget(regionBox_);
    regionStat_ = new QLabel(advFrame_);
    regionStat_->setObjectName(QStringLiteral("ProxyNote"));
    regionStat_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    regionStat_->setMinimumWidth(0);
    regionRow->addWidget(regionStat_, 1);
    av->addLayout(regionRow);

    auto *cfg = new QHBoxLayout();
    cfg->setSpacing(6);
    cfg->addWidget(fieldLabel(QStringLiteral("自定义 IP")));
    customEdit_ = new QLineEdit(advFrame_);
    customEdit_->setObjectName(QStringLiteral("ProxyField"));
    customEdit_->setPlaceholderText(QStringLiteral("逗号 / 空格分隔"));
    customEdit_->setText(ProxyConfig::CustomIpText());
    customEdit_->setMinimumWidth(0);
    cfg->addWidget(customEdit_, 1);
    auto *apply = new QPushButton(QStringLiteral("应用"), advFrame_);
    apply->setObjectName(QStringLiteral("Ghost"));
    apply->setProperty("chip", QStringLiteral("field"));
    apply->setProperty("hitGrow", false);
    apply->setCursor(Qt::PointingHandCursor);
    connect(apply, &QPushButton::clicked, this, &ProxyPage::ApplyCustomIps);
    cfg->addWidget(apply);
    av->addLayout(cfg);

    auto *saveAdv = new QPushButton(narrow_ ? QStringLiteral("保存高级设置")
                                            : QStringLiteral("保存高级设置 / 重试降级项"),
                                    advFrame_);
    saveAdv->setObjectName(QStringLiteral("Ghost"));
    saveAdv->setProperty("chip", QStringLiteral("field"));
    saveAdv->setProperty("hitGrow", false);
    saveAdv->setCursor(Qt::PointingHandCursor);
    connect(saveAdv, &QPushButton::clicked, this, &ProxyPage::SaveAdvanced);
    av->addWidget(saveAdv);

    // demo 的那条脚注：C++ 侧原来只在"启用了代理"时才提示，这里改成常驻说明
    // （规则本身没变，只是不再随模式忽隐忽现）。它排在保存按钮之后，
    // 抽屉的视觉结尾与 demo 一致：脚注 + 一行诊断。
    advNote_ = new QLabel(QStringLiteral("*使用代理时 IP 分流不生效，推荐开启 ECH 功能"), advFrame_);
    advNote_->setObjectName(QStringLiteral("ProxyFootNote"));
    advNote_->setWordWrap(true);
    av->addWidget(advNote_);

    advancedStatus_ = new QLabel(advFrame_);
    advancedStatus_->setObjectName(QStringLiteral("ProxyNote"));
    advancedStatus_->setWordWrap(true);
    av->addWidget(advancedStatus_);

    // 页脚诊断：接口/图片实际走的线路与原因（demo 里没有，属于本页的排障信息）
    effective_ = new QLabel(body);
    effective_->setObjectName(QStringLiteral("ProxyFootNote"));
    effective_->setWordWrap(true);
    effective_->setContentsMargins(12, 6, 12, 12);
    lay->addWidget(effective_);
    lay->addStretch(1);

    // ---------------- 固定底栏（demo .foot：状态 + 高级/收起 + 主操作） ----------------
    auto *foot = new QWidget(this);
    foot->setObjectName(QStringLiteral("ProxyFoot"));
    foot->setAttribute(Qt::WA_StyledBackground, true);
    foot->setFixedHeight(UiStyle::Fpx(54));
    auto *footLay = new QHBoxLayout(foot);
    footLay->setContentsMargins(12, 0, 12, 0);
    footLay->setSpacing(8);
    nodeStatus_ = new ElideLabel(QStringLiteral("未选择节点"), foot);
    nodeStatus_->setObjectName(QStringLiteral("ProxyFootStatus"));
    footLay->addWidget(nodeStatus_, 1);
    advBtn_ = new QPushButton(QStringLiteral("高级"), foot);
    advBtn_->setObjectName(QStringLiteral("Ghost"));
    advBtn_->setProperty("chip", QStringLiteral("foot"));
    advBtn_->setProperty("hitGrow", false);
    advBtn_->setCursor(Qt::PointingHandCursor);
    advBtn_->setToolTip(QStringLiteral("DOH / ECH / HTTP3 / 地区 / 自定义 IP"));
    connect(advBtn_, &QPushButton::clicked, this, &ProxyPage::ToggleAdvanced);
    footLay->addWidget(advBtn_);
    nodeBtn_ = new QPushButton(QStringLiteral("节点测速"), foot);
    nodeBtn_->setObjectName(QStringLiteral("Primary"));
    nodeBtn_->setProperty("chip", QStringLiteral("foot"));
    nodeBtn_->setProperty("hitGrow", false);
    nodeBtn_->setCursor(Qt::PointingHandCursor);
    UiStyle::MarkSoftButton(nodeBtn_);
    connect(nodeBtn_, &QPushButton::clicked, this, &ProxyPage::StartNodeTest);
    footLay->addWidget(nodeBtn_);
    root->addWidget(foot);

    RefreshRegionField();
    UpdateEffectiveHint();
    RefreshAdvancedStatus();
}

void ProxyPage::OnEnter()
{
    apiLine_ = ProxyConfig::ApiLine();
    imgLine_ = ProxyConfig::ImgLine();
    selectedIp_ = ProxyConfig::SelectedIp();
    lineResults_ = ProxyConfig::LineResults();
    if (customEdit_)
        customEdit_->setText(ProxyConfig::CustomIpText());
    RefreshRegionField();
    if (modeGroup_) {
        if (QAbstractButton *b = modeGroup_->button(int(ProxyConfig::Mode())))
            b->setChecked(true);
    }
    if (nodes_.isEmpty())
        nodes_ = ProxyConfig::AllNodes();
    if (dohEdit_)
        dohEdit_->setText(ProxyConfig::DohAddress());
    if (dohEnable_)
        dohEnable_->setChecked(ProxyConfig::DohEnabled());
    if (echBox_)
        echBox_->setChecked(ProxyConfig::EchEnabled());
    if (http3Box_)
        http3Box_->setChecked(ProxyConfig::Http3Enabled());
    RefreshLineRows();
    RenderNodes();
    UpdateEffectiveHint();
    RefreshAdvancedStatus();
}

void ProxyPage::resizeEvent(QResizeEvent *e)
{
    PageBase::resizeEvent(e);
    const bool narrow = Touch::IsMobile() || width() < 620;
    if (narrow == narrow_)
        return;
    narrow_ = narrow;
    RefreshLineRows();
    RenderNodes();
}

// ---------------------------------------------------------------- 高级抽屉
//
// Python login_proxy_new_widget.OnAdvClicked：按钮文案在 高级 / 收起 之间切换，
// 面板按"先量出展开高度、再从 0 长上去"的方式做高度过渡，展开期间给面板加
// 1px 主题色描边，收起后连描边一起撤掉。
// 曲线/时长取本项目的 motion 表（Slow=240ms + Enter/Exit），不用 Python 的
// OutBack 320ms —— 全站动效只能来自 ui/motion.*，见 motion.h 的约束。
void ProxyPage::ToggleAdvanced()
{
    SetAdvancedOpen(!advOpen_, true);
}

void ProxyPage::ExpandAdvanced()
{
    // 截图/回归走"无动画"分支：动画帧不可控，截出来的高度会抖
    SetAdvancedOpen(true, false);
}

void ProxyPage::SetAdvancedOpen(bool open, bool animate)
{
    if (!advPanel_ || !advBtn_)
        return;
    // 连点时先打断上一个补间，否则两个动画同时写 maximumHeight 会互相覆盖
    Motion::Stop(this, QStringLiteral("proxyAdv"));
    advOpen_ = open;
    if (open)
        advScrollClock_.start();
    else
        advScrollClock_.invalidate();
    advBtn_->setText(open ? QStringLiteral("收起") : QStringLiteral("高级"));

    // 展开时描边高亮：属性驱动 qss，避免内联 setStyleSheet 影响子控件
    if (advFrame_) {
        advFrame_->setProperty("open", open);
        advFrame_->style()->unpolish(advFrame_);
        advFrame_->style()->polish(advFrame_);
    }

    if (open) {
        advPanel_->setVisible(true);
        // 先放开高度让布局算出"展开后该多高"，再从 0 长上去
        advPanel_->setMaximumHeight(QWIDGETSIZE_MAX);
        if (QLayout *l = advPanel_->layout())
            l->activate();
        // 抽屉里有 wordWrap 的状态文本：sizeHint() 给的是"不折行"的高度，
        // 折行后的真实高度只有 heightForWidth 算得对，两个取大值才不会展开到一半。
        int target = advFrame_ ? advFrame_->sizeHint().height() : 0;
        if (advFrame_ && advFrame_->width() > 0 && advFrame_->hasHeightForWidth())
            target = qMax(target, advFrame_->heightForWidth(advFrame_->width()));
        target = qMax(1, target);
        advPanel_->setMaximumHeight(0);
        if (!animate) {
            ApplyAdvancedHeight(target);
            advPanel_->setMaximumHeight(QWIDGETSIZE_MAX);
            advPanel_->setMinimumHeight(0);
            // 展开后要把它滚进视口，但首次布局的高度/滚动范围还没定：
            // 一次 ensureWidgetVisible 会按"当时的 maximum"夹住（实测停在列表中间）。
            // 这里隔几个时间点各试一次，advOpen_ 为假时每次都是空操作。
            for (int delay : {0, 150, 800})
                QTimer::singleShot(delay, this, [this] { EnsureAdvancedVisible(); });
            return;
        }
        Motion::Animate(
            this, QStringLiteral("proxyAdv"), 0, target, Motion::Slow, Motion::EnterCurve(),
            [this](qreal v) { ApplyAdvancedHeight(v); },
            // 动画结束交回布局：之后字号/文案变化时高度能自然跟着变
            [this] {
                advPanel_->setMinimumHeight(0);
                advPanel_->setMaximumHeight(QWIDGETSIZE_MAX);
                EnsureAdvancedVisible();
            });
        return;
    }

    const int from = advPanel_->height();
    if (!animate) {
        advPanel_->setVisible(false);
        advPanel_->setMinimumHeight(0);
        advPanel_->setMaximumHeight(0);
        return;
    }
    Motion::Animate(this, QStringLiteral("proxyAdv"), from, 0, Motion::Slow, Motion::ExitCurve(),
                    [this](qreal v) { ApplyAdvancedHeight(v); }, [this] {
                        advPanel_->setVisible(false);
                        advPanel_->setMinimumHeight(0);
                        advPanel_->setMaximumHeight(0);
                    });
}

void ProxyPage::ApplyAdvancedHeight(qreal value)
{
    if (!advPanel_)
        return;
    const int h = qMax(0, qRound(value));
    advPanel_->setMinimumHeight(h);
    advPanel_->setMaximumHeight(h);
}

// 展开后把抽屉滚进视口：高级入口在操作行末尾，桌面窗口不高时抽屉会落在折线以下，
// 不滚一下等于"点了没反应"（Python 的抽屉是底部固定区，天然可见）。
void ProxyPage::EnsureAdvancedVisible()
{
    // 不能拿 advFrame_->isVisible() 当门槛：--quiet / 最小化截图时顶层窗口是隐藏的，
    // 整棵控件树的 isVisible() 都是 false，但布局与 grab() 照常工作。
    // 折叠状态下 advOpen_ 为 false，这里自然不做无谓滚动。
    if (!scroll_ || !advFrame_ || !advOpen_)
        return;
    // 抽屉是整页最后一块内容，滚到底就等于"把抽屉完整露出来"。
    // 不用 ensureWidgetVisible：它按调用瞬间的滚动范围夹取，而展开抽屉时
    // 节点行还在重排，实测会停在列表中间而不是抽屉上。
    QScrollBar *bar = scroll_->verticalScrollBar();
    bar->setValue(bar->maximum());
}

// ---------------------------------------------------------------- 线路
void ProxyPage::RefreshLineRows()
{
    if (!lineGrid_)
        return;
    ClearLayout(lineGrid_);
    if (apiGroup_) {
        for (QAbstractButton *b : apiGroup_->buttons())
            apiGroup_->removeButton(b);
    }
    if (imgGroup_) {
        for (QAbstractButton *b : imgGroup_->buttons())
            imgGroup_->removeButton(b);
    }
    lineDelay_.clear();
    lineSpeed_.clear();
    const UiTokens &tk = UiTheme::I().Tokens();
    QWidget *card = lineGrid_->parentWidget();

    // demo .sgrid：表头 10px 居中/左对齐，行内 11px 单选 + 10.5px 数值，
    // 行与行之间只有一条 1px 分割线（首行之前不画）。
    auto header = [this, card](const QString &text, int col, Qt::Alignment align) {
        auto *l = new QLabel(text, card);
        l->setObjectName(QStringLiteral("ProxyTableHead"));
        l->setAlignment(align | Qt::AlignVCenter);
        lineGrid_->addWidget(l, 0, col);
    };
    header(QStringLiteral("Api分流"), 0, Qt::AlignLeft);
    header(QStringLiteral("延迟"), 1, Qt::AlignHCenter);
    header(QStringLiteral("图片分流"), 2, Qt::AlignLeft);
    header(QStringLiteral("速度"), 3, Qt::AlignHCenter);
    lineGrid_->setRowMinimumHeight(0, UiStyle::Fpx(18));

    // 数值列宽按 demo 的 52/72 取，窄屏不再压缩（右侧两列是纯数字，够用）
    const int delayW = UiStyle::Fpx(narrow_ ? 52 : 60);
    const int speedW = UiStyle::Fpx(narrow_ ? 72 : 86);
    const int rowH = UiStyle::Fpx(26);
    int row = 1;
    bool firstLine = true;
    for (int idx : ProxyConfig::DisplayOrder()) {
        if (!firstLine) {
            lineGrid_->addWidget(Rule(card), row++, 0, 1, 4);
        }
        firstLine = false;
        LineTestResult result;
        for (const LineTestResult &r : lineResults_) {
            if (r.index == idx) {
                result = r;
                break;
            }
        }
        const QString label = ProxyConfig::LineLabel(idx);
        const QString radioLabel =
            narrow_ && idx == 6 ? QStringLiteral("US反代") : label;

        auto *api = new QRadioButton(radioLabel, card);
        api->setAutoExclusive(false);
        api->setProperty("hitGrow", false);   // 26px 行高不能让命中区把它拉到 44dp
        if (apiGroup_)
            apiGroup_->addButton(api);
        api->setChecked(idx == apiLine_);
        api->setToolTip(QStringLiteral("接口走 %1").arg(label));
        connect(api, &QRadioButton::clicked, this, [this, idx, label] {
            if (applyingRadio_)
                return;
            apiLine_ = idx;
            ProxyConfig::SetApiLine(idx);
            ProxyConfig::Save();
            RefreshLineRows();
            SetLineStatus(QStringLiteral("接口线路 → %1").arg(label));
            UpdateEffectiveHint();
            emit Applied();
        });
        lineGrid_->addWidget(api, row, 0, Qt::AlignLeft | Qt::AlignVCenter);

        const QColor delayColor = result.pingOk ? kOk
                                  : result.pingText.isEmpty() ? tk.textDim : tk.danger;
        QLabel *delay = Cell(result.pingText.isEmpty() ? QStringLiteral("—") : result.pingText,
                             delayW, card, delayColor);
        delay->setFixedHeight(rowH);
        lineGrid_->addWidget(delay, row, 1, Qt::AlignCenter);
        lineDelay_.insert(idx, delay);

        auto *img = new QRadioButton(radioLabel, card);
        img->setAutoExclusive(false);
        img->setProperty("hitGrow", false);
        if (imgGroup_)
            imgGroup_->addButton(img);
        img->setChecked(idx == imgLine_);
        img->setToolTip(QStringLiteral("图片走 %1").arg(label));
        connect(img, &QRadioButton::clicked, this, [this, idx, label] {
            if (applyingRadio_)
                return;
            imgLine_ = idx;
            ProxyConfig::SetImgLine(idx);
            ProxyConfig::Save();
            RefreshLineRows();
            SetLineStatus(QStringLiteral("图床线路 → %1").arg(label));
            UpdateEffectiveHint();
            emit Applied();
        });
        lineGrid_->addWidget(img, row, 2, Qt::AlignLeft | Qt::AlignVCenter);

        const QColor speedColor = result.speedOk ? kOk
                                  : result.speedText.isEmpty() ? tk.textDim : tk.danger;
        QLabel *speed = Cell(result.speedText.isEmpty() ? QStringLiteral("—") : result.speedText,
                             speedW, card, speedColor);
        speed->setFixedHeight(rowH);
        lineGrid_->addWidget(speed, row, 3, Qt::AlignCenter);
        lineSpeed_.insert(idx, speed);
        ++row;
    }
}

void ProxyPage::SetLineBusy(bool busy)
{
    if (!lineBtn_)
        return;
    lineBtn_->setEnabled(!busy);
    lineBtn_->setText(busy ? QStringLiteral("测速中") : QStringLiteral("测速"));
    if (autoBtn_)
        autoBtn_->setEnabled(!busy);
}

void ProxyPage::OnLinePing(int index, bool ok, int ms, const QString &error)
{
    QLabel *l = lineDelay_.value(index, nullptr);
    if (!l)
        return;
    l->setText(ok ? NodeTester::FormatDelay(ms) : NodeTester::FailText(error));
    l->setToolTip(error);
    l->setStyleSheet(QStringLiteral("color:%1;").arg(
        (ok ? kOk : UiTheme::I().Tokens().danger).name()));
}

void ProxyPage::OnLineSpeed(int index, bool ok, const QString &text)
{
    QLabel *l = lineSpeed_.value(index, nullptr);
    if (!l)
        return;
    l->setText(text.isEmpty() ? QStringLiteral("失败") : text);
    l->setStyleSheet(QStringLiteral("color:%1;").arg(
        (ok ? kOk : UiTheme::I().Tokens().danger).name()));
}

void ProxyPage::OnLineResult(int index, const LineTestResult &result)
{
    bool replaced = false;
    for (LineTestResult &r : lineResults_) {
        if (r.index == index) {
            r = result;
            replaced = true;
            break;
        }
    }
    if (!replaced)
        lineResults_.append(result);
}

void ProxyPage::StartLineTest()
{
    SetLineBusy(true);
    SetLineStatus(QStringLiteral("正在测速 6 条线路（每线 2 次采样）…"));
    for (QLabel *l : lineDelay_)
        l->setText(QStringLiteral("…"));
    for (QLabel *l : lineSpeed_)
        l->setText(QStringLiteral("…"));
    tester_->TestLines();
}

void ProxyPage::AutoSelectLines()
{
    const QVector<LineTestResult> results =
        lineResults_.isEmpty() ? ProxyConfig::LineResults() : lineResults_;
    const int api = ProxyConfig::RecommendLine(true, results);
    const int img = ProxyConfig::RecommendLine(false, results);
    apiLine_ = api;
    imgLine_ = img;
    ProxyConfig::SetApiLine(api);
    ProxyConfig::SetImgLine(img);
    ProxyConfig::Save();
    RefreshLineRows();
    UpdateEffectiveHint();
    lineStatus_->SetFullText(QStringLiteral("已按测速结果选择：接口 %1 / 图片 %2")
                                 .arg(ProxyConfig::LineLabel(api), ProxyConfig::LineLabel(img)));
    emit Applied();
}

// ---------------------------------------------------------------- 节点
void ProxyPage::RenderNodes()
{
    if (!nodeHost_)
        return;
    ClearLayout(nodeHost_);
    nodeSpd_.clear();
    shownRows_.clear();

    QVector<NodeResult> list;
    for (const NodeResult &r : nodes_) {
        if (ProxyConfig::MatchFilter(r.kind, filter_))
            list.append(r);
    }
    ProxyConfig::SortResults(list, selectedIp_, sortMode_);

    QWidget *host = nodeHost_->parentWidget();
    if (list.isEmpty()) {
        auto *empty = new QLabel(nodes_.isEmpty()
                                     ? QStringLiteral("还没有节点 · 点底部「节点测速」获取")
                                     : QStringLiteral("当前筛选下没有节点"),
                                 host);
        empty->setObjectName(QStringLiteral("ProxyEmpty"));
        empty->setAlignment(Qt::AlignCenter);
        empty->setContentsMargins(12, 26, 12, 26);
        empty->setWordWrap(true);
        nodeHost_->addWidget(empty);
    }

    const int rowH = UiStyle::Fpx(58);
    for (const NodeResult &r : list) {
        int index = -1;
        for (int i = 0; i < nodes_.size(); ++i) {
            if (nodes_.at(i).ip == r.ip) {
                index = i;
                break;
            }
        }
        if (index < 0)
            continue;
        const int row = shownRows_.size();
        shownRows_.append(index);

        // demo .row：58dp 两行（IP + 延迟/评分，标签 + 地区·ISP）。
        // 分隔线与选中底色都由 qss 按 [sel] 画，行内不再放一排固定宽度的格子。
        auto *w = new QWidget(host);
        w->setObjectName(QStringLiteral("ProxyNodeRow"));
        w->setAttribute(Qt::WA_StyledBackground, true);
        w->setProperty("sel", r.ip == selectedIp_);
        // 用 minimumHeight 而不是 fixedHeight：字号放大到 1.5 倍时文字不会顶出边框
        w->setMinimumHeight(rowH);
        auto *hl = new QHBoxLayout(w);
        hl->setContentsMargins(0, 0, 0, 0);
        hl->setSpacing(8);

        auto *pick = new QRadioButton(w);
        pick->setFixedWidth(UiStyle::Fpx(18));
        pick->setProperty("hitGrow", false);
        pick->setChecked(r.ip == selectedIp_);
        pick->setToolTip(QStringLiteral("选择 %1").arg(r.ip));
        connect(pick, &QRadioButton::clicked, this, [this, ip = r.ip] {
            if (applyingRadio_)
                return;
            selectedIp_ = ip;
            ProxyConfig::SetSelectedIp(ip);
            ProxyConfig::Save();
            RenderNodes();
            nodeStatus_->SetFullText(QStringLiteral("已选 %1").arg(ip));
            UpdateEffectiveHint();
            emit Applied();
        });
        hl->addWidget(pick, 0, Qt::AlignVCenter);

        auto *main = new QWidget(w);
        auto *ml = new QVBoxLayout(main);
        ml->setContentsMargins(0, 7, 0, 7);
        ml->setSpacing(2);

        auto *top = new QHBoxLayout();
        top->setSpacing(8);
        auto *ip = new ElideLabel(r.ip, main);
        ip->setObjectName(QStringLiteral("ProxyNodeIp"));
        top->addWidget(ip, 1);
        QLabel *spd = new QLabel(NodeSpeedText(r), main);
        spd->setObjectName(QStringLiteral("ProxyNodeSpd"));
        spd->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        spd->setStyleSheet(QStringLiteral("color:%1;").arg(NodeSpeedColor(r).name()));
        top->addWidget(spd, 0);
        ml->addLayout(top);

        auto *sub = new QHBoxLayout();
        sub->setSpacing(6);
        auto *tag = new ElideLabel(r.ShortName(), main);
        tag->setObjectName(QStringLiteral("ProxyTag"));
        tag->setAlignment(Qt::AlignCenter);
        tag->setFixedWidth(UiStyle::Fpx(64));
        sub->addWidget(tag, 0);
        const bool untested = r.delayMs <= 0 && r.downloadMs <= 0;
        QString geo = r.country;
        if (!r.asn.isEmpty())
            geo = geo.isEmpty() ? r.asn : geo + QStringLiteral(" · ") + r.asn;
        if (geo.isEmpty())
            geo = untested ? QStringLiteral("未测速") : QString();
        auto *geoLabel = new ElideLabel(geo, main);
        geoLabel->setObjectName(QStringLiteral("ProxyNodeGeo"));
        sub->addWidget(geoLabel, 1);
        ml->addLayout(sub);

        hl->addWidget(main, 1);
        nodeSpd_.insert(row, spd);
        nodeHost_->addWidget(w);
    }
    nodeHost_->addStretch(1);

    // Python proxyFilterGroup 的 chip 文案只有分组名，计数统一由工具条 countLabel_
    // 承担（"共 N 项 · 可用 M"）：这样 3 个 chip + 排序在 320dp 也能单行放下。
    if (filterAll_)
        filterAll_->setText(QStringLiteral("全部"));
    if (filterProxy_)
        filterProxy_->setText(QStringLiteral("ProxyIP"));
    if (filterPreferred_)
        filterPreferred_->setText(QStringLiteral("优选"));
    MarkButton(filterAll_, filter_ == NodeFilter::All);
    MarkButton(filterProxy_, filter_ == NodeFilter::ProxyGroup);
    MarkButton(filterPreferred_, filter_ == NodeFilter::PreferredGroup);
    if (sortBtn_)
        sortBtn_->setText(SortButtonText(sortMode_));
    UpdateCount();
    UpdateStatus();
    RefreshRegionStat();

    // 刚展开抽屉的 2s 内，节点行重建会改内容高度，把已经对好的滚动位置顶偏
    // （实测展开瞬间停在列表中间）。这段时间内每次重排后再对准一次；
    // 之后不再干预 —— 用户自己往下滚时不能被拽回去。
    if (advOpen_ && advScrollClock_.isValid() && advScrollClock_.elapsed() < 2500)
        QTimer::singleShot(0, this, [this] { EnsureAdvancedVisible(); });
}

void ProxyPage::RefreshNodeRow(int row)
{
    if (row < 0 || row >= shownRows_.size())
        return;
    const NodeResult &r = nodes_.at(shownRows_.at(row));
    QLabel *spd = nodeSpd_.value(row, nullptr);
    if (!spd)
        return;
    spd->setText(NodeSpeedText(r));
    spd->setStyleSheet(QStringLiteral("color:%1;").arg(NodeSpeedColor(r).name()));
}

void ProxyPage::UpdateCount()
{
    if (!countLabel_)
        return;
    int shown = 0, available = 0;
    for (int idx : shownRows_) {
        const NodeResult &r = nodes_.at(idx);
        ++shown;
        if (r.delayMs > 0)
            ++available;
    }
    countLabel_->SetFullText(available && !narrow_
                                 ? QStringLiteral("共 %1 项 · 可用 %2").arg(shown).arg(available)
                                 : QStringLiteral("共 %1 项").arg(shown));
}

void ProxyPage::UpdateStatus()
{
    if (!nodeStatus_)
        return;
    if (selectedIp_.isEmpty()) {
        nodeStatus_->SetFullText(QStringLiteral("未选择节点"));
        return;
    }
    for (const NodeResult &r : nodes_) {
        if (r.ip == selectedIp_) {
            nodeStatus_->SetFullText(QStringLiteral("已选 %1 · %2 · %3")
                                         .arg(r.ShortName(), r.ip,
                                              r.delayMs > 0 ? QStringLiteral("%1ms").arg(r.delayMs)
                                                            : QStringLiteral("未测速")));
            return;
        }
    }
    nodeStatus_->SetFullText(QStringLiteral("已选 %1（不在当前列表）").arg(selectedIp_));
}

// 测速状态只有一行（在「线路测速」标题右侧），长文案必须省略而不是把按钮挤出去
void ProxyPage::SetLineStatus(const QString &text)
{
    if (lineStatus_)
        lineStatus_->SetFullText(text);
}

void ProxyPage::UpdateEffectiveHint()
{
    if (!effective_)
        return;
    const RoutePlan api = ProxyConfig::ResolveRoute(
        QUrl(ProxyConfig::GetApiUrl2(apiLine_) + QStringLiteral("/latest/")), true);
    const RoutePlan img = ProxyConfig::ResolveRoute(
        QUrl(ProxyConfig::GetImgUrl2(imgLine_) + QStringLiteral("/media/albums/x_3x4.jpg")), false);
    effective_->setText(QStringLiteral("接口：%1（%2） · 图片：%3（%4）")
                            .arg(ProxyConfig::LineLabel(apiLine_), api.reason,
                                 ProxyConfig::LineLabel(imgLine_), img.reason));
    if (modeStatus_) {
        modeStatus_->setText(ProxyConfig::Mode() == HttpProxyMode::None
                                 ? QStringLiteral("未启用代理，直连访问")
                                 : QStringLiteral("代理模式：%1（代理优先，IP 分流不生效）")
                                       .arg(ModeName(int(ProxyConfig::Mode()))));
    }
}

// 高级抽屉里的"已获取 N 个节点"：与 demo 的 .stats-txt 同位置、同语义
void ProxyPage::RefreshRegionStat()
{
    if (!regionStat_)
        return;
    const int n = nodes_.size();
    const UiTokens &tk = UiTheme::I().Tokens();
    regionStat_->setText(n > 0 ? QStringLiteral("已获取 %1 个节点").arg(n)
                               : QStringLiteral("暂无节点"));
    regionStat_->setStyleSheet(QStringLiteral("color:%1;").arg((n > 0 ? kOk : tk.textFaint).name()));
}

void ProxyPage::SetFilter(NodeFilter filter)
{
    filter_ = filter;
    RenderNodes();
}

void ProxyPage::CycleSort()
{
    sortMode_ = sortMode_ == NodeSortMode::Smart ? NodeSortMode::Latency
                : sortMode_ == NodeSortMode::Latency ? NodeSortMode::Download
                                                    : NodeSortMode::Smart;
    RenderNodes();
}

void ProxyPage::RefreshAdvancedStatus()
{
    if (!advancedStatus_)
        return;
    const QString ech = DohClient::I().EchStatus();
    advancedStatus_->setText(ProxyConfig::AdvancedStatus()
                             + (ech.isEmpty() ? QString() : QStringLiteral(" · ") + ech)
                             + QStringLiteral(" · ") + NetBackend::Tls().note);
}

void ProxyPage::CheckDoh()
{
    const QString addr = dohEdit_ ? dohEdit_->text().trimmed() : ProxyConfig::DohAddress();
    if (addr.isEmpty()) {
        if (dohStatus_)
            dohStatus_->setText(QStringLiteral("请填写 DOH 地址"));
        return;
    }
    if (dohStatus_)
        dohStatus_->setText(QStringLiteral("正在检查 %1 …").arg(addr));
    const QString host = QUrl(ProxyConfig::GetApiUrl2(ProxyConfig::ApiLine())).host();
    DohClient::I().CheckAddress(addr, host);
}

void ProxyPage::SaveAdvanced()
{
    ProxyConfig::SetDohAddress(dohEdit_ ? dohEdit_->text() : ProxyConfig::DohAddress());
    if (dohEnable_)
        ProxyConfig::SetDohEnabled(dohEnable_->isChecked());
    if (echBox_)
        ProxyConfig::SetEchEnabled(echBox_->isChecked());
    if (http3Box_)
        ProxyConfig::SetHttp3Enabled(http3Box_->isChecked());
    ProxyConfig::Save();
    // Python 保存代理设置时调 ResetHttp3EchFallback：重新尝试降级过的选项
    ProxyConfig::ResetAdvancedFallback();
    ProxyConfig::SyncDohClient();
    RefreshAdvancedStatus();
    UpdateEffectiveHint();
    if (lineStatus_)
        SetLineStatus(QStringLiteral("高级设置已保存"));
    emit Applied();
}

void ProxyPage::SetProxyMode(int mode)
{
    // Python Setting.IsHttpProxy：非 0 时代理请求由 Qt/curl 自己走，
    // req.py 会把 ipList 清空 —— 即"代理优先于 IP 覆盖"。
    ProxyConfig::SetMode(mode);
    ProxyConfig::Save();
    UpdateEffectiveHint();
    if (lineStatus_)
        SetLineStatus(QStringLiteral("代理模式 → %1").arg(ModeName(mode)));
    emit Applied();
}

void ProxyPage::ApplyCustomIps()
{
    if (customEdit_)
        ProxyConfig::SetCustomIpText(customEdit_->text());
    if (regionEdit_) {
        // 地区框现在显示中文名（下拉选的），写盘前一律归一回二字代码；
        // 认不出来的原样保存（与 Python「表里没有就按原文发」一致）。
        const QString text = regionEdit_->text().trimmed();
        const QString code = RegionCode::ToCode(text);
        ProxyConfig::SetRegionCode(code.isEmpty() ? text : code);
    }
    ProxyConfig::Save();
    nodes_ = ProxyConfig::AllNodes();
    RenderNodes();
    RefreshRegionField();
    if (nodeStatus_)
        nodeStatus_->SetFullText(
            QStringLiteral("自定义 IP / 地区已应用，共 %1 个节点").arg(nodes_.size()));
    emit Applied();
}

// ---------------------------------------------------------------- 自定义 ProxyIP 地区
//
// 与 Python `proxyIpEdit` 一致：候选来自「已获取到的节点」的地区（RegionNames），
// 下拉里显示中文名，存盘/拼子域时换成二字代码（RegionCode::ToCode）。
// 区别：Python 用自研 Popover，这里用 QMenu —— Touch::TapGuard 已覆盖 QMenu
// （滑动超过 8dp 不 triggered），菜单本体也自带"点击外部关闭"。
void ProxyPage::RefreshRegionField()
{
    if (!regionEdit_)
        return;
    const QString code = ProxyConfig::RegionCode();
    const QString name = RegionCode::ToName(code);
    // 表里没有的代码/原文原样显示，避免"存进去了却看不见"；空值时显示 placeholder
    regionEdit_->setText(name.isEmpty() ? code : name);
    RefreshRegionStat();
}

void ProxyPage::FillRegionMenu(QMenu *menu)
{
    if (!menu)
        return;
    const QStringList names = ProxyConfig::RegionNames(nodes_);
    const QString currentCode = RegionCode::ToCode(ProxyConfig::RegionCode());

    QAction *all = menu->addAction(QStringLiteral("全部地区"));
    all->setCheckable(true);
    all->setChecked(currentCode.isEmpty());
    all->setData(QString());                    // 空 = 不限定地区

    if (names.isEmpty()) {
        // 还没测过节点（或 ipinfo 全失败）：给一条明确空态，不做假数据
        QAction *empty = menu->addAction(QStringLiteral("（暂无地区，请先「节点测速」）"));
        empty->setEnabled(false);
        return;
    }
    for (const QString &name : names) {
        QAction *a = menu->addAction(name);
        a->setCheckable(true);
        a->setData(name);
        a->setChecked(!currentCode.isEmpty() && RegionCode::ToCode(name) == currentCode);
    }
}

QMenu *ProxyPage::ShowRegionMenu(const QPoint &globalPos)
{
    if (!regionEdit_)
        return nullptr;
    // 惯性滚动中弹菜单会跟着内容飘：先把页面滚动停住
    if (scroll_) {
        if (QScroller *sc = QScroller::scroller(scroll_))
            sc->stop();
    }

    auto *menu = new QMenu(this);
    menu->setObjectName(QStringLiteral("RegionMenu"));
    menu->setAttribute(Qt::WA_DeleteOnClose);   // popup 关闭后自回收
    FillRegionMenu(menu);

    connect(menu, &QMenu::triggered, this, [this](QAction *a) {
        if (!a || !a->isEnabled())
            return;
        ApplyRegionChoice(a->data().toString());
    });

    menu->setMinimumWidth(qMax(regionBox_ ? regionBox_->width() : UiStyle::Fpx(110),
                               UiStyle::Fpx(168)));
    menu->popup(globalPos);
    return menu;
}

void ProxyPage::ApplyRegionChoice(const QString &name)
{
    const QString text = name.trimmed();
    QString stored;
    if (!text.isEmpty()) {
        // 下拉项是中文名，存盘统一用二字代码（拼子域要它）；表里没有就按原文存
        const QString code = RegionCode::ToCode(text);
        stored = code.isEmpty() ? text : code;
    }
    ProxyConfig::SetRegionCode(stored);
    ProxyConfig::Save();
    RefreshRegionField();
    if (nodeStatus_) {
        if (stored.isEmpty()) {
            nodeStatus_->SetFullText(QStringLiteral("地区 → 全部（重新测速后生效）"));
        } else {
            const QString shown = RegionCode::ToName(stored);
            nodeStatus_->SetFullText(
                QStringLiteral("地区 → %1（重新测速后生效）").arg(shown.isEmpty() ? stored : shown));
        }
    }
    emit Applied();
}

bool ProxyPage::IsRegionTap(const QPoint &pressGlobal, const QPoint &releaseGlobal) const
{
    // 与 Touch::TapGuard 同一条判定：位移不超过 8dp 才算"点击"，
    // 手指按在框上滑动（想滚页面 / 划过去）不弹菜单。
    return (releaseGlobal - pressGlobal).manhattanLength()
           <= Touch::Dp(Touch::kTapSlopDp, regionEdit_);
}

// 弹层锚点：贴住"带下划线的整框"下沿，而不是里面那截只读输入框
QPoint ProxyPage::RegionAnchor() const
{
    QWidget *w = regionBox_ ? regionBox_ : regionEdit_;
    if (!w)
        return QPoint();
    return w->mapToGlobal(QPoint(0, w->height()));
}

bool ProxyPage::eventFilter(QObject *obj, QEvent *ev)
{
    // 整框、框里的只读输入框都算同一个点击面（▾ 是透明穿透，落到框上）
    if (obj != regionEdit_ && obj != regionBox_)
        return PageBase::eventFilter(obj, ev);

    // 只读框自己判"点击"：手指按在框上滑动（页面滚动/误划）不能弹菜单。
    // 阈值与 Touch::TapGuard 同源（8dp），行为才和按钮/菜单一致。
    switch (ev->type()) {
    case QEvent::MouseButtonPress: {
        auto *me = static_cast<QMouseEvent *>(ev);
        if (me->button() == Qt::LeftButton) {
            regionPressPos_ = me->globalPosition().toPoint();
            regionPressed_ = true;
        }
        break;
    }
    case QEvent::MouseMove: {
        auto *me = static_cast<QMouseEvent *>(ev);
        if (regionPressed_ && (me->buttons() & Qt::LeftButton)
            && !IsRegionTap(regionPressPos_, me->globalPosition().toPoint())) {
            regionPressed_ = false;   // 判为拖动，这次不弹
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        auto *me = static_cast<QMouseEvent *>(ev);
        const bool tap = regionPressed_ && me->button() == Qt::LeftButton
                         && IsRegionTap(regionPressPos_, me->globalPosition().toPoint());
        regionPressed_ = false;
        if (tap)
            ShowRegionMenu(RegionAnchor());
        break;
    }
    default:
        break;
    }
    return PageBase::eventFilter(obj, ev);
}

QStringList ProxyPage::PopupRegionMenuForTest()
{
    // 走与真实点击完全相同的路径，只是位置固定；返回真实菜单项文案供断言
    QStringList items;
    QMenu *menu = ShowRegionMenu(RegionAnchor());
    if (menu) {
        for (QAction *a : menu->actions())
            items << a->text();
        menu->close();   // 只取菜单项，不让它挡着后续操作
    }
    return items;
}

QString ProxyPage::TriggerRegionMenuForTest(int index)
{
    // 点菜单项等价于 QMenu::triggered → ApplyRegionChoice：这里直接 trigger 同一批
    // QAction，验证"选中即应用 + 文本框回显"整条链路（点击本身的防误触由 TapGuard 负责）。
    QMenu *menu = ShowRegionMenu(RegionAnchor());
    if (!menu)
        return QStringLiteral("<no-menu>");
    const QList<QAction *> acts = menu->actions();
    if (index < 0 || index >= acts.size()) {
        menu->close();
        return QStringLiteral("<no-item>");
    }
    QAction *a = acts.at(index);
    if (!a->isEnabled()) {
        menu->close();
        return QStringLiteral("<disabled>");
    }
    // 自检不能污染用户配置：先记住原值，走完真实链路再写回
    const QString before = ProxyConfig::RegionCode();
    a->trigger();
    const QString written = ProxyConfig::RegionCode();
    const QString shown = regionEdit_ ? regionEdit_->text() : QString();
    ProxyConfig::SetRegionCode(before);
    ProxyConfig::Save();
    RefreshRegionField();
    menu->close();
    return written + QLatin1Char('|') + shown;
}

void ProxyPage::SeedRegionCandidatesForTest(const QStringList &countries)
{
    // 只改内存里的 nodes_：不写 ui.ini、不碰 ProxyIpLastList，进程退出即消失
    QVector<NodeResult> list;
    for (int i = 0; i < countries.size(); ++i) {
        NodeResult r;
        r.ip = QStringLiteral("10.0.0.%1").arg(i + 1);
        r.kind = NodeKind::ProxyIp;
        r.ordinal = i + 1;
        r.name = ProxyConfig::NodeName(NodeKind::ProxyIp, i + 1);
        r.country = countries.at(i);
        list << r;
    }
    nodes_ = list;
    RenderNodes();
}

void ProxyPage::StartNodeTest()
{
    if (!nodeBtn_)
        return;
    nodeBtn_->setEnabled(false);
    nodeBtn_->setText(QStringLiteral("测速中"));
    nodeStatus_->SetFullText(QStringLiteral("正在获取 ProxyIP 节点并测速…"));
    tester_->TestNodes();
}
