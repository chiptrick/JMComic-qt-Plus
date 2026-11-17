#include "app_paths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryFile>

#ifdef Q_OS_ANDROID
#  include <QJniObject>
#endif

namespace {

// Android：应用外部私有目录 /storage/emulated/0/Android/data/<pkg>/files。
// 用户用文件管理器/adb pull 能直接访问；/data/user/0/<pkg>/files 普通用户看不到
// （root 才进得去）。用 getExternalFilesDir(null).getAbsolutePath() 拿到的就是它，
// 且不需要任何存储权限（属于应用自身目录）。非 Android 返回空串。
QString AndroidExternalFilesDir()
{
#ifdef Q_OS_ANDROID
    QJniObject activity = QJniObject::callStaticObjectMethod(
        "org/qtproject/qt/android/QtNative", "activity", "()Landroid/app/Activity;");
    if (!activity.isValid())
        return QString();
    QJniObject dir = activity.callObjectMethod(
        "getExternalFilesDir", "(Ljava/lang/String;)Ljava/io/File;", nullptr);
    if (!dir.isValid())
        return QString();
    const QString path = dir.callObjectMethod("getAbsolutePath", "()Ljava/lang/String;").toString();
    return path.startsWith(QStringLiteral("/")) ? path : QString();
#else
    return QString();
#endif
}

QString EnvOverride()
{
    return qEnvironmentVariable("JM_UI_CONFIG").trimmed();
}

// 实测目录可写：mkpath 成功后写一个探针文件。只读挂载 / 无权限都会在这里暴露，
// 而 QFileInfo::isWritable() 在部分 Android ROM 上会给出错误结论。
bool ProbeWritable(const QString &dir)
{
    if (dir.isEmpty())
        return false;
    if (!QDir(dir).exists() && !QDir().mkpath(dir))
        return false;
    QTemporaryFile probe(QDir(dir).filePath(QStringLiteral(".jm_write_probe_XXXXXX")));
    if (!probe.open())
        return false;
    probe.write("ok", 2);
    probe.close();
    return true;
}

QStringList ConfigCandidates()
{
    QStringList list;
    // 1) 显式覆盖：回归/多实例隔离（原 JM_UI_CONFIG 语义保持不变）
    const QString env = EnvOverride();
    if (!env.isEmpty())
        list << env;

    const QString appConfig = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (!appConfig.isEmpty())
        list << appConfig;

    const QString localData = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (!localData.isEmpty())
        list << QDir(localData).filePath(QStringLiteral("settings"));

    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!appData.isEmpty())
        list << QDir(appData).filePath(QStringLiteral("settings"));

#ifdef Q_OS_ANDROID
    // 4) Qt 在 Android 上把 HOME 设成 /data/user/0/<pkg>/files。
    //    **绝不**退回 applicationDirPath()：那是只读的 lib 目录，且每次安装都变。
    const QString home = qEnvironmentVariable("HOME").trimmed();
    if (!home.isEmpty())
        list << QDir(home).filePath(QStringLiteral("settings")) << home;
#else
    // 5) 桌面兜底：程序目录下的 config/（开发机绿色版习惯）
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty())
        list << QDir(appDir).filePath(QStringLiteral("config"));
#endif
    list.removeDuplicates();
    return list;
}

}  // namespace

namespace AppPaths {

QString ConfigDir()
{
    static const QString dir = [] {
        const QStringList cands = ConfigCandidates();
        for (const QString &c : cands) {
            if (ProbeWritable(c))
                return QDir(c).absolutePath();
        }
        qWarning("app-path: 没有可写的配置目录（候选 %d 个），配置无法持久化",
                 int(cands.size()));
        return QString();
    }();
    return dir;
}

QString ConfigFile(const QString &name)
{
    const QString dir = ConfigDir();
    if (dir.isEmpty())
        return QString();
    return QDir(dir).filePath(name);
}

QString DataDir()
{
#ifdef Q_OS_ANDROID
    // 优先外部私有目录：用户能直接访问（文件管理器 / adb pull），不需要权限。
    // 拿不到再退回 QStandardPaths（内部 /data/user/0/<pkg>/files，普通用户不可见）。
    const QString ext = AndroidExternalFilesDir();
    if (!ext.isEmpty() && ProbeWritable(ext)) {
        QDir().mkpath(ext);
        return ext;
    }
#endif
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty()) {
        const QString home = qEnvironmentVariable("HOME").trimmed();
        if (!home.isEmpty())
            base = home;
    }
    if (base.isEmpty())
        base = QDir::tempPath();
    QDir().mkpath(base);
    return base;
}

QString CacheDir()
{
    return QDir(DataDir()).filePath(QStringLiteral("cache"));
}

