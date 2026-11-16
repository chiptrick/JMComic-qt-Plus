// 对齐 Python 导航栏时补齐的页面：我的评论 / 观看记录 / 本地收藏 / 每周必看 /
// 网络存储 / 批量超分 / 帮助。
//
// 与 Python 的对应关系见 docs/cpp_ui_parity.md §13 的表格。这里只做"页面结构与
// 交互流程对齐"：没有对应服务端接口的页面一律显示空态，不再放预置样例条目。
#include "ui_pages.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QImageWriter>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QtConcurrent>

#include "sr_model_sheet.h"
#include "theme.h"
#include "touch.h"
#include "ui_kit.h"

namespace {

QWidget *PageBody(QWidget *parent, int margin = 28)
{
    auto *w = new QWidget(parent);
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(margin, margin, margin, margin);
    lay->setSpacing(16);
    return w;
}

QVBoxLayout *BodyLayout(QWidget *body)
{
    return qobject_cast<QVBoxLayout *>(body->layout());
}

QWidget *MakeHeader(const QString &title, const QString &sub, QWidget *parent, QHBoxLayout **out)
{
    auto *host = new QWidget(parent);
    auto *lay = new QHBoxLayout(host);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(12);
    auto *texts = new QVBoxLayout();
    texts->setSpacing(2);
    auto *t = new QLabel(title, host);
    t->setObjectName(QStringLiteral("PageTitle"));
    texts->addWidget(t);
    if (!sub.isEmpty()) {
        auto *s = new QLabel(sub, host);
        s->setObjectName(QStringLiteral("PageSub"));
        texts->addWidget(s);
    }
    lay->addLayout(texts);
    lay->addStretch(1);
    if (out)
        *out = lay;
    return host;
}

QScrollArea *WrapScroll(QWidget *content, QWidget *parent)
{
    auto *area = new QScrollArea(parent);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area->setWidget(content);
    // 所有页面级滚动容器统一禁横向平移（此前的副本漏了这一步）
    Touch::LockHorizontalPan(area);
    return area;
}

}  // namespace

// 我的评论 / 观看记录 / 本地收藏 / 网络存储 已迁到独立 TU：
//   ui_pages_cloud.cpp（前三者）、ui_pages_nas.cpp（网络存储）。

// ============================================================ 每周必看 / 连载更新
// Python 对应 view/index/index_view.py 的 GetSerializationInfo：
//   day  = weekBox 索引 + 1，> 7 -> 0（0 = 全部）
//   type = ["all", "manga", "hanman"][categoryBox 索引]
// 响应形状与观看记录一致（{list,total}），因此复用 JmApi 的列表解析。
WeekPage::WeekPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    loading_ = new LoadingBar(this);
    root->addWidget(loading_);

    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    QHBoxLayout *header = nullptr;
    auto *head = MakeHeader(QStringLiteral("每周必看"),
                            QStringLiteral("按类别与星期查看连载更新"), body, &header);
    lay->addWidget(head);
    hint_ = head->findChild<QLabel *>(QStringLiteral("PageSub"));

    type_ = new TouchCombo(body);
    type_->setObjectName(QStringLiteral("SettingCombo"));
    type_->addItem(QStringLiteral("全部"), QStringLiteral("all"));
    type_->addItem(QStringLiteral("日漫"), QStringLiteral("manga"));
    type_->addItem(QStringLiteral("韩漫"), QStringLiteral("hanman"));
    type_->setMinimumWidth(0);
    header->addWidget(type_);

    day_ = new TouchCombo(body);
    day_->setObjectName(QStringLiteral("SettingCombo"));
    day_->addItem(QStringLiteral("全部"), 0);
    const QStringList weekdays = {QStringLiteral("周一"), QStringLiteral("周二"),
                                  QStringLiteral("周三"), QStringLiteral("周四"),
                                  QStringLiteral("周五"), QStringLiteral("周六"),
                                  QStringLiteral("周日")};
    for (int i = 0; i < weekdays.size(); ++i)
        day_->addItem(weekdays.at(i), i + 1);
    day_->setMinimumWidth(0);
    header->addWidget(day_);

    auto *content = new QWidget(body);
    content_ = content;
    auto *cl = new QVBoxLayout(content);
    cl->setContentsMargins(0, 0, 0, 0);
    cl->setSpacing(0);
    grid_ = new ComicGrid(content);
    connect(grid_, &ComicGrid::Activated, this, &WeekPage::OpenComic);
    cl->addWidget(grid_, 1);

    emptyHost_ = new QWidget(content);
    new QVBoxLayout(emptyHost_);
    emptyHost_->layout()->setContentsMargins(0, 0, 0, 0);
    cl->addWidget(emptyHost_, 1);

    pager_ = new QWidget(content);
    auto *pl = new QHBoxLayout(pager_);
    pl->setContentsMargins(0, 12, 0, 0);
    more_ = new QPushButton(QStringLiteral("加载更多"), pager_);
    more_->setObjectName(QStringLiteral("Ghost"));
    more_->setCursor(Qt::PointingHandCursor);
    more_->setProperty("hitGrow", false);
    pl->addStretch(1);
    pl->addWidget(more_);
    pl->addStretch(1);
    pager_->setVisible(false);
    cl->addWidget(pager_);

    lay->addWidget(content, 1);
    root->addWidget(body);

    connect(more_, &QPushButton::clicked, this, [this] { Request(page_ + 1); });
    auto onFilter = [this] { Request(1); };
    connect(type_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onFilter);
    connect(day_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, onFilter);

    SetState(State::Loading);
}

