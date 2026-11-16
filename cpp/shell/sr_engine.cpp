#include "sr_engine.h"

#include "vk/sr_vk.h"
#include "vk/vk_api.h"

#ifdef _WIN32
#define PSAPI_VERSION 1
#include <windows.h>
#include <psapi.h>
#endif

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QMutexLocker>
#include <QRecursiveMutex>
#include <QRegularExpression>
#include <QHash>
#include <QStandardPaths>

#ifdef Q_OS_ANDROID
#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

#include "onnxruntime_c_api.h"

#if defined(__has_include)
#  if __has_include("app_paths.h")
#    include "app_paths.h"
#    define JM_SR_HAS_APPPATHS 1
#  endif
#endif

namespace {

constexpr int kDefaultTile = 192;
constexpr float kInv255 = 1.0f / 255.0f;

struct Spec {
    const char *name;
    const char *file;
    const char *arch;
    int scale;
    int denoise;
    int tile;
    const char *note;
};

// 与 android/sr_qnn/models/models.txt 一一对应（这是本地模型定义，不是接口数据）。
// 同一个 onnx 被多条 products 引用的情况（waifu2x ANIME / CUNET）这里只留一条，
// 界面上一项就是一个可执行的选择。
const Spec kCatalog[] = {
    // ---- waifu2x cunet 2x（动漫，默认）----
    {"waifu2x cunet 2x 降噪3", "waifu2x_cunet_up2x_denoise3.onnx", "waifu2x", 2, 3, 192,
     "动漫风格 2 倍 + 强降噪，通用首选"},
    {"waifu2x cunet 2x 降噪2", "waifu2x_cunet_up2x_denoise2.onnx", "waifu2x", 2, 2, 192,
     "降噪略弱，线条保留更多"},
    {"waifu2x cunet 2x 降噪1", "waifu2x_cunet_up2x_denoise1.onnx", "waifu2x", 2, 1, 192,
     "轻度降噪，适合本来干净的图源"},
    {"waifu2x cunet 2x 降噪0", "waifu2x_cunet_up2x_denoise0.onnx", "waifu2x", 2, 0, 192,
     "不降噪，只放大"},
    {"waifu2x cunet 2x", "waifu2x_cunet_up2x.onnx", "waifu2x", 2, 0, 192,
     "标准 cunet 2 倍"},
    // ---- waifu2x cunet 1x（只降噪）----
    {"waifu2x cunet 1x 降噪3", "waifu2x_cunet_up1x_denoise3.onnx", "waifu2x", 1, 3, 192,
     "不放大，只做最强降噪"},
    {"waifu2x cunet 1x 降噪2", "waifu2x_cunet_up1x_denoise2.onnx", "waifu2x", 1, 2, 192,
     "不放大，中等降噪"},
    {"waifu2x cunet 1x 降噪1", "waifu2x_cunet_up1x_denoise1.onnx", "waifu2x", 1, 1, 192,
     "不放大，轻度降噪"},
    {"waifu2x cunet 1x 降噪0", "waifu2x_cunet_up1x_denoise0.onnx", "waifu2x", 1, 0, 192,
     "不放大不降噪（等价整流一遍）"},
    // ---- Real-CUGAN SE ----
    {"Real-CUGAN SE 2x", "realcugan_se_up2x.onnx", "realcugan", 2, 0, 192,
     "线条锐利，黑白漫画友好"},
    {"Real-CUGAN SE 2x 保守", "realcugan_se_up2x_conservative.onnx", "realcugan", 2, 0, 192,
     "收敛更强，细节改动更小"},
    {"Real-CUGAN SE 2x 降噪1", "realcugan_se_up2x_denoise1.onnx", "realcugan", 2, 1, 192,
     "轻度降噪 + 2 倍"},
    {"Real-CUGAN SE 2x 降噪2", "realcugan_se_up2x_denoise2.onnx", "realcugan", 2, 2, 192,
     "中度降噪 + 2 倍"},
    {"Real-CUGAN SE 2x 降噪3", "realcugan_se_up2x_denoise3.onnx", "realcugan", 2, 3, 192,
     "强降噪 + 2 倍"},
    {"Real-CUGAN SE 3x", "realcugan_se_up3x.onnx", "realcugan", 3, 0, 192,
     "3 倍放大，细节保留好"},
    {"Real-CUGAN SE 3x 保守", "realcugan_se_up3x_conservative.onnx", "realcugan", 3, 0, 192,
     "3 倍放大，改动更保守"},
    {"Real-CUGAN SE 3x 降噪3", "realcugan_se_up3x_denoise3.onnx", "realcugan", 3, 3, 192,
     "强降噪 + 3 倍"},
    {"Real-CUGAN SE 4x", "realcugan_se_up4x.onnx", "realcugan", 4, 0, 192,
     "4 倍放大，适合小图"},
    {"Real-CUGAN SE 4x 保守", "realcugan_se_up4x_conservative.onnx", "realcugan", 4, 0, 192,
     "4 倍放大，改动更保守"},
    {"Real-CUGAN SE 4x 降噪3", "realcugan_se_up4x_denoise3.onnx", "realcugan", 4, 3, 192,
     "强降噪 + 4 倍"},
    // ---- Real-CUGAN Pro ----
    {"Real-CUGAN Pro 2x", "realcugan_pro_up2x.onnx", "realcugan", 2, 0, 192,
     "Pro 模型，纹理更强"},
    {"Real-CUGAN Pro 2x 保守", "realcugan_pro_up2x_conservative.onnx", "realcugan", 2, 0, 192,
     "Pro 模型，改动保守"},
    {"Real-CUGAN Pro 2x 降噪3", "realcugan_pro_up2x_denoise3.onnx", "realcugan", 2, 3, 192,
     "Pro 模型，强降噪 + 2 倍"},
    {"Real-CUGAN Pro 3x", "realcugan_pro_up3x.onnx", "realcugan", 3, 0, 192,
     "Pro 模型，3 倍放大"},
    {"Real-CUGAN Pro 3x 保守", "realcugan_pro_up3x_conservative.onnx", "realcugan", 3, 0, 192,
     "Pro 模型，3 倍且保守"},
    {"Real-CUGAN Pro 3x 降噪3", "realcugan_pro_up3x_denoise3.onnx", "realcugan", 3, 3, 192,
     "Pro 模型，强降噪 + 3 倍"},
    // ---- Real-ESRGAN ----
    {"Real-ESRGAN anime 2x", "realesrgan_animevideo_v3_up2x.onnx", "realesrgan", 2, 0, 192,
     "动画视频向，速度快"},
    {"Real-ESRGAN anime 3x", "realesrgan_animevideo_v3_up3x.onnx", "realesrgan", 3, 0, 192,
     "动画视频向，3 倍"},
    {"Real-ESRGAN anime 4x", "realesrgan_animevideo_v3_up4x.onnx", "realesrgan", 4, 0, 192,
     "动画视频向，4 倍"},
    {"Real-ESRGAN x4plus 4x", "realesrgan_x4plus_up4x.onnx", "realesrgan", 4, 0, 192,
     "通用 4 倍，模型最大最慢"},
    {"Real-ESRGAN x4plus anime 4x", "realesrgan_x4plus_anime_up4x.onnx", "realesrgan", 4, 0, 192,
     "x4plus 动漫版，比通用版轻"},
    {"Real-ESRGAN general 4x", "realesrgan_general_up4x.onnx", "realesrgan", 4, 0, 192,
     "通用照片向 4 倍"},
    {"Real-ESRGAN general wdn 4x", "realesrgan_general_wdn_up4x.onnx", "realesrgan", 4, 0, 192,
     "通用 4 倍，带降噪"},
    // ---- SRMD ----
    {"SRMD 2x", "srmd_x2.onnx", "srmd", 2, 0, 192, "经典 SRMD，2 倍"},
    {"SRMD 3x", "srmd_x3.onnx", "srmd", 3, 0, 192, "经典 SRMD，3 倍"},
    {"SRMD 4x", "srmd_x4.onnx", "srmd", 4, 0, 192, "经典 SRMD，4 倍"},
    {"SRMD-NF 2x", "srmdnf_x2.onnx", "srmd", 2, 0, 192, "SRMD 无降噪版，2 倍"},
    {"SRMD-NF 3x", "srmdnf_x3.onnx", "srmd", 3, 0, 192, "SRMD 无降噪版，3 倍"},
    {"SRMD-NF 4x", "srmdnf_x4.onnx", "srmd", 4, 0, 192, "SRMD 无降噪版，4 倍"},
};

struct State {
    QLibrary *lib = nullptr;
    const OrtApi *api = nullptr;
    OrtEnv *env = nullptr;
    OrtSessionOptions *opts = nullptr;
    OrtSession *session = nullptr;
    OrtMemoryInfo *mem = nullptr;
    OrtAllocator *alloc = nullptr;
    std::string inName;
    std::string outName;
    int outW = 0;
    int outH = 0;
    int scale = 2;
    int tile = kDefaultTile;
    QString modelName;
    QString modelPath;
    QString runtime;
    QString err;
    bool libTried = false;
    bool probed = false;
    // ---- 推理后端 ----
    QString backend = QStringLiteral("cpu");   // cpu / qnn-htp / qnn-cpu
    QString backendNote;                       // 降级/回退原因（给人看）
    QStringList providers;                     // GetAvailableProviders 的结果
    bool qnnReady = false;                     // 库与 EP 都就绪（探测一次）
    bool qnnProbed = false;
    int qnnTier = 0;                           // 最终成功的 QNN 选项档位
    QString qnnNote;                           // QNN 专属状态说明（独立于 backendNote）
    // ---- Intel OpenVINO 插件 EP（Windows 桌面；CPU / GPU / NPU）
    // 插件是 env 级注册，ReleaseAll() 会释放 OrtEnv，所以这些状态必须一起重置。
    uint32_t ortApi = 0;                       // 实际拿到的 OrtApi 版本（插件 EP 需 >=22）
    bool ovProbed = false;
    bool ovRegistered = false;
    bool ovAutoEp = false;                     // 是否枚举到 OpenVINOExecutionProvider.AUTO
    QString ovNote;                            // 注册/设备枚举失败原因
    QString ovPluginPath;
    QStringList ovDevices;                     // ov_device 取值（NPU / GPU.0 / CPU，去重）
    // 设备指纹与 tile 上限：缓存 blob 绑定驱动/编译器/插件版本，不能跨版本复用。
    int ovMaxTiles = 0;
    int ovTiles = 0;
    QString ovFp;
    QString ovDriver, ovCompiler, ovPlugin, ovArch;
    QString ovCacheDir;                        // 本次会话实际使用的 CACHE_DIR
    QString ovAdviceNote;                      // tile 归一化吞吐标定的决策说明
    // ---- Vulkan 后端（QNN -> Vulkan -> CPU 的中间档）
    SrVk::Session *vk = nullptr;
    bool vkReady = false;
    bool vkProbed = false;
    QRecursiveMutex mutex;
};

State &S()
{
    static State s;
    return s;
}

QString AppDir()
{
    return QCoreApplication::applicationDirPath();
}

bool IsAssetPath(const QString &p)
{
    return p.startsWith(QStringLiteral("assets:"));
}

// QFileInfo::exists 对 assets: 走不到文件引擎，统一用 QFile 判一次。
bool PathExists(const QString &p)
{
    if (p.isEmpty())
        return false;
    if (!IsAssetPath(p))
        return QFileInfo::exists(p);
    QFile f(p);
    return f.exists();
}

qint64 PathSize(const QString &p)
{
    if (!IsAssetPath(p))
        return QFileInfo(p).size();
    QFile f(p);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const qint64 n = f.size();
    f.close();
    return n;
}

QString FindRuntime()
{
    QStringList cands;
    const QString envDir = qEnvironmentVariable("JM_ORT_DIR");
#ifdef Q_OS_ANDROID
    const QString libName = QStringLiteral("libonnxruntime.so");
    if (!envDir.isEmpty())
        cands << QDir(envDir).filePath(libName);
    // nativeLibraryDir 在 DT_RUNPATH 里：裸名即可命中随 APK 打包的那份
    cands << QDir(AppDir()).filePath(libName) << libName;
#else
    const QString libName = QStringLiteral("onnxruntime.dll");
    if (!envDir.isEmpty())
        cands << QDir(envDir).filePath(libName);
    cands << QDir(AppDir()).filePath(libName)
          << QDir(AppDir()).filePath(QStringLiteral("lib/onnxruntime.dll"));
#endif
    for (const QString &p : cands) {
        if (p == libName || QFileInfo::exists(p))
            return p;
    }
    return QString();
}

QStringList ModelDirs()
{
    QStringList dirs;
    const QString envDir = qEnvironmentVariable("JM_SR_MODEL_DIR");
    if (!envDir.isEmpty())
        dirs << envDir;
#ifdef Q_OS_ANDROID
    // 随 APK 打包的模型（assets/sr_models）。放最前：不占应用私有空间。
    dirs << QStringLiteral("assets:/sr_models");
#endif
    dirs << QDir(AppDir()).filePath(QStringLiteral("sr_models"))
         << QDir(AppDir()).filePath(QStringLiteral("models"));
#ifdef JM_SR_HAS_APPPATHS
    dirs << QDir(AppPaths::DataDir()).filePath(QStringLiteral("sr_models"))
         << QDir(AppPaths::CacheDir()).filePath(QStringLiteral("sr_models"));
#endif
    dirs.removeDuplicates();
    return dirs;
}

QString ResolveFile(const QString &file)
{
    if (file.isEmpty())
        return QString();
    // 显式覆盖：JM_SR_MODEL 指到一个具体文件时优先（回归/单模型验服用）
    const QString envFile = qEnvironmentVariable("JM_SR_MODEL");
    if (!envFile.isEmpty() && QFileInfo(envFile).fileName() == file && QFileInfo::exists(envFile))
        return envFile;
    for (const QString &d : ModelDirs()) {
        const QString p = d + QLatin1Char('/') + file;
        if (PathExists(p))
            return p;
    }
    return QString();
}

// ---------------------------------------------------------------- 量化模型（QDQ）
// 目录里若存在 *_q16* / *lowq* 的 ONNX，就作为"量化模型"动态加入目录。
// 这些权重是 QNN HTP 专用（uint16 激活 + uint8 权重，即 w8a16）：Vulkan 的 fp32
// 管线会明确拒绝（Graph::load 里检查 QuantizeLinear/DequantizeLinear），QNN HTP
// 才是它们的正常落点；ORT CPU 能执行但慢（--vk-quant-test 39/39 只证明可跑）。
// 让用户能选、能跑，并在状态行如实显示实际后端，比彻底隐藏更有用。
// 数组是静态存储，Spec 里的 const char* 指向稳定的 QByteArray，不会悬空。
struct DynStorage
{
    QByteArray names[128];
    QByteArray files[128];
    QByteArray notes[128];
    Spec specs[128];
    int count = 0;
};

DynStorage &Dyn()
{
    static DynStorage d;
    static bool built = false;
    if (built)
        return d;
    built = true;
    const QString note = QStringLiteral("QDQ 量化模型（w8a16，QNN NPU 优先，Vulkan 不支持）");
    QStringList seen;
    for (const QString &dir : ModelDirs()) {
        QDir qd(dir);
        const QStringList fs = qd.entryList(
            {QStringLiteral("*_q16*.onnx"), QStringLiteral("*lowq*.onnx")}, QDir::Files,
            QDir::Name);
        for (const QString &f : fs) {
            if (seen.contains(f) || d.count >= 128)
                continue;
            seen.append(f);
            const int i = d.count++;
            // 倍数从文件名解析。两种命名都要认：
            //   *_up2x*（realcugan / realesrgan / waifu2x）与 *_x2*（srmd），
            // 只认前者会让 srmd_x3_q16 被当成 2 倍——QNN 协议里的目标尺寸会算错。
            int scale = 2;
            for (int s = 2; s <= 4; ++s) {
                if (f.contains(QStringLiteral("_up%1x").arg(s))) {
                    scale = s;
                    break;
                }
                if (f.contains(QStringLiteral("_x%1").arg(s))) {
                    scale = s;
                    break;
                }
            }
            d.files[i] = f.toUtf8();
            d.names[i] = (QStringLiteral("量化 · ") + QFileInfo(f).completeBaseName()).toUtf8();
            d.notes[i] = note.toUtf8();
            d.specs[i] = Spec{d.names[i].constData(), d.files[i].constData(), "quant", scale, 0,
                              192, d.notes[i].constData()};
        }
    }
    return d;
}

const Spec *SpecByName(const QString &name)
{
    for (const Spec &s : kCatalog) {
        if (name == QString::fromUtf8(s.name))
            return &s;
    }
    DynStorage &d = Dyn();
    for (int i = 0; i < d.count; ++i) {
        if (name == QString::fromUtf8(d.specs[i].name))
            return &d.specs[i];
    }
    return nullptr;
}

const Spec *SpecByFile(const QString &file)
{
    for (const Spec &s : kCatalog) {
        if (file == QString::fromUtf8(s.file))
            return &s;
    }
    DynStorage &d = Dyn();
    for (int i = 0; i < d.count; ++i) {
        if (file == QString::fromUtf8(d.specs[i].file))
            return &d.specs[i];
    }
    return nullptr;
}

QString PrefModelName()
{
    // 直接读 ui.ini 的 [ui] srModelName：不引 UiPrefs，避免 sr_engine 反向依赖 UI 层。
    const QStringList files = {
        qEnvironmentVariable("JM_UI_CONFIG").isEmpty()
            ? QString()
            : QDir(qEnvironmentVariable("JM_UI_CONFIG")).filePath(QStringLiteral("ui.ini")),
#ifdef JM_SR_HAS_APPPATHS
        AppPaths::ConfigFile(QStringLiteral("ui.ini")),
#endif
        QDir(AppDir()).filePath(QStringLiteral("config/ui.ini")),
    };
    for (const QString &f : files) {
        if (f.isEmpty() || !QFileInfo::exists(f))
            continue;
        QSettings ini(f, QSettings::IniFormat);
        const QString v = ini.value(QStringLiteral("ui/srModelName")).toString();
        if (!v.isEmpty())
            return v;
        // 旧版只有下拉框索引（srModel 0/1/2）——迁移到具体模型名
        if (ini.contains(QStringLiteral("ui/srModel"))) {
            const int idx = ini.value(QStringLiteral("ui/srModel")).toInt();
            static const char *kLegacy[] = {"waifu2x cunet 2x 降噪3", "Real-CUGAN SE 2x",
                                            "Real-CUGAN Pro 2x"};
            if (idx >= 0 && idx < 3)
                return QString::fromUtf8(kLegacy[idx]);
        }
    }
    return QString();
}

QString FirstUsableModel()
{
    // 优先 2x 动漫模型（速度/质量平衡），再退回任意已安装项
    const char *preferred[] = {"waifu2x_cunet_up2x_denoise3.onnx", "waifu2x_cunet_up2x.onnx",
                              "realcugan_se_up2x.onnx"};
    for (const char *file : preferred) {
        const QString f = QString::fromUtf8(file);
        if (ResolveFile(f).isEmpty())
            continue;
        if (const Spec *spec = SpecByFile(f))
            return QString::fromUtf8(spec->name);
    }
    for (const Spec &s : kCatalog) {
        if (!ResolveFile(QString::fromUtf8(s.file)).isEmpty())
            return QString::fromUtf8(s.name);
    }
    return QString();
}

void ReleaseSession()
{
    State &s = S();
    if (s.vk) {
        s.vk->reset();
        delete s.vk;
        s.vk = nullptr;
    }
    s.vkReady = false;
    if (s.api) {
        if (s.session)
            s.api->ReleaseSession(s.session);
        if (s.mem)
            s.api->ReleaseMemoryInfo(s.mem);
    }
    s.session = nullptr;
    s.mem = nullptr;
    s.probed = false;
    s.outW = 0;
    s.outH = 0;
}

#ifdef Q_OS_ANDROID
void QnnProcStop();   // 定义见下方 QNN 独立进程客户端
#endif

void ReleaseAll()
{
    State &s = S();
#ifdef Q_OS_ANDROID
    QnnProcStop();
#endif
    ReleaseSession();
    if (s.api) {
        if (s.opts)
            s.api->ReleaseSessionOptions(s.opts);
        if (s.env)
            s.api->ReleaseEnv(s.env);
    }
    s.opts = nullptr;
    s.env = nullptr;
    s.alloc = nullptr;
    s.libTried = false;
    s.api = nullptr;
    // 运行时换掉了：后端能力与档位必须重新探测（换模型时也走这里）
    s.providers.clear();
    s.qnnProbed = false;
    s.qnnReady = false;
    s.qnnTier = 0;
    // env 被释放后插件 EP 的注册与 OrtEpDevice 指针一并失效，下次重新探测。
    s.ovProbed = false;
    s.ovRegistered = false;
    s.ovAutoEp = false;
    s.ovNote.clear();
    s.ovPluginPath.clear();
    s.ovDevices.clear();
    s.ovMaxTiles = 0;
    s.ovTiles = 0;
    s.ovFp.clear();
    s.ovCacheDir.clear();
    s.ovAdviceNote.clear();
    s.backend = QStringLiteral("cpu");
    s.backendNote.clear();
    if (s.lib) {
        s.lib->unload();
        delete s.lib;
        s.lib = nullptr;
    }
}

bool EnsureRuntime()
{
    State &s = S();
    if (s.api && s.env)
        return true;
    if (s.libTried)
        return s.api != nullptr && s.env != nullptr;
    s.libTried = true;

    s.runtime = FindRuntime();
    s.lib = new QLibrary(s.runtime.isEmpty() ? QStringLiteral("onnxruntime") : s.runtime);
    if (!s.lib->load()) {
#ifdef Q_OS_ANDROID
        s.err = QStringLiteral("缺少 libonnxruntime.so（应随 APK 打包在 lib/<abi>/ 下）");
#else
        s.err = QStringLiteral("缺少 onnxruntime.dll（放到程序目录，或设置 JM_ORT_DIR）");
#endif
        return false;
    }
    using GetApiBaseFn = const OrtApiBase *(*)();
    auto getBase = reinterpret_cast<GetApiBaseFn>(s.lib->resolve("OrtGetApiBase"));
    if (!getBase) {
        s.err = QStringLiteral("onnxruntime 库缺少 OrtGetApiBase");
        return false;
    }
    const OrtApiBase *base = getBase();
    // ORT 的 C API 是"前缀稳定"的结构体：老版本返回的 OrtApi 前 N 个函数指针
    // 与新版本完全一致。设备侧随包的 .so 可能与头文件版本不同，依次向下兼容。
    uint32_t usedApi = ORT_API_VERSION;
    if (base) {
        static const uint32_t kVersions[] = {ORT_API_VERSION, 22, 20, 18, 16, 14, 12};
        for (uint32_t v : kVersions) {
            s.api = base->GetApi(v);
            if (s.api) {
                usedApi = v;
                s.ortApi = v;   // 插件 EP 的 C API 需要 >=22，后面据此门控
                break;
            }
        }
    }
    if (!s.api) {
        s.err = QStringLiteral("onnxruntime 版本过旧（需要 API %1，实际 %2）")
                    .arg(ORT_API_VERSION)
                    .arg(base ? QString::fromUtf8(base->GetVersionString()) : QStringLiteral("?"));
        return false;
    }
    OrtStatus *st = s.api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "jmcomic-sr", &s.env);
    if (st) {
        s.err = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
        s.env = nullptr;
        return false;
    }
    qInfo("sr: ORT API %u (header %d), runtime=%s", usedApi, ORT_API_VERSION,
          qPrintable(s.runtime));
    return true;
}

