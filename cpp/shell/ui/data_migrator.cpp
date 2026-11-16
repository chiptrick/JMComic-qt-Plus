#include "data_migrator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QVariant>

#include "app_paths.h"
#include "session.h"

namespace {

// 与 SessionStore::Load 的 usable() 同一判据：文件存在、能解析、有内容。
// 判据三条：文件不存在=false；解析报错=false；非空文件却解析出零个键=false
//（半截写入/二进制垃圾的典型形态，QSettings 对它们是"静默读成空配置"）。
// 空文件=true（新装机的合法空配置）。
bool Parses(const QString &path)
{
    if (path.isEmpty() || !QFileInfo::exists(path))
        return false;
    if (QFileInfo(path).size() == 0)
        return true;
    QSettings probe(path, QSettings::IniFormat);
    const auto keys = probe.allKeys();   // 触发解析
    return probe.status() == QSettings::NoError && !keys.isEmpty();
}

QString NowTag()
{
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss-zzz"));
}

bool CopyFileKeepExisting(const QString &src, const QString &dst)
{
    if (!QFileInfo::exists(src))
        return false;
    if (QFileInfo::exists(dst))
        return true;   // 备份目录不覆盖：保留第一次备份的原始字节
    QDir().mkpath(QFileInfo(dst).absolutePath());
    return QFile::copy(src, dst);
}

QString SessionIniPath()
{
    // SessionStore::Path() 与 Save()/Load() 用同一解析（含 JM_SESSION_FILE 覆盖），
    // 迁移器操作的文件与应用实际读写的文件保证一致。
    return SessionStore::Path();
}

QString UiIniPath()
{
    return AppPaths::ConfigFile(QStringLiteral("ui.ini"));
}

}  // namespace

