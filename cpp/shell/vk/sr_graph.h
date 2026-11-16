// ONNX 图 -> Vulkan 计算。只做"一个 tile 进、一个 tile 出"，切块/补边/拼装由 SrEngine 负责。
//
// 三段式：
//   load()    读 ONNX（vk_onnx），建立节点表；
//   prepare() 常量折叠（形状类算子一律在 CPU 上算掉）-> 形状推断 -> 显存分配与权重上传 ->
//             把每个算子编译成一条"shader + push constant + 绑定 + workgroup 数"的执行步；
//   run()     上传输入 tile，按序 dispatch，回读输出。
//
// 为什么把参数在 prepare 里全算完：tile 尺寸固定后所有张量形状都是常量，
// 执行期只剩 dispatch，避免每帧重复推导（39 个模型里 x4plus 有 1024 个节点）。
#pragma once

#include <QString>
#include <QVector>

#include <functional>

#include <vector>

#include "vk_device.h"

namespace vk {
struct OnnxModel;
struct OnnxNode;
}  // namespace vk

namespace SrVk {

struct GraphStats
{
    int nodes = 0;        // 原始节点数
    int steps = 0;        // 真正要 dispatch 的步数
    int folded = 0;       // 常量折叠掉的节点数
    qint64 weightMb = 0;
    qint64 actMb = 0;
    int peakTensors = 0;  // 同时存活的张量数
    bool fp16 = false;    // 本次 prepare 实际使用的存储格式
    int winoLayers = 0;   // 走 Winograd F(2,3) 的卷积层数
};

// 多 tile 流水的回调集合（由调用方提供输入生成/输出消费/进度取消）。
// input(k) 返回第 k 个 tile 的 NCHW fp32 数据（指针有效期到本次回调返回）；
// output(k, data) 接收第 k 个 tile 的输出（fp32 NCHW，data 有效期到回调返回）；
// step(k) 在第 k 个 tile 完成后调用，返回 false 表示取消（不再提交后续 tile）。
struct TileStream
{
    std::function<const float *(int)> input;
    std::function<void(int, const float *)> output;
    std::function<bool(int)> step;
};

// Winograd F(2,3) 权重预变换（CPU 侧，prepare 与单算子自检共用）：
// [OC][IC][3][3] fp32 -> [OC][IC][4][4]（行主序 i*4+j）。
void WinogradTransformWeights(const float *w, int oc, int ic, std::vector<float> *out);

class Graph
{
public:
    Graph();
    ~Graph();

    bool load(const QString &onnxPath, QString *error);
    bool prepare(int tileW, int tileH, QString *error);
    bool run(const float *input, std::vector<float> *output, int *outW, int *outH,
             QString *error);
    // 多 tile 流水（深度 2）：GPU 算第 k 个 tile 时 CPU 在准备/提交第 k+1 个。
    // 与 run() 的区别：submit 与等待错开一个 tile，消除每 tile 的 GPU 空窗；
    // 输出回调在下载完成时逐个触发。取消时已提交 tile 会跑完但不回调输出。
    bool runStream(int count, const TileStream &cb, QString *error);

    bool loaded() const { return loaded_; }
    bool prepared() const { return prepared_; }
    int outW() const { return outW_; }
    int outH() const { return outH_; }
    int inW() const { return tileW_; }
    int inH() const { return tileH_; }
    int scale() const { return scale_; }
    const GraphStats &stats() const { return stats_; }
    QString summary() const;
    // 逐节点张量统计（调试数值偏差用；未开启时不产生任何回读）
    void setDumpTensors(bool on) { dump_ = on; }
    QString dumpReport() const { return dumpReport_; }
    const QString &lastError() const { return err_; }

private:
    bool prepared_ = false;
    bool loaded_ = false;
    bool dump_ = false;
    int tileW_ = 0;
    int tileH_ = 0;
    int outW_ = 0;
    int outH_ = 0;
    int scale_ = 1;
    GraphStats stats_;
    QString err_;
    QString dumpReport_;
    QString path_;
    struct Impl;
    Impl *d_ = nullptr;
};

}  // namespace SrVk