bool RunTile(const std::vector<float> &input, std::vector<float> &output, int &outW, int &outH,
             int tile);

// ---------------------------------------------------------------------------
// 推理后端选择（QNN/HTP vs CPU）
//
// 与 Python src/tools/sr_backend.py 的语义对齐：
//   auto（默认）—— 有 QNN 运行库就走 QNN，没有就 CPU；绝不因为"想用 NPU"
//                  把能出图的 CPU 路径挡掉（那就从"慢"变成"不可用"）；
//   qnn —— 强制尝试，失败回退 CPU 并把真实原因留在 note 里；
//   cpu —— 完全跳过 QNN（排查"是不是 NPU 的问题"）。
//
// 为什么 C++ 侧只需要探测两件事：
//   1. ORT 这份 .so/.dll 是否编了 QNN EP（GetAvailableProviders 里有没有 QNN）；
//   2. QNN 后端运行库（libQnnHtp.so / QnnHtp.dll）在不在——ORT 的 QNN EP 是
//      dlopen 后端 DSO 的，缺库时 SetupBackend 失败、GetCapability 返回空，
//      最后"会话能建但一个节点都没接住"，跑在 CPU 上却以为在用 NPU。
//      所以缺库直接判不可用，不去建那个"看起来成功"的会话。
// ---------------------------------------------------------------------------

// 运行期从 Vulkan 回退 CPU 时，临时把后端偏好锁定为 cpu（只影响这一次
// EnsureModel，不改 ui.ini / 环境变量，用户下次主动切换仍按原偏好走）。
bool g_forceCpuOnce = false;
// 进程内的后端偏好覆盖：SetBackendPreference(pref, persist=false) 写这里。
// 0.96.6 之前 persist=false 只是"释放会话"，偏好本身没有落到任何地方 ——
// 于是"临时切 qnn 测一下"和"临时切 cpu 对比"都还是按 ui.ini 的 auto 走，
// 量化模型对比时 CPU 档实际跑的是 NPU（maxAbs=0、耗时一模一样）。
QString g_prefOverride;

// tile 参数化：39 个 ONNX 模型把输入维度写死为 192（ORT 对其它尺寸直接报
// "Got invalid dimensions for input: x"），所以 CPU / QNN 路径必须保持 192。
// Vulkan 执行器按输入动态推断形状，允许 JM_SR_TILE / ui.ini [sr] tile 覆盖
// （32~512、按 8 对齐），用于移动端用小 tile 换显存与单步延迟。
// SRMD 与 QDQ 量化模型例外：图里有固定形状常量，换 tile 会算错通道数。
// 运行期从 Vulkan 回退 CPU 时 s.tile 会变回 192，UpscaleEx 负责整图重跑一次。
// userSet 出参：true 表示 env/ui.ini 里有用户显式设置（tile 自定义生效），
// false 表示返回的是平台默认值（此时 watchdog 降档仍可生效）。
int TileOverride(bool *userSet)
{
    if (userSet)
        *userSet = false;
    bool ok = false;
    int v = qEnvironmentVariableIntValue("JM_SR_TILE", &ok);
    if (ok && v >= 32) {
        if (userSet)
            *userSet = true;
    } else {
        QStringList files;
        const QString cfg = qEnvironmentVariable("JM_UI_CONFIG");
        if (!cfg.isEmpty())
            files << QDir(cfg).filePath(QStringLiteral("ui.ini"));
#ifdef JM_SR_HAS_APPPATHS
        files << AppPaths::ConfigFile(QStringLiteral("ui.ini"));
#endif
        files << QDir(AppDir()).filePath(QStringLiteral("config/ui.ini"));
        for (const QString &f : files) {
            if (f.isEmpty() || !QFileInfo::exists(f))
                continue;
            QSettings ini(f, QSettings::IniFormat);
            const int t = ini.value(QStringLiteral("sr/tile")).toInt();
            if (t >= 32) {
                v = t;
                if (userSet)
                    *userSet = true;
                break;
            }
        }
    }
    if (v < 32) {
#ifdef Q_OS_ANDROID
        // 移动 GPU 与 CPU 共享内存、带宽有限：默认把 Vulkan tile 降到 128。
        // 激活区约降到 (128/192)^2 ≈ 44%，3x/4x 模型才落得进 384MB 预算；
        // CPU / QNN 路径仍用模型固定的 192（见 TileForModel）。
        v = 128;
#else
        return 0;
#endif
    }
    return qBound(32, (v / 8) * 8, 512);
}

int TileForModel(const Spec *spec, bool forVulkan)
{
    const int base = spec->tile > 0 ? spec->tile : kDefaultTile;
    if (!forVulkan)
        return base;   // ORT CPU / QNN：模型输入维度固定，不能改
    bool userSet = false;
    const int over = TileOverride(&userSet);
    if (over <= 0 || over == base)
        return base;
    if (QString::fromUtf8(spec->arch) == QLatin1String("srmd")
        || QString::fromUtf8(spec->arch) == QLatin1String("quant")) {
        // SRMD 图里有 [1,1,192,192] 固定形状常量；QDQ 量化模型的输入维度
        // 同样被 ORT 钉死在 192（tile=64 实测报 invalid dimensions）。
        qWarning("sr: tile override %d ignored for %s (model requires fixed %d)", over, spec->file,
                 base);
        return base;
    }
#ifdef Q_OS_ANDROID
    // Adreno watchdog 降档（B 方案）：整图 1714 步单 submit 的 RRDB 深网络
    // （x4plus / general）tile128 单 pass ≈7s，超过 Adreno 驱动 watchdog
    // （~4s）触发 DEVICE_LOST；tile64 实测 3.8s/tile 通过且比 CPU 快 3.4×。
    // 仅在用户未显式设置 tile 时生效（设置页"Tile 大小"优先）。
    if (!userSet && over < 192) {
        static constexpr const char *kWatchdogFiles[] = {
            "x4plus", "realesrgan_general",
        };
        for (const char *k : kWatchdogFiles) {
            if (QLatin1String(spec->file).contains(QLatin1String(k))) {
                qInfo("sr: watchdog tile %d -> 64 for %s (single-pass too long for Adreno)",
                      over, spec->file);
                return 64;
            }
        }
    }
#endif
    qInfo("sr: tile override %d -> %d for %s", base, over, spec->file);
    return over;
}

// ---- Intel OpenVINO 插件 EP（ORT >= 1.22 的 plugin EP API）-------------------
// 与 QNN 不同：插件 EP 不需要重编 onnxruntime.dll，运行时注册一个导出
// CreateEpFactories 的 DLL，再用 GetEpDevices() 按 ov_device 元数据挑具体设备。
// 插件 DLL 必须与 openvino.dll / TBB / NPU 编译器 / openvino_runtime.manifest
// 在同一目录，否则 Windows 的 SxS assembly 解析会报 14001。
QString OvPluginPath()
{
    const QString dll = qEnvironmentVariable("JM_OV_EP_DLL").trimmed();
    if (!dll.isEmpty())
        return dll;
    const QString name = QStringLiteral("onnxruntime_providers_openvino_plugin.dll");
    QStringList dirs;
    const QString dir = qEnvironmentVariable("JM_OV_EP_DIR").trimmed();
    if (!dir.isEmpty())
        dirs << dir;
    dirs << QDir(AppDir()).filePath(QStringLiteral("ovep"));
    dirs << AppDir();
    for (const QString &d : dirs) {
        const QString p = QDir(d).filePath(name);
        if (QFileInfo::exists(p))
            return p;
    }
    return QString();
}

bool OvEnsureRegistered()
{
    State &s = S();
    if (s.ovProbed)
        return s.ovRegistered;
    s.ovProbed = true;
    if (!s.api || !s.env) {
        s.ovNote = QStringLiteral("ORT 运行时未就绪");
        return false;
    }
    if (s.ortApi < 22) {
        s.ovNote = QStringLiteral("ORT API %1 过低（插件 EP 需 >=22）").arg(s.ortApi);
        return false;
    }
    const QString dll = OvPluginPath();
    if (dll.isEmpty()) {
        s.ovNote = QStringLiteral("未找到 onnxruntime_providers_openvino_plugin.dll"
                                  "（设置 JM_OV_EP_DIR 或 JM_OV_EP_DLL）");
        return false;
    }
#ifdef _WIN32
    // 插件与 OpenVINO/TBB/编译器同目录；NPU 驱动还会按绝对路径去 DriverStore
    // 加载编译器 DLL，那份 DLL 的 tbb/openvino 依赖靠这里才能解析到同一套 runtime。
    const std::wstring dirW =
        QDir::toNativeSeparators(QFileInfo(dll).absolutePath()).toStdWString();
    SetDllDirectoryW(dirW.c_str());
#endif
    // ORTCHAR_T 在 Windows 是 wchar_t、其它平台是 char：插件本身只有 Windows
    // 的 .dll（OvPluginPath 在别处必然返回空、走不到这里），但这段代码仍要能编过。
    OrtStatus *st = nullptr;
#ifdef _WIN32
    const std::wstring pathW = QDir::toNativeSeparators(dll).toStdWString();
    st = s.api->RegisterExecutionProviderLibrary(s.env, "OpenVINOExecutionProvider",
                                                 pathW.c_str());
#else
    const QByteArray pathA = QFile::encodeName(QDir::toNativeSeparators(dll));
    st = s.api->RegisterExecutionProviderLibrary(s.env, "OpenVINOExecutionProvider",
                                                 pathA.constData());
#endif
    if (st) {
        s.ovNote = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
        return false;
    }
    const OrtEpDevice *const *devs = nullptr;
    size_t n = 0;
    st = s.api->GetEpDevices(s.env, &devs, &n);
    if (st) {
        s.ovNote = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const char *ep = s.api->EpDevice_EpName(devs[i]);
        const OrtKeyValuePairs *md = s.api->EpDevice_EpMetadata(devs[i]);
        const char *ov = md ? s.api->GetKeyValue(md, "ov_device") : nullptr;
        if (!ep || !ov)
            continue;
        if (std::strcmp(ep, "OpenVINOExecutionProvider") == 0) {
            const QString d = QString::fromUtf8(ov);
            if (!s.ovDevices.contains(d))
                s.ovDevices << d;
            // 插件 EP 把 OpenVINO 的设备属性直接摊在 OrtEpDevice 的 metadata 里，
            // 不需要再链 OpenVINO C API 就能拿到 NPU_MAX_TILES 与版本指纹。
            if (std::strcmp(ov, "NPU") == 0 && s.ovMaxTiles <= 0) {
                auto meta = [&](const char *k) -> QString {
                    const char *v = md ? s.api->GetKeyValue(md, k) : nullptr;
                    return v ? QString::fromUtf8(v) : QString();
                };
                s.ovDriver = meta("NPU_DRIVER_VERSION");
                s.ovCompiler = meta("NPU_COMPILER_VERSION");
                s.ovPlugin = meta("version");
                s.ovArch = meta("DEVICE_ARCHITECTURE");
                s.ovMaxTiles = meta("NPU_MAX_TILES").toInt();
            }
        } else if (std::strcmp(ep, "OpenVINOExecutionProvider.AUTO") == 0) {
            s.ovAutoEp = true;
        }
    }
    if (!s.ovDevices.contains(QStringLiteral("NPU"))) {
        s.ovNote = QStringLiteral("插件已注册但未枚举到 Intel NPU（devices=%1）")
                       .arg(s.ovDevices.join(QLatin1Char(',')));
        return false;
    }
    // NPU 3720 有 2 个 tile，只给 1 个实测慢 2.4×；必须按设备上限定，
    // 且不能超过 metadata 报告值（不同 SKU 不能用硬编码的 2）。
    if (s.ovMaxTiles <= 0)
        s.ovMaxTiles = 1;
    int tiles = s.ovMaxTiles;
    const int over = qEnvironmentVariable("JM_OV_TILES").toInt();
    if (over > 0) {
        tiles = qBound(1, over, s.ovMaxTiles);
        if (tiles != over)
            qWarning("sr: JM_OV_TILES=%d clamped to device max %d", over, s.ovMaxTiles);
    }
    s.ovTiles = tiles;
    const QString pluginHash = QString::fromLatin1(
        QCryptographicHash::hash(s.ovPlugin.toUtf8(), QCryptographicHash::Sha1)
            .toHex().left(8));
    s.ovFp = QStringLiteral("%1-%2-%3-%4")
                 .arg(s.ovDriver.isEmpty() ? QStringLiteral("drv") : s.ovDriver,
                      s.ovCompiler.isEmpty() ? QStringLiteral("cc") : s.ovCompiler,
                      s.ovArch.isEmpty() ? QStringLiteral("arch") : s.ovArch, pluginHash);
    s.ovPluginPath = dll;
    s.ovRegistered = true;
    qInfo("sr: OpenVINO EP registered devices=[%s] auto=%d plugin=%s",
          qPrintable(s.ovDevices.join(QLatin1Char(','))), int(s.ovAutoEp),
          qPrintable(dll));
    return true;
}

// tile 标定表路径：JM_OV_TILE_ADVICE > AppPaths 配置目录 > 程序目录 config/。
QString OvAdvicePath()
{
    const QString env = qEnvironmentVariable("JM_OV_TILE_ADVICE").trimmed();
    if (!env.isEmpty())
        return env;
#ifdef JM_SR_HAS_APPPATHS
    const QString cfg = AppPaths::ConfigFile(QStringLiteral("jm_ov_tile_advice.json"));
    if (!cfg.isEmpty() && QFileInfo::exists(cfg))
        return cfg;
#endif
    return QDir(AppDir()).filePath(QStringLiteral("config/jm_ov_tile_advice.json"));
}

// 按"归一化吞吐"（扣除重叠拼接后的有效输出像素/秒）选 tile。
// 标定表带驱动/编译器/架构指纹，跨设备或驱动升级后自动失效，避免误用。
// ORT 插件 EP 不支持 reshape_input（实测 I/O size mismatch），非 192 的 tile
// 必须由 bench_intel_xpu.py --group tile-advice 离线生成 ONNX 变体才可用。
bool OvResolveModel(const QString &modelPath, int baseTile, int *tile, QString *path)
{
    *tile = baseTile;
    *path = modelPath;
    State &s = S();
    // 标定表在 EnsureModel 早期读取，此时插件可能还没注册、设备指纹为空：
    // 按需先注册（有 ovProbed 保护，不会重复加载），auto/qnn 档不受影响。
    if (s.ovDriver.isEmpty() && s.ovCompiler.isEmpty() && s.ovArch.isEmpty()
        && !OvEnsureRegistered())
        return false;
    if (s.ovDriver.isEmpty() && s.ovCompiler.isEmpty() && s.ovArch.isEmpty())
        return false;
    const QString file = OvAdvicePath();
    if (file.isEmpty() || !QFileInfo::exists(file))
        return false;
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly))
        return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (!doc.isObject())
        return false;
    const QJsonObject root = doc.object();
    const QJsonObject fp = root.value(QStringLiteral("fingerprint")).toObject();
    if (fp.value(QStringLiteral("NPU_DRIVER_VERSION")).toString() != s.ovDriver
        || fp.value(QStringLiteral("NPU_COMPILER_VERSION")).toString() != s.ovCompiler
        || fp.value(QStringLiteral("DEVICE_ARCHITECTURE")).toString() != s.ovArch) {
        s.ovAdviceNote = QStringLiteral("tile 标定表设备指纹不匹配，已忽略");
        qWarning("sr: %s", qPrintable(s.ovAdviceNote));
        return false;
    }
    const QJsonObject entry = root.value(QStringLiteral("models"))
                                  .toObject()
                                  .value(QFileInfo(modelPath).fileName())
                                  .toObject();
    if (entry.isEmpty())
        return false;
    // measured_best 是实测最优，effective_best 是过了显著性门槛后才允许应用的
    // 采用值：1~10% 的增益落在噪声里，不值得多一个 5MB 变体和更多拼接重叠。
    const int measured = entry.value(QStringLiteral("best")).toInt(baseTile);
    const int best = entry.value(QStringLiteral("effective_best")).toInt(measured);
    const int base = entry.value(QStringLiteral("base")).toInt(baseTile);
    const double gain = entry.value(QStringLiteral("gain")).toDouble(1.0);
    s.ovAdviceNote = QStringLiteral("归一化吞吐标定 采用=%1 实测=%2 增益=%3x（base=%4）")
                         .arg(best)
                         .arg(measured)
                         .arg(gain, 0, 'f', 2)
                         .arg(base);
    if (best <= 0 || best == baseTile) {
        s.ovAdviceNote += QStringLiteral("，保持 %1").arg(baseTile);
        return true;
    }
    const QString variant = entry.value(QStringLiteral("variant")).toString();
    if (variant.isEmpty() || variant.startsWith(QStringLiteral("ERROR"))) {
        s.ovAdviceNote += QStringLiteral("，缺少变体模型，保持 %1").arg(baseTile);
        return true;
    }
    const QString vp = QDir(QFileInfo(modelPath).absolutePath()).filePath(variant);
    if (!PathExists(vp)) {
        s.ovAdviceNote += QStringLiteral("，变体 %1 不存在，保持 %2").arg(variant).arg(baseTile);
        return true;
    }
    *tile = best;
    *path = vp;
    s.ovAdviceNote += QStringLiteral("，使用变体 %1").arg(variant);
    return true;
}

// 编译缓存目录：blob 与驱动 / 编译器 / 插件版本强绑定，指纹变化即换目录，
// 旧 blob 自然失效（OpenVINO 明确不保证跨版本兼容）。目录必须可写，
// 写不了就宁可禁用缓存，也不要退回程序目录（权限/升级后路径漂移）。
QString OvCacheDirFor(const QString &modelPath, int tile, const QString &mode)
{
    State &s = S();
    if (qEnvironmentVariable("JM_OV_CACHE").trimmed() == QLatin1String("0"))
        return QString();
    QString root = qEnvironmentVariable("JM_OV_CACHE_DIR").trimmed();
    if (root.isEmpty()) {
#ifdef JM_SR_HAS_APPPATHS
        root = AppPaths::CacheDir();
        if (root.isEmpty() || !AppPaths::IsWritable(root))
            root = AppPaths::DataDir();
        if (!root.isEmpty() && !AppPaths::IsWritable(root))
            root.clear();
#else
        root = QDir(AppDir()).filePath(QStringLiteral("cache"));
#endif
    }
    if (root.isEmpty())
        return QString();
    const QFileInfo fi(modelPath);
    const QString key = QStringLiteral("%1|%2|%3|%4|%5")
                            .arg(fi.fileName())
                            .arg(fi.size())
                            .arg(tile)
                            .arg(mode, s.ovFp);
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha1).toHex().left(16));
    // 用 std::filesystem 逐级建目录：QDir::mkpath 在部分 Qt/Windows 组合下
    // 对多级相对路径会返回 false，error_code 能直接给出失败原因。
    const QString dir = QDir::fromNativeSeparators(
        root + QLatin1Char('/') + QStringLiteral("ov_npu/") + hash);
    std::error_code ec;
    std::filesystem::create_directories(dir.toStdString(), ec);
    if (ec) {
        qWarning() << "sr: OpenVINO cache mkdir failed" << dir << ec.message().c_str();
        return QString();
    }
    qInfo() << "sr: OpenVINO cache dir=" << dir << "fp=" << s.ovFp;
    return dir;
}

