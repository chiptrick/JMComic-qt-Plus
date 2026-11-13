#include "sr_engine.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <QMutexLocker>
#include <QRecursiveMutex>
#include <QSettings>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstring>
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

const Spec *SpecByName(const QString &name)
{
    for (const Spec &s : kCatalog) {
        if (name == QString::fromUtf8(s.name))
            return &s;
    }
    return nullptr;
}

const Spec *SpecByFile(const QString &file)
{
    for (const Spec &s : kCatalog) {
        if (file == QString::fromUtf8(s.file))
            return &s;
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

void ReleaseAll()
{
    State &s = S();
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

// 会话建立后量一次真实输出边长：cunet 2x 输入 192 只出 312（每边收 36），
// 1x 出 136。不量就只能按倍数猜，拼接处会出现位移/缺带。
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
    if (s.session && s.modelName == name && s.probed)
        return true;

    // 换模型时整段运行时重建（而不是只换 session）：连续切换十几个模型后，
    // ORT 的 arena/线程池不会全部还给系统，第 30 个模型（64MB 的 x4plus）
    // 会在建会话时直接崩掉（桌面实测）。重建 env 只有几十毫秒，
    // 换模型本来就是用户手点的一次操作，代价可以接受。
    if (s.session && s.modelName != name)
        ReleaseAll();

    if (!EnsureRuntime())
        return false;

    const Spec *spec = SpecByName(name);
    if (!spec) {
        s.err = QStringLiteral("未知模型：%1").arg(name);
        return false;
    }
    const QString path = ResolveFile(QString::fromUtf8(spec->file));
    if (path.isEmpty()) {
        s.err = QStringLiteral("模型文件不存在：%1（sr_models/ 下缺少该文件）")
                    .arg(QString::fromUtf8(spec->file));
        return false;
    }

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

    if (!check(s.api->CreateSessionOptions(&s.opts)))
        return false;
    const int threads = qBound(1, QThread::idealThreadCount() / 2, 4);
    s.api->SetIntraOpNumThreads(s.opts, threads);
    s.api->SetSessionGraphOptimizationLevel(s.opts, ORT_ENABLE_ALL);

    if (IsAssetPath(path)) {
        // APK 资源不是文件系统路径：读进内存后 CreateSessionFromArray
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            s.err = QStringLiteral("读取内置模型失败：%1").arg(path);
            return false;
        }
        const QByteArray blob = f.readAll();
        f.close();
        std::fprintf(stderr, "sr-trace: create(asset) %s %lld bytes\n",
                     qPrintable(QFileInfo(path).fileName()), static_cast<long long>(blob.size()));
        std::fflush(stderr);
        if (!check(s.api->CreateSessionFromArray(s.env, blob.constData(), size_t(blob.size()),
                                                 s.opts, &s.session)))
            return false;
        qInfo("sr: model from asset %s (%lld bytes)", qPrintable(path),
              static_cast<long long>(blob.size()));
    } else {
#ifdef _WIN32
        const std::wstring wpath = QDir::toNativeSeparators(path).toStdWString();
        std::fprintf(stderr, "sr-trace: create(file) %s\n", qPrintable(path));
        std::fflush(stderr);
        if (!check(s.api->CreateSession(s.env, wpath.c_str(), s.opts, &s.session)))
            return false;
#else
        const QByteArray upath = path.toUtf8();
        std::fprintf(stderr, "sr-trace: create(file) %s\n", qPrintable(path));
        std::fflush(stderr);
        if (!check(s.api->CreateSession(s.env, upath.constData(), s.opts, &s.session)))
            return false;
#endif
    }
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
    s.tile = spec->tile > 0 ? spec->tile : kDefaultTile;
    s.probed = false;
    return ProbeGeometry();
}

bool RunTile(const std::vector<float> &input, std::vector<float> &output, int &outW, int &outH,
             int tile)
{
    State &s = S();
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
    return QStringLiteral("%1 · CPU · %2px tile · %3")
        .arg(s.modelName)
        .arg(s.tile)
        .arg(QFileInfo(s.modelPath).fileName());
}

QString ModelPath()
{
    State &s = S();
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

}  // namespace SrEngine
