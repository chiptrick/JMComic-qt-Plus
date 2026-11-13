#include "tls_channel.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLibrary>
#include <QRegularExpression>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QStandardPaths>
#include <QTcpSocket>

#include <cstring>

namespace {

struct ssl_st;
struct ssl_ctx_st;
struct ssl_method_st;
struct bio_st;
struct bio_method_st;
struct x509_st;
using SSL = ssl_st;
using SSL_CTX = ssl_ctx_st;
using SSL_METHOD = ssl_method_st;
using BIO = bio_st;
using BIO_METHOD = bio_method_st;
using X509 = x509_st;

using p_TLS_client_method = const SSL_METHOD *(*)();
using p_SSL_CTX_new = SSL_CTX *(*)(const SSL_METHOD *);
using p_SSL_CTX_free = void (*)(SSL_CTX *);
using p_SSL_CTX_set_verify = void (*)(SSL_CTX *, int, int (*)(int, void *));
using p_SSL_CTX_load_verify_locations = int (*)(SSL_CTX *, const char *, const char *);
using p_SSL_CTX_set_default_verify_paths = int (*)(SSL_CTX *);
using p_SSL_CTX_ctrl = long (*)(SSL_CTX *, int, long, void *);
using p_SSL_new = SSL *(*)(SSL_CTX *);
using p_SSL_free = void (*)(SSL *);
using p_SSL_set_bio = void (*)(SSL *, BIO *, BIO *);
using p_SSL_set_connect_state = void (*)(SSL *);
using p_SSL_set1_ech_config_list = int (*)(SSL *, const uint8_t *, size_t);
using p_SSL_ech_get1_status = int (*)(SSL *, char **, char **);
using p_SSL_ech_set1_server_names = int (*)(SSL *, const char *, const char *, int);
using p_SSL_ech_set1_outer_server_name = int (*)(SSL *, const char *, int);
using p_SSL_ech_set1_outer_alpn_protos = int (*)(SSL *, const unsigned char *, size_t);
using p_SSL_do_handshake = int (*)(SSL *);
using p_SSL_get_error = int (*)(const SSL *, int);
using p_SSL_read = int (*)(SSL *, void *, int);
using p_SSL_write = int (*)(SSL *, const void *, int);
using p_SSL_shutdown = int (*)(SSL *);
using p_SSL_get_version = const char *(*)(const SSL *);
using p_SSL_get_verify_result = long (*)(const SSL *);
using p_SSL_get1_peer_certificate = X509 *(*)(const SSL *);
using p_SSL_set_alpn_protos = int (*)(SSL *, const unsigned char *, unsigned int);
using p_SSL_get0_alpn_selected = void (*)(const SSL *, const unsigned char **, unsigned int *);
using p_SSL_ctrl = long (*)(SSL *, int, long, void *);
using p_BIO_s_mem = const BIO_METHOD *(*)();
using p_BIO_new = BIO *(*)(const BIO_METHOD *);
using p_BIO_write = int (*)(BIO *, const void *, int);
using p_BIO_read = int (*)(BIO *, void *, int);
using p_BIO_ctrl = long (*)(BIO *, int, long, void *);
using p_X509_check_host = int (*)(X509 *, const char *, size_t, unsigned int, char **);
using p_X509_free = void (*)(X509 *);
using p_ERR_get_error = unsigned long (*)();
using p_ERR_error_string_n = void (*)(unsigned long, char *, size_t);
using p_OpenSSL_version = const char *(*)(int);

constexpr int kSslVerifyPeer = 0x01;
constexpr int kSslCtrlSetSni = 55;         // SSL_CTRL_SET_TLSEXT_HOSTNAME
constexpr int kBioCtrlPending = 10;        // BIO_CTRL_PENDING
constexpr int kErrorWantRead = 2;
constexpr int kErrorWantWrite = 3;
constexpr int kErrorZeroReturn = 6;

struct SslApi
{
    bool ok = false;
    QString dir;
    QString version;
    QString error;

    QLibrary crypto;
    QLibrary ssl;

