// HTTP/3 over QUIC 客户端实现。
//
// 结构（自下而上）：
//   QUdpSocket             —— UDP 收发与本地地址选择
//   OpenSSL 4.x (QUIC API) —— TLS1.3 握手、ECH、会话票据、密钥导出（SSL_set_quic_tls_cbs）
//   ngtcp2                 —— 连接状态机、丢包重传、流控、连接迁移、0-RTT
//   nghttp3                —— HTTP/3 帧层（SETTINGS/QPACK/HEADERS/DATA）
//
// Q2：连接按 (host, port, echConfig 指纹) 池化，一条连接并发多流。
//     Connection 拥有 socket/TLS/ngtcp2/nghttp3；Job 是一条 bidi 流。
//     连接级失败只让该连接上的在飞请求失败（由 NodeHttp 逐请求回退 TLS）。
// Q1：会话票据按 host 落盘；票据可用时 SSL_set_session + early data，
//     服务端拒绝则丢弃 0-RTT 数据并在 1-RTT 重发（仅幂等 GET）。
// Q3：分段计时输出一行 h3-timing（解析/UDP/握手/首字节），qlog 由环境变量开启。
//
// 关键点（都是踩过的坑）：
//   * QUIC 必须 TLS1.3，且 ALPN 必须是 h3；内层/外层 ALPN 都要给（走 ECH 时）。
//   * ECH 需要 SSL_ech_set1_server_names(inner=真实域名, outer=public_name)，
//     只设普通 SNI 会被 Cloudflare 回 illegal_parameter。
//   * ngtcp2 的 client_initial 回调会自动 kick 握手（conn_recv_crypto_data → SSL_do_handshake），
//     不需要手动调 SSL_do_handshake。
//   * 流数据必须经 nghttp3_conn_add_write_offset / extend_max_stream_offset 回报，
//     否则流控窗口不会推进，大响应会卡住。
//   * 0-RTT 必须先在 ngtcp2 侧装入上次记住的对端传输参数
//     （ngtcp2_conn_decode_and_set_0rtt_transport_params），否则 open_bidi_stream 直接失败。
//   * 不能在 ngtcp2/nghttp3 回调栈里销毁 conn/h3/Job：一律 deleteLater 或延迟一帧。
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

#include "quic_http.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QtGlobal>

#include <atomic>
#include <functional>

#ifdef JM_WITH_QUIC

#  include <QUdpSocket>

#  ifndef Q_OS_WIN
#    include <arpa/inet.h>
#    include <netinet/in.h>
#    include <sys/socket.h>
#    include <unistd.h>
#  endif

extern "C" {
#  include <nghttp3/nghttp3.h>
#  include <ngtcp2/ngtcp2.h>
#  include <ngtcp2/ngtcp2_crypto.h>
#  include <ngtcp2/ngtcp2_crypto_ossl.h>
}

#  include <openssl/ech.h>
#  include <openssl/err.h>
#  include <openssl/pem.h>
#  include <openssl/rand.h>
#  include <openssl/ssl.h>
#  include <openssl/x509v3.h>

#  include <algorithm>
#  include <array>
#  include <cstdarg>
#  include <cstdio>
#  include <cstring>
#  include <list>
#  include <mutex>
#  include <string>
#  include <vector>

namespace quic {
namespace {

constexpr size_t kMaxUdpPayload = 1452;
constexpr ngtcp2_duration kIdleTimeout = 30 * NGTCP2_SECONDS;
constexpr int kMaxStreamsBidi = 128;
// 应用侧背压：单连接同时承载的请求上限（对端 initial_max_streams_bidi 是 128，
// 留出余量避免把服务端流限额打满后整条连接卡住）
constexpr int kMaxJobsPerConn = 32;
constexpr int kMaxConnsPerKey = 2;
constexpr int kMaxConnsTotal = 24;
// 空闲关闭：最后一个请求完成后不立刻关（连续翻页会紧跟新请求），
// 但要短于 max_idle_timeout，避免池里堆着半死连接
constexpr int kIdleCloseMs = 15000;

// 进程内计数（Q1/Q2 的可观测性）：握手次数决定"池是否真的复用"，
// 0-RTT 三个计数决定"票据是否被接受"。全部为原子量：虽然当前所有 QUIC
// 回调都在主线程，但 --h3-probe 与页面请求可能在不同线程触发 Start。
std::atomic<int> g_handshakes{0};
std::atomic<int> g_reused{0};
std::atomic<int> g_zeroRttTried{0};
std::atomic<int> g_zeroRttAccepted{0};
std::atomic<int> g_zeroRttRejected{0};

ngtcp2_tstamp Now()
{
    using namespace std::chrono;
    return ngtcp2_tstamp(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

void EnsureCryptoInit()
{
    static std::once_flag once;
    std::call_once(once, [] { ngtcp2_crypto_ossl_init(); });
}

// ECHConfigList → 首个 ECHConfig 的 public_name（外层 SNI）。
// RFC 9849：list_len | version(2) len(2) config_id(1) kem_id(2) public_key(<2+len>)
//            cipher_suites(<2+len>) max_name_len(1) name(<1+len>) ...
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
            int p = start + 1 + 2;
            const int pk = be16(p);
            if (pk < 0 || p + 2 + pk > end)
                return QString();
            p += 2 + pk;
            const int cs = be16(p);
            if (cs < 0 || p + 2 + cs > end)
                return QString();
            p += 2 + cs;
            if (p + 1 > end)
                return QString();
            ++p;   // max_name_length
            if (p + 1 > end)
                return QString();
            const int nlen = quint8(list.at(p));
            ++p;
            if (p + nlen > end)
                return QString();
            return QString::fromUtf8(list.constData() + p, nlen);
        }
        off = end;
    }
    return QString();
}

QString EchText(int status)
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
    default: return QStringLiteral("status=%1").arg(status);
    }
}

// OpenSSL 的早数据状态：0=NOT_SENT 1=ACCEPTED 2=REJECTED
QString EarlyText(int status)
{
    switch (status) {
    case 1: return QStringLiteral("accepted");
    case 2: return QStringLiteral("rejected");
    case 0: return QStringLiteral("not_sent");
    default: return QStringLiteral("status=%1").arg(status);
    }
}

QByteArray Fingerprint(const QByteArray &data)
{
    if (data.isEmpty())
        return QByteArray();
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).left(8).toHex();
}

QString CaBundleSource()
{
    static QString cached;
    if (!cached.isEmpty())
        return cached;
    const QByteArray env = qgetenv("JM_CA_BUNDLE");
    QList<QString> candidates;
    if (!env.isEmpty())
        candidates << QStringLiteral("env:JM_CA_BUNDLE")
                   << QString::fromLocal8Bit(env);
    candidates << QStringLiteral("git:ca-bundle.crt")
               << QStringLiteral("C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt");
    candidates << QStringLiteral("git:ssl/certs/ca-bundle.crt")
               << QStringLiteral("C:/Program Files/Git/mingw64/ssl/certs/ca-bundle.crt");
    for (int i = 0; i + 1 < candidates.size(); i += 2) {
        if (QFile::exists(candidates.at(i + 1))) {
            cached = candidates.at(i) + QStringLiteral("@") + candidates.at(i + 1);
            return cached;
        }
    }
    cached = QStringLiteral("default:SSL_CTX_set_default_verify_paths");
    return cached;
}

// 返回实际使用的 CA 来源，供排障判断信任链（T2 核查点 3）
// 从 Qt 资源里的 CA bundle 逐个加进 X509_STORE。
// Android 上这是唯一可控的信任锚来源：OpenSSL 的默认目录在构建机上，
// 系统 cacerts 目录在部分镜像是不可读的（读得到但一枚也加载不进来）。
bool LoadCaFromResource(SSL_CTX *ctx)
{
    QFile f(QStringLiteral(":/tls/cacert.pem"));
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QByteArray pem = f.readAll();
    f.close();
    if (pem.isEmpty())
        return false;
    BIO *bio = BIO_new_mem_buf(pem.constData(), int(pem.size()));
    if (!bio)
        return false;
    X509_STORE *store = SSL_CTX_get_cert_store(ctx);
    int added = 0;
    for (;;) {
        X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr);
        if (!cert)
            break;
        if (X509_STORE_add_cert(store, cert) == 1)
            ++added;
        X509_free(cert);
    }
    BIO_free(bio);
    return added > 0;
}

QString LoadCaBundle(SSL_CTX *ctx)
{
    const QByteArray env = qgetenv("JM_CA_BUNDLE");
    if (!env.isEmpty()) {
        const QString p = QString::fromLocal8Bit(env);
        if (QFile::exists(p)
            && SSL_CTX_load_verify_locations(ctx, p.toUtf8().constData(), nullptr) == 1) {
            return p;
        }
    }
#ifdef Q_OS_ANDROID
    // 优先用构建期嵌入的 bundle（确定、可复现）；系统目录作为兜底。
    if (LoadCaFromResource(ctx))
        return QStringLiteral("resource:/tls/cacert.pem");
    // Android 没有单一 CA 文件：系统信任库是目录，每个证书一个 PEM。
    // 不补这一步，QUIC 侧会因为没有信任锚而握手失败（verify=20），
    // 而 QSslSocket 走 Qt 自己的加载逻辑，表现不一致。
    for (const QString &dir : {QStringLiteral("/apex/com.android.conscrypt/cacerts"),
                               QStringLiteral("/system/etc/security/cacerts")}) {
        if (QFileInfo(dir).isDir()
            && SSL_CTX_load_verify_locations(ctx, nullptr, dir.toUtf8().constData()) == 1) {
            return dir;
        }
    }
#else
    const QList<QString> candidates = {
        QStringLiteral("C:/Program Files/Git/mingw64/etc/ssl/certs/ca-bundle.crt"),
        QStringLiteral("C:/Program Files/Git/mingw64/ssl/certs/ca-bundle.crt"),
    };
    for (const QString &p : candidates) {
        if (QFile::exists(p)
            && SSL_CTX_load_verify_locations(ctx, p.toUtf8().constData(), nullptr) == 1) {
            return p;
        }
    }
#endif
    SSL_CTX_set_default_verify_paths(ctx);
    return QStringLiteral("system-default");
}

