#include "sr_plugin.h"

#include "sr_engine.h"

#if defined(__has_include)
#  if __has_include("ui/app_paths.h")
#    include "ui/app_paths.h"
#    define JM_PLUGIN_HAS_APPPATHS 1
#  elif __has_include("app_paths.h")
#    include "app_paths.h"
#    define JM_PLUGIN_HAS_APPPATHS 1
#  endif
#endif

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

namespace {

constexpr qint64 kProgStep = 64 * 1024;   // 每 64KB 回调一次进度

// 运行时状态表（下载/校验/安装）。UI 与 CLI 共享。
struct RtState {
    QString state = QStringLiteral("idle");   // not_installed/downloading/verifying/installed/error
    int progress = -1;
    QString error;
};
QMutex g_mu;
QHash<QString, RtState> g_status;

void SetStatus(const QString &file, const QString &state, int progress, const QString &error)
{
    QMutexLocker lock(&g_mu);
    RtState &s = g_status[file];
    s.state = state;
    s.progress = progress;
    s.error = error;
}

RtState GetStatus(const QString &file)
{
    QMutexLocker lock(&g_mu);
    return g_status.value(file);
}

QString DataDir()
{
#ifdef JM_PLUGIN_HAS_APPPATHS
    return AppPaths::DataDir();
#else
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    QDir().mkpath(base);
    return base;
#endif
}

QString CacheManifestPath()
{
    return QDir(DataDir()).filePath(QStringLiteral("sr_models_manifest.json"));
}

// 从文件名推断算法分组（清单里没有 arch 字段时用）。
QString ArchFromFile(const QString &file)
{
    const QString f = file.toLower();
    if (f.contains(QStringLiteral("_q16")) || f.contains(QStringLiteral("lowq")))
        return QStringLiteral("quant");
    if (f.startsWith(QStringLiteral("waifu2x")))
        return QStringLiteral("waifu2x");
    if (f.startsWith(QStringLiteral("realcugan")))
        return QStringLiteral("realcugan");
    if (f.startsWith(QStringLiteral("realesrgan")))
        return QStringLiteral("realesrgan");
    if (f.startsWith(QStringLiteral("srmd")))
        return QStringLiteral("srmd");
    return QStringLiteral("other");
}

QString DisplayNameFor(const QString &file, const QString &nameFromManifest)
{
    // 优先用引擎目录里的中文显示名（与选择浮层一致），清单的 NAME 作为兜底。
    for (const SrEngine::ModelInfo &m : SrEngine::Catalog()) {
        if (m.file.compare(file, Qt::CaseInsensitive) == 0)
            return m.name;
    }
    return nameFromManifest.isEmpty() ? QFileInfo(file).completeBaseName() : nameFromManifest;
}

// 解析一份清单 JSON 文本；成功返回 true 并填充 baseUrl 与 packages。
bool ParseManifest(const QByteArray &json, QString *baseUrl, QVector<SrPlugin::Package> *out,
                   QString *err)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (doc.isNull() || !doc.isObject()) {
        if (err)
            *err = QStringLiteral("清单不是合法 JSON：%1").arg(pe.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    if (baseUrl)
        *baseUrl = root.value(QStringLiteral("base_url")).toString();
    const QJsonArray models = root.value(QStringLiteral("models")).toArray();
    if (models.isEmpty()) {
        if (err)
            *err = QStringLiteral("清单没有 models 数组");
        return false;
    }
    out->clear();
    for (const QJsonValue &v : models) {
        const QJsonObject o = v.toObject();
        SrPlugin::Package p;
        p.file = o.value(QStringLiteral("file")).toString();
        if (p.file.isEmpty())
            continue;
        p.name = DisplayNameFor(p.file, o.value(QStringLiteral("name")).toString());
        p.arch = o.value(QStringLiteral("arch")).toString();
        if (p.arch.isEmpty())
            p.arch = ArchFromFile(p.file);
        p.scale = o.value(QStringLiteral("scale")).toInt(2);
        p.denoise = o.value(QStringLiteral("denoise")).toInt(0);
        p.tile = o.value(QStringLiteral("tile")).toInt(192);
        p.size = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble(0));
        p.sha256 = o.value(QStringLiteral("sha256")).toString().toLower();
        out->push_back(p);
    }
    if (out->isEmpty()) {
        if (err)
            *err = QStringLiteral("清单 models 里没有带 file 的条目");
        return false;
    }
    return true;
}

}  // namespace