// 给 SessionOptions 挂 OpenVINO EP。mode: "ov-npu"（只选 NPU）/ "xpu"（AUTO）。
bool OvAppendEp(OrtSessionOptions *opts, const QString &mode, const QString &modelPath,
                int tile, QString *why)
{
    State &s = S();
    if (!OvEnsureRegistered()) {
        if (why)
            *why = s.ovNote;
        return false;
    }
    const bool wantAuto = mode == QLatin1String("xpu");
    if (wantAuto && !s.ovAutoEp) {
        if (why)
            *why = QStringLiteral("插件未提供 OpenVINOExecutionProvider.AUTO");
        return false;
    }
    const OrtEpDevice *const *devs = nullptr;
    size_t n = 0;
    OrtStatus *st = s.api->GetEpDevices(s.env, &devs, &n);
    if (st) {
        if (why)
            *why = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
        return false;
    }
    const char *wantEp = wantAuto ? "OpenVINOExecutionProvider.AUTO"
                                  : "OpenVINOExecutionProvider";
    QVector<const OrtEpDevice *> picked;
    if (wantAuto) {
        // AUTO 按传入顺序挑设备：NPU 优先，其次 GPU，最后 CPU。
        for (const char *want : {"NPU", "GPU", "CPU"}) {
            for (size_t i = 0; i < n; ++i) {
                const char *ep = s.api->EpDevice_EpName(devs[i]);
                const OrtKeyValuePairs *md = s.api->EpDevice_EpMetadata(devs[i]);
                const char *ov = md ? s.api->GetKeyValue(md, "ov_device") : nullptr;
                if (!ep || !ov || std::strcmp(ep, wantEp) != 0)
                    continue;
                const bool match = std::strcmp(want, "GPU") == 0
                                       ? std::strncmp(ov, "GPU", 3) == 0
                                       : std::strcmp(ov, want) == 0;
                if (match) {
                    picked << devs[i];
                    break;
                }
            }
        }
    } else {
        for (size_t i = 0; i < n; ++i) {
            const char *ep = s.api->EpDevice_EpName(devs[i]);
            const OrtKeyValuePairs *md = s.api->EpDevice_EpMetadata(devs[i]);
            const char *ov = md ? s.api->GetKeyValue(md, "ov_device") : nullptr;
            if (ep && ov && std::strcmp(ep, wantEp) == 0 && std::strcmp(ov, "NPU") == 0) {
                picked << devs[i];
                break;
            }
        }
    }
    if (picked.isEmpty()) {
        if (why)
            *why = QStringLiteral("没有匹配的 OpenVINO EP 设备（mode=%1）").arg(mode);
        return false;
    }
    // load_config 的顶层每个键都必须是"设备名 -> 属性对象"：把 CACHE_DIR 写成
    // 顶层标量会让插件静默回退到 CPU EP（实测），必须放进 NPU 对象里。
    QVector<QByteArray> store;
    store.reserve(6);
    QVector<const char *> keys, vals;
    s.ovCacheDir.clear();
    if (!wantAuto) {
        QJsonObject npu;
        npu.insert(QStringLiteral("INFERENCE_PRECISION_HINT"), QStringLiteral("f16"));
        // 只给 1 个 tile 实测慢 2.4×；按设备 metadata 的上限显式钉死。
        if (s.ovTiles > 0)
            npu.insert(QStringLiteral("NPU_TILES"), s.ovTiles);
        const QString cache = OvCacheDirFor(modelPath, tile, mode);
        s.ovCacheDir = cache;
        if (!cache.isEmpty())
            npu.insert(QStringLiteral("CACHE_DIR"), QDir::toNativeSeparators(cache));
        QJsonObject cfg;
        cfg.insert(QStringLiteral("NPU"), npu);
        store << QByteArray("load_config")
              << QJsonDocument(cfg).toJson(QJsonDocument::Compact);
        keys << store.at(0).constData();
        vals << store.at(1).constData();
        qInfo("sr: OpenVINO NPU tiles=%d/%d cache=%s", s.ovTiles, s.ovMaxTiles,
              cache.isEmpty() ? "off" : qPrintable(cache));
    }
    // AUTO 传多个 OrtEpDevice 时插件会为每个 device 各写一次同名配置并告警，
    // 所以 AUTO 档不传 load_config（也就没有编译缓存），保持原有行为。
    const QString dump = qEnvironmentVariable("JM_OV_DUMP_SUBGRAPHS").trimmed();
    if (!dump.isEmpty()) {
        store << QByteArray("dump_subgraphs") << dump.toUtf8();
        keys << store.at(store.size() - 2).constData();
        vals << store.at(store.size() - 1).constData();
    }
    st = s.api->SessionOptionsAppendExecutionProvider_V2(
        opts, s.env, picked.constData(), size_t(picked.size()), keys.constData(),
        vals.constData(), size_t(keys.size()));
    if (st) {
        if (why)
            *why = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
        return false;
    }
    return true;
}

QString NormalizeBackend(const QString &raw)
{
    const QString v = raw.trimmed().toLower();
    if (v == QLatin1String("qnn") || v == QLatin1String("htp") || v == QLatin1String("npu"))
        return QStringLiteral("qnn");
    if (v == QLatin1String("cpu") || v == QLatin1String("ort"))
        return QStringLiteral("cpu");
    if (v == QLatin1String("vulkan") || v == QLatin1String("vk") || v == QLatin1String("gpu"))
        return QStringLiteral("vulkan");
    // Intel OpenVINO 插件 EP：ov-npu 固定跑 NPU；xpu 走 AUTO(NPU->GPU->CPU)。
    // 注意 "npu" 仍是 Qualcomm QNN 的别名（Android 兼容），不能抢。
    if (v == QLatin1String("ov-npu") || v == QLatin1String("ovnpu")
        || v == QLatin1String("intel-npu") || v == QLatin1String("openvino-npu")
        || v == QLatin1String("ov"))
        return QStringLiteral("ov-npu");
    if (v == QLatin1String("xpu") || v == QLatin1String("intel-xpu")
        || v == QLatin1String("ov-auto") || v == QLatin1String("openvino-auto"))
        return QStringLiteral("xpu");
    return QStringLiteral("auto");
}

QString PrefBackend()
{
    if (g_forceCpuOnce)
        return QStringLiteral("cpu");
    // 进程内覆盖（性能测试/数值对比的临时切换）优先于环境变量与 ui.ini
    if (!g_prefOverride.isEmpty())
        return g_prefOverride;
    const QString env = qEnvironmentVariable("JM_SR_BACKEND");
    if (!env.isEmpty())
        return NormalizeBackend(env);
    const QStringList files = {
        qEnvironmentVariable("JM_UI_CONFIG").isEmpty()
            ? QString()
            : QDir(qEnvironmentVariable("JM_UI_CONFIG")).filePath(QStringLiteral("ui.ini")),
#ifdef JM_SR_HAS_APPPATHS
        AppPaths::ConfigFile(QStringLiteral("ui.ini")),
#endif
        QDir(AppDir()).filePath(QStringLiteral("config/ui.ini")),
    };
    for (const QString &f : files) {
        if (f.isEmpty() || !QFileInfo::exists(f))
            continue;
        QSettings ini(f, QSettings::IniFormat);
        const QString v = ini.value(QStringLiteral("sr/backend")).toString();
        if (!v.isEmpty())
            return NormalizeBackend(v);
    }
    return QStringLiteral("auto");
}

// QNN 后端 DSO 的探测：Android 是 libQnnHtp.so，Windows/Linux 是 QnnHtp.dll/.so。
// 允许 JM_QNN_BACKEND 直接给出完整路径（与 Python 同名环境变量一致）。
QString QnnBackendPath()
{
    const QString env = qEnvironmentVariable("JM_QNN_BACKEND").trimmed();
    if (!env.isEmpty())
        return env;
    QStringList names;
#ifdef Q_OS_WIN
    names << QStringLiteral("QnnHtp.dll");
#else
    names << QStringLiteral("libQnnHtp.so");
#endif
    QStringList dirs;
#ifdef Q_OS_ANDROID
    dirs << AppDir() << QStringLiteral(".");   // nativeLibraryDir 在 DT_RUNPATH 里
#else
    const QString ortDir = qEnvironmentVariable("JM_ORT_DIR");
    if (!ortDir.isEmpty())
        dirs << ortDir;
    dirs << AppDir() << QDir(AppDir()).filePath(QStringLiteral("qnn"));
#endif
    for (const QString &d : dirs) {
        for (const QString &n : names) {
            const QString p = QDir(d).filePath(n);
            if (QFileInfo::exists(p))
                return p;
        }
    }
    return QString();
}

QStringList AvailableProviders()
{
    State &s = S();
    QStringList out;
    if (!s.api)
        return out;
    char **names = nullptr;
    int count = 0;
    if (s.api->GetAvailableProviders(&names, &count) != nullptr || !names)
        return out;
    for (int i = 0; i < count; ++i)
        out << QString::fromUtf8(names[i] ? names[i] : "");
    s.api->ReleaseAvailableProviders(names, count);
    return out;
}

// 档位与 Python sr_qnn.cpp 的 tier 3/2/1/0 一致：从"最快"开始，逐级只减不加。
// 任何一档失败都说明该机型/Gradle 包不支持对应选项，退一档再试，最后才退 CPU。
QVector<QPair<QString, QString>> QnnOptions(const QString &backendPath, int tier)
{
    QVector<QPair<QString, QString>> opts;
    auto add = [&opts](const char *k, const QString &v) {
        opts.append({QString::fromLatin1(k), v});
    };
    auto envOr = [](const char *key, const char *def) {
        const QString v = qEnvironmentVariable(key).trimmed();
        return v.isEmpty() ? QString::fromLatin1(def) : v;
    };

    add("backend_path", backendPath);
    add("device_id", envOr("JM_QNN_DEVICE", "0"));
    add("profiling_level", QStringLiteral("off"));
    if (tier >= 1)
        add("enable_htp_fp16_precision", envOr("JM_QNN_FP16", "1"));
    if (tier >= 2) {
        add("htp_performance_mode", envOr("JM_QNN_PERF", "burst"));
        add("htp_graph_finalization_optimization_mode", envOr("JM_QNN_FINALIZE", "2"));
        add("qnn_context_priority", envOr("JM_QNN_PRIORITY", "high"));
        if (qEnvironmentVariableIsEmpty("JM_QNN_SHARED_MEM")
            || qEnvironmentVariable("JM_QNN_SHARED_MEM") != QLatin1String("0"))
            add("enable_htp_shared_memory_allocator", QStringLiteral("1"));
        const QString soc = qEnvironmentVariable("JM_QNN_SOC_MODEL").trimmed();
        if (!soc.isEmpty())
            add("soc_model", soc);
        const QString arch = qEnvironmentVariable("JM_QNN_HTP_ARCH").trimmed();
        if (!arch.isEmpty())
            add("htp_arch", arch);
    }
    if (tier >= 3)
        add("vtcm_mb", envOr("JM_QNN_VTCM", "8"));
    return opts;
}

// ---------------------------------------------------------------------------
// QNN 独立进程客户端（仅 Android）
//
// 为什么不能在 App 进程里跑 QNN：HTP 后端要 dlopen 厂商库 libcdsprpc.so
// (FastRPC)。App 的 linker namespace 只允许 nativeLibraryDir 与
// public.libraries.txt 里的库，vivo 等机型不列 libcdsprpc -> 初始化直接挂在
// futex（真机实测无限等待）。子进程用 LD_LIBRARY_PATH 能加载 /vendor/lib64
// 的那份 —— local-dream(xororz) 与 Python 链的 libsr_qnn_proc 都是这个做法。
//
// 引擎随 APK 分发为 libsr_qnn_proc.so；gradle 用
// packaging.jniLibs.useLegacyPackaging=true 让安装器把它解压到
// nativeLibraryDir，子进程才能 exec。
//
// 协议（stdin/stdout 锁步一问一答，小端裸字段）：
//   1 hello -> 版本/后端/htp/模型表；2 add -> 推理；3 stop
// ---------------------------------------------------------------------------
#ifdef Q_OS_ANDROID
struct QnnProcState
{
    pid_t pid = -1;
    int wfd = -1;
    int rfd = -1;
    bool up = false;
    bool htp = false;
    int cores = 0;
    int modelId = -1;          // 当前模型在引擎表里的 id
    QString version;
    QString backend;
    QString htpError;
    QString note;
    QHash<QString, int> idByFile;      // onnx 文件名(小写) -> id
    QHash<QString, int> idByKey;       // 去掉扩展名的大写名 -> id
    QVector<QPair<int, int>> scaleOf;  // (id, scale) 供同倍数兜底
};
QnnProcState g_qp;
QString g_qnnLogPath;   // 子进程 stderr 落盘位置（排障时 dump 到 sr-perf.txt）

// 本 so 所在目录就是 nativeLibraryDir（引擎与 proc 同目录）
QString QnnLibDir()
{
    Dl_info info;
    if (dladdr(reinterpret_cast<void *>(&RunTile), &info) != 0 && info.dli_fname != nullptr)
        return QFileInfo(QString::fromUtf8(info.dli_fname)).absolutePath();
    return QString();
}

void QnnProcStop()
{
    if (g_qp.wfd >= 0) {
        const uint32_t cmd = 3;
        (void)!write(g_qp.wfd, &cmd, sizeof(cmd));
    }
    if (g_qp.wfd >= 0)
        close(g_qp.wfd);
    if (g_qp.rfd >= 0)
        close(g_qp.rfd);
    if (g_qp.pid > 0) {
        // 先给它 200ms 自己退，再强杀，避免留僵尸
        bool reaped = false;
        for (int i = 0; i < 20 && !reaped; ++i) {
            int st = 0;
            if (waitpid(g_qp.pid, &st, WNOHANG) == g_qp.pid)
                reaped = true;
            else
                usleep(10 * 1000);
        }
        if (!reaped) {
            kill(g_qp.pid, SIGKILL);
            int st = 0;
            waitpid(g_qp.pid, &st, 0);
        }
    }
    const QString keepNote = g_qp.note;
    g_qp = QnnProcState();
    g_qp.note = keepNote;   // 保留最后一次失败原因，供 sr-perf 诊断行显示
}

bool QpWriteAll(const void *data, size_t len)
{
    const char *p = static_cast<const char *>(data);
    size_t put = 0;
    while (put < len) {
        const ssize_t n = write(g_qp.wfd, p + put, len - put);
        if (n <= 0)
            return false;
        put += size_t(n);
    }
    return true;
}

bool QpReadAll(void *data, size_t len, int timeoutMs)
{
    char *p = static_cast<char *>(data);
    size_t got = 0;
    while (got < len) {
        struct pollfd fd
        {
            g_qp.rfd, POLLIN, 0
        };
        const int pr = poll(&fd, 1, timeoutMs);
        if (pr <= 0)
            return false;
        const ssize_t n = read(g_qp.rfd, p + got, len - got);
        if (n <= 0)
            return false;
        got += size_t(n);
    }
    return true;
}

bool QpReadStr(QString *out, int cap)
{
    QByteArray buf(cap, '\0');
    if (!QpReadAll(buf.data(), size_t(cap), 60000))
        return false;
    *out = QString::fromUtf8(buf.constData());
    return true;
}

bool QpWriteBlob(const void *data, int len)
{
    const uint32_t n = uint32_t(len);
    return QpWriteAll(&n, sizeof(n)) && (len <= 0 || QpWriteAll(data, size_t(len)));
}

bool QpReadBlob(QByteArray *out, int timeoutMs)
{
    uint32_t n = 0;
    if (!QpReadAll(&n, sizeof(n), timeoutMs))
        return false;
    if (n > (64u << 20))
        return false;   // 异常长度，别把内存打爆
    out->resize(int(n));
    return n == 0 || QpReadAll(out->data(), size_t(n), timeoutMs);
}

// 引擎在子进程里读不了 APK assets（那是 App 进程的 Qt 文件引擎能力）：
// 模型打包在 assets 时，把当前要用的那个解到 filesDir/qnn_models 再启动。
QString QnnPrepareModels(const QString &wantFile, QString *why)
{
    const QString dir = SrEngine::ModelsDir();
    if (!dir.startsWith(QLatin1String("assets:")))
        return dir;
    if (wantFile.isEmpty()) {
        if (why)
            *why = QStringLiteral("模型在 assets 里但未指定文件名");
        return QString();
    }
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath();
    const QString outDir = base + QStringLiteral("/qnn_models");
    if (!QDir().mkpath(outDir)) {
        if (why)
            *why = QStringLiteral("无法创建模型缓存目录 %1").arg(outDir);
        return QString();
    }
    const QString dst = outDir + QLatin1Char('/') + wantFile;
    const QString srcPath = dir + QLatin1Char('/') + wantFile;
    QFile src(srcPath);
    if (!src.open(QIODevice::ReadOnly)) {
        if (why)
            *why = QStringLiteral("内置模型读不到：%1").arg(srcPath);
        return QString();
    }
    const qint64 size = src.size();
    const QFileInfo dstInfo(dst);
    if (dstInfo.exists() && dstInfo.size() == size)
        return outDir;   // 已缓存
    QFile dstFile(dst);
    if (!dstFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (why)
            *why = QStringLiteral("写模型缓存失败：%1").arg(dst);
        return QString();
    }
    qint64 copied = 0;
    while (!src.atEnd()) {
        const QByteArray chunk = src.read(4 << 20);
        if (chunk.isEmpty())
            break;
        if (dstFile.write(chunk) != chunk.size()) {
            dstFile.close();
            QFile::remove(dst);
            if (why)
                *why = QStringLiteral("写模型缓存失败（磁盘空间不足？）");
            return QString();
        }
        copied += chunk.size();
    }
    dstFile.close();
    src.close();
    if (copied != size) {
        QFile::remove(dst);
        if (why)
            *why = QStringLiteral("模型拷贝不完整");
        return QString();
    }
    qInfo("sr: qnn model cached %s (%.1fMB)", qPrintable(dst), double(size) / 1048576.0);
    return outDir;
}

// 【当前未启用】DSP 路径交给引擎默认（PrepareQnnSearchPath 会把厂商目录排在
// 前面，签名 skel 优先），实测可用；保留此函数仅为将来需要接管时参考。
// 历史记录：QNN 的 HTP backend 通过 DSP_LIBRARY_PATH 找 skel（不是 ADSP_LIBRARY_PATH）。
// DSP 侧读不到 /data/app 下的 nativeLibraryDir，必须把 skel 放到 app 私有目录
// 再让 FastRPC 去取 —— local-dream 正是把 assets 里的 skel 复制到
// filesDir/runtime_libs，再把 DSP_LIBRARY_PATH 指向它。
QString QnnPrepareDspLibs(const QString &libDir, QString *why)
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath();
    const QString outDir = base + QStringLiteral("/qnn_dsp");
    if (!QDir().mkpath(outDir)) {
        if (why)
            *why = QStringLiteral("无法创建 DSP 库目录 %1").arg(outDir);
        return QString();
    }
    const QDir src(libDir);
    const QStringList skels =
        src.entryList({QStringLiteral("libQnnHtpV*Skel.so")}, QDir::Files);
    for (const QString &name : skels) {
        const QString from = src.filePath(name);
        const QString to = outDir + QLatin1Char('/') + name;
        const QFileInfo fiFrom(from);
        const QFileInfo fiTo(to);
        if (fiTo.exists() && fiTo.size() == fiFrom.size())
            continue;
        if (!QFile::copy(from, to)) {
            if (why)
                *why = QStringLiteral("复制 %1 到 %2 失败").arg(name, outDir);
            return QString();
        }
    }
    if (skels.isEmpty()) {
        if (why)
            *why = QStringLiteral("nativeLibraryDir 里没有 skel（libQnnHtpV*Skel.so）");
        return QString();
    }
    return outDir;
}

