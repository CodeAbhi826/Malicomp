#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <cstdint>
#include <cstddef>

#define MALICOMPAT_EXPORT __attribute__((visibility("default")))

// NDK Vulkan headers do not declare VK_ANDROID_native_buffer entry points
// (they are HAL-loader private, resolved via GetProcAddr). Declare the exact
// registry signatures locally for the M17 tracers.
typedef VkFlags VkSwapchainImageUsageFlagsANDROID;
typedef VkResult (VKAPI_PTR *PFN_vkGetSwapchainGrallocUsageANDROID)(
    VkDevice device, VkFormat format, VkImageUsageFlags imageUsage, int* pGrallocUsage);
typedef VkResult (VKAPI_PTR *PFN_vkGetSwapchainGrallocUsage2ANDROID)(
    VkDevice device, VkFormat format, VkImageUsageFlags imageUsage,
    VkSwapchainImageUsageFlagsANDROID swapchainImageUsage,
    uint64_t* pGrallocConsumerUsage, uint64_t* pGrallocProducerUsage);
typedef VkResult (VKAPI_PTR *PFN_vkAcquireImageANDROID)(
    VkDevice device, VkImage image, int* pNativeFenceFd,
    VkSemaphore semaphore, VkFence fence);
typedef VkResult (VKAPI_PTR *PFN_vkQueueSignalReleaseImageANDROID)(
    VkQueue queue, uint32_t waitSemaphoreCount, const VkSemaphore* pWaitSemaphores,
    VkImage image, int* pNativeFenceFd);

// Magic handle for virtualized query pools (occlusion suppression on ARM Mali)
#define VIRTUAL_QUERY_POOL_HANDLE ((VkQueryPool)(uintptr_t)0xDEADBEEFULL)

static inline bool malicompat_is_virtual_query_pool(VkQueryPool pool) {
    return pool == VIRTUAL_QUERY_POOL_HANDLE || pool == (VkQueryPool)(uintptr_t)0xDEADBEEF;
}

#ifdef __cplusplus
extern "C" {
#endif

// Vulkan Layer / Loader standard entry points
MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_vkGetInstanceProcAddr(VkInstance instance, const char* pName);
MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_vkGetDeviceProcAddr(VkDevice device, const char* pName);

// Resolve a device-level function inside the vendor dispatch domain.
// The VkDevice handles handed to us are the vendor's own; resolving via
// RTLD_DEFAULT can land on a *different* loader copy (memfd) that never saw
// the device and crashes in its dispatch. Falls back to nullptr (caller
// decides the fallback).
MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_resolve_vendor_device_proc(
    VkDevice device, const char* pName);

// Intercepted Device / Instance Functions
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator);

// Intercepted Descriptor Functions
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateDescriptorPool(
    VkDevice device,
    const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorPool* pDescriptorPool);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateDescriptorSetLayout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkUpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t* pDynamicOffsets);

// Query interception: safely neuters occlusion queries on ARM Mali
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateQueryPool(
    VkDevice device,
    const VkQueryPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkQueryPool* pQueryPool);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroyQueryPool(
    VkDevice device,
    VkQueryPool queryPool,
    const VkAllocationCallbacks* pAllocator);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdResetQueryPool(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdBeginQuery(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t query,
    VkQueryControlFlags flags);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdEndQuery(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t query);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdCopyQueryPoolResults(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount,
    VkBuffer dstBuffer,
    VkDeviceSize dstOffset,
    VkDeviceSize stride,
    VkQueryResultFlags flags);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetQueryPoolResults(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount,
    size_t dataSize,
    void* pData,
    VkDeviceSize stride,
    VkQueryResultFlags flags);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkResetQueryPool(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdWriteTimestamp(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlagBits pipelineStage,
    VkQueryPool queryPool,
    uint32_t query);

// WSI / Surface interception (Dual-Target WSI Forwarding to /system/lib64/libvulkan.so)
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateAndroidSurfaceKHR(
    VkInstance instance,
    const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormat2KHR* pSurfaceFormats);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    VkSurfaceKHR surface,
    VkBool32* pSupported);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* pAcquireInfo,
    uint32_t* pImageIndex);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkShaderModule* pShaderModule);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreatePipelineLayout(
    VkDevice device,
    const VkPipelineLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineLayout* pPipelineLayout);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreatePipelineCache(
    VkDevice device,
    const VkPipelineCacheCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineCache* pPipelineCache);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroyPipelineCache(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const VkAllocationCallbacks* pAllocator);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPipelineCacheData(
    VkDevice device,
    VkPipelineCache pipelineCache,
    size_t* pDataSize,
    void* pData);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkMergePipelineCaches(
    VkDevice device,
    VkPipelineCache dstCache,
    uint32_t srcCacheCount,
    const VkPipelineCache* pSrcCaches);