bool MakeSockAddr(const QHostAddress &addr, quint16 port, sockaddr_storage &ss, socklen_t &len)
{
    std::memset(&ss, 0, sizeof(ss));
    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        auto *sin = reinterpret_cast<sockaddr_in *>(&ss);
        sin->sin_family = AF_INET;
        sin->sin_port = htons(port);
        sin->sin_addr.s_addr = htonl(addr.toIPv4Address());
        len = sizeof(sockaddr_in);
        return true;
    }
    if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        auto *sin6 = reinterpret_cast<sockaddr_in6 *>(&ss);
        sin6->sin6_family = AF_INET6;
        sin6->sin6_port = htons(port);
        const Q_IPV6ADDR a = addr.toIPv6Address();
        std::memcpy(&sin6->sin6_addr, &a, 16);
        len = sizeof(sockaddr_in6);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// 会话票据存储（Q1）
//
// 目录：<state>/h3/，<host>.pem 是 SSL_SESSION 的 PEM，<host>.meta 是失效判据：
//   exp=<unix 秒>            票据过期时间
//   ech=<hex>                ECHConfigList 指纹（config_id 变化即换值）
//   cert=<hex>               建票据时对端叶证书指纹（证书轮换即作废）
//   tp=<hex>                 上次记住的 0-RTT 传输参数（ngtcp2 编码）
// 目录用 JM_UI_CONFIG 派生，和代理配置同源，回归套件因此天然隔离。
// ---------------------------------------------------------------------------
QString StateDir()
{
    QString base = qEnvironmentVariable("JM_UI_CONFIG");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    const QString dir = base + QStringLiteral("/h3");
    QDir().mkpath(dir);
    return dir;
}

// 信任锚指纹：路径 + 大小 + mtime。用于把会话票据绑定到"当时用来校验的 CA"。
// 不这样做的话，换掉 JM_CA_BUNDLE 后仍会复用旧票据——TLS1.3 的会话复用不会
// 重新下发证书链，OpenSSL 也就不会重新校验（2026-11-07 实测：错误 CA 下仍
// 握手成功，这条就是安全隐患，见 docs/cpp_tls_quic_security_review.md §1）。
QByteArray CaFingerprint(const QString &caSource)
{
    if (caSource.isEmpty())
        return QByteArray();
    const QFileInfo fi(caSource);
    const QByteArray raw = caSource.toUtf8() + '|' + QByteArray::number(fi.size()) + '|'
                           + QByteArray::number(fi.lastModified().toSecsSinceEpoch());
    return QCryptographicHash::hash(raw, QCryptographicHash::Sha256).left(8).toHex();
}

QString HostFileBase(const QString &host)
{
    QString safe;
    safe.reserve(host.size());
    for (const QChar c : host) {
        if (c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('-'))
            safe += c;
        else
            safe += QLatin1Char('_');
    }
    if (safe.isEmpty())
        safe = QStringLiteral("host");
    return StateDir() + QLatin1Char('/') + safe;
}

void DropTicket(const QString &host);

struct Ticket
{
    QByteArray pem;
    QByteArray tp;
    QByteArray echFp;
    QByteArray certFp;
    QByteArray caFp;     // 信任锚指纹：换 CA（JM_CA_BUNDLE）即作废
    qint64 expires = 0;

    bool isExpired() const
    {
        return expires > 0 && expires <= QDateTime::currentSecsSinceEpoch();
    }
};

Ticket LoadTicket(const QString &host, const QByteArray &echFp, const QByteArray &caFp)
{
    Ticket t;
    const QString base = HostFileBase(host);
    QFile meta(base + QStringLiteral(".meta"));
    if (!meta.open(QIODevice::ReadOnly))
        return t;
    const QList<QByteArray> lines = meta.readAll().split('\n');
    meta.close();
    for (const QByteArray &line : lines) {
        const int eq = line.indexOf('=');
        if (eq <= 0)
            continue;
        const QByteArray key = line.left(eq).trimmed();
        const QByteArray val = line.mid(eq + 1).trimmed();
        if (key == QByteArrayLiteral("exp"))
            t.expires = val.toLongLong();
        else if (key == QByteArrayLiteral("ech"))
            t.echFp = val;
        else if (key == QByteArrayLiteral("cert"))
            t.certFp = val;
        else if (key == QByteArrayLiteral("tp"))
            t.tp = QByteArray::fromHex(val);
        else if (key == QByteArrayLiteral("ca"))
            t.caFp = val;
    }
    QFile pem(base + QStringLiteral(".pem"));
    if (!pem.open(QIODevice::ReadOnly))
        return t;
    t.pem = pem.readAll();
    pem.close();

    if (t.pem.isEmpty())
        return t;
    // ECH 配置变更（config_id 换过）后旧票据不可复用：SNI/前端不同，票据也无效
    if (t.echFp != echFp) {
        DropTicket(host);
        return Ticket();
    }
    // 信任锚变了就不能复用：旧票据是"当时那套 CA"认证过的会话
    if (caFp.isEmpty() || t.caFp != caFp) {
        DropTicket(host);
        return Ticket();
    }
    if (t.isExpired()) {
        DropTicket(host);
        return Ticket();
    }
    return t;
}

void SaveTicket(const QString &host, const QByteArray &echFp, const QByteArray &certFp,
                const QByteArray &caFp, SSL_SESSION *sess, const QByteArray &tp)
{
    if (!sess || host.isEmpty())
        return;
    BIO *bio = BIO_new(BIO_s_mem());
    if (!bio)
        return;
    if (PEM_write_bio_SSL_SESSION(bio, sess) != 1) {
        const unsigned long e = ERR_get_error();
        char ebuf[256] = {0};
        if (e)
            ERR_error_string_n(e, ebuf, sizeof(ebuf));
        qWarning("h3 ticket pem failed host=%s err=%s", qPrintable(host), ebuf);
        BIO_free(bio);
        return;
    }
    BUF_MEM *mem = nullptr;
    BIO_get_mem_ptr(bio, &mem);
    const QByteArray pem = (mem && mem->data) ? QByteArray(mem->data, int(mem->length))
                                              : QByteArray();
    BIO_free(bio);
    if (pem.isEmpty())
        return;

    const QString base = HostFileBase(host);
    const qint64 expires = qint64(SSL_SESSION_get_time(sess))
                           + qint64(SSL_SESSION_get_timeout(sess));

    QSaveFile pemFile(base + QStringLiteral(".pem"));
    if (!pemFile.open(QIODevice::WriteOnly)) {
        qWarning("h3 ticket write failed %s: %s", qPrintable(pemFile.fileName()),
                 qPrintable(pemFile.errorString()));
        return;
    }
    pemFile.write(pem);
    pemFile.commit();

    QByteArray meta;
    meta += "exp=" + QByteArray::number(expires) + '\n';
    meta += "ech=" + echFp + '\n';
    meta += "cert=" + certFp + '\n';
    meta += "ca=" + caFp + '\n';
    meta += "tp=" + tp.toHex() + '\n';
    QSaveFile metaFile(base + QStringLiteral(".meta"));
    if (!metaFile.open(QIODevice::WriteOnly))
        return;
    metaFile.write(meta);
    metaFile.commit();
}

void DropTicket(const QString &host)
{
    const QString base = HostFileBase(host);
    QFile::remove(base + QStringLiteral(".pem"));
    QFile::remove(base + QStringLiteral(".meta"));
}

QByteArray PeerCertFingerprint(SSL *ssl)
{
    X509 *cert = SSL_get1_peer_certificate(ssl);
    if (!cert)
        return QByteArray();
    unsigned char md[EVP_MAX_MD_SIZE] = {0};
    unsigned int mdlen = 0;
    QByteArray fp;
    if (X509_digest(cert, EVP_sha256(), md, &mdlen) == 1 && mdlen >= 8)
        fp = QByteArray(reinterpret_cast<const char *>(md), 8).toHex();
    X509_free(cert);
    return fp;
}

bool QlogEnabled()
{
    static const bool on = !qEnvironmentVariableIsEmpty("JM_H3_QLOG");
    return on;
}

bool Verbose()
{
    // 默认静默：ngtcp2 的 INFO 日志一次握手上百行，生产路径只需要结果。
    // 排障时设 JM_H3_VERBOSE=1。
    static const bool verbose = !qEnvironmentVariableIsEmpty("JM_H3_VERBOSE");
    return verbose;
}

}   // namespace

class Connection;

// 一条请求 = 一条 client-initiated bidi 流
struct Job
{
    Request req;
    Client::Callback cb;
    std::function<void(const QString &)> logSink;

    Connection *conn = nullptr;
    int64_t streamId = -1;
    QTimer *deadline = nullptr;

    QElapsedTimer clock;       // 从进入池开始
    qint64 queueMs = 0;        // 排队等待连接/流限额的时间
    qint64 submitAtMs = -1;    // 进入连接（Start 之后）的时刻
    qint64 ttfbMs = 0;         // 首个响应字节相对 submitAt 的时间

