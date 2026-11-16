// 超分模型选择浮层 + 整行选择栏的实现（见 sr_model_sheet.h 的设计说明）。
#include "sr_model_sheet.h"

#include <QApplication>
#include <QEventLoop>
#include <QFontMetrics>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScrollBar>
#include <QStyledItemDelegate>
#include <QStyle>
#include <QTimer>
#include <QVBoxLayout>

#include "../sr_engine.h"
#include "motion.h"
#include "sr_bench.h"
#include "theme.h"
#include "touch.h"
#include "ui_kit.h"

namespace {

const UiTokens &T() { return UiTheme::I().Tokens(); }

QFont UiFont(int px)
{
    QFont f;
    f.setPixelSize(UiStyle::Fpx(px));
    return f;
}

// 模型 ID 必须等宽：一列 ID 只有字宽相同才对得齐（与 theme.cpp 的 #ProxyNodeIp 同族）。
QFont MonoFont(int px)
{
    QFont f;
    f.setFamilies({QStringLiteral("Consolas"), QStringLiteral("Menlo"),
                   QStringLiteral("DejaVu Sans Mono"), QStringLiteral("monospace")});
    f.setPixelSize(UiStyle::Fpx(px));
    return f;
}

QString ArchLabel(const QString &arch)
{
    if (arch == QLatin1String("waifu2x"))
        return QStringLiteral("waifu2x");
    if (arch == QLatin1String("realcugan"))
        return QStringLiteral("Real-CUGAN");
    if (arch == QLatin1String("realesrgan"))
        return QStringLiteral("Real-ESRGAN");
    if (arch == QLatin1String("srmd"))
        return QStringLiteral("SRMD");
    if (arch == QLatin1String("quant"))
        return QStringLiteral("量化模型（QNN / CPU）");
    return arch;
}

// 筛选 chip 用短名：分组条里已经是全称，chip 只负责"定位到哪一组"。
// 「量化模型（QNN / CPU）」这种长名会把 chip 行从 2 行挤成 3 行，白吃掉 30dp。
QString ArchChipLabel(const QString &arch)
{
    if (arch == QLatin1String("quant"))
        return QStringLiteral("量化");
    return ArchLabel(arch);
}

// 分组色点：只用来区分算法，与主题强调色无关（橙主题下也不能四组同色）。
QColor ArchColor(const QString &arch)
{
    if (arch == QLatin1String("waifu2x"))
        return QColor(0x57, 0xa9, 0xa1);
    if (arch == QLatin1String("realcugan"))
        return QColor(0x8f, 0x83, 0xd6);
    if (arch == QLatin1String("realesrgan"))
        return QColor(0xd3, 0xa0, 0x3c);
    if (arch == QLatin1String("srmd"))
        return QColor(0x6f, 0x9a, 0xd6);
    if (arch == QLatin1String("quant"))
        return QColor(0xb0, 0x7a, 0x8c);
    return T().textFaint;
}

QString SizeText(qint64 n)
{
    if (n <= 0)
        return QStringLiteral("—");
    if (n >= qint64(1024) * 1024)
        return QStringLiteral("%1 MB").arg(double(n) / (1024.0 * 1024.0), 0, 'f', 1);
    return QStringLiteral("%1 KB").arg(double(n) / 1024.0, 0, 'f', 0);
}

const SrEngine::ModelInfo *FindModel(const QVector<SrEngine::ModelInfo> &list, const QString &name)
{
    for (const SrEngine::ModelInfo &m : list) {
        if (m.name == name)
            return &m;
    }
    return nullptr;
}

QColor RowLineColor()
{
    return T().dark ? QColor(255, 255, 255, 14) : QColor(0, 0, 0, 16);
}

QColor RowHoverColor()
{
    return T().dark ? QColor(255, 255, 255, 12) : QColor(0, 0, 0, 8);
}

// 分组条：列表里的分组行与吸顶覆盖条共用这一份绘制，样式永远一致。
void PaintGroupBand(QPainter *p, const QRect &r, const QString &arch, int count)
{
    if (r.isEmpty())
        return;
    p->fillRect(r, T().bgAlt);
    p->setPen(T().border);
    p->drawLine(r.left(), r.top(), r.right() - 1, r.top());
    p->drawLine(r.left(), r.bottom() - 1, r.right() - 1, r.bottom() - 1);

    // 直角风格：色点画成 6px 方块（全站不用圆形/胶囊）
    const int dot = UiStyle::Fpx(6);
    const QRect dotRect(r.left() + UiStyle::Fpx(13), r.center().y() - dot / 2, dot, dot);
    p->fillRect(dotRect, ArchColor(arch));

    QFont f = UiFont(10);
    p->setFont(f);
    p->setPen(T().textFaint);
    const QRect textRect = r.adjusted(UiStyle::Fpx(13) + dot + UiStyle::Fpx(7), 0,
                                      -UiStyle::Fpx(13), 0);
    QFontMetrics fm(f);
    p->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(ArchLabel(arch),
                                                                         Qt::ElideRight,
                                                                         textRect.width()));

    if (count > 0) {
        QFont cf = MonoFont(10);
        p->setFont(cf);
        p->setPen(T().textFaint);
        p->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, QString::number(count));
    }
}

int BadgeWidth()
{
    QFont f = UiFont(10);
    const QFontMetrics fm(f);
    return qMax(UiStyle::Fpx(62), fm.horizontalAdvance(QStringLiteral("当前使用"))
                                      + UiStyle::Fpx(10));
}

int GroupRowHeight() { return UiStyle::Fpx(26); }

int RowPadX() { return UiStyle::Fpx(13); }
int RowPadY() { return UiStyle::Fpx(7); }
int RowGap() { return UiStyle::Fpx(8); }

int RowHeight()
{
    const QFontMetrics nm(UiFont(13));
    const QFontMetrics im(MonoFont(10));
    return RowPadY() * 2 + nm.height() + UiStyle::Fpx(2) + im.height();
}

// ---------------------------------------------------------------- 条目角色
enum Role {
    GroupRole = Qt::UserRole + 1,
    ArchRole,
    FileRole,
    NameRole,
    SizeRole,
    CountRole,
    CurrentRole,
    ChosenRole,
    InstalledRole,
};

// ---------------------------------------------------------------- 测速快照
// 后台任务写 SrBench，浮层把当前快照拷到这里；delegate 只读这份快照，
// 避免每画一行都去抢 store 的锁。
struct BenchView {
    QHash<QString, QHash<QString, SrBench::Sample>> results;
    QString activeModel;
    QString activeHw;
    bool running = false;
};

int TimingLineH()
{
    return QFontMetrics(MonoFont(10)).height();
}

struct TimingSeg {
    QString label;   // 硬件短名（分隔符时为空）
    QString value;   // "66 ms" / "测试中…" / 失败原因
    QColor color;
    bool separator = false;
};