// M14: graphics/compute pipeline create tracer (find G71 killer in paint pipeline).
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateGraphicsPipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateComputePipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines);

// M15: queue->device tracking so vkQueueSubmit resolves the vendor proc in the
// right dispatch domain (instance-query returns NULL for it on Mali r15).
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue);

// M17: VK_ANDROID_native_buffer tracers. Android's loader terminates the
// KHR_swapchain entry points internally and calls INTO the driver for these.
// This is where 19.8k acquires went while 0 presents surfaced: if SignalRelease
// fails, the loader cancelBuffer()s instead of queueBuffer() (black screen).
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAcquireImageANDROID(
    VkDevice device,
    VkImage image,
    int* pNativeFenceFd,
    VkSemaphore semaphore,
    VkFence fence);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkQueueSignalReleaseImageANDROID(
    VkQueue queue,
    uint32_t waitSemaphoreCount,
    const VkSemaphore* pWaitSemaphores,
    VkImage image,
    int* pNativeFenceFd);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetSwapchainGrallocUsageANDROID(
    VkDevice device,
    VkFormat format,
    VkImageUsageFlags imageUsage,
    int* pGrallocUsage);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetSwapchainGrallocUsage2ANDROID(
    VkDevice device,
    VkFormat format,
    VkImageUsageFlags imageUsage,
    VkSwapchainImageUsageFlagsANDROID swapchainImageUsage,
    uint64_t* pGrallocConsumerUsage,
    uint64_t* pGrallocProducerUsage);

// Standard Vulkan entry points for direct dynamic linker binding
MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName);
MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures* pFeatures);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceFeatures2* pFeatures);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice physicalDevice, VkPhysicalDeviceProperties2* pProperties);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice physicalDevice, const char* pLayerName, uint32_t* pPropertyCount, VkExtensionProperties* pProperties);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice device, const VkDescriptorPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorPool* pDescriptorPool);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device, const VkDescriptorSetLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDescriptorSetLayout* pSetLayout);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice device, const VkDescriptorSetAllocateInfo* pAllocateInfo, VkDescriptorSet* pDescriptorSets);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet* pDescriptorWrites, uint32_t descriptorCopyCount, const VkCopyDescriptorSet* pDescriptorCopies);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout, uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet* pDescriptorSets, uint32_t dynamicOffsetCount, const uint32_t* pDynamicOffsets);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue* pQueue);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkGraphicsPipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount, const VkComputePipelineCreateInfo* pCreateInfos, const VkAllocationCallbacks* pAllocator, VkPipeline* pPipelines);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineCache* pPipelineCache);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache, const VkAllocationCallbacks* pAllocator);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice device, VkPipelineCache pipelineCache, size_t* pDataSize, void* pData);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice device, VkPipelineCache dstCache, uint32_t srcCacheCount, const VkPipelineCache* pSrcCaches);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice device, const VkQueryPoolCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkQueryPool* pQueryPool);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice device, VkQueryPool queryPool, const VkAllocationCallbacks* pAllocator);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query, VkQueryControlFlags flags);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t query);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer commandBuffer, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize stride, VkQueryResultFlags flags);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount, size_t dataSize, void* pData, VkDeviceSize stride, VkQueryResultFlags flags);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkResetQueryPool(VkDevice device, VkQueryPool queryPool, uint32_t firstQuery, uint32_t queryCount);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer commandBuffer, VkPipelineStageFlagBits pipelineStage, VkQueryPool queryPool, uint32_t query);

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(VkInstance instance, const VkAndroidSurfaceCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* pAllocator);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR* pSurfaceCapabilities);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pSurfaceFormatCount, VkSurfaceFormatKHR* pSurfaceFormats);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice, uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32* pSupported);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain, uint32_t* pSwapchainImageCount, VkImage* pSwapchainImages);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex);
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);

#ifdef __cplusplus
}
#endif