    QByteArray path;
    QByteArray bodyOut;
    QByteArray body;
    bool truncated = false;
    bool finished = false;
    bool earlySent = false;    // 在握手中用 0-RTT 发出
    bool reusedConn = false;
    int status = 0;
    QString location;
    std::vector<std::string> nvStore;
};

// 一条 QUIC 连接：UDP + TLS + ngtcp2 + nghttp3 + 定时器。
// 内部类，成员对 .cpp 内的 C 回调全部公开（ngtcp2 用 void* 回调）。
class Connection : public QObject
{
public:
    struct Key
    {
        QString host;
        quint16 port = 443;
        QByteArray echFp;          // ECHConfigList 指纹（池化键 + 票据失效判据）
        QByteArray echConfigList;  // 实际注入的 ECHConfigList
        QString connectIp;         // 池化键不含它：同域换 IP 仍复用一条连接
    };

    explicit Connection(const Key &key, QObject *parent = nullptr);
    ~Connection() override;

    bool Start();

    // 接受一个请求。false = 已关闭或达到并发上限（调用方排队）。
    bool Submit(Job *job);
    // 取消单个请求（超时/调用方放弃），只影响该流
    void Abort(Job *job, const QString &why);

    void CloseNow(const QString &why);
    // CloseNow 的收尾半段：CONNECTION_CLOSE 发送 + 资源释放 + onClosed。
    // 必须在事件循环里跑，不能留在读包/超时回调栈上（见实现内注释）。
    void Teardown();

    bool usable() const { return started_ && !closing_ && !finished_; }
    bool handshakeDone() const { return handshakeDone_; }
    bool reused() const { return reusedSession_; }
    int load() const { return int(jobs.size() + pending.size()); }
    const Key &key() const { return key_; }
    QString lastError() const { return error_; }
    qint64 handshakeMs() const { return hsMs_; }
    qint64 udpMs() const { return udpMs_; }
    qint64 resolveMs() const { return resolveMs_; }

    void Log(const QString &line);
    void FailAll(const QString &why);

    // Pool 注入的回调
    std::function<void(Connection *, Job *)> onJobDone;
    std::function<void(Connection *)> onClosed;

    // ---- 内部状态（C 回调直接访问）----
    QUdpSocket *udp = nullptr;
    QTimer *timer = nullptr;
    QTimer *idle = nullptr;

    SSL_CTX *ctx = nullptr;
    SSL *ssl = nullptr;
    ngtcp2_crypto_ossl_ctx *ossl = nullptr;
    ngtcp2_crypto_conn_ref connRef{};
    ngtcp2_conn *conn = nullptr;
    nghttp3_conn *h3 = nullptr;

    ngtcp2_path_storage ps{};
    sockaddr_storage remote{};
    socklen_t remoteLen = 0;
    sockaddr_storage local{};
    socklen_t localLen = 0;

    QHash<int64_t, Job *> jobs;
    QList<Job *> pending;

    int txLog_ = 0;
    int rxLog_ = 0;
    bool finished_ = false;
    bool closing_ = false;
    bool started_ = false;
    bool h3Ready_ = false;
    bool handshakeDone_ = false;
    bool reusedSession_ = false;
    bool earlyEligible_ = false;   // 票据允许 early data
    bool zeroRttArmed_ = false;    // 0-RTT 传输参数已装入
    bool earlyUsed_ = false;       // 真的有请求走了 0-RTT
    int earlyBudget_ = 0;          // 本连接还允许几条流走 0-RTT（只给首个请求）

    QByteArray ticketTp_;
    QByteArray ticketCertFp_;
    QByteArray certFp_;
    QByteArray echFp_;
    QByteArray caFp_;

    QString tlsVersion_;
    QString echText_;
    // SSL_ech_get1_status() 的原始值；握手未完成时在 FinishJob 里再读一次
    // （ssl 此时尚未被 Cleanup 释放）。1 = SUCCESS 才算真正隐藏了 SNI。
    int echStatus_ = -103;
    QString alpn_;
    QString earlyText_;
    QString error_;
    QByteArray tpEncoded_;

    QElapsedTimer clock_;   // 连接级计时（Q3）
    qint64 resolveMs_ = 0;
    qint64 udpMs_ = 0;
    qint64 hsMs_ = 0;

    QString poolKey;        // 池里的键（由 Pool 填），关闭时用它把自己摘出去

    void EnsureH3();
    bool SubmitJob(Job *job);
    void WriteStreams();
    void ScheduleTimer();
    void OnTimer();
    void OnReadable();
    void OnHandshake();
    void FinishJob(Job *job, Response r);
    void Cleanup();
    void SaveSession(SSL_SESSION *sess);
    void WriteQlog(const void *data, size_t datalen);
    QFile *qlogFile = nullptr;

private:
    Key key_;
    bool keyInit_ = false;
};

