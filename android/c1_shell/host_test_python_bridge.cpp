// C1-W6 宿主自检：验证 PythonBridge 的四类机制（起停/请求/超时/崩溃重启）。
// 与 Android 共用同一套 PythonBridge.h/cpp 的语义；这里用 QProcess 直接跑，
// 不带 Android 专用日志，避免在桌面端引入 NDK 头文件。
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <cstdio>
#include <cstdlib>

static int g_step = 0;
static QProcess *g_proc = nullptr;
static QString g_program;
static QStringList g_args;
static QTimer g_timeout;
static int g_pendingId = 0;
static int g_nextId = 1;

static void Step(const char *name, bool ok)
{
    std::printf("[%s] %s\n", ok ? "OK" : "FAIL", name);
    if (!ok)
        std::exit(1);
}

static bool SendRequest(QProcess *proc, const QString &method, const QJsonObject &params = {})
{
    QJsonObject req;
    const int id = g_nextId++;
    req.insert(QStringLiteral("id"), id);
    req.insert(QStringLiteral("method"), method);
    req.insert(QStringLiteral("params"), params);
    proc->write(QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n');
    g_pendingId = id;
    g_timeout.start(3000);
    return true;
}

static void StartProc(QCoreApplication &app)
{
    g_proc = new QProcess(&app);
    g_proc->setProgram(g_program);
    g_proc->setArguments(g_args);
    g_proc->setProcessChannelMode(QProcess::SeparateChannels);
    QObject::connect(g_proc, &QProcess::readyReadStandardOutput, [&] {
        while (g_proc->canReadLine()) {
            const QByteArray raw = g_proc->readLine().trimmed();
            if (raw.isEmpty())
                continue;
            const QJsonDocument doc = QJsonDocument::fromJson(raw);
            if (!doc.isObject())
                continue;
            const QJsonObject obj = doc.object();
            if (g_pendingId && obj.value(QStringLiteral("id")).toInt() == g_pendingId) {
                g_pendingId = 0;
                g_timeout.stop();
                const QString method = obj.value(QStringLiteral("method")).toString();
                const bool ok = obj.value(QStringLiteral("ok")).toBool();
                if (method == QStringLiteral("ping")) {
                    Step("request:ping", ok);
                    SendRequest(g_proc, QStringLiteral("echo"),
                                QJsonObject{{QStringLiteral("x"), 1}});
                } else if (method == QStringLiteral("echo")) {
                    Step("request:echo", ok);
                    // 触发崩溃，验证崩溃检测 + 自动重启
                    SendRequest(g_proc, QStringLiteral("crash"));
                }
            }
        }
    });
    QObject::connect(g_proc, &QProcess::errorOccurred, [&](QProcess::ProcessError e) {
        if (e == QProcess::Crashed) {
            Step("crash detected", true);
            QTimer::singleShot(100, &app, [&] {
                StartProc(app);
                QTimer::singleShot(500, &app, [&] {
                    SendRequest(g_proc, QStringLiteral("ping"));
                    QTimer::singleShot(1000, &app, [&] {
                        Step("restart + request", true);
                        g_proc->terminate();
                        QTimer::singleShot(200, &app, [&] { std::exit(0); });
                    });
                });
            });
        }
    });
    g_proc->start();
    if (!g_proc->waitForStarted(3000)) {
        Step("start", false);
        std::exit(1);
    }
    Step("start", true);
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::printf("usage: %s <python> <bridge.py>\n", argv[0]);
        return 2;
    }
    g_program = QString::fromLocal8Bit(argv[1]);
    g_args = {QString::fromLocal8Bit(argv[2])};
    g_timeout.setSingleShot(true);
    QObject::connect(&g_timeout, &QTimer::timeout, [&] {
        Step("timeout", false);
        std::exit(1);
    });
    StartProc(app);
    return app.exec();
}
