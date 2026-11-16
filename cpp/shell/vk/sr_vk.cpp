#include "sr_vk.h"

#include "sr_graph.h"
#include "vk_device.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace SrVk {
namespace {

QString g_why;
bool g_tried = false;

}  // namespace

bool Available(QString *why)
{
    if (vk::Device::I().broken()) {
        const QString msg = QStringLiteral("Vulkan 设备反复丢失，已熔断并回退 CPU");
        if (why)
            *why = msg;
        return false;
    }
    if (!g_tried && !vk::Device::I().valid()) {
        // 首次探测：Ensure 会建实例/设备/arena。valid() 已为真时跳过，
        // 避免每次调用都重复打印设备摘要。
        g_tried = true;
        g_tried = true;
        QString err;
        if (!vk::Device::I().Ensure(&err)) {
            g_why = err;
            qWarning("vk: unavailable: %s", qPrintable(err));
        }
    }
    const bool ok = vk::Device::I().valid();
    if (why)
        *why = ok ? QString() : g_why;
    return ok;
}

QString Info()
{
    QString why;
    if (!Available(&why))
        return QStringLiteral("Vulkan 不可用：%1").arg(why);
    return vk::Device::I().summary();
}

bool Fp16Active()
{
    QString why;
    if (!Available(&why))
        return false;
    return vk::Device::I().fp16Usable();
}

bool Probe(QString *report)
{
    QStringList out;
    QString why;
    if (!Available(&why)) {
        out << QStringLiteral("vk-probe: unavailable %1").arg(why);
        if (report)
            *report = out.join(QLatin1Char('\n'));
        return false;
    }
    vk::Device &dev = vk::Device::I();
    out << QStringLiteral("vk-probe: %1").arg(dev.summary());
    out << QStringLiteral("vk-probe: %1").arg(dev.deviceReport());

    constexpr int kCount = 4096;
    std::vector<float> a(static_cast<size_t>(kCount));
    std::vector<float> b(static_cast<size_t>(kCount));
    std::vector<float> c(static_cast<size_t>(kCount), 0.0f);
    for (int i = 0; i < kCount; ++i) {
        a[size_t(i)] = float(i % 97) * 0.5f;
        b[size_t(i)] = float(i % 13) * 0.25f;
    }

    dev.resetActivations();
    const vk::BufferRef ba = dev.alloc(sizeof(float) * kCount);
    const vk::BufferRef bb = dev.alloc(sizeof(float) * kCount);
    const vk::BufferRef bc = dev.alloc(sizeof(float) * kCount);
    if (!ba.valid() || !bb.valid() || !bc.valid()) {
        out << QStringLiteral("vk-probe: alloc-failed %1").arg(dev.lastError());
        if (report)
            *report = out.join(QLatin1Char('\n'));
        return false;
    }

    QString err;
    if (!dev.beginPass()) {
        out << QStringLiteral("vk-probe: beginPass-failed %1").arg(dev.lastError());
        if (report)
            *report = out.join(QLatin1Char('\n'));
        return false;
    }
    const bool up = dev.upload(ba, a.data(), sizeof(float) * kCount)
                    && dev.upload(bb, b.data(), sizeof(float) * kCount);
    vk::Params pc;
    pc.set(0, kCount);
    const vk::BufferRef binds[3] = {ba, bb, bc};
    const bool dispatched =
        up && dev.dispatch("probe", pc, binds, 3, quint32((kCount + 63) / 64));
    const bool ok = dispatched && dev.endPass();
    if (!ok) {
        out << QStringLiteral("vk-probe: dispatch-failed err=%1 dispatched=%2 up=%3")
                   .arg(dev.lastError())
                   .arg(dispatched ? 1 : 0)
                   .arg(up ? 1 : 0);
        if (report)
            *report = out.join(QLatin1Char('\n'));
        return false;
    }
    QElapsedTimer t;
    t.start();
    const bool got = dev.download(bc, c.data(), sizeof(float) * kCount);
    const qint64 ms = t.elapsed();

    double maxErr = 0.0;
    int bad = 0;
    int firstBad = -1;
    for (int i = 0; i < kCount; ++i) {
        const double e = std::fabs(double(c[size_t(i)]) - (2.0 * a[size_t(i)] + b[size_t(i)]));
        if (e > 1e-4) { ++bad; if (firstBad < 0) firstBad = i; }
        maxErr = qMax(maxErr, e);
    }
    out << QStringLiteral("vk-probe: diag bad=%1 firstBad=%2 a=%3 b=%4 c=%5")
               .arg(bad).arg(firstBad)
               .arg(firstBad >= 0 ? double(a[size_t(firstBad)]) : -1.0, 0, 'g', 6)
               .arg(firstBad >= 0 ? double(b[size_t(firstBad)]) : -1.0, 0, 'g', 6)
               .arg(firstBad >= 0 ? double(c[size_t(firstBad)]) : -1.0, 0, 'g', 6);
    out << QStringLiteral("vk-probe: dispatch ok=%1 readback=%2 maxErr=%3 elapsedMs=%4")
               .arg(1)
               .arg(got ? 1 : 0)
               .arg(maxErr, 0, 'g', 4)
               .arg(ms);
    if (report)
        *report = out.join(QLatin1Char('\n'));
    for (const QString &line : out)
        qWarning("%s", line.toUtf8().constData());
    return got && maxErr < 1e-4;
}