namespace {

using Conn = Connection;

int OnHandshakeCompleted(ngtcp2_conn *, void *user_data)
{
    static_cast<Conn *>(user_data)->OnHandshake();
    return 0;
}

int OnRecvStreamData(ngtcp2_conn *conn, uint32_t flags, int64_t stream_id,
                     uint64_t /*offset*/, const uint8_t *data, size_t datalen,
                     void *user_data, void * /*stream_user_data*/)
{
    auto *c = static_cast<Conn *>(user_data);
    if (!c->h3)
        return 0;
    const nghttp3_ssize nconsumed = nghttp3_conn_read_stream2(
        c->h3, stream_id, data, datalen, flags & NGTCP2_STREAM_DATA_FLAG_FIN,
        ngtcp2_conn_get_timestamp(c->conn));
    if (nconsumed < 0) {
        c->Log(QStringLiteral("h3 read_stream: %1")
                   .arg(QString::fromLatin1(nghttp3_strerror(int(nconsumed)))));
        return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    ngtcp2_conn_extend_max_stream_offset(conn, stream_id, uint64_t(nconsumed));
    ngtcp2_conn_extend_max_offset(conn, uint64_t(nconsumed));
    return 0;
}

int OnAckedStreamDataOffset(ngtcp2_conn *, int64_t stream_id, uint64_t /*offset*/,
                            uint64_t datalen, void *user_data, void *)
{
    auto *c = static_cast<Conn *>(user_data);
    if (c->h3 && stream_id >= 0) {
        if (nghttp3_conn_add_ack_offset(c->h3, stream_id, datalen) != 0)
            return NGTCP2_ERR_CALLBACK_FAILURE;
    }
    return 0;
}

int OnStreamClose(ngtcp2_conn *, uint32_t /*flags*/, int64_t stream_id,
                  uint64_t app_error_code, void *user_data, void * /*stream_user_data*/)
{
    auto *c = static_cast<Conn *>(user_data);
    if (c->finished_)
        return 0;
    Job *job = c->jobs.value(stream_id, nullptr);
    if (c->h3)
        nghttp3_conn_close_stream2(c->h3, NGHTTP3_STREAM_CLOSE_FLAG_NONE, stream_id, 0, 0);
    if (!job || job->finished)
        return 0;
    if (app_error_code != 0) {
        c->Abort(job, QStringLiteral("h3 stream closed code=%1").arg(app_error_code));
    } else if (job->status > 0) {
        Response r;
        r.ok = true;
        r.status = job->status;
        r.body = job->body;
        r.truncated = job->truncated;
        r.location = job->location;
        c->FinishJob(job, r);
    } else {
        c->Abort(job, QStringLiteral("h3 stream closed without response"));
    }
    return 0;
}

int OnExtendMaxStreamsBidi(ngtcp2_conn *, uint64_t, void *user_data)
{
    auto *c = static_cast<Conn *>(user_data);
    if (!c->h3Ready_)
        return 0;
    // 流限额可能晚于 handshake_completed 到达：这里放行排队请求。
    // 同样是"先摘再提交"：否则 SubmitJob 的日志会被 pending + jobs 各发一次。
    const QList<Job *> waiting = c->pending;
    c->pending.clear();
    for (Job *job : waiting) {
        if (!c->SubmitJob(job))
            c->pending.append(job);
    }
    return 0;
}

int OnExtendMaxStreamsUni(ngtcp2_conn *, uint64_t, void *)
{
    return 0;
}

// 0-RTT 被服务端拒绝。实测（Cloudflare）：ngtcp2 会把 0-RTT 期间发出的流数据
// 自动在 1-RTT 重传，请求照常拿到响应（2026-11-07 实测 301/167B，total 319ms）。
// 因此这里只记日志、不手工重建流——手工重发会变成"同一个请求两条流"，
// 对端会看到重复请求（对 GET 是浪费，对将来的 POST 是数据重复）。
int OnTlsEarlyDataRejected(ngtcp2_conn *, void *user_data)
{
    auto *c = static_cast<Conn *>(user_data);
    c->Log(QStringLiteral("h3 0-rtt rejected by server (stream data re-sent in 1-RTT)"));
    return 0;
}

void OnLog(void *user_data, const char *fmt, ...)
{
    if (!Verbose())
        return;
    auto *c = static_cast<Conn *>(user_data);
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    c->Log(QStringLiteral("ngtcp2: %1").arg(QString::fromLatin1(buf)));
}

void OnQlog(void *user_data, uint32_t /*flags*/, const void *data, size_t datalen)
{
    auto *c = static_cast<Conn *>(user_data);
    c->WriteQlog(data, datalen);
}

void OnRand(uint8_t *dest, size_t destlen, const ngtcp2_rand_ctx *)
{
    RAND_bytes(dest, int(destlen));
}

int OnGetNewCid(ngtcp2_conn *, ngtcp2_cid *cid, ngtcp2_stateless_reset_token *token,
                size_t cidlen, void *)
{
    if (RAND_bytes(cid->data, int(cidlen)) != 1)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    cid->datalen = cidlen;
    if (RAND_bytes(token->data, sizeof(token->data)) != 1)
        return NGTCP2_ERR_CALLBACK_FAILURE;
    return 0;
}

// 新会话票据到达（TLS1.3 的 NST 在握手后由服务端补发）。
// 返回值 1 = 保留在 OpenSSL 内部缓存里，我们自己另存一份到磁盘。
int OnNewSession(SSL *ssl, SSL_SESSION *sess)
{
    auto *ref = static_cast<ngtcp2_crypto_conn_ref *>(SSL_get_app_data(ssl));
    auto *c = ref ? static_cast<Conn *>(ref->user_data) : nullptr;
    if (c)
        c->SaveSession(sess);
    return 1;
}

// ---- nghttp3 回调 ----

Job *JobOf(void *stream_user_data)
{
    return static_cast<Job *>(stream_user_data);
}

int OnH3RecvData(nghttp3_conn *, int64_t stream_id, const uint8_t *data, size_t datalen,
                 void *, void *stream_user_data)
{
    Job *job = JobOf(stream_user_data);
    if (!job || job->finished || stream_id != job->streamId)
        return 0;
    if (job->ttfbMs == 0 && job->submitAtMs >= 0 && job->conn)
        job->ttfbMs = qMax<qint64>(0, job->conn->clock_.elapsed() - job->submitAtMs);
    if (job->body.size() + qint64(datalen) > job->req.maxBytes) {
        const qint64 room = qMax<qint64>(0, job->req.maxBytes - job->body.size());
        job->body.append(reinterpret_cast<const char *>(data), int(room));
        job->truncated = true;
        if (job->conn)
            job->conn->Abort(job, QStringLiteral("body over maxBytes"));
        return 0;
    }
    job->body.append(reinterpret_cast<const char *>(data), int(datalen));
    return 0;
}

int OnH3DeferredConsume(nghttp3_conn *, int64_t stream_id, size_t nconsumed,
                        void *, void *)
{
    // 由 OnRecvStreamData 统一 extend；这里不能再重复一次，否则流控窗口翻倍。
    Q_UNUSED(stream_id);
    Q_UNUSED(nconsumed);
    return 0;
}

int OnH3RecvHeader(nghttp3_conn *, int64_t stream_id, int32_t /*token*/,
                   nghttp3_rcbuf *name, nghttp3_rcbuf *value, uint8_t /*flags*/,
                   void *, void *stream_user_data)
{
    Job *job = JobOf(stream_user_data);
    if (!job || job->finished || stream_id != job->streamId)
        return 0;
    const nghttp3_vec n = nghttp3_rcbuf_get_buf(name);
    const nghttp3_vec v = nghttp3_rcbuf_get_buf(value);
    const QByteArray hn(reinterpret_cast<const char *>(n.base), int(n.len));
    const QByteArray hv(reinterpret_cast<const char *>(v.base), int(v.len));
    if (job->ttfbMs == 0 && job->submitAtMs >= 0 && job->conn)
        job->ttfbMs = qMax<qint64>(0, job->conn->clock_.elapsed() - job->submitAtMs);
    if (hn == ":status") {
        bool okNum = false;
        const int st = hv.toInt(&okNum);
        if (okNum)
            job->status = st;
    } else if (hn == "location") {
        job->location = QString::fromUtf8(hv);
    }
    return 0;
}

int OnH3EndStream(nghttp3_conn *, int64_t stream_id, void *, void *stream_user_data)
{
    Job *job = JobOf(stream_user_data);
    if (!job || job->finished || stream_id != job->streamId)
        return 0;
    if (job->status <= 0) {
        if (job->conn)
            job->conn->Abort(job, QStringLiteral("h3 end_stream without status"));
        return 0;
    }
    Response r;
    r.ok = true;
    r.status = job->status;
    r.body = job->body;
    r.truncated = job->truncated;
    r.location = job->location;
    if (job->conn)
        job->conn->FinishJob(job, r);
    return 0;
}

int OnH3StopSending(nghttp3_conn *, int64_t stream_id, uint64_t app_error_code,
                    void *, void *)
{
    Q_UNUSED(stream_id);
    Q_UNUSED(app_error_code);
    return 0;
}

int OnH3ResetStream(nghttp3_conn *, int64_t stream_id, uint64_t app_error_code,
                    void *, void *)
{
    Q_UNUSED(stream_id);
    Q_UNUSED(app_error_code);
    return 0;
}

void OnH3Rand(uint8_t *dest, size_t destlen)
{
    RAND_bytes(dest, int(destlen));
}

nghttp3_ssize OnH3ReadData(nghttp3_conn *, int64_t, nghttp3_vec *vec, size_t veccnt,
                           uint32_t *pflags, void *, void *stream_user_data)
{
    Job *job = JobOf(stream_user_data);
    if (!job || job->bodyOut.isEmpty() || veccnt == 0)
        return 0;
    vec[0].base = reinterpret_cast<uint8_t *>(const_cast<char *>(job->bodyOut.constData()));
    vec[0].len = size_t(job->bodyOut.size());
    *pflags |= NGHTTP3_DATA_FLAG_EOF;
    return 1;
}

}   // namespace

Connection::Connection(const Key &key, QObject *parent) : QObject(parent), key_(key) {}

Connection::~Connection()
{
    finished_ = true;
    Cleanup();
    if (qlogFile) {
        qlogFile->close();
        delete qlogFile;
        qlogFile = nullptr;
    }
}

void Connection::Log(const QString &line)
{
    // 一条连接上的日志只投递一次：池化后同一连接挂着多个 Job，而每个 Job 的
    // logSink 通常指向同一个 Client（同一个页面），逐 Job 投递会把一行日志放大 N 倍。
    for (Job *job : jobs) {
        if (job && job->logSink) {
            job->logSink(line);
            return;
        }
    }
    for (Job *job : pending) {
        if (job && job->logSink) {
            job->logSink(line);
            return;
        }
    }
    qWarning("h3: %s", qPrintable(line));
}

bool Connection::Start()
{
    EnsureCryptoInit();
    clock_.start();
    echFp_ = key_.echFp;

    QHostAddress addr;
    if (!key_.connectIp.isEmpty()) {
        if (!addr.setAddress(key_.connectIp)) {
            error_ = QStringLiteral("bad ip %1").arg(key_.connectIp);
            return false;
        }
    } else {
        const QHostInfo info = QHostInfo::fromName(key_.host);
        if (info.addresses().isEmpty()) {
            error_ = QStringLiteral("dns failed %1").arg(key_.host);
            return false;
        }
        addr = info.addresses().first();
    }
    resolveMs_ = clock_.elapsed();

    if (!MakeSockAddr(addr, key_.port, remote, remoteLen)) {
        error_ = QStringLiteral("unsupported address");
        return false;
    }

    udp = new QUdpSocket(this);
    const bool v6 = addr.protocol() == QAbstractSocket::IPv6Protocol;
    if (!udp->bind(v6 ? QHostAddress::AnyIPv6 : QHostAddress::AnyIPv4, 0)) {
        error_ = QStringLiteral("udp bind: %1").arg(udp->errorString());
        return false;
    }
    udp->connectToHost(addr, key_.port);
    // 路径里的本地地址必须是内核实际选中的那个：Qt 在 bind(Any) 后
    // localAddress() 仍是 0.0.0.0，ngtcp2 用它做路径比对会丢包，所以问内核。
    localLen = sizeof(local);
#ifdef Q_OS_WIN
    const auto fd = static_cast<SOCKET>(udp->socketDescriptor());
#else
    const int fd = udp->socketDescriptor();   // Android/Linux: int
#endif
    if (::getsockname(fd, reinterpret_cast<sockaddr *>(&local), &localLen) != 0
        || localLen == 0) {
        QHostAddress localAddr = udp->localAddress();
        if (localAddr.isNull() || localAddr == QHostAddress::Any
            || localAddr == QHostAddress::AnyIPv4)
            localAddr = v6 ? QHostAddress(QHostAddress::LocalHostIPv6)
                           : QHostAddress(QHostAddress::LocalHost);
        if (!MakeSockAddr(localAddr, udp->localPort(), local, localLen)) {
            error_ = QStringLiteral("bad local address");
            return false;
        }
    }
    ngtcp2_path_storage_init(&ps, reinterpret_cast<ngtcp2_sockaddr *>(&local), localLen,
                             reinterpret_cast<ngtcp2_sockaddr *>(&remote), remoteLen,
                             nullptr);
    QObject::connect(udp, &QUdpSocket::readyRead, this, [this] { OnReadable(); });
    udpMs_ = clock_.elapsed() - resolveMs_;

    // ---- TLS ----
    ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) {
        error_ = QStringLiteral("SSL_CTX_new failed");
        return false;
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);   // QUIC 只允许 TLS1.3
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    const QString caSource = LoadCaBundle(ctx);
    caFp_ = CaFingerprint(caSource);
    // T2 核查点 3：信任链来源必须可判定
    Log(QStringLiteral("h3 ca-bundle %1").arg(caSource));

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_CLIENT);
    SSL_CTX_sess_set_new_cb(ctx, OnNewSession);

    ssl = SSL_new(ctx);
    if (!ssl) {
        error_ = QStringLiteral("SSL_new failed");
        return false;
    }
    // 必须先挂 conn_ref：ossl 后端的 CRYPTO_SEND/RECV 回调都用 SSL_get_app_data
    // 取 ngtcp2_crypto_conn_ref，再反查 ngtcp2_conn；缺了它握手直接 internal error。
    connRef.get_conn = [](ngtcp2_crypto_conn_ref *ref) -> ngtcp2_conn * {
        return static_cast<Conn *>(ref->user_data)->conn;
    };
    connRef.user_data = this;
    SSL_set_app_data(ssl, &connRef);
    if (ngtcp2_crypto_ossl_ctx_new(&ossl, ssl) != 0) {
        error_ = QStringLiteral("ossl ctx new failed");
        return false;
    }
    if (ngtcp2_crypto_ossl_configure_client_session(ssl) != 0) {
        error_ = QStringLiteral("ossl configure failed");
        return false;
    }
    SSL_set_connect_state(ssl);

    // 顺序按 RFC 9849 的推荐：先注入 ECHConfigList 与内/外层名字，再设 ALPN，
    // 最后设外层 ALPN。与 openssl_tls.cpp（TCP 版，实测 SUCCESS）保持一致。
    const QByteArray hostUtf8 = key_.host.toUtf8();
    SSL_set1_host(ssl, hostUtf8.constData());   // 证书名校验（T2：用域名，不用 IP）

    const bool useEch = !key_.echConfigList.isEmpty();
    const QByteArray echList = key_.echConfigList;
    const QString outer = echList.isEmpty() ? QString() : ParseEchPublicName(echList);
    const bool echNames = !echList.isEmpty() && !outer.isEmpty();

    if (useEch) {
        SSL_set1_ech_config_list(ssl, reinterpret_cast<const uint8_t *>(echList.constData()),
                                 size_t(echList.size()));
    }
    if (echNames) {
        const QByteArray outerUtf8 = outer.toUtf8();
        SSL_ech_set1_server_names(ssl, hostUtf8.constData(), outerUtf8.constData(), 0);
        Log(QStringLiteral("h3 ech inner=%1 outer=%2 configs=%3B")
                .arg(key_.host, outer)
                .arg(echList.size()));
    } else {
        SSL_set_tlsext_host_name(ssl, hostUtf8.constData());
    }
    Q_UNUSED(useEch);

    // ALPN：HTTP/3 只认 h3（内层）
    static const unsigned char kAlpn[] = {2, 'h', '3'};
    SSL_set_alpn_protos(ssl, kAlpn, sizeof(kAlpn));
    if (echNames) {
        // 外层 ALPN 用浏览器常见的 h2/http1.1：ECH 前端先解密再转真实 ALPN，
        // 外层只是伪装；只给 h3 时 Cloudflare 前端会拒绝 ECH（实测 RTT 也更长）。
        static const unsigned char kOuterAlpn[] = {2, 'h', '2', 8, 'h', 't', 't', 'p',
                                                   '/', '1', '.', '1'};
        SSL_ech_set1_outer_alpn_protos(ssl, kOuterAlpn, sizeof(kOuterAlpn));
    }

    // ---- 0-RTT：装回上次的会话与传输参数 ----
    {
        // 票据 key 与 ECH 指纹绑定：ECH 配置变了，LoadTicket 内部会丢弃
        const Ticket t = LoadTicket(key_.host, key_.echFp, caFp_);
        if (!t.pem.isEmpty()) {
            BIO *bio = BIO_new_mem_buf(t.pem.constData(), int(t.pem.size()));
            SSL_SESSION *sess = bio ? PEM_read_bio_SSL_SESSION(bio, nullptr, nullptr, nullptr)
                                    : nullptr;
            if (bio)
                BIO_free(bio);
            if (sess) {
                if (SSL_SESSION_is_resumable(sess)) {
                    SSL_set_session(ssl, sess);
                    reusedSession_ = true;
                    earlyEligible_ = SSL_SESSION_get_max_early_data(sess) > 0;
                }
                SSL_SESSION_free(sess);
            }
            ticketTp_ = t.tp;
            ticketCertFp_ = t.certFp;
        }
    }
    if (earlyEligible_) {
        SSL_set_quic_tls_early_data_enabled(ssl, 1);
        Log(QStringLiteral("h3 session resumed (early data allowed)"));
    } else if (reusedSession_) {
        Log(QStringLiteral("h3 session resumed (1-RTT only)"));
    }

    // ---- QUIC ----
    ngtcp2_callbacks cb{};
    cb.client_initial = ngtcp2_crypto_client_initial_cb;
    cb.recv_crypto_data = ngtcp2_crypto_recv_crypto_data_cb;
    cb.encrypt = ngtcp2_crypto_encrypt_cb;
    cb.decrypt = ngtcp2_crypto_decrypt_cb;
    cb.hp_mask = ngtcp2_crypto_hp_mask_cb;
    cb.recv_retry = ngtcp2_crypto_recv_retry_cb;
    cb.update_key = ngtcp2_crypto_update_key_cb;
    cb.delete_crypto_aead_ctx = ngtcp2_crypto_delete_crypto_aead_ctx_cb;
    cb.delete_crypto_cipher_ctx = ngtcp2_crypto_delete_crypto_cipher_ctx_cb;
    cb.version_negotiation = ngtcp2_crypto_version_negotiation_cb;
    cb.get_path_challenge_data2 = ngtcp2_crypto_get_path_challenge_data2_cb;
    cb.rand = OnRand;
    cb.get_new_connection_id2 = OnGetNewCid;
    cb.handshake_completed = OnHandshakeCompleted;
    cb.recv_stream_data = OnRecvStreamData;
    cb.acked_stream_data_offset = OnAckedStreamDataOffset;
    cb.stream_close = OnStreamClose;
    cb.extend_max_local_streams_bidi = OnExtendMaxStreamsBidi;
    cb.extend_max_local_streams_uni = OnExtendMaxStreamsUni;
    cb.tls_early_data_rejected = OnTlsEarlyDataRejected;

    ngtcp2_cid dcid{}, scid{};
    dcid.datalen = NGTCP2_MIN_INITIAL_DCIDLEN;
    RAND_bytes(dcid.data, int(dcid.datalen));
    scid.datalen = 8;
    RAND_bytes(scid.data, int(scid.datalen));

    ngtcp2_settings settings;
    ngtcp2_settings_default(&settings);
    settings.initial_ts = Now();
    settings.log_printf = OnLog;
    settings.max_tx_udp_payload_size = kMaxUdpPayload;
    settings.handshake_timeout = 10 * NGTCP2_SECONDS;
    if (QlogEnabled()) {
        settings.qlog_write = OnQlog;
        const QString path = StateDir() + QStringLiteral("/qlog");
        QDir().mkpath(path);
        qlogFile = new QFile(path + QStringLiteral("/h3-%1-%2.sqlog")
                                       .arg(HostFileBase(key_.host).section('/', -1))
                                       .arg(quintptr(this)));
        if (qlogFile->open(QIODevice::WriteOnly | QIODevice::Truncate))
            Log(QStringLiteral("h3 qlog %1").arg(qlogFile->fileName()));
    }

    ngtcp2_transport_params params;
    ngtcp2_transport_params_default(&params);
    params.max_idle_timeout = kIdleTimeout;
    params.initial_max_stream_data_bidi_local = 1 * 1024 * 1024;
    params.initial_max_stream_data_uni = 256 * 1024;
    params.initial_max_data = 4 * 1024 * 1024;
    params.initial_max_streams_bidi = kMaxStreamsBidi;
    params.initial_max_streams_uni = 3;   // 服务端的 control + 2 条 QPACK
    params.active_connection_id_limit = 8;

    const int rv = ngtcp2_conn_client_new(&conn, &dcid, &scid, &ps.path,
                                          NGTCP2_PROTO_VER_V1, &cb, &settings, &params,
                                          nullptr, this);
    if (rv != 0) {
        error_ = QStringLiteral("ngtcp2_conn_client_new: %1")
                     .arg(QString::fromLatin1(ngtcp2_strerror(rv)));
        return false;
    }

    // 0-RTT 必须在 open_bidi_stream 之前装入对端传输参数，否则开流直接失败
    if (reusedSession_ && !ticketTp_.isEmpty()) {
        const int rv0 = ngtcp2_conn_decode_and_set_0rtt_transport_params(
            conn, reinterpret_cast<const uint8_t *>(ticketTp_.constData()),
            size_t(ticketTp_.size()));
        if (rv0 == 0) {
            zeroRttArmed_ = true;
            // 只让首个请求走 0-RTT：多条流同时提前发送时，若服务端拒绝，
            // nghttp3 的控制流（也在 0-RTT 里开出）会一起被丢弃，握手期后续
            // 写控制流会一直阻塞到 handshake_timeout（2026-11-07 实测 burst=10 卡 10s）。
            earlyBudget_ = 1;
        } else {
            Log(QStringLiteral("h3 0rtt tp ignored: %1")
                    .arg(QString::fromLatin1(ngtcp2_strerror(rv0))));
        }
    }
    ngtcp2_conn_set_tls_native_handle(conn, ossl);

    timer = new QTimer(this);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, this, [this] { OnTimer(); });

    idle = new QTimer(this);
    idle->setSingleShot(true);
    QObject::connect(idle, &QTimer::timeout, this, [this] {
        if (jobs.isEmpty() && pending.isEmpty())
            CloseNow(QStringLiteral("idle"));
    });

    started_ = true;
    WriteStreams();   // 触发 ClientHello
    return true;
}