// 【当前未启用】真正让 HTP 跑通的是把 libcdsprpc.so 随 jniLibs 打进
// nativeLibraryDir；复制到 filesDir 这条路在 Android 10+ 走不通（app_data_file
// 不允许 dlopen）。保留此函数作为排障参考与其它机型的备选。
// 原始说明：libQnnHtpV79Stub.so 的 NEEDED 里有 libcdsprpc.so(FastRPC 客户端)，但 app 的
// linker namespace 的 default_library_paths 只有 /system/lib64:/system_ext/lib64，
// permitted_paths 里也没有 /vendor/lib64 —— 直接 dlopen 绝对路径会报
// "is not accessible for the namespace"，裸名同理（C++ 的 dlopen 不走 Java 层
// libnativeloader 的 public.libraries 白名单，那份名单只对 System.loadLibrary 生效）。
// QnnDevice_create 一碰 FastRPC 就失败，报 QNN_DEVICE_ERROR_INVALID_CONFIG。
// 但 permitted_paths 里有 /data：把厂商库复制到 app 私有目录、再让 LD_LIBRARY_PATH
// 指过去，Stub 的依赖就能解析（local-dream 把整套 QNN 运行时放 filesDir 是同一道理）。
QString QnnPrepareFastRpcLib(QString *why)
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (base.isEmpty())
        base = QDir::homePath();
    const QString outDir = base + QStringLiteral("/qnn_libs");
    if (!QDir().mkpath(outDir)) {
        if (why)
            *why = QStringLiteral("无法创建 FastRPC 库目录 %1").arg(outDir);
        return QString();
    }
    // 三个都要复制：libcdsprpc 的 NEEDED 里是 libvmmem / vendor.qti.hardware.dsp-V1-ndk，
    // 少一个都会在依赖解析时失败。
    const QStringList names = {
        QStringLiteral("libcdsprpc.so"),
        QStringLiteral("libvmmem.so"),
        QStringLiteral("vendor.qti.hardware.dsp-V1-ndk.so"),
    };
    int copied = 0;
    for (const QString &name : names) {
        const QString dst = outDir + QLatin1Char('/') + name;
        const QFileInfo dstInfo(dst);
        if (dstInfo.exists() && dstInfo.size() > 0) {
            ++copied;
            continue;
        }
        const QStringList cands = {
            QStringLiteral("/vendor/lib64/") + name,
            QStringLiteral("/odm/lib64/") + name,
        };
        bool okOne = false;
        for (const QString &src : cands) {
            QFile in(src);
            if (!in.exists() || !in.open(QIODevice::ReadOnly))
                continue;
            const QByteArray blob = in.readAll();
            in.close();
            QFile out(dst);
            if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
                continue;
            if (blob.isEmpty() || out.write(blob) != blob.size()) {
                out.close();
                QFile::remove(dst);
                continue;
            }
            out.close();
            qInfo("sr: vendor lib cached %s (%.1fKB)", qPrintable(name),
                  double(blob.size()) / 1024.0);
            okOne = true;
            ++copied;
            break;
        }
        if (!okOne && why != nullptr)
            *why = QStringLiteral("复制 %1 失败").arg(name);
    }
    if (copied == 0) {
        if (why != nullptr)
            *why = QStringLiteral("设备上没有可读的 FastRPC 相关库");
        return QString();
    }
    return outDir;
}