// ---------------------------------------------------------------- Session
struct Session::Impl
{
    Graph graph;
    QString path;
    int tile = 0;
    bool loaded = false;
    bool prepared = false;
    QString err;
};

Session::Session() : d_(new Impl) {}
Session::~Session() = default;

bool Session::load(const QString &modelPath, QString *error)
{
    reset();
    QString err;
    if (!d_->graph.load(modelPath, &err)) {
        d_->err = err;
        if (error)
            *error = err;
        return false;
    }
    d_->path = modelPath;
    d_->loaded = true;
    return true;
}

void Session::reset()
{
    d_->graph = Graph();
    d_->path.clear();
    d_->tile = 0;
    d_->loaded = false;
    d_->prepared = false;
    d_->err.clear();
}

bool Session::ready() const
{
    return d_->prepared;
}

bool Session::ensureTile(int tile, QString *error)
{
    if (!d_->loaded) {
        if (error)
            *error = QStringLiteral("会话未加载模型");
        return false;
    }
    if (d_->prepared && d_->tile == tile)
        return true;
    QString err;
    if (!Available(&err)) {
        d_->err = err;
        if (error)
            *error = err;
        return false;
    }
    // 换 tile 要重新分配（所有张量形状都变），先释放上一轮的激活区
    Graph fresh;
    if (!fresh.load(d_->path, &err)) {
        d_->err = err;
        if (error)
            *error = err;
        return false;
    }
    // 一个 Session 独占 arena：换模型/换 tile 时权重区整体回收。
    // 调用方必须先销毁上一个会话（SrEngine 与 Sweep 都是这么做的）。
    vk::Device::I().resetWeights();
    if (!fresh.prepare(tile, tile, &err)) {
        d_->err = err;
        if (error)
            *error = err;
        return false;
    }
    d_->graph = std::move(fresh);
    d_->tile = tile;
    d_->prepared = true;
    qInfo("vk-sr: %s", qPrintable(d_->graph.summary()));
    return true;
}

bool Session::run(const float *input, std::vector<float> *output, int *outW, int *outH,
                  QString *error)
{
    if (!d_->prepared) {
        if (error)
            *error = QStringLiteral("Vulkan 会话未准备");
        return false;
    }
    QString err;
    if (d_->graph.run(input, output, outW, outH, &err))
        return true;
    // VK_ERROR_DEVICE_LOST：驱动重置 / GPU watchdog / 后台显存被回收。
    // 不能只重试 run —— 所有 BufferRef 都指向旧 arena，必须重建设备后
    // 重新 load + 上传权重 + prepare，再用同一个 tile 重跑一次。
    if (vk::Device::I().lost()) {
        qWarning("vk-sr: run failed with DEVICE_LOST, rebuilding session (tile=%d)", d_->tile);
        QString recErr;
        if (vk::Device::I().Recover(&recErr)) {
            const int tile = d_->tile;
            d_->prepared = false;
            d_->graph = Graph();
            if (ensureTile(tile, &recErr) && d_->graph.run(input, output, outW, outH, &err)) {
                qInfo("vk-sr: recovered after DEVICE_LOST, gen=%d",
                      vk::Device::I().generation());
                return true;
            }
            err = QStringLiteral("重建后仍失败：%1").arg(err);
        } else {
            err = QStringLiteral("设备重建失败：%1（原错误：%2）").arg(recErr, err);
        }
    }
    d_->err = err;
    if (error)
        *error = err;
    return false;
}

