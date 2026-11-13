#include "net_backend.h"

#include "quic/quic_http.h"

#include "tls/tls_channel.h"

#include <QLibrary>
#include <QList>
#include <QSslSocket>
#include <QStringList>

namespace {

QString g_lastError;

// 底层 libssl 是否真的导出 ECH API。OpenSSL 3.2+ 默认带 ECH，但发行版
// 常用 no-ech 构建（本机 Git 自带的 3.5.5 就是），只看版本号会误判。
bool OpenSslHasEch()
{
    const QStringList names = {
#ifdef Q_OS_WIN
        QStringLiteral("libssl-3-x64.dll"), QStringLiteral("libssl-3.dll"),
#else
        QStringLiteral("libssl.so.3"), QStringLiteral("libssl.so"),
#endif
    };
    for (const QString &name : names) {
        QLibrary lib(name);
        if (lib.load() && lib.resolve("SSL_set1_ech_config_list"))
            return true;
    }
    return false;
}

// Qt 的 sslLibraryVersionNumber 对 OpenSSL 是 0xMNN00PP0L
bool OpenSslAtLeast(quint32 v, int major, int minor)
{
    return int((v >> 28) & 0xF) > major
           || (int((v >> 28) & 0xF) == major && int((v >> 20) & 0xFF) >= minor);
}

}  // namespace

namespace NetBackend {

TlsCapabilities Tls()
{
    TlsCapabilities c;
    // 自建 OpenSSL 4.x 通道可用时，ECH 真正能注入 ClientHello；
    // 这时不要再用 Qt 的 QSslSocket 能力（3.5.5 是 no-ech 构建）下结论。
    if (EchChannelAvailable()) {
        c.backend = QStringLiteral("openssl-ech");
        c.version = EchChannelStatus();   // "OpenSSL 4.1.0 @ <dir>"
        c.tls13 = true;
        c.ech = true;
        c.echRuntime = true;
        c.earlyData = false;
        c.quicTls = false;
        c.note = QStringLiteral("ECH 走自建 OpenSSL 4.x 通道；Qt QSslSocket 作为回退");
        return c;
    }
    c.backend = QSslSocket::activeBackend();
    if (c.backend.isEmpty()) {
        const QList<QString> all = QSslSocket::availableBackends();
        c.backend = all.isEmpty() ? QStringLiteral("none") : all.first();
    }
    c.version = QSslSocket::sslLibraryVersionString();
    const quint32 v = quint32(QSslSocket::sslLibraryVersionNumber());

    const bool openssl = c.version.contains(QStringLiteral("OpenSSL"), Qt::CaseInsensitive);
    c.tls13 = openssl ? OpenSslAtLeast(v, 1, 1) : c.version.contains(QStringLiteral("Schannel"));
    c.echRuntime = openssl && OpenSslHasEch();
    c.ech = false;        // Qt 6.12 没有 ECH 注入接口（后端插件也不引用 SSL_set1_ech_config_list）
    c.earlyData = false;  // QSslSocket 不暴露 early data 读写
    c.quicTls = false;    // 没有 QUIC-TLS 回调接口
    if (!openssl) {
        c.note = QStringLiteral("Schannel 不支持 ECH；Qt 6.12 亦无 ECH 注入接口");
    } else if (!OpenSslAtLeast(v, 3, 2)) {
        c.note = QStringLiteral("OpenSSL 低于 3.2，运行库不支持 ECH");
    } else if (!c.echRuntime) {
#ifdef Q_OS_ANDROID
        c.note = QStringLiteral("OpenSSL %1 为 no-ech 构建；APK 需打进 enable-ech 的 4.x"
                                "（build_quic_deps_android.sh）")
                     .arg(c.version);
#else
        c.note = QStringLiteral("OpenSSL %1 为 no-ech 构建，未导出 SSL_set1_ech_config_list")
                     .arg(c.version);
#endif
    } else {
        c.note = QStringLiteral("OpenSSL 运行库支持 ECH，但 Qt 6.12 未暴露 ECH 注入接口");
    }
    return c;
}

QuicCapabilities Quic()
{
    QuicCapabilities c;
    if (quic::Available()) {
        c.available = true;
        c.stack = QStringLiteral("ngtcp2+nghttp3");
        c.version = quic::BackendSummary();
        c.http3 = true;
        // 0-RTT 需要会话票据复用（SSL_set_session + NGTCP2_CONN_FLAG_EARLY_DATA），
        // 目前每个请求新建连接，未启用。
        c.zeroRtt = false;
        c.note = QStringLiteral("QUIC/HTTP3 已编译（OpenSSL 4.x QUIC-TLS + ngtcp2 + nghttp3）");
    } else {
        c.available = false;
        c.stack = QStringLiteral("未编译");
        c.version = QStringLiteral("-");
        c.http3 = false;
        c.zeroRtt = false;
        c.note = QStringLiteral("未编译 QUIC 栈（构建时 JM_WITH_QUIC=OFF 或缺 per-ABI 依赖）");
    }
    return c;
}

QString Summary()
{
    const TlsCapabilities t = Tls();
    const QuicCapabilities q = Quic();
    // 版本字符串带空格会破坏日志解析，只保留前两段
    // 版本字符串形如 "OpenSSL 4.1.0 @ path" / "OpenSSL 3.5.5 27 Jan 2026"：
    // 取第二段做短版本，保证日志可被 (\S+) 稳定解析。
    QString ver = t.version;
    const QStringList parts = ver.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.size() >= 2)
        ver = parts.at(1);
    if (ver.isEmpty())
        ver = QStringLiteral("-");
    return QStringLiteral("tls=%1/%2 ech=%3 echLib=%4 quic=%5 http3=%6 stack=%7")
        .arg(t.backend.isEmpty() ? QStringLiteral("none") : t.backend, ver,
             t.ech ? QStringLiteral("1") : QStringLiteral("0"),
             t.echRuntime ? QStringLiteral("1") : QStringLiteral("0"),
             q.available ? QStringLiteral("1") : QStringLiteral("0"),
             q.http3 ? QStringLiteral("1") : QStringLiteral("0"), q.stack);
}

bool SetEchConfig(const QByteArray &echConfigList)
{
    if (echConfigList.isEmpty()) {
        g_lastError = QStringLiteral("ECHConfigList 为空");
        return false;
    }
    // 自建 OpenSSL 4.x 通道会在每次建连时把 list 传给
    // SSL_set1_ech_config_list()，这里只需确认通道可用。
    if (EchChannelAvailable()) {
        g_lastError.clear();
        return true;
    }
    // 没有自建通道时，Qt 6.12 的 QSslConfiguration 没有 ECH 入口。
    g_lastError = Tls().note;
    return false;
}

QString LastError()
{
    return g_lastError;
}

}  // namespace NetBackend
