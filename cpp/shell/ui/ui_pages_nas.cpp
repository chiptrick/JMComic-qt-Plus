// 网络存储（NAS）节点管理页：列表 + 添加表单 + 校验/打开/删除。
//
// 数据来源只有一个：NasStore（ui.ini 的 [nas] nodes）。页面不做任何"演示节点"，
// 没有配置就是空态；校验结果来自 NasStore::Probe（本地看目录、WebDAV 发 PROPFIND）。
#include "ui_pages.h"

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

#include "theme.h"
#include "touch.h"
#include "ui_kit.h"

namespace {

const UiTokens &T()
{
    return UiTheme::I().Tokens();
}

QWidget *PageBody(QWidget *parent, int margin = 28)
{
    auto *w = new QWidget(parent);
    auto *lay = new QVBoxLayout(w);
    lay->setContentsMargins(margin, margin, margin, margin);
    lay->setSpacing(14);
    return w;
}

QVBoxLayout *BodyLayout(QWidget *body)
{
    return qobject_cast<QVBoxLayout *>(body->layout());
}

QWidget *MakeHeader(const QString &title, const QString &sub, QWidget *parent, QHBoxLayout **out,
                    QLabel **subOut = nullptr)
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
    auto *s = new QLabel(sub, host);
    s->setObjectName(QStringLiteral("PageSub"));
    s->setWordWrap(true);
    s->setMinimumWidth(0);
    texts->addWidget(s);
    if (subOut)
        *subOut = s;
    lay->addLayout(texts);
    lay->addStretch(1);
    if (out)
        *out = lay;
    return host;
}

void ClearLayout(QLayout *l)
{
    if (!l)
        return;
    while (QLayoutItem *it = l->takeAt(0)) {
        if (QWidget *w = it->widget())
            w->deleteLater();
        if (QLayout *child = it->layout()) {
            ClearLayout(child);
            delete child;
        }
        delete it;
    }
}

QLabel *FieldLabel(const QString &text, QWidget *parent)
{
    auto *l = new QLabel(text, parent);
    l->setObjectName(QStringLiteral("SrFieldLabel"));
    l->setMinimumWidth(0);
    return l;
}

QLineEdit *MakeInput(QWidget *parent, const QString &placeholder)
{
    auto *e = new QLineEdit(parent);
    e->setObjectName(QStringLiteral("Input"));
    e->setPlaceholderText(placeholder);
    e->setMinimumWidth(0);
    return e;
}

// 状态徽标：可用=强调色，失败=危险色，未校验=次要文字色
QLabel *StatusBadge(const NasNode &node, QWidget *parent)
{
    auto *badge = new QLabel(node.status.isEmpty() ? QStringLiteral("未校验") : node.status, parent);
    badge->setObjectName(QStringLiteral("SettingDesc"));
    badge->setMinimumWidth(0);
    badge->setWordWrap(true);
    if (node.ok)
        badge->setStyleSheet(QStringLiteral("color:%1;").arg(T().accent.name()));
    else if (node.status.startsWith(QStringLiteral("失败")))
        badge->setStyleSheet(QStringLiteral("color:%1;").arg(T().danger.name()));
    return badge;
}

}  // namespace

NasPage::NasPage(QWidget *parent) : PageBase(parent)
{
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    auto *body = PageBody(this, Touch::IsTouchMode() ? UiStyle::Fpx(12) : 28);
    auto *lay = BodyLayout(body);

    QHBoxLayout *header = nullptr;
    lay->addWidget(MakeHeader(QStringLiteral("网络存储"),
                              QStringLiteral("SMB / WebDAV / 本地目录"),
                              body, &header, &hint_));
    addBtn_ = new QPushButton(QStringLiteral("添加节点"), body);
    addBtn_->setObjectName(QStringLiteral("Primary"));
    addBtn_->setCursor(Qt::PointingHandCursor);
    addBtn_->setProperty("hitGrow", false);
    header->addWidget(addBtn_);
    connect(addBtn_, &QPushButton::clicked, this, [this] { SetFormOpen(!formOpen_); });

    BuildForm(lay, body);

    auto *area = new QScrollArea(body);
    area->setWidgetResizable(true);
    area->setFrameShape(QFrame::NoFrame);
    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    Touch::LockHorizontalPan(area);
    auto *host = new QWidget(area);
    list_ = new QVBoxLayout(host);
    list_->setContentsMargins(0, 0, 0, 0);
    list_->setSpacing(10);
    list_->addStretch(1);
    area->setWidget(host);
    lay->addWidget(area, 1);

    root->addWidget(body);

    Reload();
}

