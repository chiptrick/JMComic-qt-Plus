// Vulkan 超分后端门面（与 SrEngine 的 ORT/QNN 路径并列）。
//
// 分层：
//   vk_api     仅 loader + 入口解析 + 设备枚举（无状态）
//   vk_device  实例/设备/arena/管线/一次 pass 的命令录制
//   vk_onnx    ONNX 最小读取（protobuf -> 节点/初始化器/属性）
//   sr_graph   ONNX 图 -> Vulkan 计算（常量折叠、形状推断、显存规划、逐算子 dispatch）
//   sr_vk      会话门面：一个模型实例 + tile 绑定 + RunTile
//
// 与 CPU/RST 后端的关系：
//   * SrEngine 负责 tile 切分、padding/收缩边、结果拼装与进度/取消；
//   * 本模块只做"一组 fp32 tile 进、一组 fp32 tile 出"，即 SrEngine::RunTile 的等价物；
//   * 因此数值对比可以逐 tile 做，CPU 与 Vulkan 的输出可以直接 diff。
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

#include <vector>

#include "sr_graph.h"

namespace SrVk {

// 设备是否可用（首次调用会建实例/设备；失败原因缓存在 *why）
bool Available(QString *why = nullptr);
// 当前 Vulkan 设备是否启用 fp16 存储（JM_VK_FP16=0 或能力不足时为 false）
bool Fp16Active();
// 一行设备摘要（--vk-info / 设置页状态用）
QString Info();
// 端到端自检：设备 + arena + 描述符 + push constant + dispatch + 回读
bool Probe(QString *report);
// 单算子自检：3x3/stride1/pad1 卷积 vs CPU 参考（含 Winograd F(2,3) 用例）
bool ConvSelfTest(QString *report);
// reflect/constant Pad 单算子自检
bool PadSelfTest(QString *report);

// 一个模型的 Vulkan 会话。tile 尺寸决定所有张量形状，所以换 tile 要重新 prepare。
class Session
{
public:
    Session();
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    bool load(const QString &modelPath, QString *error);
    bool ensureTile(int tile, QString *error);
    bool ready() const;
    void reset();

    bool run(const float *input, std::vector<float> *output, int *outW, int *outH,
             QString *error);
    // 多 tile 流水（深度 2）：GPU 算第 k 个 tile 时 CPU 在准备第 k+1 个。
    // 取消时返回 false 且 error="已取消"；运行失败（含 DEVICE_LOST）时由
    // 调用方决定回退（整图重跑或切 CPU），会话内部不做中途恢复。
    bool runTiles(int count, const TileStream &cb, QString *error);
    int outW() const;
    int outH() const;
    int scale() const;
    QString summary() const;
    QString lastError() const;

    Graph *graph();

private:
    struct Impl;
    Impl *d_ = nullptr;
};

// 模型扫描：对目录下每个非量化 onnx 走一遍 load -> prepare -> run（合成输入），
// 报告逐模型状态。only 为空表示全部；命中规则是"文件名词干包含"。
struct ModelResult
{
    QString file;
    bool ok = false;
    bool skipped = false;   // 量化模型：显式跳过（原因写 note），不算失败
    QString note;
    int outW = 0;
    int outH = 0;
    qint64 elapsedMs = 0;
};
QVector<ModelResult> Sweep(const QString &dir, const QStringList &only, int tile,
                           QString *report);

// 单模型吞吐：warmup 一次后跑 iterations 次，给出 min/median/mean（ms/tile）。
// 只统计 Vulkan 侧；ORT CPU 对照由 CLI 用 SrEngine::DebugOrtTile 另做。
struct BenchResult
{
    QString file;
    int tile = 0;
    int iterations = 0;
    double minMs = 0.0;
    double medianMs = 0.0;
    double meanMs = 0.0;
    quint64 activationBytes = 0;
    bool ok = false;
    QString note;
};
BenchResult Benchmark(const QString &modelPath, int tile, int iterations, QString *report);

}  // namespace SrVk
