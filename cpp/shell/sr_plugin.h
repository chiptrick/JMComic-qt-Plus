// 超分模型包（可独立下载/安装/卸载/删除）管理。
//
// 设计目标（本轮回应用户需求）：
//   * 桌面与移动端一致：模型不再是"必须随包"的一坨 260MB，而是按需下载、可卸载删除的
//     **模型包**；用户只装自己用得上的那几个。
//   * 后端插件化（ncnn_Vulkan / sr_qnn / onnxruntime / OpenVINO）另做，本轮先落"模型包"
//     这条主链，因为模型包是后端插件的前置验证，且改动面可控、用户价值最直接。
//
// 落盘：所有模型安装到 SrPlugin::PluginsDir()（桌面 <Data>/sr_models，Android filesDir 下
//   同名目录），并加入 SrEngine 的模型搜索路径（优先级高于安装目录、低于显式 env）。
//   卸载即删该目录里的文件，不动随包 assets 的模型。
//
// 清单：优先用内嵌资源 :/sr/sr_models_manifest.json（离线可用），可用
//   RefreshManifest() 从网络（jsdelivr）拉最新并缓存到 DataDir，再次启动优先用缓存。
//
// 安装流程：临时文件下载 -> sha256 校验 -> 校验通过后原子改名到 PluginsDir。
//   校验失败/中断不留半成品；同名已存在则覆盖（升级）。
#pragma once

#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <functional>

namespace SrPlugin {

// 一个模型包在清单里的定义 + 运行时安装状态
struct Package {
    QString file;        // onnx 文件名（唯一键，也是下载名）
    QString name;        // 显示名
    QString arch;        // waifu2x/realcugan/realesrgan/srmd/quant（缺省时从文件名推断）
    int scale = 2;
    int denoise = 0;
    int tile = 192;
    qint64 size = 0;     // 清单声明的字节数（0=未知）
    QString sha256;      // 清单声明的 sha256（空=不校验，仅限调试）

    bool installed = false;    // 任意模型目录能解析到（含随包 assets）
    bool inPluginDir = false;  // 位于受管插件目录（可卸载删除）
    qint64 installedBytes = 0;