int Session::outW() const { return d_->graph.outW(); }
int Session::outH() const { return d_->graph.outH(); }
int Session::scale() const { return d_->graph.scale(); }
QString Session::summary() const { return d_->prepared ? d_->graph.summary() : QString(); }
QString Session::lastError() const { return d_->err; }
Graph *Session::graph() { return &d_->graph; }

bool Session::runTiles(int count, const TileStream &cb, QString *error)
{
    if (!d_->prepared) {
        if (error)
            *error = QStringLiteral("Vulkan 会话未准备");
        return false;
    }
    QString err;
    if (d_->graph.runStream(count, cb, &err)) {
        d_->err.clear();
        return true;
    }
    d_->err = err;
    if (error)
        *error = err;
    // DEVICE_LOST 不在流水中途恢复：已提交的 tile 无法回滚，输出已部分写回，
    // 唯一一致的做法是让上层整图重跑（CPU 回退或 Vulkan 重建后的整图重试）。
    if (vk::Device::I().lost())
        qWarning("vk-sr: stream failed with DEVICE_LOST (%d tiles, gen=%d)",
                 count, vk::Device::I().generation());
    return false;
}

// ---------------------------------------------------------------- 模型扫描
namespace {

bool IsQuantized(const QString &file)
{
    return file.contains(QLatin1String("_q16")) || file.contains(QLatin1String("lowq"));
}

}  // namespace

QVector<ModelResult> Sweep(const QString &dir, const QStringList &only, int tile, QString *report)
{
    QStringList lines;
    QVector<ModelResult> results;
    const QDir d(dir);
    const QStringList files = d.entryList({QStringLiteral("*.onnx")}, QDir::Files, QDir::Name);
    if (files.isEmpty()) {
        if (report)
            *report = QStringLiteral("vk-sweep: no onnx under %1").arg(dir);
        return results;
    }
    for (const QString &file : files) {
        if (IsQuantized(file)) {
            // 不是"静默跳过"：QDQ 模型在报告里必须留下明确原因，
            // 否则无法区分"没扫到"和"扫了但不支持"。
            ModelResult s;
            s.file = file;
            s.skipped = true;
            s.note = QStringLiteral("QDQ 量化模型，Vulkan fp32 管线不支持（QNN/CPU 路径）");
            results.append(s);
            lines << QStringLiteral("%1 SKIP %2").arg(file, s.note);
            continue;
        }
        if (!only.isEmpty()) {
            bool hit = false;
            for (const QString &k : only) {
                if (file.contains(k, Qt::CaseInsensitive)) {
                    hit = true;
                    break;
                }
            }
            if (!hit)
                continue;
        }
        ModelResult r;
        r.file = file;
        QElapsedTimer timer;
        timer.start();
        Session session;
        QString err;
        if (!session.load(d.filePath(file), &err)) {
            r.note = err;
            r.elapsedMs = timer.elapsed();
            results.append(r);
            lines << QStringLiteral("%1 FAIL load %2").arg(file, err);
            continue;
        }
        if (!session.ensureTile(tile, &err)) {
            r.note = err;
            r.elapsedMs = timer.elapsed();
            results.append(r);
            lines << QStringLiteral("%1 FAIL prepare %2").arg(file, err);
            continue;
        }
        const int inElems = 3 * tile * tile;
        std::vector<float> input(static_cast<std::size_t>(inElems), 0.0f);
        for (int i = 0; i < inElems; ++i)
            // 与 --vk-compare / onnx_ref_numpy.py 完全相同的伪随机输入，
            // 否则逐节点 diff 会把"输入不同"误判成"kernel 不同"
            input[size_t(i)] = float((i * 37 + 11) % 251) / 251.0f;
        std::vector<float> output;
        int ow = 0;
        int oh = 0;
        const bool wantDump = qEnvironmentVariableIsSet("JM_VK_DUMP");
        if (wantDump && session.graph())
            session.graph()->setDumpTensors(true);
        if (!session.run(input.data(), &output, &ow, &oh, &err)) {
            r.note = err;
            r.elapsedMs = timer.elapsed();
            results.append(r);
            lines << QStringLiteral("%1 FAIL run %2").arg(file, err);
            continue;
        }
        int bad = 0;
        float lo = 1e30f;
        float hi = -1e30f;
        for (float v : output) {
            if (!std::isfinite(v))
                ++bad;
            lo = qMin(lo, v);
            hi = qMax(hi, v);
        }
        if (wantDump && session.graph())
            std::fprintf(stderr, "%s\n", session.graph()->dumpReport().toUtf8().constData());
        r.ok = bad == 0;
        r.outW = ow;
        r.outH = oh;
        r.elapsedMs = timer.elapsed();
        r.note = QStringLiteral("%1ms nan=%2 range=[%3,%4]")
                     .arg(r.elapsedMs)
                     .arg(bad)
                     .arg(double(lo), 0, 'g', 4)
                     .arg(double(hi), 0, 'g', 4);
        results.append(r);
        lines << QStringLiteral("%1 %2 out=%3x%4 %5")
                     .arg(file, r.ok ? QStringLiteral("ok") : QStringLiteral("bad"))
                     .arg(ow)
                     .arg(oh)
                     .arg(r.note);
    }
    if (report)
        *report = lines.join(QLatin1Char('\n'));
    return results;
}

