#include "sr_graph.h"

#include "vk_half.h"
#include "vk_onnx.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <queue>

namespace SrVk {
namespace {

using vk::BufferRef;
using vk::Params;

// ---------------------------------------------------------------- 形状
struct Dims
{
    int n = 1;
    int c = 0;
    int h = 0;
    int w = 0;
    qint64 count() const { return qint64(n) * c * h * w; }
    bool valid() const { return n > 0 && c > 0 && h > 0 && w > 0; }
    QString str() const
    {
        return QStringLiteral("%1x%2x%3x%4").arg(n).arg(c).arg(h).arg(w);
    }
};

Dims FromOnnxDims(const QVector<qint64> &d)
{
    Dims r;
    const int k = d.size();
    if (k == 4) {
        r.n = int(d[0]);
        r.c = int(d[1]);
        r.h = int(d[2]);
        r.w = int(d[3]);
    } else if (k == 3) {
        r.c = int(d[0]);
        r.h = int(d[1]);
        r.w = int(d[2]);
    } else if (k == 2) {
        r.c = int(d[0]);
        r.h = 1;
        r.w = int(d[1]);
    } else if (k == 1) {
        r.c = int(d[0]);
        r.h = 1;
        r.w = 1;
    } else if (k == 0) {
        r.c = 1;
        r.h = 1;
        r.w = 1;
    } else {
        r.c = -1;
    }
    // ONNX 里 -1/-2 是"未知"；这些模型里出现 -1 只可能是导出占位，按 1 处理并在折叠里核对
    if (r.n <= 0)
        r.n = 1;
    if (r.c <= 0)
        r.c = 1;
    if (r.h <= 0)
        r.h = 1;
    if (r.w <= 0)
        r.w = 1;
    return r;
}

// ---------------------------------------------------------------- 常量折叠用的主机张量
struct HTensor
{
    QVector<int> dims;
    std::vector<float> v;

    int rank() const { return dims.size(); }
    qint64 count() const
    {
        qint64 n = 1;
        for (int d : dims)
            n *= d;
        return n;
    }
    bool ok() const { return qint64(v.size()) == count(); }
};

QVector<int> AlignDims(const QVector<int> &d, int rank)
{
    QVector<int> out;
    out.resize(rank);
    const int off = rank - d.size();
    for (int i = 0; i < rank; ++i)
        out[i] = (i < off || d[i - off] <= 0) ? 1 : d[i - off];
    return out;
}

bool BroadcastDims(const QVector<int> &a, const QVector<int> &b, QVector<int> *out)
{
    const int rank = qMax(a.size(), b.size());
    const QVector<int> aa = AlignDims(a, rank);
    const QVector<int> bb = AlignDims(b, rank);
    out->resize(rank);
    for (int i = 0; i < rank; ++i) {
        if (aa[i] == bb[i] || bb[i] == 1)
            (*out)[i] = aa[i];
        else if (aa[i] == 1)
            (*out)[i] = bb[i];
        else
            return false;
    }
    return true;
}

// 把线性下标拆成 out 形状的多维下标，再按 src 形状取偏移（长度 1 的维取 0）
inline qint64 SrcOffset(const QVector<int> &src, const QVector<int> &idx)
{
    qint64 off = 0;
    for (int i = 0; i < src.size(); ++i)
        off = off * src[i] + (src[i] == 1 ? 0 : idx[i]);
    return off;
}

void Unravel(qint64 i, const QVector<int> &dims, QVector<int> *idx)
{
    idx->resize(dims.size());
    for (int k = dims.size() - 1; k >= 0; --k) {
        const int d = dims[k] > 0 ? dims[k] : 1;
        (*idx)[k] = int(i % d);
        i /= d;
    }
}

// 折叠算子表：只处理"不依赖像素值、只依赖形状/小常量"的节点。
bool FoldableOp(const QString &op)
{
    static const QSet<QString> ops = {
        QStringLiteral("Constant"),  QStringLiteral("ConstantOfShape"),
        QStringLiteral("Mul"),       QStringLiteral("Add"),
        QStringLiteral("Sub"),       QStringLiteral("Div"),
        QStringLiteral("Equal"),     QStringLiteral("Where"),
        QStringLiteral("Expand"),    QStringLiteral("Concat"),
    };
    return ops.contains(op);
}

}  // namespace

// ---------------------------------------------------------------- Impl
struct TensorInfo
{
    QString name;
    Dims d;
    bool isConst = false;
    int defIndex = -1;   // 定义该张量的步序号（输入为 -1）
    int lastUse = -1;
    BufferRef buf;
    // 常量数据（折叠结果或 initializer 的 raw）
    bool hostOwned = false;
    std::vector<float> host;
    bool fromInit = false;
    // Winograd F(2,3) 预变换：host 侧 [OC][IC][3][3] fp32 权重 -> 设备侧
    // [OC][IC][4][4]（元素数 9 -> 16）。只在常量权重上启用（见 prepare 的判定）。
    bool wino = false;
};

struct Step
{
    QString shader;
    Params pc;
    int in[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int nIn = 0;
    int out = -1;
    quint32 gx = 1, gy = 1, gz = 1;
    QString op;
    QString outName;
};

struct Graph::Impl
{
    vk::OnnxModel model;
    QVector<TensorInfo> tensors;
    QHash<QString, int> index;
    QVector<Step> steps;
    // 分批提交切点（A 方案，防移动 GPU 驱动 watchdog）：batchCheckpoints[i]=true
    // 表示第 i 步 dispatch 后应 endPass/beginPass 切换提交批次。prepare 末尾
    // 按每步 gx*gy*gz 累计到阈值生成。run()/runStream 的 dispatch 循环共用。
    QVector<bool> batchCheckpoints;
    BufferRef actPool;
    int outTensor = -1;
    int inTensor = -1;
    qint64 actBytes = 0;
    QStringList foldNotes;
    // 存储格式：fp16 时所有张量在显存里占 2 字节/元素（shader 端 unpack/pack）
    bool fp16 = false;
    int elemBytes() const { return fp16 ? 2 : 4; }
    QString shaderName(const char *base) const
    {
        return fp16 ? QStringLiteral("%1_fp16").arg(QLatin1String(base))
                    : QString::fromLatin1(base);
    }
    // 流水的输入/输出双缓冲（prepare 里从激活池头部预留）
    BufferRef inSlot[2];
    BufferRef outSlot[2];
    bool streamReady = false;   // slots 在 prepare 里分配且未失效