    // 运行时状态（来自 g_status）
    QString state;       // not_installed / downloading / verifying / installed / error
    int progress = -1;   // 0..100，-1=无进度
    QString error;
};

// 清单里全部模型包（含未安装）。
QVector<Package> Catalog();
// 已安装的模型包。
QVector<Package> Installed();

// 受管插件目录（已 mkpath）。桌面/Android 都落在应用数据目录下。
QString PluginsDir();
// 清单 base_url（下载用的前缀）。
QString ManifestBaseUrl();
// 清单当前来源：embedded / cache / file 之一，排障用。
QString ManifestSource();

// 从网络刷新清单并写缓存；失败返回 false，err 给原因。
bool RefreshManifest(QString *err = nullptr);

// 安装一个包（按 file 精确匹配，或按 display name）。同步阻塞：内部起 QNetworkAccessManager
// + 本地事件循环下载，progress(done,total) 每 64KB 回调一次；cancel 置位尽快中止。
// 成功返回 true 且包已落到 PluginsDir、sha256 校验通过。
bool Install(const QString &fileOrName, const std::function<void(qint64, qint64)> &progress,
             const std::function<bool()> &cancel, QString *err = nullptr);

// 卸载/删除：删除 PluginsDir 里的该文件；alsoBundled=true 时同时尝试删除其它可写模型目录
// 里的同名文件（随包 assets 只读，删不掉会如实报错）。
bool Uninstall(const QString &fileOrName, bool alsoBundled, QString *err = nullptr);

// 轮询某包状态（UI 进度条用）。
QVariantMap StateFor(const QString &fileOrName);

// 轻量状态快照：file -> {state, progress}。只读内存里的下载/校验态，不扫目录、
// 不解析清单，适合 UI 高频率轮询（列表每一行要状态时用）。
struct StatusEntry {
    QString state;   // idle/downloading/verifying/error
    int progress = -1;
};
QHash<QString, StatusEntry> StatusSnapshot();
// 当前是否有任务在进行（downloading/verifying）。UI 据此决定是否继续轮询。
bool AnyActive();

// ---------------------------------------------------------------- 推理模块（ONNX Runtime）
// 桌面端「超分模块」= onnxruntime 推理运行时。它体积 ~19MB，随安装包分发占空间，
// 因此支持在应用内从 GitHub 按需下载到 <data>/sr_runtime/，SrEngine 优先加载它。
struct RuntimeInfo {
    QString id;        // 清单 id
    QString name;      // 显示名
    QString file;      // onnxruntime.dll
    QString version;
    qint64 size = 0;
    QString sha256;
    bool installed = false;      // 是否已在 sr_runtime/ 里
    bool active = false;         // 当前是否可被加载（sr_runtime/ 或随包目录里有）
    qint64 installedBytes = 0;
    QString state;               // idle/downloading/verifying/error/installed
    int progress = -1;
    QString error;
};
// 清单里的推理模块（按平台过滤；本机当前平台优先展示）。
QVector<RuntimeInfo> RuntimeCatalog();
// 该平台本机是否"已具备可加载的推理模块"（sr_runtime/ 或随安装包）。
bool RuntimeInstalled();
// 当前实际被加载/将被加载的运行时路径（可能为空）。
QString RuntimePath();

// 下载并安装推理模块（同步阻塞，内部起事件循环；progress 每 64KB 回调；cancel 中止）。
// 成功后调用 SrEngine::UnloadRuntime()，下次超分即用新运行时。
bool InstallRuntime(const QString &idOrFile,
                    const std::function<void(qint64, qint64)> &progress,
                    const std::function<bool()> &cancel, QString *err = nullptr);
// 删除已下载的推理模块（仅删 sr_runtime/ 里的那份，不动随安装包；不卸载正在运行的库）。
bool UninstallRuntime(const QString &idOrFile, QString *err = nullptr);

// ---------------------------------------------------------------- 超分后端插件（桌面）
// 把 OpenVINO 推理插件、SR-Vulkan 着色器包等做成可独立安装/删除的"后端插件包"。
// 每个后端一个 tar.gz，下载解压到 <data>/sr_backend/<targetSubdir>/；
// SrEngine（OpenVINO EP）与 SrVk（vkshader）会在该目录里优先查找插件/着色器。
struct BackendPluginInfo {
    QString id;
    QString name;
    QString desc;
    QString kind;            // shaders | binary
    QString platform;        // win-x64 ...
    QString targetSubdir;    // ovep / vkshaders
    bool installedByDefault = false;
    bool requiresBinary = false;   // 需要额外的 ORT 二进制（缺则不可用）
    qint64 archiveSize = 0;
    QString archiveSha256;
    int fileCount = 0;
    // ---- 运行时 ----
    bool installed = false;        // 目标目录里已有文件（受管或随包）
    bool managedInstalled = false;  // 在受管目录 <data>/sr_backend/ 里（可卸载）
    bool available = false;         // 当前构建下后端可用
    QString state;            // idle/downloading/verifying/error/installed
    int progress = -1;
    QString error;
};

// 受管后端插件根目录（<data>/sr_backend，已 mkpath）。
QString BackendPackagesDir();
// 清单里的后端插件（本平台优先）。
QVector<BackendPluginInfo> BackendCatalog();
// 该后端是否已安装（目标目录里有文件，或默认随包且随包目录存在）。
bool BackendInstalled(const QString &id);
// 下载并安装一个后端插件（tar.gz -> 解压 -> 逐文件 sha256 校验 -> 原子落位）。
bool InstallBackend(const QString &id,
                    const std::function<void(qint64, qint64)> &progress,
                    const std::function<bool()> &cancel, QString *err = nullptr);
// 删除后端插件（仅删 <data>/sr_backend/<targetSubdir>；随包内置不可删）。
bool UninstallBackend(const QString &id, QString *err = nullptr);

// 一行 ASCII 汇总（--plugin-list / 报告）。
QString Report();
// 离线自检：清单可解析、每项字段合法、PluginsDir 可写、命名唯一。返回 ASCII 结论。
QString SelfTest();

}  // namespace SrPlugin