void NasPage::BuildForm(QVBoxLayout *lay, QWidget *body)
{
    form_ = new QWidget(body);
    form_->setObjectName(QStringLiteral("ListRow"));
    auto *fl = new QVBoxLayout(form_);
    fl->setContentsMargins(16, 14, 16, 14);
    fl->setSpacing(8);

    auto *title = new QLabel(QStringLiteral("新增节点"), form_);
    title->setObjectName(QStringLiteral("RowTitle"));
    fl->addWidget(title);

    auto *row1 = new QHBoxLayout();
    row1->setContentsMargins(0, 0, 0, 0);
    row1->setSpacing(8);
    auto *nameCol = new QVBoxLayout();
    nameCol->setSpacing(4);
    nameCol->addWidget(FieldLabel(QStringLiteral("名称"), form_));
    nameEdit_ = MakeInput(form_, QStringLiteral("例如：家庭 NAS"));
    nameCol->addWidget(nameEdit_);
    row1->addLayout(nameCol, 1);

    auto *typeCol = new QVBoxLayout();
    typeCol->setSpacing(4);
    typeCol->addWidget(FieldLabel(QStringLiteral("类型"), form_));
    typeCombo_ = new QComboBox(form_);
    typeCombo_->setObjectName(QStringLiteral("SettingCombo"));
    for (const QString &t : NasStore::Types())
        typeCombo_->addItem(NasStore::TypeLabel(t), t);
    typeCombo_->setMinimumWidth(0);
    typeCol->addWidget(typeCombo_);
    row1->addLayout(typeCol, 1);
    fl->addLayout(row1);

    fl->addWidget(FieldLabel(QStringLiteral("地址（本地路径 / https://host/dav/ / \\\\host\\share）"), form_));
    targetEdit_ = MakeInput(form_, QStringLiteral("D:/Comics 或 https://nas.local/dav/"));
    fl->addWidget(targetEdit_);

    auto *row2 = new QHBoxLayout();
    row2->setContentsMargins(0, 0, 0, 0);
    row2->setSpacing(8);
    auto *userCol = new QVBoxLayout();
    userCol->setSpacing(4);
    userCol->addWidget(FieldLabel(QStringLiteral("用户名（WebDAV）"), form_));
    userEdit_ = MakeInput(form_, QStringLiteral("可选"));
    userCol->addWidget(userEdit_);
    row2->addLayout(userCol, 1);
    auto *passCol = new QVBoxLayout();
    passCol->setSpacing(4);
    passCol->addWidget(FieldLabel(QStringLiteral("密码（明文保存在 ui.ini）"), form_));
    passEdit_ = MakeInput(form_, QStringLiteral("可选"));
    passEdit_->setEchoMode(QLineEdit::Password);
    passCol->addWidget(passEdit_);
    row2->addLayout(passCol, 1);
    fl->addLayout(row2);

    auto *actions = new QHBoxLayout();
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(8);
    auto *save = new QPushButton(QStringLiteral("保存节点"), form_);
    save->setObjectName(QStringLiteral("Primary"));
    UiStyle::MarkSoftButton(save);
    save->setCursor(Qt::PointingHandCursor);
    save->setProperty("hitGrow", false);
    auto *cancel = new QPushButton(QStringLiteral("取消"), form_);
    cancel->setObjectName(QStringLiteral("Ghost"));
    cancel->setCursor(Qt::PointingHandCursor);
    cancel->setProperty("hitGrow", false);
    actions->addWidget(save, 0);
    actions->addWidget(cancel, 0);
    actions->addStretch(1);
    fl->addLayout(actions);
    connect(save, &QPushButton::clicked, this, &NasPage::AddFromForm);
    connect(cancel, &QPushButton::clicked, this, [this] { SetFormOpen(false); });

    form_->setVisible(false);
    lay->addWidget(form_);
}

void NasPage::SetFormOpen(bool open)
{
    formOpen_ = open;
    if (form_)
        form_->setVisible(open);
    if (addBtn_)
        addBtn_->setText(open ? QStringLiteral("收起") : QStringLiteral("添加节点"));
    if (open && nameEdit_)
        nameEdit_->setFocus();
}