    int find(const QString &name) const
    {
        auto it = index.constFind(name);
        return it == index.constEnd() ? -1 : it.value();
    }
    int add(const TensorInfo &t)
    {
        const int i = tensors.size();
        tensors.append(t);
        index.insert(t.name, i);
        return i;
    }
};

Graph::Graph() : d_(new Impl) {}
Graph::~Graph() = default;

bool Graph::load(const QString &onnxPath, QString *error)
{
    d_->model = vk::OnnxModel();
    QString err;
    if (!vk::ReadOnnxFile(onnxPath, &d_->model, &err)) {
        err_ = err;
        if (error)
            *error = err;
        return false;
    }
    // 量化模型显式拒绝：QDQ 需要反量化，算错了比不支持更糟
    for (const vk::OnnxNode &n : d_->model.nodes) {
        if (n.op == QLatin1String("DequantizeLinear")
            || n.op == QLatin1String("QuantizeLinear")) {
            err_ = QStringLiteral("量化模型（QDQ，含 %1）暂不支持 Vulkan 后端").arg(n.op);
            if (error)
                *error = err_;
            return false;
        }
    }
    path_ = onnxPath;
    loaded_ = true;
    return true;
}

QString Graph::summary() const
{
    QString extra;
    if (stats_.fp16)
        extra += QStringLiteral(" fp16=on");
    if (stats_.winoLayers > 0)
        extra += QStringLiteral(" wino=%1").arg(stats_.winoLayers);
    return QStringLiteral("%1 nodes=%2 steps=%3 folded=%4 weight=%5MB act=%6MB peak=%7%8")
        .arg(QFileInfo(path_).fileName())
        .arg(stats_.nodes)
        .arg(stats_.steps)
        .arg(stats_.folded)
        .arg(stats_.weightMb)
        .arg(stats_.actMb)
        .arg(stats_.peakTensors)
        .arg(extra);
}

namespace {

// 把 initializer 读成 float 向量。int64/int32 也一并转成 float：
// 这些模型里 int 张量只有形状与 pads 两种用途，统一当 float 处理能让折叠逻辑只写一遍。
bool InitFloats(const vk::OnnxTensor &t, std::vector<float> *out, QString *why)
{    const qint64 n = t.count();
    if (n <= 0) {
        if (why)
            *why = QStringLiteral("initializer %1 元素数为 0").arg(t.name);
        return false;
    }
    out->assign(size_t(n), 0.0f);
    if (t.dtype == vk::kFloat) {
        if (qint64(t.raw.size()) < n * 4) {
            if (why)
                *why = QStringLiteral("initializer %1 数据不足（%2 < %3）")
                           .arg(t.name)
                           .arg(t.raw.size())
                           .arg(n * 4);
            return false;
        }
        std::memcpy(out->data(), t.raw.constData(), size_t(n) * sizeof(float));
        return true;
    }
    if (t.dtype == vk::kInt64) {
        if (t.ints.size() >= n) {
            for (qint64 i = 0; i < n; ++i)
                (*out)[size_t(i)] = float(t.ints[int(i)]);
            return true;
        }
        if (qint64(t.raw.size()) >= n * 8) {
            const qint64 *p = reinterpret_cast<const qint64 *>(t.raw.constData());
            for (qint64 i = 0; i < n; ++i)
                (*out)[size_t(i)] = float(p[i]);
            return true;
        }
    }
    if (t.dtype == vk::kInt32) {
        if (t.ints.size() >= n) {
            for (qint64 i = 0; i < n; ++i)
                (*out)[size_t(i)] = float(t.ints[int(i)]);
            return true;
        }
        if (qint64(t.raw.size()) >= n * 4) {
            const qint32 *p = reinterpret_cast<const qint32 *>(t.raw.constData());
            for (qint64 i = 0; i < n; ++i)
                (*out)[size_t(i)] = float(p[i]);
            return true;
        }
    }
    if (why)
        *why = QStringLiteral("initializer %1 的 dtype=%2 暂不支持").arg(t.name).arg(t.dtype);
    return false;
}

// 回读一个设备张量到 host fp32（fp16 存储时解包）。dump/NaN 扫描等诊断路径共用。
bool ReadTensorHost(const TensorInfo &t, std::vector<float> *out, bool fp16)
{
    out->assign(size_t(t.d.count()), 0.0f);
    if (fp16) {
        std::vector<quint16> h(size_t(t.d.count()), 0);
        if (!vk::Device::I().download(t.buf, h.data(), quint64(h.size()) * 2))
            return false;
        for (size_t k = 0; k < h.size(); ++k)
            (*out)[k] = vk::HalfToFloat(h[k]);
        return true;
    }
    return vk::Device::I().download(t.buf, out->data(), quint64(out->size()) * 4);
}

// 分批提交阈值（累计工作组数）。移动/集成 GPU 的驱动 watchdog（Adreno 实测
// ~4s）按单次 submit 计时：x4plus tile128 整图 1714 步一个 submit ≈7s 必然
// 触发 DEVICE_LOST。NCNN（net.cpp）同思路按 rough_score 分档拆 submit，我们
// 按设备类型取档：桌面独显 8M（提交开销占比 <2%），其余 256K（≈25~200ms/批）。
// JM_VK_BATCH_WG=0 关闭分批（排障用），>0 显式指定。
quint64 BatchThresholdWg()
{
    bool ok = false;
    const qint64 v = qEnvironmentVariableIntValue("JM_VK_BATCH_WG", &ok);
    if (ok)
        return v > 0 ? quint64(v) : 0;
#ifdef Q_OS_ANDROID
    return 256ull * 1024;
#else
    const vk::Device &dev = vk::Device::I();
    return dev.valid() && dev.info().type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
               ? 8ull * 1024 * 1024
               : 256ull * 1024;
#endif
}

}  // namespace

// Winograd F(2,3) 权重预变换：[OC][IC][3][3] fp32 -> [OC][IC][4][4]。
//   U[i][j] = Σ_{a,b} G[i][a] · w[a][b] · G[j][b]
// G 的四行对应 4 个变换域滤波器（点 / 一阶 / 二阶 / 边），常数与 convw.comp
// 里的注释一致；数值正确性由 --vk-conv-test 的 winograd 用例兜底验证。
// 放在 SrVk 命名空间（非匿名）：sr_vk.cpp 的 convw 单算子自检复用同一份变换。
void WinogradTransformWeights(const float *w, int oc, int ic, std::vector<float> *out)
{
    static constexpr float kG[4][3] = {
        {1.0f, 0.0f, 0.0f},
        {0.5f, 0.5f, 0.5f},
        {0.5f, -0.5f, 0.5f},
        {0.0f, 0.0f, 1.0f},
    };
    out->assign(size_t(oc) * ic * 16, 0.0f);
    for (int o = 0; o < oc; ++o) {
        for (int c = 0; c < ic; ++c) {
            const float *src = w + (size_t(o) * ic + c) * 9;
            float tmp[4][3];
            for (int i = 0; i < 4; ++i)
                for (int b = 0; b < 3; ++b)
                    tmp[i][b] = kG[i][0] * src[0 * 3 + b] + kG[i][1] * src[1 * 3 + b]
                                + kG[i][2] * src[2 * 3 + b];
            float *dst = out->data() + (size_t(o) * ic + c) * 16;
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    dst[i * 4 + j] = tmp[i][0] * kG[j][0] + tmp[i][1] * kG[j][1]
                                     + tmp[i][2] * kG[j][2];
        }
    }
}

bool Graph::prepare(int tileW, int tileH, QString *error)
{
    auto fail = [&](const QString &msg) {
        err_ = msg;
        if (error)
            *error = msg;
        return false;
    };
    if (!loaded_)
        return fail(QStringLiteral("图未加载"));
    if (tileW <= 0 || tileH <= 0)
        return fail(QStringLiteral("tile 尺寸非法"));
    if (tileW != tileH)
        return fail(QStringLiteral("只支持正方形 tile（当前 %1x%2）").arg(tileW).arg(tileH));

    tileW_ = tileW;
    tileH_ = tileH;
    stats_.nodes = int(d_->model.nodes.size());
    stats_.fp16 = false;
    stats_.winoLayers = 0;

    // 存储格式：设备能力（shaderFloat16 + 16bit storage）齐备即用 fp16 存储
    //（NCNN fp16_packing：显存 half、shader 内 unpack 成 fp32 累加）。
    // JM_VK_FP16=0 可显式关闭。
    d_->fp16 = vk::Device::I().fp16Usable();
    d_->streamReady = false;
    for (TensorInfo &t : d_->tensors)
        t.wino = false;

    // ---- 输入张量
    {
        TensorInfo t;
        t.name = d_->model.inputs.first();
        t.d = Dims{1, 3, tileH, tileW};
        t.defIndex = -1;
        d_->inTensor = d_->add(t);
    }

    QString foldErr;

    // ---- 逐节点：折叠 or 建步
    for (int ni = 0; ni < int(d_->model.nodes.size()); ++ni) {
        const vk::OnnxNode &node = d_->model.nodes.at(ni);
        const QString op = node.op;

        // 收集输入张量（initializer 按需注册为常量）
        QVector<int> ins;
        for (const QString &name : node.inputs) {
            if (name.isEmpty()) {
                ins.append(-1);
                continue;
            }
            int idx = d_->find(name);
            if (idx < 0) {
                auto it = d_->model.inits.constFind(name);
                if (it != d_->model.inits.constEnd()) {
                    TensorInfo t;
                    t.name = name;
                    t.d = FromOnnxDims(it->dims);
                    t.isConst = true;
                    t.fromInit = true;
                    t.defIndex = -1000000;   // 常量最先分配
                    idx = d_->add(t);
                } else {
                    return fail(QStringLiteral("节点 %1(%2) 的输入 %3 既不在图中也不是常量")
                                    .arg(ni)
                                    .arg(op, name));
                }
            }
            ins.append(idx);
        }

        // ---- 常量折叠
        if (FoldableOp(op)) {
            bool allConst = !ins.isEmpty();
            for (int i : ins) {
                if (i < 0 || !d_->tensors[i].isConst)
                    allConst = false;
            }
            // Constant 节点没有输入（值在属性里），也必须进折叠；
            // 其余折叠算子要求所有输入都已是常量。Concat 只在全常量时折叠，
            // 否则交给 GPU 的 copy 分段（RDB 里的多路 Concat 都是动态张量）。
            if (op == QLatin1String("Constant") || (allConst && !node.inputs.isEmpty())) {
                HTensor result;
                auto hostOf = [&](int i) {
                    HTensor h;
                    const TensorInfo &t = d_->tensors[i];
                    h.dims = {t.d.n, t.d.c, t.d.h, t.d.w};
                    if (t.hostOwned) {
                        h.v = t.host;
                    } else {
                        auto it = d_->model.inits.constFind(t.name);
                        if (it != d_->model.inits.constEnd())
                            InitFloats(*it, &h.v, nullptr);
                    }
                    return h;
                };
                bool ok = true;
                if (op == QLatin1String("Constant")) {
                    const vk::OnnxAttr *a = node.find(QStringLiteral("value"));
                    if (a && a->hasT) {
                        const qint64 n = a->t.count();
                        result.dims.clear();
                        for (qint64 dd : a->t.dims)
                            result.dims.append(dd <= 0 ? 1 : int(dd));
                        if (result.dims.isEmpty())
                            result.dims.append(1);
                        if (a->t.dtype == vk::kFloat) {
                            result.v.resize(size_t(n));
                            std::memcpy(result.v.data(), a->t.raw.constData(),
                                        size_t(n) * sizeof(float));
                        } else if (a->t.dtype == vk::kInt64) {
                            result.v.resize(size_t(n));
                            for (int k = 0; k < n; ++k)
                                result.v[size_t(k)] = float(a->t.ints.value(k));
                        } else {
                            ok = false;
                        }
                    } else {
                        ok = false;
                    }
                } else if (op == QLatin1String("ConstantOfShape")) {
                    HTensor shape = hostOf(ins[0]);
                    result.dims.clear();
                    for (float f : shape.v) {
                        int d = int(f);
                        // 导出器会把"未知维"写成 -1；这些模型里它就是 1
                        result.dims.append(d <= 0 ? 1 : d);
                    }
                    const vk::OnnxAttr *a = node.find(QStringLiteral("value"));
                    float fill = 0.0f;
                    if (a && a->hasT && a->t.dtype == vk::kFloat && a->t.raw.size() >= 4)
                        std::memcpy(&fill, a->t.raw.constData(), 4);
                    qint64 n = 1;
                    for (int dd : result.dims)
                        n *= dd;
                    result.v.assign(size_t(n), fill);
                } else if (op == QLatin1String("Equal")) {
                    HTensor a = hostOf(ins[0]), b = hostOf(ins[1]);
                    QVector<int> od;
                    if (!BroadcastDims(a.dims, b.dims, &od)) {
                        ok = false;
                    } else {
                        result.dims = od;
                        qint64 total = 1;
                        for (int dd : od)
                            total *= dd;
                        const QVector<int> aa = AlignDims(a.dims, od.size());
                        const QVector<int> bb = AlignDims(b.dims, od.size());
                        result.v.resize(size_t(total));
                        for (qint64 i = 0; i < total; ++i) {
                            QVector<int> idx;
                            Unravel(i, od, &idx);
                            const float va = a.v[size_t(SrcOffset(aa, idx))];
                            const float vb = b.v[size_t(SrcOffset(bb, idx))];
                            result.v[size_t(i)] = (va == vb) ? 1.0f : 0.0f;
                        }
                    }
                } else if (op == QLatin1String("Where")) {
                    HTensor c = hostOf(ins[0]), x = hostOf(ins[1]), y = hostOf(ins[2]);
                    QVector<int> od;
                    if (!BroadcastDims(c.dims, x.dims, &od) || !BroadcastDims(od, y.dims, &od)) {
                        ok = false;
                    } else {
                        result.dims = od;
                        qint64 total = 1;
                        for (int dd : od)
                            total *= dd;
                        const QVector<int> cc = AlignDims(c.dims, od.size());
                        const QVector<int> xx = AlignDims(x.dims, od.size());
                        const QVector<int> yy = AlignDims(y.dims, od.size());
                        result.v.resize(size_t(total));
                        for (qint64 i = 0; i < total; ++i) {
                            QVector<int> idx;
                            Unravel(i, od, &idx);
                            const float cond = c.v[size_t(SrcOffset(cc, idx))];
                            result.v[size_t(i)] = cond != 0.0f
                                                      ? x.v[size_t(SrcOffset(xx, idx))]
                                                      : y.v[size_t(SrcOffset(yy, idx))];
                        }
                    }
                } else if (op == QLatin1String("Expand")) {
                    HTensor a = hostOf(ins[0]), shape = hostOf(ins[1]);
                    QVector<int> target;
                    for (float f : shape.v)
                        target.append(int(f) <= 0 ? 1 : int(f));
                    QVector<int> od;
                    // Expand 的语义是"把输入广播到 target"，目标形状可能是
                    // [1,1,192,192] 而输入是 [1,15,1,1]：结果必须是 [1,15,192,192]。
                    // 第一版直接把 target 当输出形状，把 SRMD 的 15 张退化图压成 1 张，
                    // Concat 只有 5 个通道（应为 3+15+1=19），整网输出全错。
                    if (!BroadcastDims(a.dims, target, &od)) {
                        ok = false;
                    } else {
                        result.dims = od;
                        qint64 total = 1;
                        for (int dd : od)
                            total *= dd;
                        const QVector<int> aa = AlignDims(a.dims, od.size());
                        result.v.resize(size_t(total));
                        for (qint64 i = 0; i < total; ++i) {
                            QVector<int> idx;
                            Unravel(i, od, &idx);
                            result.v[size_t(i)] = a.v[size_t(SrcOffset(aa, idx))];
                        }
                    }
                } else if (op == QLatin1String("Concat")) {
                    const int axis = int(node.intAttr(QStringLiteral("axis"), 1));
                    QVector<HTensor> parts;
                    for (int i : ins) {
                        parts.append(hostOf(i));
                        if (!parts.last().ok())
                            ok = false;
                    }
                    if (ok) {
                        result.dims = parts.first().dims;
                        int sum = 0;
                        for (const HTensor &p : parts)
                            sum += p.dims.value(axis, 0);
                        result.dims[axis] = sum;
                        qint64 total = 1;
                        for (int dd : result.dims)
                            total *= dd;
                        result.v.assign(size_t(total), 0.0f);
                        int axisOff = 0;
                        for (const HTensor &p : parts) {
                            const qint64 pTotal = p.count();
                            for (qint64 i = 0; i < pTotal; ++i) {
                                QVector<int> idx;
                                Unravel(i, p.dims, &idx);
                                idx[axis] += axisOff;
                                const qint64 dst = [&] {
                                    qint64 o = 0;
                                    for (int k = 0; k < result.dims.size(); ++k)
                                        o = o * result.dims[k] + idx[k];
                                    return o;
                                }();
                                result.v[size_t(dst)] = p.v[size_t(i)];
                            }
                            axisOff += p.dims.value(axis, 0);
                        }
                    }
                } else {   // Mul / Add / Sub / Div
                    HTensor a = hostOf(ins[0]), b = hostOf(ins[1]);
                    QVector<int> od;
                    if (!BroadcastDims(a.dims, b.dims, &od)) {
                        ok = false;
                    } else {
                        result.dims = od;
                        qint64 total = 1;
                        for (int dd : od)
                            total *= dd;
                        const QVector<int> aa = AlignDims(a.dims, od.size());
                        const QVector<int> bb = AlignDims(b.dims, od.size());
                        result.v.resize(size_t(total));
                        for (qint64 i = 0; i < total; ++i) {
                            QVector<int> idx;
                            Unravel(i, od, &idx);
                            const float va = a.v[size_t(SrcOffset(aa, idx))];
                            const float vb = b.v[size_t(SrcOffset(bb, idx))];
                            float r = va + vb;
                            if (op == QLatin1String("Mul"))
                                r = va * vb;
                            else if (op == QLatin1String("Sub"))
                                r = va - vb;
                            else if (op == QLatin1String("Div"))
                                r = vb != 0.0f ? va / vb : 0.0f;
                            result.v[size_t(i)] = r;
                        }
                    }
                }
                if (ok && node.outputs.size() == 1) {
                    TensorInfo t;
                    t.name = node.outputs.first();
                    t.isConst = true;
                    t.hostOwned = true;
                    t.host = result.v;
                    if (result.dims.size() == 4)
                        t.d = Dims{result.dims[0], result.dims[1], result.dims[2],
                                   result.dims[3]};
                    else if (result.dims.size() == 3)
                        t.d = Dims{1, result.dims[0], result.dims[1], result.dims[2]};
                    else if (result.dims.size() == 2)
                        t.d = Dims{1, result.dims[0], 1, result.dims[1]};
                    else if (result.dims.size() == 1)
                        t.d = Dims{1, result.dims[0], 1, 1};
                    else
                        t.d = Dims{1, 1, 1, 1};
                    // 4 维常量才可能是特征图，其余按"形状/标量"处理，不占显存
                    t.defIndex = -900000;
                    d_->add(t);
                    ++stats_.folded;
                    continue;
                }
                // 折叠失败就当普通节点处理（会给出手写 GPU 路径或报错）
            }
        }

        // ---- 形状推断
        Dims a = ins.isEmpty() ? Dims{} : d_->tensors[ins[0]].d;
        Dims o;
        auto inDims = [&](int k) {
            return (k < ins.size() && ins[k] >= 0) ? d_->tensors[ins[k]].d : Dims{};
        };
        auto constFloats = [&](int k, std::vector<float> *out) {
            if (k >= ins.size() || ins[k] < 0)
                return false;
            const TensorInfo &t = d_->tensors[ins[k]];
            if (t.hostOwned) {
                *out = t.host;
                return true;
            }
            auto it = d_->model.inits.constFind(t.name);
            if (it == d_->model.inits.constEnd())
                return false;
            return InitFloats(*it, out, nullptr);
        };

        if (op == QLatin1String("Conv")) {
            const Dims w = inDims(1);
            const QVector<qint64> ks = node.intsAttr(QStringLiteral("kernel_shape"));
            const int KH = ks.size() >= 2 ? int(ks[0]) : (ks.size() == 1 ? int(ks[0]) : 3);
            const int KW = ks.size() >= 2 ? int(ks[1]) : KH;
            const QVector<qint64> st = node.intsAttr(QStringLiteral("strides"), {1, 1});
            const QVector<qint64> pd = node.intsAttr(QStringLiteral("pads"), {0, 0, 0, 0});
            const int sx = int(st.value(0, 1)), sy = int(st.value(1, 1));
            const int px0 = int(pd.value(0, 0)), px1 = int(pd.value(2, 0));
            const int py0 = int(pd.value(1, 0)), py1 = int(pd.value(3, 0));
            // ---- Winograd F(2,3) 判定（通道阈值门控：C>=16 且 OC>=16）----
            // 条件：stride1、3x3 核、对称 pad、常量权重。变换把 3x3 滤波器变成
            // 4 组 3x3（元素 9 -> 16，显存 x1.78），换取 MAC 减半与读带宽减半。
            // 3 通道输入层与小通道层变换开销占比高，保持直接卷积。
            // JM_VK_WINO=0 全局禁用（排障时把 winograd 从变量里去掉）。
            TensorInfo *wT = nullptr;
            bool wino = false;
            const bool winoDisabled = qEnvironmentVariableIntValue("JM_VK_WINO") == 0;
            if (!winoDisabled && ins.size() >= 2 && ins[1] >= 0) {
                wT = &d_->tensors[ins[1]];
                wino = wT->isConst && wT->d.n == w.n && sx == 1 && sy == 1 && KH == 3 && KW == 3
                       && px0 == px1 && py0 == py1 && a.c >= 16 && w.n >= 16;
            }
            if (wino) {
                if (wT->wino) {
                    // 已被标记（共享权重且全走 wino）：共用同一份变换结果
                } else if (wT->d.h == 3 && wT->d.w == 3) {
                    wT->wino = true;
                    // 权重形状改写为 [OC][IC][4][4]：分配/上传尺寸全部按 16 元素/对
                    wT->d = Dims{w.n, w.c, 4, 4};
                    ++stats_.winoLayers;
                } else {
                    // 尺寸已被改写过但当前层判定失败（理论不可达，防御）
                    wino = false;
                }
            } else if (wT && wT->wino) {
                return fail(QStringLiteral("节点 %1(%2)：权重 %3 已做 Winograd 变换但本层"
                                           "不满足条件（权重共享且判定冲突），暂不支持")
                                .arg(ni).arg(op, wT->name));
            }
            // 权重是 [OC, IC, KH, KW]：在 Dims 里 OC 落在 n 上（不是 c）
            o = Dims{1, w.n, (a.h + py0 + py1 - KH) / sy + 1, (a.w + px0 + px1 - KW) / sx + 1};
            Step s;
            s.shader = wino ? d_->shaderName("convw") : d_->shaderName("conv");
            s.op = wino ? QStringLiteral("Conv+wino") : op;
            s.pc.set(0, a.c);
            s.pc.set(1, a.h);
            s.pc.set(2, a.w);
            s.pc.set(3, w.n);
            s.pc.set(4, KH);
            s.pc.set(5, KW);
            s.pc.set(6, sx);
            s.pc.set(7, sy);
            s.pc.set(8, px0);
            s.pc.set(9, py0);
            s.pc.set(10, o.h);
            s.pc.set(11, o.w);
            s.pc.set(12, ins.size() >= 3 && ins[2] >= 0 ? 1 : 0);
            s.in[0] = ins.value(0, -1);
            s.in[1] = ins.value(1, -1);
            s.in[2] = (ins.size() >= 3 && ins[2] >= 0) ? ins[2] : s.in[1];
            s.nIn = 3;
            if (wino) {
                // convw 的线程网格按输出 tile 排（每线程 2x2）
                s.gx = quint32(((o.w + 1) / 2 + 63) / 64);
                s.gy = quint32((o.h + 1) / 2);
                s.gz = quint32(w.n);
            } else {
                s.gx = quint32((o.w + 255) / 256);
                s.gy = quint32(o.h);
                s.gz = quint32(w.n);
            }
            s.outName = node.outputs.value(0);
            d_->steps.append(s);
        } else if (op == QLatin1String("ConvTranspose")) {
            const Dims w = inDims(1);
            const QVector<qint64> ks = node.intsAttr(QStringLiteral("kernel_shape"));
            const int KH = int(ks.value(0, 2)), KW = int(ks.value(1, KH));
            const QVector<qint64> st = node.intsAttr(QStringLiteral("strides"), {1, 1});
            const QVector<qint64> pd = node.intsAttr(QStringLiteral("pads"), {0, 0, 0, 0});
            const QVector<qint64> op2 = node.intsAttr(QStringLiteral("output_padding"), {0, 0});
            const int sx = int(st.value(0, 1)), sy = int(st.value(1, 1));
            const int px0 = int(pd.value(0, 0)), px1 = int(pd.value(2, 0));
            const int py0 = int(pd.value(1, 0)), py1 = int(pd.value(3, 0));
            // 权重是 [IC, OC, KH, KW]：outC 落在 c 上
            o = Dims{1, w.c, (a.h - 1) * sy - py0 - py1 + KH + int(op2.value(0, 0)),
                     (a.w - 1) * sx - px0 - px1 + KW + int(op2.value(1, 0))};
            Step s;
            s.shader = d_->shaderName("deconv");
            s.op = op;
            s.pc.set(0, a.c);
            s.pc.set(1, a.h);
            s.pc.set(2, a.w);
            s.pc.set(3, w.c);
            s.pc.set(4, KH);
            s.pc.set(5, KW);
            s.pc.set(6, sx);
            s.pc.set(7, sy);
            s.pc.set(8, px0);
            s.pc.set(9, py0);
            s.pc.set(10, o.h);
            s.pc.set(11, o.w);
            s.pc.set(12, ins.size() >= 3 && ins[2] >= 0 ? 1 : 0);
            s.in[0] = ins.value(0, -1);
            s.in[1] = ins.value(1, -1);
            s.in[2] = (ins.size() >= 3 && ins[2] >= 0) ? ins[2] : s.in[1];
            s.nIn = 3;
            s.gx = quint32((o.w + 255) / 256);
            s.gy = quint32(o.h);
            s.gz = quint32(w.c);
            s.outName = node.outputs.value(0);
            d_->steps.append(s);
        } else if (op == QLatin1String("Relu") || op == QLatin1String("LeakyRelu")
                   || op == QLatin1String("Sigmoid") || op == QLatin1String("PRelu")
                   || op == QLatin1String("Clip")) {
            o = a;
            Step s;
            s.shader = d_->shaderName("act");
            s.op = op;
            const qint64 n = o.count();
            s.pc.set(0, int(n));
            int mode = 0;
            int alphaBits = 0;
            int lo = 0, hi = 0;
            if (op == QLatin1String("LeakyRelu")) {
                mode = 1;
                const vk::OnnxAttr *at = node.find(QStringLiteral("alpha"));
                const float av = at ? at->f : 0.01f;
                std::memcpy(&alphaBits, &av, 4);
            } else if (op == QLatin1String("Sigmoid")) {
                mode = 2;
            } else if (op == QLatin1String("PRelu")) {
                mode = 3;
            } else if (op == QLatin1String("Clip")) {
                mode = 4;
                float l = -3.4e38f, h = 3.4e38f;
                const vk::OnnxAttr *an = node.find(QStringLiteral("min"));
                const vk::OnnxAttr *ax = node.find(QStringLiteral("max"));
                if (an)
                    l = an->f;
                if (ax)
                    h = ax->f;
                if (ins.size() >= 2 && ins[1] >= 0) {
                    std::vector<float> v;
                    if (constFloats(1, &v) && !v.empty())
                        l = v[0];
                }
                if (ins.size() >= 3 && ins[2] >= 0) {
                    std::vector<float> v;
                    if (constFloats(2, &v) && !v.empty())
                        h = v[0];
                }
                std::memcpy(&lo, &l, 4);
                std::memcpy(&hi, &h, 4);
            }
            s.pc.set(1, mode);
            s.pc.set(2, o.c);
            s.pc.set(3, o.h * o.w);
            s.pc.set(4, alphaBits);
            s.pc.set(5, lo);
            s.pc.set(6, hi);
            s.in[0] = ins.value(0, -1);
            s.in[1] = (mode == 3 && ins.size() >= 2) ? ins[1] : ins.value(0, -1);
            s.nIn = 2;
            s.gx = quint32((n + 63) / 64);
            s.outName = node.outputs.value(0);
            d_->steps.append(s);
        } else if (op == QLatin1String("Add") || op == QLatin1String("Mul")
                   || op == QLatin1String("Sub") || op == QLatin1String("Div")) {
            const Dims b2 = inDims(1);
            QVector<int> od;
            const QVector<int> ad = {a.n, a.c, a.h, a.w};
            const QVector<int> bd = {b2.n, b2.c, b2.h, b2.w};
            if (!BroadcastDims(ad, bd, &od))
                return fail(QStringLiteral("节点 %1(%2) 形状无法广播：%3 vs %4")
                                .arg(ni)
                                .arg(op, a.str(), b2.str()));
            o = Dims{od[0], od[1], od[2], od[3]};
            Step s;
            s.shader = d_->shaderName("eltwise");
            s.op = op;
            const qint64 n = o.count();
            s.pc.set(0, int(n));
            s.pc.set(1, op == QLatin1String("Mul")   ? 1
                        : op == QLatin1String("Sub") ? 2
                        : op == QLatin1String("Div") ? 3
                                                     : 0);
            s.pc.set(2, o.c);
            s.pc.set(3, o.h);
            s.pc.set(4, o.w);
            auto pack = [&](int base, const Dims &t) {
                s.pc.set(base + 0, t.c != 1 || o.c == 1 ? 1 : 0);
                s.pc.set(base + 1, t.h != 1 || o.h == 1 ? 1 : 0);
                s.pc.set(base + 2, t.w != 1 || o.w == 1 ? 1 : 0);
                s.pc.set(base + 3, t.h);
                s.pc.set(base + 4, t.w);
            };
            pack(5, a);
            pack(10, b2);
            s.in[0] = ins.value(0, -1);
            s.in[1] = ins.value(1, -1);
            s.nIn = 2;
            s.gx = quint32((n + 63) / 64);
            s.outName = node.outputs.value(0);
            d_->steps.append(s);
        } else if (op == QLatin1String("Concat")) {
            const int axis = int(node.intAttr(QStringLiteral("axis"), 1));
            if (axis != 1)
                return fail(QStringLiteral("节点 %1 Concat 只支持 axis=1（实际 %2）")
                                .arg(ni)
                                .arg(axis));
            int sum = 0;
            for (int i : ins) {
                if (i < 0)
                    return fail(QStringLiteral("节点 %1 Concat 有空输入").arg(ni));
                sum += d_->tensors[i].d.c;
            }
            o = Dims{1, sum, a.h, a.w};
            // 每个输入一段 copy，目标通道偏移累加
            int chOff = 0;
            Step s;
            s.shader = d_->shaderName("copy");
            s.op = op;
            for (int i : ins) {
                const Dims t = d_->tensors[i].d;
                s.pc.set(0, int(t.count()));
                s.pc.set(1, chOff * t.h * t.w);
                Step one = s;
                one.in[0] = i;
                one.nIn = 1;
                one.gx = quint32((t.count() + 63) / 64);
                one.outName = node.outputs.value(0);
                // out 稍后统一回填
                d_->steps.append(one);
                chOff += t.c;
            }
            // 后续形状推断需要 a/o；step 已入列
            o = Dims{1, sum, a.h, a.w};
        } else if (op == QLatin1String("Pad")) {
            std::vector<float> pv;
            if (!constFloats(1, &pv))
                return fail(QStringLiteral("节点 %1 Pad 的 pads 不是常量").arg(ni));
            QVector<int> pads;
            for (float f : pv)
                pads.append(int(f));
            if (pads.size() != 8)
                return fail(QStringLiteral("节点 %1 Pad 的 pads 长度=%2（只支持 4 维）")
                                .arg(ni)
                                .arg(pads.size()));
            const int ph0 = pads[2], pw0 = pads[3];
            const int ph1 = pads[6], pw1 = pads[7];
            o = Dims{1, a.c, a.h + ph0 + ph1, a.w + pw0 + pw1};
            const QString mode = node.strAttr(QStringLiteral("mode"), QStringLiteral("constant"));
            std::vector<float> fill;
            float fillValue = 0.0f;
            if (ins.size() >= 3 && ins[2] >= 0 && constFloats(2, &fill) && !fill.empty())
                fillValue = fill[0];
            Step s;
            s.shader = d_->shaderName("pad");
            s.op = op;
            s.pc.set(0, a.c);
            s.pc.set(1, a.h);
            s.pc.set(2, a.w);
            s.pc.set(3, o.h);
            s.pc.set(4, o.w);
            s.pc.set(5, ph0);
            s.pc.set(6, pw0);
            s.pc.set(7, mode == QLatin1String("reflect") ? 1 : 0);
            int bits = 0;
            std::memcpy(&bits, &fillValue, 4);
            s.pc.set(8, bits);
            s.in[0] = ins.value(0, -1);
            s.nIn = 1;
            s.gx = quint32((qint64(o.c) * o.h * o.w + 63) / 64);
            s.outName = node.outputs.value(0);
            d_->steps.append(s);
        } else if (op == QLatin1String("DepthToSpace")) {
            const int b = int(node.intAttr(QStringLiteral("blocksize"), 2));
            const QString mode = node.strAttr(QStringLiteral("mode"), QStringLiteral("DCR"));
            if (mode != QLatin1String("CRD"))
                return fail(QStringLiteral("节点 %1 DepthToSpace 只支持 CRD（实际 %2）")
                                .arg(ni)
                                .arg(mode));
            o = Dims{1, a.c / (b * b), a.h * b, a.w * b};
            Step s;
            s.shader = d_->shaderName("d2s");
            s.op = op;
            s.pc.set(0, o.c);
            s.pc.set(1, a.h);
            s.pc.set(2, a.w);
            s.pc.set(3, b);
            s.pc.set(4, o.h);
            s.pc.set(5, o.w);
            s.in[0] = ins.value(0, -1);
            s.nIn = 1;
            s.gx = quint32((o.count() + 63) / 64);
            s.outName = node.outputs.value(0);
            d_->steps.append(s);
        } else if (op == QLatin1String("GlobalAveragePool") || op == QLatin1String("ReduceMean")) {
            o = Dims{1, a.c, 1, 1};
            Step s;
            s.shader = d_->shaderName("reduce");
            s.op = op;
            s.pc.set(0, a.c);
            s.pc.set(1, a.h);
            s.pc.set(2, a.w);
            s.in[0] = ins.value(0, -1);
            s.nIn = 1;
            s.gx = quint32(a.c);
            s.outName = node.outputs.value(0);
            d_->steps.append(s);
        } else if (op == QLatin1String("Resize")) {
            std::vector<float> scales, sizes;
            bool haveScales = ins.size() >= 3 && ins[2] >= 0 && constFloats(2, &scales)
                              && scales.size() == 4;
            bool haveSizes = ins.size() >= 4 && ins[3] >= 0 && constFloats(3, &sizes)
                             && sizes.size() == 4;
            if (haveScales) {
                o = Dims{1, int(std::lround(a.c * scales[1])),
                         int(std::lround(a.h * scales[2])), int(std::lround(a.w * scales[3]))};
            } else if (haveSizes) {
                o = Dims{1, int(sizes[1]), int(sizes[2]), int(sizes[3])};
            } else {
                return fail(QStringLiteral("节点 %1 Resize 既没有常量 scales 也没有 sizes").arg(ni));
            }
            const QString m = node.strAttr(QStringLiteral("mode"), QStringLiteral("nearest"));
            const QString cm = node.strAttr(QStringLiteral("coordinate_transformation_mode"),
                                           QStringLiteral("half_pixel"));
            const QString nm = node.strAttr(QStringLiteral("nearest_mode"), QStringLiteral("floor"));
            int mode = 0;
            if (m == QLatin1String("linear")) {
                if (cm != QLatin1String("half_pixel"))
                    return fail(QStringLiteral("节点 %1 Resize: linear 只实现 half_pixel（实际 %2）")
                                    .arg(ni)
                                    .arg(cm));
                mode = 1;
            } else {
                if (cm != QLatin1String("asymmetric") || nm != QLatin1String("floor"))
                    return fail(QStringLiteral("节点 %1 Resize: nearest 只实现 asymmetric+floor").arg(ni));
                mode = 0;
            }
            Step s;
            s.shader = d_->shaderName("resize");
            s.op = op;
            s.pc.set(0, a.c);
            s.pc.set(1, a.h);
            s.pc.set(2, a.w);
            s.pc.set(3, o.h);
            s.pc.set(4, o.w);
            s.pc.set(5, mode);
            s.in[0] = ins.value(0, -1);
            s.nIn = 1;
            s.gx = quint32((o.count() + 63) / 64);
            s.outName = node.outputs.value(0);
            d_->steps.append(s);
        } else if (op == QLatin1String("Identity") || op == QLatin1String("Flatten")
                   || op == QLatin1String("Reshape")) {
            o = a;
            Step s;
            s.shader = d_->shaderName("copy");
            s.op = op;
            s.pc.set(0, int(a.count()));
            s.pc.set(1, 0);
            s.in[0] = ins.value(0, -1);
            s.nIn = 1;
            s.gx = quint32((a.count() + 63) / 64);
            s.outName = node.outputs.value(0);
            d_->steps.append(s);
        } else {
            return fail(QStringLiteral("节点 %1 的算子 %2 尚未实现（Vulkan 后端）")
                            .arg(ni)
                            .arg(op));
        }

        if (!o.valid())
            return fail(QStringLiteral("节点 %1(%2) 输出形状非法：%3").arg(ni).arg(op, o.str()));

        // 注册输出张量（同名可能已有折叠常量，覆盖即错）
        const QString outName = node.outputs.value(0);
        if (outName.isEmpty())
            return fail(QStringLiteral("节点 %1(%2) 没有输出名").arg(ni).arg(op));
        if (d_->find(outName) >= 0 && !d_->tensors[d_->find(outName)].isConst)
            return fail(QStringLiteral("张量 %1 被重复定义").arg(outName));
        TensorInfo t;
        t.name = outName;
        t.d = o;
        const int idx = d_->add(t);
        // 把刚才追加的步骤（可能多条，Concat 会拆成多段）绑到该输出
        for (int k = d_->steps.size() - 1; k >= 0; --k) {
            if (d_->steps[k].out >= 0)
                break;
            d_->steps[k].out = idx;
        }
    }

    // ---- 输出张量
    d_->outTensor = d_->find(d_->model.outputs.first());
    if (d_->outTensor < 0)
        return fail(QStringLiteral("找不到图输出 %1").arg(d_->model.outputs.first()));
    outH_ = d_->tensors[d_->outTensor].d.h;
    outW_ = d_->tensors[d_->outTensor].d.w;
    {
        const double s = double(outH_) / double(tileH_);
        scale_ = qMax(1, int(std::lround(s)));
    }
    // ---- 算子融合：Conv -> Relu / LeakyRelu / Sigmoid
    // 每个 conv 后基本都跟一个逐元素激活。融合后少一次 dispatch、一次全图
    // barrier 和一整张中间张量的显存往返；数值路径不变（fp32 逐元素同样公式）。
    // 只处理"conv 输出仅被该 act 消费"，PRelu（需 slope 绑定）与 Clip（需 lo/hi
    // 两个额外参数，push constant 槽位不够）跳过，保持正确性优先。
    int fused = 0;
    for (int i = 0; i + 1 < d_->steps.size();) {
        const Step a = d_->steps.at(i + 1);
        const int mode = a.pc.i[1];
        // conv 与 deconv 都支持融合激活（deconv 覆盖 waifu2x / Real-CUGAN
        // 上采样段的 78 处 ConvTranspose+LeakyRelu，省一张上采样后的大张量往返）
        const bool fusableShader = d_->steps[i].shader == QLatin1String("conv")
                                   || d_->steps[i].shader == QLatin1String("deconv");
        bool canFuse = fusableShader && a.shader == QLatin1String("act")
                       && a.in[0] == d_->steps[i].out && d_->steps[i].out != d_->outTensor
                       && mode != 3 && mode != 4;
        for (int j = 0; j < d_->steps.size() && canFuse; ++j) {
            if (j == i + 1)
                continue;
            for (int k = 0; k < d_->steps[j].nIn; ++k) {
                if (d_->steps[j].in[k] == d_->steps[i].out) {
                    canFuse = false;
                    break;
                }
            }
        }
        if (!canFuse) {
            ++i;
            continue;
        }
        d_->steps[i].pc.set(13, mode + 1);
        d_->steps[i].pc.set(14, a.pc.i[4]);   // LeakyRelu 的 alpha 位
        d_->steps[i].out = a.out;             // 直接写激活输出，下游不变
        d_->steps[i].outName = a.outName;
        d_->steps[i].op += QStringLiteral("+act");
        d_->steps.removeAt(i + 1);
        ++fused;
        ++i;
    }
    stats_.folded += fused;
    stats_.steps = d_->steps.size();

    // ---- 活跃期与显存分配
    for (int si = 0; si < d_->steps.size(); ++si) {
        for (int k = 0; k < d_->steps[si].nIn; ++k) {
            const int ti = d_->steps[si].in[k];
            if (ti >= 0)
                d_->tensors[ti].lastUse = si;
        }
    }
    for (int si = 0; si < d_->steps.size(); ++si) {
        const int ti = d_->steps[si].out;
        if (ti >= 0 && d_->tensors[ti].defIndex < 0)
            d_->tensors[ti].defIndex = si;
    }
    d_->tensors[d_->inTensor].defIndex = -1;

    // 只把"确实被某个 GPU 算子绑定"的常量放进显存。
    // 不能按元素个数判断：SE block 的 conv1 bias 恰好是 8 个 float，
    // 早期 `count > 8 才上传` 会把它漏掉，shader 读到未初始化显存（首个发散点 t11）。
    QSet<int> gpuInputs;
    for (const Step &st : d_->steps) {
        for (int k = 0; k < st.nIn; ++k) {
            if (st.in[k] >= 0 && d_->tensors[st.in[k]].isConst)
                gpuInputs.insert(st.in[k]);
        }
    }

    // 先算权重总量，按实际大小调整分区，避免"权重区固定占一半"把大激活模型挡掉。
    // wino 权重的 d 已改写为 [OC][IC][4][4]，count 即变换后元素数。
    {
        qint64 need = 0;
        for (int i = 0; i < d_->tensors.size(); ++i) {
            const TensorInfo &t = d_->tensors[i];
            if (!t.isConst || !gpuInputs.contains(i))
                continue;
            need += t.d.count() * d_->elemBytes();
        }
        vk::Device::I().setWeightReserve(quint64(need));
    }

    // 权重（initializer）与折叠常量走 persistent 区，激活走一次性池
    qint64 weightBytes = 0;
    QVector<int> actList;
    for (int i = 0; i < d_->tensors.size(); ++i) {
        TensorInfo &t = d_->tensors[i];
        if (t.isConst) {
            if (!gpuInputs.contains(i))
                continue;
            const qint64 bytes = t.d.count() * d_->elemBytes();
            if (bytes <= 0)
                return fail(QStringLiteral("张量 %1 尺寸为 0（%2）").arg(t.name, t.d.str()));
            t.buf = vk::Device::I().allocPersistent(quint64(bytes));
            if (!t.buf.valid())
                return fail(QStringLiteral("权重分配失败：%1（%2）")
                                .arg(t.name, vk::Device::I().lastError()));
            weightBytes += bytes;
            continue;
        }
        const qint64 bytes = t.d.count() * d_->elemBytes();
        if (bytes <= 0)
            return fail(QStringLiteral("张量 %1 尺寸为 0（%2）").arg(t.name, t.d.str()));
        actList.append(i);
    }
    stats_.weightMb = weightBytes / (1024 * 1024);

    // 流水双缓冲：激活池头部预留两份输入 + 两份输出（fp16 下减半），
    // 让"上传 tile k+1 输入"与"计算 tile k"在设备上重叠。激活张量整体
    // 偏移到 slot 区之后；resetActivations 只动 bump，不动这段固定区。
    const quint64 inBytes = quint64(3 * tileH_ * tileW_) * quint64(d_->elemBytes());
    const quint64 outBytes = quint64(d_->tensors[d_->outTensor].d.count())
                             * quint64(d_->elemBytes());
    const quint64 slotHead = ((2 * (inBytes + outBytes)) + 255) / 256 * 256;

    // 活跃期空闲块复用：按定义顺序分配，用完即还
    {
        struct Free { quint64 off; quint64 size; };
        QVector<Free> freeList;
        std::priority_queue<QPair<int, int>, std::vector<QPair<int, int>>,
                            std::greater<QPair<int, int>>>
            pending;   // (lastUse, tensorIndex)
        quint64 high = slotHead;
        QVector<quint64> offsets(d_->tensors.size(), 0);
        int live = 0;

        auto flush = [&](int upTo) {
            while (!pending.empty() && pending.top().first < upTo) {
                const int ti = pending.top().second;
                pending.pop();
                freeList.append(Free{offsets[ti],
                                     quint64(d_->tensors[ti].d.count()) * quint64(d_->elemBytes())});
                --live;
            }
        };

        // 一个输出张量只能分配一次：Concat 拆成 N 段 copy 时，N 个 step
        // 共享同一个 out 索引。重复分配会把同一块反复塞进空闲表，导致后续
        // 张量拿到重叠区间（x4plus 在 1509 步开始出现 NaN 就是这个原因）。
        QSet<int> allocated;
        {
            const int ti = d_->inTensor;
            offsets[ti] = slotHead;
            high = slotHead + quint64(d_->tensors[ti].d.count()) * quint64(d_->elemBytes());
            high = (high + 255) / 256 * 256;
            pending.push({d_->tensors[ti].lastUse, ti});
            allocated.insert(ti);
            ++live;
        }
        for (int si = 0; si < d_->steps.size(); ++si) {
            const int ti = d_->steps[si].out;
            if (ti < 0)
                continue;
            flush(si);
            if (allocated.contains(ti))
                continue;
            const quint64 need = quint64(d_->tensors[ti].d.count()) * quint64(d_->elemBytes());
            int best = -1;
            for (int k = 0; k < freeList.size(); ++k) {
                if (freeList[k].size >= need && (best < 0 || freeList[k].size < freeList[best].size))
                    best = k;
            }
            if (best >= 0) {
                offsets[ti] = freeList[best].off;
                if (freeList[best].size > need) {
                    freeList[best].off += need;
                    freeList[best].size -= need;
                } else {
                    freeList.remove(best);
                }
            } else {
                offsets[ti] = high;
                high += (need + 255) / 256 * 256;
            }
            pending.push({d_->tensors[ti].lastUse, ti});
            allocated.insert(ti);
            ++live;
            stats_.peakTensors = qMax(stats_.peakTensors, live);
        }
        d_->actBytes = qint64(high);
        stats_.actMb = qint64(high) / (1024 * 1024);

        d_->actPool = vk::Device::I().alloc(quint64(high));
        if (!d_->actPool.valid())
            return fail(QStringLiteral("激活区分配失败（需 %1MB）：%2")
                            .arg(quint64(high) / (1024 * 1024))
                            .arg(vk::Device::I().lastError()));
        for (int ti : actList) {
            TensorInfo &t = d_->tensors[ti];
            const quint64 bytes = quint64(t.d.count()) * quint64(d_->elemBytes());
            t.buf.buffer = d_->actPool.buffer;
            t.buf.offset = d_->actPool.offset + offsets[ti];
            t.buf.size = bytes;
        }
        // 流水 slot（激活池头部）：in 两份、out 两份
        for (int k = 0; k < 2; ++k) {
            d_->inSlot[k].buffer = d_->actPool.buffer;
            d_->inSlot[k].offset = d_->actPool.offset + k * inBytes;
            d_->inSlot[k].size = inBytes;
            d_->outSlot[k].buffer = d_->actPool.buffer;
            d_->outSlot[k].offset = d_->actPool.offset + 2 * inBytes + k * outBytes;
            d_->outSlot[k].size = outBytes;
        }
        d_->streamReady = true;
    }

    // ---- 分批提交切点（A 方案）：steps 里按 gx*gy*gz 累计，超阈值处记切点
    d_->batchCheckpoints.assign(size_t(d_->steps.size()), false);
    {
        const quint64 batchWg = BatchThresholdWg();
        quint64 acc = 0;
        for (int i = 0; i < d_->steps.size(); ++i) {
            const quint64 wg = quint64(d_->steps[i].gx) * d_->steps[i].gy * d_->steps[i].gz;
            if (batchWg > 0 && acc > 0 && acc + wg > batchWg) {
                d_->batchCheckpoints[size_t(i)] = true;
                acc = 0;
            }
            acc += wg;
        }
    }

    // ---- 上传常量
    {
        vk::Device &dev = vk::Device::I();
        bool passOpen = false;
        qint64 pendingBytes = 0;
        auto closePass = [&] {
            if (passOpen) {
                if (!dev.endPass())
                    return false;
                passOpen = false;
                // endPass 不再阻塞：staging 环复位前必须等在飞 copy 把数据读走，
                // 否则下一批权重上传会覆盖上一批还没拷完的 staging 段
                //（实测：权重 >24MB 分批上传的 x4plus / realcugan_pro 整图权重损坏）
                if (!dev.waitForAllPasses())
                    return false;
            }
            dev.resetStaging();
            pendingBytes = 0;
            return true;
        };
        for (int i = 0; i < d_->tensors.size(); ++i) {
            TensorInfo &t = d_->tensors[i];
            if (!t.isConst || !t.buf.valid())
                continue;
            const qint64 elems = t.d.count();   // wino 时 d 已改写为 [OC][IC][4][4]
            const quint64 bytes = quint64(elems) * quint64(d_->elemBytes());
            if (!passOpen) {
                if (!dev.beginPass())
                    return fail(QStringLiteral("权重上传 beginPass 失败：%1").arg(dev.lastError()));
                passOpen = true;
            }
            // host 侧统一转成 fp32 数组（折叠常量本来就是；initializer 可能是
            // int/raw），再按需做 Winograd 预变换与 fp16 打包。
            std::vector<float> f32;
            if (t.hostOwned) {
                if (t.wino) {
                    // 折叠常量一般不是 conv 权重（wino 只对 initializer 权重启用），
                    // 防御性处理：host 数据按原始 [OC][IC][3][3] 变换
                    if (qint64(t.host.size()) < qint64(t.d.n * t.d.c) * 9)
                        return fail(QStringLiteral("常量 %1 数据不足（wino）").arg(t.name));
                    const int oc = t.d.n;
                    const int ic = t.d.c;
                    WinogradTransformWeights(t.host.data(), oc, ic, &f32);
                } else {
                    f32 = t.host;
                }
            } else {
                auto it = d_->model.inits.constFind(t.name);
                if (it == d_->model.inits.constEnd())
                    return fail(QStringLiteral("常量 %1 找不到数据").arg(t.name));
                if (t.wino) {
                    std::vector<float> raw;
                    if (!InitFloats(*it, &raw, nullptr)
                        || qint64(raw.size()) < qint64(t.d.n * t.d.c) * 9)
                        return fail(QStringLiteral("常量 %1 数据不足（wino）").arg(t.name));
                    WinogradTransformWeights(raw.data(), t.d.n, t.d.c, &f32);
                } else if (it->dtype != vk::kFloat) {
                    // GPU 侧一律 fp32：int 常量（形状/scale）若真被绑定，先转 float
                    if (!InitFloats(*it, &f32, nullptr))
                        return fail(QStringLiteral("常量 %1 转换失败").arg(t.name));
                } else {
                    const quint64 rawBytes = quint64(it->count()) * 4;
                    if (quint64(it->raw.size()) < rawBytes)
                        return fail(QStringLiteral("常量 %1 数据不足").arg(t.name));
                    const float *pf = reinterpret_cast<const float *>(it->raw.constData());
                    f32.assign(pf, pf + it->count());
                }
            }
            if (t.wino && qint64(f32.size()) != elems)
                return fail(QStringLiteral("常量 %1 wino 变换后尺寸不符").arg(t.name));
            // fp16 打包（设备侧存储 half）
            const void *src = nullptr;
            std::vector<quint16> half;
            if (d_->fp16) {
                half.resize(f32.size());
                for (size_t k = 0; k < f32.size(); ++k)
                    half[k] = vk::FloatToHalf(f32[k]);
                src = half.data();
            } else {
                src = f32.data();
            }
            if (!dev.upload(t.buf, src, bytes))
                return fail(QStringLiteral("上传 %1 失败：%2").arg(t.name, dev.lastError()));
            pendingBytes += qint64(bytes);
            if (pendingBytes > 24 * 1024 * 1024) {
                if (!closePass())
                    return fail(QStringLiteral("权重上传 endPass 失败：%1").arg(dev.lastError()));
            }
        }
        if (!closePass())
            return fail(QStringLiteral("权重上传收尾失败：%1").arg(dev.lastError()));
        // 权重已进显存，主机侧副本可以放掉（x4plus 单项就有几十 MB）
        for (TensorInfo &t : d_->tensors) {
            if (t.isConst && t.hostOwned && t.buf.valid()) {
                std::vector<float>().swap(t.host);
                t.hostOwned = false;
            }
        }
    }

    stats_.fp16 = d_->fp16;
    prepared_ = true;
    return true;
}

bool Graph::run(const float *input, std::vector<float> *output, int *outW, int *outH,
                QString *error)
{
    auto fail = [&](const QString &m) {
        err_ = m;
        if (error)
            *error = m;
        return false;
    };
    if (!prepared_)
        return fail(QStringLiteral("图未 prepare"));
    vk::Device &dev = vk::Device::I();

    const int inElems = 3 * tileH_ * tileW_;
    const int outElems = d_->tensors[d_->outTensor].d.count();
    output->assign(size_t(outElems), 0.0f);

    QString lastOp;
    // 输入 fp32 -> 设备（fp16 时打包成 half；tile 很小，转换开销可忽略）
    const void *inSrc = input;
    quint64 inBytes = quint64(inElems) * 4;
    std::vector<quint16> in16;
    if (d_->fp16) {
        in16.resize(size_t(inElems));
        for (int k = 0; k < inElems; ++k)
            in16[size_t(k)] = vk::FloatToHalf(input[k]);
        inSrc = in16.data();
        inBytes = quint64(inElems) * 2;
    }
    if (!dump_) {
        if (!dev.beginPass())
            return fail(QStringLiteral("beginPass 失败：%1").arg(dev.lastError()));
        if (!dev.upload(d_->tensors[d_->inTensor].buf, inSrc, inBytes))
            return fail(QStringLiteral("上传输入失败：%1").arg(dev.lastError()));
        bool cpOk = false;
        const int cpEvery = qEnvironmentVariableIntValue("JM_VK_NAN_CHECKPOINT", &cpOk);
        const int cpFrom = qEnvironmentVariableIntValue("JM_VK_CP_FROM");
        const int cpTo = qEnvironmentVariableIntValue("JM_VK_CP_TO");
        for (int si = 0; si < d_->steps.size(); ++si) {
            const Step &s = d_->steps.at(si);
            // 绑定顺序 = 输入..., 输出；count 必须包含输出（descriptor 0..n 全要写）
            BufferRef binds[8];
            for (int k = 0; k < s.nIn; ++k)
                binds[k] = d_->tensors[s.in[k]].buf;
            binds[s.nIn] = d_->tensors[s.out].buf;
            if (!dev.dispatch(s.shader.toLatin1().constData(), s.pc, binds, s.nIn + 1, s.gx, s.gy,
                              s.gz))
                return fail(QStringLiteral("dispatch %1 失败：%2").arg(s.op, dev.lastError()));
            // 分批提交（A 方案）：到切点就把本批 submit 出去再录下一批，防移动
            // GPU 驱动 watchdog（x4plus tile128 单 pass ~7s 实测触发 DEVICE_LOST）。
            // fence 轮转保证跨批依赖：下一批的 beginPass 复用 slot 时会等上一轮。
            if (size_t(si) < d_->batchCheckpoints.size() && d_->batchCheckpoints[size_t(si)]
                && si + 1 < d_->steps.size()) {
                if (!dev.endPass())
                    return fail(QStringLiteral("endPass 失败：%1").arg(dev.lastError()));
                if (!dev.beginPass())
                    return fail(QStringLiteral("beginPass 失败：%1").arg(dev.lastError()));
            }
            if (cpOk && cpEvery > 0 && si % cpEvery == cpEvery - 1
                && (cpFrom <= 0 || si >= cpFrom) && (cpTo <= 0 || si <= cpTo)) {
                dev.endPass();
                const TensorInfo &t = d_->tensors[s.out];
                std::vector<float> probe;
                if (t.buf.valid() && t.buf.size <= 32ull * 1024 * 1024
                    && ReadTensorHost(t, &probe, d_->fp16)) {
                    {
                        int n = 0;
                        for (float v : probe)
                            n += std::isfinite(v) ? 0 : 1;
                        std::fprintf(stderr, "vk-cp: step=%d %s %s bad=%d\n", si,
                                     qPrintable(s.op), qPrintable(t.name), n);
                    }
                }
                dev.beginPass();
            }
        }
        if (!dev.endPass())
            return fail(QStringLiteral("endPass 失败：%1").arg(dev.lastError()));
        // 调试：一次 pass 结束后所有中间张量仍留在 arena 里，逐个扫 NaN，
        // 能在 1700+ 步的图上直接指出是哪一个节点开始发散。
        if (qEnvironmentVariableIsSet("JM_VK_NAN_SCAN")) {
            QStringList bad;
            for (int ti = 0; ti < d_->tensors.size(); ++ti) {
                const TensorInfo &t = d_->tensors[ti];
                if (!t.buf.valid() || t.buf.size > 32ull * 1024 * 1024)
                    continue;
                std::vector<float> tmp;
                if (!ReadTensorHost(t, &tmp, d_->fp16))
                    continue;
                int n = 0;
                for (float v : tmp) {
                    if (!std::isfinite(v))
                        ++n;
                }
                if (n > 0)
                    bad << QStringLiteral("#%1 %2 %3 nan=%4/%5")
                               .arg(ti)
                               .arg(t.name)
                               .arg(t.d.str())
                               .arg(n)
                               .arg(int(tmp.size()));
            }
            dumpReport_ = bad.join(QLatin1Char(10));
            std::fprintf(stderr, "%s\n", dumpReport_.toUtf8().constData());
        }
        if (d_->fp16) {
            std::vector<quint16> out16(size_t(outElems), 0);
            if (!dev.download(d_->tensors[d_->outTensor].buf, out16.data(),
                              quint64(outElems) * 2))
                return fail(QStringLiteral("回读输出失败：%1").arg(dev.lastError()));
            for (int k = 0; k < outElems; ++k)
                (*output)[size_t(k)] = vk::HalfToFloat(out16[size_t(k)]);
        } else if (!dev.download(d_->tensors[d_->outTensor].buf, output->data(),
                                 quint64(outElems) * 4)) {
            return fail(QStringLiteral("回读输出失败：%1").arg(dev.lastError()));
        }
    } else {
        // 调试模式：一步一步跑，每步结束都回读统计（定位第一个数值发散的节点）
        dumpReport_.clear();
        QStringList lines;
        dev.beginPass();
        dev.upload(d_->tensors[d_->inTensor].buf, inSrc, inBytes);
        dev.endPass();
        bool maxOk = false;
        int maxSteps = qEnvironmentVariableIntValue("JM_VK_DUMP_MAX", &maxOk);
        if (!maxOk)
            maxSteps = -1;
        for (int si = 0; si < d_->steps.size(); ++si) {
            if (maxSteps >= 0 && si >= maxSteps)
                break;
            const Step &s = d_->steps[si];
            BufferRef binds[8];
            for (int k = 0; k < s.nIn; ++k)
                binds[k] = d_->tensors[s.in[k]].buf;
            binds[s.nIn] = d_->tensors[s.out].buf;
            dev.beginPass();
            if (!dev.dispatch(s.shader.toLatin1().constData(), s.pc, binds, s.nIn + 1, s.gx, s.gy,
                              s.gz)) {
                dev.endPass();
                return fail(QStringLiteral("dispatch #%1 %2 失败：%3")
                                .arg(si)
                                .arg(s.op, dev.lastError()));
            }
            dev.endPass();
            const TensorInfo &t = d_->tensors[s.out];
            std::vector<float> tmp;
            if (!ReadTensorHost(t, &tmp, d_->fp16))
                return fail(QStringLiteral("回读 #%1 失败").arg(si));
            float lo = 1e30f, hi = -1e30f;
            double sum = 0.0;
            for (float v : tmp) {
                lo = qMin(lo, v);
                hi = qMax(hi, v);
                sum += v;
            }
            bool dumpOk = false;
            const int wantIdx = qEnvironmentVariableIntValue("JM_VK_VALUES", &dumpOk);
            if (dumpOk && wantIdx == si) {
                QStringList pcv;
                for (int k = 0; k < 16; ++k)
                    pcv << QString::number(s.pc.i[k]);
                lines << QStringLiteral("PC t%1 [%2]").arg(si).arg(pcv.join(QLatin1String(",")));
                QStringList vals;
                for (int k = 0; k < 16 && size_t(k) < tmp.size(); ++k)
                    vals << QString::number(double(tmp[size_t(k)]), 'g', 6);
                lines << QStringLiteral("VALUES t%1 [%2]").arg(si).arg(vals.join(QLatin1String(",")));
            }
            lines << QStringLiteral("t%1 %2 %3 %4 min=%5 max=%6 mean=%7")
                         .arg(si)
                         .arg(s.op)
                         .arg(t.d.str())
                         .arg(s.outName)
                         .arg(double(lo), 0, 'g', 5)
                         .arg(double(hi), 0, 'g', 5)
                         .arg(sum / double(qMax<size_t>(size_t(1), tmp.size())), 0, 'g', 5);
        }
        dumpReport_ = lines.join(QLatin1Char('\n'));
        if (!dev.download(d_->tensors[d_->outTensor].buf, output->data(),
                          quint64(outElems) * 4))
            return fail(QStringLiteral("回读输出失败"));
    }
    if (outW)
        *outW = outW_;
    if (outH)
        *outH = outH_;
    return true;
}

// 多 tile 流水（深度 2）：
//   submitTile(i)  = 上传输入 i（staging -> slot i%2）+ 录制 dispatch + submit
//   finishTile(i)  = 独立 copy 输出 slot i%2 -> staging + 等 xferFence + host 解包
// 顺序：submitTile(0) -> [submitTile(i+1); finishTile(i)]。GPU 上 pass i+1 与
// copy i 排队串行，但 CPU 在 finishTile(i) 等待期间 GPU 正在算 pass i+1，
// 消除了旧串行流程里"每 tile 结束 GPU 空等 CPU 录制+上传"的空窗。
// 同步安全性：
//   * slot 复用由 Device::beginPass 的 fence 等待保证（第 k 个 pass 复用
//     cmds_[k%2]/fences_[k%2] 前必须等上一轮完成）；
//   * finishTile 的 xferFence 完成在队列序上涵盖此前全部 pass，输出此时才
//     落到 host 内存；
//   * staging 环回卷时 Device::download 内部会等全部在飞 fence 再重置。
bool Graph::runStream(int count, const TileStream &cb, QString *error)
{
    auto fail = [&](const QString &m) {
        err_ = m;
        if (error)
            *error = m;
        return false;
    };
    if (!prepared_)
        return fail(QStringLiteral("图未 prepare"));
    if (count <= 0)
        return fail(QStringLiteral("tile 数为 0"));
    if (!d_->streamReady)
        return fail(QStringLiteral("流水 slot 未就绪（prepare 异常）"));
    if (!cb.input || !cb.output)
        return fail(QStringLiteral("TileStream 回调缺失"));
    vk::Device &dev = vk::Device::I();

    const int inElems = 3 * tileH_ * tileW_;
    const int outElems = d_->tensors[d_->outTensor].d.count();

    // submitTile：把 tile i 的输入打包进 slot i%2，再录 dispatch 序列
    //（in/out 张量的 buf 临时指向对应 slot，dispatch 引用随命令固化）
    auto submitTile = [&](int i) -> bool {
        const float *hostIn = cb.input(i);
        if (!hostIn)
            return fail(QStringLiteral("tile %1 输入数据为空").arg(i));
        const BufferRef inDev = d_->inSlot[i & 1];
        const BufferRef outDev = d_->outSlot[i & 1];
        d_->tensors[d_->inTensor].buf = inDev;
        d_->tensors[d_->outTensor].buf = outDev;
        if (!dev.beginPass())
            return fail(QStringLiteral("stream beginPass 失败：%1").arg(dev.lastError()));
        const void *src = hostIn;
        quint64 bytes = quint64(inElems) * 4;
        std::vector<quint16> in16;
        if (d_->fp16) {
            in16.resize(size_t(inElems));
            for (int k = 0; k < inElems; ++k)
                in16[size_t(k)] = vk::FloatToHalf(hostIn[k]);
            src = in16.data();
            bytes = quint64(inElems) * 2;
        }
        if (!dev.upload(d_->tensors[d_->inTensor].buf, src, bytes))
            return fail(QStringLiteral("stream 上传输入失败：%1").arg(dev.lastError()));
        for (int si = 0; si < d_->steps.size(); ++si) {
            const Step &s = d_->steps.at(si);
            BufferRef binds[8];
            for (int k = 0; k < s.nIn; ++k)
                binds[k] = d_->tensors[s.in[k]].buf;
            binds[s.nIn] = d_->tensors[s.out].buf;
            if (!dev.dispatch(s.shader.toLatin1().constData(), s.pc, binds, s.nIn + 1, s.gx,
                              s.gy, s.gz))
                return fail(QStringLiteral("stream dispatch %1 失败：%2")
                                .arg(s.op, dev.lastError()));
            // 分批提交（A 方案）在流水里同样生效：一个 tile 拆成多个 mini-pass，
            // slot 轮转自动限流（mini-pass 数 × tile 数 > 2 时由 beginPass 等待）
            if (size_t(si) < d_->batchCheckpoints.size() && d_->batchCheckpoints[size_t(si)]
                && si + 1 < d_->steps.size()) {
                if (!dev.endPass())
                    return fail(QStringLiteral("stream endPass 失败：%1").arg(dev.lastError()));
                if (!dev.beginPass())
                    return fail(QStringLiteral("stream beginPass 失败：%1").arg(dev.lastError()));
            }
        }
        if (!dev.endPass())
            return fail(QStringLiteral("stream endPass 失败：%1").arg(dev.lastError()));
        return true;
    };

    // finishTile：等 pass 完成（经 xferFence）、下载输出并解包成 fp32
    auto finishTile = [&](int i, std::vector<float> *out) -> bool {
        const BufferRef outDev = d_->outSlot[i & 1];
        if (d_->fp16) {
            std::vector<quint16> out16(size_t(outElems), 0);
            if (!dev.download(outDev, out16.data(), quint64(outElems) * 2))
                return fail(QStringLiteral("stream 下载输出失败：%1").arg(dev.lastError()));
            out->resize(size_t(outElems));
            for (int k = 0; k < outElems; ++k)
                (*out)[size_t(k)] = vk::HalfToFloat(out16[size_t(k)]);
        } else {
            out->resize(size_t(outElems));
            if (!dev.download(outDev, out->data(), quint64(outElems) * 4))
                return fail(QStringLiteral("stream 下载输出失败：%1").arg(dev.lastError()));
        }
        return true;
    };

    std::vector<float> hostOut;
    // 保存原始绑定（prepare 分配的激活池位置），流水结束后恢复，
    // 让后续单 tile run()/dump 路径行为不变
    const BufferRef origIn = d_->tensors[d_->inTensor].buf;
    const BufferRef origOut = d_->tensors[d_->outTensor].buf;
    if (!submitTile(0))
        return false;
    bool cancelled = false;
    for (int i = 0; i < count; ++i) {
        if (i + 1 < count && !cancelled) {
            if (!submitTile(i + 1))
                return false;
        }
        if (!finishTile(i, &hostOut))
            return false;
        cb.output(i, hostOut.data());
        if (cb.step && !cb.step(i)) {
            // 取消：已提交的 tile i+1 会跑完但不下载（上层整图重跑/终止）
            cancelled = true;
            break;
        }
    }
    d_->tensors[d_->inTensor].buf = origIn;
    d_->tensors[d_->outTensor].buf = origOut;
    if (cancelled)
        return fail(QStringLiteral("已取消"));
    return true;
}

}  // namespace SrVk
