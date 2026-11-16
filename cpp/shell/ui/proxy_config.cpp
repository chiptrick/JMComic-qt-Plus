#include "proxy_config.h"

#include "../doh_client.h"
#include "../net_backend.h"
#include "app_paths.h"
#include "data_migrator.h"
#include "region_code.h"

#include <QDir>
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>

namespace {

// 与 Python GlobalConfig 的 Url2List / PicUrlList / Cdn* / Proxy* 一致
const char *kApiList[] = {
    "https://www.cdnhjk.net",
    "https://www.cdngwc.cc",
    "https://www.cdngwc.net",
    "https://www.cdngwc.club",
};
const char *kImgList[] = {
    "https://cdn-msp.jmapiproxy1.cc",
    "https://cdn-msp.jmapiproxy3.cc",
    "https://cdn-msp.jmapinodeudzn.net",
    "https://cdn-msp.jmdanjonproxy.xyz",
};
constexpr const char *kCdnApi = "https://www.cdngwc.cc";
constexpr const char *kCdnImg = "https://cdn-msp.jmapiproxy3.cc";
constexpr const char *kProxyApi = "https://www.cdnhjk.net";
constexpr const char *kProxyImg = "https://cdn-msp.jmapiproxy3.cc";
constexpr const char *kProxyApiDomain2 = "jm2-api.jpacg.cc";
constexpr const char *kProxyImgDomain2 = "jm2-img.jpacg.cc";

// Python Setting.DohAddress 默认值
constexpr const char *kDefaultDohAddress = "https://parse.jpacg.cc/parse";

// Python Setting.PreferCDNList 默认值
constexpr const char *kDefaultCustomIps =
    "104.18.227.172,104.16.16.230,104.16.108.62,104.24.169.89,104.19.229.139,104.21.73.214";

// Python GlobalConfig.ProxyIpList（26 个，顺序固定）
const char *kProxyIpList[] = {
    "158.180.231.216", "163.47.42.64",   "43.170.8.95",    "150.136.219.11",
    "159.89.91.17",    "165.232.51.34",  "172.174.11.248", "198.199.84.192",
    "43.153.105.7",    "43.170.25.96",   "103.7.138.56",   "107.151.188.57",
    "107.172.32.207",  "129.213.150.222","147.75.230.33",  "192.9.250.241",
    "47.251.95.178",   "152.70.232.72",  "95.216.46.85",   "159.203.34.9",
    "107.172.145.153", "46.224.21.216",  "91.99.20.251",   "159.60.146.82",
    "204.168.238.95",  "62.238.51.190",
};

// Python GlobalConfig.BestCfIpList（24 个，含 IPv6，顺序固定）
const char *kBestCfIpList[] = {
    "104.18.40.104",   "172.64.229.155", "198.41.208.26",  "172.66.2.153",
    "8.35.211.230",    "172.66.0.115",   "188.164.248.227","104.26.10.124",
    "172.67.73.123",   "104.26.11.206",  "104.26.1.248",   "104.17.189.152",
    "104.17.173.109",  "91.193.58.106",  "104.17.61.57",
    "2606:4700:0:f920:12e9:bef7:bd1b:bf3", "2606:4700:0:77:ba66:ef50:489c:299d",
    "2606:4700:0:a0:6574:f93:6c28:17c5", "2606:4700::2d:a321:64d3",
    "2606:4700:0:a0:a55f:f7f9:5a17:8b8e", "2606:4700:0:e0:9996:e26c:4b53:e69a",
    "2606:4700:0:a0:65dc:517c:ad49:a2fd", "2606:4700:0:f920:12c2:bb61:198a:ca56",
    "2606:4700:0:e0:c653:a255:83dc:d0c",
};

QString SettingsPath()
{
    // 测试/多实例隔离：JM_UI_CONFIG 指定目录（与 JM_COVER_CACHE / JM_PAGE_CACHE 同一套约定）。
    // 其余情况交给 AppPaths：Android 上不再退回只读的 applicationDirPath()，
    // 否则分流/节点配置每次升级都会丢。
    const QString override = qEnvironmentVariable("JM_UI_CONFIG").trimmed();
    if (!override.isEmpty()) {
        QDir().mkpath(override);
        return QDir(override).filePath(QStringLiteral("ui.ini"));
    }
    AppPaths::MigrateLegacyFile(QStringLiteral("ui.ini"));
    return AppPaths::ConfigFile(QStringLiteral("ui.ini"));
}

struct State {
    int apiLine = 1;
    int imgLine = 1;
    QString ip;
    QStringList nodes;
    QVector<NodeResult> results;
    QVector<LineTestResult> lineResults;
    QString customIpText = QString::fromLatin1(kDefaultCustomIps);
    QStringList proxyIpLastList;
    QString region;
    int mode = 0;
    // Python ProxySetting 三项；dohEnabled 是 C++ 侧「把 DoH 结果用于真实流量」的开关
    QString dohAddress = QString::fromLatin1(kDefaultDohAddress);
    bool dohEnabled = false;
    bool echEnabled = true;
    // 0.90 产品决策：HTTP/3 默认开启。ECH 可用时先试 H3+ECH；服务端拒绝则
    // 按「隐藏 SNI 优先」回退 TCP+ECH，并在下次启动/换节点时重新尝试组合。
    bool http3Enabled = true;
    QString echConfig;
    // 节点测速并发线程数：1 = 兼容档（单线程事件循环），2..16 = 工作线程池
    int speedThreads = 1;
    bool loaded = false;
};

State &S()
{
    static State s;
    return s;
}

// Python Http3EchFallback：进程内自适应开关，重启/保存设置时复位，不落盘
struct AdvancedFallback {
    bool http3Off = false;
    bool echOff = false;
};

AdvancedFallback &Fallback()
{
    static AdvancedFallback f;
    return f;
}

// Q4：H3+ECH 组合被服务端策略拒绝的 (host, 节点) 集合。刻意只放内存：
//   * 冷启动 → 集合为空，自动重试组合；
//   * 换节点 → key 里的 IP 变了，且 SetSelectedIp() 会显式清空；
//   * 绝不写 alt-svc.json —— 那条缓存是「该域名不支持 H3」的持久化判定，
//     而这里只是「这条路线上的 ECH 组合不行」，不是协议能力结论。
QSet<QString> &EchH3Blocks()
{
    static QSet<QString> s;
    return s;
}

QString EchH3Key(const QString &host, const QString &nodeIp)
{
    return host.trimmed().toLower() + QLatin1Char('|')
           + (nodeIp.trimmed().isEmpty() ? QStringLiteral("direct") : nodeIp.trimmed());
}

// ini 里避免分隔符冲突：文本字段统一 base64
QString Enc(const QString &v)
{
    return QString::fromLatin1(v.toUtf8().toBase64());
}
QString Dec(const QString &v)
{
    return QString::fromUtf8(QByteArray::fromBase64(v.toLatin1()));
}

bool ValidIp(const QString &ip)
{
    const QString t = ip.trimmed();
    if (t.isEmpty())
        return false;
    QHostAddress a(t);
    return !a.isNull();
}

QStringList ToStringList(const char *const *arr, int n)
{
    QStringList out;
    for (int i = 0; i < n; ++i)
        out << QString::fromLatin1(arr[i]);
    return out;
}

}  // namespace

