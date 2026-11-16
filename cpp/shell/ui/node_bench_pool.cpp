#include "node_bench_pool.h"

#include <QAbstractSocket>
#include <QElapsedTimer>
#include <QMetaObject>
#include <QMutexLocker>
#include <QRunnable>
#include <QSet>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <utility>

namespace NodeBench {

namespace {

// 阻塞等待的分片长度：既是取消的响应上限，也让"还剩多少毫秒"能被反复重算。
// 不用一次性 waitForReadyRead(timeout)，否则 Cancel() 要等满 3s 才生效。
constexpr int kSliceMs = 100;
QAtomicInteger<int> gThreadOverride{0};

void Nap(int ms)
{
    if (ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// 与 node_http.cpp 的 BuildHead 保持同一组请求头：测速值必须能横向比较。
QByteArray BuildHead(const Task &t, const QString &sni)
{
    QString target = t.url.path(QUrl::FullyEncoded);
    if (target.isEmpty())
        target = QStringLiteral("/");
    if (t.url.hasQuery())
        target += QLatin1Char('?') + t.url.query(QUrl::FullyEncoded);

    QByteArray head;
    head += QByteArrayLiteral("GET ");
    head += target.toLatin1();
    head += QByteArrayLiteral(" HTTP/1.1\r\nHost: ");
    head += sni.toLatin1();
    head += QByteArrayLiteral("\r\nUser-Agent: JMComic/0.88 (Qt ");
    head += QByteArray(qVersion());
    head += QByteArrayLiteral(")\r\nCache-Control: no-cache\r\nPragma: no-cache\r\n"
                              "Expires: 0\r\nAccept-Encoding: identity\r\n"
                              "Connection: close\r\n\r\n");
    return head;
}

struct Headers {
    int status = 0;
    qint64 contentLength = -1;
    bool chunked = false;
};

// HTTP/1.1 chunked 解码：CDN 的 /latest 与封面图都走 chunked，直接把
// raw 当正文会把块长度行也算进字节数，吞吐/成功判定都会失真。
QByteArray Dechunk(const QByteArray &src, bool *complete)
{
    QByteArray out;
    *complete = false;
    int pos = 0;
    while (pos < src.size()) {
        const int eol = src.indexOf('\n', pos);
        if (eol < 0)
            return out;
        QByteArray sizeLine = src.mid(pos, eol - pos).trimmed();
        const int semi = sizeLine.indexOf(';');
        if (semi >= 0)
            sizeLine.truncate(semi);
        bool ok = false;
        const int n = sizeLine.toInt(&ok, 16);
        if (!ok)
            return out;
        pos = eol + 1;
        if (n == 0) {
            *complete = true;   // 0\r\n\r\n 终止块
            return out;
        }
        if (pos + n > src.size())
            return out;         // 这一块还没收齐
        out += src.mid(pos, n);
        pos += n + 2;           // 跳过分块尾的 \r\n
    }
    return out;
}

Headers ParseHeaders(const QByteArray &raw, int bodyStart)
{
    Headers h;
    const QList<QByteArray> lines = raw.left(bodyStart).split('\n');
    if (!lines.isEmpty()) {
        const QList<QByteArray> first = lines.first().simplified().split(' ');
        if (first.size() >= 2)
            h.status = first.at(1).toInt();
    }
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const int colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QByteArray key = line.left(colon).toLower();
        const QByteArray val = line.mid(colon + 1).trimmed();
        if (key == "content-length")
            h.contentLength = val.toLongLong();
        else if (key == "transfer-encoding" && val.toLower().contains("chunked"))
            h.chunked = true;
    }
    return h;
}

}  // namespace

int ClampThreads(int n)
{
    return qBound(1, n, kMaxThreads);
}

void SetThreadOverride(int n)
{
    gThreadOverride.storeRelease(n <= 0 ? 0 : ClampThreads(n));
}

int ThreadOverride()
{
    return gThreadOverride.loadAcquire();
}

// ---------------------------------------------------------------- 阻塞探测
Outcome ProbeBlocking(const Task &t, int worker, const QAtomicInteger<int> &cancel)
{
    Outcome out;
    out.id = t.id;
    out.worker = worker;

    QElapsedTimer timer;
    timer.start();

    auto fail = [&](const QString &err, bool timedOut = false) {
        out.ok = false;
        out.error = err;
        out.timedOut = timedOut;
        out.elapsedMs = timer.elapsed();
        out.bytes = 0;
        out.body.clear();
        return out;
    };
    auto remaining = [&]() { return qMax(0, t.timeoutMs - int(timer.elapsed())); };
    auto cancelled = [&]() { return cancel.loadAcquire() != 0; };

    try {
        if (cancelled()) {
            out.cancelled = true;
            return fail(QStringLiteral("cancelled"));
        }
        const QString host = t.url.host();
        const QString sni = t.sniHost.isEmpty() ? host : t.sniHost;
        const QString connectHost = t.connectIp.isEmpty() ? host : t.connectIp;
        if (host.isEmpty() || sni.isEmpty() || connectHost.isEmpty())
            return fail(QStringLiteral("empty host"));

        const bool tls = t.url.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
        const quint16 port = quint16(t.url.port(tls ? 443 : 80));

        // QSslSocket 全部是工作线程的栈对象：没有 parent、没有跨线程信号连接、
        // 也不碰 DohClient / QUIC / ProxyConfig 这些主线程状态。默认 SSL 配置是
        // 隐式共享的全局量，这里加锁做一次拷贝（C++11 静态局部初始化本身线程安全）。
        QSslSocket ssl;
        QTcpSocket tcp;
        QAbstractSocket *sock = tls ? static_cast<QAbstractSocket *>(&ssl)
                                    : static_cast<QAbstractSocket *>(&tcp);
        if (tls) {
            static QMutex cfgMutex;
            QSslConfiguration cfg;
            {
                QMutexLocker lock(&cfgMutex);
                cfg = QSslConfiguration::defaultConfiguration();
            }
            cfg.setPeerVerifyMode(QSslSocket::VerifyPeer);
            ssl.setSslConfiguration(cfg);
        }

        // 关键：TCP 连 IP，TLS 的 SNI 与证书校验名用域名（等价 curl --resolve，
        // 与 node_http.cpp 的 connectToHostEncrypted(connectHost, port, sni) 一致）。
        if (tls)
            ssl.connectToHostEncrypted(connectHost, port, sni);
        else
            tcp.connectToHost(connectHost, port);

        // 分片等待时，"这一片没等到"(SocketTimeoutError) 不等于"连接失败"：
        // 慢 DNS / 慢握手要一直重试到总截止，明确的 SocketError 才立刻放弃。
        // 分片同时给 Cancel() 留 100ms 级别的退出窗口。
        const bool trace = qEnvironmentVariableIsSet("JM_BENCH_TRACE");
        const bool connected = [&]() {
            while (remaining() > 0 && !cancelled()) {
                const bool step = sock->waitForConnected(qMin(kSliceMs, qMax(1, remaining())));
                if (trace)
                    qWarning("node-bench trace: connect step=%d state=%d err=%d(%s) left=%d",
                             int(step), int(sock->state()), int(sock->error()),
                             qPrintable(sock->errorString()), remaining());
                if (step)
                    return true;
                if (sock->error() != QAbstractSocket::SocketTimeoutError)
                    return false;
            }
            return false;
        }();
        if (!connected) {
            if (cancelled()) {
                out.cancelled = true;
                sock->abort();
                return fail(QStringLiteral("cancelled"));
            }
            if (remaining() <= 0) {
                sock->abort();
                return fail(QStringLiteral("timed out after %1ms").arg(t.timeoutMs), true);
            }
            return fail(sock->errorString().isEmpty() ? QStringLiteral("connection failed")
                                                      : sock->errorString());
        }
        if (tls) {
            bool encrypted = false;
            while (!encrypted && remaining() > 0 && !cancelled()) {
                const bool step = ssl.waitForEncrypted(qMin(kSliceMs, qMax(1, remaining())));
                if (trace)
                    qWarning("node-bench trace: tls step=%d encrypted=%d state=%d err=%d(%s) left=%d",
                             int(step), int(ssl.isEncrypted()), int(sock->state()),
                             int(sock->error()), qPrintable(sock->errorString()), remaining());
                if (step)
                    encrypted = true;
                else if (sock->error() != QAbstractSocket::SocketTimeoutError)
                    break;
            }
            if (!encrypted) {
                if (cancelled()) {
                    out.cancelled = true;
                    sock->abort();
                    return fail(QStringLiteral("cancelled"));
                }
                if (remaining() <= 0)
                    return fail(QStringLiteral("timed out after %1ms").arg(t.timeoutMs), true);
                return fail(sock->errorString().isEmpty() ? QStringLiteral("TLS handshake failed")
                                                          : sock->errorString());
            }
        }

        const QByteArray head = BuildHead(t, sni);
        if (sock->write(head) != head.size()
            || !sock->waitForBytesWritten(qMin(kSliceMs, qMax(1, remaining()))))
            return fail(sock->errorString().isEmpty() ? QStringLiteral("write failed")
                                                      : sock->errorString());

        QByteArray raw;
        QByteArray decoded;
        int headerEnd = -1;
        Headers ph;
        bool complete = false;
        int readIters = 0;
        while (remaining() > 0 && !cancelled()) {
            const int slice = qMin(kSliceMs, qMax(1, remaining()));
            const bool ready = sock->waitForReadyRead(slice);
            if (ready)
                raw += sock->readAll();
            if (trace && (readIters < 8 || readIters % 10 == 0))
                qWarning("node-bench trace: read i=%d ready=%d avail=%lld raw=%d state=%d err=%d left=%d",
                         readIters, int(ready), (long long)sock->bytesAvailable(), raw.size(),
                         int(sock->state()), int(sock->error()), remaining());
            ++readIters;
            if (raw.size() > t.maxBytes + 64 * 1024)
                return fail(QStringLiteral("body too large"));

            if (headerEnd < 0) {
                headerEnd = raw.indexOf(QByteArrayLiteral("\r\n\r\n"));
                if (headerEnd >= 0) {
                    ph = ParseHeaders(raw, headerEnd);
                    if (trace)
                        qWarning("node-bench trace: headers status=%d len=%lld chunked=%d raw=%d",
                                 ph.status, (long long)ph.contentLength, int(ph.chunked),
                                 raw.size());
                    if (ph.status >= 400)
                        return fail(QStringLiteral("http %1").arg(ph.status));
                    if (ph.status == 0)
                        return fail(QStringLiteral("bad response"));
                }
            }
            if (headerEnd >= 0) {
                const qint64 bodyLen = qint64(raw.size()) - headerEnd - 4;
                if (ph.contentLength >= 0) {
                    complete = bodyLen >= ph.contentLength;
                } else if (ph.chunked) {
                    bool chunkDone = false;
                    decoded = Dechunk(raw.mid(headerEnd + 4), &chunkDone);
                    complete = chunkDone;
                } else {
                    // 无长度、非 chunked：Connection: close 关闭时才算收全
                    complete = sock->state() == QAbstractSocket::UnconnectedState && bodyLen > 0;
                }
                if (complete)
                    break;
            }
            if (!ready && sock->state() == QAbstractSocket::UnconnectedState)
                break;   // Connection: close 的正常收尾
        }
        if (cancelled()) {
            out.cancelled = true;
            sock->abort();
            return fail(QStringLiteral("cancelled"));
        }
        if (headerEnd < 0) {
            if (remaining() <= 0)
                return fail(QStringLiteral("timed out after %1ms").arg(t.timeoutMs), true);
            return fail(QStringLiteral("closed before response"));
        }
        if (!complete) {
            if (remaining() <= 0)
                return fail(QStringLiteral("timed out after %1ms").arg(t.timeoutMs), true);
            return fail(QStringLiteral("incomplete body"));
        }

        QByteArray body = (ph.contentLength < 0 && ph.chunked)
                              ? decoded
                              : raw.mid(headerEnd + 4);
        if (ph.contentLength >= 0)
            body.truncate(int(qMin<qint64>(ph.contentLength, t.maxBytes)));
        if (body.size() > t.maxBytes)
            body.truncate(int(t.maxBytes));
        if (body.isEmpty())
            return fail(QStringLiteral("empty body"));

        out.ok = true;
        out.error.clear();
        out.elapsedMs = timer.elapsed();
        out.bytes = body.size();
        out.body = std::move(body);
        return out;
    } catch (const std::exception &e) {
        return fail(QStringLiteral("probe exception: %1").arg(QLatin1String(e.what())));
    } catch (...) {
        return fail(QStringLiteral("probe exception: unknown"));
    }
}

// ---------------------------------------------------------------- 调度核心
struct BatchState
{
    QVector<Task> tasks;
    Probe probe;
    int threads = 0;