// 把本应用的模型清单写成引擎认的 models.txt（引擎内置表是 Python 链那套
// anime/photo 名字，与本目录的 cunet 文件名对不上，必须显式映射）。
void QnnWriteModelsTxt(const QString &dir)
{
    // 列：<键> <文件名> <倍数> <降噪> <tile> <arch>
    // arch=quant 时子进程会关掉 offload_graph_io_quantization，让 QDQ 图的
    // I/O 量化也落在 QNN 上（否则严格档会与 CPU 兜底禁令冲突而建会话失败）。
    auto line = [](const Spec &sp) {
        QString key = QString::fromUtf8(sp.file);
        if (key.endsWith(QLatin1String(".onnx")))
            key.chop(5);
        return QStringLiteral("%1 %2 %3 %4 %5 %6")
            .arg(key.toUpper(), QString::fromUtf8(sp.file))
            .arg(sp.scale)
            .arg(sp.denoise)
            .arg(sp.tile > 0 ? sp.tile : 192)
            .arg(QString::fromUtf8(sp.arch));
    };
    QStringList lines;
    lines << QStringLiteral("# generated by jmcomic ui for qnn proc; safe to delete");
    for (const Spec &sp : kCatalog)
        lines << line(sp);
    // 动态量化模型（QDQ，如 *_q16_lowq.onnx）也是用户可选的后端输入，
    // 不在 kCatalog 里，必须一并映射，否则会判成"引擎模型表缺少"。
    DynStorage &dyn = Dyn();
    for (int i = 0; i < dyn.count; ++i)
        lines << line(dyn.specs[i]);
    QFile f(dir + QStringLiteral("/models.txt"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(lines.join(QLatin1Char('\n')).toUtf8() + '\n');
}

// 启动子进程并完成 hello 握手。失败时 *why 是可直接展示给用户的原因。
bool QnnProcStart(const QString &wantFile, QString *why)
{
    if (g_qp.up) {
        // 进程已起：仍要保证"这次要用的模型"在 qnn_models 里。模型在 assets 时
        // 只按首次 wantFile 复制，后来切换模型（尤其切到目录里的量化模型）时
        // 文件可能还没落地，子进程会静默回退到同倍数模型。此调用幂等，
        // 非 assets 目录直接返回。
        (void)QnnPrepareModels(wantFile, nullptr);
        return true;
    }
    const QString libDir = QnnLibDir();
    if (libDir.isEmpty()) {
        if (why)
            *why = QStringLiteral("找不到 nativeLibraryDir");
        return false;
    }
    const QString proc = libDir + QStringLiteral("/libsr_qnn_proc.so");
    if (!QFileInfo::exists(proc)) {
        if (why)
            *why = QStringLiteral("APK 未包含 libsr_qnn_proc.so（需 useLegacyPackaging=true 打包）");
        return false;
    }
    const QString modelsDir = QnnPrepareModels(wantFile, why);
    if (modelsDir.isEmpty())
        return false;
    QnnWriteModelsTxt(modelsDir);

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0) {
        if (why)
            *why = QStringLiteral("pipe 创建失败");
        return false;
    }
    fflush(nullptr);
    const QByteArray procPath = proc.toUtf8();
    const QByteArray libPath = libDir.toUtf8();
    const QByteArray models = modelsDir.toUtf8();
    const QByteArray cache = (modelsDir + QStringLiteral("/.qnn_cache")).toUtf8();
    // 日志路径必须在 fork 前算好：父进程也要拿它 dump 到 sr-perf.txt
    // 优先写到外部私有目录：/sdcard/Android/data/<pkg>/files 是可被 adb 直接
    // pull 的，排障不用 run-as（release 包拿不到内部 filesDir）。
    QString logDir = QStringLiteral("/storage/emulated/0/Android/data/org.jmcomic.ui/files");
    if (!QDir(logDir).exists())
        logDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (logDir.isEmpty())
        logDir = modelsDir;
    g_qnnLogPath = logDir + QStringLiteral("/qnn_proc.log");
    const QByteArray logPath = g_qnnLogPath.toUtf8();
    // 用 posix_spawn（vfork + exec）而不是 fork：
    // 宿主是 Qt 多线程进程，fork 要复制整张页表，并把调用瞬间的堆/内部锁状态
    // 带进子进程（子进程随后 exec，但 fork 到 exec 之间的状态已可能污染
    // malloc/dlopen 的 arena）；Python 宿主的 subprocess 走的就是 posix_spawn。
    // 这是两版在"如何启动 proc"上唯一的实质差异，其余（二进制/ORT/QNN 库/
    // ADSP 顺序/targetSdk）都已逐项核对一致。
    const int logFd = open(logPath.constData(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    // 继承父进程环境，只覆盖我们需要的三个键（完全替换会让 linker 丢掉
    // ANDROID_ROOT 之类的基础变量，反而加载不了库）
    QVector<QByteArray> envStore;
    for (char **e = environ; e != nullptr && *e != nullptr; ++e) {
        const QByteArray kv(*e);
        if (kv.startsWith("LD_LIBRARY_PATH=") || kv.startsWith("JM_SR_MODELS=")
            || kv.startsWith("JM_SR_CACHE="))
            continue;
        envStore.append(kv);
    }
    // LD 只给 app 自己的 lib 目录。libcdsprpc.so 已经随 jniLibs 打进
    // nativeLibraryDir（apk_data_file 允许 exec），不需要再从 filesDir 兜底：
    // app_data_file 下的 .so 在 Android 10+ 不允许 dlopen（实测只能靠
    // nativeLibraryDir 或 Java 层 libnativeloader）。也不能把 /vendor/lib64
    // 整体加进来 —— ORT 的 libc++_shared 会解析到厂商版本，子进程连 ORT 都加载不了。
    // 把厂商 FastRPC 库复制到 filesDir（在 permitted_paths 的 /data 下），
    // 再把目录传给 proc —— App 的 namespace 不允许 /vendor/lib64，
    // 但 filesDir 可以 dlopen（不需要把厂商库打进 APK）。
    const QByteArray qnnLibsDir = QnnPrepareFastRpcLib(nullptr).toUtf8();
    if (!qnnLibsDir.isEmpty()) {
        envStore.append("JM_QNN_LIBS=" + qnnLibsDir);
        envStore.append("LD_LIBRARY_PATH=" + libPath + ":" + qnnLibsDir);
    } else {
        envStore.append("LD_LIBRARY_PATH=" + libPath);
    }
    envStore.append("JM_SR_MODELS=" + models);
    envStore.append("JM_SR_CACHE=" + cache);
    if (qEnvironmentVariableIsSet("JM_QNN_DEBUG")) {
        envStore.append("JM_QNN_DEBUG=" + qgetenv("JM_QNN_DEBUG"));
    }
    QVector<char *> envp;
    envp.reserve(envStore.size() + 1);
    for (QByteArray &kv : envStore)
        envp.append(kv.data());
    envp.append(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, inPipe[0], 0);
    posix_spawn_file_actions_adddup2(&fa, outPipe[1], 1);
    if (logFd >= 0)
        posix_spawn_file_actions_adddup2(&fa, logFd, 2);
    posix_spawn_file_actions_addclose(&fa, inPipe[0]);
    posix_spawn_file_actions_addclose(&fa, inPipe[1]);
    posix_spawn_file_actions_addclose(&fa, outPipe[0]);
    posix_spawn_file_actions_addclose(&fa, outPipe[1]);
    if (logFd >= 0)
        posix_spawn_file_actions_addclose(&fa, logFd);

    pid_t pid = -1;
    int spawnRc = -1;
    // 优先用 linker64 拉起 proc：/system/bin/linker64 属 system 可执行文件，它建立
    // 的 linker namespace 的 permitted_paths 覆盖 /vendor/lib64；而直接 exec
    // /data/app 下的可执行文件时用的是 app namespace，permitted_paths 里没有
    // /vendor/lib64 —— libQnnHtpV79Stub.so 的 NEEDED libcdsprpc.so 就解析不到，
    // QnnDevice_create 报 QNN_DEVICE_ERROR_INVALID_CONFIG。
    // Python 宿主的启动链也是 [exe] -> [linker64, exe]，这里直接首选后者更稳。
    // 直接 exec：linker64 包装不会改变 namespace（实测 warning 依旧），还会让
    // proc 里的 dladdr 返回 /apex/com.android.runtime/bin，毁掉"同目录找库"逻辑。
    {
        QByteArray procPathArg = procPath;
        char *argvv[2] = {procPathArg.data(), nullptr};
        spawnRc = posix_spawn(&pid, procPath.constData(), &fa, nullptr, argvv, envp.data());
    }
    posix_spawn_file_actions_destroy(&fa);
    if (logFd >= 0)
        close(logFd);
    if (spawnRc != 0) {
        close(inPipe[1]);
        close(outPipe[0]);
        if (why)
            *why = QStringLiteral("posix_spawn 失败（%1）")
                       .arg(QString::fromLocal8Bit(strerror(spawnRc)));
        return false;
    }
    close(inPipe[0]);
    close(outPipe[1]);
    g_qp.pid = pid;
    g_qp.wfd = inPipe[1];
    g_qp.rfd = outPipe[0];

    const uint32_t cmd = 1;
    uint32_t rcmd = 0;
    int32_t status = -1;
    if (!QpWriteAll(&cmd, sizeof(cmd)) || !QpReadAll(&rcmd, sizeof(rcmd), 30000)
        || !QpReadAll(&status, sizeof(status), 30000)) {
        QnnProcStop();
        if (why)
            *why = QStringLiteral("QNN 进程未响应 hello（启动失败或超时）");
        return false;
    }
    QString ver;
    QString backend;
    QString htpErr;
    int32_t htp = 0;
    int32_t cores = 0;
    int32_t count = 0;
    if (!QpReadStr(&ver, 64) || !QpReadStr(&backend, 160)
        || !QpReadAll(&htp, sizeof(htp), 20000) || !QpReadStr(&htpErr, 256)
        || !QpReadAll(&cores, sizeof(cores), 20000) || !QpReadAll(&count, sizeof(count), 20000)) {
        QnnProcStop();
        if (why)
            *why = QStringLiteral("QNN hello 回复解析失败");
        return false;
    }
    g_qp.idByFile.clear();
    g_qp.idByKey.clear();
    g_qp.scaleOf.clear();
    for (int i = 0; i < count; ++i) {
        int32_t id = -1;
        int32_t scale = 0;
        int32_t denoise = 0;
        int32_t mtile = 0;
        QByteArray nameBlob;
        QByteArray fileBlob;
        if (!QpReadAll(&id, sizeof(id), 20000) || !QpReadBlob(&nameBlob, 20000)
            || !QpReadBlob(&fileBlob, 20000) || !QpReadAll(&scale, sizeof(scale), 20000)
            || !QpReadAll(&denoise, sizeof(denoise), 20000)
            || !QpReadAll(&mtile, sizeof(mtile), 20000)) {
            QnnProcStop();
            if (why)
                *why = QStringLiteral("QNN 模型表解析失败");
            return false;
        }
        const QString file = QString::fromUtf8(fileBlob).toLower();
        const QString mname = QString::fromUtf8(nameBlob).toUpper();
        if (!file.isEmpty() && !g_qp.idByFile.contains(file))
            g_qp.idByFile.insert(file, int(id));
        if (!mname.isEmpty())
            g_qp.idByKey.insert(mname, int(id));
        g_qp.scaleOf.append({int(id), int(scale)});
    }
    QByteArray tail;
    QpReadBlob(&tail, 20000);   // hello 末尾的 lastError，缺失不影响

    if (status < 0) {
        const QString msg =
            QStringLiteral("QNN 引擎初始化失败：%1")
                .arg(tail.isEmpty() ? QStringLiteral("未知原因") : QString::fromUtf8(tail));
        QnnProcStop();
        if (why)
            *why = msg;
        return false;
    }
    g_qp.up = true;
    g_qp.htp = htp != 0;
    g_qp.cores = int(cores);
    g_qp.version = ver;
    g_qp.backend = backend;
    g_qp.htpError = htpErr;
    g_qp.note = htp != 0 ? QStringLiteral("QNN HTP（%1）").arg(backend)
                         : QStringLiteral("QNN 已启动但 HTP 未生效：%1").arg(htpErr);
    qInfo("sr: qnn proc up ver=%s htp=%d models=%d", qPrintable(ver), int(htp), int(count));
    return true;
}

// 把本应用模型映射到引擎表里的 id（file 精确 -> 规范化名 -> 同倍数兜底）
int QnnPickModel(const Spec *spec)
{
    if (spec == nullptr)
        return -1;
    QString file = QString::fromUtf8(spec->file).toLower();
    if (g_qp.idByFile.contains(file))
        return g_qp.idByFile.value(file);
    QString key = file;
    if (key.endsWith(QLatin1String(".onnx")))
        key.chop(5);
    key = key.toUpper();
    if (g_qp.idByKey.contains(key))
        return g_qp.idByKey.value(key);
    // 量化模型绝不能用"同倍数兜底"：那会把 float 模型顶上来，界面写着量化、
    // 实际跑的是另一张图（耗时和精度全对不上）。找不到就是找不到。
    if (QString::fromUtf8(spec->arch) == QLatin1String("quant"))
        return -1;
    for (const auto &kv : g_qp.scaleOf) {
        if (kv.second == spec->scale)
            return kv.first;
    }
    return -1;
}

// 推理后再问一次引擎状态（cmd=4）：hello 的快照是建会话之前的，
// 只有跑过 add 之后 backend 才会显示 HTP/NPU 是否真的接管。
QString QnnProcInfo()
{
    if (!g_qp.up)
        return QString();
    const uint32_t cmd = 4;
    if (!QpWriteAll(&cmd, sizeof(cmd)))
        return QString();
    uint32_t rcmd = 0;
    int32_t status = -1;
    QString ver;
    QString backend;
    QString htpErr;
    int32_t htp = 0;
    int32_t cores = 0;
    int32_t zero = 0;
    if (!QpReadAll(&rcmd, sizeof(rcmd), 20000) || !QpReadAll(&status, sizeof(status), 20000)
        || !QpReadStr(&ver, 64) || !QpReadStr(&backend, 160)
        || !QpReadAll(&htp, sizeof(htp), 20000) || !QpReadStr(&htpErr, 256)
        || !QpReadAll(&cores, sizeof(cores), 20000) || !QpReadAll(&zero, sizeof(zero), 20000)) {
        QnnProcStop();
        return QString();
    }
    QByteArray tail;
    QpReadBlob(&tail, 20000);
    if (status < 0)
        return QStringLiteral("info failed: %1").arg(QString::fromUtf8(tail));
    return QStringLiteral("htp=%1 backend=%2%3")
        .arg(int(htp))
        .arg(backend)
        .arg(htpErr.isEmpty() ? QString() : QStringLiteral(" err=%1").arg(htpErr));
}

// 引擎日志尾部（排障）：只取最后 1.2KB，够看到 SetupBackend 的真实报错
QString QnnProcLogTail()
{
    if (g_qnnLogPath.isEmpty())
        return QString();
    QFile f(g_qnnLogPath);
    if (!f.open(QIODevice::ReadOnly))
        return QStringLiteral("no log at %1").arg(g_qnnLogPath);
    f.seek(qMax<qint64>(0, f.size() - 1200));
    QString text = QString::fromUtf8(f.readAll());
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return text.trimmed();
}

// 单 tile 推理：CHW float[0,1] <-> 协议里的交错 RGB888
bool QnnProcRun(const std::vector<float> &input, int tile, int modelId, double scale,
                std::vector<float> *output, int *outW, int *outH, QString *why)
{
    if (!g_qp.up) {
        if (why)
            *why = QStringLiteral("QNN 进程未启动");
        return false;
    }
    const int n = tile * tile;
    if (int(input.size()) < n * 3) {
        if (why)
            *why = QStringLiteral("输入张量尺寸不符");
        return false;
    }
    std::vector<unsigned char> px(size_t(n) * 3);
    for (int i = 0; i < n; ++i) {
        for (int c = 0; c < 3; ++c) {
            const float v = input[size_t(c) * size_t(n) + size_t(i)];
            const int q = int(v * 255.0f + 0.5f);
            px[size_t(i) * 3 + size_t(c)] =
                static_cast<unsigned char>(std::max(0, std::min(255, q)));
        }
    }
    const uint32_t cmd = 2;
    const int32_t taskId = 1;
    const int32_t mid = modelId;
    const int32_t tw = tile * int(std::max(1.0, scale));
    const int32_t th = tw;
    const int32_t w = tile;
    const int32_t h = tile;
    const int32_t stride = tile * 3;
    const int32_t ch = 3;
    const int32_t mtile = tile;
    QByteArray fmt(16, '\0');
    memcpy(fmt.data(), "rgb888", 6);
    const bool sent = QpWriteAll(&cmd, sizeof(cmd)) && QpWriteAll(&taskId, sizeof(taskId))
                      && QpWriteAll(&mid, sizeof(mid)) && QpWriteAll(&scale, sizeof(scale))
                      && QpWriteAll(&tw, sizeof(tw)) && QpWriteAll(&th, sizeof(th))
                      && QpWriteAll(&w, sizeof(w)) && QpWriteAll(&h, sizeof(h))
                      && QpWriteAll(&stride, sizeof(stride)) && QpWriteAll(&ch, sizeof(ch))
                      && QpWriteAll(fmt.constData(), size_t(fmt.size()))
                      && QpWriteAll(&mtile, sizeof(mtile))
                      && QpWriteBlob(px.data(), int(px.size()));
    if (!sent) {
        QnnProcStop();
        if (why)
            *why = QStringLiteral("QNN 进程写入失败（已退出）");
        return false;
    }
    // 首次推理含 QNN 在线编译（真机大模型可达 60~120s），给足 240s
    const int waitMs = 240000;
    uint32_t rcmd = 0;
    int32_t rtask = 0;
    int32_t status = 0;
    double tick = 0;
    int32_t ow = 0;
    int32_t oh = 0;
    int32_t ostr = 0;
    QByteArray rfmt(16, '\0');
    QByteArray errBlob;
    if (!QpReadAll(&rcmd, sizeof(rcmd), waitMs) || !QpReadAll(&rtask, sizeof(rtask), waitMs)
        || !QpReadAll(&status, sizeof(status), waitMs) || !QpReadAll(&tick, sizeof(tick), waitMs)
        || !QpReadAll(&ow, sizeof(ow), waitMs) || !QpReadAll(&oh, sizeof(oh), waitMs)
        || !QpReadAll(&ostr, sizeof(ostr), waitMs)
        || !QpReadAll(rfmt.data(), size_t(rfmt.size()), waitMs)
        || !QpReadBlob(&errBlob, waitMs)) {
        QnnProcStop();
        if (why)
            *why = QStringLiteral("QNN 推理超时或进程退出");
        return false;
    }
    if (status < 0) {
        if (why)
            *why = QStringLiteral("QNN 推理失败：%1").arg(QString::fromUtf8(errBlob));
        return false;
    }
    QByteArray outPx;
    if (!QpReadBlob(&outPx, waitMs)) {
        QnnProcStop();
        if (why)
            *why = QStringLiteral("QNN 输出读取失败");
        return false;
    }
    if (ow <= 0 || oh <= 0 || outPx.size() < ostr * oh) {
        if (why)
            *why = QStringLiteral("QNN 输出尺寸异常 %1x%2").arg(ow).arg(oh);
        return false;
    }
    const size_t plane = size_t(ow) * size_t(oh);
    output->assign(plane * 3, 0.0f);
    for (int y = 0; y < oh; ++y) {
        const auto *row = reinterpret_cast<const unsigned char *>(outPx.constData())
                          + size_t(y) * size_t(ostr);
        for (int x = 0; x < ow; ++x) {
            for (int c = 0; c < 3; ++c)
                (*output)[size_t(c) * plane + size_t(y) * size_t(ow) + size_t(x)] =
                    row[size_t(x) * 3 + size_t(c)] * (1.0f / 255.0f);
        }
    }
    *outW = int(ow);
    *outH = int(oh);
    return true;
}
#else
// 桌面没有独立进程（ORT 自带的 QNN EP 直接可用），诊断接口返回空
inline QString QnnProcInfo()
{
    return QString();
}
inline QString QnnProcLogTail()
{
    return QString();
}
#endif  // Q_OS_ANDROID

// 探测一次：pref 允许 + ORT 带 QNN EP + 后端 DSO 在。结果缓存在 State。
bool QnnUsable()
{
    State &s = S();
    if (s.qnnProbed)
        return s.qnnReady;
    s.qnnProbed = true;
    s.qnnReady = false;
    s.qnnNote.clear();

    const QString pref = PrefBackend();
    if (pref == QLatin1String("cpu")) {
        s.backendNote = QStringLiteral("JM_SR_BACKEND=cpu，跳过 QNN");
        return false;
    }

#ifdef Q_OS_ANDROID
    // QNN 一律走独立进程（见 QnnProcStart 头注释）：这里只做轻检查，
    // 真正的 fork/hello 放在 EnsureModel（工作线程），不能卡 UI 线程。
    if (qEnvironmentVariableIsSet("JM_QNN_PROBE")
        && qEnvironmentVariableIntValue("JM_QNN_PROBE") == 0) {
        s.qnnNote = QStringLiteral("JM_QNN_PROBE=0，跳过 QNN");
        s.backendNote = s.qnnNote;
        return false;
    }
    const QString libDir = QnnLibDir();
    const bool hasProc = !libDir.isEmpty()
                         && QFileInfo::exists(libDir + QStringLiteral("/libsr_qnn_proc.so"));
    if (!hasProc) {
        s.qnnNote = QStringLiteral("APK 未包含 libsr_qnn_proc.so（QNN 后端不可用）");
        s.backendNote = s.qnnNote;
        return false;
    }
    s.qnnReady = true;
    return true;
#else
    // 桌面：直接用 ORT 自带的 QNN EP（Windows 上有 QnnHtp.dll 时）
    if (!EnsureRuntime())
        return false;
    s.providers = AvailableProviders();
    const bool hasEp = s.providers.contains(QStringLiteral("QNNExecutionProvider"));
    const QString backend = QnnBackendPath();
    const bool hasLib = !backend.isEmpty();
    qInfo("sr: backend probe pref=%s ep=%d(%s) qnnLib=%s", qPrintable(pref), hasEp ? 1 : 0,
          qPrintable(s.providers.join(QLatin1Char(','))),
          hasLib ? qPrintable(backend) : "absent");
    if (!hasEp || !hasLib) {
        s.backendNote = !hasEp ? QStringLiteral("onnxruntime 未包含 QNN EP")
                               : QStringLiteral("缺少 QNN 运行库（libQnnHtp.so / QnnHtp.dll）");
        s.qnnNote = s.backendNote;
        return false;
    }
    s.qnnReady = true;
    return true;
#endif
}

bool ProbeGeometry()
{
    State &s = S();
    std::vector<float> probe(static_cast<size_t>(s.tile) * s.tile * 3, 0.0f);
    std::vector<float> out;
    int ow = 0;
    int oh = 0;
    std::fprintf(stderr, "sr-trace: probe %s tile=%d\n", s.modelName.toUtf8().constData(), s.tile);
    std::fflush(stderr);
    if (!RunTile(probe, out, ow, oh, s.tile)) {
        if (s.err.isEmpty())
            s.err = QStringLiteral("模型探针推理失败");
        return false;
    }
    s.outW = ow;
    s.outH = oh;
    s.probed = true;
    qInfo("sr: ready model=%s tile=%d out=%dx%d scale=%d", qPrintable(s.modelName), s.tile, ow, oh,
          s.scale);
    std::fprintf(stderr, "sr-trace: ready %s out=%dx%d\n", s.modelName.toUtf8().constData(), ow,
                 oh);
    std::fflush(stderr);
    return true;
}

bool EnsureModel(const QString &want)
{
    State &s = S();
    const QString name = want.isEmpty() ? SrEngine::CurrentModel() : want;
    if (name.isEmpty()) {
        if (s.err.isEmpty())
            s.err = QStringLiteral("没有可用的超分模型（sr_models/ 为空）");
        return false;
    }
    // QNN 后端没有进程内 session（会话在子进程里），只看 session/vkReady 会让
    // 每次 Status()/超分请求都把整套流程重跑一遍——包括一次探针推理。量化模型
    // 单 tile 也要几十毫秒，这里不认出来就会被当成"NPU 很慢"。
    bool haveQnn = false;
#ifdef Q_OS_ANDROID
    haveQnn = s.backend == QLatin1String("qnn-htp") && g_qp.up && g_qp.modelId >= 0;
#endif
    if ((s.session || s.vkReady || haveQnn) && s.modelName == name && s.probed)
        return true;

    // 换模型时整段运行时重建（而不是只换 session）：连续切换十几个模型后，
    // ORT 的 arena/线程池不会全部还给系统，第 30 个模型（64MB 的 x4plus）
    // 会在建会话时直接崩掉（桌面实测）。重建 env 只有几十毫秒，
    // 换模型本来就是用户手点的一次操作，代价可以接受。
    if ((s.session || s.vkReady) && s.modelName != name)
        ReleaseAll();

    if (!EnsureRuntime())
        return false;

    const Spec *spec = SpecByName(name);
    if (!spec) {
        s.err = QStringLiteral("未知模型：%1").arg(name);
        return false;
    }
    QString path = ResolveFile(QString::fromUtf8(spec->file));
    if (path.isEmpty()) {
        s.err = QStringLiteral("模型文件不存在：%1（sr_models/ 下缺少该文件）")
                    .arg(QString::fromUtf8(spec->file));
        return false;
    }
    // Intel NPU 档：tile 由归一化吞吐标定表决定（含重叠拼接开销与设备指纹校验）。
    // 标定表推荐非 192 时才切到离线生成的 ONNX 变体；ORT 插件 EP 自身不能 reshape。
    int ovTile = TileForModel(spec, false);
    const QString prefAtEntry = PrefBackend();
    if (prefAtEntry == QLatin1String("ov-npu") || prefAtEntry == QLatin1String("xpu")) {
        int advised = ovTile;
        QString variantPath;
        if (OvResolveModel(path, ovTile, &advised, &variantPath)) {
            ovTile = advised;
            path = variantPath;
        }
    }

    // ---- Vulkan 后端（优先级：QNN -> Vulkan -> CPU）
    // 做成 lambda：QNN 建会话失败时还要再试一次，不能只在函数开头试一遍。
    const QString prefWanted = PrefBackend();
    const int tileForModel = TileForModel(spec, true);
    auto tryVulkan = [&]() -> bool {
        if (!s.vk)
            s.vk = new SrVk::Session;
        QString why;
        const bool ok = s.vk->load(path, &why) && s.vk->ensureTile(tileForModel, &why);
        if (!ok) {
            s.vkReady = false;
            s.vk->reset();
            s.backendNote = QStringLiteral("Vulkan 不可用（%1）").arg(why);
            qWarning("sr: %s", qPrintable(s.backendNote));
            return false;
        }
        s.vkReady = true;
        s.backend = QStringLiteral("vulkan");
        s.backendNote = QString();
        s.modelName = name;
        s.modelPath = path;
        s.scale = std::max(1, spec->scale);
        s.tile = tileForModel;
        s.outW = s.vk->outW();
        s.outH = s.vk->outH();
        s.probed = true;
        qInfo("sr: backend=Vulkan %s out=%dx%d", qPrintable(s.vk->summary()), s.outW, s.outH);
        return true;
    };
    // 显式 GPU 偏好：跳过 QNN，直接 Vulkan；失败再走 ORT 并把原因留在 note 里。
    if (prefWanted == QLatin1String("vulkan")) {
        if (tryVulkan())
            return true;
        s.err = s.backendNote;
    }

    // QDQ 量化模型（w8a16：uint16 激活 + uint8 权重）是 QNN HTP 的主场：
    // ORT QNN EP 原生支持这种 QDQ 组合（官方文档的推荐配置就是
    // activation_type=QUInt16 + weight_type=QUInt8），所以它和 float 模型一样
    // 先试 NPU。只有 Vulkan 明确不支持（见 SrVk 的 DQL 检查），最后才落 CPU。
    const bool quantModel = QString::fromUtf8(spec->arch) == QLatin1String("quant");
    QString qnnFailWhy;   // 非空表示"量化模型本该跑 NPU 但没跑成"，供状态行解释
#ifdef Q_OS_ANDROID
    // QNN 独立进程：会话建在子进程里（App 进程建 QNN EP 会挂死在 FastRPC）。
    // 失败就往下走 Vulkan/CPU，原因留在 backendNote。
    // 显式选 Vulkan 时量化模型也要试 NPU：Vulkan 对 QDQ 图必然失败，
    // 直接落 CPU 等于把唯一能加速它的硬件让出去。
    const bool wantQnn = prefWanted == QLatin1String("qnn")
                         || prefWanted == QLatin1String("auto")
                         || (quantModel && prefWanted == QLatin1String("vulkan"));
    if (wantQnn) {
        QString why;
        // 顺序必须是"先起进程、再查 id"：引擎的模型表要 hello 之后才拿到
        if (QnnProcStart(QString::fromUtf8(spec->file), &why)) {
            const int mid = QnnPickModel(spec);
            if (mid < 0) {
                why = QStringLiteral("引擎模型表缺少 %1").arg(QString::fromUtf8(spec->file));
                QnnProcStop();
            } else {
                g_qp.modelId = mid;
                s.backend = QStringLiteral("qnn-htp");
                s.qnnTier = 3;
                s.backendNote = g_qp.note;
                s.modelName = name;
                s.modelPath = path;
                s.scale = std::max(1, spec->scale);
                // QNN 在线编译按固定 192 输入：改 tile 会让会话报维度错
                s.tile = 192;
                s.probed = false;
                if (ProbeGeometry()) {
                    // 探针已经真跑过一次推理，此时再问子进程后端才能分辨
                    // "QNN 建了会话但一个节点都没接住"（那种会话被引擎丢弃、实际跑
                    // 的是 CPU EP）和"真在 Hexagon 上跑"。不查这一下，量化模型退到
                    // CPU 时会照样显示成 QNN HTP（NPU），把 CPU 的速度当 NPU 报。
                    const QString info = QnnProcInfo();
                    if (info.contains(QLatin1String("htp=1"))) {
                        s.err.clear();
                        s.backend = QStringLiteral("qnn-htp");
                        // 注意不能用 g_qp.note：那是 hello 时的快照，HTP 还没建会话，
                        // 文案会是"QNN HTP（CPU (ONNX Runtime, QNN 未生效)）"这种自相矛盾的串。
                        s.backendNote = QStringLiteral("QNN HTP（Hexagon NPU）");
                        return true;
                    }
                    s.backend = QStringLiteral("cpu");
                    s.backendNote = QStringLiteral("QNN 未接管（%1）")
                                        .arg(info.isEmpty() ? QStringLiteral("无响应") : info);
                    QnnProcStop();
                    why = s.backendNote;
                } else {
                    s.backendNote = QStringLiteral("QNN 探针失败：%1").arg(s.err);
                    QnnProcStop();
                    why = s.err.isEmpty() ? s.backendNote : s.err;
                }
            }
        } else {
            g_qp.note = why;   // 排障：sr-perf 的 qnn= 行会显示失败原因
        }
        qnnFailWhy = why;
        qWarning("sr: qnn proc unavailable: %s", qPrintable(why));
        if (prefWanted == QLatin1String("qnn")) {
            s.backendNote = QStringLiteral("强制 QNN 但不可用（%1）").arg(why);
            s.err = s.backendNote;
        }
    }
#endif

    ReleaseSession();
    if (s.opts) {
        s.api->ReleaseSessionOptions(s.opts);
        s.opts = nullptr;
    }

    auto check = [&s](OrtStatus *st) {
        if (!st)
            return true;
        s.err = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
        return false;
    };

    // 模型二进制先读进内存（asset 与文件两条路径统一）：QNN 的会话可能失败多次，
    // 每次重试都重新读一遍文件没有意义。
    QByteArray blob;
    std::wstring wpath;
    QByteArray upath;
    if (IsAssetPath(path)) {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            s.err = QStringLiteral("读取内置模型失败：%1").arg(path);
            return false;
        }
        blob = f.readAll();
        f.close();
    } else {
#ifdef _WIN32
        wpath = QDir::toNativeSeparators(path).toStdWString();
#else
        upath = path.toUtf8();
#endif
    }

    const int threads = qBound(1, QThread::idealThreadCount() / 2, 4);

    // 建会话：QNN 档位从高到低，最后 CPU。每次尝试都要全新的 SessionOptions
    // （EP 是挂在 options 上的，失败后复用同一个 options 会带上半初始化的 EP 状态）。
    // ovMode: 空 = 普通 ORT（CPU/XNNPACK/NNAPI）；"ov-npu" = Intel NPU；
    // "xpu" = OpenVINO AUTO(NPU -> GPU -> CPU)。
    auto createSession = [&](bool useQnn, int tier, QString *why,
                             const QString &ovMode = QString()) -> bool {
        if (s.opts) {
            s.api->ReleaseSessionOptions(s.opts);
            s.opts = nullptr;
        }
        if (!check(s.api->CreateSessionOptions(&s.opts)))
            return false;
        s.api->SetIntraOpNumThreads(s.opts, threads);
        // QNN 只开 BASIC：ORT 的 Level-2 会把 Conv+LeakyRelu 融成 CPU EP 专属的
        // com.microsoft.FusedConv，QNN 分区时整张图连带下游全判给 CPU
        // （Python 侧真机实测过，见 sr_qnn.cpp 同位置注释）。
        // QNN 与 OpenVINO 都只开 BASIC：ORT 的 Level-2 融合会生成 CPU EP 专属算子，
        // 导致整张图连带下游被判回 CPU（QNN 真机实测；OpenVINO 同理）。
        s.api->SetSessionGraphOptimizationLevel(
            s.opts, (useQnn || !ovMode.isEmpty()) ? ORT_ENABLE_BASIC : ORT_ENABLE_ALL);

        if (useQnn) {
            const QString backend = QnnBackendPath();
            const QVector<QPair<QString, QString>> opts = QnnOptions(backend, tier);
            QVector<QByteArray> keysStore;
            QVector<QByteArray> valsStore;
            keysStore.reserve(opts.size());
            valsStore.reserve(opts.size());
            for (const auto &kv : opts) {
                keysStore << kv.first.toUtf8();
                valsStore << kv.second.toUtf8();
            }
            QVector<const char *> keys;
            QVector<const char *> vals;
            keys.reserve(keysStore.size());
            vals.reserve(valsStore.size());
            for (int i = 0; i < keysStore.size(); ++i) {
                keys << keysStore.at(i).constData();
                vals << valsStore.at(i).constData();
            }
            OrtStatus *st = s.api->SessionOptionsAppendExecutionProvider(
                s.opts, "QNN", keys.constData(), vals.constData(), size_t(keys.size()));
            if (st) {
                if (why)
                    *why = QString::fromUtf8(s.api->GetErrorMessage(st));
                s.api->ReleaseStatus(st);
                return false;
            }
        } else if (!ovMode.isEmpty()) {
            // 插件 EP 走 GetEpDevices + AppendExecutionProvider_V2。失败必须让
            // 外层显式回退，不能静默变成"看起来是 NPU、实际每 tile 跑 CPU"。
            if (!OvAppendEp(s.opts, ovMode, path, ovTile, why))
                return false;
        } else {
            // CPU 兜底时尽量挂上加速 EP（不可用只返回错误状态，不致命）：
            // Android 上是 XNNPACK → NNAPI，桌面通常都没有，等价纯 CPU。
            for (const char *ep : {"XNNPACK", "NNAPI"}) {
                OrtStatus *st =
                    s.api->SessionOptionsAppendExecutionProvider(s.opts, ep, nullptr, nullptr, 0);
                if (!st)
                    break;
                s.api->ReleaseStatus(st);
            }
        }

        const QByteArray modeTag = useQnn ? QByteArray("qnn")
                                          : (ovMode.isEmpty() ? QByteArray("cpu")
                                                              : ovMode.toUtf8());
        std::fprintf(stderr, "sr-trace: create(%s tier=%d) %s\n", modeTag.constData(), tier,
                     qPrintable(QFileInfo(path).fileName()));
        std::fflush(stderr);
        QElapsedTimer compileTimer;
        compileTimer.start();
        OrtStatus *st = nullptr;
        if (IsAssetPath(path)) {
            st = s.api->CreateSessionFromArray(s.env, blob.constData(), size_t(blob.size()),
                                               s.opts, &s.session);
        } else {
#ifdef _WIN32
            st = s.api->CreateSession(s.env, wpath.c_str(), s.opts, &s.session);
#else
            st = s.api->CreateSession(s.env, upath.constData(), s.opts, &s.session);
#endif
        }
        if (st) {
            if (why)
                *why = QString::fromUtf8(s.api->GetErrorMessage(st));
            s.api->ReleaseStatus(st);
            s.session = nullptr;
            std::fprintf(stderr, "sr-trace: compile(%s) FAIL %lldms\n", modeTag.constData(),
                         static_cast<long long>(compileTimer.elapsed()));
            return false;
        }
        std::fprintf(stderr, "sr-trace: compile(%s) ok %lldms\n", modeTag.constData(),
                     static_cast<long long>(compileTimer.elapsed()));
        return true;
    };

    bool madeIt = false;
    const QString pref = PrefBackend();
#ifdef Q_OS_ANDROID
    const bool wantOv = false;   // Intel 插件 EP 只在 Windows 桌面分发
#else
    const bool wantOv = pref == QLatin1String("ov-npu") || pref == QLatin1String("xpu");
#endif
#ifndef Q_OS_ANDROID
    // Intel OpenVINO 插件 EP：显式偏好优先于 QNN / Vulkan，避免被前面试过的
    // 会话抢走。失败才逐级回退 Vulkan -> CPU，原因必须留在 backendNote，
    // 否则又变成"状态写 NPU、实际跑 CPU"的老问题。
    if (wantOv) {
        QString why;
        if (createSession(false, 0, &why, pref)) {
            madeIt = true;
            s.backend = pref == QLatin1String("xpu") ? QStringLiteral("ov-xpu")
                                                     : QStringLiteral("ov-npu");
            s.backendNote = QStringLiteral("OpenVINO 插件 EP · %1 · devices=[%2]")
                                .arg(pref == QLatin1String("xpu")
                                         ? QStringLiteral("AUTO(NPU→GPU→CPU)")
                                         : QStringLiteral("NPU FP16"),
                                     s.ovDevices.join(QLatin1Char(',')));
            qInfo("sr: backend=OpenVINO %s devices=[%s]", qPrintable(pref),
                  qPrintable(s.ovDevices.join(QLatin1Char(','))));
        } else {
            const QString fail = why.isEmpty() ? s.ovNote : why;
            qWarning("sr: OpenVINO EP unavailable: %s", qPrintable(fail));
            if (tryVulkan()) {
                s.backendNote = QStringLiteral("Intel OpenVINO 不可用（%1），已回退 Vulkan")
                                    .arg(fail);
                return true;
            }
            s.backendNote = QStringLiteral("Intel OpenVINO 不可用（%1），已回退 CPU").arg(fail);
        }
    }
#endif
#ifdef Q_OS_ANDROID
    // Android 绝不在 App 进程里建 ORT QNN：FastRPC 加载不了 libcdsprpc.so，
    // tier3/2 直接失败，tier1/0 虽然"建成了"，其实是 QNN 的 CPU 后端 ——
    // 看起来像 NPU、跑出来是 CPU（真机实测 683ms vs CPU 737ms），必须拦住。
    const bool allowOrtQnn = false;
#else
    const bool allowOrtQnn = true;
#endif
    if (!wantOv && allowOrtQnn && QnnUsable()) {
        for (int tier = 3; tier >= 0 && !madeIt; --tier) {
            QString why;
            if (createSession(true, tier, &why)) {
                madeIt = true;
                s.backend = QStringLiteral("qnn-htp");
                s.qnnTier = tier;
                s.backendNote = tier == 3 ? QString()
                                          : QStringLiteral("QNN tier %1（部分提速项未启用）").arg(tier);
                qInfo("sr: backend=QNN HTP tier=%d", tier);
                break;
            }
            qWarning("sr: qnn tier %d failed: %s", tier, qPrintable(why));
            s.err = why;
        }
        if (!madeIt) {
            // 关键：失败必须回退，而且原因要能看见——否则用户只看到"超分很慢"，
            // 维护者不知道是包漏了 QNN 库还是机型拿不到 FastRPC。
            s.backendNote = QStringLiteral("QNN 建会话失败：%1").arg(s.err);
            qWarning("sr: %s", qPrintable(s.backendNote));
            // QNN 失败后的第二档是 Vulkan（auto 才允许，qnn/cpu 是显式偏好）
            if (pref == QLatin1String("auto") && tryVulkan())
                return true;
            s.backendNote += QStringLiteral("，已回退 CPU");
        }
    } else if (!wantOv && pref == QLatin1String("qnn")) {
        s.backendNote = QStringLiteral("强制 QNN 但不可用（%1），已回退 CPU")
                            .arg(s.backendNote.isEmpty() ? QStringLiteral("原因未知")
                                                        : s.backendNote);
    } else if (pref == QLatin1String("auto") && !quantModel && tryVulkan()) {
        // QNN 库/EP 都没有：桌面与有 Vulkan 驱动的 Android 上优先 GPU
        //（量化模型的 QDQ 图 Vulkan 必然拒绝，跳过这次白跑，直接落 CPU）
        return true;
    }

    if (!madeIt) {
        QString why;
        if (!createSession(false, 0, &why))
            return false;
        s.backend = QStringLiteral("cpu");
        if (s.backendNote.isEmpty() && pref == QLatin1String("auto"))
            s.backendNote = QStringLiteral("未发现 QNN 运行库，使用 CPU");
    }

    // 量化模型落到 CPU 时把原因写清楚（必须放在 CPU 会话建成之后：中间的
    // Vulkan 尝试会重写 backendNote）。这是"本该在 NPU 却退回 CPU"的现场，
    // 状态行能直接看出是库没打进包、机型不支持，还是要换模型。
    if (quantModel && s.backend != QLatin1String("qnn-htp")) {
        if (prefWanted == QLatin1String("cpu")) {
            s.backendNote = QStringLiteral("QDQ 量化模型（按设置强制 CPU 推理）");
        } else if (qnnFailWhy.isEmpty()) {
#ifdef Q_OS_ANDROID
            s.backendNote = QStringLiteral("QDQ 量化模型：NPU 不可用，使用 CPU");
#else
            s.backendNote = QStringLiteral("QDQ 量化模型：桌面版无 QNN EP，使用 CPU");
#endif
        } else {
            s.backendNote = QStringLiteral("QDQ 量化模型需 QNN HTP，NPU 不可用（%1），使用 CPU")
                                .arg(qnnFailWhy);
        }
        s.err.clear();   // 上面的 QNN 失败原因已进 backendNote，避免 Status() 误报不可用
    }

    if (IsAssetPath(path))
        qInfo("sr: model from asset %s (%lld bytes)", qPrintable(path),
              static_cast<long long>(blob.size()));

    if (!check(s.api->GetAllocatorWithDefaultOptions(&s.alloc)))
        return false;
    char *inName = nullptr;
    if (!check(s.api->SessionGetInputName(s.session, 0, s.alloc, &inName)))
        return false;
    s.inName = inName ? inName : "";
    if (inName)
        s.alloc->Free(s.alloc, inName);
    char *outName = nullptr;
    if (!check(s.api->SessionGetOutputName(s.session, 0, s.alloc, &outName)))
        return false;
    s.outName = outName ? outName : "";
    if (outName)
        s.alloc->Free(s.alloc, outName);
    if (!check(s.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &s.mem)))
        return false;

    s.modelName = name;
    s.modelPath = path;
    s.scale = std::max(1, spec->scale);
    // NPU 会话可能用了标定表推荐的变体模型，输入维度必须与之一致。
    const bool ovActive = s.backend == QLatin1String("ov-npu")
                          || s.backend == QLatin1String("ov-xpu");
    s.tile = ovActive ? ovTile : TileForModel(spec, false);
    s.probed = false;
    return ProbeGeometry();
}

// Vulkan 在推理途中失败（DEVICE_LOST 且重建不成功、显存不足、驱动异常）：
// 不再让整张图直接失败，而是重建一个 ORT CPU 会话，把当前 tile 重跑一次。
// 这是最后一道兜底：CPU 慢但一定能出结果，用户在状态行里能看到回退原因。
bool RecoverToCpu()
{
    State &s = S();
    const QString name = s.modelName;
    qWarning("sr: rebuilding ORT CPU session after Vulkan failure (%s)", qPrintable(name));
    g_forceCpuOnce = true;
    ReleaseAll();
    const bool ok = EnsureModel(name);
    g_forceCpuOnce = false;
    if (ok) {
        s.backendNote = QStringLiteral("Vulkan 运行期失败，已回退 CPU（ONNX Runtime）");
        qWarning("sr: %s", qPrintable(s.backendNote));
    } else {
        qWarning("sr: CPU fallback failed: %s", qPrintable(s.err));
    }
    return ok;
}

bool RunTile(const std::vector<float> &input, std::vector<float> &output, int &outW, int &outH,
             int tile)
{
    State &s = S();
    if (s.vkReady && s.vk) {
        QString why;
        if (!s.vk->run(input.data(), &output, &outW, &outH, &why)) {
            s.err = why;
            s.vkReady = false;
            qWarning("sr: vulkan run failed: %s", qPrintable(why));
            // RecoverToCpu 会把 s.session 建好并清掉 vkReady，递归一次即走 ORT 分支。
            if (RecoverToCpu())
                return RunTile(input, output, outW, outH, tile);
            return false;
        }
        return true;
    }
#ifdef Q_OS_ANDROID
    if (s.backend == QLatin1String("qnn-htp") && g_qp.up) {
        QString why;
        if (QnnProcRun(input, tile, g_qp.modelId, double(std::max(1, s.scale)), &output, &outW,
                       &outH, &why))
            return true;
        s.err = why;
        qWarning("sr: qnn run failed: %s", qPrintable(why));
        QnnProcStop();   // 下次 EnsureModel 会重建子进程
        return false;
    }
#endif
    if (!s.api || !s.session || !s.mem)
        return false;
    const int64_t dims[4] = {1, 3, tile, tile};
    OrtValue *inVal = nullptr;
    OrtStatus *st = s.api->CreateTensorWithDataAsOrtValue(
        s.mem, const_cast<float *>(input.data()), input.size() * sizeof(float), dims, 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal);
    if (st) {
        s.err = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
        return false;
    }
    const char *inNames[1] = {s.inName.c_str()};
    const char *outNames[1] = {s.outName.c_str()};
    const OrtValue *ins[1] = {inVal};
    OrtValue *outVal = nullptr;
    st = s.api->Run(s.session, nullptr, inNames, ins, 1, outNames, 1, &outVal);
    s.api->ReleaseValue(inVal);
    if (st) {
        s.err = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
        return false;
    }

    OrtTensorTypeAndShapeInfo *info = nullptr;
    st = s.api->GetTensorTypeAndShape(outVal, &info);
    if (st) {
        s.err = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
        s.api->ReleaseValue(outVal);
        return false;
    }
    size_t rank = 0;
    s.api->GetDimensionsCount(info, &rank);
    int64_t d[8] = {0};
    s.api->GetDimensions(info, d, std::min<size_t>(rank, 8));
    const int h = rank >= 2 ? int(d[rank - 2]) : 0;
    const int w = rank >= 1 ? int(d[rank - 1]) : 0;
    const int c = rank >= 3 ? int(d[rank - 3]) : 3;
    s.api->ReleaseTensorTypeAndShapeInfo(info);
    if (w <= 0 || h <= 0 || c <= 0) {
        s.err = QStringLiteral("模型输出尺寸异常");
        s.api->ReleaseValue(outVal);
        return false;
    }
    void *raw = nullptr;
    st = s.api->GetTensorMutableData(outVal, &raw);
    if (st || !raw) {
        if (st) {
            s.err = QString::fromUtf8(s.api->GetErrorMessage(st));
            s.api->ReleaseStatus(st);
        } else {
            s.err = QStringLiteral("读取模型输出失败");
        }
        s.api->ReleaseValue(outVal);
        return false;
    }
    const size_t count = size_t(c) * size_t(w) * size_t(h);
    output.assign(static_cast<float *>(raw), static_cast<float *>(raw) + count);
    s.api->ReleaseValue(outVal);
    outW = w;
    outH = h;
    return true;
}

inline int ClampInt(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

namespace SrEngine {

QVector<ModelInfo> Catalog()
{
    QVector<ModelInfo> out;
    out.reserve(int(sizeof(kCatalog) / sizeof(kCatalog[0])));
    for (const Spec &s : kCatalog) {
        ModelInfo m;
        m.name = QString::fromUtf8(s.name);
        m.file = QString::fromUtf8(s.file);
        m.arch = QString::fromUtf8(s.arch);
        m.scale = s.scale;
        m.denoise = s.denoise;
        m.tile = s.tile;
        m.note = QString::fromUtf8(s.note);
        const QString path = ResolveFile(m.file);
        m.installed = !path.isEmpty();
        m.bytes = m.installed ? PathSize(path) : 0;
        out.push_back(m);
    }
    // 动态量化模型（只有目录里真实存在才出现，不改变默认 39 个 fp32 的行为）
    DynStorage &d = Dyn();
    for (int i = 0; i < d.count; ++i) {
        const Spec &sp = d.specs[i];
        ModelInfo m;
        m.name = QString::fromUtf8(sp.name);
        m.file = QString::fromUtf8(sp.file);
        m.arch = QString::fromUtf8(sp.arch);
        m.scale = sp.scale;
        m.denoise = sp.denoise;
        m.tile = sp.tile;
        m.note = QString::fromUtf8(sp.note);
        const QString path = ResolveFile(m.file);
        m.installed = !path.isEmpty();
        m.bytes = m.installed ? PathSize(path) : 0;
        out.push_back(m);
    }
    return out;
}

QVector<ModelInfo> InstalledModels()
{
    QVector<ModelInfo> all = Catalog();
    QVector<ModelInfo> out;
    for (const ModelInfo &m : all) {
        if (m.installed)
            out.push_back(m);
    }
    return out;
}

QString ModelsDir()
{
    for (const QString &d : ModelDirs()) {
        if (IsAssetPath(d))
            return d;   // APK 内置：给日志用（不可写，但能说明"模型从哪来"）
        if (QFileInfo(d).isDir())
            return d;
    }
    return ModelDirs().value(0);
}

QString ModelSearchPath()
{
    return ModelDirs().join(QLatin1Char('|'));
}

bool Available()
{
    QMutexLocker<QRecursiveMutex> lock(&S().mutex);
    return EnsureModel(QString());
}

QString Status()
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    if (!EnsureModel(QString()))
        return s.err.isEmpty() ? QStringLiteral("超分不可用") : s.err;
    QString line = QStringLiteral("%1 · %2 · %3px tile · %4")
                       .arg(s.modelName, BackendLabel())
                       .arg(s.tile)
                       .arg(QFileInfo(s.modelPath).fileName());
    // 注意：降级原因不拼进 Status()。这一行会出现在超分页标题旁与设置行里，
    // 拼上 note 会超长被裁；原因由 BackendDetail()/BackendReport() 单独给。
    return line;
}

QString BackendPref()
{
    return PrefBackend();
}

QString BackendName()
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    return s.backend;
}

QString BackendLabel()
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    if (s.backend == QLatin1String("qnn-htp")) {
        return s.qnnTier >= 3 ? QStringLiteral("QNN HTP（NPU）")
                              : QStringLiteral("QNN HTP（NPU · tier %1）").arg(s.qnnTier);
    }
    if (s.backend == QLatin1String("qnn-cpu"))
        return QStringLiteral("QNN（CPU 回退）");
    if (s.backend == QLatin1String("ov-npu"))
        return QStringLiteral("Intel AI Boost（NPU · OpenVINO）");
    if (s.backend == QLatin1String("ov-xpu"))
        return QStringLiteral("Intel XPU（AUTO · OpenVINO）");
    if (s.backend == QLatin1String("vulkan"))
        return QStringLiteral("Vulkan GPU");
    return QStringLiteral("CPU（ONNX Runtime）");
}

