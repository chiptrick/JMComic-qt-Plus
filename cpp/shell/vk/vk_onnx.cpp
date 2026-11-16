#include "vk_onnx.h"

#include <QFile>
#include <QFileInfo>

#include <cstring>

namespace vk {
namespace {

// ---------------------------------------------------------------- protobuf 游标
class Pb
{
public:
    Pb(const char *d, int n) : p_(d), end_(d + n) {}

    bool eof() const { return p_ >= end_ || bad_; }
    bool bad() const { return bad_; }

    quint64 varint()
    {
        quint64 v = 0;
        int shift = 0;
        while (p_ < end_) {
            const quint8 b = quint8(*p_++);
            v |= quint64(b & 0x7f) << shift;
            if ((b & 0x80) == 0)
                return v;
            shift += 7;
            if (shift > 63) {
                bad_ = true;
                return 0;
            }
        }
        bad_ = true;
        return 0;
    }

    // 读一个 field header；成功返回 false 表示读到末尾
    bool next(quint32 *field, quint32 *wire)
    {
        if (eof())
            return false;
        const quint64 key = varint();
        if (bad_)
            return false;
        *field = quint32(key >> 3);
        *wire = quint32(key & 7);
        return true;
    }

    // wire==2 时取出 (指针, 长度)；调用方不做拷贝
    bool bytes(const char **data, int *len)
    {
        const quint64 n = varint();
        if (bad_ || quint64(end_ - p_) < n) {
            bad_ = true;
            return false;
        }
        *data = p_;
        *len = int(n);
        p_ += n;
        return true;
    }

    QByteArray byteArray()
    {
        const char *d = nullptr;
        int n = 0;
        if (!bytes(&d, &n))
            return QByteArray();
        return QByteArray(d, n);
    }

    bool skip(quint32 wire)
    {
        switch (wire) {
        case 0:
            varint();
            return !bad_;
        case 1:
            if (end_ - p_ < 8) { bad_ = true; return false; }
            p_ += 8;
            return true;
        case 2: {
            const char *d = nullptr;
            int n = 0;
            return bytes(&d, &n);
        }
        case 5:
            if (end_ - p_ < 4) { bad_ = true; return false; }
            p_ += 4;
            return true;
        default:
            bad_ = true;
            return false;
        }
    }

