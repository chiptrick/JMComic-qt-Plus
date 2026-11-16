#include "nas_store.h"

#include "app_paths.h"
#include "node_http.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QUrl>

namespace {

QString IniPath()
{
    return AppPaths::ConfigFile(QStringLiteral("ui.ini"));
}

QString BasicAuth(const QString &user, const QString &pass)
{
    const QByteArray raw = (user + QLatin1Char(':') + pass).toUtf8();
    return QStringLiteral("Basic ") + QString::fromLatin1(raw.toBase64());
}

QString NormalizeTarget(const NasNode &n)
{
    if (n.type == QLatin1String("local") || n.type == QLatin1String("smb"))
        return QDir::fromNativeSeparators(n.target.trimmed());
    return n.target.trimmed();
}

}  // namespace

namespace NasStore {

QStringList Types()
{
    return {QStringLiteral("local"), QStringLiteral("webdav"), QStringLiteral("smb")};
}

QString TypeLabel(const QString &type)
{
    if (type == QLatin1String("local"))
        return QStringLiteral("本地目录");
    if (type == QLatin1String("webdav"))
        return QStringLiteral("WebDAV");
    if (type == QLatin1String("smb"))
        return QStringLiteral("SMB 共享");
    return type;
}

QString TypeGlyphHint(const QString &type)
{
    if (type == QLatin1String("local"))
        return QStringLiteral("DIR");
    if (type == QLatin1String("webdav"))
        return QStringLiteral("DAV");
    if (type == QLatin1String("smb"))
        return QStringLiteral("SMB");
    return QStringLiteral("?");
}

QVector<NasNode> Load()
{
    QVector<NasNode> out;
    const QString ini = IniPath();
    if (ini.isEmpty())
        return out;
    QSettings s(ini, QSettings::IniFormat);
    const QString raw = s.value(QStringLiteral("nas/nodes")).toString();
    if (raw.trimmed().isEmpty())
        return out;
    const QJsonDocument doc = QJsonDocument::fromJson(raw.toUtf8());
    if (!doc.isArray())
        return out;
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        NasNode n;
        n.id = o.value(QLatin1String("id")).toString();
        n.name = o.value(QLatin1String("name")).toString();
        n.type = o.value(QLatin1String("type")).toString();
        n.target = o.value(QLatin1String("target")).toString();
        n.user = o.value(QLatin1String("user")).toString();
        n.pass = o.value(QLatin1String("pass")).toString();
        if (n.id.isEmpty() || n.type.isEmpty() || n.target.isEmpty())
            continue;
        n.status = QStringLiteral("未校验");
        out.append(n);
    }
    return out;
}

bool Save(const QVector<NasNode> &nodes, QString *error)
{
    const QString ini = IniPath();
    if (ini.isEmpty()) {
        if (error)
            *error = QStringLiteral("配置目录不可写");
        return false;
    }
    QJsonArray arr;
    for (const NasNode &n : nodes) {
        QJsonObject o;
        o.insert(QStringLiteral("id"), n.id);
        o.insert(QStringLiteral("name"), n.name);
        o.insert(QStringLiteral("type"), n.type);
        o.insert(QStringLiteral("target"), n.target);
        o.insert(QStringLiteral("user"), n.user);
        o.insert(QStringLiteral("pass"), n.pass);
        arr.append(o);
    }
    {
        QSettings s(ini, QSettings::IniFormat);
        s.setValue(QStringLiteral("nas/nodes"),
                   QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact)));
    }
    if (!QFileInfo::exists(ini)) {
        if (error)
            *error = QStringLiteral("写入 ui.ini 失败");
        return false;
    }
    if (error)
        error->clear();
    return true;
}