    p_TLS_client_method TLS_client_method = nullptr;
    p_SSL_CTX_new SSL_CTX_new = nullptr;
    p_SSL_CTX_free SSL_CTX_free = nullptr;
    p_SSL_CTX_set_verify SSL_CTX_set_verify = nullptr;
    p_SSL_CTX_load_verify_locations SSL_CTX_load_verify_locations = nullptr;
    p_SSL_CTX_set_default_verify_paths SSL_CTX_set_default_verify_paths = nullptr;
    p_SSL_CTX_ctrl SSL_CTX_ctrl = nullptr;
    p_SSL_new SSL_new = nullptr;
    p_SSL_free SSL_free = nullptr;
    p_SSL_set_bio SSL_set_bio = nullptr;
    p_SSL_set_connect_state SSL_set_connect_state = nullptr;
    p_SSL_set1_ech_config_list SSL_set1_ech_config_list = nullptr;
    p_SSL_ech_get1_status SSL_ech_get1_status = nullptr;
    p_SSL_ech_set1_server_names SSL_ech_set1_server_names = nullptr;
    p_SSL_ech_set1_outer_server_name SSL_ech_set1_outer_server_name = nullptr;
    p_SSL_ech_set1_outer_alpn_protos SSL_ech_set1_outer_alpn_protos = nullptr;
    p_SSL_do_handshake SSL_do_handshake = nullptr;
    p_SSL_get_error SSL_get_error = nullptr;
    p_SSL_read SSL_read = nullptr;
    p_SSL_write SSL_write = nullptr;
    p_SSL_shutdown SSL_shutdown = nullptr;
    p_SSL_get_version SSL_get_version = nullptr;
    p_SSL_get_verify_result SSL_get_verify_result = nullptr;
    p_SSL_get1_peer_certificate SSL_get1_peer_certificate = nullptr;
    p_SSL_set_alpn_protos SSL_set_alpn_protos = nullptr;
    p_SSL_get0_alpn_selected SSL_get0_alpn_selected = nullptr;
    p_SSL_ctrl SSL_ctrl = nullptr;
    p_BIO_s_mem BIO_s_mem = nullptr;
    p_BIO_new BIO_new = nullptr;
    p_BIO_write BIO_write = nullptr;
    p_BIO_read BIO_read = nullptr;
    p_BIO_ctrl BIO_ctrl = nullptr;
    p_X509_check_host X509_check_host = nullptr;
    p_X509_free X509_free = nullptr;
    p_ERR_get_error ERR_get_error = nullptr;
    p_ERR_error_string_n ERR_error_string_n = nullptr;
    p_OpenSSL_version OpenSSL_version = nullptr;

    // 版本号优先问库自己（OpenSSL_version(OPENSSL_VERSION)）。Android 上
    // 没有 include/openssl/opensslv.h 可读，原来的头文件解析必然返回空。
    QString VersionText(const QString &base) const
    {
        if (OpenSSL_version) {
            const char *raw = OpenSSL_version(0);   // OPENSSL_VERSION
            const QString text = raw ? QString::fromLatin1(raw) : QString();
            const QStringList parts = text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
            if (parts.size() >= 2 && parts.first() == QLatin1String("OpenSSL"))
                return parts.at(1);
            if (!text.isEmpty())
                return text;
        }
        return ReadVersion(base);
    }

    QString ReadVersion(const QString &base) const
    {
        QFile f(base + QStringLiteral("/include/openssl/opensslv.h"));
        if (!f.open(QIODevice::ReadOnly))
            return QString();
        const QString text = QString::fromLatin1(f.readAll());
        const QRegularExpression re(
            QStringLiteral("OPENSSL_VERSION_STR\\s+\"([^\"]+)\""));
        const auto m = re.match(text);
        return m.hasMatch() ? m.captured(1) : QString();
    }

