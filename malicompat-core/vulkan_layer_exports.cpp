#include "vulkan_layer_exports.h"
#include "descriptor_shim.h"
#include <cstring>
#include <dlfcn.h>
#include <unordered_map>
#include <mutex>
#include <vector>
#include <string>
#include <algorithm>

#if __has_include(<android/log.h>)
#include <android/log.h>
#define LOG_TAG "MaliCompat"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOG_TAG "MaliCompat"
#define LOGI(...) do { printf("[INFO] " LOG_TAG ": " __VA_ARGS__); printf("\n"); } while(0)
#define LOGW(...) do { printf("[WARN] " LOG_TAG ": " __VA_ARGS__); printf("\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERROR] " LOG_TAG ": " __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

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

// Vendor Mali HAL handle and function pointers
void* g_vendor_vulkan = nullptr;
hwvulkan_device_t* g_vendor_device = nullptr;
hwvulkan_device_t g_malicompat_device;

// M5: last vendor VkInstance we created. Resolving vendor procs against it
// instead of VK_NULL_HANDLE: the NULL path returns a crippled 2-extension
// list from EnumerateDeviceExtensionProperties on this driver.
VkInstance g_vendor_instance = VK_NULL_HANDLE;

// Resolve a vendor proc, preferring the live instance over VK_NULL_HANDLE.
static PFN_vkVoidFunction malicompat_vendor_proc(const char* name) {
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        PFN_vkVoidFunction pfn = g_vendor_device->GetInstanceProcAddr(
            g_vendor_instance, name);
        if (pfn) return pfn;
        if (g_vendor_instance != VK_NULL_HANDLE) {
            pfn = g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, name);
            if (pfn) return pfn;
        }
    }
    return nullptr;
}

PFN_vkCreateDescriptorPool g_vendor_vkCreateDescriptorPool = nullptr;
PFN_vkCreateDescriptorSetLayout g_vendor_vkCreateDescriptorSetLayout = nullptr;
PFN_vkAllocateDescriptorSets g_vendor_vkAllocateDescriptorSets = nullptr;
PFN_vkUpdateDescriptorSets g_vendor_vkUpdateDescriptorSets = nullptr;
PFN_vkCmdBindDescriptorSets g_vendor_vkCmdBindDescriptorSets = nullptr;

PFN_vkCreateQueryPool g_vendor_vkCreateQueryPool = nullptr;
PFN_vkDestroyQueryPool g_vendor_vkDestroyQueryPool = nullptr;
PFN_vkCmdResetQueryPool g_vendor_vkCmdResetQueryPool = nullptr;
PFN_vkCmdBeginQuery g_vendor_vkCmdBeginQuery = nullptr;
PFN_vkCmdEndQuery g_vendor_vkCmdEndQuery = nullptr;
PFN_vkCmdCopyQueryPoolResults g_vendor_vkCmdCopyQueryPoolResults = nullptr;
PFN_vkGetQueryPoolResults g_vendor_vkGetQueryPoolResults = nullptr;
PFN_vkResetQueryPool g_vendor_vkResetQueryPool = nullptr;
PFN_vkCmdWriteTimestamp g_vendor_vkCmdWriteTimestamp = nullptr;
PFN_vkCmdFillBuffer g_vendor_vkCmdFillBuffer = nullptr;

PFN_vkGetPhysicalDeviceFeatures g_vendor_vkGetPhysicalDeviceFeatures = nullptr;
PFN_vkGetPhysicalDeviceFeatures2 g_vendor_vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkGetPhysicalDeviceProperties g_vendor_vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties2 g_vendor_vkGetPhysicalDeviceProperties2 = nullptr;
PFN_vkEnumerateDeviceExtensionProperties g_vendor_vkEnumerateDeviceExtensionProperties = nullptr;
PFN_vkCreateDevice g_vendor_vkCreateDevice = nullptr;
PFN_vkDestroyDevice g_vendor_vkDestroyDevice = nullptr;
PFN_vkCreateInstance g_vendor_vkCreateInstance = nullptr;
PFN_vkEnumerateInstanceExtensionProperties g_vendor_vkEnumerateInstanceExtensionProperties = nullptr;

PFN_vkQueueSubmit g_vendor_vkQueueSubmit = nullptr;
PFN_vkCreatePipelineLayout g_vendor_vkCreatePipelineLayout = nullptr;
PFN_vkCreatePipelineCache g_vendor_vkCreatePipelineCache = nullptr;
PFN_vkDestroyPipelineCache g_vendor_vkDestroyPipelineCache = nullptr;
PFN_vkGetPipelineCacheData g_vendor_vkGetPipelineCacheData = nullptr;
PFN_vkMergePipelineCaches g_vendor_vkMergePipelineCaches = nullptr;
PFN_vkQueuePresentKHR g_vendor_vkQueuePresentKHR = nullptr;


// Android System Loader handle and WSI function pointers (F1: Dual-Target Forwarding)
void* g_system_vulkan = nullptr;
PFN_vkGetInstanceProcAddr g_system_vkGetInstanceProcAddr = nullptr;
PFN_vkCreateAndroidSurfaceKHR g_system_vkCreateAndroidSurfaceKHR = nullptr;
PFN_vkDestroySurfaceKHR g_system_vkDestroySurfaceKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR g_system_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR g_system_vkGetPhysicalDeviceSurfaceCapabilities2KHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormatsKHR g_system_vkGetPhysicalDeviceSurfaceFormatsKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceFormats2KHR g_system_vkGetPhysicalDeviceSurfaceFormats2KHR = nullptr;
PFN_vkGetPhysicalDeviceSurfacePresentModesKHR g_system_vkGetPhysicalDeviceSurfacePresentModesKHR = nullptr;
PFN_vkGetPhysicalDeviceSurfaceSupportKHR g_system_vkGetPhysicalDeviceSurfaceSupportKHR = nullptr;
PFN_vkCreateSwapchainKHR g_system_vkCreateSwapchainKHR = nullptr;
PFN_vkDestroySwapchainKHR g_system_vkDestroySwapchainKHR = nullptr;
PFN_vkGetSwapchainImagesKHR g_system_vkGetSwapchainImagesKHR = nullptr;
PFN_vkAcquireNextImageKHR g_system_vkAcquireNextImageKHR = nullptr;
PFN_vkAcquireNextImage2KHR g_system_vkAcquireNextImage2KHR = nullptr;
PFN_vkQueuePresentKHR g_system_vkQueuePresentKHR = nullptr;

std::once_flag g_init_flag;

bool malicompat_is_wsi_symbol(const char* pName) {
    if (!pName) return false;
    static const char* const kWsiSymbols[] = {
        "vkCreateAndroidSurfaceKHR",
        "vkDestroySurfaceKHR",
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
        "vkGetPhysicalDeviceSurfaceCapabilities2KHR",
        "vkGetPhysicalDeviceSurfaceFormatsKHR",
        "vkGetPhysicalDeviceSurfaceFormats2KHR",
        "vkGetPhysicalDeviceSurfacePresentModesKHR",
        "vkGetPhysicalDeviceSurfaceSupportKHR",
        "vkCreateSwapchainKHR",
        "vkDestroySwapchainKHR",
        "vkGetSwapchainImagesKHR",
        "vkAcquireNextImageKHR",
        "vkAcquireNextImage2KHR",
        "vkQueuePresentKHR",
        nullptr
    };
    for (int i = 0; kWsiSymbols[i]; ++i) {
        if (std::strcmp(pName, kWsiSymbols[i]) == 0) return true;
    }
    return false;
}