bool Validate(NasNode *node, QString *error)
{
    if (!node)
        return false;
    node->name = node->name.trimmed();
    node->target = node->target.trimmed();
    if (node->name.isEmpty()) {
        if (error)
            *error = QStringLiteral("请填写名称");
        return false;
    }
    if (node->target.isEmpty()) {
        if (error)
            *error = QStringLiteral("请填写地址");
        return false;
    }
    if (!Types().contains(node->type)) {
        if (error)
            *error = QStringLiteral("未知类型：%1").arg(node->type);
        return false;
    }
    if (node->type == QLatin1String("webdav")) {
        const QUrl u(node->target);
        if (!u.isValid() || u.host().isEmpty()
            || (u.scheme() != QLatin1String("https") && u.scheme() != QLatin1String("http"))) {
            if (error)
                *error = QStringLiteral("WebDAV 地址需形如 https://host/dav/");
            return false;
        }
    } else if (node->type == QLatin1String("smb")) {
        // Windows 的 UNC 形态：\\host\share（也接受 //host/share）
        const QString t = QDir::fromNativeSeparators(node->target);
        if (!t.startsWith(QLatin1String("//")) || t.count(QLatin1Char('/')) < 3) {
            if (error)
                *error = QStringLiteral("SMB 地址需形如 \\\\host\\share");
            return false;
        }
    }
    if (node->id.isEmpty())
        node->id = QStringLiteral("nas-%1").arg(QDateTime::currentMSecsSinceEpoch());
    node->status = QStringLiteral("未校验");
    if (error)
        error->clear();
    return true;
}

QString DescribeProbe(const NasNode &node)
{
    if (node.type == QLatin1String("webdav")) {
        QString url = NormalizeTarget(node);
        if (!url.endsWith(QLatin1Char('/')))
            url += QLatin1Char('/');
        return QStringLiteral("PROPFIND %1 depth=0 auth=%2")
            .arg(url, node.user.isEmpty() ? QStringLiteral("none") : QStringLiteral("basic"));
    }
    return QStringLiteral("STAT %1").arg(NormalizeTarget(node));
}