    QMutex mutex;              // 保护 next / outcomeSlots / done
    int next = 0;
    QVector<Outcome> outcomeSlots;    // 与 tasks 同序；outcomeSlots[pos] 至多被写一次
    int done = 0;

    QAtomicInteger<int> ownCancel{0};
    QAtomicInteger<int> *cancel = &ownCancel;   // 同步自检可注入外部标志
    QAtomicInteger<int> active{0};
    QAtomicInteger<int> peak{0};
    QAtomicInteger<int> executed{0};
    QAtomicInteger<int> failed{0};
    QAtomicInteger<int> timeouts{0};
    QAtomicInteger<int> liveWorkers{0};
    QAtomicInteger<int> finished{0};

    QElapsedTimer wall;
    std::function<void(const Outcome &)> onEach;
    std::function<void(const Report &)> onDone;
};

namespace {

std::shared_ptr<BatchState> MakeState(QVector<Task> tasks, int threads, const Probe &probe,
                                      QAtomicInteger<int> *cancel)
{
    auto st = std::make_shared<BatchState>();
    st->tasks = std::move(tasks);
    st->probe = probe;
    // 空批次不建线程；任务少于线程数时也不为"凑数"起空转线程。
    st->threads = qMin(ClampThreads(threads), st->tasks.size());
    if (cancel)
        st->cancel = cancel;
    st->outcomeSlots.resize(st->tasks.size());
    for (Outcome &o : st->outcomeSlots)
        o.id = -1;   // -1 = 该下标尚未被领取，汇总时补一条失败记录
    return st;
}

Report MakeReport(const std::shared_ptr<BatchState> &st, bool watchdog)
{
    Report rep;
    {
        QMutexLocker lock(&st->mutex);
        rep.outcomes = st->outcomeSlots;
    }
    rep.threads = st->threads;
    rep.tasks = st->tasks.size();
    rep.executed = st->executed.loadAcquire();
    rep.failed = st->failed.loadAcquire();
    rep.timeouts = st->timeouts.loadAcquire();
    rep.peak = st->peak.loadAcquire();
    rep.wallMs = st->wall.isValid() ? st->wall.elapsed() : 0;
    rep.cancelledBatch = st->cancel->loadAcquire() != 0;
    rep.watchdog = watchdog;

    // 未领取 / 被取消 / 被 watchdog 掐断的任务也必须各占一格，且 id 与
    // tasks[i] 对齐。调用方据此回写，就不会出现"某个节点还留着上一轮数据"。
    for (int i = 0; i < rep.outcomes.size(); ++i) {
        Outcome &o = rep.outcomes[i];
        if (o.id != -1)
            continue;
        o = Outcome{};
        o.id = st->tasks.at(i).id;
        o.ok = false;
        o.cancelled = !watchdog;
        o.timedOut = watchdog;
        o.error = watchdog ? QStringLiteral("batch watchdog")
                           : QStringLiteral("cancelled before probe");
    }
    return rep;
}

void WorkerLoop(const std::shared_ptr<BatchState> &st, int workerId)
{
    const int cur = st->active.fetchAndAddOrdered(1) + 1;
    int p = st->peak.loadAcquire();
    while (cur > p && !st->peak.testAndSetOrdered(p, cur))
        p = st->peak.loadAcquire();

    for (;;) {
        int pos = -1;
        {
            // 动态领取：先做完的线程继续拿下一个任务。next 与 outcomeSlots 共用
            // 同一把锁，领取、写回、计数三件事没有中间态被别的线程看到。
            QMutexLocker lock(&st->mutex);
            if (st->cancel->loadAcquire() || st->next >= st->tasks.size())
                break;
            pos = st->next++;
        }
        const Task task = st->tasks.at(pos);   // QVector 常量读，不触发 detach
        Outcome out;
        out.id = task.id;
        out.worker = workerId;
        if (st->cancel->loadAcquire()) {
            out.cancelled = true;
            out.error = QStringLiteral("cancelled");
        } else {
            try {
                out = st->probe(task, workerId, *st->cancel);
                // id / worker 由调度器盖章，探测函数无权改写，避免串号。
                out.id = task.id;
                out.worker = workerId;
            } catch (const std::exception &e) {
                out = Outcome{};
                out.id = task.id;
                out.worker = workerId;
                out.error = QStringLiteral("worker exception: %1").arg(QLatin1String(e.what()));
            } catch (...) {
                out = Outcome{};
                out.id = task.id;
                out.worker = workerId;
                out.error = QStringLiteral("worker exception: unknown");
            }
        }
        {
            QMutexLocker lock(&st->mutex);
            st->outcomeSlots[pos] = out;
            ++st->done;
        }
        st->executed.fetchAndAddOrdered(1);
        if (!out.ok)
            st->failed.fetchAndAddOrdered(1);
        if (out.timedOut)
            st->timeouts.fetchAndAddOrdered(1);
        // 进度回调只带值；生产路径在这里做队列回投，界面不受线程影响。
        if (st->onEach)
            st->onEach(out);
    }

    st->active.fetchAndSubOrdered(1);
    // 最后一个退出的 worker 负责汇总；watchdog 可能已经先抢到 finished。
    if (st->liveWorkers.fetchAndSubOrdered(1) == 1
        && st->finished.testAndSetOrdered(0, 1)) {
        const Report rep = MakeReport(st, false);
        if (st->onDone)
            st->onDone(rep);
    }
}

class WorkerRunnable : public QRunnable
{
public:
    WorkerRunnable(std::shared_ptr<BatchState> st, int workerId)
        : st_(std::move(st)), workerId_(workerId)
    {
        setAutoDelete(true);
    }
    void run() override { WorkerLoop(st_, workerId_); }

private:
    std::shared_ptr<BatchState> st_;
    int workerId_;
};

void Launch(QThreadPool *pool, const std::shared_ptr<BatchState> &st)
{
    pool->setMaxThreadCount(qMax(1, st->threads));
    st->liveWorkers.storeRelease(st->threads);
    st->wall.start();
    for (int i = 0; i < st->threads; ++i)
        pool->start(new WorkerRunnable(st, i));
}

}  // namespace

Report RunBatchSync(const QVector<Task> &tasks, int threads, const Probe &probe,
                    QAtomicInteger<int> *cancel,
                    const std::function<void(const Outcome &)> &onEach)
{
    auto st = MakeState(tasks, threads, probe, cancel);
    st->onEach = onEach;
    if (st->threads == 0) {
        st->wall.start();
        return MakeReport(st, false);
    }
    QThreadPool pool;   // 每次自检私有池，waitForDone 后销毁，不碰全局池
    Launch(&pool, st);
    pool.waitForDone();
    return MakeReport(st, false);
}

// ---------------------------------------------------------------- 异步封装
Pool::Pool(QObject *uiContext) : ctx_(uiContext)
{
    pool_.setMaxThreadCount(kMaxThreads);
}

Pool::~Pool()
{
    // ctx_ 是 Pool 的宿主（NodeTester）。析构时先取消并等 worker 全部退出，
    // 保证不会有 worker 在宿主销毁后还去 postEvent。
    ++generation_;
    running_ = false;
    if (state_)
        state_->cancel->storeRelease(1);
    pool_.waitForDone(5000);
}

void Pool::CleanupWorkers()
{
    // 上一轮被 watchdog 抢先收口时，可能还有在收尾的 worker；等一小段，
    // 避免新批次和旧批次争抢同一个私有池的线程名额。
    if (pool_.activeThreadCount() > 0)
        pool_.waitForDone(300);
}

bool Pool::Start(QVector<Task> tasks, int threads, Each each, Done done)
{
    if (running_ || !ctx_)
        return false;
    CleanupWorkers();

    ++generation_;
    const int gen = generation_;
    QObject *c = ctx_;
    auto st = MakeState(std::move(tasks), threads, ProbeBlocking, nullptr);
    state_ = st;

    st->onEach = [this, gen, c, each](const Outcome &o) {
        // 从工作线程投递；ctx_ 只在主线程解引用过一次（上面的 c）。
        QMetaObject::invokeMethod(
            c,
            [this, gen, each, o] {
                if (gen == generation_ && each)
                    each(o);
            },
            Qt::QueuedConnection);
    };
    st->onDone = [this, gen, c, done](const Report &rep) {
        QMetaObject::invokeMethod(
            c,
            [this, gen, done, rep] {
                if (gen != generation_)
                    return;
                running_ = false;
                if (done)
                    done(rep);
            },
            Qt::QueuedConnection);
    };

    running_ = true;
    if (st->threads == 0) {
        st->wall.start();
        const Report rep = MakeReport(st, false);
        QMetaObject::invokeMethod(
            c,
            [this, gen, done, rep] {
                if (gen != generation_)
                    return;
                running_ = false;
                if (done)
                    done(rep);
            },
            Qt::QueuedConnection);
        return true;
    }
    Launch(&pool_, st);

    // 整批 watchdog：按"最坏波数 × 单条超时 + 4s 余量"算预算。正常情况下
    // 每条探测自己就会在 timeoutMs 截止，这里只兜 DNS / 阻塞调用卡死的异常。
    int maxTimeout = 3000;
    for (const Task &t : st->tasks)
        maxTimeout = qMax(maxTimeout, t.timeoutMs);
    const int waves = (st->tasks.size() + st->threads - 1) / st->threads;
    const int budget = waves * maxTimeout + 4000;
    QTimer::singleShot(budget, c, [this, gen, st, c] {
        if (gen != generation_ || st->finished.loadAcquire())
            return;
        st->cancel->storeRelease(1);
        QTimer::singleShot(500, c, [this, gen, st] {
            if (gen != generation_ || !st->finished.testAndSetOrdered(0, 1))
                return;
            const Report rep = MakeReport(st, true);
            if (st->onDone)
                st->onDone(rep);
        });
    });
    return true;
}

void Pool::Cancel()
{
    // 立刻废弃本轮：generation 一变，所有在途的队列回调都会被丢掉；
    // 原子标志让 worker 在下一个 100ms 分片边界退出，不留悬挂探测。
    ++generation_;
    running_ = false;
    if (state_)
        state_->cancel->storeRelease(1);
}

// ---------------------------------------------------------------- 离线自检
bool SelfTest(QString *out)
{
    QStringList lines;
    bool ok = true;
    auto check = [&](bool cond, const QString &name) {
        if (!cond)
            ok = false;
        lines << QStringLiteral("node-bench: %1=%2").arg(name).arg(cond ? 1 : 0);
    };
    auto makeTasks = [](int n) {
        QVector<Task> v;
        for (int i = 0; i < n; ++i) {
            Task t;
            t.id = i;
            t.timeoutMs = 60;
            v.append(t);
        }
        return v;
    };

    // 1) 线程数夹紧：0/负数落到 1，17/999 落到 16
    const bool clamp = ClampThreads(-7) == 1 && ClampThreads(0) == 1
                       && ClampThreads(1) == 1 && ClampThreads(8) == 8
                       && ClampThreads(16) == 16 && ClampThreads(17) == 16
                       && ClampThreads(999) == 16;
    check(clamp, QStringLiteral("clamp"));
    lines << QStringLiteral("node-bench: clamp-values -7=%1 0=%2 1=%3 8=%4 16=%5 17=%6 999=%7")
                 .arg(ClampThreads(-7))
                 .arg(ClampThreads(0))
                 .arg(ClampThreads(1))
                 .arg(ClampThreads(8))
                 .arg(ClampThreads(16))
                 .arg(ClampThreads(17))
                 .arg(ClampThreads(999));

    // 2) 40 任务 / 4 线程：假探测含耗时差异、失败、超时与一次抛异常
    QMutex seenMutex;
    QSet<int> seen;
    int duplicates = 0;
    Probe fake = [&](const Task &t, int worker, const QAtomicInteger<int> &) -> Outcome {
        {
            QMutexLocker lock(&seenMutex);
            if (seen.contains(t.id))
                ++duplicates;
            seen.insert(t.id);
        }
        Nap(2 + t.id % 5);
        if (t.id == 13)
            throw std::runtime_error("boom");
        Outcome o;
        o.id = t.id;
        o.worker = worker;
        o.elapsedMs = 2 + t.id % 5;
        if (t.id % 11 == 0) {
            o.timedOut = true;
            o.error = QStringLiteral("timeout (fake)");
            return o;
        }
        o.ok = t.id % 7 != 0;
        if (!o.ok)
            o.error = QStringLiteral("failed (fake)");
        else
            o.body = QByteArrayLiteral("ok");
        return o;
    };
    const Report r4 = RunBatchSync(makeTasks(40), 4, fake);
    bool order = r4.outcomes.size() == 40;
    for (int i = 0; order && i < r4.outcomes.size(); ++i)
        order = r4.outcomes.at(i).id == i;
    bool workerRange = true;
    for (const Outcome &o : r4.outcomes)
        workerRange = workerRange && o.worker >= 0 && o.worker < 4;
    check(order, QStringLiteral("order40"));
    check(duplicates == 0 && r4.executed == 40, QStringLiteral("unique40"));
    check(workerRange && r4.peak >= 2 && r4.peak <= 4, QStringLiteral("peak4"));
    check(r4.outcomes.at(13).ok == false
              && r4.outcomes.at(13).error.contains(QLatin1String("exception")),
          QStringLiteral("exception"));
    check(r4.timeouts == 4, QStringLiteral("timeouts"));
    // 4 个超时(0/11/22/33) + 1 个异常(13) + 5 个 %7 失败(7/14/21/28/35) = 10
    check(r4.failed == 10, QStringLiteral("failed"));
    lines << QStringLiteral("node-bench: batch tasks=%1 threads=%2 peak=%3 executed=%4 "
                            "failed=%5 timeouts=%6 wall=%7ms")
                 .arg(r4.tasks)
                 .arg(r4.threads)
                 .arg(r4.peak)
                 .arg(r4.executed)
                 .arg(r4.failed)
                 .arg(r4.timeouts)
                 .arg(r4.wallMs);

    // 3) 16 线程上限：999 被夹到 16，且峰值不超过 16
    const Report r16 = RunBatchSync(makeTasks(64), 999, fake);
    bool order16 = r16.outcomes.size() == 64;
    for (int i = 0; order16 && i < r16.outcomes.size(); ++i)
        order16 = r16.outcomes.at(i).id == i;
    check(r16.threads == 16 && r16.peak <= 16, QStringLiteral("cap16"));
    check(order16 && r16.executed == 64 && r16.peak >= 4, QStringLiteral("order64"));
    lines << QStringLiteral("node-bench: cap batch tasks=%1 threads=%2 peak=%3 executed=%4")
                 .arg(r16.tasks)
                 .arg(r16.threads)
                 .arg(r16.peak)
                 .arg(r16.executed);

    // 4) 运行中取消：第 5 个任务里置位，后续任务不再领取，且全部补齐失败记录
    QAtomicInteger<int> cancelFlag(0);
    Probe cancelling = [&](const Task &t, int, const QAtomicInteger<int> &) {
        if (t.id == 5)
            cancelFlag.storeRelease(1);
        Nap(1);
        Outcome o;
        o.id = t.id;
        o.ok = true;
        o.body = QByteArrayLiteral("ok");
        return o;
    };
    const Report rc = RunBatchSync(makeTasks(100), 4, cancelling, &cancelFlag);
    int filled = 0;
    bool restOk = true;
    for (const Outcome &o : rc.outcomes) {
        if (o.cancelled && !o.ok) {
            ++filled;
            continue;
        }
        restOk = restOk && o.ok;
    }
    check(rc.executed < 100 && rc.cancelledBatch && restOk
              && filled == 100 - rc.executed,
          QStringLiteral("cancel"));
    check(rc.outcomes.size() == 100, QStringLiteral("cancel-filled"));
    lines << QStringLiteral("node-bench: cancel tasks=100 executed=%1 cancelled=%2 filled=%3")
                 .arg(rc.executed)
                 .arg(int(rc.cancelledBatch))
                 .arg(rc.outcomes.size());

    // 5) 空批次 / 任务少于线程数：不起多余线程，汇总仍保持一一对应
    const Report r0 = RunBatchSync({}, 16, fake);
    check(r0.tasks == 0 && r0.threads == 0 && r0.outcomes.isEmpty(),
          QStringLiteral("empty"));
    const Report r3 = RunBatchSync(makeTasks(3), 16, fake);
    check(r3.threads == 3 && r3.executed == 3 && r3.outcomes.size() == 3,
          QStringLiteral("few"));

    lines << QStringLiteral("node-bench: all=%1").arg(ok ? QStringLiteral("PASS")
                                                        : QStringLiteral("FAIL"));
    if (out)
        *out = lines.join(QLatin1Char('\n'));
    return ok;
}

}  // namespace NodeBench
