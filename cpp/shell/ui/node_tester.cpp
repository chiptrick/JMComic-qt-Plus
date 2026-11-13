#include "node_tester.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>

#include "node_http.h"

namespace {

// Python SpeedTestPingReq / SpeedTestPing2Req / SpeedTestReq 的固定样本
constexpr const char *kSpeedSample = "/media/photos/292840/00002.webp";
constexpr const char *kCoverSample = "/media/albums/1447694_3x4.jpg";

// 并发度：真机/模拟器对突发连接敏感；节点探测与线路 ping 都限 4 个在飞
constexpr int kNodeConcurrency = 8;
// ipinfo 只查地区：并发压低一点（Python 走自己的任务池），避免 50+ 节点同时打到
// parse.jpacg.cc；每条约 100~300ms，8 并发下 50 个节点约 2s 收尾。
constexpr int kInfoConcurrency = 4;
constexpr int kInfoTimeoutMs = 5000;   // Python GetIpInfoReq.timeout = 5
constexpr int kPingSamples = 2;    // 每条线路 ping 采样次数（取最小耗时）
constexpr int kSpeedSamples = 2;   // 每条线路下载采样次数（取最大吞吐）
constexpr int kProbeTimeoutMs = 3000;   // Python SpeedTestPingReq.timeout = 3
constexpr int kFetchTimeoutMs = 5000;

// Python natsorted：按数字段自然排序，保证 ProxyIP 列表顺序稳定
bool NaturalLess(const QString &a, const QString &b)
{
    int i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const QChar ca = a.at(i);
        const QChar cb = b.at(j);
        if (ca.isDigit() && cb.isDigit()) {
            int ia = i, ib = j;
            while (ia < a.size() && a.at(ia).isDigit())
                ++ia;
            while (ib < b.size() && b.at(ib).isDigit())
                ++ib;
            const qlonglong va = a.mid(i, ia - i).toLongLong();
            const qlonglong vb = b.mid(j, ib - j).toLongLong();
            if (va != vb)
                return va < vb;
            i = ia;
            j = ib;
            continue;
        }
        if (ca != cb)
            return ca < cb;
        ++i;
        ++j;
    }
    return a.size() < b.size();
}

}  // namespace

NodeTester::NodeTester(QObject *parent) : QObject(parent)
{
    nam_ = new QNetworkAccessManager(this);
}

QString NodeTester::FormatSpeed(double bytesPerSec)
{
    // Python ToolUtil.GetDownloadSize：<=0.1kb 用 bytes，<=0.1mb 用 kb，否则 mb
    if (bytesPerSec <= 0)
        return QString();
    const double kb = bytesPerSec / 1024.0;
    if (kb <= 0.1)
        return QStringLiteral("%1bytes").arg(bytesPerSec, 0, 'f', 0);
    const double mb = kb / 1024.0;
    if (mb <= 0.1)
        return QStringLiteral("%1kb").arg(kb, 0, 'f', 2);
    return QStringLiteral("%1mb").arg(mb, 0, 'f', 2);
}

QString NodeTester::FormatDelay(int ms)
{
    return QStringLiteral("%1ms").arg(ms);
}

QString NodeTester::FailText(const QString &error, bool forDownload)
{
    // Python SpeedTestPingBack/SpeedTestBack 失败时写的是 Str.GetStr(st) 这类短状态：
    // 延迟列常见「连接超时」，速度列常见「下载失败」；Qt 的 "Operation timed out"
    // 会把 70~100px 宽的单元格撑破，也没法横向比较。完整错误留给 tooltip。
    const QString e = error.toLower();
    if (e.contains(QLatin1String("未选节点")))
        return QStringLiteral("未选节点");
    if (e.startsWith(QLatin1String("http ")))
        return error;   // "http 403" 本身就很短
    if (e.contains(QLatin1String("timed out")) || e.contains(QLatin1String("timeout")))
        return forDownload ? QStringLiteral("下载失败") : QStringLiteral("连接超时");
    if (e.contains(QLatin1String("host")) && e.contains(QLatin1String("found")))
        return forDownload ? QStringLiteral("下载失败") : QStringLiteral("解析失败");
    if (e.contains(QLatin1String("ssl")) || e.contains(QLatin1String("tls")))
        return forDownload ? QStringLiteral("下载失败") : QStringLiteral("SSL错误");
    if (e.contains(QLatin1String("refused")) || e.contains(QLatin1String("closed"))
        || e.contains(QLatin1String("reset")))
        return forDownload ? QStringLiteral("下载失败") : QStringLiteral("连接失败");
    return forDownload ? QStringLiteral("下载失败") : QStringLiteral("失败");
}