// ---------------------------------------------------------------- 卷积单算子自检
// 把一个 3x3 / stride1 / pad1 的卷积与 CPU 逐元素对比，用来把"图搭错了"和
// "kernel 算错了"分开。回归时应当第一次就跑通。
bool ConvSelfTest(QString *report)
{
    QStringList out;
    QString why;
    if (!Available(&why)) {
        if (report)
            *report = QStringLiteral("vk-conv-test: unavailable %1").arg(why);
        return false;
    }
    // JM_VK_CONV_C/OC/W/PAD 可把自检放大到真实模型层的尺寸，快速定位精度/边界问题
    auto envInt = [](const char *k, int def) {
        bool ok = false;
        const int v = qEnvironmentVariableIntValue(k, &ok);
        return ok ? v : def;
    };
    const int C = qMax(1, envInt("JM_VK_CONV_C", 3));
    const int OC = qMax(1, envInt("JM_VK_CONV_OC", 2));
    const int W = qMax(1, envInt("JM_VK_CONV_W", 8));
    const int H = W;
    const int K = 3;
    const int PAD = envInt("JM_VK_CONV_PAD", 1);
    const int OH = H + 2 * PAD - K + 1;
    const int OW = W + 2 * PAD - K + 1;
    std::vector<float> x(static_cast<std::size_t>(C * H * W));
    std::vector<float> w(static_cast<std::size_t>(OC * C * K * K));
    std::vector<float> b(static_cast<std::size_t>(OC), 0.0f);
    std::vector<float> y(static_cast<std::size_t>(OC * OH * OW), 0.0f);
    for (size_t i = 0; i < x.size(); ++i)
        x[i] = float((int(i) * 37) % 97) / 97.0f - 0.5f;
    for (size_t i = 0; i < w.size(); ++i)
        w[i] = float((int(i) * 53) % 89) / 89.0f - 0.5f;
    b[0] = 0.1f;
    b[1] = -0.2f;
    // 受控实验：JM_VK_CONV_ZERO=1 时把权重全置零，输出应恒等于 bias[oc]
    const QString convMode = qEnvironmentVariable("JM_VK_CONV_ZERO");
    if (convMode == QLatin1String("1"))
        std::fill(w.begin(), w.end(), 0.0f);
    if (convMode == QLatin1String("one")) {
        std::fill(w.begin(), w.end(), 0.0f);
        w[0] = 1.0f;
    }

    // CPU 参考
    std::vector<float> ref(static_cast<std::size_t>(OC * OH * OW), 0.0f);
    for (int oc = 0; oc < OC; ++oc) {
        for (int oy = 0; oy < OH; ++oy) {
            for (int ox = 0; ox < OW; ++ox) {
                float acc = b[size_t(oc)];
                for (int c = 0; c < C; ++c) {
                    for (int ky = 0; ky < K; ++ky) {
                        const int iy = oy - PAD + ky;
                        if (iy < 0 || iy >= H)
                            continue;
                        for (int kx = 0; kx < K; ++kx) {
                            const int ix = ox - PAD + kx;
                            if (ix < 0 || ix >= W)
                                continue;
                            acc += x[size_t((c * H + iy) * W + ix)]
                                   * w[size_t(((oc * C + c) * K + ky) * K + kx)];
                        }
                    }
                }
                ref[size_t((oc * OH + oy) * OW + ox)] = acc;
            }
        }
    }

    vk::Device &dev = vk::Device::I();
    dev.resetWeights();
    const vk::BufferRef bx = dev.alloc(quint64(x.size()) * 4);
    const vk::BufferRef bw = dev.allocPersistent(quint64(w.size()) * 4);
    const vk::BufferRef bb = dev.allocPersistent(quint64(b.size()) * 4);
    const vk::BufferRef by = dev.alloc(quint64(y.size()) * 4);
    if (!bx.valid() || !bw.valid() || !bb.valid() || !by.valid()) {
        if (report)
            *report = QStringLiteral("vk-conv-test: alloc-failed %1").arg(dev.lastError());
        return false;
    }
    if (!dev.beginPass() || !dev.upload(bx, x.data(), quint64(x.size()) * 4)
        || !dev.upload(bw, w.data(), quint64(w.size()) * 4)
        || !dev.upload(bb, b.data(), quint64(b.size()) * 4)) {
        if (report)
            *report = QStringLiteral("vk-conv-test: upload-failed %1").arg(dev.lastError());
        dev.endPass();
        return false;
    }
    // 上传必须 endPass 之后才落盘（download 是独立的一次 submit，
    // 在同一个 pass 还没提交时读到的还是旧内容）
    if (!dev.endPass()) {
        if (report)
            *report = QStringLiteral("vk-conv-test: upload-end-pass-failed");
        return false;
    }
    // 先验证"上传 + 偏移"这一段：把三块设备缓冲读回来跟主机数组比对
    {
        std::vector<float> chkX(x.size()), chkW(w.size()), chkB(b.size());
        const bool okX = dev.download(bx, chkX.data(), quint64(chkX.size()) * 4);
        const bool okW = dev.download(bw, chkW.data(), quint64(chkW.size()) * 4);
        const bool okB = dev.download(bb, chkB.data(), quint64(chkB.size()) * 4);
        const bool sameX = okX && std::memcmp(chkX.data(), x.data(), x.size() * 4) == 0;
        const bool sameW = okW && std::memcmp(chkW.data(), w.data(), w.size() * 4) == 0;
        const bool sameB = okB && std::memcmp(chkB.data(), b.data(), b.size() * 4) == 0;
        out << QStringLiteral("vk-conv-test: upload x=%1 w=%2 b=%3 offX=%4 offW=%5 offB=%6")
                   .arg(sameX ? 1 : 0).arg(sameW ? 1 : 0).arg(sameB ? 1 : 0)
                   .arg(bx.offset).arg(bw.offset).arg(bb.offset);
    }

    vk::Params pc;
    pc.set(0, C);
    pc.set(1, H);
    pc.set(2, W);
    pc.set(3, OC);
    pc.set(4, K);
    pc.set(5, K);
    pc.set(6, 1);
    pc.set(7, 1);
    pc.set(8, PAD);
    pc.set(9, PAD);
    pc.set(10, OH);
    pc.set(11, OW);
    pc.set(12, 1);
    // push constant 逐槽诊断
    {
        std::vector<float> got(16, 0.0f);
        const vk::BufferRef binds2[2] = {bx, by};
        vk::Params probePc;
        for (int k = 0; k < 16; ++k)
            probePc.set(k, k * 100 + 7);
        dev.beginPass();
        dev.dispatch("pcdump", probePc, binds2, 2, 1, 1, 1);
        dev.endPass();
        dev.download(by, got.data(), quint64(got.size()) * 4);
        QStringList v;
        for (int k = 0; k < 16; ++k)
            v << QString::number(int(got[size_t(k)]));
        out << QStringLiteral("vk-conv-test: pc=[%1] expect=[7,107,207,...]").arg(v.join(QLatin1String(",")));
    }

    const vk::BufferRef binds[4] = {bx, bw, bb, by};
    const bool beginOk = dev.beginPass();
    const bool dispatched = beginOk && dev.dispatch("conv", pc, binds, 4,
                                                    quint32((OW + 255) / 256), quint32(OH),
                                                    quint32(OC));
    const bool ok = dispatched && dev.endPass();
    if (!ok) {
        if (report)
            *report = QStringLiteral("vk-conv-test: dispatch-failed %1").arg(dev.lastError());
        return false;
    }
    if (!dev.download(by, y.data(), quint64(y.size()) * 4)) {
        if (report)
            *report = QStringLiteral("vk-conv-test: download-failed %1").arg(dev.lastError());
        return false;
    }
    double maxErr = 0.0;
    int firstBad = -1;
    for (size_t i = 0; i < y.size(); ++i) {
        const double e = std::fabs(double(y[i]) - double(ref[i]));
        if (e > 1e-5 && firstBad < 0)
            firstBad = int(i);
        maxErr = qMax(maxErr, e);
    }
    for (int i = 0; i < 20; ++i)
        out << QStringLiteral("vk-conv-test:   y[%1]=%2 ref=%3")
                   .arg(i).arg(double(y[size_t(i)]), 0, 'g', 6)
                   .arg(double(ref[size_t(i)]), 0, 'g', 6);
    out << QStringLiteral("vk-conv-test: %1x%2x%3 -> %4ch k%5 pad%6 maxErr=%7 firstBad=%8")
               .arg(C).arg(H).arg(W).arg(OC).arg(K).arg(PAD)
               .arg(maxErr, 0, 'g', 4)
               .arg(firstBad);

    // ---- Winograd F(2,3) 用例：C=16/OC=16（命中 sr_graph 的通道门控），
    // 权重 CPU 预变换后走 convw kernel，与 CPU 参考对比。数值正确性是
    // G/B^T/A^T 常数与权重布局 [OC][IC][4][4] 的直接验证。
    double winoErr = -1.0;
    bool winoOk = false;
    {
        const int wC = 16;
        const int wOC = 16;
        const int wW = 8;
        const int wOH = wW, wOW = wW;   // 3x3 / stride1 / pad1 输出尺寸 = 输入尺寸
        std::vector<float> wx(size_t(wC * wW * wW));
        std::vector<float> ww(size_t(wOC * wC * 9));
        std::vector<float> wb(size_t(wOC), 0.0f);
        for (size_t i = 0; i < wx.size(); ++i)
            wx[i] = float((int(i) * 31) % 89) / 89.0f - 0.5f;
        for (size_t i = 0; i < ww.size(); ++i)
            ww[i] = float((int(i) * 47) % 83) / 83.0f - 0.5f;
        wb[0] = 0.05f;
        wb[7] = -0.15f;
        // CPU 参考（与上面直接卷积参考同一套累加顺序）
        std::vector<float> wref(size_t(wOC * wOH * wOW), 0.0f);
        for (int oc = 0; oc < wOC; ++oc)
            for (int oy = 0; oy < wOH; ++oy)
                for (int ox = 0; ox < wOW; ++ox) {
                    float acc = wb[size_t(oc)];
                    for (int c = 0; c < wC; ++c)
                        for (int ky = 0; ky < 3; ++ky) {
                            const int iy = oy - 1 + ky;
                            if (iy < 0 || iy >= wW)
                                continue;
                            for (int kx = 0; kx < 3; ++kx) {
                                const int ix = ox - 1 + kx;
                                if (ix < 0 || ix >= wW)
                                    continue;
                                acc += wx[size_t((c * wW + iy) * wW + ix)]
                                       * ww[size_t(((oc * wC + c) * 3 + ky) * 3 + kx)];
                            }
                        }
                    wref[size_t((oc * wOH + oy) * wOW + ox)] = acc;
                }
        std::vector<float> wT;
        WinogradTransformWeights(ww.data(), wOC, wC, &wT);
        std::vector<float> wy(size_t(wOC * wOH * wOW), 0.0f);

        dev.resetWeights();
        const vk::BufferRef wxb = dev.alloc(quint64(wx.size()) * 4);
        const vk::BufferRef wwb = dev.allocPersistent(quint64(wT.size()) * 4);
        const vk::BufferRef wbb = dev.allocPersistent(quint64(wb.size()) * 4);
        const vk::BufferRef wyb = dev.alloc(quint64(wy.size()) * 4);
        if (!wxb.valid() || !wwb.valid() || !wbb.valid() || !wyb.valid()) {
            out << QStringLiteral("vk-conv-test: wino alloc-failed %1").arg(dev.lastError());
        } else {
            bool upOk = false;
            if (dev.beginPass()) {
                upOk = dev.upload(wxb, wx.data(), quint64(wx.size()) * 4)
                       && dev.upload(wwb, wT.data(), quint64(wT.size()) * 4)
                       && dev.upload(wbb, wb.data(), quint64(wb.size()) * 4);
                upOk = upOk && dev.endPass();
            }
            vk::Params wpc;
            wpc.set(0, wC);
            wpc.set(1, wW);
            wpc.set(2, wW);
            wpc.set(3, wOC);
            wpc.set(4, 3);
            wpc.set(5, 3);
            wpc.set(6, 1);
            wpc.set(7, 1);
            wpc.set(8, 1);
            wpc.set(9, 1);
            wpc.set(10, wOH);
            wpc.set(11, wOW);
            wpc.set(12, 1);
            const vk::BufferRef wbinds[4] = {wxb, wwb, wbb, wyb};
            const int tileCols = (wOW + 1) / 2;
            const int tileRows = (wOH + 1) / 2;
            bool dOk = false;
            if (upOk && dev.beginPass()) {
                dOk = dev.dispatch("convw", wpc, wbinds, 4,
                                   quint32((tileCols + 63) / 64), quint32(tileRows),
                                   quint32(wOC))
                      && dev.endPass();
            }
            bool dOkGot = false;
            if (dOk)
                dOkGot = dev.download(wyb, wy.data(), quint64(wy.size()) * 4);
            winoErr = 0.0;
            int wFirstBad = -1;
            if (dOkGot) {
                for (size_t i = 0; i < wy.size(); ++i) {
                    const double e = std::fabs(double(wy[i]) - double(wref[i]));
                    if (e > 1e-4 && wFirstBad < 0)
                        wFirstBad = int(i);
                    winoErr = qMax(winoErr, e);
                }
            }
            winoOk = dOkGot && wFirstBad < 0;
            out << QStringLiteral("vk-conv-test: winograd F(2,3) 16x8x8 -> 16ch maxErr=%1 "
                                  "firstBad=%2 dispatched=%3")
                       .arg(winoErr, 0, 'g', 4)
                       .arg(wFirstBad)
                       .arg(dOkGot ? 1 : 0);
            // 调试：前 16 个输出与参考逐个对照（oc0 的前两行），能直接看出
            // 是哪个 tile / 哪个方向开始发散
            if (!winoOk) {
                for (int i = 0; i < 16 && i < wOH * wOW; ++i)
                    out << QStringLiteral("vk-conv-test:   wy[%1]=(oc0,%2,%3) vk=%4 ref=%5")
                               .arg(i).arg(i / wOW).arg(i % wOW)
                               .arg(double(wy[size_t(i)]), 0, 'g', 6)
                               .arg(double(wref[size_t(i)]), 0, 'g', 6);
            }
        }
    }

    if (report)
        *report = out.join(QLatin1Char(10));
    for (const QString &line : out)
        qWarning("%s", line.toUtf8().constData());
    return maxErr < 1e-5 && winoOk;
}

