// 节点测速的工作线程池（0.91 新增）。
//
// 为什么单独一层，而不是把 NodeTester 的 PumpNodes 并发度从 8 改到 16：
//   * 现有实现是「单线程事件循环 + N 个在飞异步请求」，所有状态（nodes_、
//     三个队列、三个 inflight 集合）都只被主线程触摸。把数字放大到 16 只是
//     放大在飞请求数，并没有多任何一个线程，也无法约束"同一时刻真正占用
//     16 条独立连接/计时器"的资源上限。
//   * 这里提供的是真实 OS 工作线程：每个线程内用阻塞 socket 独占一次探测，
//     线程数 == 同时在测的节点数。NodeHttp / QNetworkAccessManager 属于主线程
//     网络栈（QUIC、DoH、ECH 缓存、全局计数器），不能跨线程复用；工作线程
//     只依赖 QAbstractSocket 的阻塞 API，不碰任何进程级网络状态。
//
// 五条设计口径（对应需求）：
//   1. 线程数：ClampThreads() 统一夹到 [1,16]，配置/UI/CLI 三条入口都过它。
//      真实 worker 数 = min(ClampThreads(n), 任务数)，空批次不建线程。
//   2. 分配与调度：动态领取（work-stealing 的队列版）。共享游标 next 由
//      QMutex 保护，先完成的线程继续领下一个，慢节点不会把整批拖成"木桶"。
//      不做静态取模分片，是因为节点耗时方差极大（3s 超时 vs 30ms 命中）。
//   3. 共享数据：BatchState 里的 next / outcomeSlots / 计数全部在 QMutex 下读写；
//      cancel / active / peak / finished / liveWorkers 是原子量；
//      outcomeSlots[pos] 与 tasks[pos] 一一对应，一个下标至多被一个线程写一次。
//      界面侧 nodes_ 只由主线程回调改写，工作线程绝不持有 UI 对象。
//   4. 汇总：Report.outcomes 与输入 tasks **同序等长**，未领取的任务也补一条
//      失败记录；调用方按 id 回写即可，与完成先后无关。最终状态由 Report
//      再对账一次，消除"逐条回调乱序"带来的差异。
//   5. 异常/超时：每个探测有独立硬截止（QElapsedTimer + 分片的阻塞等待），
//      超时产出 ok=false 而不是伪造一个耗时数字；探测抛异常在 worker 内被
//      catch 转成失败记录，绝不让异常穿过 QRunnable::run（会 terminate）；
//      另有主线程 watchdog 兜住整批卡死，取消时置原子标志，worker 在
//      100ms 分片边界退出，迟到的回调按 generation 丢弃。
//
// 传输层的有意取舍：并发档只测 TCP/TLS（HTTP/1.1）；兼容档的主线程 NodeHttp
// 还会先试 H3/QUIC + ECH。后者的 EchH3Blocks / Alt-Svc / DoH 缓存是无锁的
// 进程级状态，搬进 16 个线程会数据竞争。因此并发档用更宽的探测截止
// （NodeTester::kPooledTimeoutMs）抵消 TCP 首包更慢的偏差；延迟口径不变，
// 仍是耗时 // 4，且所有并发任务走同一套传输，结果彼此可比。
// JM_BENCH_TRACE=1 可打印连接/TLS/读取/响应头的分步诊断。
#pragma once

#include <QAtomicInteger>
#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QVector>

#include <functional>
#include <memory>