    // 逐个候选尝试加载；返回 true 表示这套库可用。
    bool TryLoad(const QString &base)
    {
        // 库名与目录按平台分派：
        //   Windows —— OpenSSL 4 官方命名 libssl-4-x64.dll，位于 <base>/bin；
        //   Android —— APK 内 native lib 目录，文件名无版本号（libssl.so），
        //              LD_LIBRARY_PATH 已含该目录，故先试裸名，再用
        //              applicationDirPath() 的绝对路径兜底；
        //   Linux   —— 与 Android 同构。
        // 只认 Windows 命名会让 Android 永远 ech=0（0.89 的遗留缺陷）。
        QStringList sslNames;
        QStringList cryptoNames;
#ifdef Q_OS_WIN
        const QString bin = base + QStringLiteral("/bin");
        sslNames << bin + QStringLiteral("/libssl-4-x64.dll");
        cryptoNames << bin + QStringLiteral("/libcrypto-4-x64.dll");
#else
        Q_UNUSED(base)
        sslNames << QStringLiteral("libssl.so");
        cryptoNames << QStringLiteral("libcrypto.so");
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!appDir.isEmpty()) {
            sslNames << appDir + QStringLiteral("/libssl.so");
            cryptoNames << appDir + QStringLiteral("/libcrypto.so");
        }
#endif

        for (int i = 0; i < sslNames.size(); ++i) {
            const QString &sslName = sslNames.at(i);
            const QString &cryptoName = cryptoNames.at(qMin(i, cryptoNames.size() - 1));
            // 绝对路径要先存在；裸名交给动态链接器，不能做 exists 检查
            if (sslName.contains(QLatin1Char('/')) && !QFile::exists(sslName))
                continue;
            if (cryptoName.contains(QLatin1Char('/')) && !QFile::exists(cryptoName))
                continue;

            // 先 crypto 再 ssl：Windows 会复用已加载的同名模块，避免 Qt 已加载的
            // libcrypto-3 与 4.x 的依赖解析冲突。
            crypto.setFileName(cryptoName);
            if (!crypto.load()) {
                error = crypto.errorString();
                continue;
            }
            ssl.setFileName(sslName);
            if (!ssl.load()) {
                error = ssl.errorString();
                crypto.unload();
                continue;
            }
        auto R = [this](const char *name) { return ssl.resolve(name); };
        auto RC = [this](const char *name) { return crypto.resolve(name); };

        TLS_client_method = reinterpret_cast<p_TLS_client_method>(R("TLS_client_method"));
        SSL_CTX_new = reinterpret_cast<p_SSL_CTX_new>(R("SSL_CTX_new"));
        SSL_CTX_free = reinterpret_cast<p_SSL_CTX_free>(R("SSL_CTX_free"));
        SSL_CTX_set_verify = reinterpret_cast<p_SSL_CTX_set_verify>(R("SSL_CTX_set_verify"));
        SSL_CTX_load_verify_locations =
            reinterpret_cast<p_SSL_CTX_load_verify_locations>(R("SSL_CTX_load_verify_locations"));
        SSL_CTX_set_default_verify_paths = reinterpret_cast<p_SSL_CTX_set_default_verify_paths>(
            R("SSL_CTX_set_default_verify_paths"));
        SSL_CTX_ctrl = reinterpret_cast<p_SSL_CTX_ctrl>(R("SSL_CTX_ctrl"));
        SSL_new = reinterpret_cast<p_SSL_new>(R("SSL_new"));
        SSL_free = reinterpret_cast<p_SSL_free>(R("SSL_free"));
        SSL_set_bio = reinterpret_cast<p_SSL_set_bio>(R("SSL_set_bio"));
        SSL_set_connect_state = reinterpret_cast<p_SSL_set_connect_state>(R("SSL_set_connect_state"));
        SSL_set1_ech_config_list =
            reinterpret_cast<p_SSL_set1_ech_config_list>(R("SSL_set1_ech_config_list"));
        SSL_ech_get1_status = reinterpret_cast<p_SSL_ech_get1_status>(R("SSL_ech_get1_status"));
        SSL_ech_set1_server_names =
            reinterpret_cast<p_SSL_ech_set1_server_names>(R("SSL_ech_set1_server_names"));
        SSL_ech_set1_outer_server_name = reinterpret_cast<p_SSL_ech_set1_outer_server_name>(
            R("SSL_ech_set1_outer_server_name"));
        SSL_ech_set1_outer_alpn_protos = reinterpret_cast<p_SSL_ech_set1_outer_alpn_protos>(
            R("SSL_ech_set1_outer_alpn_protos"));
        SSL_do_handshake = reinterpret_cast<p_SSL_do_handshake>(R("SSL_do_handshake"));
        SSL_get_error = reinterpret_cast<p_SSL_get_error>(R("SSL_get_error"));
        SSL_read = reinterpret_cast<p_SSL_read>(R("SSL_read"));
        SSL_write = reinterpret_cast<p_SSL_write>(R("SSL_write"));
        SSL_shutdown = reinterpret_cast<p_SSL_shutdown>(R("SSL_shutdown"));
        SSL_get_version = reinterpret_cast<p_SSL_get_version>(R("SSL_get_version"));
        SSL_get_verify_result = reinterpret_cast<p_SSL_get_verify_result>(R("SSL_get_verify_result"));
        SSL_get1_peer_certificate =
            reinterpret_cast<p_SSL_get1_peer_certificate>(R("SSL_get1_peer_certificate"));
        SSL_set_alpn_protos = reinterpret_cast<p_SSL_set_alpn_protos>(R("SSL_set_alpn_protos"));
        SSL_get0_alpn_selected =
            reinterpret_cast<p_SSL_get0_alpn_selected>(R("SSL_get0_alpn_selected"));
        SSL_ctrl = reinterpret_cast<p_SSL_ctrl>(R("SSL_ctrl"));
        BIO_s_mem = reinterpret_cast<p_BIO_s_mem>(RC("BIO_s_mem"));
        BIO_new = reinterpret_cast<p_BIO_new>(RC("BIO_new"));
        BIO_write = reinterpret_cast<p_BIO_write>(RC("BIO_write"));
        BIO_read = reinterpret_cast<p_BIO_read>(RC("BIO_read"));
        BIO_ctrl = reinterpret_cast<p_BIO_ctrl>(RC("BIO_ctrl"));
        X509_check_host = reinterpret_cast<p_X509_check_host>(RC("X509_check_host"));
        X509_free = reinterpret_cast<p_X509_free>(RC("X509_free"));
        ERR_get_error = reinterpret_cast<p_ERR_get_error>(RC("ERR_get_error"));
        ERR_error_string_n = reinterpret_cast<p_ERR_error_string_n>(RC("ERR_error_string_n"));

        OpenSSL_version = reinterpret_cast<p_OpenSSL_version>(RC("OpenSSL_version"));

            const bool complete = TLS_client_method && SSL_CTX_new && SSL_new && SSL_set_bio
                                  && SSL_set1_ech_config_list && SSL_ech_get1_status
                                  && SSL_do_handshake && SSL_read && SSL_write && BIO_s_mem
                                  && BIO_new && BIO_write && BIO_read && BIO_ctrl
                                  && X509_check_host;
            if (!complete) {
                error = QStringLiteral("%1 lacks ECH symbols").arg(sslName);
                ssl.unload();
                crypto.unload();
                continue;
            }
            dir = sslName;
            version = VersionText(base);
            ok = true;
            return true;
        }
        return false;
    }