namespace SrPlugin {

QString PluginsDir()
{
    // 单一事实来源：与 SrEngine 的模型搜索路径第一项保持一致（同一 DataDir/sr_models）。
    return SrEngine::PluginsDir();
}

// 当前生效清单（读文件，不解析成对象）。优先级：网络缓存 -> 内嵌资源。
static QByteArray LoadManifest(QString *source)
{
    const QString cache = CacheManifestPath();
    QFile cf(cache);
    if (cf.exists() && cf.open(QIODevice::ReadOnly)) {
        const QByteArray data = cf.readAll();
        cf.close();
        if (!data.trimmed().isEmpty()) {
            if (source)
                *source = QStringLiteral("cache");
            return data;
        }
    }
    QFile ef(QStringLiteral(":/sr/sr_models_manifest.json"));
    if (ef.open(QIODevice::ReadOnly)) {
        const QByteArray data = ef.readAll();
        ef.close();
        if (source)
            *source = QStringLiteral("embedded");
        return data;
    }
    if (source)
        *source = QStringLiteral("missing");
    return QByteArray();
}

QString ManifestSource()
{
    QString src;
    LoadManifest(&src);
    return src;
}

static QVector<Package> LoadPackages(QString *baseUrlOut = nullptr)
{
    QVector<Package> pkgs;
    QString src;
    const QByteArray data = LoadManifest(&src);
    if (data.isEmpty())
        return pkgs;
    QString baseUrl;
    QString err;
    if (!ParseManifest(data, &baseUrl, &pkgs, &err))
        return {};
    if (baseUrlOut)
        *baseUrlOut = baseUrl;
    return pkgs;
}

QString ManifestBaseUrl()
{
    QString baseUrl;
    LoadPackages(&baseUrl);
    return baseUrl;
}

QVector<Package> Catalog()
{
    QVector<Package> pkgs = LoadPackages();
    // 用引擎的搜索路径判断"已安装"（含随包 assets），并单独标出是否在受管目录。
    const QString plugDir = QDir(PluginsDir()).absolutePath();
    for (Package &p : pkgs) {
        // 是否命中引擎可解析的任意目录
        const bool resolved = [&] {
            for (const SrEngine::ModelInfo &m : SrEngine::InstalledModels()) {
                if (m.file.compare(p.file, Qt::CaseInsensitive) == 0)
                    return true;
            }
            return false;
        }();
        p.installed = resolved;
        const QString inPlug = QDir(plugDir).filePath(p.file);
        p.inPluginDir = QFileInfo::exists(inPlug);
        p.installedBytes = 0;
        if (p.inPluginDir)
            p.installedBytes = QFileInfo(inPlug).size();
        else {
            // 随包/其它目录：字节数交给引擎
            for (const SrEngine::ModelInfo &m : SrEngine::InstalledModels()) {
                if (m.file.compare(p.file, Qt::CaseInsensitive) == 0) {
                    p.installedBytes = m.bytes;
                    break;
                }
            }
        }
        RtState rt = GetStatus(p.file);
        if (rt.state != QLatin1String("idle")) {
            p.state = rt.state;
            p.progress = rt.progress;
            p.error = rt.error;
        } else {
            p.state = p.installed ? QStringLiteral("installed") : QStringLiteral("not_installed");
        }
    }
    return pkgs;
}

QVector<Package> Installed()
{
    QVector<Package> out;
    for (const Package &p : Catalog()) {
        if (p.installed)
            out.push_back(p);
    }
    return out;
}

bool RefreshManifest(QString *err)
{
    const QByteArray cur = LoadManifest(nullptr);
    QString baseUrl;
    QString perr;
    QVector<Package> pkgs;
    if (!ParseManifest(cur, &baseUrl, &pkgs, &perr)) {
        if (err)
            *err = perr;
        return false;
    }
    // 清单 URL：优先 env JM_SR_MANIFEST_URL（私有镜像/回归），否则按 base_url 同目录拼出
    // sr_models_manifest.json。注意 base_url 指向的发布目录必须真的托管这份清单文件，
    // 否则刷新会 404（此时内嵌清单仍然可用，只是无法拿到"新增模型"）。
    QString manifestUrl = qEnvironmentVariable("JM_SR_MANIFEST_URL").trimmed();
    if (manifestUrl.isEmpty())
        manifestUrl = baseUrl + QStringLiteral("sr_models_manifest.json");

    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(manifestUrl)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JMComic-qt/sr-plugin"));
    QNetworkReply *reply = nam.get(req);

    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        timedOut = true;
        reply->abort();
        loop.quit();
    });
    timeout.start(20000);
    loop.exec();

    const QByteArray body = reply->readAll();
    const auto netErr = reply->error();
    const QString netErrStr = reply->errorString();
    reply->deleteLater();

    if (timedOut) {
        if (err)
            *err = QStringLiteral("刷新清单超时（20s）");
        return false;
    }
    if (netErr != QNetworkReply::NoError || body.trimmed().isEmpty()) {
        if (err)
            *err = QStringLiteral("刷新清单失败：%1").arg(netErrStr);
        return false;
    }

    // 先验证能解析，再落盘（避免把坏 JSON 写进缓存）。
    QVector<Package> newPkgs;
    QString nbase, nerr;
    if (!ParseManifest(body, &nbase, &newPkgs, &nerr)) {
        if (err)
            *err = QStringLiteral("远端清单无效：%1").arg(nerr);
        return false;
    }
    QFile f(CacheManifestPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("无法写入清单缓存：%1").arg(CacheManifestPath());
        return false;
    }
    f.write(body);
    f.close();
    return true;
}

static bool ResolvePackage(const QString &fileOrName, Package *out)
{
    for (const Package &p : Catalog()) {
        if (p.file.compare(fileOrName, Qt::CaseInsensitive) == 0
            || p.name == fileOrName) {
            *out = p;
            return true;
        }
    }
    return false;
}