// ---------------------------------------------------------------- Pad 单算子自检
// 用 x[r,c] = r*100+c 作为输入，输出值就直接编码了"取自哪个源坐标"，
// 一眼能看出 reflect 的镜像索引对不对（realcugan 的 18 像素 reflect pad 曾出错）。
bool PadSelfTest(QString *report)
{
    QString why;
    if (!Available(&why)) {
        if (report)
            *report = QStringLiteral("vk-pad-test: unavailable %1").arg(why);
        return false;
    }
    const int C = 1, H = 8, W = 8, PAD = 2;
    const int OH = H + 2 * PAD, OW = W + 2 * PAD;
    std::vector<float> x(static_cast<std::size_t>(C * H * W));
    std::vector<float> y(static_cast<std::size_t>(C * OH * OW), -1.0f);
    for (int r = 0; r < H; ++r)
        for (int c = 0; c < W; ++c)
            x[size_t(r * W + c)] = float(r * 100 + c);

    auto refl = [](int v, int n) {
        const int p = 2 * (n - 1);
        int m = ((v % p) + p) % p;
        return m < n ? m : p - m;
    };
    std::vector<float> ref(static_cast<std::size_t>(C * OH * OW));
    for (int oy = 0; oy < OH; ++oy)
        for (int ox = 0; ox < OW; ++ox)
            ref[size_t(oy * OW + ox)] = float(refl(oy - PAD, H) * 100 + refl(ox - PAD, W));

    vk::Device &dev = vk::Device::I();
    dev.resetWeights();
    const vk::BufferRef bx = dev.alloc(quint64(x.size()) * 4);
    const vk::BufferRef by = dev.alloc(quint64(y.size()) * 4);
    if (!dev.beginPass() || !dev.upload(bx, x.data(), quint64(x.size()) * 4)
        || !dev.endPass()) {
        if (report)
            *report = QStringLiteral("vk-pad-test: upload-failed %1").arg(dev.lastError());
        return false;
    }
    vk::Params pc;
    pc.set(0, C);
    pc.set(1, H);
    pc.set(2, W);
    pc.set(3, OH);
    pc.set(4, OW);
    pc.set(5, PAD);
    pc.set(6, PAD);
    pc.set(7, 1);   // reflect
    const vk::BufferRef binds[2] = {bx, by};
    dev.beginPass();
    const bool ok = dev.dispatch("pad", pc, binds, 2, quint32((C * OH * OW + 63) / 64), 1, 1)
                    && dev.endPass();
    if (!ok) {
        if (report)
            *report = QStringLiteral("vk-pad-test: dispatch-failed %1").arg(dev.lastError());
        return false;
    }
    if (!dev.download(by, y.data(), quint64(y.size()) * 4))
        return false;
    int bad = 0;
    QStringList firstRow;
    for (int k = 0; k < OW; ++k) {
        firstRow << QString::number(double(y[size_t(k)]), 'g', 4);
        if (std::fabs(double(y[size_t(k)]) - double(ref[size_t(k)])) > 1e-4)
            ++bad;
    }
    if (report) {
        *report = QStringLiteral("vk-pad-test: bad=%1 row0 vk=[%2] ref=[%3]")
                      .arg(bad)
                      .arg(firstRow.join(QLatin1String(",")))
                      .arg([&] {
                          QStringList v;
                          for (int k = 0; k < OW; ++k)
                              v << QString::number(double(ref[size_t(k)]), 'g', 4);
                          return v.join(QLatin1String(","));
                      }());
    }
    qWarning("vk-pad-test: bad=%d", bad);
    return bad == 0;
}