QString BackendDetail()
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    return s.backendNote;
}

QString QnnProcStatus()
{
#ifdef Q_OS_ANDROID
    if (g_qp.up)
        return QStringLiteral("proc up ver=%1 htp=%2 backend=%3")
            .arg(g_qp.version)
            .arg(int(g_qp.htp))
            .arg(g_qp.backend);
    return QStringLiteral("proc down %1")
        .arg(g_qp.note.isEmpty() ? QStringLiteral("-") : g_qp.note);
#else
    return QString();
#endif
}

QString QnnProcLogTail()
{
#ifdef Q_OS_ANDROID
    return ::QnnProcLogTail();
#else
    return QString();
#endif
}

QString BackendReport()
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    // 探针只跑一次；EnsureRuntime 失败时 providers 为空，报告里如实空着。
    if (EnsureRuntime() && !s.qnnProbed)
        QnnUsable();
    QString ort = s.providers.isEmpty() ? QStringLiteral("-") : s.providers.join(QLatin1Char(','));
    const QString qnn = s.qnnReady ? QStringLiteral("ready")
                                   : (QnnBackendPath().isEmpty() ? QStringLiteral("absent")
                                                                 : QStringLiteral("no-ep"));
    // Intel 插件 EP 的可用性也如实报告：ready=[NPU|GPU.0|CPU] / no=<原因>。
    // 只有显式要了这档后端或跑报告时才注册，避免默认 auto 平白加载 100MB 编译器。
    QString ov = QStringLiteral("no[ORT-not-ready]");
    if (s.api && s.env) {
        if (OvEnsureRegistered())
            ov = QStringLiteral("ready[%1]auto=%2 tiles=%3/%4 fp=%5 cache=%6")
                     .arg(s.ovDevices.join(QLatin1Char('|')))
                     .arg(s.ovAutoEp ? 1 : 0)
                     .arg(s.ovTiles)
                     .arg(s.ovMaxTiles)
                     .arg(s.ovFp,
                          s.ovCacheDir.isEmpty() ? QStringLiteral("off") : s.ovCacheDir);
        else
            ov = QStringLiteral("no[%1]").arg(s.ovNote);
    }
    QString note = s.backendNote.isEmpty() ? QString() : s.backendNote;
    if (!s.ovAdviceNote.isEmpty()) {
        // 单独一行短日志：sr-backend 那行很长，会被日志 handler 截掉尾部，
        // tile 决策必须能被 --sr-backend 回归直接读到。
        std::fprintf(stderr, "sr-ov-tune: %s\n", qPrintable(s.ovAdviceNote));
        note += (note.isEmpty() ? QString() : QStringLiteral(" | ")) + s.ovAdviceNote;
    }
    return QStringLiteral("sr-backend pref=%1 chosen=%2 tier=%3 ort=[%4] qnn=%5 ov=%6 note=%7")
        .arg(PrefBackend(), s.backend)
        .arg(s.qnnTier)
        .arg(ort, qnn, ov, note.isEmpty() ? QStringLiteral("-") : note);
}

bool SetBackendPreference(const QString &pref, bool persist)
{
    const QString v = NormalizeBackend(pref);
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);

    // 落盘到与 ui.ini 相同的配置目录；写不进去也继续（本次运行仍然生效）
    const QString ini =
#ifdef JM_SR_HAS_APPPATHS
        AppPaths::ConfigFile(QStringLiteral("ui.ini"));
#else
        QDir(AppDir()).filePath(QStringLiteral("config/ui.ini"));
#endif
    if (persist && !ini.isEmpty()) {
        QSettings out(ini, QSettings::IniFormat);
        out.setValue(QStringLiteral("sr/backend"), v);
        out.sync();
    }
    // persist=false 是"只在这次运行里生效"：覆盖内存偏好，不碰 ui.ini。
    // persist=true 时清掉覆盖，让环境变量/ui.ini 重新成为唯一来源。
    g_prefOverride = persist ? QString() : v;

    // 偏好只影响"下次建会话"：必须把当前会话与选项一起放掉，否则旧 EP 会一直用着。
    if (s.api) {
        ReleaseSession();
        if (s.opts) {
            s.api->ReleaseSessionOptions(s.opts);
            s.opts = nullptr;
        }
    }
    s.providers.clear();
    s.qnnProbed = false;
    s.qnnReady = false;
    s.qnnTier = 0;
    s.backend = QStringLiteral("cpu");
    s.backendNote.clear();
    s.err.clear();
    qInfo("sr: backend preference -> %s", qPrintable(v));
    return true;
}

int TilePreference()
{
    bool userSet = false;
    const int v = TileOverride(&userSet);
    return userSet ? v : 0;
}

// 进程驻留内存（MB）：内存占用口径的统一入口。Android/Linux 读 /proc，
// Windows 用 GetProcessMemoryInfo（CMake 侧 Windows 链接 psapi）。
qint64 FootprintMb()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS mc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &mc, sizeof(mc)))
        return qint64(mc.WorkingSetSize) / (1024 * 1024);
    return -1;
#else
    QFile f(QStringLiteral("/proc/self/status"));
    if (!f.open(QIODevice::ReadOnly))
        return -1;
    const QString text = QString::fromUtf8(f.readAll());
    static const QRegularExpression re(QStringLiteral("VmRSS:\\s+(\\d+) kB"));
    const QRegularExpressionMatch m = re.match(text);
    return m.hasMatch() ? m.captured(1).toLongLong() / 1024 : -1;
#endif
}

bool SetTilePreference(int tile)
{
    if (tile < 0 || tile > 512)
        return false;
    if (tile > 0 && (tile < 32 || tile % 8 != 0))
        return false;
    const QString ini =
#ifdef JM_SR_HAS_APPPATHS
        AppPaths::ConfigFile(QStringLiteral("ui.ini"));
#else
        QDir(AppDir()).filePath(QStringLiteral("config/ui.ini"));
#endif
    if (!ini.isEmpty()) {
        QSettings out(ini, QSettings::IniFormat);
        out.setValue(QStringLiteral("sr/tile"), tile);
        out.sync();
    }
    qInfo("sr: tile preference -> %d", tile);
    return true;
}

