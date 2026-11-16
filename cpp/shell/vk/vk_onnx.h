// 最小 ONNX 读取器（只为超分模型服务，不追求完整 protobuf 语义）。
//
// 为什么自己写：
//   * 本条链路的既定原则是"不引第三方运行时"（ncnn/onnx 库都不可用）；
//   * 需要的只有 GraphProto 的 node / initializer / input / output 四类字段，
//     以及 AttributeProto 的 i / f / s / ints / floats / t 六种取值；
//   * 手工解析能给出精确的失败原因（哪个节点、哪个字段），比"加载失败"有用。
//
// 支持范围（与 39 个超分模型的实测算子集合一致）：
//   Conv / ConvTranspose / Relu / LeakyRelu / PRelu / Sigmoid / Clip /
//   Add / Mul / Sub / Div / Concat / Pad / DepthToSpace / Resize /
//   GlobalAveragePool / ReduceMean / Constant / ConstantOfShape / Equal / Where / Expand
// 量化模型（DequantizeLinear / QuantizeLinear，即 *_q16 / lowq）显式不支持：
//   prepare 时给出"量化模型需要 QDQ 反量化，未实现"的原因而不是算出错误结果。
#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

namespace vk {

// TensorProto.data_type
enum OnnxDType {
    kFloat = 1,
    kInt32 = 6,
    kInt64 = 7,
    kBool = 9,
    kFloat16 = 10,
};

struct OnnxTensor
{
    QString name;
    QVector<qint64> dims;
    int dtype = 0;
    QByteArray raw;          // raw_data；若模型用 float_data/int64_data 存，这里会补齐
    QVector<float> floats;   // 仅在 float_data 路径下有值（raw 也会同步生成）
    QVector<qint64> ints;    // int64_data / int32_data

    qint64 count() const;
    bool isFloat() const { return dtype == kFloat; }
    // 按 float 读取（dtype 必须是 kFloat，raw 长度必须匹配）
    const float *fdata() const { return reinterpret_cast<const float *>(raw.constData()); }
};

struct OnnxAttr
{
    QString name;
    qint64 i = 0;
    float f = 0.0f;
    QByteArray s;
    QVector<qint64> ints;
    QVector<float> floats;
    OnnxTensor t;
    bool hasT = false;
};

struct OnnxNode
{
    QString op;
    QString name;
    QStringList inputs;
    QStringList outputs;
    QVector<OnnxAttr> attrs;

    const OnnxAttr *find(const QString &key) const;
    qint64 intAttr(const QString &key, qint64 def = 0) const;
    QVector<qint64> intsAttr(const QString &key, const QVector<qint64> &def = {}) const;
    QString strAttr(const QString &key, const QString &def = QString()) const;
};

struct OnnxModel
{
    QVector<OnnxNode> nodes;
    QHash<QString, OnnxTensor> inits;
    QStringList inputs;    // 图输入里去掉 initializer 之后的真正输入名
    QStringList outputs;
    int opset = 0;
};

bool ReadOnnxFile(const QString &path, OnnxModel *out, QString *error);

}  // namespace vk
