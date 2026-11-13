#include "ui_log.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QMutex>
#include <QStandardPaths>
#include <QTextStream>

#include <cstdio>

#ifdef Q_OS_ANDROID
#include <android/log.h>
#endif

namespace {

QMutex g_mutex;
QString g_path;

const char *LevelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return "D";
    case QtInfoMsg:
        return "I";
    case QtWarningMsg:
        return "W";
    case QtCriticalMsg:
        return "E";
    case QtFatalMsg:
        return "F";
    }
    return "?";
}

void Handler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
#ifdef Q_OS_ANDROID
    // Android：直接进 logcat（tag=jmcomic），与 Qt 默认行为一致但带统一前缀
    __android_log_print(type == QtFatalMsg ? ANDROID_LOG_FATAL
                                               : (type == QtWarningMsg ? ANDROID_LOG_WARN
                                                                       : ANDROID_LOG_INFO),
                        "jmcomic", "%s", qPrintable(msg));
#else
    const QByteArray line = QStringLiteral("%1 %2 %3\n")
                                .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                                     QString::fromLatin1(LevelName(type)), msg)
                                .toUtf8();
    {
        QMutexLocker lock(&g_mutex);
        if (!g_path.isEmpty()) {
            QFile f(g_path);
            if (f.open(QIODevice::Append | QIODevice::Text)) {
                f.write(line);
                f.close();
            }
        }
        std::fwrite(line.constData(), 1, size_t(line.size()), stderr);
        std::fflush(stderr);
    }
#endif
    if (type == QtFatalMsg)
        abort();
}

}  // namespace

namespace JmLog {

void Init()
{
    static bool installed = false;
    if (installed)
        return;
    installed = true;

#ifndef Q_OS_ANDROID
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    dir = QDir(dir).filePath(QStringLiteral("logs"));
    QDir().mkpath(dir);
    g_path = QDir(dir).filePath(
        QStringLiteral("ui-%1.log").arg(QDate::currentDate().toString(QStringLiteral("yyyyMMdd"))));
#endif
    qInstallMessageHandler(Handler);
}

QString Path()
{
    return g_path;
}

}  // namespace JmLog