void Connection::EnsureH3()
{
    if (h3)
        return;
    if (ngtcp2_conn_get_streams_uni_left2(conn) < 3) {
        Log(QStringLiteral("h3 wait uni streams (%1 left)")
                .arg(ngtcp2_conn_get_streams_uni_left2(conn)));
        return;
    }
    static const nghttp3_callbacks h3cb = [] {
        nghttp3_callbacks c{};
        c.recv_data = OnH3RecvData;
        c.deferred_consume = OnH3DeferredConsume;
        c.recv_header = OnH3RecvHeader;
        c.end_stream = OnH3EndStream;
        c.stop_sending = OnH3StopSending;
        c.reset_stream = OnH3ResetStream;
        c.rand = OnH3Rand;
        return c;
    }();

    nghttp3_settings settings;
    nghttp3_settings_default(&settings);
    settings.qpack_max_dtable_capacity = 4096;
    settings.qpack_blocked_streams = 100;

    const int rv = nghttp3_conn_client_new(&h3, &h3cb, &settings,
                                           nghttp3_mem_default(), this);
    if (rv != 0) {
        Log(QStringLiteral("nghttp3_conn_client_new: %1")
                .arg(QString::fromLatin1(nghttp3_strerror(rv))));
        return;
    }

    int64_t ctrl = -1;
    if (ngtcp2_conn_open_uni_stream(conn, &ctrl, nullptr) != 0
        || nghttp3_conn_bind_control_stream(h3, ctrl) != 0) {
        Log(QStringLiteral("h3 control stream setup failed"));
        return;
    }
    int64_t enc = -1;
    int64_t dec = -1;
    if (ngtcp2_conn_open_uni_stream(conn, &enc, nullptr) != 0
        || ngtcp2_conn_open_uni_stream(conn, &dec, nullptr) != 0
        || nghttp3_conn_bind_qpack_streams(h3, enc, dec) != 0) {
        Log(QStringLiteral("h3 qpack stream setup failed"));
        return;
    }
    h3Ready_ = true;
    Log(QStringLiteral("h3 streams ctrl=%1 enc=%2 dec=%3").arg(ctrl).arg(enc).arg(dec));
}