void WeekPage::OnEnter()
{
    if (state_ != State::Ready && state_ != State::Empty)
        SetState(State::Loading);
    Request(1);
}

void WeekPage::Request(int page)
{
    const QString t = type_ ? type_->currentData().toString() : QStringLiteral("all");
    const int d = day_ ? day_->currentData().toInt() : 0;
    page_ = qMax(1, page);
    SetState(State::Loading);
    emit RefreshRequested(t, d, page_);
}

void WeekPage::SelectFilter(int typeIndex, int dayIndex)
{
    if (type_)
        type_->setCurrentIndex(qBound(0, typeIndex, type_->count() - 1));
    if (day_)
        day_->setCurrentIndex(qBound(0, dayIndex, day_->count() - 1));
    Request(1);
}

QString WeekPage::StateName() const
{
    switch (state_) {
    case State::Loading:
        return QStringLiteral("loading");
    case State::Empty:
        return QStringLiteral("empty");
    case State::Error:
        return QStringLiteral("error");
    case State::Ready:
        return QStringLiteral("ready");
    }
    return QStringLiteral("unknown");
}

int WeekPage::CardCount() const
{
    return grid_ ? grid_->Count() : 0;
}

void WeekPage::SetItems(const QVector<Comic> &items, int total, int page)
{
    total_ = total >= 0 ? total : items.size();
    page_ = qMax(1, page);
    if (page_ == 1) {
        if (grid_)
            grid_->SetComics(items);
    } else if (grid_) {
        grid_->AddComics(items);
    }
    if (grid_ && grid_->Count() == 0) {
        SetState(State::Empty);
        return;
    }
    SetState(State::Ready);
}

void WeekPage::SetState(State state, const QString &detail)
{
    state_ = state;
    stateDetail_ = detail;
    ApplyState();
}

void WeekPage::ApplyState()
{
    if (!grid_ || !emptyHost_)
        return;
    if (loading_) {
        if (state_ == State::Loading)
            loading_->Start();
        else
            loading_->Stop();
    }
    const bool ready = state_ == State::Ready;
    grid_->setVisible(ready);
    emptyHost_->setVisible(!ready);
    if (pager_)
        pager_->setVisible(ready && total_ > 0 && page_ * 80 < total_);
    if (hint_)
        hint_->setText(ready || state_ == State::Empty
                           ? QStringLiteral("共 %1 部 · 第 %2 页").arg(total_).arg(page_)
                           : (state_ == State::Loading ? QStringLiteral("正在加载…")
                                                       : QStringLiteral("加载失败")));
    if (ready)
        return;

    while (QLayoutItem *it = emptyHost_->layout()->takeAt(0)) {
        if (QWidget *w = it->widget())
            w->deleteLater();
        delete it;
    }
    QString title;
    QString text;
    QString action;
    if (state_ == State::Loading) {
        title = QStringLiteral("正在加载连载列表…");
        text = QStringLiteral("按类别与星期请求 /serialization。");
    } else if (state_ == State::Error) {
        title = QStringLiteral("连载列表加载失败");
        text = stateDetail_.isEmpty() ? QStringLiteral("请检查网络后重试。") : stateDetail_;
        action = QStringLiteral("重试");
    } else {
        title = QStringLiteral("该筛选下没有作品");
        text = QStringLiteral("换个类别或星期再看。");
    }
    empty_ = new EmptyState(title, text, action, emptyHost_);
    emptyHost_->layout()->addWidget(empty_);
    if (QPushButton *b = empty_->ActionButton()) {
        connect(b, &QPushButton::clicked, this, [this] { Request(1); });
    }
}