    quint32 fixed32()
    {
        if (end_ - p_ < 4) { bad_ = true; return 0; }
        quint32 v = 0;
        std::memcpy(&v, p_, 4);
        p_ += 4;
        return v;
    }

private:
    const char *p_ = nullptr;
    const char *end_ = nullptr;
    bool bad_ = false;
};

QVector<qint64> ReadPackedInts(const char *d, int n)
{
    QVector<qint64> out;
    Pb pb(d, n);
    while (!pb.eof())
        out.append(qint64(pb.varint()));
    return out;
}

QVector<float> ReadPackedFloats(const char *d, int n)
{
    QVector<float> out;
    out.resize(n / 4);
    if (n >= 4)
        std::memcpy(out.data(), d, size_t(n / 4 * 4));
    return out;
}

// TensorProto: dims=1, data_type=2, float_data=4, int32_data=5, int64_data=7,
//              name=8, raw_data=9, double_data=10
void ParseTensor(const char *data, int len, OnnxTensor *t)
{
    Pb pb(data, len);
    quint32 field = 0, wire = 0;
    while (pb.next(&field, &wire)) {
        switch (field) {
        case 1:   // dims（可能是 packed，也可能逐条）
            if (wire == 0)
                t->dims.append(qint64(pb.varint()));
            else if (wire == 2) {
                const char *d = nullptr;
                int n = 0;
                if (pb.bytes(&d, &n))
                    t->dims += ReadPackedInts(d, n);
            } else {
                pb.skip(wire);
            }
            break;
        case 2:
            t->dtype = int(pb.varint());
            break;
        case 4: {   // float_data
            const char *d = nullptr;
            int n = 0;
            if (pb.bytes(&d, &n))
                t->floats = ReadPackedFloats(d, n);
            break;
        }
        case 5: {   // int32_data
            const char *d = nullptr;
            int n = 0;
            if (pb.bytes(&d, &n)) {
                QVector<qint64> v = ReadPackedInts(d, n);
                t->ints += v;
            }
            break;
        }
        case 7: {   // int64_data
            if (wire == 0) {
                t->ints.append(qint64(pb.varint()));
            } else {
                const char *d = nullptr;
                int n = 0;
                if (pb.bytes(&d, &n))
                    t->ints += ReadPackedInts(d, n);
            }
            break;
        }
        case 8:
            t->name = QString::fromUtf8(pb.byteArray());
            break;
        case 9:
            t->raw = pb.byteArray();
            break;
        default:
            pb.skip(wire);
            break;
        }
    }
    // float_data / int64_data 路径统一补齐到 raw / ints，后面只认一种表示
    if (t->raw.isEmpty() && !t->floats.isEmpty()) {
        t->raw = QByteArray(reinterpret_cast<const char *>(t->floats.constData()),
                            t->floats.size() * int(sizeof(float)));
    }
    if (t->ints.isEmpty() && t->dtype == kInt64 && !t->raw.isEmpty()) {
        const int n = t->raw.size() / 8;
        t->ints.resize(n);
        if (n > 0)
            std::memcpy(t->ints.data(), t->raw.constData(), size_t(n) * 8);
    }
}

// AttributeProto: name=1, f=2, i=3, s=4, t=5, floats=7, ints=8, strings=9
void ParseAttr(const char *data, int len, OnnxAttr *a)
{
    Pb pb(data, len);
    quint32 field = 0, wire = 0;
    while (pb.next(&field, &wire)) {
        switch (field) {
        case 1:
            a->name = QString::fromUtf8(pb.byteArray());
            break;
        case 2:
            if (wire == 5) {
                const quint32 bits = pb.fixed32();
                std::memcpy(&a->f, &bits, 4);
            } else {
                pb.skip(wire);
            }
            break;
        case 3:
            if (wire == 0)
                a->i = qint64(pb.varint());
            else
                pb.skip(wire);
            break;
        case 4:
            a->s = pb.byteArray();
            break;
        case 5: {
            const char *d = nullptr;
            int n = 0;
            if (pb.bytes(&d, &n)) {
                ParseTensor(d, n, &a->t);
                a->hasT = true;
            }
            break;
        }
        case 7: {   // floats
            const char *d = nullptr;
            int n = 0;
            if (pb.bytes(&d, &n))
                a->floats = ReadPackedFloats(d, n);
            break;
        }
        case 8:     // ints
            if (wire == 0)
                a->ints.append(qint64(pb.varint()));
            else {
                const char *d = nullptr;
                int n = 0;
                if (pb.bytes(&d, &n))
                    a->ints += ReadPackedInts(d, n);
            }
            break;
        default:
            pb.skip(wire);
            break;
        }
    }
}

// NodeProto: input=1, output=2, name=3, op_type=4, attribute=5
void ParseNode(const char *data, int len, OnnxNode *n)
{
    Pb pb(data, len);
    quint32 field = 0, wire = 0;
    while (pb.next(&field, &wire)) {
        switch (field) {
        case 1:
            n->inputs.append(QString::fromUtf8(pb.byteArray()));
            break;
        case 2:
            n->outputs.append(QString::fromUtf8(pb.byteArray()));
            break;
        case 3:
            n->name = QString::fromUtf8(pb.byteArray());
            break;
        case 4:
            n->op = QString::fromUtf8(pb.byteArray());
            break;
        case 5: {
            const char *d = nullptr;
            int l = 0;
            if (pb.bytes(&d, &l)) {
                OnnxAttr a;
                ParseAttr(d, l, &a);
                n->attrs.append(a);
            }
            break;
        }
        default:
            pb.skip(wire);
            break;
        }
    }
}

// ValueInfoProto: name=1（形状这里不解析，靠图内推断）
QString ParseValueInfoName(const char *data, int len)
{
    Pb pb(data, len);
    quint32 field = 0, wire = 0;
    while (pb.next(&field, &wire)) {
        if (field == 1)
            return QString::fromUtf8(pb.byteArray());
        pb.skip(wire);
    }
    return QString();
}

// GraphProto: node=1, initializer=5, input=11, output=12
void ParseGraph(const char *data, int len, OnnxModel *m)
{
    Pb pb(data, len);
    quint32 field = 0, wire = 0;
    while (pb.next(&field, &wire)) {
        const char *d = nullptr;
        int n = 0;
        switch (field) {
        case 1:
            if (pb.bytes(&d, &n)) {
                OnnxNode node;
                ParseNode(d, n, &node);
                m->nodes.append(node);
            }
            break;
        case 5:
            if (pb.bytes(&d, &n)) {
                OnnxTensor t;
                ParseTensor(d, n, &t);
                if (!t.name.isEmpty())
                    m->inits.insert(t.name, t);
            }
            break;
        case 11:
            if (pb.bytes(&d, &n))
                m->inputs.append(ParseValueInfoName(d, n));
            break;
        case 12:
            if (pb.bytes(&d, &n))
                m->outputs.append(ParseValueInfoName(d, n));
            break;
        default:
            pb.skip(wire);
            break;
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- 公开类型
qint64 OnnxTensor::count() const
{
    if (dims.isEmpty())
        return 1;   // 标量
    qint64 n = 1;
    for (qint64 d : dims) {
        if (d <= 0)
            return 0;
        n *= d;
    }
    return n;
}

const OnnxAttr *OnnxNode::find(const QString &key) const
{
    for (const OnnxAttr &a : attrs) {
        if (a.name == key)
            return &a;
    }
    return nullptr;
}

qint64 OnnxNode::intAttr(const QString &key, qint64 def) const
{
    const OnnxAttr *a = find(key);
    if (!a)
        return def;
    if (!a->ints.isEmpty())
        return a->ints.first();
    return a->i;
}

QVector<qint64> OnnxNode::intsAttr(const QString &key, const QVector<qint64> &def) const
{
    const OnnxAttr *a = find(key);
    if (!a)
        return def;
    if (!a->ints.isEmpty())
        return a->ints;
    if (a->i != 0)
        return {a->i};
    return def;
}

QString OnnxNode::strAttr(const QString &key, const QString &def) const
{
    const OnnxAttr *a = find(key);
    if (!a || a->s.isEmpty())
        return def;
    return QString::fromUtf8(a->s);
}

bool ReadOnnxFile(const QString &path, OnnxModel *out, QString *error)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("无法打开模型：%1").arg(QFileInfo(path).fileName());
        return false;
    }
    const QByteArray blob = f.readAll();
    f.close();
    if (blob.size() < 16) {
        if (error)
            *error = QStringLiteral("模型文件过小（%1 字节）").arg(blob.size());
        return false;
    }

    // ModelProto: graph=7, opset_import=8
    Pb pb(blob.constData(), blob.size());
    quint32 field = 0, wire = 0;
    bool gotGraph = false;
    while (pb.next(&field, &wire)) {
        const char *d = nullptr;
        int n = 0;
        if (field == 7) {
            if (pb.bytes(&d, &n)) {
                ParseGraph(d, n, out);
                gotGraph = true;
            }
        } else if (field == 8) {
            if (pb.bytes(&d, &n)) {
                // OperatorSetIdProto: domain=1, version=2
                Pb op(d, n);
                quint32 f2 = 0, w2 = 0;
                while (op.next(&f2, &w2)) {
                    if (f2 == 2)
                        out->opset = int(op.varint());
                    else
                        op.skip(w2);
                }
            }
        } else {
            pb.skip(wire);
        }
    }
    if (pb.bad() || !gotGraph) {
        if (error)
            *error = QStringLiteral("不是合法的 ONNX 模型（缺少 graph 字段）");
        return false;
    }
    // graph.input 里形如 initializer 的条目不是真正输入
    QStringList real;
    for (const QString &n : out->inputs) {
        if (n.isEmpty() || out->inits.contains(n))
            continue;
        real.append(n);
    }
    out->inputs = real;
    if (out->inputs.isEmpty() || out->outputs.isEmpty()) {
        if (error)
            *error = QStringLiteral("模型缺少输入或输出声明");
        return false;
    }
    return true;
}

}  // namespace vk