bool Install(const QString &fileOrName,
             const std::function<void(qint64, qint64)> &progress,
             const std::function<bool()> &cancel, QString *err)
{
    Package pkg;
    if (!ResolvePackage(fileOrName, &pkg)) {
        if (err)
            *err = QStringLiteral("清单里找不到模型包：%1").arg(fileOrName);
        SetStatus(fileOrName, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }
    const QString baseUrl = ManifestBaseUrl();
    if (baseUrl.isEmpty()) {
        if (err)
            *err = QStringLiteral("清单缺少 base_url，无法下载");
        SetStatus(pkg.file, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }
    const QString url = baseUrl + pkg.file;
    const QString dir = PluginsDir();
    const QString finalPath = QDir(dir).filePath(pkg.file);
    const QString partPath = finalPath + QStringLiteral(".part");

    SetStatus(pkg.file, QStringLiteral("downloading"), 0, QString());

    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JMComic-qt/sr-plugin"));
    QNetworkReply *reply = nam.get(req);

    QFile part(partPath);
    if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply->abort();
        reply->deleteLater();
        if (err)
            *err = QStringLiteral("无法写入临时文件：%1").arg(partPath);
        SetStatus(pkg.file, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }

    QCryptographicHash sha(QCryptographicHash::Sha256);
    qint64 total = pkg.size;   // 可能为 0，下面从 header 补
    qint64 received = 0;
    qint64 lastEmit = 0;
    bool aborted = false;
    bool critErr = false;
    QString errStr;
    QEventLoop loop;

    auto fail = [&](const QString &m) {
        critErr = true;
        errStr = m;
        aborted = true;
        reply->abort();
    };

    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        if (aborted)
            return;
        if (cancel && cancel()) {
            errStr = QStringLiteral("已取消");
            aborted = true;
            reply->abort();
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;
        sha.addData(chunk);
        if (part.write(chunk) != chunk.size()) {
            fail(QStringLiteral("写入磁盘失败"));
            return;
        }
        received += chunk.size();
        if (progress && (received - lastEmit >= kProgStep || received == total)) {
            lastEmit = received;
            progress(received, total);
        }
        const int pct = total > 0 ? int(100.0 * received / total) : -1;
        SetStatus(pkg.file, QStringLiteral("downloading"), pct, QString());
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop,
                     [&](qint64 r, qint64 t) {
                         if (t > 0)
                             total = t;
                         (void)r;
                     });

    // 安全超时：避免网络卡死时永久阻塞（大文件按 8KB/s 下限估算，最少 120s）
    QTimer guard;
    guard.setSingleShot(true);
    const qint64 guardMs = qBound<qint64>(qint64(180000),
                                       (total > 0 ? total / 8 : qint64(120000)),
                                       qint64(1800000));
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        if (!aborted) {
            errStr = QStringLiteral("下载超时");
            aborted = true;
            reply->abort();
        }
        loop.quit();
    });
    guard.start(int(qMax<qint64>(180000, guardMs)));

    loop.exec();

    if (!aborted) {
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty()) {
            sha.addData(tail);
            part.write(tail);
            received += tail.size();
        }
    }
    part.close();
    const auto netErr = reply->error();
    reply->deleteLater();

    if (aborted || critErr) {
        QFile::remove(partPath);
        if (err)
            *err = errStr.isEmpty() ? QStringLiteral("下载中止") : errStr;
        const bool cancelled = (errStr == QLatin1String("已取消"));
        SetStatus(pkg.file, cancelled ? QStringLiteral("not_installed")
                                      : QStringLiteral("error"),
                  -1, err ? *err : QString());
        return false;
    }
    if (netErr != QNetworkReply::NoError) {
        QFile::remove(partPath);
        if (err)
            *err = QStringLiteral("下载失败：%1").arg(reply->errorString());
        SetStatus(pkg.file, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }

    // 校验
    SetStatus(pkg.file, QStringLiteral("verifying"), 100, QString());
    if (!pkg.sha256.isEmpty()) {
        const QString got = QString::fromLatin1(sha.result().toHex());
        if (got.compare(pkg.sha256, Qt::CaseInsensitive) != 0) {
            QFile::remove(partPath);
            if (err)
                *err = QStringLiteral("sha256 校验失败（期望 %1，实际 %2）")
                           .arg(pkg.sha256, got);
            SetStatus(pkg.file, QStringLiteral("error"), -1, err ? *err : QString());
            return false;
        }
    }
    if (pkg.size > 0 && QFileInfo(partPath).size() != pkg.size) {
        QFile::remove(partPath);
        if (err)
            *err = QStringLiteral("尺寸校验失败（期望 %1，实际 %2）")
                       .arg(pkg.size)
                       .arg(QFileInfo(partPath).size());
        SetStatus(pkg.file, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }

    // 原子改名（已存在先删，实现升级覆盖）
    if (QFileInfo::exists(finalPath))
        QFile::remove(finalPath);
    if (!QFile::rename(partPath, finalPath)) {
        QFile::remove(partPath);
        if (err)
            *err = QStringLiteral("安装落盘失败：%1").arg(finalPath);
        SetStatus(pkg.file, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }
    SetStatus(pkg.file, QStringLiteral("installed"), 100, QString());
    return true;
}

bool Uninstall(const QString &fileOrName, bool alsoBundled, QString *err)
{
    Package pkg;
    if (!ResolvePackage(fileOrName, &pkg)) {
        // 清单里没有也可能在目录里，按文件名兜底
        pkg.file = fileOrName;
    }
    bool removedAny = false;
    const QString inPlug = QDir(PluginsDir()).filePath(pkg.file);
    if (QFileInfo::exists(inPlug)) {
        if (QFile::remove(inPlug)) {
            removedAny = true;
        } else if (err) {
            *err = QStringLiteral("删除失败（文件被占用？）：%1").arg(inPlug);
            return false;
        }
    }
    if (alsoBundled) {
        // 尝试删其它可写模型目录里的同名文件（随包 assets 删不掉，忽略）
        for (const QString &d : SrEngine::ModelSearchPath().split(QLatin1Char('|'),
                                                                  Qt::SkipEmptyParts)) {
            if (d.startsWith(QStringLiteral("assets:")))
                continue;
            if (QDir(d).absolutePath() == QDir(PluginsDir()).absolutePath())
                continue;
            const QString f = QDir(d).filePath(pkg.file);
            if (QFileInfo::exists(f) && QFile::remove(f))
                removedAny = true;
        }
    }
    if (!removedAny) {
        if (err)
            *err = QStringLiteral("未找到可删除的模型文件：%1").arg(pkg.file);
        return false;
    }
    {
        QMutexLocker lock(&g_mu);
        g_status.remove(pkg.file);
    }
    return true;
}

QHash<QString, StatusEntry> StatusSnapshot()
{
    QHash<QString, StatusEntry> out;
    QMutexLocker lock(&g_mu);
    for (auto it = g_status.constBegin(); it != g_status.constEnd(); ++it) {
        StatusEntry e;
        e.state = it->state;
        e.progress = it->progress;
        out.insert(it.key(), e);
    }
    return out;
}

bool AnyActive()
{
    QMutexLocker lock(&g_mu);
    for (auto it = g_status.constBegin(); it != g_status.constEnd(); ++it) {
        if (it->state == QLatin1String("downloading")
            || it->state == QLatin1String("verifying"))
            return true;
    }
    return false;
}

QVariantMap StateFor(const QString &fileOrName)
{
    QVariantMap m;
    Package pkg;
    if (ResolvePackage(fileOrName, &pkg) || !fileOrName.isEmpty())
        m[QStringLiteral("file")] = pkg.file.isEmpty() ? fileOrName : pkg.file;
    RtState rt = GetStatus(pkg.file.isEmpty() ? fileOrName : pkg.file);
    m[QStringLiteral("state")] = rt.state;
    m[QStringLiteral("progress")] = rt.progress;
    m[QStringLiteral("error")] = rt.error;
    return m;
}

QString Report()
{
    QStringList lines;
    lines << QStringLiteral("plugin-report: source=%1 base=%2 dir=%3")
                 .arg(ManifestSource(), ManifestBaseUrl(), PluginsDir());
    const QVector<Package> all = Catalog();
    int installed = 0;
    qint64 totalBytes = 0;
    qint64 installedBytes = 0;
    for (const Package &p : all) {
        installed += p.installed ? 1 : 0;
        totalBytes += p.size;
        if (p.installed)
            installedBytes += p.installedBytes;
        lines << QStringLiteral("plugin-item: file=%1 arch=%2 scale=%3 installed=%4 "
                                "inPluginDir=%5 bytes=%6 size=%7 state=%8")
                     .arg(p.file, p.arch)
                     .arg(p.scale)
                     .arg(int(p.installed))
                     .arg(int(p.inPluginDir))
                     .arg(p.installedBytes)
                     .arg(p.size)
                     .arg(p.state);
    }
    lines << QStringLiteral("plugin-summary: total=%1 installed=%2 sizes=%3/%4 remain=%5")
                 .arg(all.size())
                 .arg(installed)
                 .arg(installedBytes)
                 .arg(totalBytes)
                 .arg(totalBytes - installedBytes);
    return lines.join(QLatin1Char('\n'));
}

QString SelfTest()
{
    QStringList issues;
    QString baseUrl;
    QVector<Package> pkgs = LoadPackages(&baseUrl);
    if (pkgs.isEmpty())
        issues << QStringLiteral("清单为空或无法解析");
    if (baseUrl.isEmpty())
        issues << QStringLiteral("base_url 为空");
    else if (!baseUrl.startsWith(QStringLiteral("https://")))
        issues << QStringLiteral("base_url 不是 https");

    QHash<QString, int> seenFiles;
    int noSha = 0;
    int noSize = 0;
    for (const Package &p : pkgs) {
        if (!p.file.endsWith(QStringLiteral(".onnx")))
            issues << QStringLiteral("非 .onnx：%1").arg(p.file);
        if (p.file.contains(QLatin1Char('/')) || p.file.contains(QLatin1Char('\\')))
            issues << QStringLiteral("文件名含路径分隔符：%1").arg(p.file);
        if (p.sha256.isEmpty())
            ++noSha;
        if (p.size <= 0)
            ++noSize;
        seenFiles[p.file]++;
    }
    for (auto it = seenFiles.begin(); it != seenFiles.end(); ++it) {
        if (it.value() > 1)
            issues << QStringLiteral("重复文件名 x%1：%2").arg(it.value()).arg(it.key());
    }

    // 受管目录可写
    const QString dir = PluginsDir();
    bool writable = false;
    {
        QFile probe(QDir(dir).filePath(QStringLiteral(".jm_plugin_probe")));
        if (probe.open(QIODevice::WriteOnly)) {
            probe.write("ok", 2);
            probe.close();
            writable = true;
            QFile::remove(probe.fileName());
        }
    }
    if (!writable)
        issues << QStringLiteral("插件目录不可写：%1").arg(dir);

    const bool pass = issues.isEmpty();
    QStringList out;
    out << QStringLiteral("plugin-selftest: %1 models=%2 base=%3 dir=%4 noSha=%5 noSize=%6")
               .arg(pass ? QStringLiteral("PASS") : QStringLiteral("FAIL"))
               .arg(pkgs.size())
               .arg(baseUrl, dir)
               .arg(noSha)
               .arg(noSize);
    for (const QString &s : issues)
        out << QStringLiteral("plugin-selftest-issue: %1").arg(s);
    return out.join(QLatin1Char('\n'));
}


// ================================================================ 推理模块（ONNX Runtime）
namespace {

QString RuntimeManifestBase(QByteArray *out)
{
    // 桌面运行时清单内嵌 :/sr/sr_runtime_manifest.json；也允许用缓存覆盖（目前无 UI 刷新，
    // 保留 --plugin-refresh 之外的扩展点）。
    QFile ef(QStringLiteral(":/sr/sr_runtime_manifest.json"));
    if (ef.open(QIODevice::ReadOnly)) {
        const QByteArray data = ef.readAll();
        ef.close();
        if (out)
            *out = data;
        return QStringLiteral("embedded");
    }
    if (out)
        out->clear();
    return QStringLiteral("missing");
}

QString HostPlatform()
{
#if defined(_WIN64)
    return QStringLiteral("win-x64");
#elif defined(_WIN32)
    return QStringLiteral("win-x86");
#elif defined(Q_OS_ANDROID)
    return QStringLiteral("android");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("mac");
#else
    return QStringLiteral("linux");
#endif
}

bool ParseRuntimeManifest(const QByteArray &json, QString *baseUrl,
                          QVector<SrPlugin::RuntimeInfo> *out)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (doc.isNull() || !doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    if (baseUrl)
        *baseUrl = root.value(QStringLiteral("base_url")).toString();
    const QJsonArray arr = root.value(QStringLiteral("runtimes")).toArray();
    out->clear();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        SrPlugin::RuntimeInfo ri;
        ri.id = o.value(QStringLiteral("id")).toString();
        ri.name = o.value(QStringLiteral("name")).toString();
        ri.file = o.value(QStringLiteral("file")).toString();
        ri.version = o.value(QStringLiteral("version")).toString();
        ri.size = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble(0));
        ri.sha256 = o.value(QStringLiteral("sha256")).toString().toLower();
        if (ri.file.isEmpty())
            ri.file = QStringLiteral("onnxruntime.dll");
        if (ri.name.isEmpty())
            ri.name = ri.file;
        out->push_back(ri);
    }
    return !out->isEmpty();
}

// 该平台在本机是否"已具备可加载运行时"：sr_runtime/ 里有，或随安装包目录里有。
QString PackagedRuntime()
{
#if defined(Q_OS_ANDROID)
    return QString();   // Android 运行时随 APK，不在此模块范围
#else
    const QString appDir = QCoreApplication::applicationDirPath();
    QStringList cands;
    cands << QDir(appDir).filePath(QStringLiteral("onnxruntime.dll"))
          << QDir(appDir).filePath(QStringLiteral("lib/onnxruntime.dll"));
    for (const QString &p : cands) {
        if (QFileInfo::exists(p))
            return p;
    }
    return QString();
#endif
}

}  // namespace

