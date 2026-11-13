// C1 网络层宿主闭环：直接链接 net_client.cpp（与 Android 同一份实现），
// 覆盖 200 / 404 / 超时 / 截断 / 非法 URL / 连接失败 + 重试次数 / 单通道 / POST。
//
// 为什么用本地 QTcpServer 而不是打外网：判据是"一次真实请求返回预期结果"，
// 只有本地服务端才能确定性地复现 200/404/慢响应/连接拒绝这些分支；
// 打外网会把"网络是否可达"混进结论里。
#include <QCoreApplication>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVector>

#include <cstdio>
#include <cstdlib>
#include <functional>

#include "net_client.h"

namespace {

int g_pass = 0;
int g_fail = 0;
QString g_step;
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

// /ok -> 200 hello-c1；/nf -> 404；/slow -> 延迟 1.5s；/big -> 5KB；/echo -> 回显请求体
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
    static int ClosedPort()
    {
        QTcpServer tmp;
        tmp.listen(QHostAddress::LocalHost, 0);
        const int port = tmp.serverPort();
        tmp.close();
        return port;
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
        const int headEnd = req.indexOf("\r\n\r\n");
        if (headEnd < 0)
            return;  // 头还没收全

        const QList<QByteArray> parts = req.left(headEnd).split(' ');
        const QString path = (parts.size() > 1) ? QString::fromUtf8(parts.at(1)) : QString();
        const QByteArray body = req.mid(headEnd + 4);

        if (path == QLatin1String("/ok"))
            Reply(sock, 200, "hello-c1");
        else if (path == QLatin1String("/slow"))
            QTimer::singleShot(1500, sock, [this, sock] { Reply(sock, 200, "slow"); });
        else if (path == QLatin1String("/big"))
            Reply(sock, 200, QByteArray(5000, 'x'));
        else if (path == QLatin1String("/echo"))
            Reply(sock, 200, body);
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
    std::function<bool()> action;                 // 返回 false 表示"发送阶段就失败"
    std::function<bool(const NetResult &)> check;  // 对结果做断言
};

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    CannedServer server;
    if (!server.Start()) {
        std::printf("[FAIL] local http server listen failed\n");
        return 2;
    }
    const QString base = server.BaseUrl();
    const int deadPort = CannedServer::ClosedPort();
    std::printf("local http server: %s   closed port: %d\n",
                base.toUtf8().constData(), deadPort);

    NetClient net;
    QVector<Case> cases;
    int index = 0;
    std::function<void()> next;

    // ---- 用例表 ----
    Case ok200;
    ok200.name = QStringLiteral("GET 200 + body");
    ok200.action = [&] { return net.Get(QUrl(base + "/ok"), 5000); };
    ok200.check = [](const NetResult &r) {
        return r.ok && r.httpStatus == 200 && r.body == "hello-c1" && r.attempts == 1;
    };

    Case nf404;
    nf404.name = QStringLiteral("404 视为拿到响应");
    nf404.action = [&] { return net.Get(QUrl(base + "/nf"), 5000); };
    nf404.check = [](const NetResult &r) {
        return r.ok && r.httpStatus == 404 && r.httpError && !r.truncated;
    };

    Case slow;
    slow.name = QStringLiteral("慢响应 -> TimeOut");
    slow.action = [&] {
        NetRequest req;
        req.url = QUrl(base + "/slow");
        req.timeoutMs = 900;
        req.retries = 0;  // 只验超时本身，不掺重试
        return net.Send(req);
    };
    slow.check = [](const NetResult &r) {
        return !r.ok && r.errorCode == QLatin1String("TimeOut") && r.attempts == 1;
    };

    Case big;
    big.name = QStringLiteral("maxBytes 截断");
    big.action = [&] {
        NetRequest req;
        req.url = QUrl(base + "/big");
        req.maxBytes = 128;
        return net.Send(req);
    };
    big.check = [](const NetResult &r) {
        return r.ok && r.truncated && r.bodyLen == 128;
    };

    Case post;
    post.name = QStringLiteral("POST 回显请求体");
    post.action = [&] {
        NetRequest req;
        req.url = QUrl(base + "/echo");
        req.method = QStringLiteral("POST");
        req.headers.insert(QStringLiteral("Content-Type"), QStringLiteral("text/plain"));
        req.body = "ping-body";
        return net.Send(req);
    };
    post.check = [](const NetResult &r) {
        return r.ok && r.httpStatus == 200 && r.body == "ping-body";
    };

    Case retry;
    retry.name = QStringLiteral("拒连 -> 传输层错误且按 retries 重试");
    retry.action = [&] {
        NetRequest req;
        req.url = QUrl(QStringLiteral("http://127.0.0.1:%1/x").arg(deadPort));
        req.timeoutMs = 1200;
        req.retries = 1;
        return net.Send(req);
    };
    retry.check = [](const NetResult &r) {
        const bool transport = r.errorCode == QLatin1String("ConnectErr")
                               || r.errorCode == QLatin1String("TimeOut")
                               || r.errorCode == QLatin1String("ResetErr");
        return !r.ok && transport && r.attempts == 2;
    };

    cases << ok200 << nf404 << slow << big << post << retry;

    // 非法 URL：发送阶段即返回 false，同时仍要有 Finished(UserError)，不静默吞掉
    bool userErrSeen = false;
    QObject::connect(&net, &NetClient::Finished, [&](const NetResult &r) {
        g_guard.stop();
        if (index < 0) {
            if (r.errorCode == QLatin1String("UserError"))
                userErrSeen = true;
            return;
        }
        if (index >= cases.size()) {
            Check(QStringLiteral("响应落在预期之外"), false, r.Summary());
            return;
        }
        const Case &c = cases.at(index);
        Check(c.name, c.check(r), r.Summary());
        ++index;
        QTimer::singleShot(0, &app, next);
    });

    next = [&] {
        if (index >= cases.size()) {
            // 收尾：非法 URL + 单通道保护
            g_guard.stop();
            index = -1;
            NetRequest bad;
            bad.url = QUrl(QStringLiteral("ftp://example.com/x"));
            const bool accepted = net.Send(bad);
            Check(QStringLiteral("非法 scheme 发送被拒"), !accepted);
            Check(QStringLiteral("非法 scheme 仍发 UserError"), userErrSeen);

            // 单通道：一个请求在飞时第二个必须被拒
            net.Get(QUrl(base + "/slow"), 2000);
            const bool second = net.Get(QUrl(base + "/ok"), 1000);
            Check(QStringLiteral("单通道：并发请求被拒"), !second, net.GetLastError());

            std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
            // 允许 slow 请求自然结束，但不影响判定
            QTimer::singleShot(2200, &app, [&] { std::exit(g_fail == 0 ? 0 : 1); });
            return;
        }
        const Case &c = cases.at(index);
        std::printf("[CASE] %s\n", c.name.toUtf8().constData());
        g_step = c.name;
        g_guard.start(15000);
        if (!c.action()) {
            g_guard.stop();
            Check(c.name + QStringLiteral("（发送阶段）"), false, net.GetLastError());
            ++index;
            QTimer::singleShot(0, &app, next);
        }
    };

    QObject::connect(&g_guard, &QTimer::timeout, [&] {
        Check(QStringLiteral("步骤超时: ") + g_step, false);
        std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
        std::exit(1);
    });

    // CA 加载：本测试只打本地 HTTP，不依赖证书；仍调用一次确保接口可用且不崩
    const int ca = NetClient::EnsureCaCertificates();
    std::printf("CA certificates loaded: %d\n", ca);

    QTimer::singleShot(0, &app, next);
    return app.exec();
}
