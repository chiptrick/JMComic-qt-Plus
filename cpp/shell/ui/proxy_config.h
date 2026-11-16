// 分流线路 / IP 节点配置：与 Python 版 GlobalConfig + Setting + LoginProxyNewWidget 同义。
//
// Python 侧（逐条对齐，改动前请先读源码）：
//   * config/global_config.py
//       Url2List[4] / PicUrlList[4] / CdnApiUrl / CdnImgUrl / ProxyApiUrl / ProxyImgUrl
//       GetApiUrl2 / GetImgUrl2：1..4 取列表，5=CDN，6=US 反代，>=7 回落到列表第 1 项
//       IsCdnIndex(5) / IsProxyUrlIndex(6)
//       ProxyIpList(26) / BestCfIpList(24) / ProxyApiDomain2 / ProxyImgDomain2
//   * config/setting.py
//       ProxySelectIndex=1 / ProxyImgSelectIndex=1 / ProxyIpValue="" /
//       ProxyIpLastList="" / PreferCDNList="104.18.227.172,...(6 个)" /
//       IsHttpProxy=0 / ProxyIpCountry="" / LastProxyResult / LastIpResult
//   * server/req.py + server/server.py
//       线路 6：URL host 改成 "<反代域名>/<原 host>"（proxyUrl），且 proxyUrl 存在时
//               ipList 强制为空 —— 反代优先于 IP 覆盖。
//       线路 5：isApi/isImg/isRegister 且无系统代理时，才把 ProxyIpValue 作为 RESOLVE。
//   * view/user/login_proxy_new_widget.py
//       ShowAllItem 的节点顺序：ProxyIP(ProxyIpLastList 或内置 ProxyIpList) →
//       自定义(PreferCDNList) → 优选(BestCfIpList)；前两类去重，第三类不去重。
//       IpItem.__lt__ / MatchFilter / CheckSelectBox / SaveHistory。
//
// C++ 侧键名统一放在 QSettings（AppConfigLocation/ui.ini）的 proxy/ 分组下，
// JM_UI_CONFIG 可覆盖目录（测试隔离）。外部接口（Lines/LineAt/ApiLine/... ）保持兼容。
#pragma once

#include <QUrl>
#include <QString>
#include <QStringList>
#include <QVector>

// 三类 IP 分流节点（对应 Python TagKind 的 "proxy" / "self" / "cf"）
enum class NodeKind {
    ProxyIp = 0,    // ProxyIP分流N：ProxyIpLastList / 内置 ProxyIpList
    Custom = 1,     // 自定义分流N：PreferCDNList
    Preferred = 2,  // CDN分流N（UI 简称「优选」）：BestCfIpList
};

enum class NodeFilter {
    All = 0,
    ProxyGroup = 1,     // Python filter="proxy"：ProxyIP + 自定义
    PreferredGroup = 2, // Python filter="cf"：优选
};

enum class NodeSortMode {
    Smart = 0,      // Python IpItem.__lt__：选中优先 → 非失败 → 0.8*延迟+0.2*下载
    Latency = 1,    // 按延迟（失败沉底，选中不置顶）
    Download = 2,   // 按下载（失败沉底，选中不置顶）
};

// Python Setting.IsHttpProxy：0=无 1=Http 2=Sock5 3=系统
enum class HttpProxyMode { None = 0, Http = 1, Sock5 = 2, System = 3 };

struct ProxyLine
{
    int index = 1;          // 1..6，与 Python ProxySelectIndex 同编号
    QString name;           // 分流1..分流4 / IP分流 / US反代分流
    QString apiBase;        // https://host
    QString imgBase;        // https://host
    bool cdn = false;       // index == 5（Python IsCdnIndex）
    bool proxy = false;     // index == 6（Python IsProxyUrlIndex）
};

// 一次节点测速的结果（字段与 Python IpItem 一一对应）
struct NodeResult
{
    QString ip;
    NodeKind kind = NodeKind::Preferred;
    int ordinal = 0;        // 组内序号 j（1 起），Python IpItem.j
    QString name;           // ProxyIP分流3 / 自定义分流2 / CDN分流5
    int delayMs = 0;        // SpeedTestPingReq：/latest 耗时 // 4
    int downloadMs = 0;     // SpeedTestPing2Req：固定封面耗时 // 4
    bool failed = false;    // ping 失败
    bool downloadFailed = false;
    QString error;
    QString country;
    QString asn;

    // Python IpItem.__lt__ 的综合分
    int Score() const { return int(delayMs * 0.8 + downloadMs * 0.2); }
    bool IsFail() const { return failed || downloadFailed; }
    QString ShortName() const;   // Python ShortName：ProxyIP 3 / 自定义 2 / 优选 5
};

// 线路测速结果（Python 存 LastProxyResult，这里存成结构化值便于比较与排序）
struct LineTestResult
{
    int index = 0;
    bool pingOk = false;
    int delayMs = 0;          // 稳定口径：多次采样取最小耗时 // 4
    QString pingText;
    bool speedOk = false;
    double bytesPerSec = 0;   // 稳定口径：多次采样取最大吞吐
    QString speedText;
    QString error;
};