void Probe(const NasNode &node, const std::function<void(const NasNode &)> &done)
{
    NasNode result = node;
    result.checkedMs = QDateTime::currentMSecsSinceEpoch();

    if (node.type != QLatin1String("webdav")) {
        // local / smb：交给系统判断（SMB 需先由用户在系统里映射，见头注释）
        const QString path = NormalizeTarget(node);
        const QFileInfo fi(path);
        result.ok = fi.exists() && fi.isDir();
        if (result.ok) {
            result.status = QStringLiteral("可用");
        } else if (node.type == QLatin1String("smb")) {
            result.status = QStringLiteral("失败：路径不可见（需先在系统映射该共享）");
        } else {
            result.status = QStringLiteral("失败：目录不存在");
        }
        done(result);
        return;
    }

    QString url = NormalizeTarget(node);
    if (!url.endsWith(QLatin1Char('/')))
        url += QLatin1Char('/');

    auto *http = new NodeHttp();
    NodeHttpRequest req;
    req.method = QByteArrayLiteral("PROPFIND");
    req.url = QUrl(url);
    req.timeoutMs = 8000;
    req.maxBytes = 64 * 1024;
    req.allowQuic = false;
    req.headers.insert(QStringLiteral("Depth"), QStringLiteral("0"));
    req.headers.insert(QStringLiteral("Content-Type"), QStringLiteral("application/xml; charset=utf-8"));
    if (!node.user.isEmpty())
        req.headers.insert(QStringLiteral("Authorization"), BasicAuth(node.user, node.pass));
    req.body = QByteArrayLiteral(
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/></d:prop></d:propfind>");

    QObject::connect(http, &NodeHttp::Log, http, [](const QString &) {});
    http->Send(req, [http, result, done](const NodeHttpResponse &r) mutable {
        if (!r.ok) {
            result.ok = false;
            result.status = QStringLiteral("失败：%1").arg(r.error.isEmpty()
                                                               ? QStringLiteral("网络错误")
                                                               : r.error);
        } else if (r.status == 207 || r.status == 200) {
            result.ok = true;
            result.status = QStringLiteral("可用（WebDAV %1）").arg(r.status);
        } else if (r.status == 401 || r.status == 403) {
            result.ok = false;
            result.status = QStringLiteral("失败：认证被拒绝（HTTP %1）").arg(r.status);
        } else if (r.status == 404) {
            result.ok = false;
            result.status = QStringLiteral("失败：路径不存在（HTTP 404）");
        } else {
            result.ok = false;
            result.status = QStringLiteral("失败：HTTP %1").arg(r.status);
        }
        done(result);
        http->deleteLater();
    });
}

QString SelfTest()
{
    QStringList out;
    const QVector<NasNode> backup = Load();
    out << QStringLiteral("nas-store: backup=%1").arg(backup.size());

    // 临时目录 = 一个"真实存在的本地节点"。
    // JM_NAS_TEST_DIR 可覆盖：CI/沙箱里 %TEMP% 可能被策略禁写，测试需要指到可写目录。
    const QString base = qEnvironmentVariable("JM_NAS_TEST_DIR").trimmed().isEmpty()
                             ? QDir::tempPath()
                             : qEnvironmentVariable("JM_NAS_TEST_DIR");
    const QString tmp = QDir(base).filePath(QStringLiteral("jm-nas-store-%1")
                                               .arg(QDateTime::currentMSecsSinceEpoch()));
    const bool made = QDir().mkpath(tmp);
    out << QStringLiteral("nas-store: tmp=%1 made=%2").arg(tmp).arg(made ? 1 : 0);

    NasNode local;
    local.name = QStringLiteral("自检目录");
    local.type = QStringLiteral("local");
    local.target = tmp;
    QString err;
    const bool validLocal = Validate(&local, &err);
    out << QStringLiteral("nas-store: validate-local=%1 err=%2").arg(validLocal ? 1 : 0).arg(err);
    bool probeOk = false;
    if (validLocal) {
        Probe(local, [&probeOk](const NasNode &r) { probeOk = r.ok; });
        out << QStringLiteral("nas-store: probe-local=%1 (expect 1) tmp-exists=%2 isdir=%3 target=%4")
                   .arg(probeOk ? 1 : 0)
                   .arg(QFileInfo::exists(tmp) ? 1 : 0)
                   .arg(QFileInfo(tmp).isDir() ? 1 : 0)
                   .arg(QDir::fromNativeSeparators(local.target));
    }

    QVector<NasNode> list = backup;
    list.append(local);
    const bool saved = Save(list, &err);
    const QVector<NasNode> reloaded = Load();
    out << QStringLiteral("nas-store: save=%1 roundtrip=%2 (expect %3)")
               .arg(saved ? 1 : 0)
               .arg(reloaded.size())
               .arg(backup.size() + 1);

    // 负面：不存在的目录必须判失败；WebDAV 缺 scheme 必须拒绝
    NasNode missing;
    missing.name = QStringLiteral("不存在");
    missing.type = QStringLiteral("local");
    missing.target = tmp + QStringLiteral("/nope");
    const bool validMissing = Validate(&missing, &err);
    bool missingProbe = true;
    if (validMissing)
        Probe(missing, [&missingProbe](const NasNode &r) { missingProbe = r.ok; });
    out << QStringLiteral("nas-store: probe-missing=%1 (expect 0)").arg(missingProbe ? 1 : 0);

    NasNode badDav;
    badDav.name = QStringLiteral("坏地址");
    badDav.type = QStringLiteral("webdav");
    badDav.target = QStringLiteral("nas.local/dav");
    const bool validDav = Validate(&badDav, &err);
    out << QStringLiteral("nas-store: validate-bad-dav=%1 (expect 0) err=%2")
               .arg(validDav ? 1 : 0)
               .arg(err);

    // 还原：把这次追加的节点删掉再写回
    QVector<NasNode> restore = backup;
    Save(restore, &err);
    out << QStringLiteral("nas-store: restored=%1 final-count=%2")
               .arg(restore.size())
               .arg(Load().size());
    QDir(tmp).removeRecursively();
    return out.join(QLatin1Char('\n'));
}

}  // namespace NasStore
