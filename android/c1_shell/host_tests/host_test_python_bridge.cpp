// C1-P4-4 宿主闭环：直接链接 python_bridge.cpp（与 Android 同一份实现），
// 覆盖 start / ping / echo / net.request(200) / net.request(404) / net.request(超时)
// / net.status / crash + 自动重启 / net.close。
//
// 与旧版（已删）的区别：
//   * 旧版自己重写了一套 QProcess 逻辑，测的不是产品代码；
//   * 旧版按响应里的 `method` 分支，而旧协议根本不回 method —— 那段分支永远走不到，
//     于是"桌面闭环已验证"这个结论站不住。协议 v1 固定回带 method，这里直接断言它。
//
// 真实请求打的是本进程内起的 QTcpServer（127.0.0.1 随机端口），不依赖外网：
// P4 的判据是"一次真实请求返回预期结果"，用本地服务端才能确定性复现。
#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

#include "python_bridge.h"

namespace {

int g_pass = 0;
int g_fail = 0;
QString g_currentStep;
QTimer g_guard;

void Check(const QString &name, bool ok, const QString &detail = QString())
{
    if (ok) {
        ++g_pass;
        std::printf("[PASS] %s\n", name.toUtf8().constData());
    } else {
        ++g_fail;
        std::printf("[FAIL] %s  <- %s\n", name.toUtf8().constData(),
                    detail.toUtf8().constData());
    }
    std::fflush(stdout);
}

// ---- 本地 HTTP 服务：/ok -> 200、/slow -> 延迟 1.5s、其它 -> 404 ----
class CannedServer : public QObject
{
public:
    bool Start()
    {
        connect(&server_, &QTcpServer::newConnection, this, &CannedServer::OnNewConnection);
        return server_.listen(QHostAddress::LocalHost, 0);
    }
    QString BaseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort());
    }

private:
    void OnNewConnection()
    {
        while (QTcpSocket *sock = server_.nextPendingConnection()) {
            connect(sock, &QTcpSocket::readyRead, this, [this, sock] { OnRead(sock); });
            connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        }
    }

    void OnRead(QTcpSocket *sock)
    {
        const QByteArray req = sock->readAll();
        if (!req.contains("\r\n\r\n"))
            return;  // 请求还没收全
        const QList<QByteArray> parts = req.split(' ');
        const QString path = (parts.size() > 1) ? QString::fromUtf8(parts.at(1)) : QString();
        if (path == QLatin1String("/ok"))
            Reply(sock, 200, "hello-c1");
        else if (path == QLatin1String("/slow"))
            QTimer::singleShot(1500, sock, [this, sock] { Reply(sock, 200, "slow"); });
        else
            Reply(sock, 404, "not found");
    }

    void Reply(QTcpSocket *sock, int code, const QByteArray &body)
    {
        if (sock->state() != QAbstractSocket::ConnectedState)
            return;
        const QByteArray head = "HTTP/1.1 " + QByteArray::number(code) + " X\r\n"
                                "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                                "Connection: close\r\n\r\n";
        sock->write(head + body);
        sock->flush();
        sock->disconnectFromHost();
    }

    QTcpServer server_;
};