// ============================================================ 批量超分
// ============================================================ 批量超分
namespace {

// 卡片容器（与图片超分页同一套 id，保证两页观感一致）
QWidget *MakeBatchCard(QWidget *parent, const QString &title, QVBoxLayout **out)
{
    auto *card = new QWidget(parent);
    card->setObjectName(QStringLiteral("SrCard"));
    auto *l = new QVBoxLayout(card);
    l->setContentsMargins(UiStyle::Fpx(12), UiStyle::Fpx(10), UiStyle::Fpx(12),
                          UiStyle::Fpx(12));
    l->setSpacing(UiStyle::Fpx(8));
    auto *t = new QLabel(title, card);
    t->setObjectName(QStringLiteral("SrCardTitle"));
    l->addWidget(t);
    *out = l;
    return card;
}

// demo 的 .selbox：只读的取值框（放大倍数 / 输出目录这些"由上面推导"的字段）
QWidget *MakeValueBox(QWidget *parent, ElideLabel **value)
{
    auto *box = new QWidget(parent);
    box->setObjectName(QStringLiteral("SrPathBox"));
    box->setAttribute(Qt::WA_StyledBackground, true);
    auto *l = new QHBoxLayout(box);
    l->setContentsMargins(UiStyle::Fpx(8), UiStyle::Fpx(4), UiStyle::Fpx(8), UiStyle::Fpx(4));
    l->setSpacing(0);
    auto *v = new ElideLabel(QStringLiteral("—"), box);
    v->setObjectName(QStringLiteral("SrPathText"));
    l->addWidget(v, 1);
    *value = v;
    return box;
}

// 目录/倍数这类"标签在上、控件在下"的两列（demo 的 .grid2 + .lab）
QWidget *MakeFieldColumn(QWidget *parent, const QString &label, QWidget *control)
{
    auto *w = new QWidget(parent);
    auto *l = new QVBoxLayout(w);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(UiStyle::Fpx(4));
    auto *t = new QLabel(label, w);
    t->setObjectName(QStringLiteral("SrKvKey"));
    l->addWidget(t);
    l->addWidget(control);
    return w;
}

}  // namespace