QString FormatBenchMs(double ms)
{
    if (ms >= 1000.0)
        return QStringLiteral("%1 s").arg(ms / 1000.0, 0, 'f', 2);
    return QStringLiteral("%1 ms").arg(qRound(ms));
}

QVector<TimingSeg> TimingSegments(const BenchView &view, const QString &model)
{
    QVector<TimingSeg> out;
    const auto res = view.results.value(model);
    if (res.isEmpty() && !(view.running && view.activeModel == model))
        return out;
    double best = 1e30;
    QString bestId;
    for (auto it = res.constBegin(); it != res.constEnd(); ++it) {
        if (it->ok && it->ms > 0 && it->ms < best) {
            best = it->ms;
            bestId = it.key();
        }
    }
    for (const SrEngine::BenchHw &hw : SrBench::Hardware()) {
        if (!hw.available)
            continue;   // 不可用硬件只在工具条下的图例里写一次
        const auto it = res.constFind(hw.id);
        const bool active = view.running && view.activeModel == model && view.activeHw == hw.id;
        TimingSeg seg;
        seg.label = hw.shortLabel;
        if (it != res.constEnd()) {
            if (it->ok) {
                seg.value = FormatBenchMs(it->ms);
                seg.color = hw.id == bestId ? T().accent : T().textDim;
            } else {
                seg.value = it->note.isEmpty() ? QStringLiteral("失败") : it->note;
                seg.color = T().danger;
            }
        } else if (active) {
            seg.value = QStringLiteral("测试中…");
            seg.color = T().accent;
        } else {
            continue;
        }
        out.push_back(seg);
    }
    return out;
}

// 按可用宽度折行（最多 3 行，再宽就不折了，交给裁剪）
QVector<QVector<TimingSeg>> WrapTiming(const QVector<TimingSeg> &segs, int maxW,
                                       const QFontMetrics &fm)
{
    QVector<QVector<TimingSeg>> lines;
    QVector<TimingSeg> cur;
    int w = 0;
    const int sepW = 16;
    for (const TimingSeg &s : segs) {
        const int sw = fm.horizontalAdvance(s.label) + (s.label.isEmpty() ? 0 : 4)
                       + fm.horizontalAdvance(s.value);
        if (!cur.isEmpty() && w + sepW + sw > maxW && lines.size() < 2) {
            lines.push_back(cur);
            cur.clear();
            w = 0;
        }
        if (!cur.isEmpty()) {
            TimingSeg sep;
            sep.separator = true;
            cur.push_back(sep);
            w += sepW;
        }
        cur.push_back(s);
        w += sw;
    }
    if (!cur.isEmpty())
        lines.push_back(cur);
    return lines;
}

int TimingHeight(const BenchView &view, const QString &model, int maxW)
{
    const auto segs = TimingSegments(view, model);
    if (segs.isEmpty())
        return 0;
    const QFontMetrics fm(MonoFont(10));
    return int(WrapTiming(segs, maxW, fm).size()) * fm.height();
}

void PaintTiming(QPainter *p, const QRect &area, const BenchView &view, const QString &model)
{
    const auto segs = TimingSegments(view, model);
    if (segs.isEmpty() || area.width() <= 0)
        return;
    const QFont f = MonoFont(10);
    const QFontMetrics fm(f);
    const auto lines = WrapTiming(segs, area.width(), fm);
    p->setFont(f);
    int y = area.top();
    for (const auto &line : lines) {
        int x = area.left();
        for (const TimingSeg &s : line) {
            if (s.separator) {
                p->setPen(T().textFaint);
                p->drawText(QRect(x, y, 16, fm.height()),
                            Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("·"));
                x += 16;
                continue;
            }
            p->setPen(T().textFaint);
            const int lw = fm.horizontalAdvance(s.label);
            p->drawText(QRect(x, y, lw + 2, fm.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, s.label);
            x += lw + 4;
            p->setPen(s.color);
            const int vw = fm.horizontalAdvance(s.value);
            p->drawText(QRect(x, y, vw + 2, fm.height()),
                        Qt::AlignLeft | Qt::AlignVCenter, s.value);
            x += vw;
        }
        y += fm.height();
    }
}

// 2px 进度条（demo 里就是一条细线，不用 QProgressBar 的块状样式）
class BenchBar : public QWidget
{
public:
    explicit BenchBar(QWidget *parent = nullptr) : QWidget(parent) {}
    void SetFraction(qreal f)
    {
        f_ = qBound<qreal>(0.0, f, 1.0);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), T().border);
        if (f_ > 0.0)
            p.fillRect(QRectF(0, 0, width() * f_, height()), T().accent);
    }

private:
    qreal f_ = 0.0;
};

// ---------------------------------------------------------------- delegate
class SrModelDelegate : public QStyledItemDelegate
{
public:
    explicit SrModelDelegate(const BenchView *view, QObject *parent = nullptr)
        : QStyledItemDelegate(parent), view_(view) {}

    QSize sizeHint(const QStyleOptionViewItem &opt, const QModelIndex &index) const override
    {
        if (index.data(GroupRole).toBool())
            return QSize(0, GroupRowHeight());
        int h = RowHeight();
        if (view_) {
            // 有耗时行时行高要加上折行后的高度（宽度按当前视口算）
            h += TimingHeight(*view_, index.data(NameRole).toString(),
                              qMax(UiStyle::Fpx(60), ViewWidth(opt) - RowPadX() * 2));
            if (h > RowHeight())
                h += UiStyle::Fpx(3);
        }
        return QSize(0, h);
    }