QVector<SrPerfRow> RunPerfProbe(const QVector<int> &vulkanTiles, int iterations,
                               const QString &modelFile)
{
    State &s = S();
    // 全程持锁：性能测试会 resetWeights（销毁当前 Vulkan 会话）并临时切换
    // 后端偏好，必须与其它推理互斥。同步耗时由调用方放工作线程消化。
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    QVector<SrPerfRow> rows;
    // 指定了模型就先把整条测试切到它上面（GPU/NPU/CPU 三档必须是同一个模型，
    // 否则"量化模型跑 NPU / float 模型跑 CPU"的对比毫无意义）。
    QString probeName;
    if (!modelFile.isEmpty()) {
        for (const ModelInfo &mi : Catalog()) {
            if (mi.file == modelFile || mi.name == modelFile) {
                probeName = mi.name;
                break;
            }
        }
    }
    if (!EnsureModel(probeName)) {
        SrPerfRow r;
        r.label = QStringLiteral("准备");
        r.note = s.err.isEmpty() ? QStringLiteral("超分不可用") : s.err;
        rows.append(r);
        return rows;
    }
    const QString path = s.modelPath;
    const Spec *spec = SpecByName(s.modelName);

    // Vulkan 档：调用方未给时取 {64, 当前自动值}（设置页推荐的核心对比对）
    QVector<int> tiles = vulkanTiles;
    if (tiles.isEmpty()) {
        const int autoTile = TileForModel(spec, true);
        tiles << 64;
        if (autoTile > 0 && autoTile != 64)
            tiles << autoTile;
    }
    // SRMD/QDQ 的图里有固定形状常量，改 tile 会算错（QDQ 还是 uint16 量化图，
    // Vulkan 的 fp32 管线直接拒绝），GPU 档只说明原因，真正有数据的是 NPU/CPU 档
    const bool quantModel = QString::fromUtf8(spec->arch) == QLatin1String("quant");
    const bool tileFixed = QString::fromUtf8(spec->arch) == QLatin1String("srmd")
                           || quantModel;
    const int n = qMax(1, iterations);
    for (int t : tiles) {
        SrPerfRow r;
        r.label = QStringLiteral("GPU（tile %1）").arg(t);
        if (tileFixed) {
            r.note = quantModel
                         ? QStringLiteral("量化模型（w8a16）只支持 NPU/CPU，Vulkan 不接 QDQ 图")
                         : QStringLiteral("该模型 tile 固定 192，无法测其它档");
            rows.append(r);
            continue;
        }
        const qint64 rss0 = FootprintMb();
        SrVk::BenchResult b = SrVk::Benchmark(path, t, n, nullptr);
        const qint64 rss1 = FootprintMb();
        if (b.ok) {
            r.ok = true;
            r.ms = b.medianMs;
            r.actMb = b.activationBytes / (1024 * 1024);
            if (rss0 >= 0 && rss1 > rss0)
                r.memDeltaMb = rss1 - rss0;
        } else {
            r.note = b.note;
        }
        rows.append(r);
    }
    // NPU 档（QNN HTP）：临时切偏好重建会话（tile 固定 192），测完恢复。
    // QNN 库未随包分发时如实标注缺失原因——用户能看到"为什么没有 NPU 数据"。
    {
        SrPerfRow r;
        r.label = QStringLiteral("NPU（QNN HTP，tile 192）");
        if (!QnnUsable()) {
            // 原因来自 QnnUsable 的 qnnNote（库缺失 / 默认不探测 / EP 缺失），
            // 独立于 backendNote（后者会被 EnsureModel 的 Vulkan 失败原因覆盖）
            r.note = s.qnnNote.isEmpty() ? QStringLiteral("QNN 探测未通过") : s.qnnNote;
        } else {
            const QString savedPref = PrefBackend();
            // 模型已在函数开头切好（probeName），这里只切后端偏好。
            // persist=false：测速不该改写用户的永久偏好（新机制下内存覆盖即可生效）。
            SetBackendPreference(QStringLiteral("qnn"), /*persist=*/false);
            ReleaseAll();
            if (!EnsureModel(probeName)) {
                r.note = s.err.isEmpty() ? QStringLiteral("NPU 会话构建失败") : s.err;
            } else if (s.backend != QLatin1String("qnn-htp")) {
                r.note = QStringLiteral("QNN 建会话回退（%1）").arg(s.backend);
            } else {
                const int elems = 3 * 192 * 192;
                std::vector<float> input(size_t(elems), 0.0f);
                for (int i = 0; i < elems; ++i)
                    input[size_t(i)] = float((i * 37 + 11) % 251) / 251.0f;
                std::vector<float> out;
                int ow = 0, oh = 0;
                const qint64 rss0 = FootprintMb();
                QElapsedTimer t;
                t.start();
                // RunTile 内部失败会自动回退 CPU（s.backend 变化），回退的
                // 结果不计入 NPU 数据
                const QString backendAtRun = s.backend;
                if (RunTile(input, out, ow, oh, 192)) {
                    if (s.backend == backendAtRun) {
                        // 推理后再问一次引擎：hello 的快照是建会话之前的，
                        // 只有 backend 明确写着 Hexagon NPU 才算真正跑在 NPU 上
                        //（QNN EP 接管失败时会话仍存在，但每块 tile 都过 CPU EP）
                        const QString info = QnnProcInfo();
                        if (info.contains(QLatin1String("Hexagon"), Qt::CaseInsensitive)) {
                            r.ok = true;
                            r.ms = double(t.elapsed());
                            r.note = info;
                        } else {
                            r.note = QStringLiteral("QNN 未真正接管（%1）")
                                         .arg(info.isEmpty() ? QStringLiteral("无响应") : info);
                        }
                    } else {
                        r.note = QStringLiteral("NPU 推理中途回退 %1").arg(s.backend);
                    }
                } else {
                    r.note = s.err.isEmpty() ? QStringLiteral("NPU 推理失败") : s.err;
                }
                const qint64 rss1 = FootprintMb();
                if (r.ok && rss0 >= 0 && rss1 > rss0)
                    r.memDeltaMb = rss1 - rss0;
            }
            SetBackendPreference(savedPref, /*persist=*/false);
            ReleaseAll();
            EnsureModel(QString());
        }
        rows.append(r);
    }
    // CPU 档：ORT 输入维度固定 192，单次计时（一次足以对比数量级）
    {
        SrPerfRow r;
        r.label = QStringLiteral("CPU（tile 192）");
        const int elems = 3 * 192 * 192;
        std::vector<float> input(size_t(elems), 0.0f);
        for (int i = 0; i < elems; ++i)
            input[size_t(i)] = float((i * 37 + 11) % 251) / 251.0f;
        std::vector<float> out;
        int ow = 0, oh = 0;
        const qint64 rss0 = FootprintMb();
        QElapsedTimer t;
        t.start();
        QString err;
        if (DebugOrtTile(path, input, &out, &ow, &oh, &err)) {
            r.ok = true;
            r.ms = double(t.elapsed());
        } else {
            r.note = err;
        }
        const qint64 rss1 = FootprintMb();
        if (r.ok && rss0 >= 0 && rss1 > rss0)
            r.memDeltaMb = rss1 - rss0;
        rows.append(r);
    }
    // 测试把 Vulkan 权重区销毁了：重建当前模型的会话，让下一次推理直接可用
    //（~2-5s，在调用方的工作线程里完成，不占 UI）
    ReleaseAll();
    EnsureModel(QString());
    return rows;
}

// 量化模型上 NPU 的"算得对不对"验证：同一块合成 tile 分别走 QNN 与 CPU，
// 比数值差。QDQ 图在 NPU 上跑通但输出错（量化参数没被解析、tile 尺寸不匹配）
// 时，光看耗时是发现不了的 —— 这个接口就是补那一刀。
QString CompareNpuCpu(const QString &modelFile, int *npuMs, int *cpuMs, double *maxAbs,
                      double *psnr)
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    if (npuMs)
        *npuMs = 0;
    if (cpuMs)
        *cpuMs = 0;
    if (maxAbs)
        *maxAbs = -1;
    if (psnr)
        *psnr = -1;

    QString probeName;
    if (!modelFile.isEmpty()) {
        for (const ModelInfo &mi : Catalog()) {
            if (mi.file == modelFile || mi.name == modelFile) {
                probeName = mi.name;
                break;
            }
        }
    }
    const Spec *spec = SpecByName(probeName.isEmpty() ? CurrentModel() : probeName);
    if (spec == nullptr)
        return QStringLiteral("unknown-model");

    const int tile = 192;
    const int elems = 3 * tile * tile;
    std::vector<float> input(size_t(elems), 0.0f);
    // 输入图案要接近真实照片（平滑大块渐变 + 低频纹理），不能用逐像素伪随机：
    // 那种高频噪声在 8-bit I/O 往返下会把误差放大成整数级差异，量出来的 PSNR
    // 只反映"合成图案最坏情况"，不代表用户看到的画质。
    for (int y = 0; y < tile; ++y) {
        for (int x = 0; x < tile; ++x) {
            const float grad = float(x + y) / float(2 * tile);
            const float tex = 0.06f * std::sin(float(x) * 0.11f) * std::cos(float(y) * 0.07f);
            for (int c = 0; c < 3; ++c) {
                const float v = grad * (0.75f + 0.1f * float(c)) + tex * (1.0f - 0.2f * float(c));
                input[size_t(c) * size_t(tile * tile) + size_t(y) * size_t(tile) + size_t(x)] =
                    std::min(1.0f, std::max(0.0f, v));
            }
        }
    }

    const QString savedPref = PrefBackend();
    // 桌面 Intel OpenVINO 插件 EP 也复用这条对比：按当前偏好选 NPU 档，
    // 验真时接受 ov-npu / ov-xpu，不再要求 QNN 子进程的 Hexagon 标记。
    const bool ovMode = savedPref == QLatin1String("ov-npu")
                        || savedPref == QLatin1String("xpu");
    const QString npuBackend = ovMode
                                   ? (savedPref == QLatin1String("xpu")
                                          ? QStringLiteral("ov-xpu")
                                          : QStringLiteral("ov-npu"))
                                   : QStringLiteral("qnn-htp");
    std::vector<float> outNpu;
    std::vector<float> outCpu;
    int ow = 0, oh = 0;

    // PrefBackend() 里 JM_SR_BACKEND 优先级最高，而 SetBackendPreference(false)
    // 只释放会话、不产生内存覆盖：必须临时改环境变量，否则两档都跑同一个后端
    //（曾经出现过 maxAbs=0 的"自己跟自己比"假通过）。
    const QByteArray savedEnv = qgetenv("JM_SR_BACKEND");
    const bool hadEnv = !savedEnv.isEmpty();
    qputenv("JM_SR_BACKEND", (ovMode ? savedPref : QStringLiteral("qnn")).toUtf8());
    SetBackendPreference(ovMode ? savedPref : QStringLiteral("qnn"), /*persist=*/false);
    ReleaseAll();
    QString note = QStringLiteral("npu=");
    if (!EnsureModel(probeName))
        note += QStringLiteral("session-fail(%1)").arg(s.err);
    else if (s.backend != npuBackend)
        note += QStringLiteral("not-npu(%1 %2)").arg(s.backend, s.backendNote);
    else {
        QElapsedTimer t;
        t.start();
        const bool ok = RunTile(input, outNpu, ow, oh, tile);
        const qint64 ms = t.elapsed();
        if (npuMs)
            *npuMs = int(ms);
        QString info;   // 桌面没有 QNN 子进程，这里保持空 → 判为未接管
#ifdef Q_OS_ANDROID
        info = QnnProcInfo();
#endif
        note += QStringLiteral("%1ms %2").arg(ms).arg(info);
        if (!ok || (!ovMode && !info.contains(QLatin1String("Hexagon"))))
            outNpu.clear();
    }

    qputenv("JM_SR_BACKEND", "cpu");
    SetBackendPreference(QStringLiteral("cpu"), /*persist=*/false);
    ReleaseAll();
    int cw = 0, chh = 0;
    if (!EnsureModel(probeName)) {
        note += QStringLiteral(" cpu=session-fail");
    } else {
        QElapsedTimer t;
        t.start();
        if (RunTile(input, outCpu, cw, chh, tile)) {
            const qint64 ms = t.elapsed();
            if (cpuMs)
                *cpuMs = int(ms);
            note += QStringLiteral(" cpu=%1ms").arg(ms);
        } else {
            note += QStringLiteral(" cpu=fail(%1)").arg(s.err);
            outCpu.clear();
        }
    }

    if (hadEnv)
        qputenv("JM_SR_BACKEND", savedEnv);
    else
        qunsetenv("JM_SR_BACKEND");
    SetBackendPreference(savedPref, /*persist=*/false);
    ReleaseAll();
    EnsureModel(QString());

    if (outNpu.empty() || outCpu.empty() || ow != cw || oh != chh)
        return QStringLiteral("compare=skip(%1)").arg(note);
    if (outNpu.size() != outCpu.size())
        return QStringLiteral("compare=size-mismatch(%1)").arg(note);

    double mx = 0.0;
    double se = 0.0;
    for (size_t i = 0; i < outNpu.size(); ++i) {
        const double d = double(outNpu[i]) - double(outCpu[i]);
        mx = std::max(mx, std::fabs(d));
        se += d * d;
    }
    const double mse = se / double(outNpu.size());
    const double p = mse <= 1e-12 ? 99.0 : 10.0 * std::log10(1.0 / mse);
    if (maxAbs)
        *maxAbs = mx;
    if (psnr)
        *psnr = p;
    return QStringLiteral("compare=ok out=%1x%2 maxAbs=%3 psnr=%4dB %5")
        .arg(ow)
        .arg(oh)
        .arg(mx, 0, 'g', 4)
        .arg(p, 0, 'f', 2)
        .arg(note);
}

QString PerfRecommendation(const QVector<SrPerfRow> &rows)
{
    const SrPerfRow *bestGpu = nullptr;
    const SrPerfRow *npu = nullptr;
    const SrPerfRow *cpu = nullptr;
    for (const SrPerfRow &r : rows) {
        if (!r.ok)
            continue;
        if (r.label.startsWith(QLatin1String("NPU")))
            npu = &r;
        else if (r.label.startsWith(QLatin1String("GPU"))
                 && (!bestGpu || r.ms < bestGpu->ms))
            bestGpu = &r;
        else if (r.label.startsWith(QLatin1String("CPU")))
            cpu = &r;
    }
    if (!bestGpu && !npu && !cpu)
        return QStringLiteral("没有可用的推理档位");
    // 排序：最快的硬件优先；给出与次快硬件的倍数关系
    const SrPerfRow *order[3] = {npu, bestGpu, cpu};
    const SrPerfRow *first = nullptr;
    const SrPerfRow *second = nullptr;
    for (const SrPerfRow *r : order) {
        if (!r)
            continue;
        if (!first || r->ms < first->ms) {
            second = first;
            first = r;
        } else if (!second || r->ms < second->ms) {
            second = r;
        }
    }
    auto nameOf = [](const SrPerfRow *r) -> QString {
        if (r->label.startsWith(QLatin1String("NPU")))
            return QStringLiteral("NPU（QNN HTP）");
        if (r->label.startsWith(QLatin1String("GPU")))
            return QStringLiteral("GPU（Vulkan）");
        return QStringLiteral("CPU（ONNX Runtime）");
    };
    QString s = QStringLiteral("推荐：%1").arg(nameOf(first));
    if (first == bestGpu) {
        const QRegularExpression re(QStringLiteral("tile (\\d+)"));
        const QRegularExpressionMatch m = re.match(first->label);
        if (m.hasMatch())
            s += QStringLiteral(" · tile %1").arg(m.captured(1).toInt());
    }
    if (second && second->ms > 0 && first->ms > 0)
        s += QStringLiteral(" · 约为 %1 的 %2×")
                 .arg(nameOf(second), QString::number(second->ms / first->ms, 'f', 1));
    return s;
}

int TileForCurrentModel()
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    if (!EnsureModel(QString()))
        return -1;
    const Spec *spec = SpecByName(s.modelName);
    if (!spec)
        return -1;
    return TileForModel(spec, true);
}

QString ModelPath()
{    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    EnsureModel(QString());
    return s.modelPath;
}

QString RuntimePath()
{
    QMutexLocker<QRecursiveMutex> lock(&S().mutex);
    EnsureRuntime();
    return S().runtime;
}

QString CurrentModel()
{
    State &s = S();
    if (!s.modelName.isEmpty())
        return s.modelName;
    static const QString pref = PrefModelName();
    if (!pref.isEmpty() && SpecByName(pref))
        return pref;
    static const QString fallback = FirstUsableModel();
    return fallback;
}

bool SetModel(const QString &name)
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    if (!SpecByName(name)) {
        s.err = QStringLiteral("未知模型：%1").arg(name);
        return false;
    }
    if (s.modelName == name && s.session)
        return true;
    const QString path = ResolveFile(SpecByName(name)->file);
    if (path.isEmpty()) {
        s.err = QStringLiteral("模型文件不存在：%1").arg(SpecByName(name)->file);
        return false;
    }
    // 立刻重建会话：界面点"使用"后就能在状态行看到真实 tile/输出尺寸
    s.modelName.clear();
    return EnsureModel(name);
}

int CurrentScale()
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    if (s.session && s.probed)
        return s.scale;
    const Spec *spec = SpecByName(CurrentModel());
    return spec ? spec->scale : 2;
}

Result Upscale(const QImage &src)
{
    return UpscaleEx(src, QString(), nullptr, nullptr);
}

Result UpscaleEx(const QImage &src, const QString &model,
                 const std::function<void(int done, int total)> &progress,
                 const std::atomic_bool *cancel)
{
    Result r;
    QElapsedTimer timer;
    timer.start();
    State &s = S();
    // 整个推理期间持锁：SetModel 不能在 Run 中途释放会话
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    if (!EnsureModel(model)) {
        r.error = s.err;
        return r;
    }
    r.model = s.modelName;
    r.scale = s.scale;
    if (src.isNull() || src.width() <= 0 || src.height() <= 0) {
        r.error = QStringLiteral("空图片");
        return r;
    }
    QImage rgb = src.convertToFormat(QImage::Format_RGB888);
    if (rgb.isNull()) {
        r.error = QStringLiteral("图片格式转换失败");
        return r;
    }
    const int w = rgb.width();
    const int h = rgb.height();
    const int scale = std::max(1, s.scale);
    const int tile = s.tile > 0 ? s.tile : kDefaultTile;
    // 输出上限：4 倍模型处理整页时一步就能吃掉几百 MB，先算清楚再分配
    const qint64 need = qint64(w) * h * scale * scale * 3;
    if (need > qint64(768) * 1024 * 1024) {
        r.error = QStringLiteral("图片过大（%1x%2 ×%3 需 %4MB），请先缩小")
                      .arg(w)
                      .arg(h)
                      .arg(scale)
                      .arg(need / (1024 * 1024));
        return r;
    }

    const int shrinkX = std::max(0, (tile * scale - s.outW) / 2);
    const int shrinkY = std::max(0, (tile * scale - s.outH) / 2);
    int pad = std::max((shrinkX + scale - 1) / scale, (shrinkY + scale - 1) / scale);
    if (pad <= 0)
        pad = std::min(16, std::max(4, tile / 8));
    const int step = tile - 2 * pad;
    if (step <= 0) {
        r.error = QStringLiteral("模型收缩过大，无法分块");
        return r;
    }
    const int readX = pad * scale - shrinkX;
    const int readY = pad * scale - shrinkY;

    const int srW = w * scale;
    const int srH = h * scale;
    std::vector<uint8_t> sr(static_cast<size_t>(srW) * srH * 3, 0);
    std::vector<float> input(static_cast<size_t>(tile) * tile * 3, 0.0f);
    std::vector<float> output;

    const int rows = (h + step - 1) / step;
    if (progress)
        progress(0, rows);

    // ---- Vulkan 多 tile 流水（JM_VK_NO_STREAM=1 可退回串行）----
    // tile 序列按行优先线性化后交给 Session::runTiles：GPU 算第 k 块时 CPU 在
    // 打包/提交第 k+1 块，消除每 tile 的 GPU 空窗。数值路径与逐 tile run()
    // 完全一致（同一组 dispatch），只是提交节奏不同。
    const int cols = (w + step - 1) / step;
    const int nTiles = rows * cols;
    const bool vkStream = s.vkReady && s.vk != nullptr && nTiles >= 3
                          && !qEnvironmentVariableIsSet("JM_VK_NO_STREAM");
    if (vkStream) {
        struct TilePos { int ox, oy; };
        QVector<TilePos> origins;
        origins.reserve(nTiles);
        for (int oy = 0; oy < h; oy += step)
            for (int ox = 0; ox < w; ox += step)
                origins.append({ox, oy});
        const int ow = s.vk->outW();
        const int oh = s.vk->outH();
        SrVk::TileStream ts;
        ts.input = [&](int k) -> const float * {
            const int ox = origins[k].ox;
            const int oy = origins[k].oy;
            for (int ty = 0; ty < tile; ++ty) {
                const int sy = ClampInt(oy - pad + ty, 0, h - 1);
                const uchar *line = rgb.constScanLine(sy);
                for (int tx = 0; tx < tile; ++tx) {
                    const int sx = ClampInt(ox - pad + tx, 0, w - 1);
                    const uchar *p = line + sx * 3;
                    const size_t idx = static_cast<size_t>(ty) * tile + tx;
                    input[idx] = p[0] * kInv255;
                    input[static_cast<size_t>(tile) * tile + idx] = p[1] * kInv255;
                    input[static_cast<size_t>(tile) * tile * 2 + idx] = p[2] * kInv255;
                }
            }
            return input.data();
        };
        ts.output = [&](int k, const float *outData) {
            const int ox = origins[k].ox;
            const int oy = origins[k].oy;
            const int rh = std::min(step, h - oy);
            const int rw = std::min(step, w - ox);
            const int copyW = std::min(rw * scale, ow - readX);
            const int copyH = std::min(rh * scale, oh - readY);
            if (copyW <= 0 || copyH <= 0)
                return;
            const size_t plane = static_cast<size_t>(ow) * oh;
            for (int y = 0; y < copyH; ++y) {
                const size_t srcRow = static_cast<size_t>(readY + y) * ow + readX;
                uint8_t *dst = sr.data()
                               + (static_cast<size_t>(oy * scale + y) * srW + ox * scale) * 3;
                const float *pr = outData + srcRow;
                const float *pg = outData + plane + srcRow;
                const float *pb = outData + plane * 2 + srcRow;
                for (int x = 0; x < copyW; ++x) {
                    const float rr = pr[x] * 255.0f + 0.5f;
                    const float gg = pg[x] * 255.0f + 0.5f;
                    const float bb = pb[x] * 255.0f + 0.5f;
                    dst[x * 3 + 0] = uint8_t(std::max(0.0f, std::min(255.0f, rr)));
                    dst[x * 3 + 1] = uint8_t(std::max(0.0f, std::min(255.0f, gg)));
                    dst[x * 3 + 2] = uint8_t(std::max(0.0f, std::min(255.0f, bb)));
                }
            }
        };
        ts.step = [&](int k) -> bool {
            if (cancel && cancel->load())
                return false;
            if (progress)
                progress(std::min(rows, origins[k].oy / step + 1), rows);
            return true;
        };
        QString streamErr;
        const bool ok = s.vk->runTiles(nTiles, ts, &streamErr);
        if (ok) {
            if (cancel && cancel->load()) {
                r.error = QStringLiteral("已取消");
                return r;
            }
            qInfo("sr: stream done %d tiles (%lldms) %s", nTiles,
                  static_cast<long long>(timer.elapsed()), qPrintable(s.vk->summary()));
            r.image = QImage(sr.data(), srW, srH, QImage::Format_RGB888).copy();
            r.ms = double(timer.elapsed());
            return r;
        }
        if (streamErr == QLatin1String("已取消")) {
            r.error = QStringLiteral("已取消");
            return r;
        }
        // 运行失败（DEVICE_LOST / 显存不足 / dispatch 错误）：与单 tile 路径
        // 相同的回退语义——RecoverToCpu 后整图按新 tile 重跑
        qWarning("sr: vulkan stream failed: %s", qPrintable(streamErr));
        s.err = streamErr;
        s.vkReady = false;
        if (RecoverToCpu()) {
            static thread_local bool streamRetry = false;
            if (!streamRetry && s.backend == QLatin1String("cpu") && s.tile != tile) {
                qWarning("sr: tile changed %d -> %d after CPU fallback, restarting upscale",
                         tile, s.tile);
                streamRetry = true;
                auto again = UpscaleEx(src, model, progress, cancel);
                streamRetry = false;
                return again;
            }
            // tile 未变（Vulkan 重建成功但本次已败）：落回下面的串行循环，
            // 后续 RunTile 会按新的会话状态选择后端
        } else {
            r.error = s.err.isEmpty() ? QStringLiteral("超分推理失败") : s.err;
            return r;
        }
    }

    for (int oy = 0, row = 0; oy < h; oy += step, ++row) {
        if (cancel && cancel->load())
            break;
        const int rh = std::min(step, h - oy);
        for (int ox = 0; ox < w; ox += step) {
            const int rw = std::min(step, w - ox);
            // NCHW 打包：越界 clamp 到边缘像素
            for (int ty = 0; ty < tile; ++ty) {
                const int sy = ClampInt(oy - pad + ty, 0, h - 1);
                const uchar *line = rgb.constScanLine(sy);
                for (int tx = 0; tx < tile; ++tx) {
                    const int sx = ClampInt(ox - pad + tx, 0, w - 1);
                    const uchar *p = line + sx * 3;
                    const size_t idx = static_cast<size_t>(ty) * tile + tx;
                    input[idx] = p[0] * kInv255;
                    input[static_cast<size_t>(tile) * tile + idx] = p[1] * kInv255;
                    input[static_cast<size_t>(tile) * tile * 2 + idx] = p[2] * kInv255;
                }
            }
            int ow = 0;
            int oh = 0;
            if (!RunTile(input, output, ow, oh, tile)) {
                // Vulkan 运行期失败回退 CPU 后，s.tile 会回到模型固定的 192；
                // 输入缓冲、pad、step 都是按旧 tile 算的，不能继续拼装，
                // 整图按新 tile 重跑一次（guard 防止无限递归）。
                static thread_local bool retrying = false;
                if (!retrying && s.backend == QLatin1String("cpu") && s.tile != tile) {
                    qWarning("sr: tile changed %d -> %d after CPU fallback, restarting upscale",
                             tile, s.tile);
                    retrying = true;
                    auto again = UpscaleEx(src, model, progress, cancel);
                    retrying = false;
                    return again;
                }
                r.error = s.err.isEmpty() ? QStringLiteral("超分推理失败") : s.err;
                return r;
            }
            const int copyW = std::min(rw * scale, ow - readX);
            const int copyH = std::min(rh * scale, oh - readY);
            if (copyW <= 0 || copyH <= 0) {
                r.error = QStringLiteral("模型输出有效区域为空");
                return r;
            }
            const size_t plane = static_cast<size_t>(ow) * oh;
            for (int y = 0; y < copyH; ++y) {
                const size_t srcRow = static_cast<size_t>(readY + y) * ow + readX;
                uint8_t *dst = sr.data()
                               + (static_cast<size_t>(oy * scale + y) * srW + ox * scale) * 3;
                const float *pr = output.data() + srcRow;
                const float *pg = output.data() + plane + srcRow;
                const float *pb = output.data() + plane * 2 + srcRow;
                for (int x = 0; x < copyW; ++x) {
                    const float rr = pr[x] * 255.0f + 0.5f;
                    const float gg = pg[x] * 255.0f + 0.5f;
                    const float bb = pb[x] * 255.0f + 0.5f;
                    dst[x * 3 + 0] = uint8_t(std::max(0.0f, std::min(255.0f, rr)));
                    dst[x * 3 + 1] = uint8_t(std::max(0.0f, std::min(255.0f, gg)));
                    dst[x * 3 + 2] = uint8_t(std::max(0.0f, std::min(255.0f, bb)));
                }
            }
        }
        if (progress)
            progress(row + 1, rows);
        qInfo("sr: row %d/%d done (%lldms)", oy + rh, h, static_cast<long long>(timer.elapsed()));
    }
    if (cancel && cancel->load()) {
        r.error = QStringLiteral("已取消");
        return r;
    }
    r.image = QImage(sr.data(), srW, srH, QImage::Format_RGB888).copy();
    r.ms = double(timer.elapsed());
    return r;
}