BatchSrPage::BatchSrPage(QWidget *parent) : PageBase(parent)
{
    watcher_ = new QFutureWatcher<QString>(this);
    connect(watcher_, &QFutureWatcher<QString>::finished, this, [this] {
        busy_ = false;
        if (start_)
            start_->setEnabled(true);
        const QString summary = watcher_->result();
        if (hint_)
            hint_->setText(summary);
        if (progress_)
            progress_->setValue(progress_->maximum());
        emit Toast(summary);
    });

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this, Touch::IsTouchMode() ? UiStyle::Fpx(12) : 28);
    auto *lay = BodyLayout(body);

    lay->addWidget(MakeHeader(QStringLiteral("批量超分"),
                              QStringLiteral("对整个目录做超分，结果输出到 <目录>/out"), body,
                              nullptr));

    // ---- 导入目录（demo 的 .field：标签 + 路径框 + 按钮）----
    auto *dirRow = new QWidget(body);
    auto *dl = new QHBoxLayout(dirRow);
    dl->setContentsMargins(0, 0, 0, 0);
    dl->setSpacing(UiStyle::Fpx(8));
    auto *dirLabel = new QLabel(QStringLiteral("导入目录"), dirRow);
    dirLabel->setObjectName(QStringLiteral("SrFieldLabel"));
    dl->addWidget(dirLabel, 0);
    dir_ = new ElideLabel(QStringLiteral("尚未选择"), dirRow);
    dir_->setObjectName(QStringLiteral("SrPathText"));
    auto *dirBox = new QWidget(dirRow);
    dirBox->setObjectName(QStringLiteral("SrPathBox"));
    dirBox->setAttribute(Qt::WA_StyledBackground, true);
    auto *dbl = new QHBoxLayout(dirBox);
    dbl->setContentsMargins(UiStyle::Fpx(8), UiStyle::Fpx(6), UiStyle::Fpx(8), UiStyle::Fpx(6));
    dbl->addWidget(dir_, 1);
    dl->addWidget(dirBox, 1);
    auto *pick = new QPushButton(QStringLiteral("选择…"), dirRow);
    pick->setObjectName(QStringLiteral("Ghost"));
    pick->setCursor(Qt::PointingHandCursor);
    pick->setProperty("hitGrow", false);
    dl->addWidget(pick, 0);
    lay->addWidget(dirRow);

    // ---- 输出参数：整行模型选择栏 + 两列参数 ----
    QVBoxLayout *c = nullptr;
    auto *card = MakeBatchCard(body, QStringLiteral("输出参数"), &c);
    modelField_ = new SrModelField(card);
    c->addWidget(modelField_);
    connect(modelField_, &SrModelField::clicked, this, [this] {
        const QString keep = modelName_.isEmpty() ? SrEngine::CurrentModel() : modelName_;
        const QString picked = SrModelSheet::Pick(this, keep);
        if (picked.isEmpty())
            return;
        modelName_ = picked;
        if (modelField_)
            modelField_->SetModel(picked);
        if (scaleVal_) {
            int sc = 0;
            for (const SrEngine::ModelInfo &m : SrEngine::Catalog()) {
                if (m.name == picked) {
                    sc = m.scale;
                    break;
                }
            }
            scaleVal_->SetFullText(sc > 0 ? QStringLiteral("%1×（随模型）").arg(sc)
                                          : QStringLiteral("—"));
        }
    });

    auto *grid = new QHBoxLayout();
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(UiStyle::Fpx(8));
    ElideLabel *scaleValue = nullptr;
    grid->addWidget(MakeFieldColumn(card, QStringLiteral("放大倍数"),
                                    MakeValueBox(card, &scaleValue)), 1);
    scaleVal_ = scaleValue;

    fmt_ = new TouchCombo(card);
    fmt_->setObjectName(QStringLiteral("SettingCombo"));
    // 只列 Qt 真正写得出来的格式：Qt 6.12 的安装包不带 qwebp 插件（内页解码
    // 走的是自带 libwebp，编码器没有），把 WebP 摆上去只会得到"写入失败"。
    QStringList writeFormats;
    const QList<QByteArray> supported = QImageWriter::supportedImageFormats();
    if (supported.contains("png"))
        writeFormats << QStringLiteral("PNG");
    if (supported.contains("jpeg") || supported.contains("jpg"))
        writeFormats << QStringLiteral("JPEG");
    if (supported.contains("webp"))
        writeFormats << QStringLiteral("WebP");
    if (writeFormats.isEmpty())
        writeFormats << QStringLiteral("PNG");
    fmt_->addItems(writeFormats);
    fmt_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    fmt_->setMinimumContentsLength(4);
    if (Touch::IsTouchMode())
        fmt_->setMinimumWidth(0);
    grid->addWidget(MakeFieldColumn(card, QStringLiteral("转换格式"), fmt_), 1);
    c->addLayout(grid);

    ElideLabel *outValue = nullptr;
    auto *outBox = MakeValueBox(card, &outValue);
    outVal_ = outValue;
    c->addWidget(MakeFieldColumn(card, QStringLiteral("输出目录"), outBox));

    progress_ = new QProgressBar(card);
    progress_->setObjectName(QStringLiteral("RowProgress"));
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->setTextVisible(true);
    progress_->setFixedHeight(UiStyle::Fpx(10));
    // 没开始时别画一条空轨道：在深色主题下它就是一条黑杠，像渲染出错
    progress_->setVisible(false);
    c->addWidget(progress_);

    // 完成摘要里有输出目录，可能很长：用会折行的 QLabel，别让路径被省略掉
    hint_ = new QLabel(QStringLiteral("尚未开始。超分在后台线程里跑，不会卡住界面。"), card);
    hint_->setObjectName(QStringLiteral("SrKvKey"));
    hint_->setWordWrap(true);
    hint_->setMinimumWidth(0);
    c->addWidget(hint_);

    start_ = new QPushButton(QStringLiteral("开始处理"), card);
    start_->setObjectName(QStringLiteral("Primary"));
    UiStyle::MarkSoftButton(start_);
    start_->setCursor(Qt::PointingHandCursor);
    c->addWidget(start_);
    lay->addWidget(card);

    connect(pick, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(
            this, QStringLiteral("选择要超分的目录"),
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
        if (dir.isEmpty())
            return;
        dirPath_ = dir;
        dir_->SetFullText(dir);
        UpdateOutDir();
        int count = 0;
        QDirIterator it(dir, {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                              QStringLiteral("*.png"), QStringLiteral("*.webp")},
                        QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            ++count;
        }
        if (hint_)
            hint_->setText(count > 0
                                   ? QStringLiteral("已选择目录，共 %1 张图片待处理。").arg(count)
                                   : QStringLiteral("该目录下没有图片。"));
    });
    connect(start_, &QPushButton::clicked, this, &BatchSrPage::StartBatch);

    // 不加 stretch 的话，QVBoxLayout 会把多余高度摊给标题和卡片，
    // 表现为"标题飘到页面上半空、卡片浮在中间"（第一版就是这样）。
    lay->addStretch(1);
    root->addWidget(WrapScroll(body, this));

    UpdateOutDir();
}