    int ViewWidth(const QStyleOptionViewItem &opt) const
    {
        if (opt.rect.width() > 0)
            return opt.rect.width();
        if (auto *v = qobject_cast<QListWidget *>(parent()))
            return v->viewport()->width();
        return 400;
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt,
               const QModelIndex &index) const override
    {
        if (index.data(GroupRole).toBool()) {
            PaintGroupBand(p, opt.rect, index.data(ArchRole).toString(),
                           index.data(CountRole).toInt());
            return;
        }

        const bool installed = index.data(InstalledRole).toBool();
        const bool current = index.data(CurrentRole).toBool();
        const bool chosen = index.data(ChosenRole).toBool();
        const QRect r = opt.rect;

        p->save();
        p->setClipRect(r);
        // 先铺不透明底：viewport 是透明的（卡片底色透上来），半透明高亮直接叠
        // 上去会和背后的黑底混合 —— 实测「当前使用」整行变成黑条。
        p->fillRect(r, T().surface);

        if (current) {
            // 不要用 UiTokens::accentSoft：那个字段是 QString 直接赋给 QColor 的，
            // 解析失败会变成黑色（其它页面用 .name() 也早就吃到这个坑）。这里现算。
            QColor soft = T().accent;
            soft.setAlpha(T().dark ? 34 : 30);
            p->fillRect(r, soft);
        }
        else if (chosen)
            p->fillRect(r, T().surfaceHover);
        else if (opt.state & QStyle::State_MouseOver)
            p->fillRect(r, RowHoverColor());

        // 左侧色条：「当前使用」实心主色，本次「已选」半透明，两者一眼可分
        if (current || chosen) {
            QColor rail = T().accent;
            if (!current)
                rail.setAlpha(110);
            p->fillRect(QRect(r.left(), r.top() + UiStyle::Fpx(6), UiStyle::Fpx(3),
                              r.height() - UiStyle::Fpx(12)),
                        rail);
        }
        p->fillRect(QRect(r.left(), r.bottom(), r.width(), 1), RowLineColor());

        const int badgeW = BadgeWidth();
        // 三列门槛：留下 badge 列后模型名/ID 仍要放得下。用 340 而不是 400 ——
        // 480dp 窗口扣掉 4px 滚动条后，Fpx(400) 在 1.33 字号下会把它判成"太窄"。
        const bool showSize = r.width() >= UiStyle::Fpx(340);
        const int sizeW = showSize ? UiStyle::Fpx(54) : 0;
        int nameW = r.width() - RowPadX() * 2 - badgeW - RowGap() - (showSize ? sizeW + RowGap() : 0);
        nameW = qMax(UiStyle::Fpx(60), nameW);

        const QFont nameFont = UiFont(13);
        const QFont idFont = MonoFont(10);
        const QFontMetrics nfm(nameFont);
        const QFontMetrics ifm(idFont);
        const int y = r.top() + RowPadY();

        const QString name = index.data(NameRole).toString();
        p->setFont(nameFont);
        QColor nameColor = T().text;
        if (current)
            nameColor = T().accent;
        else if (!installed)
            nameColor = T().textFaint;
        p->setPen(nameColor);
        p->drawText(QRect(r.left() + RowPadX(), y, nameW, nfm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                    nfm.elidedText(name, Qt::ElideRight, nameW));

        const QString file = index.data(FileRole).toString();
        p->setFont(idFont);
        p->setPen(installed ? T().textFaint : T().border.lighter(150));
        const int idTop = y + nfm.height() + UiStyle::Fpx(2);
        p->drawText(QRect(r.left() + RowPadX(), idTop, nameW, ifm.height()),
                    Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                    ifm.elidedText(file, Qt::ElideRight, nameW));

        const int rightEdge = r.right() - RowPadX();
        if (showSize) {
            p->setFont(MonoFont(11));
            p->setPen(installed ? T().textDim : T().textFaint);
            p->drawText(QRect(rightEdge - badgeW - RowGap() - sizeW, r.top(), sizeW, r.height()),
                        Qt::AlignRight | Qt::AlignVCenter,
                        index.data(SizeRole).toString());
        }

        // 右列角标：当前使用 / 已选 / 未安装 / —（空位用破折号占位，行不会跳）
        QString badge;
        QColor fg = T().textFaint;
        QColor bg = Qt::transparent;
        QColor border = T().border;
        if (current) {
            badge = QStringLiteral("当前使用");
            fg = T().bg;
            bg = T().accent;
            border = T().accent;
        } else if (chosen) {
            badge = QStringLiteral("已选");
            fg = T().text;
            border = T().textFaint;
        } else if (!installed) {
            badge = QStringLiteral("未安装");
            fg = T().textFaint;
            border = T().border;
        } else {
            badge = QStringLiteral("—");
        }
        const int badgeH = UiStyle::Fpx(19);
        const QRect badgeRect(rightEdge - badgeW, r.center().y() - badgeH / 2, badgeW, badgeH);
        if (badge != QStringLiteral("—")) {
            p->setPen(QPen(border, 1));
            p->setBrush(bg);
            p->drawRect(badgeRect.adjusted(0, 0, -1, -1));
            p->setBrush(Qt::NoBrush);
            p->setFont(UiFont(10));
            p->setPen(fg);
            p->drawText(badgeRect, Qt::AlignCenter, badge);
        } else {
            p->setPen(T().textFaint);
            p->setFont(UiFont(10));
            p->drawText(badgeRect, Qt::AlignCenter, badge);
        }

        if (view_) {
            const QFontMetrics fm(MonoFont(10));
            const int top = idTop + ifm.height() + UiStyle::Fpx(3);
            PaintTiming(p, QRect(r.left() + RowPadX(), top, r.width() - RowPadX() * 2,
                                 r.bottom() - top),
                        *view_, name);
        }

        p->restore();
    }

private:
    const BenchView *view_ = nullptr;
};

// ---------------------------------------------------------------- 吸顶分组条
// 用真正的子控件而不是在 paintEvent 里补画：滚动时视口可能只重绘露出的一条，
// 补画出的分组条会留下残影/半截；子控件位置由滚动值驱动，永远整块。
class SrStickyBand : public QWidget
{
public:
    explicit SrStickyBand(QWidget *parent) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents, true);
        setObjectName(QStringLiteral("SrStickyBand"));
    }

    void SetGroup(const QString &arch, int count)
    {
        arch_ = arch;
        count_ = count;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        PaintGroupBand(&p, rect(), arch_, count_);
    }

private:
    QString arch_;
    int count_ = 0;
};

}  // namespace

// ============================================================ 浮层
class SrModelSheetWidget : public QWidget
{
public:
    SrModelSheetWidget(QWidget *host, const QString &current, bool blocking, bool animate = true)
        : QWidget(host), host_(host), current_(current), animate_(animate)
    {
        setAttribute(Qt::WA_StyledBackground, false);
        // 全屏遮罩浮层：吸顶分组条、悬浮「继续测试」条、列表行都参与
        // Touch::TapGuard 的统一滑动/点击仲裁（浮层不是顶层窗口，靠属性识别）
        setProperty("tapGuardOverlay", true);
        setGeometry(host->rect());
        setFocusPolicy(Qt::StrongFocus);
        host->installEventFilter(this);
        if (blocking)
            loop_ = new QEventLoop(this);
        BuildUi();
        if (!current_.isEmpty()) {
            for (const SrEngine::ModelInfo &m : SrEngine::Catalog()) {
                if (m.name == current_ && m.installed) {
                    chosen_ = current_;
                    break;
                }
            }
        }
        Rebuild();
        SrBench::EnsureLoaded();
        UpdateBenchLegend();
        UpdateBenchUi();
        poll_ = new QTimer(this);
        poll_->setInterval(160);
        connect(poll_, &QTimer::timeout, this, [this] { UpdateBenchUi(); });
        poll_->start();
    }

    QString Chosen() const { return accepted_ ? chosen_ : QString(); }