namespace DataMigrator {

void RollingBackup(const QString &iniPath)
{
    if (iniPath.isEmpty() || !QFileInfo::exists(iniPath))
        return;
    // 只有主文件解析通过才覆盖备份：绝不把坏文件滚到好备份上。
    if (!Parses(iniPath))
        return;
    const QString bak = iniPath + QStringLiteral(".bak");
    QFile::remove(bak);
    if (!QFile::copy(iniPath, bak))
        qWarning("migrate: 滚动备份失败 %s -> %s", qPrintable(iniPath), qPrintable(bak));
}

QString QuarantineIfBroken(const QString &iniPath)
{
    QString log;
    if (iniPath.isEmpty() || !QFileInfo::exists(iniPath))
        return log;
    if (Parses(iniPath))
        return log;

    const QString corrupt = iniPath + QStringLiteral(".corrupt-") + NowTag();
    if (!QFile::rename(iniPath, corrupt)) {
        qWarning("migrate: %s 解析失败且无法隔离（rename 失败），保持原样",
                 qPrintable(iniPath));
        return log;
    }
    log += QStringLiteral("migrate: 损坏隔离 %1 -> %2\n")
               .arg(QFileInfo(iniPath).fileName(), QFileInfo(corrupt).fileName());

    const QString bak = iniPath + QStringLiteral(".bak");
    if (Parses(bak)) {
        if (QFile::copy(bak, iniPath)) {
            log += QStringLiteral("migrate: 已从备份恢复 %1\n").arg(QFileInfo(bak).fileName());
        } else {
            qWarning("migrate: 从备份恢复失败 %s", qPrintable(bak));
        }
    } else {
        log += QStringLiteral("migrate: 无可用备份，%1 按空配置处理\n")
                   .arg(QFileInfo(iniPath).fileName());
    }
    return log;
}

int SchemaVersion()
{
    const QString ini = UiIniPath();
    if (ini.isEmpty() || !QFileInfo::exists(ini))
        return 1;
    QSettings s(ini, QSettings::IniFormat);
    bool ok = false;
    const int v = s.value(QStringLiteral("meta/schemaVersion"), 1).toInt(&ok);
    return (ok && v > 0) ? v : 1;
}

namespace {

// v1 -> v2（0.95 及更早没有 schemaVersion，一律视为 v1）。
// 目前唯一的键级差异：proxy/http3（旧）与 proxy/http3On（新）双写已由
// ProxyConfig::Save 保持，这里只兜底"旧版本只写了旧键"的场景。
// 后续键改名/目录搬迁都追加 v<n>->v<n+1> 步骤，不要改写旧步骤。
// session.ini 的 v1 缺 uid/signedToday 属于合法旧数据：SessionStore::Load 的
// 缺省值语义已经是"缺则 0/false"，不需要搬键。
bool MigrateV1ToV2(QSettings &ui)
{
    if (ui.contains(QStringLiteral("proxy/http3")) &&
        !ui.contains(QStringLiteral("proxy/http3On")))
        ui.setValue(QStringLiteral("proxy/http3On"),
                    ui.value(QStringLiteral("proxy/http3")));
    return true;
}

}  // namespace

void Run()
{
    const QString uiIni = UiIniPath();
    const QString sessionIni = SessionIniPath();
    if (uiIni.isEmpty() && sessionIni.isEmpty()) {
        qWarning("migrate: 没有可写的配置目录，跳过迁移检查");
        return;
    }

    // 1) 损坏隔离（在备份之前：坏文件不进备份，恢复出来的好文件才值得备）
    const QString q1 = QuarantineIfBroken(uiIni);
    const QString q2 = QuarantineIfBroken(sessionIni);
    if (!q1.isEmpty())
        qWarning("%s", q1.trimmed().toUtf8().constData());
    if (!q2.isEmpty())
        qWarning("%s", q2.trimmed().toUtf8().constData());

    // 2) 全新安装：两份配置都不存在，无迁移可言，也不写 schema（等首次 Save 自然生成）
    if (!QFileInfo::exists(uiIni) && !QFileInfo::exists(sessionIni))
        return;

    const int from = SchemaVersion();
    if (from > kCurrentSchema) {
        qWarning("migrate: 配置 schema=%d 高于本程序支持的 %d（降级运行），不做迁移",
                 from, kCurrentSchema);
        return;
    }
    if (from == kCurrentSchema)
        return;

    // 3) 升级前备份：原始字节进 backup/v<from>/，成功与否都不动原文件
    const QString backupDir =
        QDir(AppPaths::ConfigDir()).filePath(QStringLiteral("backup/v%1").arg(from));
    int copied = 0;
    for (const QString &name : {QStringLiteral("ui.ini"), QStringLiteral("session.ini")}) {
        const QString p = (name == QStringLiteral("ui.ini")) ? uiIni : sessionIni;
        if (CopyFileKeepExisting(p, QDir(backupDir).filePath(name)))
            ++copied;
        const QString bak = p + QStringLiteral(".bak");
        if (CopyFileKeepExisting(bak, QDir(backupDir).filePath(name + QStringLiteral(".bak"))))
            ++copied;
    }
    qWarning("migrate: 升级备份 schema=%d -> %s（%d 个文件）", from, qPrintable(backupDir),
             copied);

    // 4) 顺序执行迁移步骤；任一步失败不提升版本号，下次启动重试
    bool ok = true;
    int to = from;
    {
        QSettings ui(uiIni, QSettings::IniFormat);
        if (from <= 1 && kCurrentSchema >= 2) {
            if (MigrateV1ToV2(ui)) {
                to = 2;
            } else {
                ok = false;
            }
        }
        // v2->v3 ... 在这里追加
        if (ok) {
            ui.setValue(QStringLiteral("meta/schemaVersion"), to);
            ui.setValue(QStringLiteral("meta/lastWriteVersion"),
                        QCoreApplication::applicationVersion());
            ui.sync();
            if (ui.status() != QSettings::NoError) {
                qWarning("migrate: 写入 schemaVersion 失败 status=%d（下次启动重试）",
                         int(ui.status()));
                ok = false;
            }
        }
    }
    if (ok)
        qWarning("migrate: v%d->v%d 完成", from, to);
    else
        qWarning("migrate: v%d->v%d 失败，下次启动重试", from, to);
}

QString ExportData(const QString &dirPath)
{
    if (dirPath.isEmpty())
        return QStringLiteral("目标目录为空");
    QDir out(dirPath);
    if (!out.exists() && !QDir().mkpath(dirPath))
        return QStringLiteral("无法创建目录 ") + dirPath;

    QStringList copied, missing;
    for (const QString &name :
         {QStringLiteral("session.ini"), QStringLiteral("ui.ini")}) {
        const QString p = (name == QStringLiteral("ui.ini")) ? UiIniPath() : SessionIniPath();
        if (p.isEmpty() || !QFileInfo::exists(p)) {
            missing << name;
            continue;
        }
        if (!QFile::copy(p, out.filePath(name)))
            return QStringLiteral("复制失败: ") + p;
        copied << name;
        const QString bak = p + QStringLiteral(".bak");
        if (QFileInfo::exists(bak) && !QFile::copy(bak, out.filePath(name + QStringLiteral(".bak"))))
            return QStringLiteral("复制失败: ") + bak;
    }
    if (copied.isEmpty())
        return QStringLiteral("没有可导出的配置文件（%1 / %2 均不存在）")
            .arg(QStringLiteral("session.ini"), QStringLiteral("ui.ini"));
    qWarning("export-data: ok %s（缺失: %s）",
             copied.join(QLatin1Char(',')).toUtf8().constData(),
             missing.isEmpty() ? QStringLiteral("-").toUtf8().constData()
                               : missing.join(QLatin1Char(',')).toUtf8().constData());
    return QString();
}

QString ImportData(const QString &dirPath)
{
    if (dirPath.isEmpty())
        return QStringLiteral("来源目录为空");
    QDir in(dirPath);
    if (!in.exists())
        return QStringLiteral("目录不存在: ") + dirPath;

    bool any = false;
    for (const QString &name :
         {QStringLiteral("session.ini"), QStringLiteral("ui.ini")}) {
        const QString src = in.filePath(name);
        if (!QFileInfo::exists(src))
            continue;
        const QString dst = (name == QStringLiteral("ui.ini")) ? UiIniPath() : SessionIniPath();
        if (dst.isEmpty())
            return QStringLiteral("配置目录不可写，无法导入 ") + name;
        // 导入前的现场滚动进 .bak：导入内容不合意时还能退回
        RollingBackup(dst);
        if (!QFile::remove(dst) && QFileInfo::exists(dst))
            return QStringLiteral("无法替换现有文件: ") + dst;
        if (!QFile::copy(src, dst))
            return QStringLiteral("导入失败: ") + src;
        any = true;
        qWarning("import-data: %s -> %s", qPrintable(src), qPrintable(dst));
    }
    if (!any)
        return QStringLiteral("目录里没有 session.ini / ui.ini: ") + dirPath;
    return QString();
}

}  // namespace DataMigrator
