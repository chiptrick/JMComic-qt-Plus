#include "cache_path.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include "app_paths.h"
#include "data_migrator.h"

namespace {

QString EnvRoot()
{
    return qEnvironmentVariable("JM_CACHE_DIR").trimmed();
}

QString ConfiguredRoot()
{
    QSettings s(CachePath::ConfigFile(), QSettings::IniFormat);
    return s.value(QStringLiteral("cache/root")).toString().trimmed();
}

// 返回 true 表示目录可用（存在或已创建）
bool EnsureDir(const QString &path)
{
    if (path.isEmpty())
        return false;
    if (QDir(path).exists())
        return true;
    return QDir().mkpath(path);
}

}  // namespace

namespace CachePath {

QString UiIniPath()
{
    // 与 session.ini 同目录，统一由 AppPaths 解析（Android 上不再退回只读的 appdir）。
    static const QString path = [] {
        AppPaths::MigrateLegacyFile(QStringLiteral("ui.ini"));
        return AppPaths::ConfigFile(QStringLiteral("ui.ini"));
    }();
    return path;
}

QString ConfigFile()
{
    return UiIniPath();
}

QString DefaultRoot()
{
    return AppPaths::CacheDir();
}

QString Root()
{
    const QString env = EnvRoot();
    if (!env.isEmpty()) {
        EnsureDir(env);
        return env;
    }
    const QString cfg = ConfiguredRoot();
    if (!cfg.isEmpty() && EnsureDir(cfg))
        return cfg;
    const QString def = DefaultRoot();
    EnsureDir(def);
    return def;
}

bool IsCustom()
{
    if (!EnvRoot().isEmpty())
        return true;
    const QString cfg = ConfiguredRoot();
    return !cfg.isEmpty();
}

bool SetRoot(const QString &path)
{
    const QString clean = path.trimmed();
    if (clean.isEmpty()) {
    QSettings s(ConfigFile(), QSettings::IniFormat);
    s.remove(QStringLiteral("cache/root"));
    s.sync();
    if (s.status() == QSettings::NoError)
        DataMigrator::RollingBackup(ConfigFile());
    qInfo("cache-path: reset to default %s", qPrintable(DefaultRoot()));
        return EnsureDir(DefaultRoot());
    }
    if (!EnsureDir(clean)) {
        qWarning("cache-path: cannot create %s", qPrintable(clean));
        return false;
    }
    QSettings s(ConfigFile(), QSettings::IniFormat);
    s.setValue(QStringLiteral("cache/root"), QDir::cleanPath(clean));
    s.sync();
    if (s.status() == QSettings::NoError)
        DataMigrator::RollingBackup(ConfigFile());
    qInfo("cache-path: set %s", qPrintable(clean));
    return true;
}

QString CoverDir()
{
    const QString dir = QDir(Root()).filePath(QStringLiteral("covers"));
    EnsureDir(dir);
    return dir;
}

QString PageDir()
{
    const QString dir = QDir(Root()).filePath(QStringLiteral("pages"));
    EnsureDir(dir);
    return dir;
}

QString StatusLine()
{
    return QStringLiteral("cache-path: root=%1 custom=%2 covers=%3 pages=%4")
        .arg(Root())
        .arg(int(IsCustom()))
        .arg(CoverDir())
        .arg(PageDir());
}

}  // namespace CachePath