bool IsWritable(const QString &dir)
{
    return ProbeWritable(dir);
}

bool MigrateLegacyFile(const QString &name)
{
    // 显式指定 JM_UI_CONFIG 的场景是回归/多实例隔离：那是"干净环境"的语义，
    // 把用户目录里的旧配置拷进来会污染断言（上一轮实测就出现过 loggedIn=1 的假阳性）。
    if (!EnvOverride().isEmpty())
        return false;

    const QString target = ConfigFile(name);
    if (target.isEmpty() || QFileInfo::exists(target))
        return false;

    QStringList legacy;
#ifndef Q_OS_ANDROID
    legacy << QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config") + QLatin1Char('/') + name);
#endif
    // Qt 把 ConfigLocation 与 AppConfigLocation 分开过，旧版本可能落在通用配置目录。
    const QString generic = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (!generic.isEmpty())
        legacy << QDir(generic).filePath(QStringLiteral("JMComic/JMComic/") + name);

    for (const QString &src : legacy) {
        if (!QFileInfo::exists(src))
            continue;
        QDir().mkpath(QFileInfo(target).absolutePath());
        if (QFile::copy(src, target)) {
            qWarning("app-path: 迁移旧配置 %s -> %s", qPrintable(src), qPrintable(target));
            return true;
        }
    }
    return false;
}

QString Report()
{
    const auto loc = [](QStandardPaths::StandardLocation t) {
        return QStandardPaths::writableLocation(t);
    };
    const QString appConfig = loc(QStandardPaths::AppConfigLocation);
    const QString localData = loc(QStandardPaths::AppLocalDataLocation);
    const QString appData = loc(QStandardPaths::AppDataLocation);
    const QString cache = loc(QStandardPaths::CacheLocation);

    const QString dir = ConfigDir();
    QStringList lines;
    lines << QStringLiteral("path-report: appName=%1 org=%2 ver=%3")
                 .arg(QCoreApplication::applicationName(),
                      QCoreApplication::organizationName(),
                      QCoreApplication::applicationVersion());
    lines << QStringLiteral("path-report: appDir=%1").arg(QCoreApplication::applicationDirPath());
    lines << QStringLiteral("path-report: home=%1").arg(qEnvironmentVariable("HOME"));
    lines << QStringLiteral("path-report: AppConfigLocation=%1 writable=%2")
                 .arg(appConfig.isEmpty() ? QStringLiteral("<empty>") : appConfig,
                      appConfig.isEmpty() ? QStringLiteral("no") : (IsWritable(appConfig) ? QStringLiteral("yes") : QStringLiteral("no")));
    lines << QStringLiteral("path-report: AppLocalDataLocation=%1").arg(localData.isEmpty() ? QStringLiteral("<empty>") : localData);
    lines << QStringLiteral("path-report: AppDataLocation=%1").arg(appData.isEmpty() ? QStringLiteral("<empty>") : appData);
    lines << QStringLiteral("path-report: CacheLocation=%1").arg(cache.isEmpty() ? QStringLiteral("<empty>") : cache);
    lines << QStringLiteral("path-report: JM_UI_CONFIG=%1").arg(qEnvironmentVariable("JM_UI_CONFIG"));
    lines << QStringLiteral("path-report: configDir=%1").arg(dir.isEmpty() ? QStringLiteral("<none>") : dir);
#ifdef Q_OS_ANDROID
    lines << QStringLiteral("path-report: androidExternalFiles=%1")
                 .arg(AndroidExternalFilesDir().isEmpty() ? QStringLiteral("<none>")
                                                          : AndroidExternalFilesDir());
#endif
    lines << QStringLiteral("path-report: dataDir=%1 writable=%2")
                 .arg(DataDir(), IsWritable(DataDir()) ? QStringLiteral("yes") : QStringLiteral("no"));
    lines << QStringLiteral("path-report: cacheDir=%1").arg(CacheDir());

    const QStringList files = {QStringLiteral("session.ini"), QStringLiteral("ui.ini")};
    for (const QString &f : files) {
        const QString p = ConfigFile(f);
        const QFileInfo fi(p);
        lines << QStringLiteral("path-report: %1=%2 exists=%3 size=%4 mtime=%5")
                     .arg(f,
                          p.isEmpty() ? QStringLiteral("<none>") : p,
                          fi.exists() ? QStringLiteral("1") : QStringLiteral("0"))
                     .arg(fi.exists() ? fi.size() : 0)
                     .arg(fi.exists() ? fi.lastModified().toString(Qt::ISODate) : QStringLiteral("-"));
    }
    return lines.join(QLatin1Char('\n'));
}

}  // namespace AppPaths