void InitRealVulkan() {
    std::call_once(g_init_flag, []() {
        // 1. Open Vendor Mali HAL driver
        g_vendor_vulkan = dlopen("/vendor/lib64/hw/vulkan.universal7884B.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_vendor_vulkan) {
            g_vendor_vulkan = dlopen("/vendor/lib64/hw/vulkan.universal7904.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (!g_vendor_vulkan) {
            g_vendor_vulkan = dlopen("/vendor/lib64/egl/libGLES_mali.so", RTLD_NOW | RTLD_LOCAL);
        }

        if (g_vendor_vulkan) {
            g_vendor_vkCreateDescriptorPool = reinterpret_cast<PFN_vkCreateDescriptorPool>(dlsym(g_vendor_vulkan, "vkCreateDescriptorPool"));
            g_vendor_vkCreateDescriptorSetLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(dlsym(g_vendor_vulkan, "vkCreateDescriptorSetLayout"));
            g_vendor_vkAllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(dlsym(g_vendor_vulkan, "vkAllocateDescriptorSets"));
            g_vendor_vkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(dlsym(g_vendor_vulkan, "vkUpdateDescriptorSets"));
            g_vendor_vkCmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(dlsym(g_vendor_vulkan, "vkCmdBindDescriptorSets"));

            g_vendor_vkCreateQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(dlsym(g_vendor_vulkan, "vkCreateQueryPool"));
            g_vendor_vkDestroyQueryPool = reinterpret_cast<PFN_vkDestroyQueryPool>(dlsym(g_vendor_vulkan, "vkDestroyQueryPool"));
            g_vendor_vkCmdResetQueryPool = reinterpret_cast<PFN_vkCmdResetQueryPool>(dlsym(g_vendor_vulkan, "vkCmdResetQueryPool"));
            g_vendor_vkCmdBeginQuery = reinterpret_cast<PFN_vkCmdBeginQuery>(dlsym(g_vendor_vulkan, "vkCmdBeginQuery"));
            g_vendor_vkCmdEndQuery = reinterpret_cast<PFN_vkCmdEndQuery>(dlsym(g_vendor_vulkan, "vkCmdEndQuery"));
            g_vendor_vkCmdCopyQueryPoolResults = reinterpret_cast<PFN_vkCmdCopyQueryPoolResults>(dlsym(g_vendor_vulkan, "vkCmdCopyQueryPoolResults"));
            g_vendor_vkGetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(dlsym(g_vendor_vulkan, "vkGetQueryPoolResults"));
            g_vendor_vkResetQueryPool = reinterpret_cast<PFN_vkResetQueryPool>(dlsym(g_vendor_vulkan, "vkResetQueryPool"));
            g_vendor_vkCmdWriteTimestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(dlsym(g_vendor_vulkan, "vkCmdWriteTimestamp"));
            g_vendor_vkCmdFillBuffer = reinterpret_cast<PFN_vkCmdFillBuffer>(dlsym(g_vendor_vulkan, "vkCmdFillBuffer"));

            g_vendor_vkGetPhysicalDeviceFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(dlsym(g_vendor_vulkan, "vkGetPhysicalDeviceFeatures"));
            g_vendor_vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(dlsym(g_vendor_vulkan, "vkGetPhysicalDeviceFeatures2"));
            if (!g_vendor_vkGetPhysicalDeviceFeatures2) {
                g_vendor_vkGetPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(dlsym(g_vendor_vulkan, "vkGetPhysicalDeviceFeatures2KHR"));
            }
            g_vendor_vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(dlsym(g_vendor_vulkan, "vkGetPhysicalDeviceProperties"));
            g_vendor_vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(dlsym(g_vendor_vulkan, "vkGetPhysicalDeviceProperties2"));
            if (!g_vendor_vkGetPhysicalDeviceProperties2) {
                g_vendor_vkGetPhysicalDeviceProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(dlsym(g_vendor_vulkan, "vkGetPhysicalDeviceProperties2KHR"));
            }
            g_vendor_vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(dlsym(g_vendor_vulkan, "vkEnumerateDeviceExtensionProperties"));
            g_vendor_vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(dlsym(g_vendor_vulkan, "vkCreateDevice"));
            g_vendor_vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(dlsym(g_vendor_vulkan, "vkDestroyDevice"));
            g_vendor_vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(dlsym(g_vendor_vulkan, "vkCreateInstance"));
            g_vendor_vkEnumerateInstanceExtensionProperties = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(dlsym(g_vendor_vulkan, "vkEnumerateInstanceExtensionProperties"));

            g_vendor_vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(dlsym(g_vendor_vulkan, "vkQueueSubmit"));
            g_vendor_vkCreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(dlsym(g_vendor_vulkan, "vkCreatePipelineLayout"));
            g_vendor_vkCreatePipelineCache = reinterpret_cast<PFN_vkCreatePipelineCache>(dlsym(g_vendor_vulkan, "vkCreatePipelineCache"));
            g_vendor_vkDestroyPipelineCache = reinterpret_cast<PFN_vkDestroyPipelineCache>(dlsym(g_vendor_vulkan, "vkDestroyPipelineCache"));
            g_vendor_vkGetPipelineCacheData = reinterpret_cast<PFN_vkGetPipelineCacheData>(dlsym(g_vendor_vulkan, "vkGetPipelineCacheData"));
            g_vendor_vkMergePipelineCaches = reinterpret_cast<PFN_vkMergePipelineCaches>(dlsym(g_vendor_vulkan, "vkMergePipelineCaches"));
            g_vendor_vkQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(dlsym(g_vendor_vulkan, "vkQueuePresentKHR"));
        }

        // 2. Open Android System Loader (/system/lib64/libvulkan.so) for WSI / Surface Forwarding (F1)
        g_system_vulkan = dlopen("/system/lib64/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!g_system_vulkan) {
            g_system_vulkan = dlopen("/system/lib/libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (!g_system_vulkan) {
            g_system_vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        }

        if (g_system_vulkan) {
            g_system_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_system_vulkan, "vkGetInstanceProcAddr"));
            g_system_vkCreateAndroidSurfaceKHR = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(dlsym(g_system_vulkan, "vkCreateAndroidSurfaceKHR"));
            g_system_vkDestroySurfaceKHR = reinterpret_cast<PFN_vkDestroySurfaceKHR>(dlsym(g_system_vulkan, "vkDestroySurfaceKHR"));
            g_system_vkGetPhysicalDeviceSurfaceCapabilitiesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
            g_system_vkGetPhysicalDeviceSurfaceCapabilities2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
            g_system_vkGetPhysicalDeviceSurfaceFormatsKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
            g_system_vkGetPhysicalDeviceSurfaceFormats2KHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormats2KHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfaceFormats2KHR"));
            g_system_vkGetPhysicalDeviceSurfacePresentModesKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
            g_system_vkGetPhysicalDeviceSurfaceSupportKHR = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfaceSupportKHR"));
            g_system_vkCreateSwapchainKHR = reinterpret_cast<PFN_vkCreateSwapchainKHR>(dlsym(g_system_vulkan, "vkCreateSwapchainKHR"));
            g_system_vkDestroySwapchainKHR = reinterpret_cast<PFN_vkDestroySwapchainKHR>(dlsym(g_system_vulkan, "vkDestroySwapchainKHR"));
            g_system_vkGetSwapchainImagesKHR = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(dlsym(g_system_vulkan, "vkGetSwapchainImagesKHR"));
            g_system_vkAcquireNextImageKHR = reinterpret_cast<PFN_vkAcquireNextImageKHR>(dlsym(g_system_vulkan, "vkAcquireNextImageKHR"));
            g_system_vkAcquireNextImage2KHR = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(dlsym(g_system_vulkan, "vkAcquireNextImage2KHR"));
            g_system_vkQueuePresentKHR = reinterpret_cast<PFN_vkQueuePresentKHR>(dlsym(g_system_vulkan, "vkQueuePresentKHR"));
        }

        LOGI("Initialized MaliCompat: vendor HAL=%p, system loader=%p", g_vendor_vulkan, g_system_vulkan);
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
    InitRealVulkan();
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;

    uint32_t real_count = 0;
    VkResult res = VK_SUCCESS;
    if (g_vendor_device && g_vendor_device->EnumerateInstanceExtensionProperties) {
        res = g_vendor_device->EnumerateInstanceExtensionProperties(pLayerName, &real_count, nullptr);
    } else if (g_vendor_vkEnumerateInstanceExtensionProperties) {
        res = g_vendor_vkEnumerateInstanceExtensionProperties(pLayerName, &real_count, nullptr);
    }

    // Include WSI extensions provided by Android system loader
    std::vector<const char*> extraExts = {
        "VK_KHR_surface",
        "VK_KHR_android_surface"
    };

    if (!pProperties) {
        *pPropertyCount = real_count + static_cast<uint32_t>(extraExts.size());
        return VK_SUCCESS;
    }

    uint32_t capacity = *pPropertyCount;
    if (capacity > 0 && (g_vendor_device || g_vendor_vkEnumerateInstanceExtensionProperties)) {
        if (g_vendor_device && g_vendor_device->EnumerateInstanceExtensionProperties) {
            g_vendor_device->EnumerateInstanceExtensionProperties(pLayerName, &capacity, pProperties);
        } else if (g_vendor_vkEnumerateInstanceExtensionProperties) {
            g_vendor_vkEnumerateInstanceExtensionProperties(pLayerName, &capacity, pProperties);
        }
    }

    for (const char* extName : extraExts) {
        bool found = false;
        for (uint32_t i = 0; i < capacity; ++i) {
            if (std::strcmp(pProperties[i].extensionName, extName) == 0) {
                found = true;
                break;
            }
        }
        if (!found && capacity < *pPropertyCount) {
            std::strncpy(pProperties[capacity].extensionName, extName, VK_MAX_EXTENSION_NAME_SIZE);
            pProperties[capacity].specVersion = 25;
            capacity++;
        }
    }
    *pPropertyCount = capacity;
    return res;
}

static VkResult VKAPI_CALL malicompat_hal_CreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance) {
    InitRealVulkan();
    // Diagnostic probe (M2): log what the app asks the instance for.
    if (pCreateInfo) {
        uint32_t api = pCreateInfo->pApplicationInfo ? pCreateInfo->pApplicationInfo->apiVersion : 0;
        LOGI("CreateInstance request: apiVersion=0x%08x extCount=%u", api, pCreateInfo->enabledExtensionCount);
        if (pCreateInfo->ppEnabledExtensionNames) {
            for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
                const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
                LOGI("CreateInstance request ext[%u] = '%s'", i, ext ? ext : "(null)");
            }
        }
    }
    VkResult instanceRes = VK_ERROR_INITIALIZATION_FAILED;
    if (g_vendor_device && g_vendor_device->CreateInstance) {
        instanceRes = g_vendor_device->CreateInstance(pCreateInfo, pAllocator, pInstance);
    } else if (g_vendor_vkCreateInstance) {
        instanceRes = g_vendor_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
    }
    LOGI("CreateInstance result: %d (instance=%p)", instanceRes,
         (pInstance ? (const void*)*pInstance : nullptr));
    if (instanceRes == VK_SUCCESS && pInstance && *pInstance) {
        // M5: remember the vendor instance so later vendor-proc resolution
        // uses it instead of VK_NULL_HANDLE (which yields crippled results).
        g_vendor_instance = *pInstance;
    }
    return instanceRes;
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

// M6: device-domain resolution for VkDevice handles that are the vendor's
// own. See header note. Defined here (external linkage) because callers live
// in other TUs; InitRealVulkan is available at this point.
MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_resolve_vendor_device_proc(
    VkDevice device, const char* name) {
    if (!name) return nullptr;
    PFN_vkGetDeviceProcAddr vendorGetDevice = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        auto p = malicompat_vendor_proc("vkGetDeviceProcAddr");
        if (p) vendorGetDevice = reinterpret_cast<PFN_vkGetDeviceProcAddr>(p);
    }
    if (vendorGetDevice && device) {
        PFN_vkVoidFunction pfn = vendorGetDevice(device, name);
        if (pfn) return pfn;
    }
    if (g_vendor_vulkan) {
        PFN_vkVoidFunction pfn =
            reinterpret_cast<PFN_vkVoidFunction>(dlsym(g_vendor_vulkan, name));
        if (pfn) return pfn;
    }
    return nullptr;
}

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
            malicompat_vendor_proc("vkGetPhysicalDeviceFeatures"));
    }
    if (!realPfn && g_vendor_vkGetPhysicalDeviceFeatures) {
        realPfn = g_vendor_vkGetPhysicalDeviceFeatures;
    }

    if (realPfn) {
        realPfn(physicalDevice, pFeatures);
    }

    if (pFeatures) {
        // M4: pass through real vertexPipelineStoresAndAtomics / fillModeNonSolid.
        // Spoofing them TRUE lets feature gates pass, but the vendor then
        // rejects vkCreateDevice (routed straight to it, bypassing our
        // sanitizer). Gate tolerance for the one hard requirement left
        // (vertexPipelineStoresAndAtomics) is handled separately.
        LOGI("vkGetPhysicalDeviceFeatures passthrough (vertexPipelineStoresAndAtomics=%d fillModeNonSolid=%d)",
             (int)pFeatures->vertexPipelineStoresAndAtomics, (int)pFeatures->fillModeNonSolid);
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceFeatures2* pFeatures) {
    InitRealVulkan();
    PFN_vkGetPhysicalDeviceFeatures2 realPfn = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realPfn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            malicompat_vendor_proc("vkGetPhysicalDeviceFeatures2"));
        if (!realPfn) {
            realPfn = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
                malicompat_vendor_proc("vkGetPhysicalDeviceFeatures2KHR"));
        }
    }
    if (!realPfn && g_vendor_vkGetPhysicalDeviceFeatures2) {
        realPfn = g_vendor_vkGetPhysicalDeviceFeatures2;
    }

    if (realPfn) {
        realPfn(physicalDevice, pFeatures);
    } else {
        PFN_vkGetPhysicalDeviceFeatures realFeat1 = nullptr;
        if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
            realFeat1 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
                malicompat_vendor_proc("vkGetPhysicalDeviceFeatures"));
        }
        if (!realFeat1 && g_vendor_vkGetPhysicalDeviceFeatures) {
            realFeat1 = g_vendor_vkGetPhysicalDeviceFeatures;
        }
        if (realFeat1 && pFeatures) {
            realFeat1(physicalDevice, &pFeatures->features);
        }
    }
    if (!pFeatures) return;

    // M4: pass through real values (see vkGetPhysicalDeviceFeatures note).
    LOGI("vkGetPhysicalDeviceFeatures2 passthrough (vertexPipelineStoresAndAtomics=%d fillModeNonSolid=%d)",
         (int)pFeatures->features.vertexPipelineStoresAndAtomics, (int)pFeatures->features.fillModeNonSolid);

    // Intercept pNext chain to expose emulated descriptor indexing features (F2)
    uint32_t depth = 0;
    auto* next = reinterpret_cast<VkBaseOutStructure*>(pFeatures->pNext);
    while (next && depth++ < 64) {
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
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
            auto* v12 = reinterpret_cast<VkPhysicalDeviceVulkan12Features*>(next);
            v12->descriptorIndexing = VK_TRUE;
            v12->shaderInputAttachmentArrayDynamicIndexing = VK_TRUE;
            v12->shaderUniformTexelBufferArrayDynamicIndexing = VK_TRUE;
            v12->shaderStorageTexelBufferArrayDynamicIndexing = VK_TRUE;
            v12->shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
            v12->shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            v12->shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
            v12->shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
            v12->shaderInputAttachmentArrayNonUniformIndexing = VK_TRUE;
            v12->shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
            v12->shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
            v12->descriptorBindingUniformBufferUpdateAfterBind = VK_TRUE;
            v12->descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            v12->descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
            v12->descriptorBindingStorageBufferUpdateAfterBind = VK_TRUE;
            v12->descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
            v12->descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
            v12->descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            v12->descriptorBindingPartiallyBound = VK_TRUE;
            v12->descriptorBindingVariableDescriptorCount = VK_TRUE;
            v12->runtimeDescriptorArray = VK_TRUE;
            LOGI("Intercepted VkPhysicalDeviceVulkan12Features: descriptor indexing flags enabled!");
        }
        next = next->pNext;
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties2* pProperties) {
    InitRealVulkan();
    if (!pProperties) return;

    PFN_vkGetPhysicalDeviceProperties2 realPfn = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realPfn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2"));
        if (!realPfn) {
            realPfn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2KHR"));
        }
    }
    if (!realPfn && g_vendor_vkGetPhysicalDeviceProperties2) {
        realPfn = g_vendor_vkGetPhysicalDeviceProperties2;
    }

    if (realPfn) {
        realPfn(physicalDevice, pProperties);
    } else {
        PFN_vkGetPhysicalDeviceProperties realProps1 = nullptr;
        if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
            realProps1 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
                g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties"));
        }
        if (!realProps1 && g_vendor_vkGetPhysicalDeviceProperties) {
            realProps1 = g_vendor_vkGetPhysicalDeviceProperties;
        }
        if (realProps1) {
            realProps1(physicalDevice, &pProperties->properties);
        }
    }

    // Intercept pNext chain to expose spoofed descriptor indexing limits (F3)
    uint32_t depth = 0;
    auto* next = reinterpret_cast<VkBaseOutStructure*>(pProperties->pNext);
    while (next && depth++ < 64) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES) {
            auto* props = reinterpret_cast<VkPhysicalDeviceDescriptorIndexingProperties*>(next);
            props->maxUpdateAfterBindDescriptorsInAllPools = 1048576;
            props->shaderUniformBufferArrayNonUniformIndexingNative = VK_FALSE;
            props->shaderSampledImageArrayNonUniformIndexingNative = VK_FALSE;
            props->shaderStorageBufferArrayNonUniformIndexingNative = VK_FALSE;
            props->shaderStorageImageArrayNonUniformIndexingNative = VK_FALSE;
            props->shaderInputAttachmentArrayNonUniformIndexingNative = VK_FALSE;
            props->robustBufferAccessUpdateAfterBind = VK_FALSE;
            props->quadDivergentImplicitLod = VK_FALSE;
            props->maxPerStageDescriptorUpdateAfterBindSamplers = 128;
            props->maxPerStageDescriptorUpdateAfterBindUniformBuffers = 16;
            props->maxPerStageDescriptorUpdateAfterBindStorageBuffers = 16;
            props->maxPerStageDescriptorUpdateAfterBindSampledImages = 128;
            props->maxPerStageDescriptorUpdateAfterBindStorageImages = 16;
            props->maxPerStageDescriptorUpdateAfterBindInputAttachments = 8;
            props->maxPerStageUpdateAfterBindResources = 128;
            props->maxDescriptorSetUpdateAfterBindSamplers = 128;
            props->maxDescriptorSetUpdateAfterBindUniformBuffers = 16;
            props->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 8;
            props->maxDescriptorSetUpdateAfterBindStorageBuffers = 16;
            props->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 4;
            props->maxDescriptorSetUpdateAfterBindSampledImages = 128;
            props->maxDescriptorSetUpdateAfterBindStorageImages = 16;
            props->maxDescriptorSetUpdateAfterBindInputAttachments = 8;
            LOGI("Intercepted VkPhysicalDeviceDescriptorIndexingProperties: limits spoofed");
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
            auto* v12 = reinterpret_cast<VkPhysicalDeviceVulkan12Properties*>(next);
            v12->maxUpdateAfterBindDescriptorsInAllPools = 1048576;
            v12->shaderUniformBufferArrayNonUniformIndexingNative = VK_FALSE;
            v12->shaderSampledImageArrayNonUniformIndexingNative = VK_FALSE;
            v12->shaderStorageBufferArrayNonUniformIndexingNative = VK_FALSE;
            v12->shaderStorageImageArrayNonUniformIndexingNative = VK_FALSE;
            v12->shaderInputAttachmentArrayNonUniformIndexingNative = VK_FALSE;
            v12->robustBufferAccessUpdateAfterBind = VK_FALSE;
            v12->quadDivergentImplicitLod = VK_FALSE;
            v12->maxPerStageDescriptorUpdateAfterBindSamplers = 128;
            v12->maxPerStageDescriptorUpdateAfterBindUniformBuffers = 16;
            v12->maxPerStageDescriptorUpdateAfterBindStorageBuffers = 16;
            v12->maxPerStageDescriptorUpdateAfterBindSampledImages = 128;
            v12->maxPerStageDescriptorUpdateAfterBindStorageImages = 16;
            v12->maxPerStageDescriptorUpdateAfterBindInputAttachments = 8;
            v12->maxPerStageUpdateAfterBindResources = 128;
            v12->maxDescriptorSetUpdateAfterBindSamplers = 128;
            v12->maxDescriptorSetUpdateAfterBindUniformBuffers = 16;
            v12->maxDescriptorSetUpdateAfterBindUniformBuffersDynamic = 8;
            v12->maxDescriptorSetUpdateAfterBindStorageBuffers = 16;
            v12->maxDescriptorSetUpdateAfterBindStorageBuffersDynamic = 4;
            v12->maxDescriptorSetUpdateAfterBindSampledImages = 128;
            v12->maxDescriptorSetUpdateAfterBindStorageImages = 16;
            v12->maxDescriptorSetUpdateAfterBindInputAttachments = 8;
            LOGI("Intercepted VkPhysicalDeviceVulkan12Properties: limits spoofed");
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

    // Diagnostic probe (M2b): confirm the app enumerates extensions through us.
    LOGI("EnumerateDeviceExtensionProperties: physDev=%p layer=%s mode=%s",
         (const void*)physicalDevice, pLayerName ? pLayerName : "(null)",
         pProperties ? "fill" : "count-query");

    PFN_vkEnumerateDeviceExtensionProperties realEnum = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realEnum = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            malicompat_vendor_proc("vkEnumerateDeviceExtensionProperties"));
    }
    if (!realEnum && g_vendor_vkEnumerateDeviceExtensionProperties) {
        realEnum = g_vendor_vkEnumerateDeviceExtensionProperties;
    }

    if (!pProperties) {
        uint32_t count = 0;
        VkResult res = VK_SUCCESS;
        if (realEnum) {
            res = realEnum(physicalDevice, pLayerName, &count, nullptr);
        }
        if (res == VK_SUCCESS || res == VK_INCOMPLETE) {
            // M3: reserve room for both emulated extensions (indexing + swapchain).
            *pPropertyCount = count + 2;
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

    bool found_indexing = false;
    bool found_swapchain = false;
    for (uint32_t i = 0; i < real_count; ++i) {
        if (std::strcmp(pProperties[i].extensionName, "VK_EXT_descriptor_indexing") == 0) {
            found_indexing = true;
        } else if (std::strcmp(pProperties[i].extensionName, "VK_KHR_swapchain") == 0) {
            found_swapchain = true;
        }
    }
    LOGI("EnumerateDeviceExtensionProperties fill: res=%d vendor_count=%u capacity=%u indexing=%d swapchain=%d",
         res, real_count, capacity, (int)found_indexing, (int)found_swapchain);
    for (uint32_t i = 0; i < real_count && i < 3; ++i) {
        LOGI("EnumerateDeviceExtensionProperties vendor[%u] = '%s'", i, pProperties[i].extensionName);
    }

    if (!found_indexing && real_count < capacity) {
        std::strncpy(pProperties[real_count].extensionName, "VK_EXT_descriptor_indexing", VK_MAX_EXTENSION_NAME_SIZE);
        pProperties[real_count].specVersion = 2;
        real_count++;
        LOGI("Appended emulated VK_EXT_descriptor_indexing to device extensions list");
    }
    if (!found_swapchain && real_count < capacity) {
        std::strncpy(pProperties[real_count].extensionName, "VK_KHR_swapchain", VK_MAX_EXTENSION_NAME_SIZE);
        pProperties[real_count].specVersion = 68;
        real_count++;
        LOGI("Appended emulated VK_KHR_swapchain to device extensions list (served by system WSI)");
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

    // Diagnostic probe (M2): dump the full device request. Bounded: runs at
    // most once per vkCreateDevice call.
    LOGI("vkCreateDevice request: extCount=%u queueInfos=%u features=%p pNext=%p",
         pCreateInfo->enabledExtensionCount, pCreateInfo->queueCreateInfoCount,
         (const void*)pCreateInfo->pEnabledFeatures, pCreateInfo->pNext);
    if (pCreateInfo->ppEnabledExtensionNames) {
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
            const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
            LOGI("vkCreateDevice request ext[%u] = '%s'", i, ext ? ext : "(null)");
        }
    }
    {
        const auto* node = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
        uint32_t depth = 0;
        while (node && depth < 16) {
            LOGI("vkCreateDevice request pNext[%u] sType=%d", depth, (int)node->sType);
            node = node->pNext;
            ++depth;
        }
    }

    // Gracefully reject null extension array when extension count > 0
    if (pCreateInfo->enabledExtensionCount > 0 && !pCreateInfo->ppEnabledExtensionNames) {
        LOGE("vkCreateDevice: enabledExtensionCount > 0 but ppEnabledExtensionNames is NULL");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkDeviceCreateInfo modifiedCreateInfo = *pCreateInfo;

    // M3: determine whether the vendor ICD really lacks VK_KHR_swapchain (we
    // report it as emulated and serve WSI through the system loader). Fresh
    // query per call; device creation is rare. Defaults to passthrough.
    bool vendor_lacks_swapchain = false;
    {
        PFN_vkEnumerateDeviceExtensionProperties vendorEnum = nullptr;
        if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
            vendorEnum = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
                malicompat_vendor_proc("vkEnumerateDeviceExtensionProperties"));
        }
        if (!vendorEnum && g_vendor_vkEnumerateDeviceExtensionProperties) {
            vendorEnum = g_vendor_vkEnumerateDeviceExtensionProperties;
        }
        if (vendorEnum) {
            uint32_t vn = 0;
            if (vendorEnum(physicalDevice, nullptr, &vn, nullptr) == VK_SUCCESS && vn > 0 && vn <= 512) {
                std::vector<VkExtensionProperties> vexts(vn);
                uint32_t fill = vn;
                if (vendorEnum(physicalDevice, nullptr, &fill, vexts.data()) == VK_SUCCESS) {
                    vendor_lacks_swapchain = true;
                    for (uint32_t k = 0; k < fill; ++k) {
                        if (std::strcmp(vexts[k].extensionName, "VK_KHR_swapchain") == 0) {
                            vendor_lacks_swapchain = false;
                            break;
                        }
                    }
                }
            }
        }
    }
    LOGI("CreateDevice: vendor VK_KHR_swapchain %s", vendor_lacks_swapchain ? "MISSING (emulate+strip)" : "present (passthrough)");

    // 1. Filter out VK_EXT_descriptor_indexing from enabled extensions for vendor driver (F4)
    std::vector<const char*> filteredExtensions;
    if (pCreateInfo->ppEnabledExtensionNames) {
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
            const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
            if (ext && std::strcmp(ext, "VK_EXT_descriptor_indexing") == 0) {
                LOGI("Filtering out unsupported VK_EXT_descriptor_indexing from vendor vkCreateDevice");
                continue;
            }
            if (ext && vendor_lacks_swapchain && std::strcmp(ext, "VK_KHR_swapchain") == 0) {
                LOGI("Filtering out emulated VK_KHR_swapchain from vendor vkCreateDevice (system WSI serves it)");
                continue;
            }
            if (ext) {
                filteredExtensions.push_back(ext);
            }
        }
    }
    modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(filteredExtensions.size());
    modifiedCreateInfo.ppEnabledExtensionNames = filteredExtensions.data();

    // 2. Sanitize enabled features: clear flags unsupported by Mali-G71 hardware (F4)
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

    // 3. Unlink indexing feature structs from pNext and sanitize features (F4)
    // Never mutate caller memory or read-only pCreateInfo->pNext directly with const_cast.
    // Build local sanitized copies.
    auto shouldStrip = [](VkStructureType sType) -> bool {
        return sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES ||
               sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
    };

    constexpr uint32_t kMaxPNextDepth = 64;

    VkPhysicalDeviceFeatures2 localFeatures2{};

    uint32_t rootDepth = 0;
    const auto* cur = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
    while (cur && rootDepth++ < kMaxPNextDepth && shouldStrip(cur->sType)) {
        LOGI("Unlinking indexing feature struct (sType=%d) from vendor vkCreateDevice pNext", cur->sType);
        cur = cur->pNext;
    }

    if (!cur || rootDepth >= kMaxPNextDepth) {
        modifiedCreateInfo.pNext = nullptr;
    } else if (cur->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
        localFeatures2 = *reinterpret_cast<const VkPhysicalDeviceFeatures2*>(cur);
        localFeatures2.features.vertexPipelineStoresAndAtomics = VK_FALSE;
        localFeatures2.features.fillModeNonSolid = VK_FALSE;
        LOGI("Sanitized unsupported vertexPipelineStoresAndAtomics / fillModeNonSolid in local VkPhysicalDeviceFeatures2 copy");

        // Sanitize nested pNext in localFeatures2
        uint32_t f2_depth = 0;
        const auto* f2_cur = reinterpret_cast<const VkBaseInStructure*>(localFeatures2.pNext);
        while (f2_cur && f2_depth++ < kMaxPNextDepth && shouldStrip(f2_cur->sType)) {
            LOGI("Unlinking indexing feature struct (sType=%d) from local VkPhysicalDeviceFeatures2 copy pNext", f2_cur->sType);
            f2_cur = f2_cur->pNext;
        }

        if (!f2_cur || f2_depth >= kMaxPNextDepth) {
            localFeatures2.pNext = nullptr;
        } else {
            localFeatures2.pNext = const_cast<void*>(static_cast<const void*>(f2_cur));
        }

        modifiedCreateInfo.pNext = &localFeatures2;
    } else {
        modifiedCreateInfo.pNext = const_cast<void*>(static_cast<const void*>(cur));
    }

    PFN_vkCreateDevice realCreate = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realCreate = reinterpret_cast<PFN_vkCreateDevice>(
            malicompat_vendor_proc("vkCreateDevice"));
    }
    if (!realCreate && g_vendor_vkCreateDevice) {
        realCreate = g_vendor_vkCreateDevice;
    }

    if (!realCreate) {
        LOGE("Failed to resolve real vkCreateDevice");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = realCreate(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);
    LOGI("Vendor vkCreateDevice returned: %d (device=%p)", res, pDevice ? *pDevice : nullptr);

    // Initialize dummy fallback resources on the newly created device (F8)
    if (res == VK_SUCCESS && pDevice && *pDevice) {
        malicompat::VulkanDescriptorShim::Instance().InitializeDeviceResources(*pDevice, physicalDevice);
    }

    return res;
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator) {
    InitRealVulkan();
    malicompat::VulkanDescriptorShim::Instance().CleanupDeviceResources(device);

    PFN_vkDestroyDevice realDestroy = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realDestroy = reinterpret_cast<PFN_vkDestroyDevice>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkDestroyDevice"));
    }
    if (!realDestroy && g_vendor_vkDestroyDevice) {
        realDestroy = g_vendor_vkDestroyDevice;
    }
    if (realDestroy) {
        realDestroy(device, pAllocator);
    }
}

