#include "vulkan_layer_exports.h"
#include "descriptor_shim.h"
#include <cstring>
#include <dlfcn.h>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>
#include <android/log.h>

#define LOG_TAG "MaliCompat"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Standard Android HAL type definitions (strictly aligned for 64-bit LP64)
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
#ifdef __LP64__
    uint64_t reserved[12];
#else
    uint32_t reserved[12];
#endif
    int (*close)(struct hw_device_t* device);
};

struct hwvulkan_device_t {
    struct hw_device_t common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

namespace {

void* g_real_vulkan = nullptr;
PFN_vkGetInstanceProcAddr g_real_vkGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr g_real_vkGetDeviceProcAddr = nullptr;

PFN_vkCreateDescriptorSetLayout g_real_vkCreateDescriptorSetLayout = nullptr;
PFN_vkAllocateDescriptorSets g_real_vkAllocateDescriptorSets = nullptr;
PFN_vkUpdateDescriptorSets g_real_vkUpdateDescriptorSets = nullptr;
PFN_vkCmdBindDescriptorSets g_real_vkCmdBindDescriptorSets = nullptr;
PFN_vkCreateQueryPool g_real_vkCreateQueryPool = nullptr;
PFN_vkGetPhysicalDeviceFeatures g_real_vkGetPhysicalDeviceFeatures = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 g_real_vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkEnumerateDeviceExtensionProperties g_real_vkEnumerateDeviceExtensionProperties = nullptr;
PFN_vkCreateDevice g_real_vkCreateDevice = nullptr;
PFN_vkCreateInstance g_real_vkCreateInstance = nullptr;
PFN_vkEnumerateInstanceExtensionProperties g_real_vkEnumerateInstanceExtensionProperties = nullptr;

std::once_flag g_init_flag;
void* g_vendor_vulkan = nullptr;
hwvulkan_device_t* g_vendor_device = nullptr;
hwvulkan_device_t g_malicompat_device;

void InitRealVulkan() {
    std::call_once(g_init_flag, []() {
        g_real_vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (g_real_vulkan) {
            g_real_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_real_vulkan, "vkGetInstanceProcAddr"));
            g_real_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(g_real_vulkan, "vkGetDeviceProcAddr"));
            g_real_vkGetPhysicalDeviceFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(dlsym(g_real_vulkan, "vkGetPhysicalDeviceFeatures"));
            g_real_vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(dlsym(g_real_vulkan, "vkGetPhysicalDeviceFeatures2"));
            g_real_vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(dlsym(g_real_vulkan, "vkEnumerateDeviceExtensionProperties"));
            g_real_vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(dlsym(g_real_vulkan, "vkCreateDevice"));
            g_real_vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(dlsym(g_real_vulkan, "vkCreateInstance"));
            g_real_vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(dlsym(g_real_vulkan, "vkEnumerateInstanceExtensionProperties"));
        }
        
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

static int malicompat_device_close(struct hw_device_t* /*device*/) {
    if (g_vendor_device && g_vendor_device->common.close) {
        return g_vendor_device->common.close(&g_vendor_device->common);
    }
    return 0;
}

static VkResult VKAPI_CALL malicompat_hal_EnumerateInstanceExtensionProperties(
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    if (g_vendor_device && g_vendor_device->EnumerateInstanceExtensionProperties) {
        return g_vendor_device->EnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
    }
    InitRealVulkan();
    if (g_real_vkEnumerateInstanceExtensionProperties) {
        return g_real_vkEnumerateInstanceExtensionProperties(pLayerName, pPropertyCount, pProperties);
    }
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL malicompat_hal_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    if (g_vendor_device && g_vendor_device->CreateInstance) {
        return g_vendor_device->CreateInstance(pCreateInfo, pAllocator, pInstance);
    }
    InitRealVulkan();
    if (g_real_vkCreateInstance) {
        return g_real_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

static int malicompat_hal_open(const struct hw_module_t* module, const char* id, struct hw_device_t** device) {
    LOGI("MaliCompat HAL open called: id=%s", id ? id : "null");
    InitRealVulkan();

    if (g_vendor_vulkan) {
        auto* vendor_hmi = reinterpret_cast<hw_module_t*>(dlsym(g_vendor_vulkan, "HMI"));
        if (vendor_hmi && vendor_hmi->methods && vendor_hmi->methods->open) {
            struct hw_device_t* raw_dev = nullptr;
            int res = vendor_hmi->methods->open(vendor_hmi, id, &raw_dev);
            if (res == 0 && raw_dev) {
                g_vendor_device = reinterpret_cast<hwvulkan_device_t*>(raw_dev);
                LOGI("Vendor HAL open succeeded: device=%p", g_vendor_device);
            } else {
                LOGW("Vendor HAL open failed: %d", res);
            }
        }
    }

    std::memset(&g_malicompat_device, 0, sizeof(g_malicompat_device));
    g_malicompat_device.common.tag = HARDWARE_DEVICE_TAG;
    g_malicompat_device.common.version = 0;
    g_malicompat_device.common.module = const_cast<hw_module_t*>(module);
    g_malicompat_device.common.close = malicompat_device_close;
    g_malicompat_device.EnumerateInstanceExtensionProperties = malicompat_hal_EnumerateInstanceExtensionProperties;
    g_malicompat_device.CreateInstance = malicompat_hal_CreateInstance;
    g_malicompat_device.GetInstanceProcAddr = malicompat_vkGetInstanceProcAddr;

    *device = &g_malicompat_device.common;
    LOGI("MaliCompat HAL device returned: %p (GetInstanceProcAddr=%p)", *device, g_malicompat_device.GetInstanceProcAddr);
    return 0;
}

static hw_module_methods_t g_malicompat_methods = {
    .open = malicompat_hal_open
};

} // namespace

extern "C" {

// Android HAL Module Information symbol required by Android Vulkan loader
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

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures* pFeatures) {
    InitRealVulkan();
    PFN_vkGetPhysicalDeviceFeatures realPfn = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realPfn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures"));
    }
    if (!realPfn && g_real_vkGetPhysicalDeviceFeatures) {
        realPfn = g_real_vkGetPhysicalDeviceFeatures;
    }
    if (!realPfn && g_real_vkGetInstanceProcAddr) {
        realPfn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
            g_real_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures"));
    }

    if (realPfn) {
        realPfn(physicalDevice, pFeatures);
    }

    if (pFeatures) {
        // Emulate vertexPipelineStoresAndAtomics (required by Xenos / RexGPU emulation)
        pFeatures->vertexPipelineStoresAndAtomics = VK_TRUE;
        // fillModeNonSolid is also spoofed to satisfy wireframe/debug pipelines
        pFeatures->fillModeNonSolid = VK_TRUE;
        LOGI("Intercepted vkGetPhysicalDeviceFeatures: spoofed vertexPipelineStoresAndAtomics=VK_TRUE, fillModeNonSolid=VK_TRUE");
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures) {
    InitRealVulkan();
    PFN_vkGetPhysicalDeviceFeatures2 realPfn = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realPfn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2"));
    }
    if (!realPfn && g_real_vkGetPhysicalDeviceFeatures2) {
        realPfn = g_real_vkGetPhysicalDeviceFeatures2;
    }
    if (!realPfn && g_real_vkGetInstanceProcAddr) {
        realPfn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            g_real_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2"));
    }

    if (realPfn) {
        realPfn(physicalDevice, pFeatures);
    }
    if (!pFeatures) return;

    pFeatures->features.vertexPipelineStoresAndAtomics = VK_TRUE;
    pFeatures->features.fillModeNonSolid = VK_TRUE;

    // Intercept pNext chain to expose emulated descriptor indexing features
    auto* next = reinterpret_cast<VkBaseOutStructure*>(pFeatures->pNext);
    while (next) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            auto* indexing = reinterpret_cast<VkPhysicalDeviceDescriptorIndexingFeatures*>(next);
            indexing->shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
            indexing->shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
            indexing->shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
            indexing->shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
            indexing->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            indexing->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            indexing->shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
            indexing->shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE;
            indexing->shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
            indexing->shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
            indexing->descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
            indexing->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            indexing->descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
            indexing->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            indexing->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
            indexing->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
            indexing->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            indexing->descriptorBindingPartiallyBound = VK_TRUE;
            indexing->descriptorBindingVariableDescriptorCount = VK_TRUE;
            indexing->runtimeDescriptorArray = VK_TRUE;
            LOGI("Intercepted VkPhysicalDeviceDescriptorIndexingFeatures: all descriptor indexing flags enabled!");
            break;
        }
        next = next->pNext;
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice physicalDevice,
    const char* pLayerName,
    uint32_t* pPropertyCount,
    VkExtensionProperties* pProperties) {
    InitRealVulkan();
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;

    PFN_vkEnumerateDeviceExtensionProperties realEnum = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realEnum = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties"));
    }
    if (!realEnum && g_real_vkEnumerateDeviceExtensionProperties) {
        realEnum = g_real_vkEnumerateDeviceExtensionProperties;
    }
    if (!realEnum && g_real_vkGetInstanceProcAddr) {
        realEnum = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            g_real_vkGetInstanceProcAddr(nullptr, "vkEnumerateDeviceExtensionProperties"));
    }

    if (!pProperties) {
        uint32_t count = 0;
        VkResult res = VK_SUCCESS;
        if (realEnum) {
            res = realEnum(physicalDevice, pLayerName, &count, nullptr);
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
    if (realEnum) {
        res = realEnum(physicalDevice, pLayerName, &real_count, pProperties);
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

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice) {
    InitRealVulkan();
    LOGI("Intercepted vkCreateDevice");

    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;

    VkDeviceCreateInfo modifiedCreateInfo = *pCreateInfo;

    // Filter out VK_EXT_descriptor_indexing from enabled extensions for vendor driver
    std::vector<const char*> filteredExtensions;
    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
        const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
        if (ext && std::strcmp(ext, "VK_EXT_descriptor_indexing") == 0) {
            LOGI("Filtering out unsupported VK_EXT_descriptor_indexing from vendor vkCreateDevice");
            continue;
        }
        filteredExtensions.push_back(ext);
    }
    modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(filteredExtensions.size());
    modifiedCreateInfo.ppEnabledExtensionNames = filteredExtensions.data();

    // Sanitize enabled features: clear flags unsupported by Mali-G71 hardware
    VkPhysicalDeviceFeatures modifiedFeatures;
    if (pCreateInfo->pEnabledFeatures) {
        modifiedFeatures = *pCreateInfo->pEnabledFeatures;
        if (modifiedFeatures.vertexPipelineStoresAndAtomics) {
            LOGI("Clearing unsupported vertexPipelineStoresAndAtomics for vendor vkCreateDevice");
            modifiedFeatures.vertexPipelineStoresAndAtomics = VK_FALSE;
        }
        if (modifiedFeatures.fillModeNonSolid) {
            LOGI("Clearing unsupported fillModeNonSolid for vendor vkCreateDevice");
            modifiedFeatures.fillModeNonSolid = VK_FALSE;
        }
        modifiedCreateInfo.pEnabledFeatures = &modifiedFeatures;
    }

    // Unlink VkPhysicalDeviceDescriptorIndexingFeatures from pNext
    VkBaseOutStructure* prev = nullptr;
    auto* cur = reinterpret_cast<VkBaseOutStructure*>(const_cast<void*>(pCreateInfo->pNext));
    VkBaseOutStructure* newHead = cur;

    while (cur) {
        if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
            LOGI("Unlinking VkPhysicalDeviceDescriptorIndexingFeatures from vendor vkCreateDevice pNext");
            if (prev) {
                prev->pNext = cur->pNext;
            } else {
                newHead = cur->pNext;
            }
            cur = cur->pNext;
            continue;
        }
        prev = cur;
        cur = cur->pNext;
    }
    modifiedCreateInfo.pNext = newHead;

    PFN_vkCreateDevice realCreate = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realCreate = reinterpret_cast<PFN_vkCreateDevice>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice"));
    }
    if (!realCreate && g_real_vkCreateDevice) {
        realCreate = g_real_vkCreateDevice;
    }
    if (!realCreate && g_real_vkGetInstanceProcAddr) {
        realCreate = reinterpret_cast<PFN_vkCreateDevice>(
            g_real_vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice"));
    }

    if (!realCreate) {
        LOGE("Failed to resolve real vkCreateDevice");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = realCreate(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);
    LOGI("Vendor vkCreateDevice returned: %d (device=%p)", res, pDevice ? *pDevice : nullptr);
    return res;
}

MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    InitRealVulkan();
    if (!pName) return nullptr;

    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDevice);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceFeatures);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceFeatures2);
    if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkEnumerateDeviceExtensionProperties);
    if (std::strcmp(pName, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorSetLayout);
    if (std::strcmp(pName, "vkAllocateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkAllocateDescriptorSets);
    if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkUpdateDescriptorSets);
    if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdBindDescriptorSets);
    if (std::strcmp(pName, "vkCreateQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateQueryPool);

    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        auto pfn = g_vendor_device->GetInstanceProcAddr(instance, pName);
        if (pfn) return pfn;
    }
    if (g_real_vkGetInstanceProcAddr) {
        return g_real_vkGetInstanceProcAddr(instance, pName);
    }
    return nullptr;
}

MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    InitRealVulkan();
    if (!pName) return nullptr;

    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetDeviceProcAddr);
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
    if (!g_real_vkCreateDescriptorSetLayout && g_real_vulkan) {
        g_real_vkCreateDescriptorSetLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(dlsym(g_real_vulkan, "vkCreateDescriptorSetLayout"));
    }
    return malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorSetLayout(
        device, pCreateInfo, pAllocator, pSetLayout, g_real_vkCreateDescriptorSetLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets) {
    InitRealVulkan();
    if (!g_real_vkAllocateDescriptorSets && g_real_vulkan) {
        g_real_vkAllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(dlsym(g_real_vulkan, "vkAllocateDescriptorSets"));
    }
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
    if (!g_real_vkUpdateDescriptorSets && g_real_vulkan) {
        g_real_vkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(dlsym(g_real_vulkan, "vkUpdateDescriptorSets"));
    }
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
    if (!g_real_vkCmdBindDescriptorSets && g_real_vulkan) {
        g_real_vkCmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(dlsym(g_real_vulkan, "vkCmdBindDescriptorSets"));
    }
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

    // Neuter occlusion queries on ARM Mali to prevent driver resets
    if (pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION) {
        *pQueryPool = reinterpret_cast<VkQueryPool>(0xDEADBEEF);
        return VK_SUCCESS;
    }

    if (!g_real_vkCreateQueryPool && g_real_vulkan) {
        g_real_vkCreateQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(dlsym(g_real_vulkan, "vkCreateQueryPool"));
    }
    if (g_real_vkCreateQueryPool) {
        return g_real_vkCreateQueryPool(device, pCreateInfo, pAllocator, pQueryPool);
    }
    return VK_SUCCESS;
}

