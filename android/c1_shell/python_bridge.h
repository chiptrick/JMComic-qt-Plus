// C1-W6/P4：Python 子进程桥 —— QProcess + 行分隔 JSON 请求/响应。
//
// 设计目标：
//   * 提供"子进程起停 / 请求 / 超时 / 崩溃重启"四类机制的闭环；
//   * 协议 v1（docs/c1_bridge_protocol.md）：每行一个 JSON 对象，
//     request 带 id/method/params；response 带 id/method/ok/result|error；
//     **响应回带 method** 是 P4 补的：只靠 id 关联时，调用方无从判断回来的是哪个方法；
//   * 与 Python 侧的具体业务解耦：只要能跑一个读 stdin/写 stdout 的脚本就能接；
//   * Android 上如果设备侧没有 Python 运行时，Start() 会失败并给出明确错误，
//     不会假装"已连接"。
//
// 可测性（P4-4）：本类不依赖 Android 头文件（日志走 C1_LOG_* 宏，见 .cpp），
// 因此桌面宿主的 host_test_python_bridge 直接链接同一份实现，不再另写一份等价逻辑。
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

    // P4：真实网络请求（method=net.request）。httpMethod 是 HTTP 动词（GET/POST/...），
    // params 可带 headers/body/retries/max_bytes/include_body/proxy。
    // 这里把 Python 侧超时设成略短于 C++ 侧：先让子进程自己报 TimeOut（带错误码），
    // 而不是让 C++ 先超时——否则"超时"就退化成"没回话"，定位不到是哪一层。
    bool NetRequest(const QString &httpMethod, const QString &url,
                    const QJsonObject &params = {}, int timeoutMs = 15000);

    // P4：子进程自述（net.status）/ 优雅关闭（net.close）。
    bool NetStatus(int timeoutMs = 3000);
    bool NetClose(int timeoutMs = 3000);

    // 自动重启开关：子进程异常退出时自动再起一次（默认开）
    void SetAutoRestart(bool on) { autoRestart_ = on; }
    bool IsAutoRestart() const { return autoRestart_; }

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
    // Stop() 主动停止 vs 子进程自己死掉：Python 抛异常时退出码非 0 但 ExitStatus 是
    // NormalExit，只看 CrashExit 会漏掉真机上最常见的崩溃形态（S3 用例）。
    bool stopping_ = false;
    bool restarting_ = false;
    QString lastError_;
};