void NodeTester::Cancel()
{
    ++generation_;
    busy_ = false;
    nodePingQueue_.clear();
    nodeSpeedQueue_.clear();
    nodeInfoQueue_.clear();
    nodePingInflight_.clear();
    nodeSpeedInflight_.clear();
    nodeInfoInflight_.clear();
    nodePhase_ = 0;
    linePingPending_ = 0;
}

void NodeTester::Get(const Probe &p, Done done)
{
    const int gen = generation_;

    // 节点 IP 覆盖：连接落 IP，SNI / 证书校验名 / Host 仍是 url.host()。
    // 与 Python SetCurlOpt(RESOLVE host:443:ip) 同语义；QNAM 做不到自定义 SNI。
    if (!p.connectIp.isEmpty()) {
        if (!nodeHttp_) {
            nodeHttp_ = new NodeHttp(this);
            connect(nodeHttp_, &NodeHttp::Log, this,
                    [this](const QString &line) { emit Log(line); });
        }
        NodeHttpRequest hr;
        hr.url = p.url;
        hr.connectIp = p.connectIp;
        hr.sniHost = p.url.host();
        hr.timeoutMs = p.timeoutMs;
        hr.maxBytes = 4 * 1024 * 1024;
        hr.headers.insert(QStringLiteral("User-Agent"),
                          QStringLiteral("JMComic/0.88 (Qt %1)")
                              .arg(QString::fromLatin1(qVersion())));
        hr.headers.insert(QStringLiteral("Cache-Control"), QStringLiteral("no-cache"));
        hr.headers.insert(QStringLiteral("Pragma"), QStringLiteral("no-cache"));
        hr.headers.insert(QStringLiteral("Expires"), QStringLiteral("0"));
        nodeHttp_->Send(hr, [gen, this, done](const NodeHttpResponse &r) {
            if (gen != generation_)
                return;
            // 3s 内只拿到响应头、体没下来（慢节点）时按失败处理，否则分数失真
            const bool ok = r.ok && r.status > 0 && r.status < 400 && !r.body.isEmpty()
                            && !r.truncated;
            done(ok, r.elapsedMs, ok ? r.body : QByteArray(),
                 ok ? QString()
                    : (r.error.isEmpty() ? QStringLiteral("http %1").arg(r.status) : r.error));
        });
        return;
    }

    QNetworkRequest req{p.url};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(p.timeoutMs);
    req.setHeader(QNetworkRequest::UserAgentHeader,
                  QStringLiteral("JMComic/0.88 (Qt %1)").arg(QString::fromLatin1(qVersion())));
    // Python 的 no-cache 头：测速不能吃缓存，否则延迟没意义
    req.setRawHeader("Cache-Control", "no-cache");
    req.setRawHeader("Pragma", "no-cache");
    req.setRawHeader("Expires", "0");

    QNetworkReply *reply = nam_->get(req);
    QElapsedTimer timer;
    timer.start();
    connect(reply, &QNetworkReply::finished, this, [reply, timer, gen, this, done] {
        reply->deleteLater();
        const qint64 elapsed = timer.elapsed();
        if (gen != generation_)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool ok = reply->error() == QNetworkReply::NoError && status > 0 && status < 400;
        const QByteArray body = ok ? reply->readAll() : QByteArray();
        done(ok && !body.isEmpty(), elapsed, ok ? body : QByteArray(),
             ok ? QString() : reply->errorString());
    });
}