// Standard exported Vulkan symbols for direct dynamic linker binding
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo* c, const VkAllocationCallbacks* a, VkDescriptorSetLayout* l) {
    return malicompat_vkCreateDescriptorSetLayout(d, c, a, l);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo* a, VkDescriptorSet* s) {
    return malicompat_vkAllocateDescriptorSets(d, a, s);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d, uint32_t w, const VkWriteDescriptorSet* pw, uint32_t c, const VkCopyDescriptorSet* pc) {
    malicompat_vkUpdateDescriptorSets(d, w, pw, c, pc);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer b, VkPipelineBindPoint p, VkPipelineLayout l, uint32_t f, uint32_t c, const VkDescriptorSet* s, uint32_t o, const uint32_t* po) {
    malicompat_vkCmdBindDescriptorSets(b, p, l, f, c, s, o, po);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo* c, const VkAllocationCallbacks* a, VkQueryPool* q) {
    return malicompat_vkCreateQueryPool(d, c, a, q);
}
MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return malicompat_vkGetInstanceProcAddr(instance, pName);
}
MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    return malicompat_vkGetDeviceProcAddr(device, pName);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    InitRealVulkan();
    if (g_vendor_device && g_vendor_device->CreateInstance) {
        return g_vendor_device->CreateInstance(pCreateInfo, pAllocator, pInstance);
    }
    if (g_real_vkCreateInstance) {
        return g_real_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice d, const VkDeviceCreateInfo* c, const VkAllocationCallbacks* a, VkDevice* dev) {
    return malicompat_vkCreateDevice(d, c, a, dev);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice d, VkPhysicalDeviceFeatures* f) {
    malicompat_vkGetPhysicalDeviceFeatures(d, f);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice d, VkPhysicalDeviceFeatures2* f) {
    malicompat_vkGetPhysicalDeviceFeatures2(d, f);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice d, VkPhysicalDeviceFeatures2* f) {
    malicompat_vkGetPhysicalDeviceFeatures2(d, f);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice d, const char* l, uint32_t* c, VkExtensionProperties* p) {
    return malicompat_vkEnumerateDeviceExtensionProperties(d, l, c, p);
}

} // extern "C"