void NasPage::OnEnter()
{
    Reload();
}

void NasPage::Reload()
{
    nodes_ = NasStore::Load();
    RebuildRows();
}

QString NasPage::Report() const
{
    int ready = 0;
    for (const NasNode &n : nodes_) {
        if (n.ok)
            ++ready;
    }
    return QStringLiteral("nas nodes=%1 ready=%2 form=%3")
        .arg(nodes_.size())
        .arg(ready)
        .arg(formOpen_ ? 1 : 0);
}

void NasPage::RebuildRows()
{
    if (!list_)
        return;
    // 保留尾部 stretch
    while (list_->count() > 1) {
        QLayoutItem *it = list_->takeAt(0);
        if (QWidget *w = it->widget())
            w->deleteLater();
        if (QLayout *child = it->layout()) {
            ClearLayout(child);
            delete child;
        }
        delete it;
    }
    if (hint_) {
        hint_->setText(nodes_.isEmpty()
                           ? QStringLiteral("SMB / WebDAV / 本地目录")
                           : QStringLiteral("共 %1 个节点 · 校验只检查可达性").arg(nodes_.size()));
    }

    if (nodes_.isEmpty()) {
        auto *empty = new EmptyState(
            QStringLiteral("尚未添加节点"),
            QStringLiteral("添加本地目录、WebDAV 或\nSMB 共享后，可在这里校验并打开。"),
            QString(), list_->parentWidget() ? list_->parentWidget() : this);
        list_->insertWidget(0, empty);
        return;
    }

    for (int i = 0; i < nodes_.size(); ++i) {
        const NasNode n = nodes_.at(i);
        auto *card = new QWidget(list_->parentWidget() ? list_->parentWidget() : this);
        card->setObjectName(QStringLiteral("ListRow"));
        auto *cl = new QVBoxLayout(card);
        cl->setContentsMargins(14, 10, 14, 10);
        cl->setSpacing(6);

        auto *top = new QHBoxLayout();
        top->setContentsMargins(0, 0, 0, 0);
        top->setSpacing(8);
        auto *glyph = new QLabel(NasStore::TypeGlyphHint(n.type), card);
        glyph->setObjectName(QStringLiteral("SrKvKey"));
        top->addWidget(glyph);
        auto *name = new QLabel(n.name, card);
        name->setObjectName(QStringLiteral("RowTitle"));
        name->setMinimumWidth(0);
        top->addWidget(name);
        auto *type = new QLabel(NasStore::TypeLabel(n.type), card);
        type->setObjectName(QStringLiteral("SettingDesc"));
        top->addWidget(type);
        top->addStretch(1);

        auto *probe = new QPushButton(QStringLiteral("校验"), card);
        probe->setObjectName(QStringLiteral("Ghost"));
        probe->setCursor(Qt::PointingHandCursor);
        probe->setProperty("hitGrow", false);
        auto *open = new QPushButton(QStringLiteral("打开"), card);
        open->setObjectName(QStringLiteral("Ghost"));
        open->setCursor(Qt::PointingHandCursor);
        open->setProperty("hitGrow", false);
        auto *del = new QPushButton(QStringLiteral("删除"), card);
        del->setObjectName(QStringLiteral("Ghost"));
        del->setCursor(Qt::PointingHandCursor);
        del->setProperty("hitGrow", false);
        top->addWidget(probe);
        top->addWidget(open);
        top->addWidget(del);
        cl->addLayout(top);

        auto *target = new QLabel(n.target, card);
        target->setObjectName(QStringLiteral("SettingDesc"));
        target->setMinimumWidth(0);
        target->setWordWrap(true);
        cl->addWidget(target);
        cl->addWidget(StatusBadge(n, card));

        const QString id = n.id;
        connect(probe, &QPushButton::clicked, this, [this, id] {
            for (int k = 0; k < nodes_.size(); ++k) {
                if (nodes_.at(k).id == id) {
                    ProbeOne(k);
                    return;
                }
            }
        });
        connect(open, &QPushButton::clicked, this, [this, id] {
            for (const NasNode &node : nodes_) {
                if (node.id != id)
                    continue;
                if (node.type == QLatin1String("webdav")) {
                    emit Toast(QStringLiteral("WebDAV 浏览需要完整客户端，本期只支持校验"));
                    return;
                }
                const QFileInfo fi(QDir::fromNativeSeparators(node.target));
                if (!fi.exists() || !fi.isDir()) {
                    emit Toast(node.type == QLatin1String("smb")
                                   ? QStringLiteral("共享不可见：请先在系统里映射该网络驱动器")
                                   : QStringLiteral("目录不存在：%1").arg(node.target));
                    return;
                }
                emit OpenLocal(fi.absoluteFilePath());
                return;
            }
        });
        connect(del, &QPushButton::clicked, this, [this, id] { RemoveNodeForTest(id); });

        list_->insertWidget(list_->count() - 1, card);
    }
}