// Diagnostic probe (M2b): trace physical-device enumeration; forward unchanged.
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkEnumeratePhysicalDevices(
    VkInstance instance,
    uint32_t* pPhysicalDeviceCount,
    VkPhysicalDevice* pPhysicalDevices) {
    InitRealVulkan();
    LOGI("EnumeratePhysicalDevices: instance=%p mode=%s", (const void*)instance,
         pPhysicalDevices ? "fill" : "count-query");
    PFN_vkEnumeratePhysicalDevices realEnum = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realEnum = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            g_vendor_device->GetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    }
    if (!realEnum && g_vendor_vulkan) {
        realEnum = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            dlsym(g_vendor_vulkan, "vkEnumeratePhysicalDevices"));
    }
    if (!realEnum) {
        LOGE("EnumeratePhysicalDevices: no vendor implementation");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult res = realEnum(instance, pPhysicalDeviceCount, pPhysicalDevices);
    LOGI("EnumeratePhysicalDevices result: %d count=%u", res,
         pPhysicalDeviceCount ? *pPhysicalDeviceCount : 0);
    return res;
}

MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    InitRealVulkan();
    if (!pName) return nullptr;

    // Diagnostic probe (M2/M2b): trace whether the loader/app ever resolves
    // the device-creation path through us. Bounded to a small watched set to
    // avoid log flooding.
    {
        static const char* kWatchedProcNames[] = {
            "vkCreateDevice", "vkEnumeratePhysicalDevices",
            "vkEnumerateDeviceExtensionProperties",
            "vkGetPhysicalDeviceProperties", "vkGetPhysicalDeviceProperties2",
            "vkGetPhysicalDeviceFeatures2", "vkGetPhysicalDeviceFeatures2KHR",
            "vkGetPhysicalDeviceQueueFamilyProperties",
            "vkGetPhysicalDeviceMemoryProperties", nullptr
        };
        for (const char** w = kWatchedProcNames; *w; ++w) {
            if (std::strcmp(pName, *w) == 0) {
                LOGI("GetInstanceProcAddr query: '%s' (instance=%p)", pName, (const void*)instance);
                break;
            }
        }
    }

    // Direct entry point interception
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetInstanceProcAddr);
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkCreateDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDevice);
    if (std::strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkDestroyDevice);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceFeatures);
    if (std::strcmp(pName, "vkGetPhysicalDeviceFeatures2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceFeatures2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceFeatures2);
    if (std::strcmp(pName, "vkGetPhysicalDeviceProperties2") == 0 ||
        std::strcmp(pName, "vkGetPhysicalDeviceProperties2KHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceProperties2);
    if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkEnumerateDeviceExtensionProperties);
    if (std::strcmp(pName, "vkEnumeratePhysicalDevices") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkEnumeratePhysicalDevices);

    // Descriptor Interception
    if (std::strcmp(pName, "vkCreateDescriptorPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorPool);
    if (std::strcmp(pName, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorSetLayout);
    if (std::strcmp(pName, "vkAllocateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkAllocateDescriptorSets);
    if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkUpdateDescriptorSets);
    if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdBindDescriptorSets);

    // Query Interception (F6)
    if (std::strcmp(pName, "vkCreateQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateQueryPool);
    if (std::strcmp(pName, "vkDestroyQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkDestroyQueryPool);
    if (std::strcmp(pName, "vkCmdResetQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdResetQueryPool);
    if (std::strcmp(pName, "vkCmdBeginQuery") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdBeginQuery);
    if (std::strcmp(pName, "vkCmdEndQuery") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdEndQuery);
    if (std::strcmp(pName, "vkCmdCopyQueryPoolResults") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdCopyQueryPoolResults);
    if (std::strcmp(pName, "vkGetQueryPoolResults") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetQueryPoolResults);
    if (std::strcmp(pName, "vkResetQueryPool") == 0 ||
        std::strcmp(pName, "vkResetQueryPoolEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkResetQueryPool);
    if (std::strcmp(pName, "vkCmdWriteTimestamp") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdWriteTimestamp);

    // Presentation, Execution & Pipeline Interception (routed prior to loader WSI check)
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkQueuePresentKHR);
    if (std::strcmp(pName, "vkQueueSubmit") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkQueueSubmit);
    if (std::strcmp(pName, "vkCreateShaderModule") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateShaderModule);
    if (std::strcmp(pName, "vkCreatePipelineLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreatePipelineLayout);
    if (std::strcmp(pName, "vkCreatePipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreatePipelineCache);
    if (std::strcmp(pName, "vkDestroyPipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkDestroyPipelineCache);
    if (std::strcmp(pName, "vkGetPipelineCacheData") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPipelineCacheData);
    if (std::strcmp(pName, "vkMergePipelineCaches") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkMergePipelineCaches);

    // WSI / Surface Forwarding (F1): Route to Android system loader
    if (malicompat_is_wsi_symbol(pName)) {
        PFN_vkVoidFunction pfn = nullptr;
        if (g_system_vkGetInstanceProcAddr) {
            pfn = g_system_vkGetInstanceProcAddr(instance, pName);
        }
        if (!pfn && g_system_vulkan) {
            pfn = reinterpret_cast<PFN_vkVoidFunction>(dlsym(g_system_vulkan, pName));
        }
        if (pfn) {
            LOGI("GetInstanceProcAddr (WSI forwarding to system loader): '%s' -> %p", pName, pfn);
            return pfn;
        }
        // Fallback to our own exported WSI wrappers
        if (std::strcmp(pName, "vkCreateAndroidSurfaceKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateAndroidSurfaceKHR);
        if (std::strcmp(pName, "vkDestroySurfaceKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkDestroySurfaceKHR);
        if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
        if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceFormatsKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceSurfaceFormatsKHR);
        if (std::strcmp(pName, "vkGetPhysicalDeviceSurfacePresentModesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceSurfacePresentModesKHR);
        if (std::strcmp(pName, "vkGetPhysicalDeviceSurfaceSupportKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceSurfaceSupportKHR);
        if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateSwapchainKHR);
        if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkDestroySwapchainKHR);
        if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetSwapchainImagesKHR);
        if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkAcquireNextImageKHR);
        if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkQueuePresentKHR);
    }

    // Default: forward hardware rasterization / compute calls to vendor Mali driver
    PFN_vkVoidFunction pfn = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        pfn = g_vendor_device->GetInstanceProcAddr(instance, pName);
    }
    if (!pfn && g_vendor_vulkan) {
        pfn = reinterpret_cast<PFN_vkVoidFunction>(dlsym(g_vendor_vulkan, pName));
    }
    return pfn;
}

MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    InitRealVulkan();
    if (!pName) return nullptr;

    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetDeviceProcAddr);
    if (std::strcmp(pName, "vkDestroyDevice") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkDestroyDevice);

    // Descriptor Interception
    if (std::strcmp(pName, "vkCreateDescriptorPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorPool);
    if (std::strcmp(pName, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorSetLayout);
    if (std::strcmp(pName, "vkAllocateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkAllocateDescriptorSets);
    if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkUpdateDescriptorSets);
    if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdBindDescriptorSets);

    // Query Interception (F6)
    if (std::strcmp(pName, "vkCreateQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateQueryPool);
    if (std::strcmp(pName, "vkDestroyQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkDestroyQueryPool);
    if (std::strcmp(pName, "vkCmdResetQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdResetQueryPool);
    if (std::strcmp(pName, "vkCmdBeginQuery") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdBeginQuery);
    if (std::strcmp(pName, "vkCmdEndQuery") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdEndQuery);
    if (std::strcmp(pName, "vkCmdCopyQueryPoolResults") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdCopyQueryPoolResults);
    if (std::strcmp(pName, "vkGetQueryPoolResults") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetQueryPoolResults);
    if (std::strcmp(pName, "vkResetQueryPool") == 0 ||
        std::strcmp(pName, "vkResetQueryPoolEXT") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkResetQueryPool);
    if (std::strcmp(pName, "vkCmdWriteTimestamp") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdWriteTimestamp);

    // Presentation, Execution & Pipeline Interception (routed prior to loader WSI check)
    if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkQueuePresentKHR);
    if (std::strcmp(pName, "vkQueueSubmit") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkQueueSubmit);
    if (std::strcmp(pName, "vkCreateShaderModule") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateShaderModule);
    if (std::strcmp(pName, "vkCreatePipelineLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreatePipelineLayout);
    if (std::strcmp(pName, "vkCreatePipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreatePipelineCache);
    if (std::strcmp(pName, "vkDestroyPipelineCache") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkDestroyPipelineCache);
    if (std::strcmp(pName, "vkGetPipelineCacheData") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPipelineCacheData);
    if (std::strcmp(pName, "vkMergePipelineCaches") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkMergePipelineCaches);

    // WSI / Swapchain Functions (F1)
    if (malicompat_is_wsi_symbol(pName)) {
        PFN_vkVoidFunction pfn = nullptr;
        if (g_system_vulkan) {
            pfn = reinterpret_cast<PFN_vkVoidFunction>(dlsym(g_system_vulkan, pName));
        }
        if (pfn) {
            LOGI("GetDeviceProcAddr (WSI forwarding to system loader): '%s' -> %p", pName, pfn);
            return pfn;
        }
        if (std::strcmp(pName, "vkCreateSwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateSwapchainKHR);
        if (std::strcmp(pName, "vkDestroySwapchainKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkDestroySwapchainKHR);
        if (std::strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetSwapchainImagesKHR);
        if (std::strcmp(pName, "vkAcquireNextImageKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkAcquireNextImageKHR);
        if (std::strcmp(pName, "vkQueuePresentKHR") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkQueuePresentKHR);
    }

    // Default: forward to vendor driver. M7: resolve via the live instance
    // (NULL gives crippled results on this driver); without it the SDK fails
    // to load device functions and CreateIfSupported returns null.
    PFN_vkVoidFunction pfn = malicompat_vendor_proc(pName);
    if (!pfn && g_vendor_vulkan) {
        pfn = reinterpret_cast<PFN_vkVoidFunction>(dlsym(g_vendor_vulkan, pName));
    }
    return pfn;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateDescriptorPool(
    VkDevice device,
    const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorPool* pDescriptorPool) {
    InitRealVulkan();
    PFN_vkCreateDescriptorPool realPfn = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realPfn = reinterpret_cast<PFN_vkCreateDescriptorPool>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDescriptorPool"));
    }
    if (!realPfn && g_vendor_vkCreateDescriptorPool) {
        realPfn = g_vendor_vkCreateDescriptorPool;
    }
    if (!realPfn) {
        LOGE("Failed to resolve vendor vkCreateDescriptorPool");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorPool(
        device, pCreateInfo, pAllocator, pDescriptorPool, realPfn);
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateDescriptorSetLayout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout) {
    InitRealVulkan();
    return malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorSetLayout(
        device, pCreateInfo, pAllocator, pSetLayout, g_vendor_vkCreateDescriptorSetLayout);
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets) {
    InitRealVulkan();
    return malicompat::VulkanDescriptorShim::Instance().InterceptAllocateDescriptorSets(
        device, pAllocateInfo, pDescriptorSets, g_vendor_vkAllocateDescriptorSets);
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkUpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies) {
    InitRealVulkan();
    malicompat::VulkanDescriptorShim::Instance().InterceptUpdateDescriptorSets(
        device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies, g_vendor_vkUpdateDescriptorSets);
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdBindDescriptorSets(
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
        commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets, g_vendor_vkCmdBindDescriptorSets);
}

// Query Interception: Safe Virtual Query Pool Handler (F6)
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateQueryPool(
    VkDevice device,
    const VkQueryPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkQueryPool* pQueryPool) {
    InitRealVulkan();
    if (!pCreateInfo || !pQueryPool) return VK_ERROR_INITIALIZATION_FAILED;

    // Neuter occlusion queries on ARM Mali to prevent driver resets
    if (pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION) {
        LOGI("vkCreateQueryPool: created virtual occlusion query pool (count=%u)", pCreateInfo->queryCount);
        *pQueryPool = VIRTUAL_QUERY_POOL_HANDLE;
        return VK_SUCCESS;
    }

    if (g_vendor_vkCreateQueryPool) {
        return g_vendor_vkCreateQueryPool(device, pCreateInfo, pAllocator, pQueryPool);
    }
    LOGW("vkCreateQueryPool: vendor vkCreateQueryPool null, fallback to virtual pool");
    *pQueryPool = VIRTUAL_QUERY_POOL_HANDLE;
    return VK_SUCCESS;
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroyQueryPool(
    VkDevice device,
    VkQueryPool queryPool,
    const VkAllocationCallbacks* pAllocator) {
    InitRealVulkan();
    if (malicompat_is_virtual_query_pool(queryPool)) {
        LOGI("vkDestroyQueryPool: destroyed virtual query pool %p (no-op)", queryPool);
        return;
    }
    if (g_vendor_vkDestroyQueryPool) {
        g_vendor_vkDestroyQueryPool(device, queryPool, pAllocator);
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdResetQueryPool(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount) {
    InitRealVulkan();
    if (malicompat_is_virtual_query_pool(queryPool)) {
        return; // Safe no-op
    }
    if (g_vendor_vkCmdResetQueryPool) {
        g_vendor_vkCmdResetQueryPool(commandBuffer, queryPool, firstQuery, queryCount);
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdBeginQuery(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t query,
    VkQueryControlFlags flags) {
    InitRealVulkan();
    if (malicompat_is_virtual_query_pool(queryPool)) {
        return; // Safe no-op
    }
    if (g_vendor_vkCmdBeginQuery) {
        g_vendor_vkCmdBeginQuery(commandBuffer, queryPool, query, flags);
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdEndQuery(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t query) {
    InitRealVulkan();
    if (malicompat_is_virtual_query_pool(queryPool)) {
        return; // Safe no-op
    }
    if (g_vendor_vkCmdEndQuery) {
        g_vendor_vkCmdEndQuery(commandBuffer, queryPool, query);
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdCopyQueryPoolResults(
    VkCommandBuffer commandBuffer,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount,
    VkBuffer dstBuffer,
    VkDeviceSize dstOffset,
    VkDeviceSize stride,
    VkQueryResultFlags flags) {
    InitRealVulkan();
    if (malicompat_is_virtual_query_pool(queryPool)) {
        if (dstBuffer != VK_NULL_HANDLE && g_vendor_vkCmdFillBuffer) {
            VkDeviceSize totalSize = (stride > 0) ? (queryCount * stride) : (queryCount * sizeof(uint32_t));
            if ((totalSize % 4 == 0) && (dstOffset % 4 == 0) && totalSize > 0) {
                g_vendor_vkCmdFillBuffer(commandBuffer, dstBuffer, dstOffset, totalSize, 0);
                return;
            }
        }
        return; // Safe no-op
    }
    if (g_vendor_vkCmdCopyQueryPoolResults) {
        g_vendor_vkCmdCopyQueryPoolResults(commandBuffer, queryPool, firstQuery, queryCount, dstBuffer, dstOffset, stride, flags);
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetQueryPoolResults(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount,
    size_t dataSize,
    void* pData,
    VkDeviceSize stride,
    VkQueryResultFlags flags) {
    InitRealVulkan();
    if (malicompat_is_virtual_query_pool(queryPool)) {
        if (!pData || dataSize == 0) {
            return VK_SUCCESS;
        }
        std::memset(pData, 0, dataSize);

        if (flags & VK_QUERY_RESULT_WITH_AVAILABILITY_BIT) {
            uint8_t* pByteData = reinterpret_cast<uint8_t*>(pData);
            bool is64Bit = (flags & VK_QUERY_RESULT_64_BIT) != 0;
            size_t effectiveStride = static_cast<size_t>(stride);
            if (effectiveStride == 0) {
                effectiveStride = is64Bit ? (sizeof(uint64_t) * 2) : (sizeof(uint32_t) * 2);
            }

            for (uint32_t i = 0; i < queryCount; ++i) {
                size_t queryOffset = i * effectiveStride;
                if (is64Bit) {
                    size_t availOffset = queryOffset + sizeof(uint64_t);
                    if (availOffset + sizeof(uint64_t) <= dataSize) {
                        *reinterpret_cast<uint64_t*>(pByteData + availOffset) = 1ULL;
                    }
                } else {
                    size_t availOffset = queryOffset + sizeof(uint32_t);
                    if (availOffset + sizeof(uint32_t) <= dataSize) {
                        *reinterpret_cast<uint32_t*>(pByteData + availOffset) = 1U;
                    }
                }
            }
        }
        return VK_SUCCESS;
    }
    if (g_vendor_vkGetQueryPoolResults) {
        return g_vendor_vkGetQueryPoolResults(device, queryPool, firstQuery, queryCount, dataSize, pData, stride, flags);
    }
    return VK_SUCCESS;
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkResetQueryPool(
    VkDevice device,
    VkQueryPool queryPool,
    uint32_t firstQuery,
    uint32_t queryCount) {
    InitRealVulkan();
    if (malicompat_is_virtual_query_pool(queryPool)) {
        return; // Safe no-op
    }
    if (g_vendor_vkResetQueryPool) {
        g_vendor_vkResetQueryPool(device, queryPool, firstQuery, queryCount);
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdWriteTimestamp(
    VkCommandBuffer commandBuffer,
    VkPipelineStageFlagBits pipelineStage,
    VkQueryPool queryPool,
    uint32_t query) {
    InitRealVulkan();
    if (malicompat_is_virtual_query_pool(queryPool)) {
        return; // Safe no-op
    }
    if (g_vendor_vkCmdWriteTimestamp) {
        g_vendor_vkCmdWriteTimestamp(commandBuffer, pipelineStage, queryPool, query);
    }
}

// WSI / Surface Forwarding Implementations (F1)
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateAndroidSurfaceKHR(
    VkInstance instance,
    const VkAndroidSurfaceCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSurfaceKHR* pSurface) {
    InitRealVulkan();
    if (g_system_vkCreateAndroidSurfaceKHR) {
        return g_system_vkCreateAndroidSurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkCreateAndroidSurfaceKHR>(dlsym(g_system_vulkan, "vkCreateAndroidSurfaceKHR"));
        if (pfn) return pfn(instance, pCreateInfo, pAllocator, pSurface);
    }
    LOGE("vkCreateAndroidSurfaceKHR: system loader /system/lib64/libvulkan.so failed to resolve");
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroySurfaceKHR(
    VkInstance instance,
    VkSurfaceKHR surface,
    const VkAllocationCallbacks* pAllocator) {
    InitRealVulkan();
    if (g_system_vkDestroySurfaceKHR) {
        g_system_vkDestroySurfaceKHR(instance, surface, pAllocator);
        return;
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkDestroySurfaceKHR>(dlsym(g_system_vulkan, "vkDestroySurfaceKHR"));
        if (pfn) pfn(instance, surface, pAllocator);
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    VkSurfaceCapabilitiesKHR* pSurfaceCapabilities) {
    InitRealVulkan();
    if (g_system_vkGetPhysicalDeviceSurfaceCapabilitiesKHR) {
        return g_system_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, pSurfaceCapabilities);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR"));
        if (pfn) return pfn(physicalDevice, surface, pSurfaceCapabilities);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfaceCapabilities2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    VkSurfaceCapabilities2KHR* pSurfaceCapabilities) {
    InitRealVulkan();
    if (g_system_vkGetPhysicalDeviceSurfaceCapabilities2KHR) {
        return g_system_vkGetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfaceCapabilities2KHR"));
        if (pfn) return pfn(physicalDevice, pSurfaceInfo, pSurfaceCapabilities);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormatKHR* pSurfaceFormats) {
    InitRealVulkan();
    if (g_system_vkGetPhysicalDeviceSurfaceFormatsKHR) {
        return g_system_vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormatsKHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfaceFormatsKHR"));
        if (pfn) return pfn(physicalDevice, surface, pSurfaceFormatCount, pSurfaceFormats);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfaceFormats2KHR(
    VkPhysicalDevice physicalDevice,
    const VkPhysicalDeviceSurfaceInfo2KHR* pSurfaceInfo,
    uint32_t* pSurfaceFormatCount,
    VkSurfaceFormat2KHR* pSurfaceFormats) {
    InitRealVulkan();
    if (g_system_vkGetPhysicalDeviceSurfaceFormats2KHR) {
        return g_system_vkGetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceFormats2KHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfaceFormats2KHR"));
        if (pfn) return pfn(physicalDevice, pSurfaceInfo, pSurfaceFormatCount, pSurfaceFormats);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice physicalDevice,
    VkSurfaceKHR surface,
    uint32_t* pPresentModeCount,
    VkPresentModeKHR* pPresentModes) {
    InitRealVulkan();
    if (g_system_vkGetPhysicalDeviceSurfacePresentModesKHR) {
        return g_system_vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, pPresentModeCount, pPresentModes);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfacePresentModesKHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfacePresentModesKHR"));
        if (pfn) return pfn(physicalDevice, surface, pPresentModeCount, pPresentModes);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice physicalDevice,
    uint32_t queueFamilyIndex,
    VkSurfaceKHR surface,
    VkBool32* pSupported) {
    InitRealVulkan();
    if (g_system_vkGetPhysicalDeviceSurfaceSupportKHR) {
        return g_system_vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, queueFamilyIndex, surface, pSupported);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(dlsym(g_system_vulkan, "vkGetPhysicalDeviceSurfaceSupportKHR"));
        if (pfn) return pfn(physicalDevice, queueFamilyIndex, surface, pSupported);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain) {
    InitRealVulkan();
    if (g_system_vkCreateSwapchainKHR) {
        return g_system_vkCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkCreateSwapchainKHR>(dlsym(g_system_vulkan, "vkCreateSwapchainKHR"));
        if (pfn) return pfn(device, pCreateInfo, pAllocator, pSwapchain);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator) {
    InitRealVulkan();
    if (g_system_vkDestroySwapchainKHR) {
        g_system_vkDestroySwapchainKHR(device, swapchain, pAllocator);
        return;
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkDestroySwapchainKHR>(dlsym(g_system_vulkan, "vkDestroySwapchainKHR"));
        if (pfn) pfn(device, swapchain, pAllocator);
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages) {
    InitRealVulkan();
    if (g_system_vkGetSwapchainImagesKHR) {
        return g_system_vkGetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(dlsym(g_system_vulkan, "vkGetSwapchainImagesKHR"));
        if (pfn) return pfn(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAcquireNextImageKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint64_t timeout,
    VkSemaphore semaphore,
    VkFence fence,
    uint32_t* pImageIndex) {
    InitRealVulkan();
    if (g_system_vkAcquireNextImageKHR) {
        return g_system_vkAcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkAcquireNextImageKHR>(dlsym(g_system_vulkan, "vkAcquireNextImageKHR"));
        if (pfn) return pfn(device, swapchain, timeout, semaphore, fence, pImageIndex);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAcquireNextImage2KHR(
    VkDevice device,
    const VkAcquireNextImageInfoKHR* pAcquireInfo,
    uint32_t* pImageIndex) {
    InitRealVulkan();
    if (g_system_vkAcquireNextImage2KHR) {
        return g_system_vkAcquireNextImage2KHR(device, pAcquireInfo, pImageIndex);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkAcquireNextImage2KHR>(dlsym(g_system_vulkan, "vkAcquireNextImage2KHR"));
        if (pfn) return pfn(device, pAcquireInfo, pImageIndex);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo) {
    InitRealVulkan();

    // 1. Cross-Driver alignment: Because VkQueue is allocated on the vendor Mali device (g_vendor_device),
    // passing it to the Android system loader causes VK_ERROR_DEVICE_LOST. First route presentation
    // through the vendor driver/HAL where VkQueue was allocated.
    PFN_vkQueuePresentKHR vendorPresent = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        vendorPresent = reinterpret_cast<PFN_vkQueuePresentKHR>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkQueuePresentKHR"));
    }
    if (!vendorPresent && g_vendor_vkQueuePresentKHR) {
        vendorPresent = g_vendor_vkQueuePresentKHR;
    }
    if (!vendorPresent && g_vendor_vulkan) {
        vendorPresent = reinterpret_cast<PFN_vkQueuePresentKHR>(dlsym(g_vendor_vulkan, "vkQueuePresentKHR"));
    }
    if (vendorPresent) {
        return vendorPresent(queue, pPresentInfo);
    }

    // 2. Fallback to system loader only if vendor driver does not export vkQueuePresentKHR
    if (g_system_vkQueuePresentKHR) {
        return g_system_vkQueuePresentKHR(queue, pPresentInfo);
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkQueuePresentKHR>(dlsym(g_system_vulkan, "vkQueuePresentKHR"));
        if (pfn) return pfn(queue, pPresentInfo);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkQueueSubmit(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence) {
    // Safe empty submission handling: if submitCount is 0 and fence is null, return VK_SUCCESS
    if (submitCount == 0 && fence == VK_NULL_HANDLE) {
        return VK_SUCCESS;
    }

    InitRealVulkan();
    PFN_vkQueueSubmit realSubmit = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkQueueSubmit"));
    }
    if (!realSubmit && g_vendor_vkQueueSubmit) {
        realSubmit = g_vendor_vkQueueSubmit;
    }
    if (!realSubmit && g_vendor_vulkan) {
        realSubmit = reinterpret_cast<PFN_vkQueueSubmit>(dlsym(g_vendor_vulkan, "vkQueueSubmit"));
    }

    if (realSubmit) {
        VkResult res = realSubmit(queue, submitCount, pSubmits, fence);
        if (res != VK_SUCCESS) {
            LOGW("malicompat_vkQueueSubmit: vendor vkQueueSubmit returned %d", static_cast<int>(res));
        }
        return res;
    }

    LOGW("malicompat_vkQueueSubmit: vendor vkQueueSubmit unresolvable, returning VK_SUCCESS fallback");
    return VK_SUCCESS;
}

MALICOMPAT_EXPORT __attribute__((weak)) VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkShaderModule* pShaderModule) {
    if (!pCreateInfo || !pShaderModule) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    InitRealVulkan();
    PFN_vkCreateShaderModule realCreate = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realCreate = reinterpret_cast<PFN_vkCreateShaderModule>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateShaderModule"));
    }
    if (!realCreate && g_vendor_vulkan) {
        realCreate = reinterpret_cast<PFN_vkCreateShaderModule>(dlsym(g_vendor_vulkan, "vkCreateShaderModule"));
    }

    if (realCreate) {
        return realCreate(device, pCreateInfo, pAllocator, pShaderModule);
    }
    *pShaderModule = reinterpret_cast<VkShaderModule>(0x501234);
    return VK_SUCCESS;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreatePipelineLayout(
    VkDevice device,
    const VkPipelineLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineLayout* pPipelineLayout) {
    InitRealVulkan();
    if (!pCreateInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkPipelineLayoutCreateInfo sanitizedInfo = *pCreateInfo;
    if (sanitizedInfo.setLayoutCount > 4) {
        LOGW("malicompat_vkCreatePipelineLayout: clamping setLayoutCount from %u to 4 (Mali-G71 hardware limit)",
             sanitizedInfo.setLayoutCount);
        sanitizedInfo.setLayoutCount = 4;
    }

    PFN_vkCreatePipelineLayout realFunc = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realFunc = reinterpret_cast<PFN_vkCreatePipelineLayout>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreatePipelineLayout"));
    }
    if (!realFunc && g_vendor_vkCreatePipelineLayout) {
        realFunc = g_vendor_vkCreatePipelineLayout;
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<PFN_vkCreatePipelineLayout>(dlsym(g_vendor_vulkan, "vkCreatePipelineLayout"));
    }

    if (realFunc) {
        return realFunc(device, &sanitizedInfo, pAllocator, pPipelineLayout);
    }

    LOGW("malicompat_vkCreatePipelineLayout: vendor vkCreatePipelineLayout unresolvable, returning mock handle");
    if (pPipelineLayout) {
        *pPipelineLayout = reinterpret_cast<VkPipelineLayout>(0x401234);
    }
    return VK_SUCCESS;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreatePipelineCache(
    VkDevice device,
    const VkPipelineCacheCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineCache* pPipelineCache) {
    InitRealVulkan();
    PFN_vkCreatePipelineCache realFunc = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realFunc = reinterpret_cast<PFN_vkCreatePipelineCache>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreatePipelineCache"));
    }
    if (!realFunc && g_vendor_vkCreatePipelineCache) {
        realFunc = g_vendor_vkCreatePipelineCache;
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<PFN_vkCreatePipelineCache>(dlsym(g_vendor_vulkan, "vkCreatePipelineCache"));
    }
    if (realFunc) {
        return realFunc(device, pCreateInfo, pAllocator, pPipelineCache);
    }
    LOGW("malicompat_vkCreatePipelineCache: vendor HAL unresolvable, returning mock cache");
    if (pPipelineCache) {
        *pPipelineCache = reinterpret_cast<VkPipelineCache>(0x301234);
    }
    return VK_SUCCESS;
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroyPipelineCache(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const VkAllocationCallbacks* pAllocator) {
    InitRealVulkan();
    PFN_vkDestroyPipelineCache realFunc = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realFunc = reinterpret_cast<PFN_vkDestroyPipelineCache>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkDestroyPipelineCache"));
    }
    if (!realFunc && g_vendor_vkDestroyPipelineCache) {
        realFunc = g_vendor_vkDestroyPipelineCache;
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<PFN_vkDestroyPipelineCache>(dlsym(g_vendor_vulkan, "vkDestroyPipelineCache"));
    }
    if (realFunc) {
        realFunc(device, pipelineCache, pAllocator);
    }
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetPipelineCacheData(
    VkDevice device,
    VkPipelineCache pipelineCache,
    size_t* pDataSize,
    void* pData) {
    InitRealVulkan();
    PFN_vkGetPipelineCacheData realFunc = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realFunc = reinterpret_cast<PFN_vkGetPipelineCacheData>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkGetPipelineCacheData"));
    }
    if (!realFunc && g_vendor_vkGetPipelineCacheData) {
        realFunc = g_vendor_vkGetPipelineCacheData;
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<PFN_vkGetPipelineCacheData>(dlsym(g_vendor_vulkan, "vkGetPipelineCacheData"));
    }
    if (realFunc) {
        return realFunc(device, pipelineCache, pDataSize, pData);
    }
    if (pDataSize) {
        *pDataSize = 0;
    }
    return VK_SUCCESS;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkMergePipelineCaches(
    VkDevice device,
    VkPipelineCache dstCache,
    uint32_t srcCacheCount,
    const VkPipelineCache* pSrcCaches) {
    InitRealVulkan();
    PFN_vkMergePipelineCaches realFunc = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realFunc = reinterpret_cast<PFN_vkMergePipelineCaches>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkMergePipelineCaches"));
    }
    if (!realFunc && g_vendor_vkMergePipelineCaches) {
        realFunc = g_vendor_vkMergePipelineCaches;
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<PFN_vkMergePipelineCaches>(dlsym(g_vendor_vulkan, "vkMergePipelineCaches"));
    }
    if (realFunc) {
        return realFunc(device, dstCache, srcCacheCount, pSrcCaches);
    }
    return VK_SUCCESS;
}


// Standard exported Vulkan symbols for direct dynamic linker binding
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice d, const VkDescriptorPoolCreateInfo* c, const VkAllocationCallbacks* a, VkDescriptorPool* p) {
    return malicompat_vkCreateDescriptorPool(d, c, a, p);
}
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
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice d, VkQueryPool q, const VkAllocationCallbacks* a) {
    malicompat_vkDestroyQueryPool(d, q, a);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer b, VkQueryPool q, uint32_t f, uint32_t c) {
    malicompat_vkCmdResetQueryPool(b, q, f, c);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginQuery(VkCommandBuffer b, VkQueryPool q, uint32_t query, VkQueryControlFlags f) {
    malicompat_vkCmdBeginQuery(b, q, query, f);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndQuery(VkCommandBuffer b, VkQueryPool q, uint32_t query) {
    malicompat_vkCmdEndQuery(b, q, query);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdCopyQueryPoolResults(VkCommandBuffer b, VkQueryPool q, uint32_t f, uint32_t c, VkBuffer d, VkDeviceSize o, VkDeviceSize s, VkQueryResultFlags fl) {
    malicompat_vkCmdCopyQueryPoolResults(b, q, f, c, d, o, s, fl);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice d, VkQueryPool q, uint32_t f, uint32_t c, size_t ds, void* pd, VkDeviceSize s, VkQueryResultFlags fl) {
    return malicompat_vkGetQueryPoolResults(d, q, f, c, ds, pd, s, fl);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkResetQueryPool(VkDevice d, VkQueryPool q, uint32_t f, uint32_t c) {
    malicompat_vkResetQueryPool(d, q, f, c);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp(VkCommandBuffer b, VkPipelineStageFlagBits p, VkQueryPool q, uint32_t query) {
    malicompat_vkCmdWriteTimestamp(b, p, q, query);
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
    if (g_vendor_vkCreateInstance) {
        return g_vendor_vkCreateInstance(pCreateInfo, pAllocator, pInstance);
    }
    return VK_ERROR_INITIALIZATION_FAILED;
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice d, const VkDeviceCreateInfo* c, const VkAllocationCallbacks* a, VkDevice* dev) {
    return malicompat_vkCreateDevice(d, c, a, dev);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice d, const VkAllocationCallbacks* a) {
    malicompat_vkDestroyDevice(d, a);
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
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2* p) {
    malicompat_vkGetPhysicalDeviceProperties2(d, p);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice d, VkPhysicalDeviceProperties2* p) {
    malicompat_vkGetPhysicalDeviceProperties2(d, p);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice d, const char* l, uint32_t* c, VkExtensionProperties* p) {
    return malicompat_vkEnumerateDeviceExtensionProperties(d, l, c, p);
}

// WSI Exports (F1)
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateAndroidSurfaceKHR(VkInstance i, const VkAndroidSurfaceCreateInfoKHR* c, const VkAllocationCallbacks* a, VkSurfaceKHR* s) {
    return malicompat_vkCreateAndroidSurfaceKHR(i, c, a, s);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks* a) {
    malicompat_vkDestroySurfaceKHR(i, s, a);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR* c) {
    return malicompat_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p, s, c);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice p, VkSurfaceKHR s, uint32_t* c, VkSurfaceFormatKHR* f) {
    return malicompat_vkGetPhysicalDeviceSurfaceFormatsKHR(p, s, c, f);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice p, VkSurfaceKHR s, uint32_t* c, VkPresentModeKHR* m) {
    return malicompat_vkGetPhysicalDeviceSurfacePresentModesKHR(p, s, c, m);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice p, uint32_t q, VkSurfaceKHR s, VkBool32* sup) {
    return malicompat_vkGetPhysicalDeviceSurfaceSupportKHR(p, q, s, sup);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR* c, const VkAllocationCallbacks* a, VkSwapchainKHR* s) {
    return malicompat_vkCreateSwapchainKHR(d, c, a, s);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks* a) {
    malicompat_vkDestroySwapchainKHR(d, s, a);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR s, uint32_t* c, VkImage* i) {
    return malicompat_vkGetSwapchainImagesKHR(d, s, c, i);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore sm, VkFence f, uint32_t* idx) {
    return malicompat_vkAcquireNextImageKHR(d, s, t, sm, f, idx);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR* p) {
    return malicompat_vkQueuePresentKHR(q, p);
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    return malicompat_vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkShaderModule* pShaderModule) {
    return malicompat_vkCreateShaderModule(device, pCreateInfo, pAllocator, pShaderModule);
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice device, const VkPipelineLayoutCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineLayout* pPipelineLayout) {
    return malicompat_vkCreatePipelineLayout(device, pCreateInfo, pAllocator, pPipelineLayout);
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice device, const VkPipelineCacheCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkPipelineCache* pPipelineCache) {
    return malicompat_vkCreatePipelineCache(device, pCreateInfo, pAllocator, pPipelineCache);
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice device, VkPipelineCache pipelineCache, const VkAllocationCallbacks* pAllocator) {
    malicompat_vkDestroyPipelineCache(device, pipelineCache, pAllocator);
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice device, VkPipelineCache pipelineCache, size_t* pDataSize, void* pData) {
    return malicompat_vkGetPipelineCacheData(device, pipelineCache, pDataSize, pData);
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkMergePipelineCaches(VkDevice device, VkPipelineCache dstCache, uint32_t srcCacheCount, const VkPipelineCache* pSrcCaches) {
    return malicompat_vkMergePipelineCaches(device, dstCache, srcCacheCount, pSrcCaches);
}

} // extern "C"
