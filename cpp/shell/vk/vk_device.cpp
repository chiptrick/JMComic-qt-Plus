#include "vk_device.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QtGui/rhi/qshader.h>
#include <QVector>

#include <cstring>

namespace vk {
namespace {

// Vulkan11/12Features 的 sType 枚举名在 Khronos 头的草案期与正式期不同
//（桌面随仓头用 VULKAN_1_2_FEATURES，NDK 新头用 VULKAN_12_FEATURES，数值一致）。
// 16bit storage 归属 Vulkan11Features（shaderFloat16 才是 12Features 的成员）。
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_11_FEATURES
constexpr VkStructureType kSTVulkan11Features = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_11_FEATURES;
#else
constexpr VkStructureType kSTVulkan11Features = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
#endif
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_12_FEATURES
constexpr VkStructureType kSTVulkan12Features = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_12_FEATURES;
#else
constexpr VkStructureType kSTVulkan12Features = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
#endif

Device *g_dev = nullptr;
bool g_deviceLost = false;   // 单线程：SrEngine 的 QRecursiveMutex 已串行化所有会话
int g_lostTimes = 0;
bool g_deviceBroken = false; // 反复 DEVICE_LOST -> 进程内熔断，交给 CPU

// 设备级别入口的解析结果放在这里，避免在头文件里堆 40 个成员
struct Procs
{
    bool ok = false;
};

QByteArray LoadShaderBytes(const QString &name, QString *error)
{
    // 1) JM_VK_SHADER_DIR：开发期直接读磁盘（改 shader 不用重跑 cmake）
    const QString dir = qEnvironmentVariable("JM_VK_SHADER_DIR");
    if (!dir.isEmpty()) {
        QFile f(QDir(dir).filePath(name + QStringLiteral(".qsb")));
        if (f.open(QIODevice::ReadOnly))
            return f.readAll();
    }
    // 2) 构建期 qsb 产物打进 qrc
    QFile f(QStringLiteral(":/vkshader/%1.qsb").arg(name));
    if (!f.open(QIODevice::ReadOnly)) {
        if (error)
            *error = QStringLiteral("缺少着色器 %1.qsb（qsb 未生成或未打包）").arg(name);
        return {};
    }
    return f.readAll();
}

bool Check(VkResult r, const char *what, QString *error)
{
    if (r == VK_SUCCESS)
        return true;
    if (r == VK_ERROR_DEVICE_LOST) {
        // 设备丢失后所有 VkBuffer / VkPipeline / VkDeviceMemory 句柄都失效，
        // 继续用只会得到更多 DEVICE_LOST，必须让会话层走"重建 or 回退 CPU"。
        g_deviceLost = true;
        ++g_lostTimes;
        if (error)
            *error = QStringLiteral("%1 失败：VK_ERROR_DEVICE_LOST（驱动重置或 GPU 超时）")
                         .arg(QString::fromLatin1(what));
        return false;
    }
    if (error)
        *error = QStringLiteral("%1 失败：%2").arg(QString::fromLatin1(what), ResultText(r));
    return false;
}

}  // namespace

quint64 BudgetMbFromEnv()
{
    bool ok = false;
    const int mb = qEnvironmentVariableIntValue("JM_VK_VRAM_MB", &ok);
    return ok && mb > 0 ? quint64(mb) : 0;
}

Device &Device::I()
{
    if (!g_dev)
        g_dev = new Device();
    return *g_dev;
}

quint64 Device::BudgetBytes() const
{
    const quint64 env = BudgetMbFromEnv();
    if (env > 0)
        return env * 1024 * 1024;
    // 默认：设备本地堆的一部分，桌面 1/4（上限 1GB），移动端 1/8（上限 384MB）。
    // Android 的 DEVICE_LOCAL 堆上报的是整机共享 RAM（Adreno 830 实测 11GB），
    // 直接按 1/4 会算出 1GB 的 arena，一张 x4plus tile 就能触发 LMK / GPU watchdog。
#ifdef Q_OS_ANDROID
    const quint64 cap = 384ull * 1024 * 1024;
    quint64 byHeap = info_.vramBytes / 8;
#else
    const quint64 cap = 1024ull * 1024 * 1024;
    quint64 byHeap = info_.vramBytes / 4;
#endif
    if (byHeap == 0)
        byHeap = cap / 2;
    return qBound(128ull * 1024 * 1024, qMin(byHeap, cap), cap);
}

QString Device::summary() const
{
    return QStringLiteral("dev=%1 idx=%2 vramMB=%3 arenaMB=%4 budgetMB=%5 wgInvoc=%6 shmemKB=%7 "
                         "fp16=%8 pushDesc=%9")
        .arg(info_.name)
        .arg(deviceIndex_)
        .arg(info_.vramBytes / (1024 * 1024))
        .arg(arenaBytes_ / (1024 * 1024))
        .arg(budget_ / (1024 * 1024))
        .arg(info_.maxWorkGroupInvocations)
        .arg(info_.maxComputeSharedMemory / 1024)
        .arg(fp16Feature_ ? QStringLiteral("on") : QStringLiteral("off"))
        .arg(hasPushDesc_ ? QStringLiteral("on") : QStringLiteral("off"));
}

QString Device::deviceReport() const
{
    return QStringLiteral("vk: loader=%1 %2 stagingMB=%3 persistentMB=%4")
        .arg(Get().loaderPath, info_.summary())
        .arg(stagingBytes_ / (1024 * 1024))
        .arg(weightBump_ / (1024 * 1024));
}

bool Device::createInstance(QString *error)
{
    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "jmcomic-ui";
    app.applicationVersion = VK_MAKE_VERSION(0, 95, 0);
    app.pEngineName = "jmcomic-sr-vk";
    // 请求到 1.2：vkGetPhysicalDeviceFeatures2（1.1 core）是能力探测的前提，
    // loader 按 instance apiVersion 门控入口解析（1.0 的 instance 拿不到它）。
    // 真实能力仍以物理设备为准（pickDevice 里再查）。
    quint32 maxApi = VK_API_VERSION_1_0;
    if (Get().EnumerateInstanceVersion)
        Get().EnumerateInstanceVersion(&maxApi);
    const quint32 want = VK_MAKE_VERSION(1, 2, 0);
    app.apiVersion = maxApi < want ? maxApi : want;

    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    // 不开校验层：发布包里没有 layer，开着只会少一条可用路径
    if (!Check(Get().CreateInstance(&ci, nullptr, &instance_), "vkCreateInstance", error))
        return false;
    // 实例建好后才能解析其余 instance 级入口（见 vk_api.cpp 的注释）
    if (!ResolveInstance(instance_)) {
        if (error)
            *error = Get().error;
        return false;
    }
    return true;
}

bool Device::pickDevice(QString *error, int preferIndex)
{
    // 设备枚举需要 instance：先建一个临时 instance，选好设备后再用同一个 instance 建 device
    quint32 count = 0;
    if (!Check(Get().EnumeratePhysicalDevices(instance_, &count, nullptr), "vkEnumeratePhysicalDevices",
               error))
        return false;
    if (count == 0) {
        if (error)
            *error = QStringLiteral("没有可用 Vulkan 物理设备");
        return false;
    }
    QVector<VkPhysicalDevice> devs(static_cast<int>(count));
    Get().EnumeratePhysicalDevices(instance_, &count, devs.data());

    // 收集能力并打分：独显 > 集显 > 其它；同档次按显存降序
    int best = -1;
    int bestScore = -1;
    for (quint32 i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p{};
        Get().GetPhysicalDeviceProperties(devs[int(i)], &p);
        int score = 0;
        switch (p.deviceType) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
            score = 300;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
            score = 200;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
            score = 150;
            break;
        case VK_PHYSICAL_DEVICE_TYPE_CPU:
            score = 50;   // lavapipe/swiftshader：可用但慢，作为最后手段
            break;
        default:
            score = 100;
            break;
        }
        if (preferIndex >= 0)
            score = (int(i) == preferIndex) ? 1000 : 0;
        if (score > bestScore) {
            bestScore = score;
            best = int(i);
        }
    }
    if (best < 0) {
        if (error)
            *error = QStringLiteral("没有可用的计算设备");
        return false;
    }
    phys_ = devs[best];
    deviceIndex_ = best;