    void Load()
    {
        if (ok)
            return;
        // error 只是"上一个候选的失败原因"，不能当成"已尝试过"的标记：
        // 多个候选要全部试完才能判失败。
        error.clear();
        QStringList dirs;
        const QString env = qEnvironmentVariable("JM_OPENSSL_ECH_DIR");
        if (!env.isEmpty())
            dirs << env;
#ifdef Q_OS_WIN
        dirs << QStringLiteral("C:/Program Files/OpenSSL-4.1")
             << QStringLiteral("C:/Program Files/OpenSSL-Win64");
#else
        // Android / Linux：库由 APK 或系统前缀提供，与目录无关
        dirs << QString();
#endif
        for (const QString &d : dirs) {
            if (TryLoad(d))
                return;
        }
        if (error.isEmpty()) {
#ifdef Q_OS_ANDROID
            error = QStringLiteral("APK 内未找到带 ECH 的 libssl.so");
#else
            error = QStringLiteral("未找到带 ECH 的 OpenSSL 4.x（检查 JM_OPENSSL_ECH_DIR）");
#endif
        }
    }
};

SslApi &Api()
{
    static SslApi api;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        api.Load();
    }
    return api;
}

// ECHConfigList → 第一个 ECHConfig 的 public_name（外层 SNI）。
// 结构（RFC 9849）：list_len | version(2) len(2) config_id(1) kem_id(2)
// public_key(len+data) cipher_suites(len+data) max_name_len(1) name(len+data) ...
QString ParseEchPublicName(const QByteArray &list)
{
    auto be16 = [&list](int off) -> int {
        if (off < 0 || off + 2 > list.size())
            return -1;
        return (quint8(list.at(off)) << 8) | quint8(list.at(off + 1));
    };
    if (list.size() < 4)
        return QString();
    int off = (be16(0) == list.size() - 2) ? 2 : 0;
    while (off + 4 <= list.size()) {
        const int version = be16(off);
        const int len = be16(off + 2);
        if (version < 0 || len < 0)
            break;
        const int start = off + 4;
        const int end = start + len;
        if (end > list.size())
            break;
        if (version == 0xfe0d) {
            int p = start + 1 + 2;   // config_id + kem_id
            const int pkLen = be16(p);
            if (pkLen < 0 || p + 2 + pkLen > end)
                break;
            p += 2 + pkLen;
            const int csLen = be16(p);
            if (csLen < 0 || p + 2 + csLen > end)
                break;
            p += 2 + csLen + 1;      // cipher_suites + maximum_name_length
            if (p + 1 > end)
                break;
            const int nameLen = quint8(list.at(p));
            ++p;
            if (p + nameLen > end)
                break;
            return QString::fromUtf8(list.mid(p, nameLen));
        }
        off = end;
    }
    return QString();
}