    void Exec()
    {
        if (loop_)
            loop_->exec();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0, int(130 * scrim_)));
    }

    void keyPressEvent(QKeyEvent *e) override
    {
        if (e->key() == Qt::Key_Escape) {
            Finish(false);
            return;
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            // 没有可确认的选择时不吃回车：否则一按就把浮层关掉，像"点了却没生效"
            if (okBtn_ && okBtn_->isEnabled())
                Finish(true);
            return;
        }
        QWidget::keyPressEvent(e);
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        // 点遮罩 = 取消（与确认框一致；浮层本身不吞掉列表以外的点击）
        if (card_ && !card_->geometry().contains(e->position().toPoint()))
            Finish(false);
        QWidget::mousePressEvent(e);
    }

    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        if (obj == host_ && ev->type() == QEvent::Resize) {
            setGeometry(host_->rect());
            LayoutCard();
            UpdateSticky();
        }
        return QWidget::eventFilter(obj, ev);
    }

    void showEvent(QShowEvent *e) override
    {
        QWidget::showEvent(e);
        LayoutCard();
        // 列表的实际行高/可视区要等第一次 polish 之后才成立，滚动定位放到下一轮
        // 事件循环：否则 scrollToItem 用的是"还没布局"的空矩形，等于没滚
        QTimer::singleShot(0, this, [this] { ScrollToChosen(); });
        if (intro_)
            return;
        intro_ = true;
        // 截图钩子（Show）直接落最终态：进场动画只播 180ms，但截图时点由
        // --shot-delay 决定，抢在动画中途就会拍出半透明的浮层（实测过）。
        if (!animate_ || !Motion::Enabled()) {
            scrim_ = 1.0;
            return;
        }
        const QPoint home = card_->pos();
        Motion::Animate(this, QStringLiteral("scrim"), 0.0, 1.0, Motion::Base, Motion::EnterCurve(),
                        [this](qreal v) {
                            scrim_ = v;
                            update();
                        });
        card_->move(home + QPoint(0, UiStyle::Fpx(16)));
        Motion::SlideTo(card_, home, Motion::Base, Motion::EnterCurve());
        Motion::FadeIn(card_, Motion::Base);
    }