void BatchSrPage::OnEnter()
{
    if (!modelField_)
        return;
    // 进页时对齐引擎当前模型（上一次批处理选的那个只在本页有效，不写 ui.ini）
    const QString keep = modelName_.isEmpty() ? SrEngine::CurrentModel() : modelName_;
    const QVector<SrEngine::ModelInfo> all = SrEngine::Catalog();
    int scale = 0;
    for (const SrEngine::ModelInfo &m : all) {
        if (m.name == keep) {
            scale = m.scale;
            break;
        }
    }
    modelName_ = keep;
    modelField_->SetModel(keep);
    if (scaleVal_)
        scaleVal_->SetFullText(scale > 0 ? QStringLiteral("%1×（随模型）").arg(scale)
                                         : QStringLiteral("—"));
    UpdateOutDir();
    if (start_) {
        const bool anyInstalled = !SrEngine::InstalledModels().isEmpty();
        start_->setEnabled(anyInstalled && !busy_);
    }
    if (hint_ && hint_->text().isEmpty())
        hint_->setText(QStringLiteral("超分在后台线程里跑，不会卡住界面。"));
}

void BatchSrPage::UpdateOutDir()
{
    if (!outVal_)
        return;
    outVal_->SetFullText(dirPath_.isEmpty()
                             ? QStringLiteral("选择目录后自动创建 <目录>/out")
                             : QDir(dirPath_).filePath(QStringLiteral("out")));
}