QString CaBundlePath()
{
    static QString cached;
    if (!cached.isEmpty())
        return cached;

    // 1) 显式指定
    const QString env = qEnvironmentVariable("JM_CA_BUNDLE");
    if (!env.isEmpty() && QFile::exists(env)) {
        cached = env;
        return cached;
    }
    // 2) 本机已有的 Mozilla bundle（Git for Windows 自带；Android 上不存在）
    const QStringList known = {
        QStringLiteral("C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"),
        QStringLiteral("C:/Program Files/Git/mingw64/ssl/certs/ca-bundle.crt"),
    };
    for (const QString &p : known) {
        if (QFile::exists(p)) {
            cached = p;
            return cached;
        }
    }

    // 3) Qt 的系统根证书 → 临时 PEM
    const QList<QSslCertificate> cas = QSslConfiguration::systemCaCertificates();
    if (cas.isEmpty())
        return QString();
    QByteArray pem;
    for (const QSslCertificate &c : cas)
        pem += c.toPem();
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    const QString path = QDir(dir).filePath(QStringLiteral("jm-system-ca.pem"));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write(pem);
    f.close();
    cached = path;
    return cached;
}

}  // namespace

QString EchStatusText(int status)
{
    switch (status) {
    case 1: return QStringLiteral("SUCCESS");
    case 2: return QStringLiteral("GREASE");
    case 3: return QStringLiteral("GREASE_ECH");
    case 4: return QStringLiteral("BACKEND");
    case 0: return QStringLiteral("FAILED");
    case -101: return QStringLiteral("NOT_TRIED");
    case -102: return QStringLiteral("BAD_NAME");
    case -103: return QStringLiteral("NOT_CONFIGURED");
    case -105: return QStringLiteral("FAILED_ECH");
    case -106: return QStringLiteral("FAILED_ECH_BAD_NAME");
    default: return QString::number(status);
    }
}

bool EchChannelAvailable()
{
    return Api().ok;
}

QString EchChannelStatus()
{
    const SslApi &api = Api();
    return api.ok ? QStringLiteral("OpenSSL %1 @ %2").arg(api.version, api.dir)
                  : api.error;
}

namespace {

class OpenSslEchChannel : public TlsChannel
{
public:
    explicit OpenSslEchChannel(QObject *parent) : TlsChannel(parent) {}
    ~OpenSslEchChannel() override { Cleanup(); }

