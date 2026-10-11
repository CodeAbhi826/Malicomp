#include "vulkan_layer_exports.h"
#include "descriptor_shim.h"
#include <cstring>
#include <dlfcn.h>
#include <unordered_map>
#include <mutex>
#include <android/log.h>

#define LOG_TAG "MaliCompat"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
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
PFN_vkEnumerateDeviceExtensionProperties g_real_vkEnumerateDeviceExtensionProperties = nullptr;

std::once_flag g_init_flag;
void* g_vendor_vulkan = nullptr;

void InitRealVulkan() {
    std::call_once(g_init_flag, []() {
        // Try system libvulkan first
        g_real_vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (g_real_vulkan) {
            g_real_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_real_vulkan, "vkGetInstanceProcAddr"));
            g_real_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(g_real_vulkan, "vkGetDeviceProcAddr"));
            g_real_vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(dlsym(g_real_vulkan, "vkGetPhysicalDeviceFeatures2"));
            g_real_vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(dlsym(g_real_vulkan, "vkEnumerateDeviceExtensionProperties"));
        }
        
        // Also open vendor HAL library directly to forward HMI module metadata and device entry points
        g_vendor_vulkan = dlopen("/vendor/lib64/hw/vulkan.universal7884B.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_vendor_vulkan) {
            g_vendor_vulkan = dlopen("/vendor/lib64/hw/vulkan.universal7904.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (!g_vendor_vulkan) {
            g_vendor_vulkan = dlopen("/vendor/lib64/egl/libGLES_mali.so", RTLD_NOW | RTLD_LOCAL);
        }

        LOGI("Initialized MaliCompat: real libvulkan=%p, vendor HAL=%p", g_real_vulkan, g_vendor_vulkan);
    });
}

} // namespace

// Standard Android HAL type definitions
#define HARDWARE_MODULE_TAG 0x48574d54 // ('H' 'W' 'M' 'T')
#define HARDWARE_DEVICE_TAG 0x48574454 // ('H' 'W' 'D' 'T')

struct hw_module_t;
struct hw_device_t;

struct hw_module_methods_t {
    int (*open)(const struct hw_module_t* module, const char* id, struct hw_device_t** device);
};

struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char* id;
    const char* name;
    const char* author;
    struct hw_module_methods_t* methods;
    void* dso;
#ifdef __LP64__
    uint64_t reserved[32 - 7];
#else
    uint32_t reserved[32 - 7];
#endif
};

struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t* module;
    uint32_t reserved[12];
    int (*close)(struct hw_device_t* device);
};

