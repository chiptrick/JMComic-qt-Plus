// Vulkan 最小装载层：动态加载 loader，只解析本项目用到的入口。
//
// 为什么不用 import lib / 直接链 vulkan-1：
//   * 桌面端这台机器没有 Vulkan SDK（没有 .dll.a、没有头文件），只有系统自带的
//     vulkan-1.dll；MinGW 直接链 .dll 不可靠；
//   * Android 端 libvulkan.so 在系统分区，dlopen 才能在"设备没有 Vulkan"时优雅降级；
//   * 与项目里 ONNX Runtime 的做法一致（QLibrary + 函数指针），缺失时只报原因不崩。
//
// 头文件：桌面端用 cpp/shell/3rdparty/vulkan/include（Khronos，随仓），
//        Android 用 NDK 自带的 vulkan/vulkan.h。两者都定义 VK_NO_PROTOTYPES，
//        全部入口从 loader 取，避免链接期依赖。
#pragma once

#include <QString>
#include <QStringList>

#include <vulkan/vulkan.h>

namespace vk {

// 一个物理设备的能力摘要（诊断/选设备用）
struct DeviceInfo
{
    QString name;
    quint32 vendorId = 0;
    quint32 deviceId = 0;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    quint32 apiVersion = 0;
    quint32 driverVersion = 0;
    quint64 vramBytes = 0;        // DEVICE_LOCAL 最大堆
    quint32 maxWorkGroupInvocations = 0;
    quint32 maxWorkGroupSize[3] = {0, 0, 0};
    quint32 maxStorageBufferRange = 0;
    quint32 maxComputeSharedMemory = 0;
    bool has8BitStorage = false;
    bool has16BitStorage = false;
    // shaderFloat16（Vulkan 1.2 core 特性）+ storageBuffer16BitAccess：fp16 存储与
    // 计算的硬件门槛。1.1 及更低的设备一律视为不支持（桌面 NVIDIA/AMD/Intel 与
    // Adreno 6xx/7xx/8xx 都是 1.2+，覆盖所有现实目标）。
    bool hasShaderFloat16 = false;
    // VK_KHR_push_descriptor：dispatch 免 allocate/update/bind set 的快速路径。
    bool hasPushDescriptor = false;
    QString typeName() const;
    QString summary() const;   // 一行 ASCII，便于日志断言
};

// loader 与入口集合。Load() 幂等。
struct Api
{
    bool loaded = false;
    QString loaderPath;
    QString error;

    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkEnumerateInstanceVersion EnumerateInstanceVersion = nullptr;
    PFN_vkCreateInstance CreateInstance = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures GetPhysicalDeviceFeatures = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties = nullptr;
    PFN_vkCreateDevice CreateDevice = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
    // 1.1+ instance 级（apiVersion 不足时 loader 返回 null，能力探测随之降级）
    PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
    PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;

    // device-level
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkResetDescriptorPool ResetDescriptorPool = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdDispatch CmdDispatch = nullptr;
    PFN_vkCmdPushConstants CmdPushConstants = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdCopyBuffer CmdCopyBuffer = nullptr;
    PFN_vkCmdFillBuffer CmdFillBuffer = nullptr;
    // VK_KHR_push_descriptor（可选扩展；hasPushDescriptor 为真时才解析/使用）
    PFN_vkCmdPushDescriptorSetKHR CmdPushDescriptorSetKHR = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
};

Api &Get();
// 幂等加载；失败时 Get().error 有原因（缺 loader / 缺入口名）。
// 只解析 instance 之前的入口（vkCreateInstance / vkEnumerateInstanceVersion）：
// 其余 instance 级函数必须在 vkCreateInstance 之后用带 instance 的
// vkGetInstanceProcAddr 才拿得到（Windows loader 在 instance=null 时只回少数几个）。
bool Load();
// 用已创建的 instance 补齐 instance 级入口（CreateInstance 之后调用）
bool ResolveInstance(VkInstance inst);

// 枚举设备（需已 Load）。失败时 error 有原因。
bool EnumerateDevices(QList<DeviceInfo> *out, QString *error);

// 把 VkResult 转成人能读的原因
QString ResultText(VkResult r);

}  // namespace vk