    bool Connect(const QString &host, quint16 port, const QString &sni,
                 const QByteArray &echConfigList, const QStringList &alpn) override
    {
        if (!Api().ok) {
            emit Error(Api().error);
            return false;
        }
        host_ = host;
        sni_ = sni;
        echConfig_ = echConfigList;
        alpn_ = alpn;

        sock_ = new QTcpSocket(this);
        connect(sock_, &QTcpSocket::connected, this, [this] { StartTls(); });
        connect(sock_, &QTcpSocket::readyRead, this, [this] { OnSocketData(); });
        connect(sock_, &QTcpSocket::disconnected, this, [this] {
            if (!closed_) {
                closed_ = true;
                emit Disconnected();
            }
        });
        connect(sock_, &QTcpSocket::errorOccurred, this,
                [this](QAbstractSocket::SocketError e) {
                    if (e == QAbstractSocket::RemoteHostClosedError) {
                        if (!closed_) {
                            closed_ = true;
                            emit Disconnected();
                        }
                        return;
                    }
                    Fail(sock_->errorString());
                });
        sock_->connectToHost(host, port);
        return true;
    }

    void Write(const QByteArray &data) override
    {
        if (!info_.handshakeOk) {
            pending_ += data;
            return;
        }
        WritePlain(data);
    }

    QByteArray ReadAll() override
    {
        const QByteArray out = in_;
        in_.clear();
        return out;
    }

    void Close() override
    {
        if (closed_)
            return;
        closed_ = true;
        if (ssl_ && Api().SSL_shutdown)
            Api().SSL_shutdown(ssl_);
        FlushWbio();
        if (sock_)
            sock_->disconnectFromHost();
    }

    TlsInfo Info() const override { return info_; }
    bool SupportsEch() const override { return true; }

private:
    QString LastError()
    {
        const unsigned long e = Api().ERR_get_error ? Api().ERR_get_error() : 0;
        if (!e)
            return QStringLiteral("ssl error");
        char buf[256] = {0};
        Api().ERR_error_string_n(e, buf, sizeof(buf));
        return QString::fromLatin1(buf);
    }

    void Cleanup()
    {
        if (ssl_) {
            Api().SSL_free(ssl_);   // 同时释放 rbio/wbio
            ssl_ = nullptr;
            rbio_ = nullptr;
            wbio_ = nullptr;
        }
        if (ctx_) {
            Api().SSL_CTX_free(ctx_);
            ctx_ = nullptr;
        }
    }

    void Fail(const QString &text)
    {
        if (failed_)
            return;
        failed_ = true;
        info_.error = text;
        emit Log(QStringLiteral("tls fail: %1").arg(text));
        emit Error(text);
        Close();
    }

