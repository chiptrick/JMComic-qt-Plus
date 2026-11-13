// TLS 通道抽象：把"建连 + 握手 + 读写"从 HTTP 客户端里抽出来。
//
// 现有实现：
//   * QtSslChannel   —— QSslSocket，Qt 自带的 TLS 后端（无 ECH），作为回退
//   * OpenSslEchChannel —— 动态加载 OpenSSL 4.1（带 ECH 的构建），
//     用 memory BIO 驱动 TLS 状态机，支持 ECHConfigList 注入与 0-RTT 所需接口
//
// 为什么动态加载：Qt 已加载的是 OpenSSL 3.5.5（no-ech 构建），不能用它做 ECH；
// 4.1 只提供 DLL（没有导入库），用 QLibrary + 函数指针即可并存，不需要改 Qt。
#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

struct TlsInfo
{
    bool handshakeOk = false;
    int echStatus = -999;      // OpenSSL SSL_ECH_STATUS_*（1=SUCCESS, 2=GREASE, ...）
    QString echText;           // SUCCESS / GREASE / NOT_TRIED / FAILED ...
    QString version;           // TLSv1.3
    QString alpn;
    bool verifyOk = false;
    long verifyResult = -1;
    QString error;
};

class TlsChannel : public QObject
{
    Q_OBJECT
public:
    explicit TlsChannel(QObject *parent = nullptr) : QObject(parent) {}

    // 异步连接：host 是 TCP 目标（可能是节点 IP），sni 是 TLS 的 SNI/证书校验名。
    // echConfigList 为空表示普通 SNI。
    virtual bool Connect(const QString &host, quint16 port, const QString &sni,
                         const QByteArray &echConfigList, const QStringList &alpn) = 0;
    virtual void Write(const QByteArray &data) = 0;
    virtual QByteArray ReadAll() = 0;
    virtual void Close() = 0;
    virtual TlsInfo Info() const = 0;
    virtual bool SupportsEch() const = 0;

signals:
    void Connected();
    void ReadyRead();
    void Disconnected();
    void Error(const QString &text);
    void Log(const QString &line);
};

// 4.1 DLL 可加载且导出 ECH 时返回 true
bool EchChannelAvailable();
QString EchChannelStatus();        // "OpenSSL 4.1.0 @ <dir>" 或不可用原因
QString EchStatusText(int status); // SSL_ECH_STATUS_* → 可读文本

// 创建通道；不可用时返回 nullptr，调用方应回退 QSslSocket
TlsChannel *CreateEchChannel(QObject *parent);