struct Case
{
    QString name;
    QString expectMethod;
    std::function<void()> action;
    std::function<bool(const QJsonObject &)> check;
};

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::printf("usage: %s <python-exe> <bridge.py>\n", argv[0]);
        return 2;
    }
    const QString python = QString::fromLocal8Bit(argv[1]);
    const QString script = QString::fromLocal8Bit(argv[2]);

    CannedServer server;
    if (!server.Start()) {
        std::printf("[FAIL] local http server listen failed\n");
        return 2;
    }
    const QString base = server.BaseUrl();
    std::printf("local http server: %s\n", base.toUtf8().constData());

    PythonBridge bridge;
    int crashes = 0;
    QObject::connect(&bridge, &PythonBridge::Crashed, [&] { ++crashes; });
    QObject::connect(&bridge, &PythonBridge::Error, [&](const QString &text) {
        std::printf("       error: %s\n", text.toUtf8().constData());
    });

    QVector<Case> cases;
    int index = 0;
    bool crashPhaseActive = false;
    std::function<void()> next;
    std::function<void()> crashPhase;

    Case ping;
    ping.name = QStringLiteral("ping 回 pong 且回带 method");
    ping.expectMethod = QStringLiteral("ping");
    ping.action = [&] { bridge.SendRequest(QStringLiteral("ping"), {}, 5000); };
    ping.check = [](const QJsonObject &o) {
        return o.value(QStringLiteral("ok")).toBool()
               && o.value(QStringLiteral("result")).toString() == QLatin1String("pong");
    };

    Case echo;
    echo.name = QStringLiteral("echo 原样返回 params");
    echo.expectMethod = QStringLiteral("echo");
    echo.action = [&] {
        bridge.SendRequest(QStringLiteral("echo"), {{QStringLiteral("x"), 1}}, 5000);
    };
    echo.check = [](const QJsonObject &o) {
        return o.value(QStringLiteral("ok")).toBool()
               && o.value(QStringLiteral("result")).toObject()
                          .value(QStringLiteral("x")).toInt() == 1;
    };

    Case netOk;
    netOk.name = QStringLiteral("net.request 200 + 一次成功");
    netOk.expectMethod = QStringLiteral("net.request");
    netOk.action = [&] { bridge.NetRequest(QStringLiteral("GET"), base + QStringLiteral("/ok")); };
    netOk.check = [](const QJsonObject &o) {
        const QJsonObject r = o.value(QStringLiteral("result")).toObject();
        return o.value(QStringLiteral("ok")).toBool()
               && r.value(QStringLiteral("http_status")).toInt() == 200
               && r.value(QStringLiteral("body")).toString() == QLatin1String("hello-c1")
               && r.value(QStringLiteral("attempts")).toInt() == 1;
    };

    Case net404;
    net404.name = QStringLiteral("net.request 404 视为拿到响应");
    net404.expectMethod = QStringLiteral("net.request");
    net404.action = [&] { bridge.NetRequest(QStringLiteral("GET"), base + QStringLiteral("/nf")); };
    net404.check = [](const QJsonObject &o) {
        const QJsonObject r = o.value(QStringLiteral("result")).toObject();
        return o.value(QStringLiteral("ok")).toBool()
               && r.value(QStringLiteral("http_status")).toInt() == 404
               && r.value(QStringLiteral("http_error")).toBool();
    };

    Case netSlow;
    netSlow.name = QStringLiteral("net.request 超时 -> TimeOut");
    netSlow.expectMethod = QStringLiteral("net.request");
    netSlow.action = [&] {
        bridge.NetRequest(QStringLiteral("GET"), base + QStringLiteral("/slow"), {}, 1200);
    };
    netSlow.check = [](const QJsonObject &o) {
        const QJsonObject e = o.value(QStringLiteral("error")).toObject();
        return !o.value(QStringLiteral("ok")).toBool()
               && e.value(QStringLiteral("code")).toString() == QLatin1String("TimeOut");
    };

    Case status;
    status.name = QStringLiteral("net.status 自述");
    status.expectMethod = QStringLiteral("net.status");
    status.action = [&] { bridge.NetStatus(); };
    status.check = [](const QJsonObject &o) {
        const QJsonObject r = o.value(QStringLiteral("result")).toObject();
        return o.value(QStringLiteral("ok")).toBool()
               && r.value(QStringLiteral("backend")).toString() == QLatin1String("stdlib")
               && r.contains(QStringLiteral("rss_kb"));
    };

    cases << ping << echo << netOk << net404 << netSlow << status;

    QObject::connect(&bridge, &PythonBridge::Response, [&](const QJsonObject &obj) {
        g_guard.stop();
        // crash 阶段由它自己的处理器接管，这里不再按用例表判定
        if (crashPhaseActive)
            return;
        if (index >= cases.size()) {
            Check(QStringLiteral("响应落在预期之外"), false,
                  QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
            return;
        }
        const Case &c = cases.at(index);
        const QString method = obj.value(QStringLiteral("method")).toString();
        Check(c.name + QStringLiteral(" / method 回带"), method == c.expectMethod,
              QStringLiteral("got=%1 want=%2").arg(method, c.expectMethod));
        Check(c.name, c.check(obj),
              QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact)));
        ++index;
        QTimer::singleShot(0, &app, next);
    });

    crashPhase = [&] {
        std::printf("[CASE] crash + 自动重启\n");
        crashPhaseActive = true;
        const int before = crashes;
        g_currentStep = QStringLiteral("crash");
        g_guard.start(20000);
        bridge.SendRequest(QStringLiteral("crash"), {}, 5000);
        QTimer::singleShot(1500, &app, [&, before] {
            g_guard.stop();
            Check(QStringLiteral("crash 被检测到"), crashes > before,
                  QStringLiteral("crashes=%1").arg(crashes));
            Check(QStringLiteral("crash 后自动重启"), bridge.IsRunning(),
                  bridge.GetLastError());
            // 注意：Qt 的 UniqueConnection 只对成员函数指针有效，对 lambda 在 Debug 版
            // 会直接断言中止；这里用一次性标志代替。
            auto done = std::make_shared<bool>(false);
            QObject::connect(&bridge, &PythonBridge::Response, &bridge,
                             [&, done](const QJsonObject &obj) {
                                 if (*done)
                                     return;
                                 if (obj.value(QStringLiteral("method")).toString()
                                     != QLatin1String("ping"))
                                     return;
                                 *done = true;
                                 Check(QStringLiteral("重启后 ping 可用"),
                                       obj.value(QStringLiteral("ok")).toBool());
                                 QTimer::singleShot(100, &app, [&] {
                                     bridge.NetClose();
                                     QTimer::singleShot(1200, &app, [&] {
                                         Check(QStringLiteral("net.close 后进程退出"),
                                               !bridge.IsRunning());
                                         std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
                                         std::exit(g_fail == 0 ? 0 : 1);
                                     });
                                 });
                             });
            QTimer::singleShot(400, &app, [&] {
                bridge.SendRequest(QStringLiteral("ping"), {}, 5000);
            });
        });
    };

    next = [&] {
        if (index >= cases.size()) {
            crashPhase();
            return;
        }
        const Case &c = cases.at(index);
        std::printf("[CASE] %s\n", c.name.toUtf8().constData());
        g_currentStep = c.name;
        g_guard.start(20000);
        c.action();
    };

    QObject::connect(&g_guard, &QTimer::timeout, [&] {
        Check(QStringLiteral("步骤超时: ") + g_currentStep, false);
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        std::exit(1);
    });

    QTimer::singleShot(0, &app, [&] {
        const bool ok = bridge.Start(python, {script});
        Check(QStringLiteral("start"), ok, bridge.GetLastError());
        QTimer::singleShot(800, &app, [&] { next(); });
    });

    return app.exec();
}