    void StartTls()
    {
        SslApi &api = Api();
        ctx_ = api.SSL_CTX_new(api.TLS_client_method());
        if (!ctx_) {
            Fail(QStringLiteral("SSL_CTX_new: %1").arg(LastError()));
            return;
        }
        // ECH 只在 TLS 1.3 有效；不锁版本时 ClientHello 可能退到 1.2，
        // 带 ECH 扩展会被服务端以 illegal_parameter 拒绝（实测）。
        api.SSL_CTX_ctrl(ctx_, 123 /*SSL_CTRL_SET_MIN_PROTO_VERSION*/,
                         0x0304 /*TLS1_3_VERSION*/, nullptr);
        api.SSL_CTX_set_verify(ctx_, kSslVerifyPeer, nullptr);
        const QString ca = CaBundlePath();
        int caLoaded = 0;
        if (!ca.isEmpty())
            caLoaded = api.SSL_CTX_load_verify_locations(ctx_, ca.toUtf8().constData(), nullptr);
        if (caLoaded != 1)
            api.SSL_CTX_set_default_verify_paths(ctx_);
        emit Log(QStringLiteral("tls ca path=%1 load=%2")
                     .arg(ca.isEmpty() ? QStringLiteral("-") : ca)
                     .arg(caLoaded));

        ssl_ = api.SSL_new(ctx_);
        if (!ssl_) {
            Fail(QStringLiteral("SSL_new: %1").arg(LastError()));
            return;
        }
        if (!echConfig_.isEmpty()) {
            const int r = api.SSL_set1_ech_config_list(
                ssl_, reinterpret_cast<const uint8_t *>(echConfig_.constData()),
                size_t(echConfig_.size()));
            emit Log(QStringLiteral("tls ech config len=%1 set=%2")
                         .arg(echConfig_.size())
                         .arg(r));
        }
        const QByteArray inner = sni_.toUtf8();
        const QString outerName = echConfig_.isEmpty() ? QString() : ParseEchPublicName(echConfig_);
        bool namesSet = false;
        if (!outerName.isEmpty() && api.SSL_ech_set1_server_names) {
            // ECH：内层是真实域名，外层用 ECHConfig 的 public_name。
            // 只设 SSL_ctrl(SNI, 真实域名) 会被服务端视为 outer/inner 不一致，
            // 实测 Cloudflare 直接回 TLS alert illegal_parameter。
            const QByteArray outer = outerName.toUtf8();
            const int r = api.SSL_ech_set1_server_names(ssl_, inner.constData(),
                                                        outer.constData(), 0);
            emit Log(QStringLiteral("tls ech names inner=%1 outer=%2 set=%3")
                         .arg(sni_, outerName)
                         .arg(r));
            namesSet = (r == 1);
        }
        if (!namesSet) {
            api.SSL_ctrl(ssl_, kSslCtrlSetSni, 0,
                         const_cast<char *>(inner.constData()));   // 普通 SNI
        }

        if (!alpn_.isEmpty()) {
            QByteArray protos;
            for (const QString &p : alpn_) {
                const QByteArray b = p.toLatin1();
                protos.append(char(b.size()));
                protos.append(b);
            }
            api.SSL_set_alpn_protos(ssl_,
                                    reinterpret_cast<const unsigned char *>(protos.constData()),
                                    unsigned(protos.size()));
            if (!echConfig_.isEmpty() && api.SSL_ech_set1_outer_alpn_protos) {
                // ECH 外层 ClientHello 必须带 outer ALPN；缺了 Cloudflare 会回
                // TLS alert illegal_parameter（实测 s_client 加 -ech_outer_alpn 才成功）。
                const int r = api.SSL_ech_set1_outer_alpn_protos(
                    ssl_, reinterpret_cast<const unsigned char *>(protos.constData()),
                    size_t(protos.size()));
                emit Log(QStringLiteral("tls ech outer alpn=%1 set=%2")
                             .arg(alpn_.join(QLatin1Char(',')))
                             .arg(r));
            }
        }

        rbio_ = api.BIO_new(api.BIO_s_mem());
        wbio_ = api.BIO_new(api.BIO_s_mem());
        if (!rbio_ || !wbio_) {
            Fail(QStringLiteral("BIO_new failed"));
            return;
        }
        api.SSL_set_bio(ssl_, rbio_, wbio_);   // 所有权交给 SSL
        api.SSL_set_connect_state(ssl_);
        DoHandshake();
    }

    void DoHandshake()
    {
        for (;;) {
            const int r = Api().SSL_do_handshake(ssl_);
            FlushWbio();
            if (r == 1) {
                OnHandshakeDone();
                return;
            }
            const int e = Api().SSL_get_error(ssl_, r);
            if (e == kErrorWantRead)
                return;   // 等 socket 数据
            if (e == kErrorWantWrite)
                continue;
            Fail(QStringLiteral("handshake: %1").arg(LastError()));
            return;
        }
    }

