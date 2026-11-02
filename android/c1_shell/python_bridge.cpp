#include "python_bridge.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <android/log.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "C1PyBridge", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "C1PyBridge", __VA_ARGS__)

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
        LOGE("%s", qPrintable(lastError_));
        proc_->deleteLater();
        proc_ = nullptr;
        return false;
    }
    LOGI("started %s %s", qPrintable(program), qPrintable(args.join(' ')));
    emit Started();
    return true;
}

void PythonBridge::Stop()
{
    if (proc_) {
        if (proc_->state() != QProcess::NotRunning) {
            proc_->terminate();
            if (!proc_->waitForFinished(2000))
                proc_->kill();
        }
        proc_->deleteLater();
        proc_ = nullptr;
    }
    pendingId_ = 0;
    timeout_.stop();
}

bool PythonBridge::SendRequest(const QString &method, const QJsonObject &params, int timeoutMs)
{
    if (!IsRunning()) {
        lastError_ = QStringLiteral("bridge not running");
        LOGE("%s", qPrintable(lastError_));
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
    LOGI("req id=%d method=%s", id, qPrintable(method));
    return true;
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
            LOGE("%s", qPrintable(lastError_));
            emit Error(lastError_);
            continue;
        }
        const QJsonObject obj = doc.object();
        if (pendingId_ && obj.value(QStringLiteral("id")).toInt() == pendingId_) {
            pendingId_ = 0;
            timeout_.stop();
        }
        emit Response(obj);
    }
}

void PythonBridge::OnFinished(int exitCode, QProcess::ExitStatus status)
{
    LOGI("finished exit=%d status=%d", exitCode, int(status));
    if (status == QProcess::CrashExit) {
        lastError_ = QStringLiteral("crashed exit=%1").arg(exitCode);
        emit Crashed();
        if (autoRestart_)
            Restart();
    }
}

void PythonBridge::OnErrorOccurred(QProcess::ProcessError error)
{
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
    LOGE("%s", qPrintable(text));
    if (error == QProcess::Crashed && autoRestart_)
        Restart();
    emit Error(text);
}

void PythonBridge::OnTimeout()
{
    lastError_ = QStringLiteral("request timeout method=%1").arg(pendingMethod_);
    LOGE("%s", qPrintable(lastError_));
    pendingId_ = 0;
    emit Error(lastError_);
}

void PythonBridge::Restart()
{
    LOGI("auto restart");
    QTimer::singleShot(100, this, [this] {
        if (!IsRunning())
            Start(program_, args_);
    });
}