// ---------------------------------------------------------------- 线路
NodeTester::Probe NodeTester::MakeLineProbe(int index, bool api) const
{
    const QString base = api ? ProxyConfig::GetApiUrl2(index) : ProxyConfig::GetImgUrl2(index);
    QUrl url(QString::fromLatin1("%1%2").arg(
        base, api ? QStringLiteral("/latest/") : QString::fromLatin1(kSpeedSample)));
    if (api) {
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("page"), QStringLiteral("0"));
        url.setQuery(q);
    }

    Probe p;
    // Python SpeedTestPingReq=3s；SpeedTestReq 走 ServerReq 默认 5s
    p.timeoutMs = api ? kProbeTimeoutMs : 5000;
    if (index == 6) {
        // Python server.py __DealHeaders：线路 6 把 host 换成 "<反代域名>/<原 host>"，
        // 且 proxyUrl 存在时不做 IP 覆盖。
        const QString proxyHost =
            api ? ProxyConfig::ProxyApiDomain2() : ProxyConfig::ProxyImgDomain2();
        const QString origin = url.host();
        QUrl u = url;
        u.setHost(proxyHost);
        u.setPath(QLatin1Char('/') + origin + url.path(QUrl::FullyEncoded));
        p.url = u;
    } else if (index == 5) {
        p.url = url;
        const QString ip = ProxyConfig::SelectedIp();
        if (!ip.isEmpty() && ProxyConfig::Mode() == HttpProxyMode::None)
            p.connectIp = ip;   // IP分流线路：用当前选中节点测
    } else {
        p.url = url;
    }
    return p;
}

void NodeTester::TestLines()
{
    Cancel();
    ++generation_;
    busy_ = true;
    StartPingPhase();
}

void NodeTester::StartPingPhase()
{
    lineResults_.clear();
    pingSamples_.clear();
    speedSamples_.clear();
    for (int i = 0; i < 6; ++i) {
        LineTestResult r;
        r.index = i + 1;
        lineResults_.append(r);
        pingSamples_.append(QVector<qint64>());
        speedSamples_.append(QVector<double>());
    }
    linePingPending_ = 0;
    const QString ip = ProxyConfig::SelectedIp();
    const bool ipReady = !ip.isEmpty() && ProxyConfig::Mode() == HttpProxyMode::None
                         && ProxyConfig::NodeUsable(ip);
    emit Log(QStringLiteral("line-test: 6 lines (2 ping samples each, ip-line node=%1)")
                 .arg(ipReady ? ip : QStringLiteral("none")));

    const int gen = generation_;
    for (int index = 1; index <= 6; ++index) {
        if (index == 5 && !ipReady) {
            // 没有可用节点也要给出确定性、可比较的状态，而不是把线路 5 藏起来
            lineResults_[4].error = QStringLiteral("未选节点");
            lineResults_[4].pingText = QStringLiteral("未选节点");
            continue;
        }
        for (int s = 0; s < kPingSamples; ++s) {
            ++linePingPending_;
            Get(MakeLineProbe(index, true),
                [this, gen, index](bool ok, qint64 elapsed, const QByteArray &,
                                   const QString &err) {
                    if (gen != generation_ || !busy_)
                        return;
                    if (ok)
                        pingSamples_[index - 1].append(elapsed);
                    else if (lineResults_[index - 1].error.isEmpty())
                        lineResults_[index - 1].error = err;
                    if (--linePingPending_ == 0)
                        StartSpeedPhase();
                });
        }
    }
    if (linePingPending_ == 0)
        StartSpeedPhase();
}