QString RuntimePath()
{
    return SrEngine::RuntimePath();
}

bool RuntimeInstalled()
{
    const QString dirFile = QDir(SrEngine::RuntimeDir()).filePath(QStringLiteral("onnxruntime.dll"));
    if (QFileInfo::exists(dirFile))
        return true;
    return !PackagedRuntime().isEmpty();
}

QVector<RuntimeInfo> RuntimeCatalog()
{
    QVector<RuntimeInfo> out;
    QByteArray json;
    RuntimeManifestBase(&json);
    QString baseUrl;
    if (!ParseRuntimeManifest(json, &baseUrl, &out))
        return out;
    const QString host = HostPlatform();
    const QString dirFile = QDir(SrEngine::RuntimeDir()).filePath(QStringLiteral("onnxruntime.dll"));
    const bool inDir = QFileInfo::exists(dirFile);
    const QString packaged = PackagedRuntime();
    const QString actual = SrEngine::RuntimePath();
    for (RuntimeInfo &ri : out) {
        ri.installed = inDir;
        ri.installedBytes = inDir ? QFileInfo(dirFile).size() : 0;
        ri.active = !actual.isEmpty();
        RtState rt = GetStatus(QStringLiteral("rt:") + ri.id);
        if (rt.state != QLatin1String("idle")) {
            ri.state = rt.state;
            ri.progress = rt.progress;
            ri.error = rt.error;
        } else if (inDir) {
            ri.state = QStringLiteral("installed");
        } else if (!packaged.isEmpty()) {
            // 随安装包已带：显示"随包提供"，但用户仍可下载覆盖到 sr_runtime/
            ri.state = QStringLiteral("installed");
        } else {
            ri.state = QStringLiteral("not_installed");
        }
        Q_UNUSED(host);
    }
    return out;
}