    VkPhysicalDeviceProperties p{};
    Get().GetPhysicalDeviceProperties(phys_, &p);
    info_.name = QString::fromUtf8(p.deviceName);
    info_.vendorId = p.vendorID;
    info_.deviceId = p.deviceID;
    info_.type = p.deviceType;
    info_.apiVersion = p.apiVersion;
    info_.driverVersion = p.driverVersion;
    info_.maxWorkGroupInvocations = p.limits.maxComputeWorkGroupInvocations;
    info_.maxWorkGroupSize[0] = p.limits.maxComputeWorkGroupSize[0];
    info_.maxWorkGroupSize[1] = p.limits.maxComputeWorkGroupSize[1];
    info_.maxWorkGroupSize[2] = p.limits.maxComputeWorkGroupSize[2];
    info_.maxStorageBufferRange = p.limits.maxStorageBufferRange;
    info_.maxComputeSharedMemory = p.limits.maxComputeSharedMemorySize;

    VkPhysicalDeviceMemoryProperties mp{};
    Get().GetPhysicalDeviceMemoryProperties(phys_, &mp);
    for (quint32 h = 0; h < mp.memoryHeapCount; ++h) {
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            info_.vramBytes = qMax(info_.vramBytes, quint64(mp.memoryHeaps[h].size));
    }

    // ---- 能力探测：fp16（shaderFloat16 + 16bit storage）与 push descriptor ----
    // 只认 Vulkan 1.2+ 的物理设备：两条能力都是 1.2 core 特性，1.1 及以下
    // 一律关闭（桌面 NVIDIA/AMD/Intel 与 Adreno 6xx+ 全部 1.2+，覆盖现实目标；
    // 省掉一整套 1.1 + KHR 扩展的 feature 链探测与启用路径）。
    fp16Feature_ = false;
    hasPushDesc_ = false;
    info_.has16BitStorage = false;
    info_.has8BitStorage = false;
    info_.hasShaderFloat16 = false;
    info_.hasPushDescriptor = false;
    if (VK_VERSION_MAJOR(p.apiVersion) > 1 || VK_VERSION_MINOR(p.apiVersion) >= 2) {
        VkPhysicalDeviceVulkan12Features f12{};
        f12.sType = kSTVulkan12Features;
        VkPhysicalDeviceVulkan11Features f11{};
        f11.sType = kSTVulkan11Features;
        f12.pNext = &f11;
        VkPhysicalDeviceFeatures2 f2{};
        f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        f2.pNext = &f12;
        if (Get().GetPhysicalDeviceFeatures2) {
            Get().GetPhysicalDeviceFeatures2(phys_, &f2);
            info_.hasShaderFloat16 = f12.shaderFloat16 != VK_FALSE;
            // storageBuffer16BitAccess 是 Vulkan11Features 的成员（1.2 里由 11
            // features 结构表达）；8bit storage 在 12 features。桌面旧头与 NDK
            // 新头都遵守这个归属。
            info_.has16BitStorage = f11.storageBuffer16BitAccess != VK_FALSE;
            info_.has8BitStorage = f12.storageBuffer8BitAccess != VK_FALSE;
            fp16Feature_ = info_.hasShaderFloat16 && info_.has16BitStorage;
        }
    }
    if (Get().EnumerateDeviceExtensionProperties) {
        quint32 extCount = 0;
        Get().EnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, nullptr);
        if (extCount > 0) {
            QVector<VkExtensionProperties> exts(static_cast<int>(extCount));
            Get().EnumerateDeviceExtensionProperties(phys_, nullptr, &extCount, exts.data());
            for (const VkExtensionProperties &e : exts) {
                if (std::strcmp(e.extensionName, "VK_KHR_push_descriptor") == 0)
                    hasPushDesc_ = true;
            }
        }
    }
    info_.hasPushDescriptor = hasPushDesc_;
    return true;
}