void NodeTester::StartSpeedPhase()
{
    // 每条线路用"最小 ping 耗时 // 4"作为延迟，保持 Python 口径
    for (int i = 0; i < 6; ++i) {
        QVector<qint64> &v = pingSamples_[i];
        LineTestResult &r = lineResults_[i];
        r.pingOk = !v.isEmpty();
        if (r.pingOk) {
            std::sort(v.begin(), v.end());
            r.delayMs = int(v.first() / 4);
            r.pingText = FormatDelay(r.delayMs);
        } else if (r.pingText.isEmpty()) {
            r.pingText = FailText(r.error);
        }
        emit LinePing(r.index, r.pingOk, r.delayMs, r.error);
    }
    emit Log(QStringLiteral("line-ping done, start download test"));
    speedLineIndex_ = 1;
    SpeedLine(1, 0);
}

void NodeTester::SpeedLine(int index, int sample)
{
    if (!busy_)
        return;
    if (index > 6) {
        FinishLines();
        return;
    }
    const QString ip = ProxyConfig::SelectedIp();
    const bool ipReady = !ip.isEmpty() && ProxyConfig::Mode() == HttpProxyMode::None
                         && ProxyConfig::NodeUsable(ip);
    if (index == 5 && !ipReady) {
        lineResults_[4].speedText = QStringLiteral("未选节点");
        emit LineSpeed(index, false, lineResults_[4].speedText);
        SpeedLine(index + 1, 0);
        return;
    }

    const int gen = generation_;
    Get(MakeLineProbe(index, false),
        [this, gen, index, sample](bool ok, qint64 elapsed, const QByteArray &body,
                                   const QString &err) {
            if (gen != generation_ || !busy_)
                return;
            if (ok && !body.isEmpty() && elapsed > 0)
                speedSamples_[index - 1].append(double(body.size()) * 1000.0 / double(elapsed));
            else if (lineResults_[index - 1].error.isEmpty())
                lineResults_[index - 1].error = err;
            if (sample + 1 < kSpeedSamples) {
                SpeedLine(index, sample + 1);
                return;
            }
            LineTestResult &r = lineResults_[index - 1];
            QVector<double> &v = speedSamples_[index - 1];
            r.speedOk = !v.isEmpty();
            if (r.speedOk) {
                std::sort(v.begin(), v.end());
                r.bytesPerSec = v.last();   // 取最优吞吐，抵消抖动
                r.speedText = FormatSpeed(r.bytesPerSec);
            } else if (r.speedText.isEmpty()) {
                r.speedText = FailText(r.error, true);
            }
            emit LineSpeed(index, r.speedOk, r.speedText);
            SpeedLine(index + 1, 0);
        });
}

void NodeTester::FinishLines()
{
    for (const LineTestResult &r : lineResults_)
        emit LineResult(r.index, r);
    ProxyConfig::SetLineResults(lineResults_);
    ProxyConfig::Save();   // Python SaveHistory(LastProxyResult)
    emit Log(QStringLiteral("line-test done: %1 lines").arg(lineResults_.size()));
    busy_ = false;
    emit LinesFinished();
    emit LineResultsReady(lineResults_);
}

// ---------------------------------------------------------------- 节点
void NodeTester::TestNodes()
{
    Cancel();
    ++generation_;
    busy_ = true;
    nodePhase_ = 1;
    nodes_.clear();
    FetchProxyIpList();
}

