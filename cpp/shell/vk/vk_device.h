// Vulkan 计算设备：实例/设备/队列、显存 arena、计算管线与一次 pass 的命令录制。
//
// 显存策略（桌面/移动同一套代码，参数不同）：
//   * 只建 **一个** device-local VkBuffer 作为 arena，所有权重与中间张量都是它的
//     子区间（descriptor 绑 (buffer, offset, range)）。原因：
//       - vkAllocateMemory 有 maxMemoryAllocationCount 上限（常见 4096），
//         39 个模型里 x4plus 单个就有 704 个初始化器，逐个分配必然打爆；
//       - 子分配只做 bump，一个 pass 结束整段重置，没有碎片整理成本。
//   * arena 大小 = min(设备本地堆 / 4, JM_VK_VRAM_MB 或默认 1024MB)；
//     移动端按堆大小自动缩到 256~384MB（见 vk::BudgetBytes）。
//   * 权重常驻一段（Prepare 时一次性写入），激活值每 tile 从另一段 bump 分配；
//     空间不足时先报 KO_OF_MEMORY 再由上层缩 tile / 回退 CPU。
//   * 上传/回读只用一个 host-visible staging buffer（默认 64MB），
//     与设备缓冲之间通过 vkCmdCopyBuffer 搬运，整 pass 一次 submit + fence 等待。
//
// 管线：SPIR-V 从 .qsb（Qt QShader 容器）里取，qsb 在构建期生成（见 CMakeLists）。
// 所有 kernel 共用一套 descriptor 布局：8 个 storage buffer + 16 个 push constant。
#pragma once

#include "vk_api.h"

#include <QByteArray>
#include <QHash>
#include <QString>

#include <vector>

namespace vk {

constexpr quint32 kMaxBindings = 8;

struct BufferRef
{
    VkBuffer buffer = VK_NULL_HANDLE;
    quint64 offset = 0;
    quint64 size = 0;
    bool valid() const { return buffer != VK_NULL_HANDLE && size > 0; }
};

// kernel 参数：push constant（64 字节，够放 N/C/H/W/K 等）
struct Params
{
    qint32 i[16] = {0};
    void set(int idx, qint32 v) { i[idx] = v; }
};

class Device
{
public:
    static Device &I();

    // 建实例/选设备/建队列/建 arena。error 给出人话原因。
    // preferIndex < 0 时自动挑选（独显优先，其次集显，其次其它）。
    bool Ensure(QString *error, int preferIndex = -1);
    void Shutdown();
    bool valid() const { return device_ != VK_NULL_HANDLE; }
    // VK_ERROR_DEVICE_LOST：驱动重置 / GPU watchdog / 后台被系统回收显存后会出现。
    // 一旦置位，所有后续 dispatch 都不能再信任旧句柄；lost() 供会话层决定是否重建。
    bool lost() const;
    // 反复 DEVICE_LOST 后进程内熔断：不再尝试重建，直接回退 CPU
    bool broken() const;
    // 丢弃旧实例/设备/arena 并重建（保留 preferIndex）。重建后权重与激活全部失效，
    // 调用方必须重新 load/prepare 会话。失败时保留人话原因，交给上层回退 CPU。
    bool Recover(QString *error);
    int generation() const { return generation_; }

    const DeviceInfo &info() const { return info_; }
    QString summary() const;   // 一行：dev/vram/budget/limits
    int deviceIndex() const { return deviceIndex_; }

    // ---- fp16（NCNN fp16_packing 模式：存储 half、累加 fp32）----
    // 纯硬件能力：shaderFloat16 + storageBuffer16BitAccess（Vulkan 1.2 core）。
    bool fp16Feature() const { return fp16Feature_; }
    // 实际可用 = 硬件能力 && JM_VK_FP16 未显式禁用（=0 时退回 fp32）
    bool fp16Usable() const;

    // ---- descriptor 复用 ----
    // VK_KHR_push_descriptor 可用（dispatch 不再 allocate/update/bind set）
    bool pushDescriptor() const { return hasPushDesc_; }

    // arena 分配（device-local）。失败返回无效 ref 并置 lastError()。
    BufferRef alloc(quint64 bytes, quint64 alignment = 256);
    // 权重区：Prepare 阶段一次性分配，此后不重置
    BufferRef allocPersistent(quint64 bytes, quint64 alignment = 256);
    void resetActivations();
    // 只重置 staging 环形缓冲的写指针（批量上传权重时需要，不动激活区）
    void resetStaging();
    // 重置权重区（换模型时用）。调用方必须保证没有会话还在用旧的权重缓冲。
    void resetWeights();
    // 按模型实际权重大小调整"权重区/激活区"分界。默认对半分会白占 512MB，
    // 而 x4plus 的激活张量要 792MB；权重最多 67MB，给它 80~96MB 就够。
    void setWeightReserve(quint64 bytes);
    quint64 activationUsed() const { return actBump_; }
    quint64 arenaBytes() const { return arenaBytes_; }
    quint64 budgetBytes() const { return budget_; }
    QString lastError() const { return lastError_; }