void NasPage::Persist()
{
    QString err;
    if (!NasStore::Save(nodes_, &err))
        emit Toast(QStringLiteral("保存失败：%1").arg(err));
}

void NasPage::ProbeOne(int index)
{
    if (index < 0 || index >= nodes_.size() || busy_)
        return;
    busy_ = true;
    NasNode node = nodes_.at(index);
    emit Toast(QStringLiteral("正在校验 %1…").arg(node.name));
    NasStore::Probe(node, [this, index](const NasNode &result) {
        busy_ = false;
        if (index < nodes_.size()) {
            nodes_[index].ok = result.ok;
            nodes_[index].status = result.status;
            nodes_[index].checkedMs = result.checkedMs;
        }
        // 校验结果不落盘（status 属于运行时），只重建行
        RebuildRows();
        emit Toast(QStringLiteral("%1：%2").arg(result.name, result.status));
    });
}

void NasPage::AddFromForm()
{
    if (!nameEdit_ || !targetEdit_ || !typeCombo_)
        return;
    NasNode n;
    n.name = nameEdit_->text();
    n.type = typeCombo_->currentData().toString();
    n.target = targetEdit_->text();
    n.user = userEdit_ ? userEdit_->text().trimmed() : QString();
    n.pass = passEdit_ ? passEdit_->text() : QString();

    QString err;
    if (!NasStore::Validate(&n, &err)) {
        emit Toast(err);
        return;
    }
    for (const NasNode &old : nodes_) {
        if (old.type == n.type && old.target.compare(n.target, Qt::CaseInsensitive) == 0) {
            emit Toast(QStringLiteral("相同地址的节点已存在"));
            return;
        }
    }
    nodes_.append(n);
    Persist();
    RebuildRows();
    SetFormOpen(false);
    if (nameEdit_)
        nameEdit_->clear();
    if (targetEdit_)
        targetEdit_->clear();
    if (userEdit_)
        userEdit_->clear();
    if (passEdit_)
        passEdit_->clear();
    emit Toast(QStringLiteral("已添加：%1").arg(n.name));
    // 添加后立刻探测一次，用户不用再点"校验"
    ProbeOne(nodes_.size() - 1);
}

QString NasPage::AddNodeForTest(const QString &name, const QString &type, const QString &target,
                                const QString &user, const QString &pass, QString *error)
{
    NasNode n;
    n.name = name;
    n.type = type;
    n.target = target;
    n.user = user;
    n.pass = pass;
    QString err;
    if (!NasStore::Validate(&n, &err)) {
        if (error)
            *error = err;
        return QString();
    }
    nodes_.append(n);
    Persist();
    RebuildRows();
    if (error)
        error->clear();
    return n.id;
}

bool NasPage::RemoveNodeForTest(const QString &id)
{
    for (int i = 0; i < nodes_.size(); ++i) {
        if (nodes_.at(i).id != id)
            continue;
        const QString name = nodes_.at(i).name;
        nodes_.removeAt(i);
        Persist();
        RebuildRows();
        emit Toast(QStringLiteral("已删除：%1").arg(name));
        return true;
    }
    return false;
}

bool NasPage::ProbeNodeForTest(const QString &id)
{
    for (int i = 0; i < nodes_.size(); ++i) {
        if (nodes_.at(i).id != id)
            continue;
        if (nodes_.at(i).type == QLatin1String("webdav"))
            return false;   // 异步；回归只覆盖 local/smb 的同步判定
        ProbeOne(i);
        return nodes_.at(i).ok;
    }
    return false;
}
