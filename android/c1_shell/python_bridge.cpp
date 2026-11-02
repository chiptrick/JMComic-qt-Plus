#include "python_bridge.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QTimer>

// P4-4：桌面宿主也要能编译同一份实现，否则 host_test 只能另写一套等价逻辑，
// 测的就不是真实代码了（这正是 W6 那条"桌面闭环已验证"站不住的原因）。
#if defined(__ANDROID__)
#include <android/log.h>
#define C1_LOG_INFO(...) __android_log_print(ANDROID_LOG_INFO, "C1PyBridge", __VA_ARGS__)
#define C1_LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, "C1PyBridge", __VA_ARGS__)
#else
#include <QDebug>
#define C1_LOG_INFO(...) qInfo(__VA_ARGS__)
#define C1_LOG_ERROR(...) qWarning(__VA_ARGS__)
#endif

PythonBridge::PythonBridge(QObject *parent)
    : QObject(parent)
{
    timeout_.setSingleShot(true);
    connect(&timeout_, &QTimer::timeout, this, &PythonBridge::OnTimeout);
}

bool PythonBridge::Start(const QString &program, const QStringList &args)
{
    Stop();
    program_ = program;
    args_ = args;
    lastError_.clear();
    stopping_ = false;
    proc_ = new QProcess(this);
    proc_->setProgram(program);
    proc_->setArguments(args);
    proc_->setProcessChannelMode(QProcess::SeparateChannels);
    connect(proc_, &QProcess::readyReadStandardOutput, this, &PythonBridge::OnReadyRead);
    connect(proc_, &QProcess::finished, this, &PythonBridge::OnFinished);
    connect(proc_, &QProcess::errorOccurred, this, &PythonBridge::OnErrorOccurred);
    proc_->start();
    if (!proc_->waitForStarted(3000)) {
        lastError_ = QStringLiteral("start failed: %1").arg(proc_->errorString());
        C1_LOG_ERROR("%s", qPrintable(lastError_));
        proc_->deleteLater();
        proc_ = nullptr;
        return false;
    }
    C1_LOG_INFO("started %s %s", qPrintable(program), qPrintable(args.join(' ')));
    emit Started();
    return true;
}

void PythonBridge::Stop()
{
    stopping_ = true;
    // 先把 proc_ 摘下来再收拾：terminate()/waitForFinished() 期间会同步派发 finished，
    // 若那时 proc_ 仍指向同一对象，OnFinished 会把它 deleteLater 掉，这里再用就是野指针。
    QProcess *proc = proc_;
    proc_ = nullptr;
    pendingId_ = 0;
    timeout_.stop();
    if (!proc)
        return;
    if (proc->state() != QProcess::NotRunning) {
        proc->terminate();
        if (!proc->waitForFinished(2000))
            proc->kill();
    }
    proc->deleteLater();
}

bool PythonBridge::SendRequest(const QString &method, const QJsonObject &params, int timeoutMs)
{
    if (!IsRunning()) {
        lastError_ = QStringLiteral("bridge not running");
        C1_LOG_ERROR("%s", qPrintable(lastError_));
        emit Error(lastError_);
        return false;
    }
    // 单通道协议：同时只允许一个 in-flight 请求。覆盖 pendingId_ 会让"谁的超时"
    // 彻底说不清，宁可明确拒绝（P4 的最小闭环不需要并发）。
    if (pendingId_) {
        lastError_ = QStringLiteral("bridge busy: %1 pending").arg(pendingMethod_);
        C1_LOG_ERROR("%s", qPrintable(lastError_));
        emit Error(lastError_);
        return false;
    }
    QJsonObject req;
    const int id = nextId_++;
    req.insert(QStringLiteral("id"), id);
    req.insert(QStringLiteral("method"), method);
    req.insert(QStringLiteral("params"), params);
    const QByteArray line = QJsonDocument(req).toJson(QJsonDocument::Compact) + '\n';
    pendingId_ = id;
    pendingMethod_ = method;
    proc_->write(line);
    timeout_.start(timeoutMs);
    C1_LOG_INFO("req id=%d method=%s", id, qPrintable(method));
    return true;
}

bool PythonBridge::NetRequest(const QString &httpMethod, const QString &url,
                              const QJsonObject &params, int timeoutMs)
{
    if (url.isEmpty()) {
        lastError_ = QStringLiteral("NetRequest: url is empty");
        C1_LOG_ERROR("%s", qPrintable(lastError_));
        emit Error(lastError_);
        return false;
    }
    QJsonObject p = params;
    p.insert(QStringLiteral("method"), httpMethod.isEmpty() ? QStringLiteral("GET") : httpMethod);
    p.insert(QStringLiteral("url"), url);
    // 子进程自己先超时，才能带回具体错误码；C++ 侧再多留 500ms 兜底
    if (!p.contains(QStringLiteral("timeout_ms")))
        p.insert(QStringLiteral("timeout_ms"), qMax(500, timeoutMs - 500));
    return SendRequest(QStringLiteral("net.request"), p, timeoutMs);
}

