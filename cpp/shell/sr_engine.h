// 超分引擎（ONNX Runtime，waifu2x / Real-CUGAN / Real-ESRGAN / SRMD）
//
// 为什么单独一层：
//   * ONNX Runtime 是**运行时加载**（QLibrary），不把 19MB DLL 写进链接依赖；
//     缺 DLL / 缺模型时只返回不可用原因，不影响阅读器其它功能；
//   * 推理在后台线程里跑（调用方用 QtConcurrent），这里只保证 Run 串行，
//     因为 ORT 的 session 虽线程安全，但 CPU 推理并发两页只会互相拖慢。
//
// 模型从哪来：
//   * Android：随 APK 打包在 assets/sr_models/（见 ui/CMakeLists.txt），
//     通过 QFile("assets:/sr_models/x.onnx") 读进内存后用 CreateSessionFromArray；
//     同时支持 JM_SR_MODEL_DIR / 应用数据目录下的真实文件（覆盖优先）。
//   * 桌面：程序目录 sr_models/（构建时拷进去）、JM_SR_MODEL_DIR。
//
// 几何：不同架构的输出边长不同（cunet 2x 输入 192 只出 312；1x 出 136；
// Real-CUGAN / Real-ESRGAN / SRMD 基本是严格的 N 倍）。这里不猜、不读 metadata，
// 会话建立后用一块全 0 的 tile 做**探针推理**量出真实输出边长，再据此算收缩边距，
// 因此新增模型不需要改任何代码。
#pragma once

#include <QImage>
#include <QString>
#include <QVector>

#include <atomic>
#include <functional>
#include <vector>