bool Device::createDevice(QString *error)
{
    quint32 famCount = 0;
    Get().GetPhysicalDeviceQueueFamilyProperties(phys_, &famCount, nullptr);
    QVector<VkQueueFamilyProperties> fams(static_cast<int>(famCount));
    Get().GetPhysicalDeviceQueueFamilyProperties(phys_, &famCount, fams.data());
    int chosen = -1;
    for (quint32 i = 0; i < famCount; ++i) {
        if (fams[int(i)].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            chosen = int(i);
            break;
        }
    }
    if (chosen < 0) {
        if (error)
            *error = QStringLiteral("设备没有计算队列（VK_QUEUE_COMPUTE_BIT）");
        return false;
    }
    queueFamily_ = quint32(chosen);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    // push descriptor：扩展在 pickDevice 里已确认存在，这里只负责启用
    const char *kPushDescExt = "VK_KHR_push_descriptor";
    if (hasPushDesc_) {
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &kPushDescExt;
    }
    // fp16：shaderFloat16（12 features）与 storageBuffer16BitAccess（11 features）
    // 都要显式启用才能在 shader 里用 float16_t/SSBO half 数组
    VkPhysicalDeviceVulkan12Features f12{};
    VkPhysicalDeviceVulkan11Features f11{};
    if (fp16Feature_) {
        f12.sType = kSTVulkan12Features;
        f12.shaderFloat16 = VK_TRUE;
        f11.sType = kSTVulkan11Features;
        f11.storageBuffer16BitAccess = VK_TRUE;
        f12.pNext = &f11;
        dci.pNext = &f12;
    }
    // 不要求任何其它特性/扩展：kernel 只用 storage buffer + push constant
    if (!Check(Get().CreateDevice(phys_, &dci, nullptr, &device_), "vkCreateDevice", error))
        return false;

    // device 级入口统一用 vkGetDeviceProcAddr 再解析一遍
    auto gdpa = Get().GetDeviceProcAddr;
    if (!gdpa) {
        if (error)
            *error = QStringLiteral("loader 未提供 vkGetDeviceProcAddr");
        return false;
    }
    auto R = [&](auto &slot, const char *name) {
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(gdpa(device_, name));
        return slot != nullptr;
    };
    const bool ok = R(Get().DestroyDevice, "vkDestroyDevice") && R(Get().DeviceWaitIdle, "vkDeviceWaitIdle")
                    && R(Get().GetDeviceQueue, "vkGetDeviceQueue")
                    && R(Get().CreateBuffer, "vkCreateBuffer") && R(Get().DestroyBuffer, "vkDestroyBuffer")
                    && R(Get().GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements")
                    && R(Get().AllocateMemory, "vkAllocateMemory") && R(Get().FreeMemory, "vkFreeMemory")
                    && R(Get().BindBufferMemory, "vkBindBufferMemory") && R(Get().MapMemory, "vkMapMemory")
                    && R(Get().UnmapMemory, "vkUnmapMemory")
                    && R(Get().CreateShaderModule, "vkCreateShaderModule")
                    && R(Get().DestroyShaderModule, "vkDestroyShaderModule")
                    && R(Get().CreateDescriptorSetLayout, "vkCreateDescriptorSetLayout")
                    && R(Get().DestroyDescriptorSetLayout, "vkDestroyDescriptorSetLayout")
                    && R(Get().CreatePipelineLayout, "vkCreatePipelineLayout")
                    && R(Get().DestroyPipelineLayout, "vkDestroyPipelineLayout")
                    && R(Get().CreateComputePipelines, "vkCreateComputePipelines")
                    && R(Get().DestroyPipeline, "vkDestroyPipeline")
                    && R(Get().CreateDescriptorPool, "vkCreateDescriptorPool")
                    && R(Get().DestroyDescriptorPool, "vkDestroyDescriptorPool")
                    && R(Get().AllocateDescriptorSets, "vkAllocateDescriptorSets")
                    && R(Get().ResetDescriptorPool, "vkResetDescriptorPool")
                    && R(Get().UpdateDescriptorSets, "vkUpdateDescriptorSets")
                    && R(Get().CreateCommandPool, "vkCreateCommandPool")
                    && R(Get().DestroyCommandPool, "vkDestroyCommandPool")
                    && R(Get().AllocateCommandBuffers, "vkAllocateCommandBuffers")
                    && R(Get().FreeCommandBuffers, "vkFreeCommandBuffers")
                    && R(Get().BeginCommandBuffer, "vkBeginCommandBuffer")
                    && R(Get().EndCommandBuffer, "vkEndCommandBuffer")
                    && R(Get().CmdBindPipeline, "vkCmdBindPipeline")
                    && R(Get().CmdBindDescriptorSets, "vkCmdBindDescriptorSets")
                    && R(Get().CmdDispatch, "vkCmdDispatch")
                    && R(Get().CmdPushConstants, "vkCmdPushConstants")
                    && R(Get().CmdPipelineBarrier, "vkCmdPipelineBarrier")
                    && R(Get().CmdCopyBuffer, "vkCmdCopyBuffer")
                    && R(Get().CmdFillBuffer, "vkCmdFillBuffer")
                    && R(Get().CreateFence, "vkCreateFence") && R(Get().DestroyFence, "vkDestroyFence")
                    && R(Get().WaitForFences, "vkWaitForFences")
                    && R(Get().ResetFences, "vkResetFences") && R(Get().QueueSubmit, "vkQueueSubmit")
                    && R(Get().QueueWaitIdle, "vkQueueWaitIdle");
    if (!ok) {
        if (error)
            *error = QStringLiteral("设备级入口解析不完整（Vulkan 1.0 必需函数缺失）");
        return false;
    }
    // push descriptor 的命令入口（可选）：解析不到就当扩展不可用，回退轮转池
    if (hasPushDesc_) {
        Get().CmdPushDescriptorSetKHR = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            gdpa(device_, "vkCmdPushDescriptorSetKHR"));
        if (!Get().CmdPushDescriptorSetKHR) {
            qWarning("vk: VK_KHR_push_descriptor advertised but vkCmdPushDescriptorSetKHR missing");
            hasPushDesc_ = false;
        }
    }
    // 队列必须在 device 级入口解析之后取：vkGetDeviceQueue 是 device 级函数，
    // 提前调用就是"函数指针还是 null"（第一次实测直接段错误）。
    Get().GetDeviceQueue(device_, queueFamily_, 0, &queue_);
    if (queue_ == VK_NULL_HANDLE) {
        if (error)
            *error = QStringLiteral("vkGetDeviceQueue 未返回计算队列");
        return false;
    }

    auto makeSetLayout = [&](VkDescriptorSetLayoutCreateFlags flags, VkDescriptorSetLayout *out) {
        VkDescriptorSetLayoutBinding b[kMaxBindings]{};
        for (quint32 i = 0; i < kMaxBindings; ++i) {
            b[i].binding = i;
            b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo sl{};
        sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sl.flags = flags;
        sl.bindingCount = kMaxBindings;
        sl.pBindings = b;
        return Check(Get().CreateDescriptorSetLayout(device_, &sl, nullptr, out),
                     "vkCreateDescriptorSetLayout", error);
    };
    if (!makeSetLayout(0, &setLayout_))
        return false;
    // push descriptor 版布局：dispatch 时 set 内容随命令推入，无需 allocate/bind
    if (hasPushDesc_ && !makeSetLayout(VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                       &setLayoutPush_))
        return false;

    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pc.offset = 0;
    pc.size = sizeof(Params);
    auto makePipeLayout = [&](VkDescriptorSetLayout layout, VkPipelineLayout *out) {
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &layout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pc;
        return Check(Get().CreatePipelineLayout(device_, &pl, nullptr, out),
                     "vkCreatePipelineLayout", error);
    };
    if (!makePipeLayout(setLayout_, &pipeLayout_))
        return false;
    if (hasPushDesc_ && !makePipeLayout(setLayoutPush_, &pipeLayoutPush_))
        return false;

    VkCommandPoolCreateInfo cp{};
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    // 每个命令缓冲独立 reset（轮转复用前 ResetFences + Begin，不需要重置整个池）
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = queueFamily_;
    if (!Check(Get().CreateCommandPool(device_, &cp, nullptr, &cmdPool_), "vkCreateCommandPool",
               error))
        return false;
    {
        // 双份轮转：第 k 个 pass 用 cmds_[k%2]/fences_[k%2]，覆盖前等上一轮完成
        VkCommandBufferAllocateInfo cb{};
        cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cb.commandPool = cmdPool_;
        cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cb.commandBufferCount = 2;
        if (!Check(Get().AllocateCommandBuffers(device_, &cb, cmds_), "vkAllocateCommandBuffers",
                   error))
            return false;
    }
    {
        // SIGNALED 初始：首次 beginPass 的"slot 复用等待"必须立即通过
        // （从未提交过的 fence 处于未触发态，等它会死锁——第一次实测挂在 --vk-conv-test）
        VkFenceCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (int k = 0; k < 2; ++k) {
            if (!Check(Get().CreateFence(device_, &fci, nullptr, &fences_[k]), "vkCreateFence",
                       error))
                return false;
        }
        // 独立的 xfer fence：upload/download 小提交的同步与 pass 轮转分开
        if (!Check(Get().CreateFence(device_, &fci, nullptr, &xferFence_), "vkCreateFence(xfer)",
                   error))
            return false;
    }

    if (hasPushDesc_) {
        // push descriptor 模式：不需要 descriptor pool，也不需要轮转 set
        return true;
    }
    VkDescriptorPoolSize ps{};
    // 轮转池：一次 allocate 预取全部 set，dispatch 直接取用（省掉每 dispatch 的
    // vkAllocateDescriptorSets）。x4plus 单 pass ~1500 个 set，给 8192 留余量。
    // pass 全部同步完成后 set 都空闲，回卷无需显式 reset。
    // 预取失败也不致命：dispatch 回退到"每 dispatch allocate"的旧路径。
    int ringSets = 8192;
    VkResult poolR = VK_ERROR_INITIALIZATION_FAILED;
    for (const int attempt : {8192, 2048}) {
        ringSets = attempt;
        ps.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ps.descriptorCount = quint32(ringSets) * kMaxBindings;
        VkDescriptorPoolCreateInfo dpi{};
        dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpi.maxSets = ringSets;
        dpi.poolSizeCount = 1;
        dpi.pPoolSizes = &ps;
        poolR = Get().CreateDescriptorPool(device_, &dpi, nullptr, &descPool_);
        if (poolR == VK_SUCCESS)
            break;
        descPool_ = VK_NULL_HANDLE;
    }
    if (!Check(poolR, "vkCreateDescriptorPool", error))
        return false;
    {
        QVector<VkDescriptorSetLayout> layouts(ringSets, setLayout_);
        setRing_.resize(ringSets);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = descPool_;
        ai.descriptorSetCount = ringSets;
        ai.pSetLayouts = layouts.constData();
        if (Get().AllocateDescriptorSets(device_, &ai, setRing_.data()) != VK_SUCCESS) {
            // 预取失败（老驱动对一次分配上千 set 有上限）：清空走旧路径
            setRing_.clear();
        }
    }
    return true;
}

bool Device::createArena(QString *error)
{
    budget_ = BudgetBytes();
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = budget_;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    // 单次绑定不能超过 maxStorageBufferRange
    if (budget_ > info_.maxStorageBufferRange)
        budget_ = info_.maxStorageBufferRange;
    bi.size = budget_;
    if (!Check(Get().CreateBuffer(device_, &bi, nullptr, &arena_), "vkCreateBuffer(arena)", error))
        return false;

    VkMemoryRequirements req{};
    Get().GetBufferMemoryRequirements(device_, arena_, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    Get().GetPhysicalDeviceMemoryProperties(phys_, &mp);
    auto findType = [&mp](quint32 bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags avoid) {
        for (quint32 i = 0; i < mp.memoryTypeCount; ++i) {
            if (!(bits & (1u << i)))
                continue;
            const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            if ((f & want) == want && (f & avoid) == 0)
                return int(i);
        }
        return -1;
    };
    int typeIndex = findType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    if (typeIndex < 0)
        typeIndex = findType(req.memoryTypeBits, 0, 0);
    if (typeIndex < 0) {
        if (error)
            *error = QStringLiteral("找不到可用的显存类型");
        return false;
    }
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = quint32(typeIndex);
    if (!Check(Get().AllocateMemory(device_, &ai, nullptr, &arenaMem_), "vkAllocateMemory(arena)",
               error)) {
        // 一次申请整块可能被拒：减半重试（移动端堆更碎片化）
        for (quint64 mb : {quint64(512), quint64(384), quint64(256), quint64(128)}) {
            if (mb * 1024 * 1024 >= budget_)
                continue;
            Get().DestroyBuffer(device_, arena_, nullptr);
            budget_ = mb * 1024 * 1024;
            bi.size = budget_;
            Get().CreateBuffer(device_, &bi, nullptr, &arena_);
            Get().GetBufferMemoryRequirements(device_, arena_, &req);
            ai.allocationSize = req.size;
            if (Get().AllocateMemory(device_, &ai, nullptr, &arenaMem_) == VK_SUCCESS)
                break;
        }
        if (arenaMem_ == VK_NULL_HANDLE)
            return false;
    }
    if (!Check(Get().BindBufferMemory(device_, arena_, arenaMem_, 0), "vkBindBufferMemory(arena)",
               error))
        return false;
    arenaBytes_ = budget_;
    weightLimit_ = budget_ / 2;   // 权重区上限一半，激活值用另一半
    weightBump_ = 0;
    actBump_ = weightLimit_;

    // staging：host visible + coherent，默认 64MB（一次 tile 的输入/输出远小于此）
    quint64 stagingBytes = 64ull * 1024 * 1024;
    const int envMb = qEnvironmentVariableIntValue("JM_VK_STAGING_MB");
    if (envMb > 0)
        stagingBytes = quint64(envMb) * 1024 * 1024;
    stagingBytes = qMin(stagingBytes, budget_ / 4);
    VkBufferCreateInfo sbi{};
    sbi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    sbi.size = stagingBytes;
    sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!Check(Get().CreateBuffer(device_, &sbi, nullptr, &staging_), "vkCreateBuffer(staging)",
               error))
        return false;
    VkMemoryRequirements sreq{};
    Get().GetBufferMemoryRequirements(device_, staging_, &sreq);
    int hostType = findType(sreq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                                    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    if (hostType < 0) {
        if (error)
            *error = QStringLiteral("找不到 host-visible 内存类型");
        return false;
    }
    VkMemoryAllocateInfo sai{};
    sai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    sai.allocationSize = sreq.size;
    sai.memoryTypeIndex = quint32(hostType);
    if (!Check(Get().AllocateMemory(device_, &sai, nullptr, &stagingMem_), "vkAllocateMemory(staging)",
               error))
        return false;
    if (!Check(Get().BindBufferMemory(device_, staging_, stagingMem_, 0), "vkBindBufferMemory(staging)",
               error))
        return false;
    if (!Check(Get().MapMemory(device_, stagingMem_, 0, VK_WHOLE_SIZE, 0, &stagingMap_), "vkMapMemory",
               error))
        return false;
    stagingBytes_ = stagingBytes;
    return true;
}

bool Device::Ensure(QString *error, int preferIndex)
{
    if (valid())
        return true;
    if (!Load()) {
        if (error)
            *error = Get().error;
        return false;
    }
    if (!createInstance(error))
        return false;
    if (!pickDevice(error, preferIndex))
        return false;
    if (!createDevice(error))
        return false;
    if (!createArena(error))
        return false;
    qInfo("vk: %s", qPrintable(summary()));
    return true;
}

void Device::Shutdown()
{
    if (!valid())
        return;
    // DEVICE_LOST 之后 DeviceWaitIdle 不再可靠（驱动可能永久不返回），直接销毁。
    if (!g_deviceLost)
        Get().DeviceWaitIdle(device_);
    for (VkPipeline p : pipelines_)
        Get().DestroyPipeline(device_, p, nullptr);
    pipelines_.clear();
    if (stagingMap_)
        Get().UnmapMemory(device_, stagingMem_);
    if (staging_)
        Get().DestroyBuffer(device_, staging_, nullptr);
    if (stagingMem_)
        Get().FreeMemory(device_, stagingMem_, nullptr);
    if (arena_)
        Get().DestroyBuffer(device_, arena_, nullptr);
    if (arenaMem_)
        Get().FreeMemory(device_, arenaMem_, nullptr);
    if (descPool_)
        Get().DestroyDescriptorPool(device_, descPool_, nullptr);
    if (pipeLayoutPush_)
        Get().DestroyPipelineLayout(device_, pipeLayoutPush_, nullptr);
    if (pipeLayout_)
        Get().DestroyPipelineLayout(device_, pipeLayout_, nullptr);
    if (setLayoutPush_)
        Get().DestroyDescriptorSetLayout(device_, setLayoutPush_, nullptr);
    if (setLayout_)
        Get().DestroyDescriptorSetLayout(device_, setLayout_, nullptr);
    for (int k = 0; k < 2; ++k) {
        if (fences_[k])
            Get().DestroyFence(device_, fences_[k], nullptr);
    }
    if (xferFence_)
        Get().DestroyFence(device_, xferFence_, nullptr);
    if (cmdPool_)
        Get().DestroyCommandPool(device_, cmdPool_, nullptr);
    if (device_)
        Get().DestroyDevice(device_, nullptr);
    if (instance_ && Get().DestroyInstance)
        Get().DestroyInstance(instance_, nullptr);
    device_ = VK_NULL_HANDLE;
    instance_ = VK_NULL_HANDLE;
    staging_ = VK_NULL_HANDLE;
    stagingMem_ = VK_NULL_HANDLE;
    stagingMap_ = nullptr;
    arena_ = VK_NULL_HANDLE;
    arenaMem_ = VK_NULL_HANDLE;
    // 这些句柄原来没清：Recover 重建时会覆盖指针，但残留值会让"重建后是否
    // 拿到新对象"无法从日志判断，也容易在后续检查里误判。全部归零。
    phys_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    for (int k = 0; k < 2; ++k) {
        cmds_[k] = VK_NULL_HANDLE;
        fences_[k] = VK_NULL_HANDLE;
    }
    xferFence_ = VK_NULL_HANDLE;
    cmdPool_ = VK_NULL_HANDLE;
    descPool_ = VK_NULL_HANDLE;
    setLayout_ = VK_NULL_HANDLE;
    setLayoutPush_ = VK_NULL_HANDLE;
    pipeLayout_ = VK_NULL_HANDLE;
    pipeLayoutPush_ = VK_NULL_HANDLE;
    setRing_.clear();
    setRingIdx_ = 0;
    arenaBytes_ = budget_ = 0;
    weightBump_ = actBump_ = weightLimit_ = 0;
    stagingBytes_ = stagingBump_ = 0;
    passOpen_ = false;
    passSeq_ = 0;
    boundPipe_ = VK_NULL_HANDLE;
}

bool Device::fp16Usable() const
{
    if (!fp16Feature_)
        return false;
    // JM_VK_FP16=0 显式关闭；未设置或其它值按硬件能力走
    const int v = qEnvironmentVariableIntValue("JM_VK_FP16");
    return v != 0 || !qEnvironmentVariableIsSet("JM_VK_FP16");
}

bool Device::lost() const
{
    return g_deviceLost;
}

bool Device::broken() const
{
    return g_deviceBroken;
}

bool Device::Recover(QString *error)
{
    if (!g_deviceLost)
        return true;
    // 移动 GPU 的 watchdog 往往连续触发：重建一次后立刻又丢，再重建只会
    // 反复重置驱动。第二次起直接熔断，让上层回退 ORT CPU（真机 Adreno 830
    // 连跑 4x 模型时实测）。
    if (g_lostTimes >= 2) {
        g_deviceBroken = true;
        qWarning("vk: repeated DEVICE_LOST (%d times), fusing Vulkan off for this process",
                 g_lostTimes);
        if (error)
            *error = QStringLiteral("Vulkan 设备反复丢失（%1 次），已熔断并回退 CPU")
                         .arg(g_lostTimes);
        return false;
    }
    qWarning("vk: DEVICE_LOST detected, rebuilding instance/device/arena");
    Shutdown();
    g_deviceLost = false;
    lastError_.clear();
    // 保留 deviceIndex_：桌面双显卡时重建必须落在同一块 GPU 上，
    // 否则上层仍按旧设备的显存预算在规划 tile。
    if (!Ensure(error, deviceIndex_)) {
        g_deviceLost = true;
        return false;
    }
    ++generation_;
    qInfo("vk: device recovered, generation=%d dev=%s", generation_, qPrintable(summary()));
    return true;
}

BufferRef Device::allocPersistent(quint64 bytes, quint64 alignment)
{
    BufferRef r;
    if (!arena_ || bytes == 0)
        return r;
    const quint64 off = (weightBump_ + alignment - 1) / alignment * alignment;
    if (off + bytes > weightLimit_) {
        lastError_ = QStringLiteral("权重区不足：需要 %1MB，已用 %2MB / 上限 %3MB")
                         .arg(bytes / (1024 * 1024))
                         .arg(weightBump_ / (1024 * 1024))
                         .arg(weightLimit_ / (1024 * 1024));
        return r;
    }
    weightBump_ = off + bytes;
    r.buffer = arena_;
    r.offset = off;
    r.size = bytes;
    return r;
}

BufferRef Device::alloc(quint64 bytes, quint64 alignment)
{
    BufferRef r;
    if (!arena_ || bytes == 0)
        return r;
    const quint64 off = (actBump_ + alignment - 1) / alignment * alignment;
    if (off + bytes > arenaBytes_) {
        lastError_ = QStringLiteral("激活区不足：需要 %1MB，剩余 %2MB（可用 JM_VK_VRAM_MB 调大预算）")
                         .arg(bytes / (1024 * 1024))
                         .arg((arenaBytes_ - actBump_) / (1024 * 1024));
        return r;
    }
    actBump_ = off + bytes;
    r.buffer = arena_;
    r.offset = off;
    r.size = bytes;
    return r;
}

void Device::resetStaging()
{
    stagingBump_ = 0;
}

void Device::resetWeights()
{
    weightBump_ = 0;
    actBump_ = weightLimit_;
    stagingBump_ = 0;
    lastError_.clear();
}

void Device::setWeightReserve(quint64 bytes)
{
    constexpr quint64 kBoundary = 16ull * 1024 * 1024;
    const quint64 need = ((bytes + kBoundary) / kBoundary) * kBoundary;
    // 不超过预算一半：显存小的设备宁可让大模型回退 CPU，也不能把激活区压没
    if (need == 0 || need > arenaBytes_ / 2)
        return;
    weightLimit_ = need;
    weightBump_ = 0;
    actBump_ = weightLimit_;
}

void Device::resetActivations()
{
    actBump_ = weightLimit_;
    stagingBump_ = 0;
    lastError_.clear();
}

bool Device::upload(const BufferRef &dst, const void *src, quint64 bytes)
{
    if (!dst.valid() || bytes == 0 || bytes > dst.size) {
        lastError_ = QStringLiteral("upload 参数非法（bytes=%1 dst=%2）").arg(bytes).arg(dst.size);
        return false;
    }
    if (bytes > stagingBytes_) {
        lastError_ = QStringLiteral("单次上传超过 staging 大小（%1MB）").arg(stagingBytes_ / (1024 * 1024));
        return false;
    }
    // staging 是环形的：一次 pass 里可能连续上传多块（权重/输入），
    // 全部从 offset 0 拷贝会让后一次 memcpy 覆盖前一次的内容（实测 maxErr=96 的元凶）。
    const quint64 aligned = (stagingBump_ + 255) / 256 * 256;
    if (aligned + bytes > stagingBytes_) {
        lastError_ = QStringLiteral("staging 环用尽（已用 %1MB / 共 %2MB）")
                         .arg(aligned / (1024 * 1024))
                         .arg(stagingBytes_ / (1024 * 1024));
        return false;
    }
    stagingBump_ = aligned + bytes;
    std::memcpy(static_cast<char *>(stagingMap_) + aligned, src, size_t(bytes));

    VkBufferCopy region{};
    region.srcOffset = aligned;
    region.dstOffset = dst.offset;
    region.size = bytes;
    if (passOpen_) {
        Get().CmdCopyBuffer(cmds_[passSeq_ % 2], staging_, dst.buffer, 1, &region);
        VkMemoryBarrier mb{};
        mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        Get().CmdPipelineBarrier(cmds_[passSeq_ % 2], VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0,
                                 nullptr);
        return true;
    }
    // 无 pass：独立小提交（xfer fence 与 pass 轮转分开）
    VkCommandBufferAllocateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = cmdPool_;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    VkCommandBuffer tmp = VK_NULL_HANDLE;
    if (!Check(Get().AllocateCommandBuffers(device_, &cb, &tmp), "vkAllocateCommandBuffers",
               &lastError_))
        return false;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Get().BeginCommandBuffer(tmp, &bi);
    Get().CmdCopyBuffer(tmp, staging_, dst.buffer, 1, &region);
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    Get().CmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    Get().EndCommandBuffer(tmp);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &tmp;
    Get().ResetFences(device_, 1, &xferFence_);
    const VkResult r = Get().QueueSubmit(queue_, 1, &si, xferFence_);
    if (r == VK_SUCCESS)
        Get().WaitForFences(device_, 1, &xferFence_, VK_TRUE, UINT64_MAX);
    Get().FreeCommandBuffers(device_, cmdPool_, 1, &tmp);
    if (!Check(r, "vkQueueSubmit(upload)", &lastError_))
        return false;
    return true;
}

bool Device::beginPass()
{
    const int slot = int(passSeq_ % 2);
    // slot 复用安全等待：上一轮（passSeq_-2）的 pass 必须已经完成，才能
    // 1) 重录命令缓冲（驱动要求在飞的 buffer 不能 Begin）；
    // 2) 覆盖该 slot 引用的输入/输出设备缓冲。
    // 从未提交过的 fence 是 SIGNALED 初始态，等待立即返回。
    if (passSeq_ >= 2
        && !Check(Get().WaitForFences(device_, 1, &fences_[slot], VK_TRUE, UINT64_MAX),
                  "vkWaitForFences(slot reuse)", &lastError_))
        return false;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (!Check(Get().BeginCommandBuffer(cmds_[slot], &bi), "vkBeginCommandBuffer", &lastError_))
        return false;
    // descriptor 策略：
    //   * push descriptor：无 pool，命令里直接推；
    //   * 轮转池（setRing_ 非空）：一次 allocate 预取，pool 永不 reset（pass
    //     同步完成后 set 全部空闲，回卷即复用）；
    //   * 回退（池预取失败）：每 dispatch allocate，所以每 pass 开头 reset。
    if (descPool_ != VK_NULL_HANDLE && setRing_.isEmpty())
        Get().ResetDescriptorPool(device_, descPool_, 0);
    // 命令缓冲重新录制后，旧的管线绑定状态不再有效（Vulkan 规范：
    // vkBeginCommandBuffer 之后 pipeline bind state 为 undefined）
    boundPipe_ = VK_NULL_HANDLE;
    passOpen_ = true;
    return true;
}

bool Device::ensurePipeline(const char *shader, VkPipeline *out, QString *error)
{
    const QString key = QString::fromLatin1(shader);
    auto it = pipelines_.constFind(key);
    if (it != pipelines_.constEnd()) {
        *out = it.value();
        return true;
    }
    const QByteArray blob = LoadShaderBytes(key, error);
    if (blob.isEmpty())
        return false;
    const QShader qs = QShader::fromSerialized(blob);
    if (!qs.isValid()) {
        if (error)
            *error = QStringLiteral("%1.qsb 无法解析（qsb 版本与 Qt 不匹配）").arg(key);
        return false;
    }
    const QShaderCode code = qs.shader({QShader::SpirvShader, 100});
    if (code.shader().isEmpty()) {
        if (error)
            *error = QStringLiteral("%1.qsb 内没有 SPIR-V（qsb 需用 --glsl 450 生成）").arg(key);
        return false;
    }
    VkShaderModuleCreateInfo sm{};
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = size_t(code.shader().size());
    sm.pCode = reinterpret_cast<const quint32 *>(code.shader().constData());
    VkShaderModule mod = VK_NULL_HANDLE;
    if (!Check(Get().CreateShaderModule(device_, &sm, nullptr, &mod), "vkCreateShaderModule", error))
        return false;

    const QByteArray entry = code.entryPoint().isEmpty() ? QByteArrayLiteral("main")
                                                         : code.entryPoint();
    VkComputePipelineCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = entry.constData();
    // push descriptor 与轮转池的 set 布局不同，pipeline layout 必须与
    // dispatch 时使用的布局一致（验证层强校验）
    ci.layout = hasPushDesc_ ? pipeLayoutPush_ : pipeLayout_;
    VkPipeline pipe = VK_NULL_HANDLE;
    const VkResult r = Get().CreateComputePipelines(device_, VK_NULL_HANDLE, 1, &ci, nullptr, &pipe);
    Get().DestroyShaderModule(device_, mod, nullptr);
    if (!Check(r, "vkCreateComputePipelines", error))
        return false;
    pipelines_.insert(key, pipe);
    *out = pipe;
    return true;
}

bool Device::dispatch(const char *shader, const Params &pc, const BufferRef *buffers, int count,
                      quint32 groupsX, quint32 groupsY, quint32 groupsZ)
{
    if (!passOpen_) {
        lastError_ = QStringLiteral("dispatch 必须在 beginPass/endPass 之间");
        return false;
    }
    if (count > int(kMaxBindings)) {
        lastError_ = QStringLiteral("绑定数超过 %1").arg(kMaxBindings);
        return false;
    }
    VkCommandBuffer cmd = cmds_[passSeq_ % 2];
    VkPipeline pipe = VK_NULL_HANDLE;
    QString err;
    if (!ensurePipeline(shader, &pipe, &err)) {
        lastError_ = err;
        return false;
    }
    // 少于 8 个绑定时用最后一个有效绑定补齐：SPIR-V 里声明的 binding 必须都有描述符
    VkDescriptorBufferInfo infos[kMaxBindings];
    for (quint32 i = 0; i < kMaxBindings; ++i) {
        const BufferRef &b = buffers[qMin<int>(i, qMax(0, count - 1))];
        infos[int(i)].buffer = b.buffer;
        infos[int(i)].offset = b.offset;
        infos[int(i)].range = b.size;
    }
    VkWriteDescriptorSet w[kMaxBindings]{};
    for (quint32 i = 0; i < kMaxBindings; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &infos[int(i)];
    }
    // 管线绑定去重：连续的同名 kernel（一串 conv）不再重复 CmdBindPipeline。
    // 命令缓冲在 beginPass 重置后绑定失效，所以每 pass 开头清一次。
    if (pipe != boundPipe_) {
        Get().CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        boundPipe_ = pipe;
    }
    VkPipelineLayout layout = pipeLayout_;
    if (hasPushDesc_ && Get().CmdPushDescriptorSetKHR) {
        // 快速路径：descriptor 内容直接进命令流，零 allocate / 零 set bind
        layout = pipeLayoutPush_;
        Get().CmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayoutPush_,
                                      0, kMaxBindings, w);
    } else {
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (!setRing_.isEmpty()) {
            // 轮转池：pass 同步完成后 set 全部空闲，回卷安全（queue FIFO 语义）
            set = setRing_[setRingIdx_];
            setRingIdx_ = (setRingIdx_ + 1) % setRing_.size();
        } else {
            VkDescriptorSetAllocateInfo ai{};
            ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ai.descriptorPool = descPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &setLayout_;
            if (!Check(Get().AllocateDescriptorSets(device_, &ai, &set),
                       "vkAllocateDescriptorSets", &lastError_))
                return false;
        }
        for (quint32 i = 0; i < kMaxBindings; ++i)
            w[i].dstSet = set;
        Get().UpdateDescriptorSets(device_, kMaxBindings, w, 0, nullptr);
        Get().CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1, &set,
                                    0, nullptr);
    }
    Get().CmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Params), &pc);
    Get().CmdDispatch(cmd, groupsX, groupsY, groupsZ);
    // 每个 kernel 之间插一个 barrier：下一层要读上一层写的数据。
    // 用 buffer barrier 并把范围钉在"本 dispatch 写的输出区间"，而不是
    // 全局 memory barrier——x4plus 一个 tile 有 1714 次 dispatch，全局
    // barrier 会让驱动每次都做全 arena 的缓存维护（真机实测是主要开销之一）。
    // 输出约定是最后一个绑定（sr_graph 里 binds[nIn] = 输出）。
    const BufferRef &out = buffers[qMax(0, count - 1)];
    VkBufferMemoryBarrier bb{};
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    bb.buffer = out.buffer;
    bb.offset = out.offset;
    bb.size = out.size;
    Get().CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &bb, 0,
                             nullptr);
    return true;
}

