// 分流线路与 IP 节点的测速器：逐条对齐 Python 版行为，并让结果稳定可比。
//
// Python 侧（login_proxy_new_widget.py / server/req.py / server/user_handler.py）：
//   * 线路测速 SpeedTest：1..6 循环，跳过 cdnIndex=5；
//     ping = <api>/latest?page=0 (timeout 3s)，指标 = 耗时毫秒 // 4；
//     speed = <img>/media/photos/292840/00002.webp，指标 = 字节数 / 耗时；
//     结果写进 LastProxyResult（重进页面可恢复），失败显示状态文案而不是数字。
//   * 节点测速 StartTestIp：先从 check.jpacg.cc 取 ProxyIP 列表（失败清空并回落内置
//     ProxyIpList），ShowAllItem 按三类重建，再对全部节点 ping、对非失败项测图片；
//     同时按 `GetIpInfoReq(item.ip)` 补 country/asn（缺才查，结果写 LastIpResult）；
//     全部回来后 Sort + CheckSelectBox + 存 LastIpResult。
//
// C++ 侧差异（有意为之，均为"稳定、可比较"服务）：
//   * 线路 5（IP分流）也纳入测速：有选中节点就走节点，没有则给出确定性状态"未选节点"；
//     Python 跳过它是因为它由节点列表单独测，这里补齐后 6 条线路都有可比较的结果。
//   * 每条线路 ping 取 2 次采样的最小耗时、speed 取 2 次采样的最大吞吐：
//     单次 3s 采样抖动很大，多样本后可横向比较；指标口径仍是 //4 与字节/秒。
//   * 节点探测走 NodeHttp（QSslSocket 连 IP + 域名 SNI），等价 Python curl --resolve。
#pragma once

#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <functional>

#include "proxy_config.h"

class QNetworkAccessManager;
class QNetworkReply;
class NodeHttp;
struct NodeHttpResponse;

class NodeTester : public QObject
{
    Q_OBJECT
public:
    explicit NodeTester(QObject *parent = nullptr);

    void TestLines();   // 6 条线路：全部 ping（并发）→ 全部 speed（串行）
    void TestNodes();   // 三类节点：取 ProxyIP 列表 → ping → 图片 → 排序 → 自动选中
    void Cancel();
    bool Busy() const { return busy_; }

    // Python ToolUtil.GetDownloadSize 的档位：bytes / kb / mb
    static QString FormatSpeed(double bytesPerSec);
    static QString FormatDelay(int ms);
    // Python 的失败列显示 Str.GetStr(st)（"超时"/"连接失败" 等短状态），
    // 而不是 Qt 的英文长串；完整错误留给 tooltip。
    static QString FailText(const QString &error, bool forDownload = false);

signals:
    // 以下三个保持旧接口（聚合后的最优采样值）
    void LinePing(int index, bool ok, int ms, const QString &error);
    void LineSpeed(int index, bool ok, const QString &text);
    void LinesFinished();
    // 新增：结构化结果 / 节点列表就绪（页面据此立刻渲染三类节点）
    void LineResult(int index, const LineTestResult &result);
    void LineResultsReady(const QVector<LineTestResult> &results);
    void NodeListReady(const QVector<NodeResult> &nodes);

    void NodeUpdated(const NodeResult &result);
    void NodesFinished(const QVector<NodeResult> &sorted, const QString &selected);
    void Log(const QString &line);

private:
    struct Probe {
        QUrl url;
        QString connectIp;    // 非空 = 节点 IP 覆盖（NodeHttp，SNI/Host 取 url.host()）
        int timeoutMs = 3000;
    };
    using Done = std::function<void(bool ok, qint64 elapsedMs, const QByteArray &body,
                                    const QString &err)>;

    void Get(const Probe &p, Done done);
    void FetchProxyIpList();
    void BuildNodes();
    Probe MakeLineProbe(int index, bool api) const;
    void StartPingPhase();
    void StartSpeedPhase();
    void SpeedLine(int index, int sample);
    void FinishLines();

    void PingNode(int index);
    void SpeedNode(int index);
    void FetchIpInfo(int index, int attempt = 0);
    void PumpNodes();
    void FinishNodes();

    QNetworkAccessManager *nam_ = nullptr;
    NodeHttp *nodeHttp_ = nullptr;
    bool busy_ = false;
    int generation_ = 0;

    // 线路测速：结果按 index-1 索引，固定 6 条
    QVector<LineTestResult> lineResults_;
    QVector<QVector<qint64>> pingSamples_;    // 每线路多次 ping 的耗时
    QVector<QVector<double>> speedSamples_;   // 每线路多次下载的吞吐
    int linePingPending_ = 0;
    int speedLineIndex_ = 0;

    // 节点测速
    QVector<NodeResult> nodes_;
    QQueue<int> nodePingQueue_;
    QQueue<int> nodeSpeedQueue_;
    QQueue<int> nodeInfoQueue_;
    QSet<int> nodePingInflight_;
    QSet<int> nodeSpeedInflight_;
    QSet<int> nodeInfoInflight_;
    // 0=idle 1=fetch 2=ping 3=speed 4=ipinfo（补地区，Python 与 ping 并发发，这里
    // 单独收尾：地区只用于下拉候选，晚一点到不影响测速结果与排序）
    int nodePhase_ = 0;
};