void NodeTester::FetchProxyIpList()
{
    // Python GetProxyIpInfoReq：check.jpacg.cc/resolve?proxyip=proxyip[.地区].cmliussss.net
    // 失败清空 ProxyIpLastList → ShowAllItem 回落内置 ProxyIpList。
    const QString region = ProxyConfig::RegionCode();
    const QString host = QStringLiteral("check.jpacg.cc");
    const QString proxyip = region.isEmpty()
                                ? QStringLiteral("proxyip.cmliussss.net")
                                : QStringLiteral("proxyip.%1.cmliussss.net").arg(region);
    const QUrl url(QStringLiteral("https://%1/resolve?proxyip=%2").arg(host, proxyip));
    const int gen = generation_;
    auto tryFetch = [this, gen, url](int attempt) {
        if (gen != generation_ || !busy_)
            return;
        QNetworkRequest req{url};
        req.setTransferTimeout(kFetchTimeoutMs);
        req.setRawHeader("Cache-Control", "no-cache");
        QNetworkReply *reply = nam_->get(req);
        connect(reply, &QNetworkReply::finished, this, [this, reply, gen, url, attempt] {
            reply->deleteLater();
            if (gen != generation_ || !busy_)
                return;
            QStringList ips;
            bool ok = reply->error() == QNetworkReply::NoError;
            if (ok) {
                const QJsonArray arr = QJsonDocument::fromJson(reply->readAll()).array();
                for (const QJsonValue &v : arr) {
                    // Python GetProxyIpInfoReqHandler：元素可能是 "ip:port"，取冒号前
                    const QString ip = v.toString().section(QLatin1Char(':'), 0, 0);
                    if (!ip.isEmpty())
                        ips << ip;
                }
            }
            if (!ok && attempt < 1) {
                // Python resetUrl = check2.jpacg.cc
                QUrl alt = url;
                alt.setHost(QStringLiteral("check2.jpacg.cc"));
                QNetworkRequest req2{alt};
                req2.setTransferTimeout(kFetchTimeoutMs);
                QNetworkReply *r2 = nam_->get(req2);
                connect(r2, &QNetworkReply::finished, this, [this, r2, gen, attempt] {
                    r2->deleteLater();
                    if (gen != generation_ || !busy_)
                        return;
                    QStringList ips;
                    if (r2->error() == QNetworkReply::NoError) {
                        const QJsonArray arr = QJsonDocument::fromJson(r2->readAll()).array();
                        for (const QJsonValue &v : arr) {
                            const QString ip = v.toString().section(QLatin1Char(':'), 0, 0);
                            if (!ip.isEmpty())
                                ips << ip;
                        }
                    }
                    std::stable_sort(ips.begin(), ips.end(), NaturalLess);
                    ProxyConfig::SetProxyIpLastList(ips);
                    ProxyConfig::Save();
                    emit Log(QStringLiteral("proxyip fetch(retry) %1 nodes").arg(ips.size()));
                    Q_UNUSED(attempt)
                    BuildNodes();
                });
                return;
            }
            std::stable_sort(ips.begin(), ips.end(), NaturalLess);
            ProxyConfig::SetProxyIpLastList(ips);
            ProxyConfig::Save();
            emit Log(QStringLiteral("proxyip fetch %1 nodes (region=%2)")
                         .arg(ips.size())
                         .arg(ProxyConfig::RegionCode().isEmpty() ? QStringLiteral("all")
                                                                 : ProxyConfig::RegionCode()));
            BuildNodes();
        });
    };
    tryFetch(0);
}

void NodeTester::BuildNodes()
{
    nodes_ = ProxyConfig::AllNodes();
    emit Log(QStringLiteral("node-test: %1 nodes (ProxyIP %2 / 自定义 %3 / 优选 %4)")
                 .arg(nodes_.size())
                 .arg(int(std::count_if(nodes_.begin(), nodes_.end(), [](const NodeResult &r) {
                     return r.kind == NodeKind::ProxyIp;
                 })))
                 .arg(int(std::count_if(nodes_.begin(), nodes_.end(), [](const NodeResult &r) {
                     return r.kind == NodeKind::Custom;
                 })))
                 .arg(int(std::count_if(nodes_.begin(), nodes_.end(), [](const NodeResult &r) {
                     return r.kind == NodeKind::Preferred;
                 }))));
    emit NodeListReady(nodes_);
    for (int i = 0; i < nodes_.size(); ++i)
        nodePingQueue_.enqueue(i);
    nodePhase_ = 2;
    PumpNodes();
}