private:
    QPushButton *MakeBenchButton(const QString &text, const char *role, QWidget *parent)
    {
        auto *b = new QPushButton(text, parent);
        b->setObjectName(QStringLiteral("SrBenchButton"));
        b->setProperty("benchBtn", true);
        if (role && *role)
            b->setProperty(role, true);
        b->setCursor(Qt::PointingHandCursor);
        b->setProperty("hitGrow", false);
        b->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        return b;
    }

    QStringList InstalledNames() const
    {
        QStringList out;
        for (const SrEngine::ModelInfo &m : SrEngine::InstalledModels())
            out << m.name;
        return out;
    }

    QStringList NamesOfFilter() const
    {
        QStringList out;
        for (const SrEngine::ModelInfo &m : SrEngine::InstalledModels()) {
            if (filter_.isEmpty() || m.arch == filter_)
                out << m.name;
        }
        return out;
    }

    void StartBench(const QStringList &models)
    {
        if (models.isEmpty())
            return;
        SrBench::Start(models);
        UpdateBenchUi();
    }

    void UpdateBenchLegend()
    {
        if (!legend_)
            return;
        QStringList tags;
        QStringList missing;
        // 多于一块 GPU 时必须点名（GPU0/GPU1 各自是什么卡），否则两列耗时对不上号；
        // 单卡时只写 GPU0 保持紧凑。硬件指纹用的是 label，这里只改显示。
        int gpuCount = 0;
        for (const SrEngine::BenchHw &h : SrBench::Hardware()) {
            if (h.id.startsWith(QLatin1String("gpu")))
                ++gpuCount;
        }
        for (const SrEngine::BenchHw &h : SrBench::Hardware()) {
            if (!h.available) {
                missing << QStringLiteral("%1 不可用：%2").arg(h.shortLabel, h.note);
                continue;
            }
            if (gpuCount > 1 && h.id.startsWith(QLatin1String("gpu"))) {
                tags << QString(h.label).replace(QStringLiteral(" · "), QStringLiteral(" "));
            } else {
                tags << h.shortLabel;
            }
        }
        QString text = QStringLiteral("测试硬件 %1 · 192×192 单块")
                           .arg(tags.join(QStringLiteral(" · ")));
        if (!missing.isEmpty())
            text += QStringLiteral("（%1）").arg(missing.join(QStringLiteral("；")));
        legend_->setText(text);
    }

    void RelayoutResume()
    {
        if (!resume_ || !list_)
            return;
        const int w = qMax(0, list_->viewport()->width() - UiStyle::Fpx(18));
        resume_->setGeometry(UiStyle::Fpx(9), UiStyle::Fpx(7), w,
                             qMax(UiStyle::Fpx(34), resume_->sizeHint().height()));
        resume_->raise();
    }

    void UpdateBenchUi()
    {
        const SrBench::Progress snap = SrBench::Snapshot();
        if (benchAll_) {
            const bool run = snap.running;
            benchAll_->setEnabled(!run);
            benchSel_->setEnabled(!run && !chosen_.isEmpty());
            benchGroup_->setEnabled(!run && !filter_.isEmpty());
            benchStop_->setVisible(run);
            benchStop_->setEnabled(run && !snap.cancelling);
        }
        if (benchBar_) {
            benchBar_->setVisible(snap.running);
            benchBar_->SetFraction(snap.total > 0 ? double(snap.done) / snap.total : 0.0);
        }
        if (benchStat_) {
            QString text;
            if (snap.running) {
                text = QStringLiteral("测试中 %1/%2")
                           .arg(qMin(snap.done + 1, snap.total))
                           .arg(snap.total);
                if (!snap.activeHw.isEmpty())
                    text += QStringLiteral(" · %1").arg(SrBench::HardwareShort(snap.activeHw));
            } else if (!snap.pending.isEmpty()) {
                text = snap.lastSummary.isEmpty()
                           ? QStringLiteral("已取消 · 完成 %1/%2").arg(snap.done).arg(snap.total)
                           : snap.lastSummary;
            } else if (!snap.lastSummary.isEmpty()) {
                text = snap.lastSummary;
            } else {
                QStringList tags;
                for (const SrEngine::BenchHw &h : SrBench::Hardware()) {
                    if (h.available)
                        tags << h.shortLabel;
                }
                text = QStringLiteral("将测试 %1").arg(tags.join(QStringLiteral(" · ")));
            }
            benchStat_->setText(text);
            const bool hot = snap.running;
            if (benchStat_->property("run").toBool() != hot) {
                benchStat_->setProperty("run", hot);
                benchStat_->style()->unpolish(benchStat_);
                benchStat_->style()->polish(benchStat_);
            }
        }
        if (resume_) {
            const bool show = !snap.running && !snap.pending.isEmpty();
            if (show && resumeText_)
                resumeText_->setText(QStringLiteral("上次测试未完成 %1/%2")
                                         .arg(snap.done)
                                         .arg(snap.total));
            resume_->setVisible(show);
            if (show)
                RelayoutResume();
        }
        if (snap.version != benchVersion_) {
            benchVersion_ = snap.version;
            benchView_.results = SrBench::All();
            benchView_.activeModel = snap.activeModel;
            benchView_.activeHw = snap.activeHw;
            benchView_.running = snap.running;
            activeRow_ = snap.activeModel;
            Rebuild(true);
        }
    }

    void BuildUi()
    {
        card_ = new QWidget(this);
        card_->setObjectName(QStringLiteral("SrSheetCard"));
        auto *lay = new QVBoxLayout(card_);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(0);

        // ---- 抓手（仅手机形态）----
        grip_ = new QWidget(card_);
        grip_->setObjectName(QStringLiteral("SrSheetGrip"));
        grip_->setFixedHeight(UiStyle::Fpx(14));
        auto *gl = new QHBoxLayout(grip_);
        gl->setContentsMargins(0, UiStyle::Fpx(7), 0, 0);
        auto *gripBar = new QWidget(grip_);
        gripBar->setObjectName(QStringLiteral("SrSheetGripBar"));
        gripBar->setFixedSize(UiStyle::Fpx(36), UiStyle::Fpx(4));
        gl->addWidget(gripBar, 0, Qt::AlignHCenter);
        lay->addWidget(grip_);

        // ---- 标题 ----
        auto *head = new QWidget(card_);
        auto *hl = new QHBoxLayout(head);
        hl->setContentsMargins(UiStyle::Fpx(13), UiStyle::Fpx(8), UiStyle::Fpx(13),
                               UiStyle::Fpx(6));
        hl->setSpacing(UiStyle::Fpx(8));
        auto *texts = new QVBoxLayout();
        texts->setSpacing(UiStyle::Fpx(3));
        auto *title = new QLabel(QStringLiteral("选择超分模型"), head);
        title->setObjectName(QStringLiteral("SrSheetTitle"));
        texts->addWidget(title);
        auto *sub = new QLabel(QStringLiteral("按算法分组 · 分组条吸顶 · 当前使用高亮"),
                               head);
        sub->setObjectName(QStringLiteral("SrSheetSub"));
        sub->setWordWrap(true);
        sub->setMinimumWidth(0);
        texts->addWidget(sub);
        hl->addLayout(texts, 1);
        auto *close = new QPushButton(QStringLiteral("×"), head);
        close->setObjectName(QStringLiteral("SrSheetClose"));
        close->setCursor(Qt::PointingHandCursor);
        close->setProperty("hitGrow", false);
        close->setFixedSize(UiStyle::Fpx(26), UiStyle::Fpx(26));
        connect(close, &QPushButton::clicked, this, [this] { Finish(false); });
        hl->addWidget(close, 0, Qt::AlignTop);
        lay->addWidget(head);

        // ---- 算法筛选（计数跟着 chip 走，和 demo 一致）----
        const QVector<SrEngine::ModelInfo> all = SrEngine::Catalog();
        QHash<QString, int> counts;
        QStringList order;
        for (const SrEngine::ModelInfo &m : all) {
            if (!counts.contains(m.arch))
                order << m.arch;
            counts[m.arch] += 1;
        }
        auto *chipsHost = new QWidget(card_);
        auto *cl = new FlowLayout(chipsHost, UiStyle::Fpx(6));
        cl->setContentsMargins(UiStyle::Fpx(13), UiStyle::Fpx(4), UiStyle::Fpx(13),
                               UiStyle::Fpx(2));
        struct ChipDef {
            QString key;
            QString label;
            int count;
        };
        QVector<ChipDef> defs;
        defs.push_back({QString(), QStringLiteral("全部"), int(all.size())});
        for (const QString &a : order)
            defs.push_back({a, ArchChipLabel(a), counts.value(a)});
        for (const ChipDef &d : defs) {
            auto *chip = new Chip(QStringLiteral("%1 %2").arg(d.label).arg(d.count), true,
                                  chipsHost);
            chip->setProperty("hitGrow", false);
            chip->setChecked(d.key.isEmpty());
            cl->addWidget(chip);
            chips_.push_back(chip);
            const QString key = d.key;
            // 不要按值捕获 chips_：这时它还在填，后面几个 chip 会漏掉，点了不生效
            connect(chip, &QPushButton::clicked, this, [this, key, chip] {
                for (auto *c : chips_)
                    c->setChecked(c == chip);
                filter_ = key;
                Rebuild();
                // 「仅测试当前分组」的可用性跟着筛选走：不立刻刷新的话，
                // 点完 chip 马上点按钮会撞上还没解禁的旧状态（按钮是灰的，点了没反应）
                UpdateBenchUi();
            });
        }
        lay->addWidget(chipsHost);

        // ---- 测速工具条（demo v2：第 1 行按钮，第 2 行状态 + 细进度条）----
        auto *tools = new QWidget(card_);
        toolsGrid_ = new QGridLayout(tools);
        toolsGrid_->setContentsMargins(UiStyle::Fpx(13), UiStyle::Fpx(9), UiStyle::Fpx(13), 0);
        toolsGrid_->setHorizontalSpacing(UiStyle::Fpx(8));
        toolsGrid_->setVerticalSpacing(UiStyle::Fpx(6));
        toolsGrid_->setColumnStretch(3, 1);
        benchAll_ = MakeBenchButton(QStringLiteral("一键测试全部"), "benchPrimary", tools);
        benchSel_ = MakeBenchButton(QStringLiteral("测试选中模型"), nullptr, tools);
        benchGroup_ = MakeBenchButton(QStringLiteral("仅测试当前分组"), nullptr, tools);
        benchStop_ = MakeBenchButton(QStringLiteral("停止"), "benchStop", tools);
        benchAll_->setObjectName(QStringLiteral("SrBenchAll"));
        benchSel_->setObjectName(QStringLiteral("SrBenchSel"));
        benchGroup_->setObjectName(QStringLiteral("SrBenchGroup"));
        benchStop_->setObjectName(QStringLiteral("SrBenchStop"));
        toolsGrid_->addWidget(benchAll_, 0, 0);
        toolsGrid_->addWidget(benchSel_, 0, 1);
        toolsGrid_->addWidget(benchGroup_, 0, 2);
        toolsGrid_->addWidget(benchStop_, 0, 4, Qt::AlignRight | Qt::AlignVCenter);
        benchStop_->hide();
        toolsWide_ = true;
        lay->addWidget(tools);

        auto *statRow = new QWidget(card_);
        auto *sl = new QHBoxLayout(statRow);
        sl->setContentsMargins(UiStyle::Fpx(13), UiStyle::Fpx(7), UiStyle::Fpx(13), 0);
        sl->setSpacing(UiStyle::Fpx(8));
        benchStat_ = new QLabel(statRow);
        benchStat_->setObjectName(QStringLiteral("SrBenchStat"));
        benchStat_->setMinimumWidth(0);
        sl->addWidget(benchStat_, 0);
        benchBar_ = new BenchBar(statRow);
        benchBar_->setFixedHeight(UiStyle::Fpx(2));
        benchBar_->hide();
        sl->addWidget(benchBar_, 1);
        lay->addWidget(statRow);

        legend_ = new QLabel(card_);
        legend_->setObjectName(QStringLiteral("SrBenchLegend"));
        legend_->setWordWrap(true);
        legend_->setMinimumWidth(0);
        legend_->setContentsMargins(UiStyle::Fpx(13), UiStyle::Fpx(6), UiStyle::Fpx(13),
                                    UiStyle::Fpx(7));
        lay->addWidget(legend_);

        connect(benchAll_, &QPushButton::clicked, this, [this] {
            if (SrBench::Snapshot().running)
                return;
            // “全部”按字面意思跑全部模型：有筛选时先切回全部，否则看起来像没反应
            if (!filter_.isEmpty()) {
                filter_.clear();
                for (auto *c : chips_)
                    c->setChecked(c->text().startsWith(QStringLiteral("全部")));
                Rebuild();
            }
            StartBench(InstalledNames());
        });
        connect(benchSel_, &QPushButton::clicked, this, [this] {
            if (!chosen_.isEmpty())
                StartBench(QStringList{chosen_});
        });
        connect(benchGroup_, &QPushButton::clicked, this, [this] {
            if (!filter_.isEmpty())
                StartBench(NamesOfFilter());
        });
        connect(benchStop_, &QPushButton::clicked, this, [this] { SrBench::Cancel(); });

        // ---- 列头 ----
        auto *colHead = new QWidget(card_);
        colHead->setObjectName(QStringLiteral("SrSheetColHead"));
        colHead->setFixedHeight(UiStyle::Fpx(26));
        auto *chol = new QHBoxLayout(colHead);
        chol->setContentsMargins(RowPadX(), 0, RowPadX(), 0);
        chol->setSpacing(RowGap());
        auto *colTitle = new QLabel(QStringLiteral("模型 · 完整 ID"), colHead);
        colTitle->setObjectName(QStringLiteral("SrSheetCol"));
        chol->addWidget(colTitle, 0);
        // 计数跟在列头后面（demo 的 "41 / 41"），不单独占一行：320dp 下再起一行会把
        // 列表挤到屏幕外
        count_ = new QLabel(QString(), colHead);
        count_->setObjectName(QStringLiteral("SrSheetColCount"));
        chol->addWidget(count_, 0);
        chol->addStretch(1);
        sizeHead_ = new QLabel(QStringLiteral("体积"), colHead);
        sizeHead_->setObjectName(QStringLiteral("SrSheetCol"));
        sizeHead_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sizeHead_->setFixedWidth(UiStyle::Fpx(54));
        chol->addWidget(sizeHead_);
        auto *stateHead = new QLabel(QStringLiteral("状态"), colHead);
        stateHead->setObjectName(QStringLiteral("SrSheetCol"));
        stateHead->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        stateHead->setFixedWidth(BadgeWidth());
        chol->addWidget(stateHead);
        lay->addWidget(colHead);

        // ---- 列表 ----
        list_ = new QListWidget(card_);
        list_->setObjectName(QStringLiteral("SrModelList"));
        list_->setFrameShape(QFrame::NoFrame);
        list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        list_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
        list_->setSelectionMode(QAbstractItemView::NoSelection);
        list_->setFocusPolicy(Qt::NoFocus);
        list_->setUniformItemSizes(false);
        list_->setMouseTracking(true);
        list_->setItemDelegate(new SrModelDelegate(&benchView_, list_));
        lay->addWidget(list_, 1);

        sticky_ = new SrStickyBand(list_->viewport());
        sticky_->hide();
        connect(list_->verticalScrollBar(), &QScrollBar::valueChanged, this,
                [this] { UpdateSticky(); });
        connect(list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
            if (!item || item->data(GroupRole).toBool() || !item->data(InstalledRole).toBool())
                return;
            chosen_ = item->data(NameRole).toString();
            MarkChosen();
        });
        connect(list_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
            if (!item || item->data(GroupRole).toBool() || !item->data(InstalledRole).toBool())
                return;
            chosen_ = item->data(NameRole).toString();
            Finish(true);
        });

        // ---- 取消后的"继续测试"悬浮条（浮在列表顶部）----
        resume_ = new QWidget(list_->viewport());
        resume_->setObjectName(QStringLiteral("SrBenchResume"));
        resume_->setAttribute(Qt::WA_StyledBackground, true);
        auto *rl = new QHBoxLayout(resume_);
        rl->setContentsMargins(UiStyle::Fpx(9), UiStyle::Fpx(7), UiStyle::Fpx(9), UiStyle::Fpx(7));
        rl->setSpacing(UiStyle::Fpx(8));
        resumeText_ = new QLabel(resume_);
        resumeText_->setObjectName(QStringLiteral("SrBenchResumeText"));
        resumeText_->setMinimumWidth(0);
        rl->addWidget(resumeText_, 1);
        auto *resumeGo = MakeBenchButton(QStringLiteral("继续测试"), "benchPrimary", resume_);
        auto *resumeNo = MakeBenchButton(QStringLiteral("忽略"), nullptr, resume_);
        resumeGo->setObjectName(QStringLiteral("SrBenchResumeGo"));
        resumeNo->setObjectName(QStringLiteral("SrBenchResumeNo"));
        rl->addWidget(resumeGo);
        rl->addWidget(resumeNo);
        connect(resumeGo, &QPushButton::clicked, this, [this] {
            SrBench::ResumePending();
            if (resume_)
                resume_->hide();
            UpdateBenchUi();
        });
        connect(resumeNo, &QPushButton::clicked, this, [this] {
            SrBench::DismissPending();
            if (resume_)
                resume_->hide();
            UpdateBenchUi();
        });
        resume_->hide();

        // ---- 底栏 ----
        auto *foot = new QWidget(card_);
        foot->setObjectName(QStringLiteral("SrSheetFoot"));
        auto *fl = new QVBoxLayout(foot);
        fl->setContentsMargins(UiStyle::Fpx(13), UiStyle::Fpx(8), UiStyle::Fpx(13),
                               UiStyle::Fpx(10));
        fl->setSpacing(UiStyle::Fpx(8));
        auto *selRow = new QHBoxLayout();
        selRow->setSpacing(UiStyle::Fpx(6));
        auto *selLabel = new QLabel(QStringLiteral("已选"), foot);
        selLabel->setObjectName(QStringLiteral("SrSheetSelKey"));
        selRow->addWidget(selLabel, 0);
        chosenText_ = new ElideLabel(QStringLiteral("—"), foot);
        chosenText_->setObjectName(QStringLiteral("SrSheetSelVal"));
        chosenText_->setFont(MonoFont(10));
        selRow->addWidget(chosenText_, 1);
        fl->addLayout(selRow);

        auto *btns = new QHBoxLayout();
        btns->setSpacing(UiStyle::Fpx(8));
        auto *cancel = new QPushButton(QStringLiteral("关闭"), foot);
        cancel->setObjectName(QStringLiteral("Ghost"));
        UiStyle::MarkSoftButton(cancel);
        cancel->setCursor(Qt::PointingHandCursor);
        cancel->setProperty("hitGrow", false);
        connect(cancel, &QPushButton::clicked, this, [this] { Finish(false); });
        btns->addWidget(cancel, 1);
        okBtn_ = new QPushButton(QStringLiteral("确定"), foot);
        okBtn_->setObjectName(QStringLiteral("Primary"));
        UiStyle::MarkSoftButton(okBtn_);
        okBtn_->setCursor(Qt::PointingHandCursor);
        okBtn_->setProperty("hitGrow", false);
        connect(okBtn_, &QPushButton::clicked, this, [this] { Finish(true); });
        btns->addWidget(okBtn_, 1);
        fl->addLayout(btns);
        lay->addWidget(foot);

        // 触摸：列表自己也要能被手指拖动（InstallScrollGestures 只扫已存在的子树，
        // 浮层是点开时才建的，必须在这里补装一次）
        Touch::InstallScrollGestures(list_);

        MarkChosen();
    }

    void Rebuild(bool keepScroll = false)
    {
        if (!list_)
            return;
        const int keepValue = keepScroll ? list_->verticalScrollBar()->value() : 0;
        QVector<SrEngine::ModelInfo> all = SrEngine::Catalog();
        models_ = all;

        list_->clear();
        QString lastArch;
        QVector<SrEngine::ModelInfo> group;
        int shown = 0;

        auto flushGroup = [&] {
            if (group.isEmpty())
                return;
            auto *head = new QListWidgetItem(list_);
            head->setFlags(Qt::ItemIsEnabled);
            head->setData(GroupRole, true);
            head->setData(ArchRole, lastArch);
            head->setData(CountRole, int(group.size()));
            for (const SrEngine::ModelInfo &m : group) {
                auto *it = new QListWidgetItem(list_);
                it->setFlags(m.installed ? Qt::ItemIsEnabled : Qt::NoItemFlags);
                it->setData(GroupRole, false);
                it->setData(ArchRole, m.arch);
                it->setData(NameRole, m.name);
                it->setData(FileRole, m.file);
                it->setData(SizeRole, SizeText(m.bytes));
                it->setData(InstalledRole, m.installed);
                it->setData(CurrentRole, m.name == current_);
                it->setData(ChosenRole, m.name == chosen_);
                ++shown;
            }
            group.clear();
        };

        for (const SrEngine::ModelInfo &m : all) {
            if (!filter_.isEmpty() && m.arch != filter_)
                continue;
            if (m.arch != lastArch) {
                flushGroup();
                lastArch = m.arch;
            }
            group.push_back(m);
        }
        flushGroup();

        if (count_) {
            count_->setText(QStringLiteral("%1 / %2").arg(shown).arg(all.size()));
        }
        MarkChosen();
        // 首次构建把滚动归零；测速刷新时保留原位置，否则列表会每次跳回顶部
        list_->verticalScrollBar()->setValue(keepScroll ? keepValue : 0);
        if (keepScroll) {
            for (int i = 0; i < list_->count(); ++i) {
                if (list_->item(i)->data(NameRole).toString() == activeRow_) {
                    list_->scrollToItem(list_->item(i), QAbstractItemView::EnsureVisible);
                    break;
                }
            }
        }
        UpdateSticky();
    }

    void MarkChosen()
    {
        if (!list_)
            return;
        for (int i = 0; i < list_->count(); ++i) {
            QListWidgetItem *it = list_->item(i);
            if (it->data(GroupRole).toBool())
                continue;
            it->setData(ChosenRole, it->data(NameRole).toString() == chosen_);
        }
        list_->viewport()->update();

        if (chosenText_) {
            const SrEngine::ModelInfo *m = FindModel(models_, chosen_);
            chosenText_->SetFullText(chosen_.isEmpty()
                                         ? QStringLiteral("—")
                                         : (m ? QStringLiteral("%1 · %2").arg(m->name, m->file)
                                              : chosen_));
        }
        if (okBtn_) {
            const SrEngine::ModelInfo *m = FindModel(models_, chosen_);
            okBtn_->setEnabled(!chosen_.isEmpty() && m && m->installed);
        }
        UpdateSticky();
    }

    void UpdateSticky()
    {
        if (!list_ || !sticky_)
            return;
        int cur = -1;
        for (int i = 0; i < list_->count(); ++i) {
            QListWidgetItem *it = list_->item(i);
            if (!it->data(GroupRole).toBool())
                continue;
            if (list_->visualItemRect(it).top() <= 0)
                cur = i;
            else
                break;
        }
        if (cur < 0) {
            sticky_->hide();
            return;
        }
        const QRect curRect = list_->visualItemRect(list_->item(cur));
        if (curRect.top() >= 0) {
            sticky_->hide();   // 分组条本身就在顶部，已经由 delegate 画好
            return;
        }
        int top = 0;
        for (int i = cur + 1; i < list_->count(); ++i) {
            QListWidgetItem *it = list_->item(i);
            if (!it->data(GroupRole).toBool())
                continue;
            const int nt = list_->visualItemRect(it).top();
            if (nt < GroupRowHeight())
                top = nt - GroupRowHeight();   // 下一组把自己的标题顶上去（iOS 式）
            break;
        }
        sticky_->SetGroup(list_->item(cur)->data(ArchRole).toString(),
                          list_->item(cur)->data(CountRole).toInt());
        sticky_->setGeometry(0, top, list_->viewport()->width(), GroupRowHeight());
        sticky_->show();
        sticky_->raise();
    }

    void ScrollToChosen()
    {
        if (!list_)
            return;
        for (int i = 0; i < list_->count(); ++i) {
            QListWidgetItem *it = list_->item(i);
            if (it->data(GroupRole).toBool())
                continue;
            if (it->data(NameRole).toString() != chosen_)
                continue;
            list_->scrollToItem(it, QAbstractItemView::PositionAtCenter);
            break;
        }
        UpdateSticky();
    }

    // 宽屏：停止与三个按钮同一行、靠右（demo 的排法）；
    // 窄屏：三个按钮一行已经占满，停止落到第二行右侧，绝不挤压/裁切按钮。
    void RelayoutToolButtons()
    {
        if (!toolsGrid_ || !card_)
            return;
        const bool wide = card_->width() >= UiStyle::Fpx(420);
        if (wide == toolsWide_)
            return;
        toolsWide_ = wide;
        toolsGrid_->removeWidget(benchStop_);
        if (wide)
            toolsGrid_->addWidget(benchStop_, 0, 4, Qt::AlignRight | Qt::AlignVCenter);
        else
            toolsGrid_->addWidget(benchStop_, 1, 0, 1, 5, Qt::AlignRight | Qt::AlignVCenter);
    }

    void LayoutCard()
    {
        if (!card_)
            return;
        const bool sheetMode = Touch::IsTouchMode() || width() < UiStyle::Fpx(620);
        const int margin = UiStyle::Fpx(8);
        if (sheetMode) {
            // 手机：贴底浮层，留出顶部一小条遮罩，明确"这是盖在页面上的"
            const int h = qBound(UiStyle::Fpx(360), int(height() * 0.92), UiStyle::Fpx(760));
            card_->setGeometry(0, height() - h, width(), h);
        } else {
            const int w = qMin(width() - 2 * margin, UiStyle::Fpx(560));
            const int h = qMax(UiStyle::Fpx(320),
                               qMin(height() - 2 * margin, UiStyle::Fpx(680)));
            card_->setGeometry((width() - w) / 2, (height() - h) / 2, w, h);
        }
        if (grip_)
            grip_->setVisible(sheetMode);
        RelayoutToolButtons();
        // 体积列头必须跟 delegate 用同一口径（列宽按列表视口算，不是卡片宽度），
        // 否则窄屏会出现"列头有体积、行里没有"的错位
        if (sizeHead_) {
            const int vw = list_ && list_->viewport()->width() > 0 ? list_->viewport()->width()
                                                                    : card_->width() - 4;
            sizeHead_->setVisible(vw >= UiStyle::Fpx(340));
        }
    }

    void Finish(bool ok)
    {
        if (finished_)
            return;
        finished_ = true;
        // 关闭浮层 = 取消测试：已完成的结果留在 SrBench 里，下次打开提示继续
        if (SrBench::Snapshot().running)
            SrBench::Cancel();
        const SrEngine::ModelInfo *m = FindModel(models_, chosen_);
        accepted_ = ok && !chosen_.isEmpty() && m && m->installed;
        if (!Motion::Enabled()) {
            if (loop_ && loop_->isRunning())
                loop_->quit();
            hide();
            return;
        }
        Motion::Animate(this, QStringLiteral("scrim"), scrim_, 0.0, Motion::Fast,
                        Motion::ExitCurve(), [this](qreal v) {
                            scrim_ = v;
                            update();
                        });
        Motion::FadeOut(card_, Motion::Fast);
        Motion::SlideTo(card_, card_->pos() + QPoint(0, UiStyle::Fpx(8)), Motion::Fast,
                        Motion::ExitCurve());
        if (loop_ && loop_->isRunning())
            QTimer::singleShot(Motion::Fast + 10, loop_, [this] {
                if (loop_ && loop_->isRunning())
                    loop_->quit();
            });
        QTimer::singleShot(Motion::Fast + 20, this, [this] { hide(); });
    }

    QWidget *host_ = nullptr;
    QWidget *card_ = nullptr;
    QWidget *grip_ = nullptr;
    QListWidget *list_ = nullptr;
    SrStickyBand *sticky_ = nullptr;
    QLabel *sizeHead_ = nullptr;
    QLabel *count_ = nullptr;
    QPushButton *benchAll_ = nullptr;
    QPushButton *benchSel_ = nullptr;
    QPushButton *benchGroup_ = nullptr;
    QPushButton *benchStop_ = nullptr;
    class QGridLayout *toolsGrid_ = nullptr;
    bool toolsWide_ = true;
    QLabel *benchStat_ = nullptr;
    BenchBar *benchBar_ = nullptr;
    QLabel *legend_ = nullptr;
    QWidget *resume_ = nullptr;
    QLabel *resumeText_ = nullptr;
    BenchView benchView_;
    quint64 benchVersion_ = 0;
    QString activeRow_;
    ElideLabel *chosenText_ = nullptr;
    QPushButton *okBtn_ = nullptr;
    QVector<Chip *> chips_;
    QVector<SrEngine::ModelInfo> models_;
    QEventLoop *loop_ = nullptr;
    QTimer *poll_ = nullptr;
    QString current_;
    QString chosen_;
    QString filter_;
    bool animate_ = true;
    bool accepted_ = false;
    bool intro_ = false;
    bool finished_ = false;
    qreal scrim_ = 0.0;
};

