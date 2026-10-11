#include "vulkan_layer_exports.h"
#include "descriptor_shim.h"
#include <cstring>
#include <dlfcn.h>
#include <unordered_map>
#include <mutex>
#include <android/log.h>

#define LOG_TAG "MaliCompat"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

void* g_real_vulkan = nullptr;
PFN_vkGetInstanceProcAddr g_real_vkGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr g_real_vkGetDeviceProcAddr = nullptr;

PFN_vkCreateDescriptorSetLayout g_real_vkCreateDescriptorSetLayout = nullptr;
PFN_vkAllocateDescriptorSets g_real_vkAllocateDescriptorSets = nullptr;
PFN_vkUpdateDescriptorSets g_real_vkUpdateDescriptorSets = nullptr;
PFN_vkCmdBindDescriptorSets g_real_vkCmdBindDescriptorSets = nullptr;
PFN_vkCreateQueryPool g_real_vkCreateQueryPool = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 g_real_vkGetPhysicalDeviceFeatures2 = nullptr;

std::once_flag g_init_flag;
void* g_vendor_vulkan = nullptr;

void InitRealVulkan() {
    std::call_once(g_init_flag, []() {
        // Try system libvulkan first
        g_real_vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (g_real_vulkan) {
            g_real_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_real_vulkan, "vkGetInstanceProcAddr"));
            g_real_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(g_real_vulkan, "vkGetDeviceProcAddr"));
        }
        
        // Also open vendor HAL library directly to forward HMI module metadata
        g_vendor_vulkan = dlopen("/vendor/lib64/hw/vulkan.universal7884B.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_vendor_vulkan) {
            g_vendor_vulkan = dlopen("/vendor/lib64/hw/vulkan.universal7904.so", RTLD_NOW | RTLD_LOCAL);
        }

        LOGI("Initialized MaliCompat: real libvulkan=%p, vendor HAL=%p", g_real_vulkan, g_vendor_vulkan);
    });
}

} // namespace

extern "C" {

// Android HAL Module Information symbol required by Android Vulkan loader when loaded via adrenotools
__attribute__((visibility("default"))) struct {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    void* dso;
    uint8_t reserved[128];
} HMI = {
    0x444d4f48, // HARDWARE_MODULE_TAG ('H' 'O' 'M' 'D')
    1, 0,
    "vulkan",
    "MaliCompat Vulkan Translation Shim",
    "CodeAbhi826",
    nullptr,
    {0}
};

} // namespace