bool Device::endPass()
{
    if (!passOpen_)
        return true;
    const int slot = int(passSeq_ % 2);
    if (!Check(Get().EndCommandBuffer(cmds_[slot]), "vkEndCommandBuffer", &lastError_))
        return false;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmds_[slot];
    Get().ResetFences(device_, 1, &fences_[slot]);
    if (!Check(Get().QueueSubmit(queue_, 1, &si, fences_[slot]), "vkQueueSubmit", &lastError_))
        return false;
    passOpen_ = false;
    ++passSeq_;
    // 不在这里等待 fence：等待由
    //   * 下一个同 slot pass 的 beginPass（slot 复用安全），或
    //   * download 的 xferFence（队列序：xfer 完成即涵盖此前全部 pass）
    // 承担。串行调用方（run 单 tile）的行为不变——download 里的等待
    // 与旧的 endPass 等待完全等价，只是少了一次 fence 语义往返。
    return true;
}

bool Device::waitForAllPasses()
{
    if (!valid())
        return false;
    // 两个 fence 都是 SIGNALED 初始：从未提交过的等待立即返回，已完成的同理。
    // 队列 FIFO 语义下最近提交的 fence 完成即涵盖此前全部 pass，这里全等
    // 只是保险（深度 2 流水时两个 slot 可能都在飞）。
    VkFence both[2] = {fences_[0], fences_[1]};
    return Check(Get().WaitForFences(device_, 2, both, VK_TRUE, UINT64_MAX),
                 "vkWaitForFences(all)", &lastError_);
}