namespace SrModelSheet {

QString Pick(QWidget *host, const QString &current)
{
    if (!host)
        return QString();
    auto *sheet = new SrModelSheetWidget(host->window(), current, true);
    sheet->show();
    sheet->raise();
    sheet->setFocus();
    sheet->Exec();
    const QString out = sheet->Chosen();
    sheet->deleteLater();
    return out;
}

QWidget *Show(QWidget *host, const QString &current)
{
    if (!host)
        return nullptr;
    auto *sheet = new SrModelSheetWidget(host->window(), current, false, /*animate=*/false);
    sheet->show();
    sheet->raise();
    sheet->setFocus();
    return sheet;
}

}  // namespace SrModelSheet

// ============================================================ 整行选择栏
SrModelField::SrModelField(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("SrModelField"));
    setAttribute(Qt::WA_StyledBackground, true);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumWidth(0);

    auto *lay = new QHBoxLayout(this);
    lay->setContentsMargins(UiStyle::Fpx(11), UiStyle::Fpx(6), UiStyle::Fpx(10),
                            UiStyle::Fpx(6));
    lay->setSpacing(UiStyle::Fpx(8));

    auto *texts = new QVBoxLayout();
    texts->setSpacing(UiStyle::Fpx(2));
    texts->setContentsMargins(0, 0, 0, 0);
    main_ = new ElideLabel(QString(), this);
    main_->setObjectName(QStringLiteral("SrFieldMain"));
    main_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    texts->addWidget(main_);
    id_ = new ElideLabel(QString(), this);
    id_->setObjectName(QStringLiteral("SrFieldId"));
    id_->setFont(MonoFont(10));
    id_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    texts->addWidget(id_);
    lay->addLayout(texts, 1);

    go_ = new QLabel(this);
    go_->setAttribute(Qt::WA_TransparentForMouseEvents, true);
    go_->setPixmap(Glyph::Pixmap(Glyph::ChevronRight, UiStyle::Fpx(12), T().textFaint));
    lay->addWidget(go_, 0, Qt::AlignVCenter);

    UpdateTexts();
}

void SrModelField::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint()))
        emit clicked();
    QWidget::mouseReleaseEvent(event);
}

void SrModelField::SetModel(const QString &name)
{
    name_ = name;
    UpdateTexts();
}

void SrModelField::UpdateTexts()
{
    const QVector<SrEngine::ModelInfo> all = SrEngine::Catalog();
    const SrEngine::ModelInfo *m = FindModel(all, name_);
    if (main_) {
        main_->SetFullText(m ? m->name : QStringLiteral("未选择模型"));
        main_->setProperty("installed", m && m->installed);
    }
    if (id_)
        id_->SetFullText(m ? m->file : QStringLiteral("点此选择超分模型"));
    setToolTip(m ? QStringLiteral("%1\n%2\n%3").arg(m->name, m->file, m->note)
                 : QStringLiteral("点此选择超分模型"));
    // 未安装的模型不能跑：选择栏只做提示，真正拦截在 SelectModel/StartBatch
    if (m && !m->installed)
        setToolTip(QStringLiteral("%1\n%2\n未安装：sr_models/ 下缺少 %3")
                       .arg(m->name, m->note, m->file));
}
