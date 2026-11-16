// 可指定 SNI 的 HTTPS 客户端：让"分流节点"真正作用到流量上。
//
// 背景（为什么不能继续用 QNetworkAccessManager）：
//   Python 版用 curl 的 `--resolve host:443:ip` 把连接落到节点 IP、TLS 仍用域名做
//   SNI 与证书校验。Qt 的 QNetworkAccessManager 没有对应能力：
//     * QSslConfiguration 在 6.12 里没有 setPeerVerifyName；
//     * QNAM 的 SNI 固定取 URL 的 host，若把 host 改成 IP，Cloudflare 会直接断连
//       （实测 curl -k https://<ip> 同样 000）。
//   唯一可行的入口是 QSslSocket 的三参 connectToHostEncrypted(hostName, port, sslPeerName)：
//   TCP 连 hostName（这里传 IP），TLS 的 SNI 与证书校验名用 sslPeerName（传域名）。
//   实测 TLS1.3 握手成功并拿到 HTTP/1.1 200，与 curl --resolve 等价。
//
// 因此本文件实现一个"够用的 HTTP/1.1 客户端"：
//   * 只支持 https/http 的 GET/POST（项目里只有这两种）；
//   * 处理 Content-Length / chunked / Connection: close 三种定界；
//   * 最多跟随 3 次重定向；换 host 时自动放弃 IP 覆盖（重新走 DNS）；
//   * 单请求超时与 maxBytes 上限，语义与 NetClient 保持一致。
// 不做连接复用：分流节点场景下并发量由调用方限制（封面 3 / 页图 6 / API 单通道）。
#pragma once

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

struct NodeHttpJob;   // node_http.cpp 内部状态机

// quicOnly 模式下回给调用方的两个哨兵错误（见 net_client.cpp 的直连 H3 探测）：
// kH3Unavailable = 本次没有可用的 H3，调用方改用自身通道（QNAM/HTTP2）继续；
// kH3CertFailure = 证书校验失败，**禁止**换一套 CA 重试（T2 安全约束），
// 调用方必须直接判失败而不是回退。
constexpr const char *kH3Unavailable = "h3-unavailable";
constexpr const char *kH3CertFailure = "h3-cert-failure: ";

struct NodeHttpRequest
{
    QByteArray method = QByteArrayLiteral("GET");
    QUrl url;
    // 非空：TCP 直连该 IP（等价 curl --resolve 的地址部分）
    QString connectIp;
    // 非空：TLS SNI + 证书校验名（等价 curl --resolve 的域名部分）；默认取 url.host()
    QString sniHost;
    QHash<QString, QString> headers;
    QByteArray body;
    int timeoutMs = 12000;
    qint64 maxBytes = 512 * 1024;
    int maxRedirects = 3;
    // 允许先用 HTTP/3（QUIC）试一次；失败自动回退到 TLS/HTTP1.1。
    // 回退重试时内部会置 false，避免死循环。
    bool allowQuic = true;
    // 只做 H3 探测（NetClient 直连路径用）：不碰 TCP、不自建 HTTP/1.1 回退。
    // 失败时以 kH3Unavailable / kH3CertFailure 回调，由调用方决定回落到哪条通道。
    // 默认为 false，节点/DoH 路径的既有行为完全不变。
    bool quicOnly = false;
    // Q4：显式带上的 ECHConfigList。H3+ECH 组合被拒、回退 TCP+ECH 时把同一份
    // 配置带过去，避免依赖 DoH 缓存是否还在（缓存过期就会悄悄退化成明文 SNI）。
    // 为空时 TCP 分支沿用 DohClient::CachedEch() 的既有行为。
    QByteArray echConfigList;
};

struct NodeHttpResponse
{
    bool ok = false;          // 是否拿到 HTTP 响应（4xx/5xx 也算拿到）
    int status = 0;
    bool httpError = false;   // status >= 400
    QByteArray body;
    bool truncated = false;
    QString error;            // 传输层失败原因（ok=false 时有效）
    qint64 elapsedMs = 0;
    QString finalUrl;
    // 响应头（键统一小写）。多数调用方不需要；账号网页流程要靠 set-cookie 复用
    // 站点会话（先 GET 表单页拿 cookie，再 POST 提交），所以这里如实带出来。
    // 同名头出现多次时按换行拼接（set-cookie 就会这样），解析方自行 split。
    QHash<QString, QString> headers;
};

class NodeHttp : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const NodeHttpResponse &)>;

    explicit NodeHttp(QObject *parent = nullptr);

    // 发起一次请求；回调一定在主线程、且最多触发一次。
    void Send(const NodeHttpRequest &req, Callback cb);

signals:
    void Log(const QString &line);

private:
    // redirects 是已跟随次数（内部递归用）
    void Start(const NodeHttpRequest &req, int redirects, Callback cb);
    // 收口：需要时先跟随重定向，否则回调结果。ok/error 是"本次传输"的结论。
    void Settle(const std::shared_ptr<NodeHttpJob> &job, bool ok, const QString &error);
    // 收到明文（TLS 解密后 / 明文 socket）后的统一解析路径
    void Consume(const std::shared_ptr<NodeHttpJob> &job, const QByteArray &data);
    // 优先尝试 HTTP/3（QUIC）；失败回退到本文件的 TLS/HTTP1.1 路径
    void StartQuic(const std::shared_ptr<NodeHttpJob> &job);

    // Q4：换节点检测（Send 是顶层入口，内部递归走 Start 不重复检查）
    QString lastSelectedIp_;
};