bool SelfTest(QString *report, QImage *out)
{
    QElapsedTimer t;
    t.start();
    bool ok = Available();
    QString text = ok ? QStringLiteral("sr-selftest: OK %1").arg(Status())
                      : QStringLiteral("sr-selftest: UNAVAILABLE %1").arg(Status());
    if (ok) {
        QImage probe(64, 64, QImage::Format_RGB888);
        for (int y = 0; y < 64; ++y) {
            uchar *line = probe.scanLine(y);
            for (int x = 0; x < 64; ++x) {
                line[x * 3 + 0] = uchar(x * 4);
                line[x * 3 + 1] = uchar(y * 4);
                line[x * 3 + 2] = uchar((x + y) * 2);
            }
        }
        const int scale = CurrentScale();
        Result r = Upscale(probe);
        ok = !r.image.isNull() && r.image.width() == 64 * scale
             && r.image.height() == 64 * scale;
        text += QStringLiteral(" · probe=%1x%2 · %3ms")
                    .arg(r.image.width())
                    .arg(r.image.height())
                    .arg(r.ms, 0, 'f', 0);
        if (!r.error.isEmpty())
            text += QStringLiteral(" · err=%1").arg(r.error);
        if (out)
            *out = r.image;
    }
    text += QStringLiteral(" · total=%1ms").arg(t.elapsed());
    if (report)
        *report = text;
    return ok;
}

QVector<BenchHw> BenchHardware()
{
    QVector<BenchHw> out;
    // GPU：逐个物理设备（多显卡时每块各一条）。失败也留一条不可用项，
    // 界面图例里能直接说明"为什么没有 GPU 数据"。
    QList<vk::DeviceInfo> devs;
    QString verr;
    if (vk::EnumerateDevices(&devs, &verr) && !devs.isEmpty()) {
        for (int i = 0; i < devs.size(); ++i) {
            BenchHw h;
            h.id = QStringLiteral("gpu%1").arg(i);
            h.shortLabel = QStringLiteral("GPU%1").arg(i);
            h.label = QStringLiteral("%1 · %2").arg(h.shortLabel, devs.at(i).name);
            h.available = true;
            out.push_back(h);
        }
    } else {
        BenchHw h;
        h.id = QStringLiteral("gpu");
        h.shortLabel = QStringLiteral("GPU");
        h.label = QStringLiteral("GPU");
        h.available = false;
        h.note = verr.isEmpty() ? QStringLiteral("没有可用的 Vulkan 设备") : verr;
        out.push_back(h);
    }
    // NPU（QNN HTP）
    {
        BenchHw h;
        h.id = QStringLiteral("npu");
        h.shortLabel = QStringLiteral("NPU");
        h.label = QStringLiteral("NPU · QNN HTP");
        h.available = QnnUsable();
        if (!h.available) {
            State &st = S();
            h.note = st.qnnNote.isEmpty() ? QStringLiteral("QNN 探测未通过") : st.qnnNote;
        }
        out.push_back(h);
    }
    // CPU 恒可用
    {
        BenchHw h;
        h.id = QStringLiteral("cpu");
        h.shortLabel = QStringLiteral("CPU");
        h.label = QStringLiteral("CPU · ONNX Runtime");
        h.available = true;
        out.push_back(h);
    }
    return out;
}

QVector<BenchSample> BenchModel(const QString &modelName, const QVector<QString> &hwIds,
                                int tile, int gpuIterations,
                                const std::function<void(const QString &)> &progress,
                                const std::atomic_bool *cancel)
{
    QVector<BenchSample> out;
    State &s = S();
    // 全程持锁：切 GPU/切后端都会重建会话，不能与页面推理并发
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    const Spec *spec = SpecByName(modelName);
    if (!spec)
        return out;
    const QString path = ResolveFile(spec->file);
    if (path.isEmpty())
        return out;

    // 量化模型与 SRMD 的图把输入边长写死成 192：调用方给的 tile 只对
    // Vulkan 那类"任意输入"的模型有意义，这两类必须钳回 192，否则子进程
    // 按 192 建好的会话会收到 128/160 的张量直接报维度错。
    const QString benchArch = QString::fromUtf8(spec->arch);
    const int t = (benchArch == QLatin1String("quant") || benchArch == QLatin1String("srmd"))
                      ? 192
                      : qBound(64, tile, 512);
    const int elems = 3 * t * t;
    std::vector<float> input(size_t(elems), 0.0f);
    for (int i = 0; i < elems; ++i)
        input[size_t(i)] = float((i * 37 + 11) % 251) / 251.0f;

    for (const QString &hwId : hwIds) {
        if (cancel && cancel->load())
            break;
        BenchSample r;
        r.hwId = hwId;
        if (progress)
            progress(hwId);

        if (hwId.startsWith(QLatin1String("gpu"))) {
            const int idx = hwId.mid(3).toInt();
            // 换物理设备必须先把旧会话/权重放掉（它们绑在上一块 GPU 上）
            ReleaseAll();
            vk::Device::I().Shutdown();
            QString err;
            if (!vk::Device::I().Ensure(&err, idx)) {
                r.note = err.isEmpty() ? QStringLiteral("Vulkan 设备不可用") : err;
            } else {
                QString rep;
                const SrVk::BenchResult b =
                    SrVk::Benchmark(path, t, qMax(1, gpuIterations), &rep);
                r.ok = b.ok;
                r.ms = b.medianMs;
                if (!b.ok)
                    r.note = b.note.isEmpty() ? QStringLiteral("基准失败") : b.note;
            }
        } else if (hwId == QLatin1String("npu")) {
            if (!QnnUsable()) {
                State &st = S();
                r.note = st.qnnNote.isEmpty() ? QStringLiteral("QNN 探测未通过") : st.qnnNote;
            } else {
                const QString savedPref = PrefBackend();
                SetBackendPreference(QStringLiteral("qnn"), /*persist=*/false);
                ReleaseAll();
                if (!EnsureModel(modelName)) {
                    r.note = s.err.isEmpty() ? QStringLiteral("NPU 会话构建失败") : s.err;
                } else if (s.backend != QLatin1String("qnn-htp")) {
                    r.note = QStringLiteral("QNN 建会话回退（%1）").arg(s.backend);
                } else {
                    std::vector<float> o;
                    int ow = 0, oh = 0;
                    QElapsedTimer timer;
                    timer.start();
                    const QString atRun = s.backend;
                    if (RunTile(input, o, ow, oh, t)) {
                        if (s.backend != atRun) {
                            r.note = QStringLiteral("NPU 推理中途回退 %1").arg(s.backend);
                        } else {
                            const QString info = QnnProcInfo();
                            if (info.contains(QLatin1String("Hexagon"), Qt::CaseInsensitive)) {
                                r.ok = true;
                                r.ms = double(timer.elapsed());
                            } else {
                                r.note = QStringLiteral("QNN 未真正接管（%1）")
                                             .arg(info.isEmpty() ? QStringLiteral("无响应") : info);
                            }
                        }
                    } else {
                        r.note = s.err.isEmpty() ? QStringLiteral("NPU 推理失败") : s.err;
                    }
                }
                SetBackendPreference(savedPref, /*persist=*/false);
                ReleaseAll();
            }
        } else if (hwId == QLatin1String("cpu")) {
            std::vector<float> o;
            int ow = 0, oh = 0;
            QString err;
            QElapsedTimer timer;
            timer.start();
            if (DebugOrtTile(path, input, &o, &ow, &oh, &err)) {
                r.ok = true;
                r.ms = double(timer.elapsed());
            } else {
                r.note = err.isEmpty() ? QStringLiteral("CPU 推理失败") : err;
            }
        } else {
            r.note = QStringLiteral("未知硬件：%1").arg(hwId);
        }
        out.push_back(r);
    }
    return out;
}

void BenchFinish(const QString &restoreModel, bool resetGpu)
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    ReleaseAll();
    if (resetGpu) {
        // 测速时把设备切到了某块 GPU；收尾必须切回默认（独显优先），
        // 否则后续阅读/超分会默默跑在副卡上。
        vk::Device::I().Shutdown();
        QString err;
        vk::Device::I().Ensure(&err, -1);
    }
    s.modelName.clear();
    EnsureModel(restoreModel);
}

bool ModelSweep(const QStringList &models, const QSize &probeSize, const QString &outDir,
                QString *report)
{
    const int w = qBound(32, probeSize.width(), 512);
    const int h = qBound(32, probeSize.height(), 512);
    QImage probe(w, h, QImage::Format_RGB888);
    for (int y = 0; y < h; ++y) {
        uchar *line = probe.scanLine(y);
        for (int x = 0; x < w; ++x) {
            line[x * 3 + 0] = uchar(40 + (x * 180) / w);
            line[x * 3 + 1] = uchar(30 + (y * 200) / h);
            line[x * 3 + 2] = uchar(20 + ((x + y) * 160) / (w + h));
        }
    }
    QVector<ModelInfo> all = InstalledModels();
    QStringList lines;
    int pass = 0;
    int fail = 0;
    std::fprintf(stderr, "sr-model-test: start installed=%d probe=%dx%d\n", int(all.size()), w, h);
    std::fflush(stderr);
    for (const ModelInfo &m : all) {
        if (!models.isEmpty() && !models.contains(m.name))
            continue;
        std::fprintf(stderr, "sr-model-test: try %s\n", m.name.toUtf8().constData());
        std::fflush(stderr);
        QElapsedTimer t;
        t.start();
        Result r = UpscaleEx(probe, m.name, nullptr, nullptr);
        const bool ok = !r.image.isNull() && r.image.width() == w * m.scale
                        && r.image.height() == h * m.scale;
        ++(ok ? pass : fail);
        QString saved;
        if (ok && !outDir.isEmpty()) {
            QDir().mkpath(outDir);
            saved = QDir(outDir).filePath(QStringLiteral("%1.png").arg(
                m.file.left(m.file.size() - 5)));
            r.image.save(saved);
        }
        const QString line =
            QStringLiteral("sr-model: name=%1 scale=%2 ok=%3 %4x%5 ms=%6 err=%7%8")
                .arg(m.name)
                .arg(m.scale)
                .arg(int(ok))
                .arg(r.image.width())
                .arg(r.image.height())
                .arg(t.elapsed())
                .arg(r.error.isEmpty() ? QStringLiteral("-") : r.error)
                .arg(saved.isEmpty() ? QString() : QStringLiteral(" out=%1").arg(saved));
        lines << line;
        // 直接写 stderr：这些行要在 JmLog 安装之前（CLI 自检）就能看见，
        // qWarning 在那个阶段还没被接管，设备侧则由 logcat 收集。
        std::fprintf(stderr, "%s\n", line.toUtf8().constData());
        std::fflush(stderr);
    }
    const QString summary = QStringLiteral("sr-model-test: models=%1 pass=%2 fail=%3")
                                .arg(pass + fail)
                                .arg(pass)
                                .arg(fail);
    lines << summary;
    std::fprintf(stderr, "%s\n", summary.toUtf8().constData());
    std::fflush(stderr);
    if (report)
        *report = lines.join(QLatin1Char('\n'));
    return fail == 0 && pass > 0;
}

void Shutdown()
{
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    ReleaseAll();
}

// ---------------------------------------------------------------- 调试：用 ORT 跑同一个 tile
// 用途：Vulkan 与 CPU 的逐 tile 数值对比（--vk-compare）。刻意不碰 State 里的会话，
// 免得把用户当前模型挤掉；代价是每次比较重建一个 session（只在回归时用）。
bool DebugOrtTile(const QString &modelPath, const std::vector<float> &input,
                  std::vector<float> *output, int *outW, int *outH, QString *error)
{
    auto fail = [&](const QString &m) {
        if (error)
            *error = m;
        return false;
    };
    State &s = S();
    QMutexLocker<QRecursiveMutex> lock(&s.mutex);
    if (!EnsureRuntime())
        return fail(QStringLiteral("ORT 运行时不可用：%1").arg(s.runtime));
    if (input.empty())
        return fail(QStringLiteral("输入为空"));

    OrtSessionOptions *opts = nullptr;
    OrtSession *session = nullptr;
    OrtMemoryInfo *mem = nullptr;
    auto check = [&](OrtStatus *st) {
        if (!st)
            return true;
        s.err = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
        return false;
    };
    if (!check(s.api->CreateSessionOptions(&opts)))
        return fail(s.err);
    s.api->SetIntraOpNumThreads(opts, qBound(1, QThread::idealThreadCount() / 2, 4));
    s.api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL);
    OrtStatus *st = nullptr;
    // Android 的模型是 assets:/sr_models/... 这种 Qt 伪路径，ORT 打不开；
    // 与 EnsureModel 一致，先把整份权重读进内存再 CreateSessionFromArray。
    QByteArray blob;
    if (IsAssetPath(modelPath)) {
        QFile f(modelPath);
        if (!f.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("读取内置模型失败：%1").arg(modelPath));
        blob = f.readAll();
        f.close();
        st = s.api->CreateSessionFromArray(s.env, blob.constData(), size_t(blob.size()), opts,
                                           &session);
    } else {
#ifdef _WIN32
        const std::wstring wpath = QDir::toNativeSeparators(modelPath).toStdWString();
        st = s.api->CreateSession(s.env, wpath.c_str(), opts, &session);
#else
        const QByteArray upath = modelPath.toUtf8();
        st = s.api->CreateSession(s.env, upath.constData(), opts, &session);
#endif
    }
    if (st) {
        s.api->ReleaseStatus(st);
        s.api->ReleaseSessionOptions(opts);
        return fail(QStringLiteral("ORT 建会话失败：%1").arg(QFileInfo(modelPath).fileName()));
    }
    s.api->ReleaseSessionOptions(opts);
    if (!check(s.api->GetAllocatorWithDefaultOptions(&s.alloc))
        || !check(s.api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem))) {
        s.api->ReleaseSession(session);
        return fail(s.err);
    }
    char *inName = nullptr;
    char *outName = nullptr;
    if (!check(s.api->SessionGetInputName(session, 0, s.alloc, &inName))
        || !check(s.api->SessionGetOutputName(session, 0, s.alloc, &outName))) {
        s.api->ReleaseSession(session);
        s.api->ReleaseMemoryInfo(mem);
        return fail(s.err);
    }
    // 由输入元素数反推 tile（NCHW，3 通道）
    const int tile = int(std::lround(std::sqrt(double(input.size()) / 3.0)));
    const int64_t dims[4] = {1, 3, tile, tile};
    OrtValue *inVal = nullptr;
    if (!check(s.api->CreateTensorWithDataAsOrtValue(
            mem, const_cast<float *>(input.data()), input.size() * sizeof(float), dims, 4,
            ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inVal))) {
        s.api->ReleaseSession(session);
        s.api->ReleaseMemoryInfo(mem);
        return fail(s.err);
    }
    const char *inNames[1] = {inName};
    const char *outNames[1] = {outName};
    OrtValue *outVal = nullptr;
    st = s.api->Run(session, nullptr, inNames, &inVal, 1, outNames, 1, &outVal);
    const bool runOk = (st == nullptr);
    if (!runOk) {
        s.err = QString::fromUtf8(s.api->GetErrorMessage(st));
        s.api->ReleaseStatus(st);
    }
    bool ok = runOk;
    if (ok) {
        OrtTensorTypeAndShapeInfo *info = nullptr;
        if (!check(s.api->GetTensorTypeAndShape(outVal, &info))) {
            ok = false;
        } else {
            size_t count = 0;
            s.api->GetTensorShapeElementCount(info, &count);
            std::vector<int64_t> shape(count ? 4 : 0, 0);
            size_t dimCount = 0;
            s.api->GetDimensionsCount(info, &dimCount);
            std::vector<int64_t> d(dimCount, 0);
            if (dimCount > 0)
                s.api->GetDimensions(info, d.data(), dimCount);
            if (d.size() == 4) {
                if (outH)
                    *outH = int(d[2]);
                if (outW)
                    *outW = int(d[3]);
            }
            float *data = nullptr;
            if (!check(s.api->GetTensorMutableData(outVal, reinterpret_cast<void **>(&data)))) {
                ok = false;
            } else {
                output->assign(data, data + count);
            }
            s.api->ReleaseTensorTypeAndShapeInfo(info);
        }
    }
    if (outVal)
        s.api->ReleaseValue(outVal);
    s.api->ReleaseValue(inVal);
    s.api->ReleaseSession(session);
    s.api->ReleaseMemoryInfo(mem);
    if (outName)
        s.alloc->Free(s.alloc, outName);
    if (inName)
        s.alloc->Free(s.alloc, inName);
    if (!ok)
        return fail(s.err.isEmpty() ? QStringLiteral("ORT 推理失败") : s.err);
    return true;
}

}  // namespace SrEngine
