// C1-W6：Python 子进程桥 —— QProcess + 行分隔 JSON 请求/响应。
//
// 设计目标：
//   * 提供"子进程起停 / 请求 / 超时 / 崩溃重启"四类机制的闭环；
//   * 协议：每行一个 JSON 对象（request 带 id/method/params；response 带 id/ok/result/error）；
//   * 与 Python 侧的具体业务解耦：只要能跑一个读 stdin/写 stdout 的脚本就能接；
//   * Android 上如果设备侧没有 Python 运行时，Start() 会失败并给出明确错误，
//     不会假装"已连接"。
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>

class PythonBridge : public QObject
{
    Q_OBJECT
public:
    explicit PythonBridge(QObject *parent = nullptr);

    // 启动子进程；已在跑则先停再起。返回 false 时 GetLastError() 有原因。
    bool Start(const QString &program, const QStringList &args = {});
    void Stop();
    bool IsRunning() const { return proc_ && proc_->state() == QProcess::Running; }
    QString GetLastError() const { return lastError_; }

    // 发一次请求；超时或失败返回 false。响应通过 Response 信号回来。
    bool SendRequest(const QString &method, const QJsonObject &params, int timeoutMs = 5000);

    // 自动重启开关：子进程异常退出时自动再起一次（默认开）
    void SetAutoRestart(bool on) { autoRestart_ = on; }

signals:
    void Started();
    void Response(const QJsonObject &obj);
    void Error(const QString &text);
    void Crashed();

private slots:
    void OnReadyRead();
    void OnFinished(int exitCode, QProcess::ExitStatus status);
    void OnErrorOccurred(QProcess::ProcessError error);
    void OnTimeout();

private:
    void Restart();

    QProcess *proc_ = nullptr;
    QTimer timeout_;
    int nextId_ = 1;
    int pendingId_ = 0;
    QString pendingMethod_;
    QString program_;
    QStringList args_;
    bool autoRestart_ = true;
    QString lastError_;
};