// 一次请求的最终路由（把 Python req.py 的 proxyUrl / ipList 优先级集中到一处）
struct RoutePlan
{
    QUrl url;                 // 线路 6 时已改写为 https://<反代域名>/<原 host>/...
    QString connectIp;        // 非空 = IP 覆盖（节点或 DoH 解析结果，走 NodeHttp）
    QString sniHost;          // IP 覆盖时的 TLS SNI / 证书校验名
    bool viaNode = false;
    bool viaDoh = false;      // 这个 IP 来自 DoH 解析而非用户选中的节点
    int line = 1;
    QString reason;           // 诊断：为什么走/不走 IP 覆盖
};

namespace ProxyConfig {

// ---------------- 线路 ----------------
QVector<ProxyLine> Lines();              // 索引顺序 1..6（外部接口不变）
ProxyLine LineAt(int index);             // 越界/非法 → 回落线路 1
ProxyLine CdnLine();
bool IsCdnIndex(int index);
bool IsProxyIndex(int index);
QVector<int> DisplayOrder();             // Python UI 顺序：1,2,3,4,6,5
QString LineLabel(int index);            // 分流1..分流4 / IP分流 / US反代分流
QString GetApiUrl2(int index);           // Python GlobalConfig.GetApiUrl2
QString GetImgUrl2(int index);
QString ProxyApiDomain2();               // jm2-api.jpacg.cc
QString ProxyImgDomain2();               // jm2-img.jpacg.cc

int ApiLine();                           // 1..6，默认 1
void SetApiLine(int index);
int ImgLine();                           // 1..6，默认 1
void SetImgLine(int index);

// ---------------- 选中节点 ----------------
QString SelectedIp();
void SetSelectedIp(const QString &ip);

QStringList Nodes();                     // 历史节点列表（兼容旧接口）
void SetNodes(const QStringList &ips);
QStringList DefaultNodes();              // = ProxyIpList（26 个内置 ProxyIP）
QVector<NodeResult> Results();
void SetResults(const QVector<NodeResult> &results);

// ---------------- 三类节点的配置 / 分类 / 顺序 / 匹配 ----------------
QStringList ProxyIpList();               // 内置 26 个（Python GlobalConfig.ProxyIpList）
QStringList BestCfIpList();              // 内置 24 个（Python GlobalConfig.BestCfIpList）
QString CustomIpText();                  // Python Setting.PreferCDNList 原文
QStringList CustomIpList();              // 按 [、，；;,\s]\s* 切分
void SetCustomIpText(const QString &text);
QStringList ProxyIpLastList();           // Python Setting.ProxyIpLastList（获取到的节点）
void SetProxyIpLastList(const QStringList &ips);
QString RegionCode();                    // Python Setting.ProxyIpCountry
void SetRegionCode(const QString &code);
// Python RegionNames：已获取节点里出现过的地区（country 形如「美国·Los Angeles」，
// 取 · 前的国家名），去重后字典序排序 —— 「自定义ProxyIP地区」下拉的候选来源。
QStringList RegionNames(const QVector<NodeResult> &nodes);
HttpProxyMode Mode();                    // Python Setting.IsHttpProxy
void SetMode(int mode);

// Python ShowAllItem 的完全相同顺序与去重规则
QVector<NodeResult> AllNodes();
NodeKind KindOf(const QString &name);    // Python TagKind：按名字前缀判定
bool MatchFilter(NodeKind kind, NodeFilter filter);
QString NodeName(NodeKind kind, int ordinal);
void SortResults(QVector<NodeResult> &results, const QString &selectedIp,
                 NodeSortMode mode = NodeSortMode::Smart);
QString PickBest(const QVector<NodeResult> &results, const QString &current);

// ---------------- 节点测速并发度（0.91 新增） ----------------
// proxy/speedThreads，读写入 ui.ini 的 proxy/ 分组；取值恒被夹在 [1,16]。
// 语义（刻意让「1」不等于「1 个工作线程」，以保证升级后行为逐字节不变）：
//   1     = 兼容档（默认）：沿用 NodeTester 的单线程事件循环 + 8 个在飞请求，
//           不创建任何工作线程，是 0.90 及以前的全部行为；
//   2..16 = 新增工作线程池：每个线程一次只跑 1 个阻塞探测，最多 16 个线程
//           同时测速；线程数即真实 OS 线程数，不是"在飞请求数"。
// ClampSpeedThreads 是纯函数（不读盘、不改状态），供 UI 与离线自检共用。
constexpr int kMaxSpeedThreads = 16;
int ClampSpeedThreads(int n);
int SpeedThreads();
void SetSpeedThreads(int n);

// ---------------- 线路测速结果与线路推荐 ----------------
QVector<LineTestResult> LineResults();
void SetLineResults(const QVector<LineTestResult> &results);
// 结合测速结果与 IP 配置推荐线路：无有效结果 / IP 分流不可用 → 默认 1
int RecommendLine(bool api, const QVector<LineTestResult> &results);

// ---------------- DOH / ECH / HTTP3（Python ProxySetting 三项） ----------------
// Python：DohAddress 默认 https://parse.jpacg.cc/parse；EnableEch 默认 1；
// IsOpenHTTP3 默认 0。**C++ 壳 0.90 起刻意分歧：HTTP3 默认开**（见 §14.7/§28）。
// C++ 额外给 DOH 一个显式「作用于流量」开关（默认关），
// 因为 Python 的 CurlOpt.DOH_URL 那段是注释掉的，默认不会接管每个请求的解析。
QString DohAddress();
void SetDohAddress(const QString &url);
bool DohEnabled();               // DoH 解析结果是否用于真实请求（默认 false）
void SetDohEnabled(bool on);
bool EchEnabled();
void SetEchEnabled(bool on);
bool Http3Enabled();
void SetHttp3Enabled(bool on);

// H3 失败注入（仅验收/回归，不写回配置）：bad-port / drop-first / 空=关闭。
// 0.89 时"H3 失败 → HTTP/1.1 回退"只能靠 DoH/节点链路恰好出错来触发，
// 不可复现；这里把 H3 指向一个没有 QUIC 服务的端口，走的是与真实失败
// 完全相同的代码路径（node_http 的 r.ok==false 分支）。
QString H3FailInject();
void SetH3FailInject(const QString &mode);
// 取一次判定并消耗（drop-first 只在首次返回 true）。
bool TakeH3FailInject();
QString EchConfig();             // DoH HTTPS RR 取到的 ECHConfigList(base64)
void SetEchConfig(const QString &cfg);

// Qt 6.12 的 TLS/HTTP 后端没有 ECH 与 HTTP/3 接口（见 doh_client.h 说明）。
// 与 Python IsAdvancedTlsSupported() 同义：返回 false 时降级重试没有意义。
bool AdvancedTlsSupported();
bool EchOff();                   // Http3EchFallback.echOff
bool Http3Off();                 // Http3EchFallback.http3Off
void DowngradeAdvanced(bool echFail);
void ResetAdvancedFallback();    // 页面保存时调用（Python ResetHttp3EchFallback）
QString AdvancedStatus();        // 一行状态，页面与自检共用

// ---------------- Q4（0.90）：ECH-over-QUIC 被服务端策略拒绝 ----------------
// 决策：隐藏 SNI 优先。一旦 (host, 节点 IP) 上的 H3+ECH 组合被服务端拒绝，
// 本进程内不再对该组合发 H3，直接走 TCP+ECH；下次冷启动或换节点再重试。
// 状态**只存内存**：不写 ini、不写 alt-svc.json，冷启动天然复位。
// 节点标识：优先显式 IP 覆盖，其次 DoH 解析出的首个 IP，都没有则用空串
// （由 EchH3Blocked 归一成 "direct"）。NodeHttp 与 NetClient 必须共用这一份。
QString EchH3NodeKey(const QString &host, const QString &connectIp);
bool EchH3Blocked(const QString &host, const QString &nodeIp);
void NoteEchH3Rejected(const QString &host, const QString &nodeIp);
void ResetEchH3Blocks();         // 换节点 / 页面保存时调用
int EchH3BlockCount();           // --advanced-status / 自检可见

// ---------------- 回退/降级的计数（Q5） ----------------
// 只做进程内累计，不落盘：目的是让「本次会话到底降级了几次」在页面与自检里
// 看到同一个数字，而不是翻日志。计数点都在主线程（QUIC 回调、NodeHttp 回调），
// 实现上用原子量兜底，避免将来把请求挪到工作线程时静默出错。
struct AdvancedCounters
{
    int h3Ok = 0;        // H3 请求成功
    int h3Fail = 0;      // H3 尝试失败（含握手/读取/超时）
    int h3Fallback = 0;  // H3 失败后回退到 TLS/HTTP1.1
    int echOk = 0;       // ECH 握手 SUCCESS
    int echFail = 0;     // ECH 握手非 SUCCESS（FAILED/GREASE/降级）
};

void NoteH3(bool ok);
void NoteH3Fallback();
void NoteEch(bool success);
AdvancedCounters Counters();
QString CountersLine();          // "H3 成功 3 / 失败 1 / 回退 1 · ECH SUCCESS 5 / FAILED 0"
void ResetCounters();            // 自检/回归用来取基线
void SyncDohClient();            // 把当前配置推给 DohClient（幂等）

// ---------------- 请求路由（优先级/回退的唯一入口） ----------------
// 优先级：线路 6 反代 > 系统/用户代理禁用 IP 覆盖 > 线路 5 + 有效节点 >
//         DoH 解析 IP（开启时）> 域名直连
RoutePlan ResolveRoute(const QUrl &url, bool isApi);
bool NodeUsable(const QString &ip);      // 选中节点是否在结果里 ping 通过

void Load();
void Save();

}  // namespace ProxyConfig
