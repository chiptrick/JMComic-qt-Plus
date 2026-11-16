#include "vk_api.h"

#include <QLibrary>
#include <QVector>

#include <cstring>

namespace vk {
namespace {

Api g_api;

#ifdef Q_OS_ANDROID
const char *kLoaderName = "libvulkan.so";
#else
const char *kLoaderName = "vulkan-1";
#endif

template <typename T>
bool Resolve(T *slot, PFN_vkGetInstanceProcAddr getProc, VkInstance inst, const char *name,
             QString *missing)
{
    if (*slot)
        return true;
    auto fn = getProc(inst, name);
    if (!fn) {
        if (missing && !missing->contains(QString::fromLatin1(name)))
            *missing += QString::fromLatin1(name) + QLatin1Char(' ');
        return false;
    }
    *slot = reinterpret_cast<T>(fn);
    return true;
}

}  // namespace

Api &Get()
{
    return g_api;
}

QString ResultText(VkResult r)
{
    switch (r) {
    case VK_SUCCESS:
        return QStringLiteral("VK_SUCCESS");
    case VK_NOT_READY:
        return QStringLiteral("VK_NOT_READY");
    case VK_TIMEOUT:
        return QStringLiteral("VK_TIMEOUT");
    case VK_INCOMPLETE:
        return QStringLiteral("VK_INCOMPLETE");
    case VK_ERROR_OUT_OF_HOST_MEMORY:
        return QStringLiteral("VK_ERROR_OUT_OF_HOST_MEMORY");
    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        return QStringLiteral("VK_ERROR_OUT_OF_DEVICE_MEMORY");
    case VK_ERROR_INITIALIZATION_FAILED:
        return QStringLiteral("VK_ERROR_INITIALIZATION_FAILED");
    case VK_ERROR_DEVICE_LOST:
        return QStringLiteral("VK_ERROR_DEVICE_LOST");
    case VK_ERROR_MEMORY_MAP_FAILED:
        return QStringLiteral("VK_ERROR_MEMORY_MAP_FAILED");
    case VK_ERROR_LAYER_NOT_PRESENT:
        return QStringLiteral("VK_ERROR_LAYER_NOT_PRESENT");
    case VK_ERROR_EXTENSION_NOT_PRESENT:
        return QStringLiteral("VK_ERROR_EXTENSION_NOT_PRESENT");
    case VK_ERROR_FEATURE_NOT_PRESENT:
        return QStringLiteral("VK_ERROR_FEATURE_NOT_PRESENT");
    case VK_ERROR_INCOMPATIBLE_DRIVER:
        return QStringLiteral("VK_ERROR_INCOMPATIBLE_DRIVER");
    case VK_ERROR_TOO_MANY_OBJECTS:
        return QStringLiteral("VK_ERROR_TOO_MANY_OBJECTS");
    case VK_ERROR_FORMAT_NOT_SUPPORTED:
        return QStringLiteral("VK_ERROR_FORMAT_NOT_SUPPORTED");
    case VK_ERROR_FRAGMENTED_POOL:
        return QStringLiteral("VK_ERROR_FRAGMENTED_POOL");
    case VK_ERROR_OUT_OF_POOL_MEMORY:
        return QStringLiteral("VK_ERROR_OUT_OF_POOL_MEMORY");
    default:
        return QStringLiteral("VkResult(%1)").arg(int(r));
    }
}

bool Load()
{
    Api &a = g_api;
    if (a.loaded)
        return true;
    if (!a.error.isEmpty())
        return false;

    // 必须是 static 且禁止卸载：QLibrary 析构会 dlclose，函数指针立刻变悬空
    // （第一次实测就是"解析全部成功、调用即段错误"）。
    static QLibrary lib;
    lib.setFileName(QString::fromLatin1(kLoaderName));
    lib.setLoadHints(QLibrary::PreventUnloadHint);
    if (!lib.load()) {
        a.error = QStringLiteral("缺少 Vulkan loader（%1）：%2")
                      .arg(QString::fromLatin1(kLoaderName), lib.errorString());
        return false;
    }
    a.loaderPath = lib.fileName();

    auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(lib.resolve("vkGetInstanceProcAddr"));
    if (!gipa) {
        a.error = QStringLiteral("Vulkan loader 缺少 vkGetInstanceProcAddr（%1）").arg(lib.fileName());
        return false;
    }
    a.GetInstanceProcAddr = gipa;

    // instance 之前只能拿到这三个（Windows loader 的 vkGetInstanceProcAddr 在
    // instance=null 时对其余名字一律返回 null，这不是缺函数，是调用时机不对）
    QString missing;
    Resolve(&a.EnumerateInstanceVersion, gipa, nullptr, "vkEnumerateInstanceVersion", &missing);
    Resolve(&a.CreateInstance, gipa, nullptr, "vkCreateInstance", &missing);
    if (!a.CreateInstance) {
        a.error = QStringLiteral("Vulkan loader 缺少必需入口：%1").arg(missing.trimmed());
        return false;
    }
    a.loaded = true;
    return true;
}

bool ResolveInstance(VkInstance inst)
{
    Api &a = g_api;
    if (!a.loaded || inst == VK_NULL_HANDLE) {
        a.error = QStringLiteral("ResolveInstance 需要已加载的 loader 与有效 instance");
        return false;
    }
    QString missing;
    auto gipa = a.GetInstanceProcAddr;
    Resolve(&a.DestroyInstance, gipa, inst, "vkDestroyInstance", &missing);
    Resolve(&a.GetDeviceProcAddr, gipa, inst, "vkGetDeviceProcAddr", &missing);
    Resolve(&a.EnumeratePhysicalDevices, gipa, inst, "vkEnumeratePhysicalDevices", &missing);
    Resolve(&a.GetPhysicalDeviceProperties, gipa, inst, "vkGetPhysicalDeviceProperties", &missing);
    Resolve(&a.GetPhysicalDeviceFeatures, gipa, inst, "vkGetPhysicalDeviceFeatures", &missing);
    Resolve(&a.GetPhysicalDeviceMemoryProperties, gipa, inst,
            "vkGetPhysicalDeviceMemoryProperties", &missing);
    Resolve(&a.GetPhysicalDeviceQueueFamilyProperties, gipa, inst,
            "vkGetPhysicalDeviceQueueFamilyProperties", &missing);
    Resolve(&a.CreateDevice, gipa, inst, "vkCreateDevice", &missing);
    // 1.1+ 入口可选：解析不到只是能力降级（fp16 / push descriptor 关闭）
    Resolve(&a.EnumerateDeviceExtensionProperties, gipa, inst,
            "vkEnumerateDeviceExtensionProperties", &missing);
    Resolve(&a.GetPhysicalDeviceFeatures2, gipa, inst, "vkGetPhysicalDeviceFeatures2", &missing);
    if (!a.GetPhysicalDeviceFeatures2)
        Resolve(&a.GetPhysicalDeviceFeatures2, gipa, inst,
                "vkGetPhysicalDeviceFeatures2KHR", &missing);
    if (!a.EnumeratePhysicalDevices || !a.CreateDevice || !a.GetDeviceProcAddr) {
        a.error = QStringLiteral("instance 级入口缺失：%1").arg(missing.trimmed());
        return false;
    }
    return true;
}

bool EnumerateDevices(QList<DeviceInfo> *out, QString *error)
{
    if (!Load()) {
        if (error)
            *error = g_api.error;
        return false;
    }
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "jmcomic-vk-probe";
    app.apiVersion = VK_API_VERSION_1_0;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = g_api.CreateInstance(&ci, nullptr, &inst);
    if (r != VK_SUCCESS) {
        if (error)
            *error = QStringLiteral("vkCreateInstance 失败：%1").arg(ResultText(r));
        return false;
    }
    if (!ResolveInstance(inst)) {
        if (error)
            *error = g_api.error;
        return false;
    }
    quint32 count = 0;
    g_api.EnumeratePhysicalDevices(inst, &count, nullptr);
    QVector<VkPhysicalDevice> devs(static_cast<int>(count));
    if (count > 0)
        g_api.EnumeratePhysicalDevices(inst, &count, devs.data());

    for (quint32 i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p{};
        g_api.GetPhysicalDeviceProperties(devs[int(i)], &p);
        DeviceInfo info;
        info.name = QString::fromUtf8(p.deviceName);
        info.vendorId = p.vendorID;
        info.deviceId = p.deviceID;
        info.type = p.deviceType;
        info.apiVersion = p.apiVersion;
        info.driverVersion = p.driverVersion;
        info.maxWorkGroupInvocations = p.limits.maxComputeWorkGroupInvocations;
        info.maxWorkGroupSize[0] = p.limits.maxComputeWorkGroupSize[0];
        info.maxWorkGroupSize[1] = p.limits.maxComputeWorkGroupSize[1];
        info.maxWorkGroupSize[2] = p.limits.maxComputeWorkGroupSize[2];
        info.maxStorageBufferRange = p.limits.maxStorageBufferRange;
        info.maxComputeSharedMemory = p.limits.maxComputeSharedMemorySize;

        VkPhysicalDeviceMemoryProperties mp{};
        g_api.GetPhysicalDeviceMemoryProperties(devs[int(i)], &mp);
        for (quint32 h = 0; h < mp.memoryHeapCount; ++h) {
            if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                info.vramBytes = qMax(info.vramBytes, quint64(mp.memoryHeaps[h].size));
        }
        if (out)
            out->append(info);
    }
    // 用 instance 层销毁：此处只需枚举，立即销毁（DestroyInstance 可能未解析）
    if (g_api.DestroyInstance)
        g_api.DestroyInstance(inst, nullptr);
    if (count == 0) {
        if (error)
            *error = QStringLiteral("没有可用 Vulkan 物理设备（驱动/显卡不支持）");
        return false;
    }
    return true;
}

QString DeviceInfo::typeName() const
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return QStringLiteral("igpu");
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return QStringLiteral("dgpu");
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return QStringLiteral("vgpu");
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return QStringLiteral("cpu");
    default:
        return QStringLiteral("other");
    }
}

QString DeviceInfo::summary() const
{
    return QStringLiteral("name=%1 type=%2 api=%3.%4.%5 vramMB=%6 wgInvoc=%7 sbRangeMB=%8 shmemKB=%9")
        .arg(name, typeName())
        .arg(VK_VERSION_MAJOR(apiVersion))
        .arg(VK_VERSION_MINOR(apiVersion))
        .arg(VK_VERSION_PATCH(apiVersion))
        .arg(vramBytes / (1024 * 1024))
        .arg(maxWorkGroupInvocations)
        .arg(maxStorageBufferRange / (1024 * 1024))
        .arg(maxComputeSharedMemory / 1024);
}

}  // namespace vk