namespace NodeBench {

// 硬上限：三条配置入口都经过 ClampThreads，不得在各处另写 qBound。
constexpr int kMaxThreads = 16;
int ClampThreads(int n);

// 进程级运行时覆盖（只给 CLI / 回归用）：0 = 未覆盖。它不写 ui.ini，也不会被
// ProxyConfig::Save() 顺带持久化，避免一次回归把用户配置改成 16 线程。
void SetThreadOverride(int n);
int ThreadOverride();

// 一个探测任务。id 由调用方给（NodeTester 传 nodes_ 下标），汇总时按它回写。
struct Task {
    int id = -1;
    QUrl url;
    QString connectIp;    // 非空 = 连接落该 IP，TLS SNI/校验名仍是 sniHost
    QString sniHost;      // 空 = 取 url.host()
    int timeoutMs = 3000;
    qint64 maxBytes = 1024 * 1024;   // 正文上限，防止 16 线程各吃 4MB
};

// 单条结论。ok=false 时 elapsedMs 只用于诊断，不得拿来算延迟/吞吐。
struct Outcome {
    int id = -1;
    int worker = -1;
    bool ok = false;
    bool timedOut = false;
    bool cancelled = false;
    qint64 elapsedMs = 0;
    qint64 bytes = 0;
    QByteArray body;
    QString error;
};

// 整批汇总。outcomes.size() == tasks 数，outcomes[i].id == tasks[i].id。
struct Report {
    QVector<Outcome> outcomes;
    int threads = 0;
    int tasks = 0;
    int executed = 0;      // 真正跑过探测的任务数（取消后 < tasks）
    int failed = 0;
    int timeouts = 0;
    int peak = 0;          // 实测最大同时在跑的线程数，自检据此验证并发度
    qint64 wallMs = 0;
    bool cancelledBatch = false;
    bool watchdog = false;
};

// 探测函数签名。cancel 是整批共享的取消标志；实现必须在阻塞等待里分片检查它。
using Probe = std::function<Outcome(const Task &, int worker,
                                    const QAtomicInteger<int> &cancel)>;

// 生产用探测：阻塞 HTTPS/HTTP1.1 GET，等价于 NodeHttp 的 TCP 分支
// （connectToHostEncrypted(IP, port, 域名 SNI)，Connection: close）。
// 只在工作线程里调用；不访问 ProxyConfig / DohClient / QUIC 等主线程状态。
Outcome ProbeBlocking(const Task &t, int worker, const QAtomicInteger<int> &cancel);

struct BatchState;   // 调度核心的共享状态，实现见 .cpp

// 同步版调度核心：给离线自检用（可注入假探测，不打网络）。返回时所有
// worker 已退出。生产路径走下面的 Pool（异步、回调回主线程）。
Report RunBatchSync(const QVector<Task> &tasks, int threads, const Probe &probe,
                    QAtomicInteger<int> *cancel = nullptr,
                    const std::function<void(const Outcome &)> &onEach = {});

// 异步封装：Start/Cancel/Busy 都在主线程调用；Each/Done 也保证在主线程触发。
// 不是 QObject：回调靠 QMetaObject::invokeMethod(上下文对象, 队列连接) 回投，
// 因此不需要 moc，也不需要给自定义结构体注册 metatype。
class Pool
{
public:
    using Each = std::function<void(const Outcome &)>;
    using Done = std::function<void(const Report &)>;

    // uiContext 必须是主线程对象（通常就是 NodeTester 自己）。它的生命周期
    // 覆盖 Pool；对象销毁后 Qt 不会再投递队列回调，天然避免悬空 this。
    explicit Pool(QObject *uiContext);
    ~Pool();

    bool Start(QVector<Task> tasks, int threads, Each each, Done done);
    void Cancel();
    bool Busy() const { return running_; }
    int Generation() const { return generation_; }

private:
    void CleanupWorkers();

    // 裸指针即可：Pool 是宿主（NodeTester）的成员，宿主析构必然先走 Pool 的
    // 析构（取消 + 等 worker 退出），因此不存在 worker 投递时宿主已销毁的窗口。
    QObject *ctx_ = nullptr;
    QThreadPool pool_;                       // 私有池：不干扰全局 QThreadPool
    std::shared_ptr<BatchState> state_;
    int generation_ = 0;
    bool running_ = false;
};

// 离线确定性自检（不打网络）：线程数夹紧、任务唯一性、并发度上限、
// 汇总顺序、异常/超时/取消兜底。返回 true=全部通过；out 收到多行报告。
bool SelfTest(QString *out);

}  // namespace NodeBench