bool Connection::Submit(Job *job)
{
    if (!job || job->finished)
        return false;
    if (!usable())
        return false;
    if (load() >= kMaxJobsPerConn)
        return false;

    idle->stop();
    job->conn = this;
    job->reusedConn = handshakeDone_ || reusedSession_;

    job->deadline = new QTimer(this);
    job->deadline->setSingleShot(true);
    QObject::connect(job->deadline, &QTimer::timeout, this, [this, job] {
        Abort(job, QStringLiteral("timeout %1ms").arg(job->req.timeoutMs));
    });
    job->deadline->start(qMax(1000, job->req.timeoutMs));

    // 0-RTT：只有幂等 GET（无 body）才允许提前发
    const bool canEarly = zeroRttArmed_ && !handshakeDone_ && earlyBudget_ > 0
                          && job->req.method == QByteArrayLiteral("GET")
                          && job->req.body.isEmpty();
    if (h3Ready_) {
        if (SubmitJob(job))
            return true;
        pending.append(job);
        return true;
    }
    if (canEarly) {
        EnsureH3();
        if (h3Ready_) {
            // 先置标记再提交：SubmitJob 里的日志要如实反映"这条流走的是 0-RTT"
            job->earlySent = true;
            if (SubmitJob(job)) {
                earlyUsed_ = true;
                --earlyBudget_;
                return true;
            }
            job->earlySent = false;
        }
    }
    pending.append(job);
    return true;
}

bool Connection::SubmitJob(Job *job)
{
    if (!h3 || !job || job->finished || job->streamId >= 0)
        return false;

    int64_t sid = -1;
    if (ngtcp2_conn_open_bidi_stream(conn, &sid, job) != 0) {
        // 对端还没放开 bidi 限额：等 extend_max_local_streams_bidi 再开
        return false;
    }

    QString authority = job->req.host;
    if (job->req.port != 443)
        authority += QStringLiteral(":%1").arg(job->req.port);

    job->nvStore.clear();
    auto push = [job](const QByteArray &n, const QByteArray &v) {
        job->nvStore.push_back(std::string(n.constData(), size_t(n.size())));
        job->nvStore.push_back(std::string(v.constData(), size_t(v.size())));
    };
    push(":method", job->req.method);
    push(":scheme", "https");
    push(":authority", authority.toUtf8());
    push(":path", job->path);
    for (const auto &h : job->req.headers) {
        if (h.first.startsWith(':'))
            continue;   // 伪首部由上面固定给出
        push(h.first, h.second);
    }

    std::vector<nghttp3_nv> nva;
    nva.reserve(job->nvStore.size() / 2);
    for (size_t i = 0; i + 1 < job->nvStore.size(); i += 2) {
        nghttp3_nv nv{};
        nv.name = reinterpret_cast<uint8_t *>(job->nvStore[i].data());
        nv.namelen = job->nvStore[i].size();
        nv.value = reinterpret_cast<uint8_t *>(job->nvStore[i + 1].data());
        nv.valuelen = job->nvStore[i + 1].size();
        nv.flags = NGHTTP3_NV_FLAG_NONE;
        nva.push_back(nv);
    }

    nghttp3_data_reader dr{};
    dr.read_data = OnH3ReadData;
    const int rv = nghttp3_conn_submit_request(
        h3, sid, nva.data(), nva.size(), job->bodyOut.isEmpty() ? nullptr : &dr, job);
    if (rv != 0) {
        Log(QStringLiteral("h3 submit_request: %1")
                .arg(QString::fromLatin1(nghttp3_strerror(rv))));
        ngtcp2_conn_shutdown_stream_write(conn, 0, sid, NGHTTP3_H3_REQUEST_CANCELLED);
        return false;
    }

    job->streamId = sid;
    job->submitAtMs = clock_.elapsed();
    jobs.insert(sid, job);
    if (job->reusedConn)
        g_reused.fetch_add(1);
    Log(QStringLiteral("h3 request %1 %2%3 stream=%4 early=%5")
            .arg(QString::fromLatin1(job->req.method), job->req.host,
                 QString::fromUtf8(job->path))
            .arg(sid)
            .arg(job->earlySent ? 1 : 0));
    WriteStreams();
    return true;
}

void Connection::WriteStreams()
{
    if (finished_ || !conn || !udp)
        return;

    std::array<uint8_t, kMaxUdpPayload> buf{};
    for (;;) {
        std::array<nghttp3_vec, 8> vec{};
        int64_t sid = -1;
        int fin = 0;
        nghttp3_ssize sveccnt = 0;
        if (h3 && ngtcp2_conn_get_max_data_left2(conn)) {
            sveccnt = nghttp3_conn_writev_stream(h3, &sid, &fin, vec.data(), vec.size());
            if (sveccnt < 0) {
                FailAll(QStringLiteral("nghttp3 writev: %1")
                            .arg(QString::fromLatin1(nghttp3_strerror(int(sveccnt)))));
                return;
            }
        }

        ngtcp2_pkt_info pi{};
        ngtcp2_ssize ndatalen = 0;
        uint32_t flags = NGTCP2_WRITE_STREAM_FLAG_MORE;
        if (fin)
            flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;

        const ngtcp2_ssize nwrite = ngtcp2_conn_writev_stream(
            conn, &ps.path, &pi, buf.data(), buf.size(), &ndatalen, flags, sid,
            reinterpret_cast<const ngtcp2_vec *>(vec.data()), size_t(sveccnt), Now());
        if (nwrite < 0) {
            switch (nwrite) {
            case NGTCP2_ERR_STREAM_DATA_BLOCKED:
                if (h3 && sid >= 0)
                    nghttp3_conn_block_stream(h3, sid);
                continue;
            case NGTCP2_ERR_STREAM_SHUT_WR:
                if (h3 && sid >= 0)
                    nghttp3_conn_shutdown_stream_write(h3, sid);
                continue;
            case NGTCP2_ERR_WRITE_MORE:
                if (ndatalen > 0 && h3)
                    nghttp3_conn_add_write_offset(h3, sid, uint64_t(ndatalen));
                continue;
            default: {
                const unsigned long e = ERR_get_error();
                char ebuf[256] = {0};
                if (e)
                    ERR_error_string_n(e, ebuf, sizeof(ebuf));
                FailAll(QStringLiteral("writev_stream: %1 ssl=%2")
                            .arg(QString::fromLatin1(ngtcp2_strerror(int(nwrite))),
                                 QString::fromLatin1(ebuf)));
                return;
            }
            }
        }
        if (ndatalen > 0 && h3)
            nghttp3_conn_add_write_offset(h3, sid, uint64_t(ndatalen));
        if (nwrite == 0)
            break;

        const qint64 sent = udp->write(reinterpret_cast<const char *>(buf.data()), nwrite);
        if (sent != nwrite) {
            Log(QStringLiteral("h3 udp write %1/%2 err=%3")
                    .arg(sent).arg(nwrite).arg(udp->errorString()));
        } else if (Verbose() && txLog_ < 6) {
            ++txLog_;
            Log(QStringLiteral("h3 tx %1B sid=%2").arg(nwrite).arg(sid));
        }
    }
    ScheduleTimer();
}

void Connection::ScheduleTimer()
{
    if (!timer || !conn || finished_)
        return;
    const ngtcp2_tstamp expiry = ngtcp2_conn_get_expiry2(conn);
    if (expiry == UINT64_MAX) {
        timer->stop();
        return;
    }
    const ngtcp2_tstamp now = Now();
    const qint64 ms = expiry <= now ? 1 : qint64((expiry - now) / 1000000);
    timer->start(int(qBound<qint64>(qint64(1), ms, qint64(60000))));
}

void Connection::OnTimer()
{
    if (finished_ || !conn)
        return;
    const int rv = ngtcp2_conn_handle_expiry(conn, Now());
    if (rv != 0) {
        CloseNow(QStringLiteral("handle_expiry: %1")
                     .arg(QString::fromLatin1(ngtcp2_strerror(rv))));
        return;
    }
    WriteStreams();
}