bool PythonBridge::NetStatus(int timeoutMs)
{
    return SendRequest(QStringLiteral("net.status"), QJsonObject(), timeoutMs);
}

bool PythonBridge::NetClose(int timeoutMs)
{
    return SendRequest(QStringLiteral("net.close"), QJsonObject(), timeoutMs);
}

void PythonBridge::OnReadyRead()
{
    if (!proc_)
        return;
    while (proc_->canReadLine()) {
        const QByteArray raw = proc_->readLine().trimmed();
        if (raw.isEmpty())
            continue;
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            lastError_ = QStringLiteral("bad json: %1").arg(QString::fromUtf8(raw.left(120)));
            C1_LOG_ERROR("%s", qPrintable(lastError_));
            emit Error(lastError_);
            continue;
        }
        const QJsonObject obj = doc.object();
        if (pendingId_ && obj.value(QStringLiteral("id")).toInt() == pendingId_) {
            pendingId_ = 0;
            timeout_.stop();
            // 协议层失败（ok=false）在这一层就落成可读错误，调用方不必再解 error 对象；
            // Response 仍然照发，保留 attempts/elapsed 等细节给上层做主数据。
            if (!obj.value(QStringLiteral("ok")).toBool()) {
                const QJsonObject e = obj.value(QStringLiteral("error")).toObject();
                lastError_ = QStringLiteral("%1: %2")
                                 .arg(e.value(QStringLiteral("code")).toString(),
                                      e.value(QStringLiteral("message")).toString());
                C1_LOG_ERROR("%s", qPrintable(lastError_));
                emit Error(lastError_);
            }
        }
        emit Response(obj);
    }
}

void PythonBridge::OnFinished(int exitCode, QProcess::ExitStatus status)
{
    // Stop()/Start() 换进程后，旧进程的 finished 仍会到达：只处理当前这一个
    if (!proc_ || sender() != proc_)
        return;
    C1_LOG_INFO("finished exit=%d status=%d", exitCode, int(status));
    // 非 0 退出码同样是"死了"：Python 抛未捕获异常时 ExitStatus 是 NormalExit，
    // 只认 CrashExit 会让 S3（crash 后自动重启）在真机上永远不触发。
    const bool abnormal = (status == QProcess::CrashExit)
                          || (exitCode != 0 && !stopping_);
    if (abnormal) {
        lastError_ = QStringLiteral("bridge died exit=%1 status=%2")
                         .arg(exitCode)
                         .arg(int(status));
        C1_LOG_ERROR("%s", qPrintable(lastError_));
        pendingId_ = 0;
        timeout_.stop();
        proc_->deleteLater();
        proc_ = nullptr;
        emit Crashed();
        if (autoRestart_)
            Restart();
        return;
    }
    if (stopping_) {
        proc_->deleteLater();
        proc_ = nullptr;
        return;
    }
    // 正常退出（例如 net.close 后子进程自己 break）：收尾，别留下 NotRunning 的 proc_
    C1_LOG_INFO("bridge exited normally exit=%d", exitCode);
    proc_->deleteLater();
    proc_ = nullptr;
}

void PythonBridge::OnErrorOccurred(QProcess::ProcessError error)
{
    // 主动 Stop() 时 terminate() 同样会报 Crashed；那是我们杀的，不是崩溃。
    // 不区分的话，日志里会出现"假 crash + auto restart"，S3 的判据就失去意义。
    if (stopping_)
        return;
    QString text;
    switch (error) {
    case QProcess::FailedToStart:
        text = QStringLiteral("failed to start");
        break;
    case QProcess::Crashed:
        text = QStringLiteral("crashed");
        break;
    case QProcess::Timedout:
        text = QStringLiteral("timed out");
        break;
    case QProcess::WriteError:
        text = QStringLiteral("write error");
        break;
    case QProcess::ReadError:
        text = QStringLiteral("read error");
        break;
    default:
        text = QStringLiteral("unknown error");
    }
    lastError_ = text;
    C1_LOG_ERROR("%s", qPrintable(text));
    if (error == QProcess::Crashed && autoRestart_)
        Restart();
    emit Error(text);
}

void PythonBridge::OnTimeout()
{
    lastError_ = QStringLiteral("request timeout method=%1").arg(pendingMethod_);
    C1_LOG_ERROR("%s", qPrintable(lastError_));
    pendingId_ = 0;
    emit Error(lastError_);
}

void PythonBridge::Restart()
{
    // errorOccurred(Crashed) 与 finished 可能先后到达同一次死亡，去抖避免连起重启
    if (restarting_)
        return;
    restarting_ = true;
    C1_LOG_INFO("auto restart");
    QTimer::singleShot(100, this, [this] {
        restarting_ = false;
        if (!IsRunning())
            Start(program_, args_);
    });
}