bool Device::download(const BufferRef &src, void *dst, quint64 bytes)
{
    if (!src.valid() || bytes == 0 || bytes > src.size || bytes > stagingBytes_) {
        lastError_ = QStringLiteral("download 参数非法（bytes=%1 src=%2）").arg(bytes).arg(src.size);
        return false;
    }
    // staging 段也走环形：流水场景下 pass 还在飞时 download 不能覆盖它的
    // 输入段（旧实现写死 dstOffset 0，串行流程里 endPass 已同步所以没事，
    // 流水后必须让开）。段耗尽时等全部在飞 pass 完成再重置环。
    const quint64 aligned = (stagingBump_ + 255) / 256 * 256;
    if (aligned + bytes > stagingBytes_) {
        VkFence both[2] = {fences_[0], fences_[1]};
        if (!Check(Get().WaitForFences(device_, 2, both, VK_TRUE, UINT64_MAX),
                   "vkWaitForFences(staging 回卷)", &lastError_))
            return false;
        stagingBump_ = 0;
        stagingBump_ = 256;   // 避开 0：download 段仍可能与早期上传段重合，256 对齐无害
        return download(src, dst, bytes);
    }
    stagingBump_ = aligned + bytes;

    VkCommandBufferAllocateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb.commandPool = cmdPool_;
    cb.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb.commandBufferCount = 1;
    VkCommandBuffer tmp = VK_NULL_HANDLE;
    if (!Check(Get().AllocateCommandBuffers(device_, &cb, &tmp), "vkAllocateCommandBuffers",
               &lastError_))
        return false;
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Get().BeginCommandBuffer(tmp, &bi);
    VkMemoryBarrier mb{};
    mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    Get().CmdPipelineBarrier(tmp, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    VkBufferCopy region{};
    region.srcOffset = src.offset;
    region.dstOffset = aligned;
    region.size = bytes;
    Get().CmdCopyBuffer(tmp, src.buffer, staging_, 1, &region);
    Get().EndCommandBuffer(tmp);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &tmp;
    Get().ResetFences(device_, 1, &xferFence_);
    const VkResult r = Get().QueueSubmit(queue_, 1, &si, xferFence_);
    if (r == VK_SUCCESS)
        Get().WaitForFences(device_, 1, &xferFence_, VK_TRUE, UINT64_MAX);
    Get().FreeCommandBuffers(device_, cmdPool_, 1, &tmp);
    if (!Check(r, "vkQueueSubmit(download)", &lastError_))
        return false;
    // xferFence 完成在队列序上涵盖此前提交的全部 pass：这里返回时输出已
    // 落到 host 内存，且对应的 pass 不会再被 GPU 访问
    std::memcpy(dst, static_cast<const char *>(stagingMap_) + aligned, size_t(bytes));
    return true;
}

}  // namespace vk