struct hwvulkan_device_t {
    struct hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

// Forward declaration
extern "C" {
extern hw_module_t HMI;
}

namespace {

static hwvulkan_device_t g_malicompat_device;

static int malicompat_device_close(struct hw_device_t* /*device*/) {
    return 0;
}

static VkResult VKAPI_CALL malicompat_hal_EnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    InitRealVulkan();
    if (g_real_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            dlsym(g_real_vulkan, "vkEnumerateInstanceExtensionProperties"));
        if (pfn) return pfn(pLayerName, pPropertyCount, pProperties);
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL malicompat_hal_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    InitRealVulkan();
    if (g_real_vkGetInstanceProcAddr) {
        auto pfn = reinterpret_cast<PFN_vkCreateInstance>(
            g_real_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
        if (pfn) return pfn(pCreateInfo, pAllocator, pInstance);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

static int malicompat_hal_open(const struct hw_module_t* module, const char* id, struct hw_device_t** device) {
    LOGI("MaliCompat HAL open called: id=%s", id ? id : "null");
    InitRealVulkan();

    // If vendor HAL is available, try delegating open to vendor module first
    if (g_vendor_vulkan) {
        auto* vendor_hmi = reinterpret_cast<hw_module_t*>(dlsym(g_vendor_vulkan, "HMI"));
        if (vendor_hmi && vendor_hmi->methods && vendor_hmi->methods->open) {
            LOGI("Forwarding HAL open to vendor HMI (%p)", vendor_hmi);
            int res = vendor_hmi->methods->open(vendor_hmi, id, device);
            if (res == 0 && device && *device) {
                LOGI("Vendor HAL open succeeded: device=%p", *device);
                // Intercept GetInstanceProcAddr on the returned hwvulkan_device_t
                auto* vulkan_dev = reinterpret_cast<hwvulkan_device_t*>(*device);
                vulkan_dev->GetInstanceProcAddr = malicompat_vkGetInstanceProcAddr;
                return 0;
            }
            LOGW("Vendor HAL open returned %d, falling back to internal device", res);
        }
    }

    // Fallback: populate internal hwvulkan_device_t
    std::memset(&g_malicompat_device, 0, sizeof(g_malicompat_device));
    g_malicompat_device.common.tag = HARDWARE_DEVICE_TAG;
    g_malicompat_device.common.version = 0;
    g_malicompat_device.common.module = const_cast<hw_module_t*>(module);
    g_malicompat_device.common.close = malicompat_device_close;
    g_malicompat_device.EnumerateInstanceExtensionProperties = malicompat_hal_EnumerateInstanceExtensionProperties;
    g_malicompat_device.CreateInstance = malicompat_hal_CreateInstance;
    g_malicompat_device.GetInstanceProcAddr = malicompat_vkGetInstanceProcAddr;

    *device = &g_malicompat_device.common;
    LOGI("MaliCompat fallback HAL device returned: %p", *device);
    return 0;
}

static hw_module_methods_t g_malicompat_methods = {
    .open = malicompat_hal_open
};

} // namespace

extern "C" {

// Android HAL Module Information symbol required by Android Vulkan loader when loaded via adrenotools
__attribute__((visibility("default"), used)) hw_module_t HMI = {
    .tag = HARDWARE_MODULE_TAG,
    .module_api_version = 1,
    .hal_api_version = 0,
    .id = "vulkan",
    .name = "MaliCompat Vulkan Translation Shim",
    .author = "CodeAbhi826",
    .methods = &g_malicompat_methods,
    .dso = nullptr,
    .reserved = {0}
};

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

VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    InitRealVulkan();
    if (!g_real_vkEnumerateDeviceExtensionProperties) {
        if (g_real_vkGetInstanceProcAddr) {
            g_real_vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                g_real_vkGetInstanceProcAddr(nullptr, "vkEnumerateDeviceExtensionProperties"));
        }
        if (!g_real_vkEnumerateDeviceExtensionProperties && g_real_vulkan) {
            g_real_vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                dlsym(g_real_vulkan, "vkEnumerateDeviceExtensionProperties"));
        }
    }

    if (!pProperties) {
        uint32_t count = 0;
        VkResult res = VK_SUCCESS;
        if (g_real_vkEnumerateDeviceExtensionProperties) {
            res = g_real_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &count, nullptr);
        }
        if (res == VK_SUCCESS || res == VK_INCOMPLETE) {
            *pPropertyCount = count + 1;
        } else {
            *pPropertyCount = count;
        }
        return res;
    }

    uint32_t capacity = *pPropertyCount;
    uint32_t real_count = capacity;
    VkResult res = VK_SUCCESS;
    if (g_real_vkEnumerateDeviceExtensionProperties) {
        res = g_real_vkEnumerateDeviceExtensionProperties(physicalDevice, pLayerName, &real_count, pProperties);
    }

    bool found = false;
    for (uint32_t i = 0; i < real_count; ++i) {
        if (std::strcmp(pProperties[i].extensionName, "VK_EXT_descriptor_indexing") == 0) {
            found = true;
            break;
        }
    }

    if (!found && real_count < capacity) {
        std::strncpy(pProperties[real_count].extensionName, "VK_EXT_descriptor_indexing", VK_MAX_EXTENSION_NAME_SIZE);
        pProperties[real_count].specVersion = 2;
        real_count++;
        LOGI("Appended emulated VK_EXT_descriptor_indexing to device extensions list");
    }
    *pPropertyCount = real_count;
    return res;
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
    if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkEnumerateDeviceExtensionProperties);

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
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceFeatures2);
    if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkEnumerateDeviceExtensionProperties);

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
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice d, VkPhysicalDeviceFeatures2* f) {
    malicompat_vkGetPhysicalDeviceFeatures2(d, f);
}
VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice d, const char* l, uint32_t* c, VkExtensionProperties* p) {
    return malicompat_vkEnumerateDeviceExtensionProperties(d, l, c, p);
}

} // extern "C"
