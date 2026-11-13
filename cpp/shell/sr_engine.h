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

// 引擎是否可用（ORT 库 + 当前模型都能找到且会话创建成功）。失败原因见 Status()。
bool Available();
// 一行状态：可用时 "waifu2x cunet 2× 降噪3 · CPU · 192px tile"；不可用时给出原因
QString Status();
QString ModelPath();
QString RuntimePath();

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

// 回归：对同一张图跑一批模型，逐条写日志（--sr-model-test）
bool ModelSweep(const QStringList &models, const QSize &probeSize, const QString &outDir,
                QString *report);

}  // namespace SrEngine