void NodeTester::PumpNodes()
{
    if (!busy_)
        return;
    if (nodePhase_ == 2) {
        while (nodePingInflight_.size() < kNodeConcurrency && !nodePingQueue_.isEmpty()) {
            const int idx = nodePingQueue_.dequeue();
            nodePingInflight_.insert(idx);
            PingNode(idx);
        }
        if (nodePingInflight_.isEmpty() && nodePingQueue_.isEmpty()) {
            nodePhase_ = 3;
            for (int i = 0; i < nodes_.size(); ++i) {
                if (!nodes_.at(i).failed)
                    nodeSpeedQueue_.enqueue(i);
            }
            emit Log(QStringLiteral("node-ping done, speed test %1").arg(nodeSpeedQueue_.size()));
            PumpNodes();
        }
        return;
    }
    if (nodePhase_ == 3) {
        while (nodeSpeedInflight_.size() < kNodeConcurrency && !nodeSpeedQueue_.isEmpty()) {
            const int idx = nodeSpeedQueue_.dequeue();
            nodeSpeedInflight_.insert(idx);
            SpeedNode(idx);
        }
        if (nodeSpeedInflight_.isEmpty() && nodeSpeedQueue_.isEmpty()) {
            // 测速结束后补一轮 ipinfo：只查「ping 通且还没有地区」的节点。
            // Python 是对全部 item 并发发，但失败节点本来选不上，56 个全查会让
            // 整轮多等十几秒（4 并发 × 5s 超时）；这里按可用节点裁剪。
            for (int i = 0; i < nodes_.size(); ++i) {
                const NodeResult &r = nodes_.at(i);
                if (!r.failed && r.country.isEmpty())
                    nodeInfoQueue_.enqueue(i);
            }
            if (nodeInfoQueue_.isEmpty()) {
                nodePhase_ = 0;
                FinishNodes();
                return;
            }
            nodePhase_ = 4;
            emit Log(QStringLiteral("node-speed done, ipinfo %1").arg(nodeInfoQueue_.size()));
            PumpNodes();
        }
        return;
    }
    if (nodePhase_ == 4) {
        while (nodeInfoInflight_.size() < kInfoConcurrency && !nodeInfoQueue_.isEmpty()) {
            const int idx = nodeInfoQueue_.dequeue();
            nodeInfoInflight_.insert(idx);
            FetchIpInfo(idx);
        }
        if (nodeInfoInflight_.isEmpty() && nodeInfoQueue_.isEmpty()) {
            nodePhase_ = 0;
            FinishNodes();
        }
    }
}