void Connection::OnReadable()
{
    if (finished_ || !udp || !conn)
        return;
    QByteArray dg;
    while (udp->hasPendingDatagrams()) {
        const qint64 n = udp->pendingDatagramSize();
        if (n <= 0)
            break;
        dg.resize(int(n));
        const qint64 got = udp->readDatagram(dg.data(), dg.size());
        if (got <= 0)
            break;
        if (Verbose() && rxLog_ < 4) {
            ++rxLog_;
            Log(QStringLiteral("h3 rx %1B head=%2")
                    .arg(got)
                    .arg(QString::fromLatin1(dg.left(16).toHex(' '))));
        }
        ngtcp2_pkt_info pi{};
        const int rv = ngtcp2_conn_read_pkt(
            conn, &ps.path, &pi, reinterpret_cast<const uint8_t *>(dg.constData()),
            size_t(got), Now());
        if (rv != 0) {
            // 证书校验失败时 ngtcp2 只给 ERR_CRYPTO，真正原因在 OpenSSL 错误队列里。
            // T2 要求"证书错误必须可见"，这里把 SSL 的原因附上，避免只剩一句 CRYPTO。
            QString why = QStringLiteral("read_pkt: %1")
                              .arg(QString::fromLatin1(ngtcp2_strerror(rv)));
            if (rv == NGTCP2_ERR_CRYPTO || rv == NGTCP2_ERR_CALLBACK_FAILURE) {
                const unsigned long e = ERR_peek_error();
                if (e) {
                    char ebuf[256] = {0};
                    ERR_error_string_n(e, ebuf, sizeof(ebuf));
                    long vr = SSL_get_verify_result(ssl);
                    why += QStringLiteral(" ssl=%1 verify=%2")
                               .arg(QString::fromLatin1(ebuf))
                               .arg(vr);
                }
            }
            CloseNow(why);
            return;
        }
        if (finished_)
            return;
    }
    WriteStreams();
}

void Connection::OnHandshake()
{
    handshakeDone_ = true;
    hsMs_ = clock_.elapsed();

    const unsigned char *alpn = nullptr;
    unsigned int alpnlen = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpnlen);
    alpn_ = QString::fromLatin1(reinterpret_cast<const char *>(alpn), int(alpnlen));
    tlsVersion_ = QString::fromLatin1(SSL_get_version(ssl));
    // OpenSSL 4.x 的 ECH 状态语义：只有 1=SUCCESS 说明服务端接受了内层 SNI；
    // 0=FAILED / 4=BACKEND / -102=BAD_NAME 等都意味着真实域名仍暴露在外层。
    // 没注入 ECHConfigList 时 OpenSSL 也返回 0，不能报成 FAILED。
    echStatus_ = key_.echConfigList.isEmpty()
                     ? -103   // NOT_CONFIGURED
                     : SSL_ech_get1_status(ssl, nullptr, nullptr);
    echText_ = key_.echConfigList.isEmpty() ? QStringLiteral("未启用") : EchText(echStatus_);

    certFp_ = PeerCertFingerprint(ssl);
    if (reusedSession_ && !certFp_.isEmpty() && !ticketCertFp_.isEmpty()
        && certFp_ != ticketCertFp_) {
        // 证书轮换后旧票据不再可信（T2：不能用旧会话掩盖证书变化）
        Log(QStringLiteral("h3 ticket dropped (peer cert changed)"));
        DropTicket(key_.host);
        reusedSession_ = false;
        earlyEligible_ = false;
    }

    // 本次记住的对端传输参数，下次 0-RTT 要用
    std::array<uint8_t, 512> tpbuf{};
    const ngtcp2_ssize tpn =
        ngtcp2_conn_encode_0rtt_transport_params2(conn, tpbuf.data(), tpbuf.size());
    if (tpn > 0) {
        tpEncoded_ = QByteArray(reinterpret_cast<const char *>(tpbuf.data()), int(tpn));
    } else {
        tpEncoded_.clear();
    }

    const int earlyStatus = SSL_get_early_data_status(ssl);
    earlyText_ = earlyUsed_ ? EarlyText(earlyStatus) : QStringLiteral("未启用");
    if (earlyUsed_) {
        g_zeroRttTried.fetch_add(1);
        if (earlyStatus == 1)
            g_zeroRttAccepted.fetch_add(1);
        else if (earlyStatus == 2)
            g_zeroRttRejected.fetch_add(1);
        Log(QStringLiteral("h3 0-rtt %1").arg(earlyText_));
    }

    Log(QStringLiteral("h3 handshake ok version=%1 alpn=%2 ech=%3 0rtt=%4 hs=%5ms")
            .arg(tlsVersion_, alpn_, echText_, earlyText_)
            .arg(hsMs_));

    EnsureH3();
    // 先摘出 pending 再提交：SubmitJob 内部会 Log，而 Log 会给 pending+jobs
    // 里的同一个 job 各发一次，导致同一行日志出现两遍。
    const QList<Job *> waiting = pending;
    pending.clear();
    for (Job *job : waiting) {
        if (!SubmitJob(job))
            pending.append(job);
    }
    WriteStreams();
}

void Connection::Abort(Job *job, const QString &why)
{
    if (!job || job->finished)
        return;
    if (job->streamId >= 0) {
        if (h3)
            nghttp3_conn_shutdown_stream_write(h3, job->streamId);
        if (conn) {
            ngtcp2_conn_shutdown_stream_write(conn, 0, job->streamId,
                                              NGHTTP3_H3_REQUEST_CANCELLED);
            ngtcp2_conn_shutdown_stream_read(conn, 0, job->streamId,
                                             NGHTTP3_H3_REQUEST_CANCELLED);
        }
    }
    Response r;
    r.error = why;
    FinishJob(job, r);
}

void Connection::FinishJob(Job *job, Response r)
{
    if (!job || job->finished)
        return;
    job->finished = true;
    if (job->deadline) {
        job->deadline->stop();
        job->deadline->deleteLater();
        job->deadline = nullptr;
    }
    const int64_t sid = job->streamId;
    if (sid >= 0)
        jobs.remove(sid);
    pending.removeAll(job);
    job->streamId = -1;

    if (r.status <= 0 && r.error.isEmpty())
        r.error = QStringLiteral("no response");
    if (r.elapsedMs <= 0)
        r.elapsedMs = job->clock.elapsed();
    // 失败时不要声称是 HTTP/3 响应：探针输出里 version 与 ok 必须自洽
    r.version = r.ok ? QStringLiteral("HTTP/3") : QString();
    r.alpn = alpn_;
    r.tlsVersion = tlsVersion_;
    r.echText = echText_;
    r.earlyText = earlyText_;
    r.reused = job->reusedConn;
    r.ttfbMs = job->ttfbMs;

    // Q4：注入过 ECH 但服务端没接受 → 即使 HTTP/3 层返回 200，真实 SNI 也是
    // 暴露的，必须让调用方按「隐藏 SNI 优先」丢弃这个结果并回退 TCP+ECH。
    if (!key_.echConfigList.isEmpty()) {
        if (!handshakeDone_ && ssl)
            echStatus_ = SSL_ech_get1_status(ssl, nullptr, nullptr);
        r.echRejected = echStatus_ != 1;
        if (r.echText.isEmpty())
            r.echText = EchText(echStatus_);
        if (r.echRejected)
            Log(QStringLiteral("h3 ech rejected status=%1 text=%2 (will prefer tcp+ech)")
                    .arg(echStatus_)
                    .arg(r.echText));
    }

    // Q3：一行 h3-timing 把首包成本拆开，便于判断"慢在解析/UDP/握手/服务端"
    // 注意：不要用 %10 及以上的占位符 —— QString::arg 的两位占位符与 %1 混用会错位。
    QString timing = QStringLiteral(
                         "h3-timing host=%1 %2 -> %3 %4B queue=%5ms ttfb=%6ms total=%7ms "
                         "resolve=%8ms udp=%9ms")
                         .arg(key_.host, QString::fromUtf8(job->path))
                         .arg(r.ok ? r.status : 0)
                         .arg(r.body.size())
                         .arg(job->queueMs)
                         .arg(job->ttfbMs)
                         .arg(r.elapsedMs)
                         .arg(resolveMs_)
                         .arg(udpMs_);
    timing += QStringLiteral(" hs=%1ms 0rtt=%2 reused=%3")
                  .arg(hsMs_)
                  .arg(earlyText_, job->reusedConn ? QStringLiteral("1")
                                                   : QStringLiteral("0"));
    Log(timing);

    auto cb = job->cb;
    job->cb = nullptr;
    if (cb)
        cb(r);
    if (onJobDone)
        onJobDone(this, job);
    delete job;
    if (!finished_ && jobs.isEmpty() && pending.isEmpty() && idle)
        idle->start(kIdleCloseMs);
}

void Connection::FailAll(const QString &why)
{
    if (error_.isEmpty())
        error_ = why;
    QList<Job *> stranded = jobs.values();
    stranded += pending;
    for (Job *job : stranded) {
        if (!job || job->finished)
            continue;
        Response r;
        r.error = why;
        FinishJob(job, r);
    }
}