// ---------------------------------------------------------------- 吞吐基准
BenchResult Benchmark(const QString &modelPath, int tile, int iterations, QString *report)
{
    BenchResult r;
    r.file = QFileInfo(modelPath).fileName();
    r.tile = tile;
    r.iterations = qMax(1, iterations);
    QString err;
    Session session;
    if (!session.load(modelPath, &err) || !session.ensureTile(tile, &err)) {
        r.note = err;
        if (report)
            *report = QStringLiteral("vk-bench: %1 FAIL %2").arg(r.file, err);
        return r;
    }
    const int elems = 3 * tile * tile;
    std::vector<float> input(static_cast<std::size_t>(elems), 0.0f);
    for (int i = 0; i < elems; ++i)
        input[std::size_t(i)] = float((i * 37 + 11) % 251) / 251.0f;
    std::vector<float> out;
    int ow = 0;
    int oh = 0;
    // warmup：首批含管线编译/权重上传后的第一次同步，不能计入稳态吞吐
    if (!session.run(input.data(), &out, &ow, &oh, &err)) {
        r.note = QStringLiteral("warmup 失败：%1").arg(err);
        if (report)
            *report = QStringLiteral("vk-bench: %1 FAIL %2").arg(r.file, r.note);
        return r;
    }
    QVector<double> samples;
    samples.reserve(r.iterations);
    for (int k = 0; k < r.iterations; ++k) {
        QElapsedTimer t;
        t.start();
        if (!session.run(input.data(), &out, &ow, &oh, &err)) {
            r.note = QStringLiteral("第 %1 次失败：%2").arg(k + 1).arg(err);
            if (report)
                *report = QStringLiteral("vk-bench: %1 FAIL %2").arg(r.file, r.note);
            return r;
        }
        samples.append(double(t.nsecsElapsed()) / 1.0e6);
    }
    std::sort(samples.begin(), samples.end());
    r.minMs = samples.first();
    r.medianMs = samples.at(samples.size() / 2);
    double sum = 0.0;
    for (double v : samples)
        sum += v;
    r.meanMs = sum / double(samples.size());
    r.activationBytes = vk::Device::I().activationUsed();
    r.ok = true;
    r.note = QStringLiteral("out=%1x%2 act=%3MB")
                 .arg(ow)
                 .arg(oh)
                 .arg(r.activationBytes / (1024 * 1024));
    if (report)
        *report = QStringLiteral("vk-bench: %1 tile=%2 n=%3 min=%4ms median=%5ms mean=%6ms %7 dev=%8")
                      .arg(r.file)
                      .arg(tile)
                      .arg(r.iterations)
                      .arg(r.minMs, 0, 'f', 2)
                      .arg(r.medianMs, 0, 'f', 2)
                      .arg(r.meanMs, 0, 'f', 2)
                      .arg(r.note)
                      .arg(vk::Device::I().info().name);
    return r;
}
}  // namespace SrVk