namespace SrEngine {

// 模型目录里的一项（对齐 android/sr_qnn/models/models.txt）
struct ModelInfo {
    QString name;    // 显示名，同时是唯一键（写入 ui.ini 的 srModelName）
    QString file;    // onnx 文件名
    QString arch;    // waifu2x / realcugan / realesrgan / srmd
    int scale = 2;   // 原生放大倍数（1 = 只降噪）
    int denoise = 0; // 降噪等级 0~3
    int tile = 192;  // 模型固定输入边长
    QString note;    // 界面说明
    // ---- 运行时探测（Catalog() 每次重新扫描）----
    bool installed = false;
    qint64 bytes = 0;
};

// 全量模型目录（含未安装项：installed=false）。按 arch 排序，便于界面分组。
QVector<ModelInfo> Catalog();
// 只返回磁盘/APK 里真实存在、可以立刻推理的模型
QVector<ModelInfo> InstalledModels();
// 主模型目录（第一个可用的真实目录；Android 上优先应用数据目录）
QString ModelsDir();
// 报告用：模型搜索路径（一行，SEP 分隔）
QString ModelSearchPath();
// 用户按需安装的模型包目录（受管、可卸载删除）。桌面/移动端都落在应用数据目录下。
// 由 SrPlugin 使用；此处暴露给 UI 与 CLI 判断"是否在插件目录里"。
QString PluginsDir();

// 推理模块（ONNX Runtime 动态库）按需下载目录。桌面端应用内可从 GitHub 下载
// onnxruntime.dll 到这里，SrEngine 会优先加载它（找不到才用安装目录那份）。
QString RuntimeDir();
// 卸载已加载的 onnxruntime（换/更新运行时后调用，下次推理重建会话即生效）。
void UnloadRuntime();

// 引擎是否可用（ORT 库 + 当前模型都能找到且会话创建成功）。失败原因见 Status()。
bool Available();
// 一行状态：可用时 "waifu2x cunet 2× 降噪3 · CPU · 192px tile"；不可用时给出原因
QString Status();
QString ModelPath();
QString RuntimePath();

// ---- 推理后端（auto / qnn / vulkan / cpu / ov-npu / xpu）----
// auto：Qualcomm QNN -> Vulkan GPU -> CPU（Android 走独立 QNN 子进程）。
// ov-npu / xpu：桌面 Intel OpenVINO 插件 EP，分别固定跑 AI Boost NPU 与
//   AUTO(NPU->GPU->CPU)。插件 DLL 不随包，需 JM_OV_EP_DIR / JM_OV_EP_DLL
//   指定（或放在程序目录 ovep/）；缺失时按 Vulkan -> CPU 显式回退。
// JM_SR_BACKEND 覆盖 ui.ini [sr] backend（env 优先）。
QString BackendPref();      // 规范化后的偏好值
QString BackendName();      // 实际生效：cpu / qnn-htp / qnn-cpu / vulkan / ov-npu / ov-xpu
QString BackendLabel();     // 界面用："CPU（ONNX Runtime）" / "QNN HTP（NPU）"
// 后端选择的补充说明（回退/降级原因）；没有可说的返回空串
QString BackendDetail();
// QNN 独立进程状态（Android；桌面返回空串）。排障用。
QString QnnProcStatus();
// QNN 子进程日志尾部（排障；桌面返回空串）。
QString QnnProcLogTail();
// 回归：ASCII 一行（pref=auto chosen=cpu ort=... qnn=absent note=...）
QString BackendReport();
// 切换后端偏好并立刻释放会话（下一次推理按新偏好重建）。
// persist=true 时写 ui.ini [sr] backend；测速这类临时切换必须传 false，
// 否则中途退出会把"临时用 NPU 测一下"变成用户的永久偏好。
bool SetBackendPreference(const QString &pref, bool persist = true);

// ---- Vulkan tile 偏好与性能测试（设置页"超分"组）----
// tile 偏好：0=自动（按平台默认 + watchdog 降档），32~512（8 对齐）为显式设置。
// env JM_SR_TILE 优先于 ui.ini [sr] tile。SRMD/QDQ 模型不支持改 tile，仍用 192。
int TilePreference();
bool SetTilePreference(int tile);   // 0 表示恢复自动
// 当前模型在 Vulkan 路径实际将使用的 tile（含平台默认、watchdog 降档与用户
// 偏好）；模型未就绪时返回 -1。真机 vk-report 的默认 tile 也走这里，
// 保证验收 CLI 与生产链路一致。
int TileForCurrentModel();

// 性能测试的一行结果（同步阻塞，调用方自行放到工作线程；内部持有引擎锁，
// 测试期间其它推理会排队）。测试会重建 Vulkan 会话，结束时恢复当前模型。
struct SrPerfRow {
    QString label;      // "GPU（tile 64）" / "CPU（tile 192）" / "NPU（QNN HTP，tile 192）"
    bool ok = false;    // Vulkan 档可能因 DEVICE_LOST / 显存不足失败
    double ms = 0.0;    // Vulkan：median；CPU / NPU：单次
    qint64 actMb = -1;  // Vulkan 激活区占用（MB）；CPU / NPU 无对应值为 -1
    // 该档推理后的进程内存净增量（MB，含会话/权重/激活驻留）。QNN HTP 的
    // 权重在 HTP 共享内存，host RSS 反映的是注册表与 I/O 缓冲部分。
    qint64 memDeltaMb = -1;
    QString note;       // 失败原因 / 补充说明
};
// 对当前模型跑一组建档基准：Vulkan 按 candidates 逐档（iterations 次 median），
// NPU（QNN HTP 就绪时）tile192 一次，CPU 固定 tile192 一次。
// candidates 为空时取 {64, TileForModel 默认值}。
// modelFile 非空时用该模型（按 onnx 文件名，如 waifu2x_cunet_up2x_denoise3.onnx）
// 测 NPU 档，便于验证"非量化模型在 HTP 上的真实表现"；空则用当前模型。
QVector<SrPerfRow> RunPerfProbe(const QVector<int> &vulkanTiles, int iterations,
                               const QString &modelFile = QString());
// 依据 RunPerfProbe 结果生成推荐文案（后端优先级 + tile 大小 + 相对其它硬件
// 的倍数）。NPU 跑赢时推荐 NPU 优先，其次是 GPU，最后 CPU。
QString PerfRecommendation(const QVector<SrPerfRow> &rows);

// 验证"量化模型在 NPU 上算得对不对"：同一块 192×192 合成图分别走 QNN 与 CPU，
// 比较输出。返回一行 ASCII 报告（compare=ok out=WxH maxAbs=.. psnr=..dB ...）；
// npuMs/cpuMs/maxAbs/psnr 可为空。QDQ 图在 HTP 上跑通但量化参数或 tile 尺寸
// 不匹配时，耗时正常而画面是错的 —— 这个接口就是补那一刀。
QString CompareNpuCpu(const QString &modelFile, int *npuMs = nullptr, int *cpuMs = nullptr,
                      double *maxAbs = nullptr, double *psnr = nullptr);

// 当前模型（显示名）。未显式设置时取 ui.ini 里的 srModelName，再退回第一个可用的 2x 模型。
QString CurrentModel();
// 切换模型：释放旧会话，下次推理时重建。名字非法或文件缺失返回 false 且不改状态。
bool SetModel(const QString &name);

// 当前模型的放大倍数（选模型后立刻可用；未初始化时返回模型的标称倍数）
int CurrentScale();

struct Result {
    QImage image;
    double ms = 0.0;
    QString error;
    QString model;
    int scale = 0;
};

// 当前模型 2x/3x/4x 超分。失败时 image 为空、error 有原因。
Result Upscale(const QImage &src);
// 指定模型 + 进度（按行） + 取消标志。progress 为空表示不关心进度。
Result UpscaleEx(const QImage &src, const QString &model,
                 const std::function<void(int done, int total)> &progress = nullptr,
                 const std::atomic_bool *cancel = nullptr);

// 进程退出前释放 ORT 对象（可重复调用）
void Shutdown();

// 回归：生成 64x64 渐变图跑一次当前模型，验证 DLL/模型/分块/输出尺寸
bool SelfTest(QString *report, QImage *out = nullptr);

// ---- 模型列表测速（ui/sr_bench.*）----
// 一块可测的硬件。id：gpu0/gpu1/...（物理设备序号）/ npu / cpu。
// 不可用的项也会返回（available=false + note），界面只在图例里标一次。
struct BenchHw {
    QString id;
    QString shortLabel;   // GPU0 / NPU / CPU
    QString label;        // GPU0 · RTX 3060
    bool available = false;
    QString note;         // 不可用原因
};
// 枚举可用硬件（GPU 逐个物理设备）。首次调用会建 Vulkan 实例并探测 QNN，
// 调用方应缓存结果。
QVector<BenchHw> BenchHardware();

struct BenchSample {
    QString hwId;
    bool ok = false;
    double ms = 0.0;
    QString note;         // 失败原因
};

// 对指定模型跑一组建档：GPU 用 SrVk 单 tile 中位数，NPU 走 QNN HTP 单次，
// CPU 走 ORT 单次。三者都是 192×192 单块，可直接比较。
// 同步阻塞（内部持引擎锁）；调用方放工作线程。progress 在每块硬件开始前回调，
// cancel 置位后在硬件之间尽快返回（已出的结果照常返回）。
QVector<BenchSample> BenchModel(const QString &modelName, const QVector<QString> &hwIds,
                                int tile = 192, int gpuIterations = 3,
                                const std::function<void(const QString &hwId)> &progress = nullptr,
                                const std::atomic_bool *cancel = nullptr);

// 测速收尾：释放会话、把 Vulkan 设备切回默认（resetGpu=true 时），
// 并按 restoreModel 重建会话。调用方必须在测速结束后调用一次。
void BenchFinish(const QString &restoreModel, bool resetGpu);

// 回归：对同一张图跑一批模型，逐条写日志（--sr-model-test）
// 调试：用 ORT 跑同一个 tile（与 Vulkan 逐像素对比用），不影响当前会话
bool DebugOrtTile(const QString &modelPath, const std::vector<float> &input,
                  std::vector<float> *output, int *outW, int *outH, QString *error);
bool ModelSweep(const QStringList &models, const QSize &probeSize, const QString &outDir,
                QString *report);

}  // namespace SrEngine