void Connection::CloseNow(const QString &why)
{
    if (finished_)
        return;
    closing_ = true;
    // FailAll 可以在当前栈上跑：Job 只是拿到错误回调，不碰 ngtcp2/nghttp3 对象。
    FailAll(why);
    finished_ = true;
    if (timer)
        timer->stop();
    if (idle)
        idle->stop();
    // 拆除绝不能留在回调栈上：read_pkt / handle_expiry / shutdown 都可能走到这里，
    // 而此时代码还在 ngtcp2/nghttp3 的调用帧里。同步 Cleanup() 释放 conn/h3/ssl
    // 是 use-after-free（实测证书失败路径直接崩在 Cleanup，见 crash backtrace
    // OnReadable -> CloseNow -> Cleanup）。丢到事件循环里做，回调帧先退干净。
    if (error_.isEmpty())
        error_ = why;
    QTimer::singleShot(0, this, [this] { Teardown(); });
}

void Connection::Teardown()
{
    if (conn && udp) {
        ngtcp2_ccerr cc;
        ngtcp2_ccerr_default(&cc);
        std::array<uint8_t, 128> buf{};
        ngtcp2_pkt_info pi{};
        const ngtcp2_ssize n = ngtcp2_conn_write_connection_close(
            conn, &ps.path, &pi, buf.data(), buf.size(), &cc, Now());
        if (n > 0)
            udp->write(reinterpret_cast<const char *>(buf.data()), n);
    }
    Cleanup();
    if (onClosed) {
        auto f = onClosed;
        onClosed = nullptr;
        f(this);
    }
}

void Connection::Cleanup()
{
    if (h3) {
        nghttp3_conn_del(h3);
        h3 = nullptr;
    }
    if (conn) {
        ngtcp2_conn_del(conn);
        conn = nullptr;
    }
    if (udp) {
        udp->disconnect(this);
        udp->close();
        udp->deleteLater();
        udp = nullptr;
    }
    if (ssl) {
        SSL_free(ssl);          // ossl_ctx 持有同一 SSL 指针，先解绑
        ssl = nullptr;
    }
    if (ossl) {
        ngtcp2_crypto_ossl_ctx_del(ossl);
        ossl = nullptr;
    }
    if (ctx) {
        SSL_CTX_free(ctx);
        ctx = nullptr;
    }
}

void Connection::SaveSession(SSL_SESSION *sess)
{
    if (finished_ || !sess)
        return;
    // 票据保存失败不影响请求；下次只是退化成完整握手
    SaveTicket(key_.host, key_.echFp, certFp_, caFp_, sess, tpEncoded_);
    Log(QStringLiteral("h3 ticket saved list=%1")
            .arg(QDir(StateDir()).entryList({QStringLiteral("*.pem")}, QDir::Files).size()));
}

void Connection::WriteQlog(const void *data, size_t datalen)
{
    if (!qlogFile || !qlogFile->isOpen() || !data || datalen == 0)
        return;
    qlogFile->write(reinterpret_cast<const char *>(data), qint64(datalen));
    qlogFile->flush();
}

// ---------------------------------------------------------------------------
// 连接池
// ---------------------------------------------------------------------------

class Pool
{
public:
    static Pool &I()
    {
        static Pool pool;
        return pool;
    }

    void Submit(const Request &req, Client::Callback cb,
                std::function<void(const QString &)> sink)
    {
        auto *job = new Job;
        job->req = req;
        job->cb = std::move(cb);
        job->logSink = std::move(sink);
        job->clock.start();

        QByteArray path = req.path.toUtf8();
        if (path.isEmpty())
            path = "/";
        job->path = path;
        job->bodyOut = req.body;

        const QString key = KeyOf(req);
        job->req.path = QString::fromUtf8(path);

        if (Connection *c = Pick(key)) {
            if (c->Submit(job)) {
                ++jobs_;
                return;
            }
        }
        if (CanOpen(key)) {
            OpenConnection(key, req, job);
            return;
        }
        queue_[key].append(job);
    }

    void OnJobDone(Connection *c, Job *)
    {
        if (jobs_ > 0)
            --jobs_;
        if (!c)
            return;
        const QString key = c->poolKey;
        Pump(key);
    }

    void OnConnClosed(Connection *c)
    {
        if (!c)
            return;
        const QString key = c->poolKey;
        if (conns_.contains(key))
            conns_[key].removeAll(c);
        if (total_ > 0)
            --total_;
        c->deleteLater();
        Pump(key);
    }

    PoolStats Snapshot() const
    {
        PoolStats s;
        s.connections = total_;
        s.jobs = jobs_;
        int queued = 0;
        for (auto it = queue_.constBegin(); it != queue_.constEnd(); ++it)
            queued += it.value().size();
        s.queued = queued;
        s.handshakes = g_handshakes.load();
        s.reused = g_reused.load();
        s.zeroRttTried = g_zeroRttTried.load();
        s.zeroRttAccepted = g_zeroRttAccepted.load();
        s.zeroRttRejected = g_zeroRttRejected.load();
        return s;
    }

    void Shutdown()
    {
        for (auto it = conns_.begin(); it != conns_.end(); ++it) {
            for (Connection *c : it.value()) {
                if (c)
                    c->CloseNow(QStringLiteral("shutdown"));
            }
        }
        conns_.clear();
        queue_.clear();
        total_ = 0;
        jobs_ = 0;
    }

private:
    static QString KeyOf(const Request &req)
    {
        return QStringLiteral("%1|%2|%3")
            .arg(req.host)
            .arg(req.port)
            .arg(QString::fromLatin1(Fingerprint(req.echConfigList)));
    }

    Connection *Pick(const QString &key)
    {
        const QList<Connection *> list = conns_.value(key);
        for (Connection *c : list) {
            if (c && c->usable() && c->load() < kMaxJobsPerConn)
                return c;
        }
        return nullptr;
    }

    bool CanOpen(const QString &key) const
    {
        return conns_.value(key).size() < kMaxConnsPerKey && total_ < kMaxConnsTotal;
    }

    void OpenConnection(const QString &key, const Request &req, Job *job)
    {
        Connection::Key ck;
        ck.host = req.host;
        ck.port = req.port;
        ck.echFp = Fingerprint(req.echConfigList);
        ck.echConfigList = req.echConfigList;
        ck.connectIp = req.connectIp;

        auto *c = new Connection(ck);
        c->poolKey = key;
        c->onJobDone = [this](Connection *cc, Job *j) { OnJobDone(cc, j); };
        c->onClosed = [this](Connection *cc) { OnConnClosed(cc); };
        conns_[key].append(c);
        ++total_;
        g_handshakes.fetch_add(1);

        if (!c->Start()) {
            const QString err = c->lastError();
            conns_[key].removeAll(c);
            --total_;
            c->deleteLater();
            FailJob(job, err.isEmpty() ? QStringLiteral("connect failed") : err);
            return;
        }
        if (c->Submit(job)) {
            ++jobs_;
            return;
        }
        queue_[key].append(job);
    }

    void Pump(const QString &key)
    {
        if (key.isEmpty())
            return;
        QList<Job *> &q = queue_[key];
        while (!q.isEmpty()) {
            Job *job = q.first();
            if (!job || job->finished) {
                q.removeFirst();
                continue;
            }
            Connection *c = Pick(key);
            if (!c) {
                if (CanOpen(key)) {
                    q.removeFirst();
                    OpenConnection(key, job->req, job);
                    continue;
                }
                break;   // 仍有上限压力，等下一次回收
            }
            q.removeFirst();
            job->queueMs = job->clock.elapsed();
            if (!c->Submit(job)) {
                // 连接刚好满了/关了：放回队首，下一轮再试
                q.prepend(job);
                break;
            }
            ++jobs_;
        }
        if (q.isEmpty())
            queue_.remove(key);
    }

    void FailJob(Job *job, const QString &why)
    {
        if (!job)
            return;
        Response r;
        r.error = why;
        r.elapsedMs = job->clock.elapsed();
        auto cb = job->cb;
        job->cb = nullptr;
        job->finished = true;
        if (cb)
            cb(r);
        delete job;
    }

    QHash<QString, QList<Connection *>> conns_;
    QHash<QString, QList<Job *>> queue_;
    int total_ = 0;
    int jobs_ = 0;
};

// ---------------------------------------------------------------------------

Client::Client(QObject *parent) : QObject(parent) {}

Client::~Client() = default;

void Client::Start(const Request &req, Callback cb)
{
    QPointer<Client> guard(this);
    Pool::I().Submit(req, std::move(cb), [guard](const QString &line) {
        if (guard)
            emit guard->Log(line);
    });
}

bool Available()
{
    return true;
}

QString BackendSummary()
{
    const ngtcp2_info *t = ngtcp2_version(0);
    const nghttp3_info *h = nghttp3_version(0);
    return QStringLiteral("ngtcp2 %1 / nghttp3 %2 / %3")
        .arg(QString::fromLatin1(t ? t->version_str : "?"),
             QString::fromLatin1(h ? h->version_str : "?"),
             QString::fromLatin1(OpenSSL_version(OPENSSL_VERSION_STRING)));
}

PoolStats Stats()
{
    return Pool::I().Snapshot();
}

void ShutdownPool()
{
    Pool::I().Shutdown();
}

}   // namespace quic

#else   // !JM_WITH_QUIC —— Android 等没有 QUIC 栈的平台

namespace quic {

Client::Client(QObject *parent) : QObject(parent) {}
Client::~Client() = default;

void Client::Start(const Request &req, Callback cb)
{
    Response r;
    r.error = QStringLiteral("QUIC 栈未编译进本平台");
    if (cb)
        cb(r);
    Q_UNUSED(req);
}

bool Available()
{
    return false;
}

QString BackendSummary()
{
    return QStringLiteral("未编译 QUIC 栈（需 OpenSSL 4.x + ngtcp2 + nghttp3）");
}

PoolStats Stats()
{
    return PoolStats();
}

void ShutdownPool() {}

}   // namespace quic

#endif   // JM_WITH_QUIC