void NodeTester::PingNode(int index)
{
    if (index < 0 || index >= nodes_.size())
        return;
    QUrl url(ProxyConfig::GetApiUrl2(5) + QStringLiteral("/latest/"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("page"), QStringLiteral("0"));
    url.setQuery(q);

    Probe p;
    p.url = url;
    p.connectIp = nodes_.at(index).ip;
    p.timeoutMs = kProbeTimeoutMs;

    const int gen = generation_;
    Get(p, [this, index, gen](bool ok, qint64 elapsed, const QByteArray &, const QString &err) {
        if (gen != generation_ || !busy_ || nodePhase_ != 2)
            return;
        nodePingInflight_.remove(index);
        NodeResult &r = nodes_[index];
        r.failed = !ok;
        r.delayMs = ok ? int(elapsed / 4) : 0;
        r.error = ok ? QString() : err;
        emit NodeUpdated(r);
        PumpNodes();
    });
}

void NodeTester::SpeedNode(int index)
{
    if (index < 0 || index >= nodes_.size())
        return;
    Probe p;
    p.url = QUrl(ProxyConfig::GetImgUrl2(5) + QString::fromLatin1(kCoverSample));
    p.connectIp = nodes_.at(index).ip;
    p.timeoutMs = kProbeTimeoutMs;

    const int gen = generation_;
    Get(p, [this, index, gen](bool ok, qint64 elapsed, const QByteArray &, const QString &err) {
        if (gen != generation_ || !busy_ || nodePhase_ != 3)
            return;
        nodeSpeedInflight_.remove(index);
        NodeResult &r = nodes_[index];
        r.downloadFailed = !ok;
        r.downloadMs = ok ? int(elapsed / 4) : 0;
        if (!ok && r.error.isEmpty())
            r.error = err;
        emit NodeUpdated(r);
        PumpNodes();
    });
}

// Python GetIpInfoReq：https://parse.jpacg.cc/ipinfo?ip=<ip>（失败换 parse2）
// 只用于补「国家·省份」与 isp —— 节点列表的地区列、「自定义ProxyIP地区」下拉都靠它。
// 与 ping/speed 不同：这条**不走节点 IP 覆盖**（Python SetProxy(0,"","") + SetIndex(0,0)），
// 因为查的是 IP 归属地本身，走节点会把查询源也换掉。
void NodeTester::FetchIpInfo(int index, int attempt)
{
    if (index < 0 || index >= nodes_.size())
        return;
    QUrl url(QStringLiteral("https://parse.jpacg.cc/ipinfo"));
    if (attempt > 0)   // Python resetUrl：parse2.jpacg.cc
        url.setHost(QStringLiteral("parse2.jpacg.cc"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("ip"), nodes_.at(index).ip);
    url.setQuery(q);

    Probe p;
    p.url = url;
    p.timeoutMs = kInfoTimeoutMs;

    const int gen = generation_;
    Get(p, [this, index, gen, attempt](bool ok, qint64, const QByteArray &body, const QString &) {
        if (gen != generation_ || nodePhase_ != 4)
            return;
        QJsonObject data;
        if (ok) {
            const QJsonObject root = QJsonDocument::fromJson(body).object();
            if (root.value(QStringLiteral("code")).toInt() == 200)
                data = root.value(QStringLiteral("data")).toObject();
        }
        if (data.isEmpty() && attempt < 1) {
            // 备用域名重试：仍在同一个 inflight 名额里，不重新排队
            FetchIpInfo(index, attempt + 1);
            return;
        }
        nodeInfoInflight_.remove(index);
        if (!data.isEmpty()) {
            const QString country = data.value(QStringLiteral("country")).toString();
            const QString province = data.value(QStringLiteral("province")).toString();
            NodeResult &r = nodes_[index];
            // Python：item.country = f"{country}·{province}"，item.asn = isp
            r.country = province.isEmpty() ? country : country + QStringLiteral("·") + province;
            r.asn = data.value(QStringLiteral("isp")).toString();
            emit NodeUpdated(r);
        }
        PumpNodes();
    });
}

void NodeTester::FinishNodes()
{    busy_ = false;
    const QString current = ProxyConfig::SelectedIp();
    QVector<NodeResult> sorted = nodes_;
    ProxyConfig::SortResults(sorted, current, NodeSortMode::Smart);
    const QString best = ProxyConfig::PickBest(sorted, current);
    if (!best.isEmpty() && best != current)
        ProxyConfig::SetSelectedIp(best);

    ProxyConfig::SetResults(sorted);
    QStringList ips;
    for (const NodeResult &r : sorted)
        ips << r.ip;
    ProxyConfig::SetNodes(ips);
    ProxyConfig::Save();   // Python Setting.LastIpResult

    int pingOk = 0, imgOk = 0;
    for (const NodeResult &r : sorted) {
        if (!r.failed)
            ++pingOk;
        if (!r.failed && !r.downloadFailed)
            ++imgOk;
    }
    emit Log(QStringLiteral("node-test done: total=%1 ping_ok=%2 img_ok=%3 selected=%4")
                 .arg(sorted.size())
                 .arg(pingOk)
                 .arg(imgOk)
                 .arg(best.isEmpty() ? QStringLiteral("-") : best));
    emit NodesFinished(sorted, best);
}