    // 上传：把 host 数据写进 persistent/activation 区间（会记录到当前 pass 的命令里）
    bool upload(const BufferRef &dst, const void *src, quint64 bytes);

    // 一次 tile 的 pass：Begin -> 若干 dispatch -> End（submit+wait）-> readback
    bool beginPass();
    bool endPass();
    // 等待两个 slot 的在飞 fence 全部完成（staging 环回卷/复用前必须调用：
    // endPass 已不阻塞，在飞 pass 的 copy 命令可能还没读走 staging 段）
    bool waitForAllPasses();
    bool dispatch(const char *shader, const Params &pc, const BufferRef *buffers, int count,
                  quint32 groupsX, quint32 groupsY = 1, quint32 groupsZ = 1);
    // 读回（在 endPass 之后调用）
    bool download(const BufferRef &src, void *dst, quint64 bytes);

    QString deviceReport() const;

private:
    Device() = default;
    bool createInstance(QString *error);
    bool pickDevice(QString *error, int preferIndex);
    bool createDevice(QString *error);
    bool createArena(QString *error);
    bool ensurePipeline(const char *shader, VkPipeline *out, QString *error);
    quint64 BudgetBytes() const;

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    quint32 queueFamily_ = 0;
    // ---- 命令缓冲/fence 双份轮转（tile 流水的载体）----
    // passSeq_ 单调递增；第 k 个 pass 用 cmds_[k%2] 与 fences_[k%2]。
    // 复用 slot 前必须等上一轮的 fence（beginPass 里做），保证：
    //   * 命令缓冲录制不会覆盖还在执行中的命令（驱动要求）；
    //   * 该 slot 引用的输入/输出设备缓冲已无 GPU 读取方。
    // endPass 只 submit 不等待：等待交给 slot 复用（下一个同 slot 的 pass）
    // 或下载路径的 xferFence（其完成语义在队列上涵盖先前的全部 pass）。
    VkCommandBuffer cmds_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkFence fences_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
    long long passSeq_ = 0;
    // 上传/下载等独立小提交的同步对象：与 pass fence 分开，避免轮转复用时
    // 把"下载等待"误判成"pass 复用等待"
    VkFence xferFence_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    // push descriptor 版布局（仅 hasPushDesc_ 时创建）：pipelineLayout 要与
    // pipeline 一致，所以两种模式各建一个 layout，创建 pipeline 时按模式选用
    VkDescriptorSetLayout setLayoutPush_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayoutPush_ = VK_NULL_HANDLE;
    // 轮转 descriptor set 池（非 push 模式的回退优化）：一次 allocate 预取全部，
    // dispatch 直接取用（vs 每次 vkAllocateDescriptorSets）。pass 同步完成后
    // set 全部空闲，回卷无需显式 reset
    QVector<VkDescriptorSet> setRing_;
    int setRingIdx_ = 0;

    VkBuffer arena_ = VK_NULL_HANDLE;
    VkDeviceMemory arenaMem_ = VK_NULL_HANDLE;
    quint64 arenaBytes_ = 0;
    quint64 budget_ = 0;
    quint64 weightBump_ = 0;   // 从 0 向上
    quint64 actBump_ = 0;      // 从 weightLimit_ 向上
    quint64 weightLimit_ = 0;  // 权重区上限（与激活区分界）

    // host-visible staging 环：权重/输入上传与输出回读都走它
    VkBuffer staging_ = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem_ = VK_NULL_HANDLE;
    void *stagingMap_ = nullptr;
    quint64 stagingBytes_ = 0;
    quint64 stagingBump_ = 0;

    QHash<QString, VkPipeline> pipelines_;
    DeviceInfo info_;
    int deviceIndex_ = -1;
    QString lastError_;
    bool passOpen_ = false;
    VkPipeline boundPipe_ = VK_NULL_HANDLE;   // 连续同名 kernel 时跳过重复 bind
    bool fp16Feature_ = false;   // shaderFloat16 + storageBuffer16BitAccess（硬件能力）
    bool hasPushDesc_ = false;   // VK_KHR_push_descriptor（device 扩展）
    int generation_ = 0;   // 每次 Recover 成功 +1，日志里能区分新旧设备
};

// 显存预算（MB）：入口统一，便于日志里复现
quint64 BudgetMbFromEnv();

}  // namespace vk