QString NodeResult::ShortName() const
{
    switch (kind) {
    case NodeKind::ProxyIp:
        return QStringLiteral("ProxyIP %1").arg(ordinal);
    case NodeKind::Custom:
        return QStringLiteral("自定义 %1").arg(ordinal);
    default:
        return QStringLiteral("优选 %1").arg(ordinal);
    }
}

namespace ProxyConfig {

QVector<ProxyLine> Lines()
{
    QVector<ProxyLine> out;
    for (int i = 1; i <= 6; ++i) {
        ProxyLine l;
        l.index = i;
        if (i <= 4) {
            l.name = QStringLiteral("分流%1").arg(i);
            l.apiBase = QString::fromLatin1(kApiList[i - 1]);
            l.imgBase = QString::fromLatin1(kImgList[i - 1]);
        } else if (i == 5) {
            l.name = QStringLiteral("IP分流");
            l.apiBase = QString::fromLatin1(kCdnApi);
            l.imgBase = QString::fromLatin1(kCdnImg);
            l.cdn = true;
        } else {
            l.name = QStringLiteral("US反代分流");
            l.apiBase = QString::fromLatin1(kProxyApi);
            l.imgBase = QString::fromLatin1(kProxyImg);
            l.proxy = true;
        }
        out.append(l);
    }
    return out;
}

ProxyLine LineAt(int index)
{
    const QVector<ProxyLine> all = Lines();
    // Python GetApiUrl2 对越界索引没有保护；这里显式回落到线路 1，避免越界崩溃
    if (index < 1 || index > 6)
        return all.first();
    return all.at(index - 1);
}

ProxyLine CdnLine() { return LineAt(5); }
bool IsCdnIndex(int index) { return index == 5; }
bool IsProxyIndex(int index) { return index == 6; }

QVector<int> DisplayOrder()
{
    // Python ui_login_new.ui speedGrid 的行顺序：1,2,3,4,6(US反代),5(IP分流)
    return {1, 2, 3, 4, 6, 5};
}

QString LineLabel(int index)
{
    if (index < 1 || index > 6)
        return LineAt(1).name;
    return LineAt(index).name;
}

QString GetApiUrl2(int index)
{
    if (IsCdnIndex(index))
        return QString::fromLatin1(kCdnApi);
    if (IsProxyIndex(index))
        return QString::fromLatin1(kProxyApi);
    if (index >= 7 || index < 1)   // Python：自定义域名索引回落列表第 1 项
        return QString::fromLatin1(kApiList[0]);
    return QString::fromLatin1(kApiList[index - 1]);
}

QString GetImgUrl2(int index)
{
    if (IsCdnIndex(index))
        return QString::fromLatin1(kCdnImg);
    if (IsProxyIndex(index))
        return QString::fromLatin1(kProxyImg);
    if (index >= 7 || index < 1)
        return QString::fromLatin1(kImgList[0]);
    return QString::fromLatin1(kImgList[index - 1]);
}

QString ProxyApiDomain2() { return QString::fromLatin1(kProxyApiDomain2); }
QString ProxyImgDomain2() { return QString::fromLatin1(kProxyImgDomain2); }

int ApiLine() { return qBound(1, S().apiLine, 6); }
void SetApiLine(int index) { S().apiLine = qBound(1, index, 6); }
int ImgLine() { return qBound(1, S().imgLine, 6); }
void SetImgLine(int index) { S().imgLine = qBound(1, index, 6); }

QString SelectedIp() { return S().ip; }
void SetSelectedIp(const QString &ip)
{
    const QString next = ip.trimmed();
    if (next == S().ip)
        return;
    S().ip = next;
    // Q4：换节点 = 换一条路径到前端，重新尝试 H3+ECH 组合。
    ResetEchH3Blocks();
}

QStringList Nodes() { return S().nodes; }
void SetNodes(const QStringList &ips) { S().nodes = ips; }

QStringList ProxyIpList()
{
    return ToStringList(kProxyIpList, int(sizeof(kProxyIpList) / sizeof(kProxyIpList[0])));
}
QStringList BestCfIpList()
{
    return ToStringList(kBestCfIpList, int(sizeof(kBestCfIpList) / sizeof(kBestCfIpList[0])));
}
QStringList DefaultNodes() { return ProxyIpList(); }

QString CustomIpText() { return S().customIpText; }
void SetCustomIpText(const QString &text) { S().customIpText = text; }
QStringList CustomIpList()
{
    // Python：re.split(r'[、，；;,\s]\s*', Setting.PreferCDNList.value)
    const QRegularExpression sep(QStringLiteral("[、，；;,\\s]\\s*"));
    return S().customIpText.split(sep, Qt::SkipEmptyParts);
}
QStringList ProxyIpLastList() { return S().proxyIpLastList; }
void SetProxyIpLastList(const QStringList &ips) { S().proxyIpLastList = ips; }
QString RegionCode() { return S().region; }
void SetRegionCode(const QString &code) { S().region = code.trimmed(); }

QStringList RegionNames(const QVector<NodeResult> &nodes)
{
    // Python RegionNames：`item.country`（形如「美国·Los Angeles」）取 · 前部分，
    // 去重后 names.sort()。只认确实拿到过地区的节点，没测过就是空列表。
    QStringList names;
    QSet<QString> seen;
    for (const NodeResult &r : nodes) {
        const QString name = RegionCode::ShortName(r.country);
        if (name.isEmpty() || seen.contains(name))
            continue;
        seen.insert(name);
        names << name;
    }
    names.sort();
    return names;
}
HttpProxyMode Mode() { return static_cast<HttpProxyMode>(qBound(0, S().mode, 3)); }
void SetMode(int mode) { S().mode = qBound(0, mode, 3); }

QVector<NodeResult> Results() { return S().results; }
void SetResults(const QVector<NodeResult> &results) { S().results = results; }

QString NodeName(NodeKind kind, int ordinal)
{
    switch (kind) {
    case NodeKind::ProxyIp:
        return QStringLiteral("ProxyIP分流%1").arg(ordinal);
    case NodeKind::Custom:
        return QStringLiteral("自定义分流%1").arg(ordinal);
    default:
        return QStringLiteral("CDN分流%1").arg(ordinal);
    }
}

NodeKind KindOf(const QString &name)
{
    // Python component/list/proxy_node_card.py TagKind
    if (name.startsWith(QStringLiteral("ProxyIP")))
        return NodeKind::ProxyIp;
    if (name.startsWith(QStringLiteral("自定义")))
        return NodeKind::Custom;
    return NodeKind::Preferred;
}

bool MatchFilter(NodeKind kind, NodeFilter filter)
{
    switch (filter) {
    case NodeFilter::ProxyGroup:
        return kind == NodeKind::ProxyIp || kind == NodeKind::Custom;
    case NodeFilter::PreferredGroup:
        return kind == NodeKind::Preferred;
    default:
        return true;
    }
}

QVector<NodeResult> AllNodes()
{
    // Python ShowAllItem：三类顺序、前两类用 allips 去重、第三类不去重。
    // 组内序号 j 取 enumerate 的原始序号（被去重的项仍占位）。
    QVector<NodeResult> out;
    QSet<QString> allips;
    auto add = [&out](const QString &ip, NodeKind kind, int ordinal) {
        NodeResult r;
        r.ip = ip.trimmed();
        r.kind = kind;
        r.ordinal = ordinal;
        r.name = NodeName(kind, ordinal);
        out.append(r);
    };

    // 1) ProxyIP分流：成功获取过的列表优先，否则内置 26 个
    QStringList proxyIps = S().proxyIpLastList;
    if (proxyIps.isEmpty())
        proxyIps = ProxyIpList();
    int j = 0;
    for (const QString &ip : proxyIps) {
        ++j;
        const QString t = ip.trimmed();
        if (t.isEmpty() || allips.contains(t))
            continue;
        allips.insert(t);
        add(t, NodeKind::ProxyIp, j);
    }

    // 2) 自定义分流：Setting.PreferCDNList
    j = 0;
    const QStringList custom = CustomIpList();
    for (const QString &ip : custom) {
        ++j;
        const QString t = ip.trimmed();
        if (t.isEmpty() || allips.contains(t))
            continue;
        allips.insert(t);
        add(t, NodeKind::Custom, j);
    }

    // 3) 优选（CDN分流）：BestCfIpList，Python 这里既不查 allips 也不写入
    j = 0;
    const QStringList best = BestCfIpList();
    for (const QString &ip : best) {
        ++j;
        const QString t = ip.trimmed();
        if (t.isEmpty())
            continue;
        add(t, NodeKind::Preferred, j);
    }

    // Python LoadHistory：把上次测速结果按 IP 回填
    for (NodeResult &r : out) {
        for (const NodeResult &h : S().results) {
            if (h.ip != r.ip)
                continue;
            r.delayMs = h.delayMs;
            r.downloadMs = h.downloadMs;
            r.failed = h.failed;
            r.downloadFailed = h.downloadFailed;
            r.error = h.error;
            r.country = h.country;
            r.asn = h.asn;
            break;
        }
    }
    return out;
}

void SortResults(QVector<NodeResult> &results, const QString &selectedIp, NodeSortMode mode)
{
    // Python：智能排序 = 选中优先 → 失败沉底 → 0.8*延迟+0.2*下载；
    // 按延迟/按下载 = 失败沉底 → 对应数值（未测速记为 1e9），选中不置顶。
    // stable_sort 保证同档顺序与 Python sorted() 一致（都是稳定排序）。
    std::stable_sort(results.begin(), results.end(),
                     [&selectedIp, mode](const NodeResult &a, const NodeResult &b) {
                         if (mode == NodeSortMode::Smart) {
                             const bool aSel = !selectedIp.isEmpty() && a.ip == selectedIp;
                             const bool bSel = !selectedIp.isEmpty() && b.ip == selectedIp;
                             if (aSel != bSel)
                                 return aSel;
                             if (a.IsFail() != b.IsFail())
                                 return !a.IsFail();
                             return a.Score() < b.Score();
                         }
                         if (a.IsFail() != b.IsFail())
                             return !a.IsFail();
                         if (mode == NodeSortMode::Latency) {
                             const int av = a.delayMs > 0 ? a.delayMs : 1000000000;
                             const int bv = b.delayMs > 0 ? b.delayMs : 1000000000;
                             return av < bv;
                         }
                         const int av = a.downloadMs > 0 ? a.downloadMs : 1000000000;
                         const int bv = b.downloadMs > 0 ? b.downloadMs : 1000000000;
                         return av < bv;
                     });
}

QString PickBest(const QVector<NodeResult> &results, const QString &current)
{
    // Python CheckSelectBox：选中项只要还在列表里就保持（哪怕已测速失败）；
    // 不在列表里则取 sorted(allItems)[0] 并写回 ProxyIpValue。
    if (results.isEmpty())
        return current;
    QVector<NodeResult> sorted = results;
    SortResults(sorted, QString(), NodeSortMode::Smart);
    for (const NodeResult &r : sorted) {
        if (!current.isEmpty() && r.ip == current)
            return current;
    }
    return sorted.first().ip;
}

QVector<LineTestResult> LineResults() { return S().lineResults; }
void SetLineResults(const QVector<LineTestResult> &results) { S().lineResults = results; }

int RecommendLine(bool api, const QVector<LineTestResult> &results)
{
    // 结合线路测速结果与 IP 分流配置推荐接口/图床线路：
    //   * 只考虑 ping 成功的线路；IP分流(5) 还要求已选节点、无系统代理且节点 ping 通过
    //   * 先比延迟（升序），同延迟比吞吐（降序），再同则取编号小的
    //   * 没有任何有效结果 → Python 默认线路 1
    const bool ipReady = !SelectedIp().isEmpty() && Mode() == HttpProxyMode::None
                         && NodeUsable(SelectedIp());
    int bestIndex = 0;
    int bestDelay = 0;
    double bestSpeed = -1.0;
    const int currentLine = api ? ApiLine() : ImgLine();
    for (const LineTestResult &r : results) {
        if (r.index < 1 || r.index > 6 || !r.pingOk || r.delayMs <= 0)
            continue;
        if (r.index == 5 && !ipReady)
            continue;
        if (bestIndex == 0 || r.delayMs < bestDelay
            || (r.delayMs == bestDelay && r.bytesPerSec > bestSpeed)
            || (r.delayMs == bestDelay && qFuzzyCompare(1.0 + r.bytesPerSec, 1.0 + bestSpeed)
                && r.index == currentLine)) {
            bestIndex = r.index;
            bestDelay = r.delayMs;
            bestSpeed = r.bytesPerSec;
        }
    }
    return bestIndex == 0 ? 1 : bestIndex;
}

bool NodeUsable(const QString &ip)
{
    if (ip.isEmpty())
        return false;
    // 没有历史测速结果时按"未知即可用"处理，避免刚选完节点就被禁用
    for (const NodeResult &r : S().results) {
        if (r.ip == ip)
            return !r.failed;
    }
    return true;
}

void SyncDohClient()
{
    DohClient::I().Configure(DohAddress(), EchEnabled());
    DohClient::I().SetActive(DohEnabled());
    // 开启后立即预热当前两条线路的域名：否则第一个请求只能直连，
    // 用户会觉得「保存了但没生效」。缓存按 TTL 复用，重复调用由 resolving_ 去重。
    if (DohEnabled()) {
        DohClient::I().Resolve(QUrl(GetApiUrl2(ApiLine())).host());
        DohClient::I().Resolve(QUrl(GetImgUrl2(ImgLine())).host());
    }
}

QString DohAddress() { return S().dohAddress; }
void SetDohAddress(const QString &url) { S().dohAddress = url.trimmed(); SyncDohClient(); }
bool DohEnabled() { return S().dohEnabled; }
void SetDohEnabled(bool on) { S().dohEnabled = on; SyncDohClient(); }
bool EchEnabled() { return S().echEnabled; }
void SetEchEnabled(bool on) { S().echEnabled = on; SyncDohClient(); }
bool Http3Enabled() { return S().http3Enabled; }
void SetHttp3Enabled(bool on) { S().http3Enabled = on; }
QString EchConfig() { return S().echConfig; }
void SetEchConfig(const QString &cfg) { S().echConfig = cfg; }

bool AdvancedTlsSupported()
{
    // 与 Python IsAdvancedTlsSupported() 同义，但结论来自真实探测：
    // 只有当前 TLS 后端能把 ECHConfigList 注入 ClientHello 才算支持。
    // 替换成 OpenSSL 3.2+/BoringSSL 后，NetBackend::Tls().ech 会变 true，
    // 这里与页面状态无需改动。
    return NetBackend::Tls().ech;
}

bool EchOff() { return Fallback().echOff; }
bool Http3Off() { return Fallback().http3Off; }

void DowngradeAdvanced(bool echFail)
{
    // Python DowngradeTlsOptions：不支持高级 TLS 时不值得降级重试，
    // 这里仍把状态标出来，供页面显示「已回退」，但返回语义由调用方按 false 处理。
    if (echFail)
        Fallback().echOff = true;
    else
        Fallback().http3Off = true;
}

void ResetAdvancedFallback()
{
    Fallback().http3Off = false;
    Fallback().echOff = false;
    // 页面保存 = 用户重新确认高级设置，等同于给 H3+ECH 组合一次新机会
    EchH3Blocks().clear();
    SyncDohClient();   // 重新尝试 ECH 配置，并把 DoH 开关同步到客户端
}

QString EchH3NodeKey(const QString &host, const QString &connectIp)
{
    if (!connectIp.isEmpty())
        return connectIp.trimmed();
    if (DohEnabled()) {
        const QStringList ips = DohClient::I().CachedIps(host);
        if (!ips.isEmpty())
            return ips.first();
    }
    return QString();
}

bool EchH3Blocked(const QString &host, const QString &nodeIp)
{
    if (host.isEmpty())
        return false;
    return EchH3Blocks().contains(EchH3Key(host, nodeIp));
}

void NoteEchH3Rejected(const QString &host, const QString &nodeIp)
{
    if (host.isEmpty())
        return;
    EchH3Blocks().insert(EchH3Key(host, nodeIp));
    // UI 状态也标成「已降级」，与 Python Http3EchFallback 的语义对齐
    Fallback().http3Off = true;
}

void ResetEchH3Blocks()
{
    EchH3Blocks().clear();
    Fallback().http3Off = false;
}

int EchH3BlockCount()
{
    return EchH3Blocks().size();
}

namespace {
// A7：H3 失败注入。g_h3Inject 只在主线程写（命令行启动期），
// g_h3InjectUsed 用原子量兜底（将来若把请求挪到工作线程也不会错）。
QString g_h3Inject;
std::atomic<int> g_h3InjectUsed{0};
}   // namespace

QString H3FailInject() { return g_h3Inject; }

void SetH3FailInject(const QString &mode)
{
    g_h3Inject = mode;
    g_h3InjectUsed.store(0);
}

bool TakeH3FailInject()
{
    const QString mode = g_h3Inject;
    if (mode.isEmpty())
        return false;
    if (mode == QLatin1String("bad-port"))
        return true;                                   // 每次都失败
    if (mode == QLatin1String("drop-first"))
        return g_h3InjectUsed.fetch_add(1) == 0;       // 只有第一次失败
    return false;
}

namespace {
// Q5：回退计数。原子量只是防御——当前所有计数点都在主线程。
std::atomic<int> g_h3Ok{0};
std::atomic<int> g_h3Fail{0};
std::atomic<int> g_h3Fallback{0};
std::atomic<int> g_echOk{0};
std::atomic<int> g_echFail{0};
}   // namespace

void NoteH3(bool ok)
{
    if (ok)
        g_h3Ok.fetch_add(1);
    else
        g_h3Fail.fetch_add(1);
}

void NoteH3Fallback() { g_h3Fallback.fetch_add(1); }

void NoteEch(bool success)
{
    if (success)
        g_echOk.fetch_add(1);
    else
        g_echFail.fetch_add(1);
}

AdvancedCounters Counters()
{
    AdvancedCounters c;
    c.h3Ok = g_h3Ok.load();
    c.h3Fail = g_h3Fail.load();
    c.h3Fallback = g_h3Fallback.load();
    c.echOk = g_echOk.load();
    c.echFail = g_echFail.load();
    return c;
}

QString CountersLine()
{
    const AdvancedCounters c = Counters();
    return QStringLiteral("H3 成功 %1 / 失败 %2 / 回退 %3 · ECH SUCCESS %4 / FAILED %5")
        .arg(c.h3Ok).arg(c.h3Fail).arg(c.h3Fallback).arg(c.echOk).arg(c.echFail);
}

void ResetCounters()
{
    g_h3Ok.store(0);
    g_h3Fail.store(0);
    g_h3Fallback.store(0);
    g_echOk.store(0);
    g_echFail.store(0);
}

QString AdvancedStatus()
{
    const TlsCapabilities tls = NetBackend::Tls();
    const QuicCapabilities quic = NetBackend::Quic();
    const QString echState =
        !EchEnabled() ? QStringLiteral("关")
        : EchOff()    ? QStringLiteral("已降级")
        : tls.ech     ? QStringLiteral("开")
                      : QStringLiteral("开·后端不支持，已回退普通 TLS");
    const QString h3State =
        !Http3Enabled() ? QStringLiteral("关")
        : Http3Off()    ? QStringLiteral("已降级")
        : quic.http3    ? QStringLiteral("开（QUIC/HTTP3，失败回退 HTTP/2）")
                        : QStringLiteral("开·后端不支持，已回退 HTTP/2");
    // 页面与自检共用这一份：能力摘要 + 本次会话的降级计数（Q5）
    // echH3Blocked 是 Q4 的进程内抑制数；新进程恒为 0，用来证明「下次启动重试」
    return QStringLiteral("DOH %1（%2） · ECH %3 · HTTP3 %4 · %5 · %6 · echH3Blocked=%7")
        .arg(DohEnabled() ? QStringLiteral("开") : QStringLiteral("仅检查"), DohAddress(),
             echState, h3State, NetBackend::Summary(), CountersLine())
        .arg(EchH3BlockCount());
}

namespace {

// 本机/局域网地址：绝不能套用"节点 IP 覆盖"或反代改写。
// 真机/桌面都可能出现这种 URL（本地测试服务器、NAS 图床、局域网缓存），
// 一旦被改成连 CDN 节点，请求会被对端当成错误的主机名，返回半截正文甚至空包。
bool IsLocalOrPrivateHost(const QString &host)
{
    const QString h = host.trimmed().toLower();
    if (h.isEmpty())
        return false;
    if (h == QLatin1String("localhost") || h == QLatin1String("::1")
        || h == QLatin1String("0.0.0.0"))
        return true;
    if (h.startsWith(QLatin1String("127.")))
        return true;
    if (h.startsWith(QLatin1String("10.")) || h.startsWith(QLatin1String("192.168.")))
        return true;
    if (h.startsWith(QLatin1String("172."))) {
        const QStringList parts = h.split(QLatin1Char('.'));
        if (parts.size() >= 2) {
            const int second = parts.at(1).toInt();
            if (second >= 16 && second <= 31)
                return true;
        }
    }
    return false;
}

}  // namespace

RoutePlan ResolveRoute(const QUrl &url, bool isApi)
{
    // Python req.py 的优先级：
    //   1) proxyUrl（线路 6 反代）存在 → URL 改写，且 ipList 强制为空
    //   2) 系统/用户代理（IsHttpProxy != 0）→ 不做 DNS 覆盖
    //   3) 线路 5 且 ProxyIpValue 有效 → RESOLVE host:443:ip
    //   4) 其余 → 域名直连
    RoutePlan plan;
    plan.url = url;
    const int line = isApi ? ApiLine() : ImgLine();
    plan.line = line;

    // 本机/私网地址：直连（不套节点、不套反代、不走 DoH 覆盖）
    if (IsLocalOrPrivateHost(url.host())) {
        plan.reason = QStringLiteral("local host %1 direct").arg(url.host());
        return plan;
    }

    if (line == 6) {
        const QString proxyHost = isApi ? ProxyApiDomain2() : ProxyImgDomain2();
        const QString originHost = url.host();
        QUrl u = url;
        u.setHost(proxyHost);
        u.setPath(QLatin1Char('/') + originHost + url.path(QUrl::FullyEncoded));
        plan.url = u;
        plan.reason = QStringLiteral("proxy-line %1").arg(proxyHost);
        return plan;
    }

    if (line == 5) {
        if (Mode() != HttpProxyMode::None) {
            plan.reason = QStringLiteral("ip-line skipped: http proxy mode");
            return plan;
        }
        const QString ip = SelectedIp();
        if (ip.isEmpty() || !ValidIp(ip)) {
            plan.reason = QStringLiteral("ip-line fallback: no valid node");
            return plan;
        }
        if (!NodeUsable(ip)) {
            plan.reason = QStringLiteral("ip-line fallback: node unreachable");
            return plan;
        }
        plan.connectIp = ip;
        plan.sniHost = url.host();
        plan.viaNode = true;
        plan.reason = QStringLiteral("ip-line via node %1").arg(ip);
        return plan;
    }
    // 4) DoH：解析到的 IP 同样通过 NodeHttp 落地（连接 IP、SNI/证书名仍是域名）。
    //    缓存未命中时本次先走系统 DNS，同时异步预热，下次请求即可命中。
    //    有代理时不做 IP 覆盖（与 Python 的 proxyUrl/ipList 优先级一致）。
    if (DohEnabled() && Mode() == HttpProxyMode::None) {
        const QStringList ips = DohClient::I().CachedIps(url.host());
        if (!ips.isEmpty()) {
            plan.connectIp = ips.first();
            plan.sniHost = url.host();
            plan.viaNode = true;
            plan.viaDoh = true;
            plan.reason = QStringLiteral("doh %1 via %2").arg(url.host(), plan.connectIp);
            return plan;
        }
        DohClient::I().Resolve(url.host());
    }
    plan.reason = QStringLiteral("line %1 direct").arg(line);
    return plan;
}

int ClampSpeedThreads(int n)
{
    // 0 / 负数 / 缺省都落到兼容档 1；上限 16 是硬约束，UI、ini、CLI 三条入口
    // 都过这一个函数，任何一条都无法绕过。
    return qBound(1, n, kMaxSpeedThreads);
}

int SpeedThreads()
{
    Load();
    return S().speedThreads;
}

void SetSpeedThreads(int n)
{
    Load();
    S().speedThreads = ClampSpeedThreads(n);
}

void Load()
{
    if (S().loaded)
        return;
    S().loaded = true;
    QSettings s(SettingsPath(), QSettings::IniFormat);
    S().apiLine = qBound(1, s.value(QStringLiteral("proxy/apiLine"), 1).toInt(), 6);
    S().imgLine = qBound(1, s.value(QStringLiteral("proxy/imgLine"), 1).toInt(), 6);
    S().ip = s.value(QStringLiteral("proxy/ip")).toString();
    S().nodes = s.value(QStringLiteral("proxy/nodes")).toStringList();
    S().customIpText = s.value(QStringLiteral("proxy/customIpText"),
                               QString::fromLatin1(kDefaultCustomIps)).toString();
    S().proxyIpLastList = s.value(QStringLiteral("proxy/proxyIpLastList")).toStringList();
    S().region = s.value(QStringLiteral("proxy/region")).toString();
    S().mode = qBound(0, s.value(QStringLiteral("proxy/mode"), 0).toInt(), 3);
    // Python ProxySetting 的三项配置（DohAddress / EnableEch / IsOpenHTTP3）
    S().dohAddress = s.value(QStringLiteral("proxy/dohAddress"),
                             QString::fromLatin1(kDefaultDohAddress)).toString();
    S().dohEnabled = s.value(QStringLiteral("proxy/dohEnabled"), false).toBool();
    S().echEnabled = s.value(QStringLiteral("proxy/enableEch"), true).toBool();
    // 0.90 之前默认关，且 Save() 会把这个默认值写进 ini；若继续读旧键，
    // 老安装升级后仍然是关，等于「默认开启」的决策落不了地。改读新键
    // proxy/http3On（缺省 true），旧键只在 Save() 里继续写，供旧版本回读。
    S().http3Enabled = s.value(QStringLiteral("proxy/http3On"), true).toBool();
    S().echConfig = s.value(QStringLiteral("proxy/echConfig")).toString();
    // 新键：老配置没有这个键时取 1（兼容档），因此升级不会悄悄改变测速行为。
    // 读入即夹紧，避免手工把 ini 改成 999 后起 999 个线程。
    S().speedThreads = ClampSpeedThreads(s.value(QStringLiteral("proxy/speedThreads"), 1).toInt());

    // 节点结果：兼容旧的 5 字段格式，新的 9 字段把分类/组内序号/地区一起带上
    const QStringList rows = s.value(QStringLiteral("proxy/results")).toStringList();
    QVector<NodeResult> out;
    for (const QString &row : rows) {
        const QStringList f = row.split(QLatin1Char('|'));
        if (f.size() < 5)
            continue;
        NodeResult r;
        r.ip = f.at(0);
        r.delayMs = f.at(1).toInt();
        r.downloadMs = f.at(2).toInt();
        r.failed = f.at(3) == QLatin1String("1");
        r.downloadFailed = f.at(4) == QLatin1String("1");
        if (f.size() >= 9) {
            r.kind = static_cast<NodeKind>(qBound(0, f.at(5).toInt(), 2));
            r.ordinal = f.at(6).toInt();
            r.country = Dec(f.at(7));
            r.asn = Dec(f.at(8));
            r.name = NodeName(r.kind, r.ordinal);
        } else {
            r.kind = KindOf(r.name);
        }
        out.append(r);
    }
    S().results = out;

    const QStringList lineRows = s.value(QStringLiteral("proxy/lineResults")).toStringList();
    QVector<LineTestResult> lines;
    for (const QString &row : lineRows) {
        const QStringList f = row.split(QLatin1Char('|'));
        if (f.size() < 7)
            continue;
        LineTestResult r;
        r.index = f.at(0).toInt();
        r.pingOk = f.at(1) == QLatin1String("1");
        r.delayMs = f.at(2).toInt();
        r.pingText = Dec(f.at(3));
        r.speedOk = f.at(4) == QLatin1String("1");
        r.bytesPerSec = f.at(5).toDouble();
        r.speedText = Dec(f.at(6));
        if (f.size() >= 8)
            r.error = Dec(f.at(7));
        lines.append(r);
    }
    S().lineResults = lines;
    SyncDohClient();   // 启动即按配置预热 DoH / ECH
}

void Save()
{
    QSettings s(SettingsPath(), QSettings::IniFormat);
    s.setValue(QStringLiteral("proxy/apiLine"), S().apiLine);
    s.setValue(QStringLiteral("proxy/imgLine"), S().imgLine);
    s.setValue(QStringLiteral("proxy/ip"), S().ip);
    s.setValue(QStringLiteral("proxy/nodes"), S().nodes);
    s.setValue(QStringLiteral("proxy/customIpText"), S().customIpText);
    s.setValue(QStringLiteral("proxy/proxyIpLastList"), S().proxyIpLastList);
    s.setValue(QStringLiteral("proxy/region"), S().region);
    s.setValue(QStringLiteral("proxy/mode"), S().mode);
    s.setValue(QStringLiteral("proxy/dohAddress"), S().dohAddress);
    s.setValue(QStringLiteral("proxy/dohEnabled"), S().dohEnabled);
    s.setValue(QStringLiteral("proxy/enableEch"), S().echEnabled);
    s.setValue(QStringLiteral("proxy/http3On"), S().http3Enabled);
    s.setValue(QStringLiteral("proxy/http3"), S().http3Enabled);   // 旧版本回读兼容
    s.setValue(QStringLiteral("proxy/echConfig"), S().echConfig);
    s.setValue(QStringLiteral("proxy/speedThreads"), S().speedThreads);

    QStringList rows;
    for (const NodeResult &r : S().results) {
        rows << QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8|%9")
                    .arg(r.ip)
                    .arg(r.delayMs)
                    .arg(r.downloadMs)
                    .arg(r.failed ? 1 : 0)
                    .arg(r.downloadFailed ? 1 : 0)
                    .arg(int(r.kind))
                    .arg(r.ordinal)
                    .arg(Enc(r.country), Enc(r.asn));
    }
    s.setValue(QStringLiteral("proxy/results"), rows);

    QStringList lineRows;
    for (const LineTestResult &r : S().lineResults) {
        lineRows << QStringLiteral("%1|%2|%3|%4|%5|%6|%7|%8")
                        .arg(r.index)
                        .arg(r.pingOk ? 1 : 0)
                        .arg(r.delayMs)
                        .arg(Enc(r.pingText))
                        .arg(r.speedOk ? 1 : 0)
                        .arg(r.bytesPerSec, 0, 'f', 2)
                        .arg(Enc(r.speedText), Enc(r.error));
    }
    s.setValue(QStringLiteral("proxy/lineResults"), lineRows);
    s.sync();
    if (s.status() == QSettings::NoError)
        DataMigrator::RollingBackup(SettingsPath());   // 写成功才滚动备份
    else
        qWarning("proxy-config: 写入失败 status=%d", int(s.status()));
}

}  // namespace ProxyConfig