bool InstallRuntime(const QString &idOrFile,
                    const std::function<void(qint64, qint64)> &progress,
                    const std::function<bool()> &cancel, QString *err)
{
    QByteArray json;
    RuntimeManifestBase(&json);
    QString baseUrl;
    QVector<RuntimeInfo> list;
    if (!ParseRuntimeManifest(json, &baseUrl, &list)) {
        if (err)
            *err = QStringLiteral("推理模块清单缺失或无法解析");
        return false;
    }
    RuntimeInfo pick;
    bool found = false;
    for (const RuntimeInfo &ri : list) {
        if (ri.id == idOrFile || ri.file == idOrFile) {
            pick = ri;
            found = true;
            break;
        }
    }
    if (!found) {
        // 缺省取清单第一条（本机唯一运行时）
        pick = list.first();
    }
    const QString statusKey = QStringLiteral("rt:") + pick.id;

    // URL：JM_SR_RUNTIME_URL 覆盖（直链 exe/zip）；否则 base_url + file
    QString url = qEnvironmentVariable("JM_SR_RUNTIME_URL").trimmed();
    if (url.isEmpty()) {
        if (baseUrl.isEmpty()) {
            if (err)
                *err = QStringLiteral("推理模块清单缺少 base_url");
            return false;
        }
        url = baseUrl + pick.file;
    }
    const bool isZip = url.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive);

    const QString dir = SrEngine::RuntimeDir();
    const QString finalPath = QDir(dir).filePath(QStringLiteral("onnxruntime.dll"));
    const QString partPath = finalPath + QStringLiteral(".part");
    const QString zipPath = QDir(dir).filePath(QStringLiteral("rt-download.zip"));

    SetStatus(statusKey, QStringLiteral("downloading"), 0, QString());

    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JMComic-qt/sr-runtime"));
    QNetworkReply *reply = nam.get(req);

    // 落盘目标：zip 直链先写 zipPath，否则写 .part
    const QString writePath = isZip ? zipPath : partPath;
    QFile part(writePath);
    if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply->abort();
        reply->deleteLater();
        if (err)
            *err = QStringLiteral("无法写入临时文件：%1").arg(writePath);
        SetStatus(statusKey, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }

    QCryptographicHash sha(QCryptographicHash::Sha256);
    qint64 total = pick.size;
    qint64 received = 0;
    qint64 lastEmit = 0;
    bool aborted = false;
    QString errStr;
    QEventLoop loop;

    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        if (aborted)
            return;
        if (cancel && cancel()) {
            errStr = QStringLiteral("已取消");
            aborted = true;
            reply->abort();
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;
        sha.addData(chunk);
        if (part.write(chunk) != chunk.size()) {
            errStr = QStringLiteral("写入磁盘失败");
            aborted = true;
            reply->abort();
            return;
        }
        received += chunk.size();
        if (progress && (received - lastEmit >= kProgStep || received == total)) {
            lastEmit = received;
            progress(received, total);
        }
        SetStatus(statusKey, QStringLiteral("downloading"),
                  total > 0 ? int(100.0 * received / total) : -1, QString());
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop, [&](qint64, qint64 t) {
        if (t > 0)
            total = t;
    });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        if (!aborted) {
            errStr = QStringLiteral("下载超时");
            aborted = true;
            reply->abort();
        }
        loop.quit();
    });
    guard.start(int(qBound<qint64>(qint64(180000),
                                   total > 0 ? total / 8 : qint64(120000),
                                   qint64(1800000))));
    loop.exec();
    if (!aborted) {
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty()) {
            sha.addData(tail);
            part.write(tail);
            received += tail.size();
        }
    }
    part.close();
    const auto netErr = reply->error();
    reply->deleteLater();

    if (aborted || netErr != QNetworkReply::NoError) {
        QFile::remove(writePath);
        if (err)
            *err = errStr.isEmpty() ? QStringLiteral("下载失败：%1").arg(netErr) : errStr;
        const bool cancelled = (errStr == QLatin1String("已取消"));
        SetStatus(statusKey, cancelled ? QStringLiteral("not_installed")
                                       : QStringLiteral("error"),
                  -1, err ? *err : QString());
        return false;
    }

    SetStatus(statusKey, QStringLiteral("verifying"), 100, QString());

    if (isZip) {
        // 从 zip 里抽出 onnxruntime.dll 到 partPath
        // 说明：这里只支持"zip 内含 onnxruntime.dll"的简单结构（Release 打包用）。
        // 优先用 Qt 的 QZipReader 不可用（默认未链接），改用系统解压不现实；用 unzip 式
        // 手动解析成本高。改为：zip 直链仅作兜底，要求用户用 exe/裸 dll 直链。
        QFile::remove(zipPath);
        if (err)
            *err = QStringLiteral("暂不支持 .zip 直链，请用裸 onnxruntime.dll 直链"
                                  "（JM_SR_RUNTIME_URL 指向 .dll）");
        SetStatus(statusKey, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }

    // 校验（若有 sha256/尺寸）
    if (!pick.sha256.isEmpty()) {
        const QString got = QString::fromLatin1(sha.result().toHex());
        if (got.compare(pick.sha256, Qt::CaseInsensitive) != 0) {
            QFile::remove(partPath);
            if (err)
                *err = QStringLiteral("sha256 校验失败（期望 %1，实际 %2）")
                           .arg(pick.sha256, got);
            SetStatus(statusKey, QStringLiteral("error"), -1, err ? *err : QString());
            return false;
        }
    }
    if (pick.size > 0 && QFileInfo(partPath).size() != pick.size) {
        QFile::remove(partPath);
        if (err)
            *err = QStringLiteral("尺寸校验失败（期望 %1，实际 %2）")
                       .arg(pick.size)
                       .arg(QFileInfo(partPath).size());
        SetStatus(statusKey, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }

    if (QFileInfo::exists(finalPath))
        QFile::remove(finalPath);
    if (!QFile::rename(partPath, finalPath)) {
        QFile::remove(partPath);
        if (err)
            *err = QStringLiteral("安装落盘失败：%1").arg(finalPath);
        SetStatus(statusKey, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }
    // 换运行时：卸载旧库，下次推理重建会话即用新文件
    SrEngine::UnloadRuntime();
    SetStatus(statusKey, QStringLiteral("installed"), 100, QString());
    return true;
}

bool UninstallRuntime(const QString &idOrFile, QString *err)
{
    const QString finalPath = QDir(SrEngine::RuntimeDir()).filePath(QStringLiteral("onnxruntime.dll"));
    if (!QFileInfo::exists(finalPath)) {
        if (err)
            *err = QStringLiteral("未找到已下载的推理模块（sr_runtime/onnxruntime.dll 不存在）");
        return false;
    }
    SrEngine::UnloadRuntime();
    if (!QFile::remove(finalPath)) {
        if (err)
            *err = QStringLiteral("删除失败（文件被占用？）：%1").arg(finalPath);
        return false;
    }
    {
        QMutexLocker lock(&g_mu);
        g_status.remove(QStringLiteral("rt:") + idOrFile);
    }
    return true;
}


// ================================================================ 超分后端插件（桌面）
namespace {

QByteArray LoadBackendManifest()
{
    QFile ef(QStringLiteral(":/sr/sr_backend_manifest.json"));
    if (ef.open(QIODevice::ReadOnly)) {
        const QByteArray data = ef.readAll();
        ef.close();
        return data;
    }
    return QByteArray();
}

QString AppBinDir()
{
    return QCoreApplication::applicationDirPath();
}

// 目标目录：优先受管目录 <data>/sr_backend/<sub>；不存在则回退随包 <bin>/<sub>。
QString BackendSubDirFor(const QString &sub, bool *managed = nullptr)
{
    const QString m = QDir(SrPlugin::BackendPackagesDir()).filePath(sub);
    if (QDir(m).exists()) {
        if (managed)
            *managed = true;
        return m;
    }
    if (managed)
        *managed = false;
    return QDir(AppBinDir()).filePath(sub);
}

bool ParseBackends(const QByteArray &json, QString *baseUrl, QVector<SrPlugin::BackendPluginInfo> *out)
{
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &pe);
    if (doc.isNull() || !doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    if (baseUrl)
        *baseUrl = root.value(QStringLiteral("base_url")).toString();
    const QJsonArray arr = root.value(QStringLiteral("backends")).toArray();
    out->clear();
    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        SrPlugin::BackendPluginInfo b;
        b.id = o.value(QStringLiteral("id")).toString();
        b.name = o.value(QStringLiteral("name")).toString();
        b.desc = o.value(QStringLiteral("desc")).toString();
        b.kind = o.value(QStringLiteral("kind")).toString();
        b.platform = o.value(QStringLiteral("platform")).toString();
        b.targetSubdir = o.value(QStringLiteral("targetSubdir")).toString();
        b.installedByDefault = o.value(QStringLiteral("installedByDefault")).toBool(false);
        b.requiresBinary = o.value(QStringLiteral("requiresBinary")).toBool(false);
        b.archiveSize = static_cast<qint64>(o.value(QStringLiteral("archiveSize")).toDouble(0));
        b.archiveSha256 = o.value(QStringLiteral("archiveSha256")).toString().toLower();
        b.fileCount = o.value(QStringLiteral("files")).toArray().size();
        if (b.id.isEmpty())
            continue;
        out->push_back(b);
    }
    return !out->isEmpty();
}

// 目标目录里是否已有文件（任一文件即认为已安装）
bool DirHasFiles(const QString &dir)
{
    QDir d(dir);
    if (!d.exists())
        return false;
    const auto entries = d.entryList(QDir::Files);
    return !entries.isEmpty();
}

// 从 tar.gz 解压到 destDir（平铺）。用系统 tar（Windows 10+ 自带 bsdtar，Linux/mac 也有）。
// - Windows: tar -xzf <gz> -C <dest>
// - 其它: 同
bool ExtractTarGz(const QString &gz, const QString &destDir, QString *err)
{
    QDir().mkpath(destDir);
    QString prog = QStringLiteral("tar");
    QProcess p;
    p.setWorkingDirectory(destDir);
    p.start(prog, {QStringLiteral("-xzf"), QDir::toNativeSeparators(gz),
                   QStringLiteral("-C"), QDir::toNativeSeparators(destDir)});
    if (!p.waitForStarted(5000)) {
        if (err)
            *err = QStringLiteral("无法启动解压程序 tar（系统缺少 tar？）");
        return false;
    }
    if (!p.waitForFinished(300000)) {
        p.kill();
        if (err)
            *err = QStringLiteral("解压超时");
        return false;
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
        if (err)
            *err = QStringLiteral("解压失败（tar exit=%1）：%2")
                       .arg(p.exitCode())
                       .arg(QString::fromLocal8Bit(p.readAllStandardError()).trimmed());
        return false;
    }
    return true;
}

}  // namespace

QString BackendPackagesDir()
{
#ifdef JM_PLUGIN_HAS_APPPATHS
    const QString dir = QDir(AppPaths::DataDir()).filePath(QStringLiteral("sr_backend"));
#else
    const QString dir = QDir(AppBinDir()).filePath(QStringLiteral("sr_backend"));
#endif
    QDir().mkpath(dir);
    return dir;
}

QVector<BackendPluginInfo> BackendCatalog()
{
    QVector<BackendPluginInfo> out;
    const QByteArray json = LoadBackendManifest();
    QString baseUrl;
    if (!ParseBackends(json, &baseUrl, &out))
        return out;
    for (BackendPluginInfo &b : out) {
        const QString managedDir = QDir(SrPlugin::BackendPackagesDir()).filePath(b.targetSubdir);
        const QString binDir = QDir(AppBinDir()).filePath(b.targetSubdir);
        b.managedInstalled = DirHasFiles(managedDir);
        const bool bundledFiles = DirHasFiles(binDir);
        b.installed = b.managedInstalled || bundledFiles;
        const QString sub = b.managedInstalled ? managedDir : binDir;
        // 可用性：着色器包随包即内置可用（安装插件版则覆盖）；二进制插件需目标目录里有
        // onnxruntime_providers_openvino_plugin.dll。
        if (b.kind == QLatin1String("shaders")) {
            b.available = true;
        } else if (b.requiresBinary) {
            b.available = QFileInfo::exists(QDir(sub).filePath(
                QStringLiteral("onnxruntime_providers_openvino_plugin.dll")));
        } else {
            b.available = b.installed;
        }
        RtState rt = GetStatus(QStringLiteral("be:") + b.id);
        if (rt.state != QLatin1String("idle")) {
            b.state = rt.state;
            b.progress = rt.progress;
            b.error = rt.error;
        } else {
            b.state = b.installed ? QStringLiteral("installed") : QStringLiteral("not_installed");
        }
    }
    return out;
}

bool BackendInstalled(const QString &id)
{
    for (const BackendPluginInfo &b : BackendCatalog()) {
        if (b.id == id)
            return b.installed;
    }
    return false;
}

bool InstallBackend(const QString &id,
                    const std::function<void(qint64, qint64)> &progress,
                    const std::function<bool()> &cancel, QString *err)
{
    QVector<BackendPluginInfo> list;
    QString baseUrl;
    if (!ParseBackends(LoadBackendManifest(), &baseUrl, &list)) {
        if (err)
            *err = QStringLiteral("后端插件清单缺失或无法解析");
        return false;
    }
    BackendPluginInfo pick;
    bool found = false;
    for (const BackendPluginInfo &b : list) {
        if (b.id == id) {
            pick = b;
            found = true;
            break;
        }
    }
    if (!found) {
        if (err)
            *err = QStringLiteral("清单里找不到后端插件：%1").arg(id);
        return false;
    }
    // files 明细（用于解压后逐文件校验）
    QFile mf(QStringLiteral(":/sr/sr_backend_manifest.json"));
    QJsonArray filesArr;
    if (mf.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(mf.readAll());
        mf.close();
        for (const QJsonValue &v : doc.object().value(QStringLiteral("backends")).toArray()) {
            if (v.toObject().value(QStringLiteral("id")).toString() == pick.id)
                filesArr = v.toObject().value(QStringLiteral("files")).toArray();
        }
    }

    const QString statusKey = QStringLiteral("be:") + pick.id;
    // JM_SR_BACKEND_URL：目录型覆盖（推荐，自动拼 <id>.tar.gz）；若已以 .tar.gz 结尾则当直链。
    QString url = qEnvironmentVariable("JM_SR_BACKEND_URL").trimmed();
    if (!url.isEmpty()) {
        if (!url.endsWith(QStringLiteral(".tar.gz"), Qt::CaseInsensitive)) {
            if (!url.endsWith(QLatin1Char('/')))
                url += QLatin1Char('/');
            url += pick.id + QStringLiteral(".tar.gz");
        }
    } else {
        if (baseUrl.isEmpty()) {
            if (err)
                *err = QStringLiteral("后端插件清单缺少 base_url");
            return false;
        }
        url = baseUrl + pick.id + QStringLiteral(".tar.gz");
    }

    const QString tmpDir = QDir(SrPlugin::BackendPackagesDir()).filePath(QStringLiteral(".tmp_") + pick.id);
    QDir(tmpDir).removeRecursively();
    QDir().mkpath(tmpDir);
    const QString gzPath = QDir(tmpDir).filePath(pick.id + QStringLiteral(".tar.gz"));
    const QString extractDir = QDir(tmpDir).filePath(QStringLiteral("extract"));
    QDir().mkpath(extractDir);

    SetStatus(statusKey, QStringLiteral("downloading"), 0, QString());
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("JMComic-qt/sr-backend"));
    QNetworkReply *reply = nam.get(req);
    QFile part(gzPath);
    if (!part.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reply->abort();
        reply->deleteLater();
        if (err)
            *err = QStringLiteral("无法写临时文件");
        SetStatus(statusKey, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }
    QCryptographicHash sha(QCryptographicHash::Sha256);
    qint64 total = pick.archiveSize;
    qint64 received = 0, lastEmit = 0;
    bool aborted = false;
    QString errStr;
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&] {
        if (aborted)
            return;
        if (cancel && cancel()) {
            errStr = QStringLiteral("已取消");
            aborted = true;
            reply->abort();
            return;
        }
        const QByteArray chunk = reply->readAll();
        if (chunk.isEmpty())
            return;
        sha.addData(chunk);
        if (part.write(chunk) != chunk.size()) {
            errStr = QStringLiteral("写入失败");
            aborted = true;
            reply->abort();
            return;
        }
        received += chunk.size();
        if (progress && (received - lastEmit >= kProgStep || received == total)) {
            lastEmit = received;
            progress(received, total);
        }
        SetStatus(statusKey, QStringLiteral("downloading"),
                  total > 0 ? int(100.0 * received / total) : -1, QString());
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::downloadProgress, &loop, [&](qint64, qint64 t) {
        if (t > 0)
            total = t;
    });
    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, [&] {
        if (!aborted) {
            errStr = QStringLiteral("下载超时");
            aborted = true;
            reply->abort();
        }
        loop.quit();
    });
    guard.start(int(qBound<qint64>(qint64(300000),
                                   total > 0 ? total / 6 : qint64(180000),
                                   qint64(3600000))));
    loop.exec();
    // 收尾：把 finished 之前仍滞留在 reply 缓冲里的最后一段读出来（大文件尤其必要，
    // 否则 sha256 会少最后一块 → 误判校验失败）。
    if (!aborted) {
        const QByteArray tail = reply->readAll();
        if (!tail.isEmpty()) {
            sha.addData(tail);
            part.write(tail);
            received += tail.size();
        }
    }
    part.close();
    const auto netErr = reply->error();
    reply->deleteLater();
    if (aborted || netErr != QNetworkReply::NoError) {
        QDir(tmpDir).removeRecursively();
        if (err)
            *err = errStr.isEmpty() ? QStringLiteral("下载失败：%1").arg(netErr) : errStr;
        const bool cancelled = (errStr == QLatin1String("已取消"));
        SetStatus(statusKey, cancelled ? QStringLiteral("not_installed") : QStringLiteral("error"),
                  -1, err ? *err : QString());
        return false;
    }

    // 归档校验（若清单给出）
    SetStatus(statusKey, QStringLiteral("verifying"), 100, QString());
    if (!pick.archiveSha256.isEmpty()) {
        const QString got = QString::fromLatin1(sha.result().toHex());
        if (got.compare(pick.archiveSha256, Qt::CaseInsensitive) != 0) {
            QDir(tmpDir).removeRecursively();
            if (err)
                *err = QStringLiteral("归档 sha256 校验失败");
            SetStatus(statusKey, QStringLiteral("error"), -1, err ? *err : QString());
            return false;
        }
    }

    // 解压
    QString exErr;
    if (!ExtractTarGz(gzPath, extractDir, &exErr)) {
        QDir(tmpDir).removeRecursively();
        if (err)
            *err = exErr;
        SetStatus(statusKey, QStringLiteral("error"), -1, err ? *err : QString());
        return false;
    }

    // 逐文件校验（若清单给了 files）
    if (!filesArr.isEmpty()) {
        for (const QJsonValue &v : filesArr) {
            const QJsonObject o = v.toObject();
            const QString name = o.value(QStringLiteral("name")).toString();
            const QString want = o.value(QStringLiteral("sha256")).toString().toLower();
            const QString p = QDir(extractDir).filePath(name);
            if (!QFileInfo::exists(p)) {
                QDir(tmpDir).removeRecursively();
                if (err)
                    *err = QStringLiteral("解压后缺少文件：%1").arg(name);
                SetStatus(statusKey, QStringLiteral("error"), -1, err ? *err : QString());
                return false;
            }
            if (!want.isEmpty()) {
                QFile f(p);
                QCryptographicHash fh(QCryptographicHash::Sha256);
                if (f.open(QIODevice::ReadOnly)) {
                    fh.addData(f.readAll());
                    f.close();
                }
                if (QString::fromLatin1(fh.result().toHex()).compare(want, Qt::CaseInsensitive) != 0) {
                    QDir(tmpDir).removeRecursively();
                    if (err)
                        *err = QStringLiteral("文件校验失败：%1").arg(name);
                    SetStatus(statusKey, QStringLiteral("error"), -1, err ? *err : QString());
                    return false;
                }
            }
        }
    }

    // 原子落位：把 extract/ 内容并入 <data>/sr_backend/<sub>/
    const QString targetDir = QDir(SrPlugin::BackendPackagesDir()).filePath(pick.targetSubdir);
    QDir().mkpath(targetDir);
    const auto entries = QDir(extractDir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &fi : entries) {
        const QString dst = QDir(targetDir).filePath(fi.fileName());
        if (fi.isDir()) {
            QDir(dst).removeRecursively();
            QDir().rename(fi.absoluteFilePath(), dst);
        } else {
            if (QFileInfo::exists(dst))
                QFile::remove(dst);
            if (!QFile::rename(fi.absoluteFilePath(), dst)) {
                QFile::copy(fi.absoluteFilePath(), dst);
            }
        }
    }
    QDir(tmpDir).removeRecursively();
    SetStatus(statusKey, QStringLiteral("installed"), 100, QString());
    return true;
}