    void OnHandshakeDone()
    {
        SslApi &api = Api();
        info_.handshakeOk = true;
        info_.version = QString::fromLatin1(api.SSL_get_version(ssl_));

        const unsigned char *alpn = nullptr;
        unsigned int alpnLen = 0;
        api.SSL_get0_alpn_selected(ssl_, &alpn, &alpnLen);
        if (alpn && alpnLen)
            info_.alpn = QString::fromLatin1(reinterpret_cast<const char *>(alpn), int(alpnLen));

        char *inner = nullptr;
        char *outer = nullptr;
        info_.echStatus = api.SSL_ech_get1_status(ssl_, &inner, &outer);
        info_.echText = EchStatusText(info_.echStatus);

        info_.verifyResult = api.SSL_get_verify_result(ssl_);
        X509 *cert = api.SSL_get1_peer_certificate(ssl_);
        int hostOk = 0;
        if (cert) {
            const QByteArray sni = sni_.toUtf8();
            hostOk = api.X509_check_host(cert, sni.constData(), size_t(sni.size()), 0, nullptr);
            api.X509_free(cert);
        }
        info_.verifyOk = (info_.verifyResult == 0 && hostOk == 1);

        emit Log(QStringLiteral("tls handshake ok version=%1 alpn=%2 ech=%3 verify=%4/%5")
                     .arg(info_.version,
                          info_.alpn.isEmpty() ? QStringLiteral("-") : info_.alpn, info_.echText,
                          QString::number(info_.verifyResult), QString::number(hostOk)));

        if (!info_.verifyOk) {
            Fail(QStringLiteral("certificate verify failed: result=%1 host=%2")
                     .arg(info_.verifyResult)
                     .arg(hostOk));
            return;
        }
        emit Connected();
        if (!pending_.isEmpty()) {
            const QByteArray p = pending_;
            pending_.clear();
            WritePlain(p);
        }
    }

    void OnSocketData()
    {
        if (!sock_ || closed_)
            return;
        const QByteArray data = sock_->readAll();
        if (!data.isEmpty() && rbio_)
            Api().BIO_write(rbio_, data.constData(), data.size());
        if (!info_.handshakeOk) {
            DoHandshake();
            return;
        }
        PumpRead();
    }

    void PumpRead()
    {
        char buf[16384];
        bool got = false;
        bool peerClosed = false;
        for (;;) {
            const int n = Api().SSL_read(ssl_, buf, int(sizeof(buf)));
            if (n > 0) {
                in_.append(buf, n);
                got = true;
                continue;
            }
            const int e = Api().SSL_get_error(ssl_, n);
            if (e == kErrorWantRead || e == kErrorWantWrite)
                break;
            if (e == kErrorZeroReturn) {
                peerClosed = true;
                break;
            }
            Fail(QStringLiteral("read: %1").arg(LastError()));
            return;
        }
        // 必须先交付明文再通知断连：否则上层在 Disconnected 时看不到最后一批
        // 数据，会把完整响应当成 "closed mid-body"（实测 ECH + chunked 必现）。
        if (got)
            emit ReadyRead();
        if (peerClosed && !closed_) {
            closed_ = true;
            emit Disconnected();
        }
    }

    void WritePlain(const QByteArray &data)
    {
        int off = 0;
        while (off < data.size()) {
            const int n = Api().SSL_write(ssl_, data.constData() + off, data.size() - off);
            FlushWbio();
            if (n <= 0) {
                const int e = Api().SSL_get_error(ssl_, n);
                if (e == kErrorWantRead || e == kErrorWantWrite)
                    continue;
                Fail(QStringLiteral("write: %1").arg(LastError()));
                return;
            }
            off += n;
        }
    }

    void FlushWbio()
    {
        if (!wbio_ || !sock_)
            return;
        for (;;) {
            const long pending = Api().BIO_ctrl(wbio_, kBioCtrlPending, 0, nullptr);
            if (pending <= 0)
                break;
            QByteArray buf(int(pending), 0);
            const int n = Api().BIO_read(wbio_, buf.data(), int(pending));
            if (n <= 0)
                break;
            sock_->write(buf.constData(), n);
        }
    }

    QTcpSocket *sock_ = nullptr;
    SSL_CTX *ctx_ = nullptr;
    SSL *ssl_ = nullptr;
    BIO *rbio_ = nullptr;
    BIO *wbio_ = nullptr;
    QByteArray in_;
    QByteArray pending_;
    QByteArray echConfig_;
    QStringList alpn_;
    QString host_;
    QString sni_;
    TlsInfo info_;
    bool closed_ = false;
    bool failed_ = false;
};

}  // namespace

TlsChannel *CreateEchChannel(QObject *parent)
{
    if (!Api().ok)
        return nullptr;
    return new OpenSslEchChannel(parent);
}