void BatchSrPage::StartBatch()
{
    if (busy_)
        return;
    if (dirPath_.isEmpty() || !QFileInfo(dirPath_).isDir()) {
        emit Toast(QStringLiteral("请先选择目录"));
        return;
    }
    if (!SrEngine::Available()) {
        emit Toast(QStringLiteral("超分不可用：%1").arg(SrEngine::Status()));
        if (hint_)
            hint_->setText(QStringLiteral("超分不可用：%1").arg(SrEngine::Status()));
        return;
    }
    const QString modelName = modelName_.isEmpty() ? SrEngine::CurrentModel() : modelName_;
    const QString outDir = QDir(dirPath_).filePath(QStringLiteral("out"));
    QDir().mkpath(outDir);
    QStringList files;
    QDirIterator it(dirPath_, {QStringLiteral("*.jpg"), QStringLiteral("*.jpeg"),
                               QStringLiteral("*.png"), QStringLiteral("*.webp")},
                   QDir::Files);
    while (it.hasNext())
        files << it.next();
    if (files.isEmpty()) {
        emit Toast(QStringLiteral("该目录下没有图片"));
        return;
    }
    busy_ = true;
    modelName_ = modelName;
    if (start_)
        start_->setEnabled(false);
    if (progress_) {
        progress_->setRange(0, files.size());
        progress_->setValue(0);
        progress_->setVisible(true);
    }
    if (hint_)
        hint_->setText(QStringLiteral("开始处理 %1 张（%2）…").arg(files.size()).arg(modelName));

    QPointer<BatchSrPage> guard(this);
    auto report = [guard](int done, int total, const QString &name) {
        if (!guard)
            return;
        QMetaObject::invokeMethod(
            guard,
            [guard, done, total, name] {
                if (!guard)
                    return;
                if (guard->progress_)
                    guard->progress_->setValue(done);
                if (guard->hint_)
                    guard->hint_->setText(QStringLiteral("处理中 %1/%2 · %3")
                                              .arg(done)
                                              .arg(total)
                                              .arg(name));
            },
            Qt::QueuedConnection);
    };
    const QString fmtName = fmt_ ? fmt_->currentText() : QStringLiteral("PNG");
    const QString ext = fmtName == QStringLiteral("JPEG")
                            ? QStringLiteral("_sr.jpg")
                            : (fmtName == QStringLiteral("WebP") ? QStringLiteral("_sr.webp")
                                                                 : QStringLiteral("_sr.png"));
    const int quality = fmtName == QStringLiteral("JPEG") ? 92 : -1;
    watcher_->setFuture(QtConcurrent::run([files, modelName, outDir, ext, quality, report] {
        int ok = 0;
        int fail = 0;
        QString firstError;
        for (int i = 0; i < files.size(); ++i) {
            const QString path = files.at(i);
            const QImage in(path);
            if (in.isNull()) {
                ++fail;
                if (firstError.isEmpty())
                    firstError = QStringLiteral("读取失败：%1").arg(QFileInfo(path).fileName());
                report(i + 1, files.size(), QFileInfo(path).fileName());
                continue;
            }
            const SrEngine::Result r = SrEngine::UpscaleEx(in, modelName, nullptr, nullptr);
            if (r.image.isNull()) {
                ++fail;
                if (firstError.isEmpty())
                    firstError = QStringLiteral("%1：%2").arg(QFileInfo(path).fileName(), r.error);
            } else {
                const QString out =
                    QDir(outDir).filePath(QFileInfo(path).completeBaseName() + ext);
                if (r.image.save(out, nullptr, quality))
                    ++ok;
                else {
                    ++fail;
                    if (firstError.isEmpty())
                        firstError = QStringLiteral("写入失败：%1").arg(out);
                }
            }
            report(i + 1, files.size(), QFileInfo(path).fileName());
        }
        QString summary = QStringLiteral("批量超分完成：成功 %1 张，失败 %2 张 → %3")
                              .arg(ok)
                              .arg(fail)
                              .arg(outDir);
        if (!firstError.isEmpty())
            summary += QStringLiteral("（首个错误：%1）").arg(firstError);
        return summary;
    }));
}

// ============================================================ 帮助
HelpPage::HelpPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this);
    auto *lay = BodyLayout(body);

    lay->addWidget(MakeHeader(QStringLiteral("帮助"),
                              QStringLiteral("常见问题、快捷键与日志位置"), body, nullptr));

    struct Entry {
        const char *q;
        const char *a;
    };
    const Entry entries[] = {
        {"为什么封面加载不出来？",
         "封面走内存 → 磁盘 → 网络三级缓存。先在设置里检查网络与代理，再点封面右上角重试。"},
        {"阅读器怎么翻页？",
         "左右滑动翻页；点按屏幕左下/右下三分之一也是翻页，点上方左右角显示/隐藏工具条。"},
        {"下载的文件在哪？",
         "默认在应用数据目录的 download/ 下（桌面：AppData\\LocalLow\\JMComic\\JMComic\\download），"
         "可在设置 → 下载与缓存 → 保存目录里改成任意可写目录；下载页右上角「打开目录」可直接看。"},
        {"如何反馈问题？",
         "设置 → 关于里可以打开日志目录，把当天的日志一起发出来即可。"},
    };
    for (const Entry &e : entries) {
        auto *card = new QWidget(body);
        card->setObjectName(QStringLiteral("ListRow"));
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(14, 10, 14, 10);
        cl->setSpacing(4);
        auto *q = new QLabel(QString::fromUtf8(e.q), card);
        q->setObjectName(QStringLiteral("RowTitle"));
        cl->addWidget(q);
        auto *a = new QLabel(QString::fromUtf8(e.a), card);
        a->setObjectName(QStringLiteral("SettingDesc"));
        a->setWordWrap(true);
        cl->addWidget(a);
        lay->addWidget(card);
    }

    auto *ver = new QLabel(QStringLiteral("JMComic C++ UI · %1 · Qt %2")
                               .arg(QCoreApplication::applicationVersion(),
                                    QString::fromLatin1(qVersion())),
                           body);
    ver->setObjectName(QStringLiteral("PageSub"));
    lay->addWidget(ver);
    lay->addStretch(1);

    root->addWidget(WrapScroll(body, this));
}