extern "C" {

VKAPI_ATTR void VKAPI_CALL malicompat_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures) {
    InitRealVulkan();
    if (g_real_vkGetPhysicalDeviceFeatures2) {
        g_real_vkGetPhysicalDeviceFeatures2(physicalDevice, pFeatures);
    }
    if (!pFeatures) return;

    // Intercept pNext chain to expose emulated descriptor indexing features to games
    auto* next = reinterpret_cast<VkBaseOutStructure*>(pFeatures->pNext);
    while (next) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            auto* indexing = reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(next);
            // Emulate capabilities: tell the game's check that these features are available via MaliCompat!
            indexing->runtimeDescriptorArray = VK_TRUE;
            indexing->descriptorBindingPartiallyBound = VK_TRUE;
            indexing->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            indexing->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            LOGI("Intercepted VkPhysicalDeviceDescriptorIndexingFeatures query: emulated flags enabled");
            break;
        }
        next = next->pNext;
    }
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    InitRealVulkan();
    if (!pName) return nullptr;

    if (std::strcmp(pName, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorSetLayout);
    if (std::strcmp(pName, "vkAllocateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkAllocateDescriptorSets);
    if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkUpdateDescriptorSets);
    if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdBindDescriptorSets);
    if (std::strcmp(pName, "vkCreateQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateQueryPool);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceFeatures2);

    if (g_real_vkGetInstanceProcAddr) {
        return g_real_vkGetInstanceProcAddr(instance, pName);
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    InitRealVulkan();
    if (!pName) return nullptr;

    if (std::strcmp(pName, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorSetLayout);
    if (std::strcmp(pName, "vkAllocateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkAllocateDescriptorSets);
    if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkUpdateDescriptorSets);
    if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdBindDescriptorSets);
    if (std::strcmp(pName, "vkCreateQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateQueryPool);

    if (g_real_vkGetDeviceProcAddr) {
        return g_real_vkGetDeviceProcAddr(device, pName);
    }
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateDescriptorSetLayout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout) {
    InitRealVulkan();
    return malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorSetLayout(
        device, pCreateInfo, pAllocator, pSetLayout, g_real_vkCreateDescriptorSetLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets) {
    InitRealVulkan();
    return malicompat::VulkanDescriptorShim::Instance().InterceptAllocateDescriptorSets(
        device, pAllocateInfo, pDescriptorSets, g_real_vkAllocateDescriptorSets);
}

VKAPI_ATTR void VKAPI_CALL malicompat_vkUpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies) {
    InitRealVulkan();
    malicompat::VulkanDescriptorShim::Instance().InterceptUpdateDescriptorSets(
        device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies, g_real_vkUpdateDescriptorSets);
}

VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t* pDynamicOffsets) {
    InitRealVulkan();
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets, g_real_vkCmdBindDescriptorSets);
}

VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateQueryPool(
    VkDevice device,
    const VkQueryPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkQueryPool* pQueryPool) {
    InitRealVulkan();
    if (!pCreateInfo || !pQueryPool) return VK_ERROR_INITIALIZATION_FAILED;

    // Quirk: Neuter occlusion queries on ARM Mali to prevent driver resets
    if (pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION) {
        *pQueryPool = reinterpret_cast<VkQueryPool>(0xDEADBEEF);
        return VK_SUCCESS;
    }

    if (g_real_vkCreateQueryPool) {
        return g_real_vkCreateQueryPool(device, pCreateInfo, pAllocator, pQueryPool);
    }
    return VK_SUCCESS;
}

// Standard LD_PRELOAD aliases for direct transparent interception
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo* c, const VkAllocationCallbacks* a, VkDescriptorSetLayout* l) {
    return malicompat_vkCreateDescriptorSetLayout(d, c, a, l);
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo* a, VkDescriptorSet* s) {
    return malicompat_vkAllocateDescriptorSets(d, a, s);
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d, uint32_t w, const VkWriteDescriptorSet* pw, uint32_t c, const VkCopyDescriptorSet* pc) {
    malicompat_vkUpdateDescriptorSets(d, w, pw, c, pc);
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer b, VkPipelineBindPoint p, VkPipelineLayout l, uint32_t f, uint32_t c, const VkDescriptorSet* s, uint32_t o, const uint32_t* po) {
    malicompat_vkCmdBindDescriptorSets(b, p, l, f, c, s, o, po);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo* c, const VkAllocationCallbacks* a, VkQueryPool* q) {
    return malicompat_vkCreateQueryPool(d, c, a, q);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return malicompat_vkGetInstanceProcAddr(instance, pName);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return malicompat_vkGetDeviceProcAddr(device, pName);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    InitRealVulkan();
    auto realCreate = reinterpret_cast<PFN_vkCreateInstance>(g_real_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!realCreate) return VK_ERROR_INITIALIZATION_FAILED;
    return realCreate(pCreateInfo, pAllocator, pInstance);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t* pApiVersion) {
    InitRealVulkan();
    auto realEnum = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(g_real_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
    if (realEnum) return realEnum(pApiVersion);
    if (pApiVersion) *pApiVersion = VK_API_VERSION_1_1;
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance instance, uint32_t* pPhysicalDeviceCount, VkPhysicalDevice* pPhysicalDevices) {
    InitRealVulkan();
    auto realEnum = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(g_real_vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    if (!realEnum) return VK_ERROR_INITIALIZATION_FAILED;
    return realEnum(instance, pPhysicalDeviceCount, pPhysicalDevices);
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator) {
    InitRealVulkan();
    auto realDestroy = reinterpret_cast<PFN_vkDestroyInstance>(g_real_vkGetInstanceProcAddr(instance, "vkDestroyInstance"));
    if (realDestroy) realDestroy(instance, pAllocator);
}

} // extern "C"
