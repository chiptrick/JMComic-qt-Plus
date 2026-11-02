#include "pyruntime.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "C1PyRuntime", __VA_ARGS__)

namespace PyRuntime {
namespace {

QString AppFilesDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

// APK 里 extractNativeLibs=false，nativeLibraryDir 里没有可执行文件；
// Python 运行时统一打进 assets/，第一次用时复制到应用私有目录。
bool CopyAsset(const QString &name, const QString &dst)
{
    static const QStringList bases = {
        QStringLiteral("assets:/"), QStringLiteral("assets:/pylibs/")};
    for (const QString &base : bases) {
        const QString src = base + name;
        if (QFile::exists(src))
            return QFile::copy(src, dst);
    }
    return false;
}

bool EnsureExecutable(const QString &path)
{
    QFile f(path);
    if (!f.exists())
        return false;
    // chmod 755；QFile 不暴露权限接口，走 toybox
    return QProcess::execute(QStringLiteral("/system/bin/chmod"),
                             {QStringLiteral("755"), path}) == 0;
}

}  // namespace

bool EnsureReady(QString *pyBin, QString *err)
{
    const QString filesDir = AppFilesDir();
    const QString binDir = filesDir + QStringLiteral("/pybin");
    const QString bundleRoot = filesDir + QStringLiteral("/pybundle");
    const QString bin = binDir + QStringLiteral("/libpythonbin.so");
    const QString stdlib = bundleRoot + QStringLiteral("/_python_bundle/stdlib.zip");

    QDir().mkpath(binDir);
    QDir().mkpath(bundleRoot);

    if (!QFile::exists(bin)) {
        for (const QString &name : {QStringLiteral("libpythonbin.so"),
                                    QStringLiteral("libpython3.14.so"),
                                    QStringLiteral("libsqlite3.so"),
                                    QStringLiteral("libffi.so"),
                                    QStringLiteral("libcrypto.so"),
                                    QStringLiteral("libssl.so")}) {
            const QString dst = binDir + QLatin1Char('/') + name;
            if (!QFile::exists(dst) && !CopyAsset(name, dst)) {
                if (err) *err = QStringLiteral("copy %1 failed").arg(name);
                return false;
            }
        }
        if (!EnsureExecutable(bin)) {
            if (err) *err = QStringLiteral("chmod libpythonbin failed");
            return false;
        }
    }

    if (!QFile::exists(stdlib)) {
        // 上次解包失败可能留下不可读目录；先清空再解，避免半包状态
        QDir(bundleRoot).removeRecursively();
        QDir().mkpath(bundleRoot);
        const QString tmp = filesDir + QStringLiteral("/libpybundle.tmp");
        if (!QFile::exists(tmp) && !CopyAsset(QStringLiteral("libpybundle.so"), tmp)) {
            if (err) *err = QStringLiteral("copy libpybundle failed");
            return false;
        }
        LOGI("bundle tmp size=%lld", (long long)QFileInfo(tmp).size());
        // toybox tar 在 Android 15 上可用；libpybundle.so 实际是 tar.gz 容器
        const int rc = QProcess::execute(QStringLiteral("/system/bin/tar"),
                                         {QStringLiteral("-xzf"), tmp,
                                          QStringLiteral("-C"), bundleRoot,
                                          QStringLiteral("-o")});
        QFile::remove(tmp);
        if (rc != 0 || !QFile::exists(stdlib)) {
            if (err) *err = QStringLiteral("unpack libpybundle failed rc=%1").arg(rc);
            return false;
        }
        LOGI("unpacked python bundle to %s", qPrintable(bundleRoot));
    }

    qputenv("PYTHONPATH",
            QStringLiteral("%1:%2:%3")
                .arg(bundleRoot + QStringLiteral("/_python_bundle/stdlib.zip"),
                     bundleRoot + QStringLiteral("/_python_bundle/modules"),
                     bundleRoot + QStringLiteral("/_python_bundle/site-packages"))
                .toUtf8());
    qputenv("LD_LIBRARY_PATH", binDir.toUtf8());
    if (pyBin) *pyBin = bin;
    return true;
}

}  // namespace PyRuntime