bool UninstallBackend(const QString &id, QString *err)
{
    QVector<BackendPluginInfo> list;
    if (!ParseBackends(LoadBackendManifest(), nullptr, &list)) {
        if (err)
            *err = QStringLiteral("后端插件清单缺失");
        return false;
    }
    BackendPluginInfo pick;
    bool found = false;
    for (const BackendPluginInfo &b : list) {
        if (b.id == id) {
            pick = b;
            found = true;
            break;
        }
    }
    if (!found) {
        if (err)
            *err = QStringLiteral("找不到后端插件：%1").arg(id);
        return false;
    }
    const QString targetDir = QDir(SrPlugin::BackendPackagesDir()).filePath(pick.targetSubdir);
    if (!DirHasFiles(targetDir)) {
        if (err)
            *err = QStringLiteral("受管目录未安装插件（%1 为空）；内置/随包副本不在此删除范围")
                       .arg(targetDir);
        return false;
    }
    if (!QDir(targetDir).removeRecursively()) {
        if (err)
            *err = QStringLiteral("删除失败（文件被占用？）：%1").arg(targetDir);
        return false;
    }
    // OpenVINO 删掉后要释放已注册的 EP（下次推理会重新探测）
    if (pick.kind == QLatin1String("binary"))
        SrEngine::UnloadRuntime();
    {
        QMutexLocker lock(&g_mu);
        g_status.remove(QStringLiteral("be:") + id);
    }
    return true;
}

}  // namespace SrPlugin
