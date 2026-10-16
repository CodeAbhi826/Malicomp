#include "vulkan_layer_exports.h"
#include "descriptor_shim.h"
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <dlfcn.h>
#include <unordered_map>
#include <atomic>
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

// M16: last vendor VkDevice created (for command-buffer-level proc resolution).
VkDevice g_last_vendor_device = VK_NULL_HANDLE;

// M17f: exact vendor procs saved at query time (identical to what the default
// fallback would return). Wrappers call these; no plain ELF exports.
PFN_vkAcquireImageANDROID g_exact_AcquireImage = nullptr;
PFN_vkQueueSignalReleaseImageANDROID g_exact_SignalRelease = nullptr;

// Phase 4: MaliCompatFlightRecorder. Persistent on-device log (logcat rolls
// over), ZERO behavior change: never alters a returned pointer or VkResult.
// Covers AcquireImage/SignalRelease only via observation, never interception.
namespace flight {
FILE* g_file = nullptr;
std::atomic<uint32_t> g_hal_open{0};
std::atomic<uint32_t> g_create_instance{0};
std::atomic<uint32_t> g_create_device{0};
std::atomic<uint32_t> g_get_queue{0};
std::atomic<uint32_t> g_q0_queues{0};
std::atomic<uint32_t> g_q1_queues{0};
std::atomic<uint32_t> g_swapchain{0};
std::atomic<uint32_t> g_swap_images{0};
std::atomic<uint32_t> g_pipe_gfx{0};
std::atomic<uint32_t> g_pipe_comp{0};
std::atomic<uint32_t> g_submit{0};
std::atomic<uint32_t> g_submit_ok{0};
std::atomic<uint32_t> g_submit_q0{0};
std::atomic<uint32_t> g_submit_q1{0};
std::atomic<uint32_t> g_present_q{0};
std::atomic<uint32_t> g_present_ok{0};
std::atomic<uint32_t> g_summary_n{0};

FILE* file() {
    if (g_file) return g_file;
    const char* cands[] = {
        "/sdcard/Android/data/io.github.nfsmwrecomp/files/malicompat_flight.log",
        "/data/local/tmp/malicompat_flight.log",
        nullptr
    };
    for (int i = 0; cands[i]; ++i) {
        FILE* f = fopen(cands[i], "a");
        if (f) {
            setvbuf(f, nullptr, _IONBF, 0);
            g_file = f;
            return f;
        }
    }
    return nullptr;
}
void line(const char* fmt, ...) {
    FILE* f = file();
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}
void maps_snapshot(const char* why) {
    FILE* f = file();
    if (!f) return;
    FILE* m = fopen("/proc/self/maps", "r");
    if (!m) return;
    fprintf(f, "--- [MAPS] %s ---\n", why);
    char buf[1024];
    while (fgets(buf, sizeof(buf), m)) {
        if (strstr(buf, ".so") || strstr(buf, "memfd")) fputs(buf, f);
    }
    fprintf(f, "--- [MAPS END] ---\n");
    fflush(f);
    fclose(m);
}
void proclog(const char* api, const char* name, PFN_vkVoidFunction pfn, void* caller) {
    static std::atomic<uint32_t> n{0};
    uint32_t k = ++n;
    if (k > 400) return;
    char tb[256];
    Dl_info ci{};
    const char* cn = "?";
    if (caller && dladdr(caller, &ci) && ci.dli_fname) cn = ci.dli_fname;
    Dl_info ti{};
    const char* tn = "?";
    uintptr_t off = 0;
    if (pfn && dladdr(reinterpret_cast<void*>(pfn), &ti) && ti.dli_fname) {
        tn = ti.dli_fname;
        off = (uintptr_t)pfn - (uintptr_t)ti.dli_fbase;
    }
    snprintf(tb, sizeof(tb), "%s", tn);
    char flag[64] = "";
    bool wsi = name && (strstr(name, "Swapchain") || strstr(name, "Present") ||
               strstr(name, "Surface") || strstr(name, "AcquireNextImage"));
    if (wsi && cn && strstr(cn, "memfd") && tn && strncmp(tn, "/system", 7) == 0) {
        snprintf(flag, sizeof(flag), " STATUS=DUAL_LOADER_WSI_MISMATCH");
    }
    line("[SELF-TEST][PROC] #%u api=%s sym=%s caller=%s -> pfn=%p target=%s+%#zx%s",
         k, api, name ? name : "(null)", cn, (void*)pfn, tb, off, flag);
}
void summary() {
    uint32_t n = ++g_summary_n;
    line("# ================ [MALICOMPAT TIMELINE SUMMARY #%u] ================", n);
    line("#01 HAL Open              : hits=%u", g_hal_open.load());
    line("#02 vkCreateInstance      : hits=%u", g_create_instance.load());
    line("#03 vkCreateDevice        : hits=%u", g_create_device.load());
    line("#04 vkGetDeviceQueue      : hits=%u q0=%u q1=%u", g_get_queue.load(), g_q0_queues.load(), g_q1_queues.load());
    line("#05 vkCreateSwapchain     : hits=%u", g_swapchain.load());
    line("#06 vkGetSwapchainImages  : hits=%u", g_swap_images.load());
    line("#07 Pipelines gfx/comp    : %u/%u", g_pipe_gfx.load(), g_pipe_comp.load());
    line("#08 AcquireNextImage      : presenter-path (see game.log C7)");
    line("#09 vkQueueSubmit         : hits=%u ok=%u", g_submit.load(), g_submit_ok.load());
    line("#10 vkQueuePresentKHR     : hits=%u ok=%u", g_present_q.load(), g_present_ok.load());
    line("# --> END SUMMARY #%u", n);
}
}  // namespace flight

// M15: queue->device tracking for submit resolution (defined here, used by
// vkQueueSubmit below and populated by vkGetDeviceQueue further down).
std::mutex g_queue_map_mutex;
std::unordered_map<VkQueue, VkDevice> g_queue_to_device;

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
    // M10: ensure KHR_get_physical_device_properties2 is enabled so the SDK
    // actually issues Properties2/Features2 queries (otherwise their structs
    // stay zeroed and every requirement check fails). Harmless when already
    // present; the extension is core since 1.1.
    VkInstanceCreateInfo modifiedInfo;
    std::vector<const char*> instanceExts;
    const VkInstanceCreateInfo* pInfoForCreate = pCreateInfo;
    if (pCreateInfo) {
        bool hasProps2 = false;
        if (pCreateInfo->ppEnabledExtensionNames) {
            for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; ++i) {
                const char* ext = pCreateInfo->ppEnabledExtensionNames[i];
                if (ext) {
                    instanceExts.push_back(ext);
                    if (std::strcmp(ext, "VK_KHR_get_physical_device_properties2") == 0) {
                        hasProps2 = true;
                    }
                }
            }
        }
        if (!hasProps2) {
            instanceExts.push_back("VK_KHR_get_physical_device_properties2");
            LOGI("CreateInstance: appending VK_KHR_get_physical_device_properties2");
        }
        modifiedInfo = *pCreateInfo;
        modifiedInfo.enabledExtensionCount = static_cast<uint32_t>(instanceExts.size());
        modifiedInfo.ppEnabledExtensionNames = instanceExts.data();
        pInfoForCreate = &modifiedInfo;
    }
    if (g_vendor_device && g_vendor_device->CreateInstance) {
        instanceRes = g_vendor_device->CreateInstance(pInfoForCreate, pAllocator, pInstance);
    } else if (g_vendor_vkCreateInstance) {
        instanceRes = g_vendor_vkCreateInstance(pInfoForCreate, pAllocator, pInstance);
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
    flight::g_hal_open++;
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
        //
        // M18: independentBlend gate. rex::ui::vulkan::VulkanDevice::CreateIfSupported
        // caches this into +0x1CA of the VulkanDevice capability struct; the guest's
        // DibujosVulkan::Inicializar (nfsmw_nativo_dibujos.cpp:2196) reads it first and
        // logs "[nativo] C6: el dispositivo Vulkan no tiene independentBlend: no se dibuja",
        // then returns false -- which leaves the whole guest draw path inert
        // (C6 dibujados=0 pipelines=0 while C2 output pass runs at 60fps).
        // independentBlend is a member of the CORE VkPhysicalDeviceFeatures struct, so it
        // can never be delivered through a Features2 pNext chain; Mali-G71 (Bifrost) does
        // not implement it. The guest uses it purely as a go/no-go capability gate.
        const VkBool32 wasIB = pFeatures->independentBlend;
        pFeatures->independentBlend = VK_TRUE;
        LOGI("M18: independentBlend %d -> VK_TRUE (DibujosVulkan::Inicializar gate)", (int)wasIB);

        // M25: vertexPipelineStoresAndAtomics spoof, for the with_gpu_emulation
        // experiment ONLY.
        //
        // vulkan_device.cpp:121 makes this an unconditional gate:
        //     if (!supported_features.vertexPipelineStoresAndAtomics) return nullptr;
        // This device reports 0, so any run with with_gpu_emulation = true dies at
        // device creation before reaching anything else. Measured on Mali-G71:
        // fragmentStoresAndAtomics = 1, vertexPipelineStoresAndAtomics = 0.
        //
        // The M4 note here used to say spoofing this "lets feature gates pass, but
        // the vendor then rejects vkCreateDevice (routed straight to it, bypassing
        // our sanitizer)". That is no longer true: M20 added a real
        // malicompat_vkCreateDevice that resolves the vendor function, and M24
        // added a pEnabledFeatures sanitiser that clears every bit the driver does
        // not support, by name, before the driver ever sees it. So the spoof can
        // now satisfy the gate without becoming a hard requirement.
        //
        // vkCreateDevice is the only consumer that matters here: XE_UI_VULKAN_FEATURE
        // copies supported_features wholesale into enabled_features, which is what
        // pEnabledFeatures is. The M24 sanitiser strips it back out again.
        const VkBool32 wasVPSA = pFeatures->vertexPipelineStoresAndAtomics;
        if (!wasVPSA) {
            pFeatures->vertexPipelineStoresAndAtomics = VK_TRUE;
            LOGI("M25: vertexPipelineStoresAndAtomics %d -> VK_TRUE (with_gpu_emulation "
                 "experiment; M24 sanitiser will strip it back before the driver)",
                 (int)wasVPSA);
        }
        // M7b: log the full gate-relevant set + whether vendor resolution worked.
        LOGI("vkGetPhysicalDeviceFeatures via=%s independentBlend=%d fragmentStoresAndAtomics=%d vertexPipelineStoresAndAtomics=%d geometryShader=%d fillModeNonSolid=%d samplerAnisotropy=%d",
             realPfn ? "vendor" : "NONE(ZEROS?)",
             (int)pFeatures->independentBlend, (int)pFeatures->fragmentStoresAndAtomics,
             (int)pFeatures->vertexPipelineStoresAndAtomics, (int)pFeatures->geometryShader,
             (int)pFeatures->fillModeNonSolid, (int)pFeatures->samplerAnisotropy);
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

    // M24: record what the driver ACTUALLY reports before we spoof anything.
    // This is the decisive measurement for the "is descriptor indexing really
    // absent, or are we stripping a feature the driver has?" question.
    //
    // The layer currently claims descriptor indexing it does not implement:
    // it appends VK_EXT_descriptor_indexing to the enabled list, forces all six
    // of the guest's required features to VK_TRUE, and then STRIPS
    // VkPhysicalDeviceVulkan12Features (sType 51) back out of vkCreateDevice's
    // pNext so the driver is never told it has them. Meanwhile the guest asks for
    // a 4096-entry SAMPLED_IMAGE heap and a 512-entry SAMPLER heap, against a
    // driver whose real limits are 256 and 128, and every geometry pipeline comes
    // back VK_ERROR_INITIALIZATION_FAILED (cause 53).
    //
    // If the vendor reports descriptorIndexing = VK_TRUE here, we are throwing
    // away a capability we already have and the fix is to stop stripping it.
    // If it reports VK_FALSE, the driver genuinely lacks it and emulated bindless
    // virtualization is the only remaining route.
    {
        const auto* node = reinterpret_cast<const VkBaseOutStructure*>(pFeatures->pNext);
        uint32_t depth = 0;
        while (node && depth < 16) {
            const int st = (int)node->sType;
            if (st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES) {
                const auto* v12 = reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(node);
                LOGI("M24: VENDOR RAW VkPhysicalDeviceVulkan12Features: descriptorIndexing=%d "
                     "runtimeDescriptorArray=%d shaderSampledImageArrayNonUniformIndexing=%d "
                     "descriptorBindingPartiallyBound=%d "
                     "descriptorBindingSampledImageUpdateAfterBind=%d "
                     "descriptorBindingUpdateUnusedWhilePending=%d bufferDeviceAddress=%d "
                     "scalarBlockLayout=%d uniformBufferStandardLayout=%d",
                     (int)v12->descriptorIndexing, (int)v12->runtimeDescriptorArray,
                     (int)v12->shaderSampledImageArrayNonUniformIndexing,
                     (int)v12->descriptorBindingPartiallyBound,
                     (int)v12->descriptorBindingSampledImageUpdateAfterBind,
                     (int)v12->descriptorBindingUpdateUnusedWhilePending,
                     (int)v12->bufferDeviceAddress, (int)v12->scalarBlockLayout,
                     (int)v12->uniformBufferStandardLayout);
            } else if (st == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES) {
                const auto* di =
                    reinterpret_cast<const VkPhysicalDeviceDescriptorIndexingFeatures*>(node);
                LOGI("M24: VENDOR RAW VkPhysicalDeviceDescriptorIndexingFeatures: "
                     "shaderInputAttachmentArrayDynamicIndexing=%d runtimeDescriptorArray=%d "
                     "descriptorBindingPartiallyBound=%d "
                     "descriptorBindingSampledImageUpdateAfterBind=%d "
                     "descriptorBindingUpdateUnusedWhilePending=%d",
                     (int)di->shaderInputAttachmentArrayDynamicIndexing,
                     (int)di->runtimeDescriptorArray, (int)di->descriptorBindingPartiallyBound,
                     (int)di->descriptorBindingSampledImageUpdateAfterBind,
                     (int)di->descriptorBindingUpdateUnusedWhilePending);
            }
            node = reinterpret_cast<const VkBaseOutStructure*>(node->pNext);
            ++depth;
        }
        if (depth == 0) {
            LOGI("M24: vendor returned NO pNext structs from vkGetPhysicalDeviceFeatures2 "
                 "(descriptor indexing is a 1.2 core feature, so its absence means "
                 "the driver does not have it)");
        }
    }

    // M4: pass through real values (see vkGetPhysicalDeviceFeatures note).
    LOGI("vkGetPhysicalDeviceFeatures2 passthrough (vertexPipelineStoresAndAtomics=%d fillModeNonSolid=%d)",
         (int)pFeatures->features.vertexPipelineStoresAndAtomics, (int)pFeatures->features.fillModeNonSolid);

    // M18: same independentBlend gate as the vk1 path. This one is the CORE
    // VkPhysicalDeviceFeatures inside VkPhysicalDeviceFeatures2 -- it is NOT in any
    // pNext chain, so the descriptor-indexing spoof below cannot cover it. The guest
    // reads it via rex::ui::vulkan::VulkanDevice::CreateIfSupported -> +0x1CA.
    {
        const VkBool32 wasIB2 = pFeatures->features.independentBlend;
        pFeatures->features.independentBlend = VK_TRUE;
        LOGI("M18: Features2 independentBlend %d -> VK_TRUE", (int)wasIB2);
    }

    // Intercept pNext chain to expose emulated descriptor indexing features (F2)
    uint32_t depth = 0;

    // M19 telemetry: dump exactly which structs the host chains into this query.
    // This is the single measurement that settles whether the descriptor-indexing
    // spoof below can ever run -- if the host chains nothing, it cannot.
    {
        char chain[512];
        int off = 0;
        const auto* n = reinterpret_cast<const VkBaseOutStructure*>(pFeatures->pNext);
        uint32_t d2 = 0;
        while (n && d2++ < 32) {
            off += std::snprintf(chain + off, sizeof(chain) - off, "%s0x%X",
                                 off ? "," : "", (unsigned)n->sType);
            if (off >= (int)sizeof(chain) - 8) break;
            n = reinterpret_cast<const VkBaseOutStructure*>(n->pNext);
        }
        LOGI("M19: Features2 pNext chain [%s]  (core.independentBlend=%d)",
             off ? chain : "<EMPTY>", (int)pFeatures->features.independentBlend);
    }

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
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES) {
            // M19: the host may chain the 1.1 aggregate instead of the 1.2 one once
            // it sees apiVersion >= 1.2. Same descriptor-indexing surface.
            auto* v11 = reinterpret_cast<VkPhysicalDeviceVulkan11Features*>(next);
            v11->storageBuffer16BitAccess = VK_TRUE;
            v11->uniformAndStorageBuffer16BitAccess = VK_TRUE;
            v11->storagePushConstant16 = VK_TRUE;
            v11->storageInputOutput16 = VK_TRUE;
            v11->multiview = VK_TRUE;
            v11->multiviewGeometryShader = VK_TRUE;
            v11->multiviewTessellationShader = VK_TRUE;
            v11->variablePointersStorageBuffer = VK_TRUE;
            v11->variablePointers = VK_TRUE;
            v11->protectedMemory = VK_FALSE;
            v11->samplerYcbcrConversion = VK_TRUE;
            LOGI("M19: intercepted VkPhysicalDeviceVulkan11Features");
#ifdef VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES) {
            LOGI("M19: chained VkPhysicalDeviceVulkan13Features (passthrough)");
#endif
        }
        next = next->pNext;
    }
}

// M19: vk1 physical-device properties interceptor. Previously we only intercepted
// the Properties2 flavour, so the plain vkGetPhysicalDeviceProperties() went
// straight to the vendor and the app kept seeing the driver's real apiVersion
// 1.1.131 (Mali-G71 / Bifrost, r26p0).
//
// rex::ui::vulkan::VulkanDevice::CreateIfSupported (librexruntime.so 0x42EADC) builds
// its vkGetPhysicalDeviceFeatures2 pNext chain behind extension gates read from
// [instance+0x1A9 / 0x211 / 0x212 / 0x215]. On this run it chained nothing, so
// VkPhysicalDeviceDescriptorIndexingFeatures was never requested, our Features2
// pNext spoof never ran, every descriptor-indexing derived capability stayed false,
// and the guest's DibujosVulkan::Inicializar aborted at its first check:
//   [error] [nativo] C6: el dispositivo Vulkan no tiene independentBlend: no se dibuja
// Reporting 1.2.0 gives the runtime the descriptor-indexing core path instead of
// making it depend on VK_EXT_descriptor_indexing being enabled.
//
// NOTE: we encode apiVersion arithmetically rather than with VK_MAKE_API_VERSION /
// VK_API_VERSION_MAJOR, which are absent from the Vulkan headers this project builds
// against. Layout per the Vulkan spec:
//   apiVersion = (variant << 29) | (major << 22) | (minor << 12) | patch
static constexpr uint32_t kMalicompatApiVersion12 = (1u << 22) | (2u << 12);

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice physicalDevice,
    VkPhysicalDeviceProperties* pProperties) {
    InitRealVulkan();
    PFN_vkGetPhysicalDeviceProperties realPfn = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realPfn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            malicompat_vendor_proc("vkGetPhysicalDeviceProperties"));
    }
    if (!realPfn && g_vendor_vkGetPhysicalDeviceProperties) {
        realPfn = g_vendor_vkGetPhysicalDeviceProperties;
    }
    if (realPfn) {
        realPfn(physicalDevice, pProperties);
    }
    if (!pProperties) return;

    // M19: lift apiVersion to 1.2.0 so descriptor indexing is core rather than an
    // extension the host must remember to enable.
    const uint32_t wasApi = pProperties->apiVersion;
    pProperties->apiVersion = kMalicompatApiVersion12;
    LOGI("M19: vkGetPhysicalDeviceProperties apiVersion %u.%u -> %u.%u (device '%s')",
         (unsigned)((wasApi >> 22) & 0x7Fu), (unsigned)((wasApi >> 12) & 0x3FFu),
         1u, 2u, pProperties->deviceName);
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
    // M19: same apiVersion lift as the vk1 entry point.
    if (pProperties->properties.apiVersion < kMalicompatApiVersion12) {
        const uint32_t wasApi = pProperties->properties.apiVersion;
        pProperties->properties.apiVersion = kMalicompatApiVersion12;
        LOGI("M19: Properties2 apiVersion %u.%u.%u -> 1.2.0",
             (unsigned)((wasApi >> 22) & 0x7Fu),
             (unsigned)((wasApi >> 12) & 0x3FFu),
             (unsigned)(wasApi & 0xFFFu));
    }

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

// M24: VkPhysicalDeviceFeatures member names, in declaration order, so the
// pEnabledFeatures sanitiser can report exactly which bit it cleared. The struct
// is 55 VkBool32 fields, so word*32+bit indexes this table directly.
static const char* const kCoreFeatureNames[55] = {
    "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend",
    "geometryShader", "tessellationShader", "sampleRateShading", "dualSrcBlend",
    "logicOp", "multiDrawIndirect", "drawIndirectFirstInstance", "depthClamp",
    "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines",
    "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy",
    "textureCompressionETC2", "textureCompressionASTC_LDR", "textureCompressionBC",
    "occlusionQueryPrecise", "pipelineStatisticsQuery",
    "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics",
    "shaderTessellationAndGeometryPointSize", "shaderImageGatherExtended",
    "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample",
    "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
    "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing",
    "shaderClipDistance", "shaderCullDistance", "shaderFloat64",
    "shaderInt64", "shaderInt16", "shaderResourceResidency", "shaderResourceMinLod",
    "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D",
    "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples",
    "sparseResidency8Samples", "sparseResidency16Samples", "sparseResidencyAliased",
    "variablePointersStorageBuffer", "variablePointers",
};

// A1: one-time dump of every descriptor limit that constrains the bindless
// heaps, plus a per-binding overflow verdict at vkCreateDescriptorSetLayout.
//
// Why: the guest asks for SAMPLED_IMAGE x 4096 and SAMPLER x 512. This device
// reports maxPerStageDescriptorSampledImages = 256 and maxPerStageDescriptorSamplers
// = 128, and the vendor reports descriptorIndexing = 0 (M24), so both bindings are
// illegal and every geometry pipeline returns VK_ERROR_INITIALIZATION_FAILED --
// "C6: no se pudo crear un pipeline (causa 53)".
//
// Option A is to chunk those heaps. The chunk arithmetic cannot be guessed: the
// per-stage limits are totals across every binding a stage declares, not per
// binding, so we need the real numbers before splitting anything. This logs them,
// and for each offending binding reports the overflow and the chunk count that
// would be required.
namespace malicompat_a1 {
struct Limits {
    bool done = false;
    VkPhysicalDevice physDev = VK_NULL_HANDLE;
    uint32_t maxPerStageSamplers = 0;
    uint32_t maxPerStageSampledImages = 0;
    uint32_t maxPerStageStorageBuffers = 0;
    uint32_t maxPerStageUniformBuffers = 0;
    uint32_t maxPerStageResources = 0;
    uint32_t maxDescriptorSetSamplers = 0;
    uint32_t maxDescriptorSetSampledImages = 0;
};
static Limits lim_;
// Captured at vkCreateDevice: that is the only entry point that receives a
// VkPhysicalDevice. Descriptor set layouts only get a VkDevice.
static VkPhysicalDevice g_a1_phys_dev = VK_NULL_HANDLE;

static void DumpLimitsOnce(VkPhysicalDevice physDev) {
    if (lim_.done || !physDev) return;  // retry later if physDev not captured yet
    lim_.done = true;
    VkPhysicalDeviceProperties props = {};
    PFN_vkGetPhysicalDeviceProperties p1 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        g_vendor_device && g_vendor_device->GetInstanceProcAddr
            ? malicompat_vendor_proc("vkGetPhysicalDeviceProperties") : nullptr);
    if (!p1) p1 = g_vendor_vkGetPhysicalDeviceProperties;
    if (!p1) {
        LOGI("A1: could not resolve vkGetPhysicalDeviceProperties; limits unavailable");
        return;
    }
    p1(physDev, &props);
    lim_.physDev = physDev;
    lim_.maxPerStageSamplers = props.limits.maxPerStageDescriptorSamplers;
    lim_.maxPerStageSampledImages = props.limits.maxPerStageDescriptorSampledImages;
    lim_.maxPerStageStorageBuffers = props.limits.maxPerStageDescriptorStorageBuffers;
    lim_.maxPerStageUniformBuffers = props.limits.maxPerStageDescriptorUniformBuffers;
    lim_.maxPerStageResources = props.limits.maxPerStageResources;
    lim_.maxDescriptorSetSamplers = props.limits.maxDescriptorSetSamplers;
    lim_.maxDescriptorSetSampledImages = props.limits.maxDescriptorSetSampledImages;

    LOGI("A1: DRIVER DESCRIPTOR LIMITS  perStageSamplers=%u perStageSampledImages=%u "
         "perStageUniformBuffers=%u perStageStorageBuffers=%u perStageResources=%u | "
         "perSetSamplers=%u perSetSampledImages=%u",
         lim_.maxPerStageSamplers, lim_.maxPerStageSampledImages,
         lim_.maxPerStageUniformBuffers, lim_.maxPerStageStorageBuffers,
         lim_.maxPerStageResources, lim_.maxDescriptorSetSamplers,
         lim_.maxDescriptorSetSampledImages);
}

static const char* TypeName(uint32_t t) {
    switch (t) {
        case 0: return "SAMPLER";
        case 1: return "COMBINED_IMAGE_SAMPLER";
        case 2: return "SAMPLED_IMAGE";
        case 3: return "STORAGE_IMAGE";
        case 6: return "UNIFORM_BUFFER";
        case 7: return "STORAGE_BUFFER";
        case 8: return "UNIFORM_BUFFER_DYNAMIC";
        default: return "OTHER";
    }
}

// Report each binding that exceeds its per-stage limit, with the chunk count an
// Option A splitter would need. Deliberately does not modify anything: this run
// only establishes whether chunking is even arithmetically feasible.
static void ReportOverflow(const VkDescriptorSetLayoutCreateInfo* ci) {
    if (!ci) return;
    for (uint32_t i = 0; i < ci->bindingCount; ++i) {
        const auto& b = ci->pBindings[i];
        uint32_t limit = 0;
        switch (b.descriptorType) {
            case 0: limit = lim_.maxPerStageSamplers; break;
            case 2: limit = lim_.maxPerStageSampledImages; break;
            default: continue;
        }
        if (b.descriptorCount <= limit) continue;
        const uint32_t chunks = (b.descriptorCount + limit - 1) / limit;
        LOGW("A1: BINDING OVERFLOW binding=%u type=%s count=%u > perStageLimit=%u "
             "=> needs %u chunk(s) of %u (Option A splitter must rewrite SPIR-V "
             "indexing and expand %u bind ranges)",
             (unsigned)b.binding, TypeName(b.descriptorType), (unsigned)b.descriptorCount,
             limit, chunks, limit, chunks);
    }
}
} // namespace malicompat_a1

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

    // M24: sanitise pEnabledFeatures against what the driver really supports.
    //
    // With with_gpu_emulation = true, librexruntime copies supported_features
    // wholesale into enabled_features (XE_UI_VULKAN_FEATURE does
    // `enabled_features.name = supported_features.name`), so anything the layer
    // spoofs in vkGetPhysicalDeviceFeatures comes straight back as a *request* at
    // vkCreateDevice. The driver answers VK_ERROR_FEATURE_NOT_PRESENT for a bit it
    // does not have, and the device fails to be created at all.
    //
    // This is the same reason M4 deliberately passes vertexPipelineStoresAndAtomics
    // through. Rather than leaving that to guesswork, ask the vendor what it has and
    // clear any requested bit it does not, reporting each one. A bit we spoofed in
    // order to satisfy a guest capability gate must never reach the driver as a
    // hard requirement.
    // A1: capture the physical device. This is the only entry point that receives one;
    // descriptor set layout calls only get a VkDevice but need the physical limits.
    malicompat_a1::g_a1_phys_dev = physicalDevice;
    malicompat_a1::DumpLimitsOnce(physicalDevice);

    if (modifiedCreateInfo.pEnabledFeatures && physicalDevice) {
        // pEnabledFeatures is const, so sanitise a local copy and point the
        // create-info at it. Declared here so it outlives the vendor call below.
        VkPhysicalDeviceFeatures sanitizedFeatures = *modifiedCreateInfo.pEnabledFeatures;
        modifiedCreateInfo.pEnabledFeatures = &sanitizedFeatures;

        VkPhysicalDeviceFeatures realVendorFeatures = {};
        PFN_vkGetPhysicalDeviceFeatures realQuery = nullptr;
        if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
            realQuery = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
                malicompat_vendor_proc("vkGetPhysicalDeviceFeatures"));
        }
        if (!realQuery) {
            realQuery = g_vendor_vkGetPhysicalDeviceFeatures;
        }
        if (realQuery) {
            realQuery(physicalDevice, &realVendorFeatures);
        }
        if (realQuery) {
            const uint32_t* req = reinterpret_cast<const uint32_t*>(&sanitizedFeatures);
            const uint32_t* have = reinterpret_cast<const uint32_t*>(&realVendorFeatures);
            const uint32_t nWords = sizeof(VkPhysicalDeviceFeatures) / sizeof(uint32_t);
            char names[512];
            int nOff = 0;
            for (uint32_t w = 0; w < nWords; ++w) {
                uint32_t cleared = req[w] & ~have[w];
                if (!cleared) continue;
                for (uint32_t bit = 0; bit < 32 && cleared; ++bit, cleared >>= 1) {
                    if (!(cleared & 1u)) continue;
                    const uint32_t idx = w * 32 + bit;
                    nOff += snprintf(names + nOff, sizeof(names) - nOff, "%s%s",
                                     nOff ? "," : "", kCoreFeatureNames[idx] ? kCoreFeatureNames[idx] : "?");
                    if (nOff >= (int)sizeof(names) - 4) break;
                }
                if (nOff >= (int)sizeof(names) - 4) break;
            }
            if (nOff) {
                // Reassemble defensively: clear the whole unsupported word set.
                for (uint32_t w = 0; w < nWords; ++w) {
                    reinterpret_cast<uint32_t*>(&sanitizedFeatures)[w] &= have[w];
                }
                LOGW("M24: cleared unsupported pEnabledFeatures bits before vkCreateDevice: %s",
                     names);
            } else {
                LOGI("M24: every requested pEnabledFeatures bit is supported by the driver");
            }
        } else {
            LOGW("M24: could not query vendor features; leaving pEnabledFeatures untouched");
        }
    }

    VkResult res = realCreate(physicalDevice, &modifiedCreateInfo, pAllocator, pDevice);
    LOGI("Vendor vkCreateDevice returned: %d (device=%p)", res, pDevice ? *pDevice : nullptr);

    flight::g_create_device++;
    {
        static bool s_maps_done = false;
        if (!s_maps_done) { s_maps_done = true; flight::maps_snapshot("first-vkCreateDevice"); }
    }
    // Initialize dummy fallback resources on the newly created device (F8)
    if (res == VK_SUCCESS && pDevice && *pDevice) {
        g_last_vendor_device = *pDevice;
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
    // M20: vkDestroyDevice is a device command, so a NULL-instance
    // vkGetInstanceProcAddr cannot see it. Resolve in the vendor device dispatch
    // domain first, same as every other device command (M16/M20). Low severity --
    // only affects teardown, since a missed resolve just leaks the device on exit.
    if (device) {
        realDestroy = reinterpret_cast<PFN_vkDestroyDevice>(
            malicompat_resolve_vendor_device_proc(device, "vkDestroyDevice"));
    }
    if (!realDestroy && g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
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
                // M8: attribute the query to its caller module (loader vs SDK).
                Dl_info info{};
                const void* ret = __builtin_return_address(0);
                const char* mod = "?";
                const char* sym = "?";
                if (dladdr(ret, &info)) {
                    if (info.dli_fname) mod = info.dli_fname;
                    if (info.dli_sname) sym = info.dli_sname;
                }
                LOGI("GetInstanceProcAddr query: '%s' (instance=%p) from %s:%s",
                     pName, (const void*)instance, mod, sym);
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
    // M19: also intercept the Vulkan1.0 properties entry point, so apiVersion is
    // lifted to 1.2.0 even when the host never calls the Properties2 flavour.
    if (std::strcmp(pName, "vkGetPhysicalDeviceProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetPhysicalDeviceProperties);
    if (std::strcmp(pName, "vkEnumerateDeviceExtensionProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkEnumerateDeviceExtensionProperties);
    if (std::strcmp(pName, "vkEnumeratePhysicalDevices") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkEnumeratePhysicalDevices);

    // Descriptor Interception
    if (std::strcmp(pName, "vkCreateDescriptorPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorPool);
    if (std::strcmp(pName, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorSetLayout);
    if (std::strcmp(pName, "vkAllocateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkAllocateDescriptorSets);
    if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkUpdateDescriptorSets);
    if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdBindDescriptorSets);
    if (std::strcmp(pName, "vkCmdDraw") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdDraw);
    if (std::strcmp(pName, "vkCmdDrawIndexed") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdDrawIndexed);
    if (std::strcmp(pName, "vkCmdDrawIndirect") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdDrawIndirect);

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
    if (std::strcmp(pName, "vkCreateGraphicsPipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateGraphicsPipelines);
    if (std::strcmp(pName, "vkCreateComputePipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateComputePipelines);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetDeviceQueue);
    // M17e: AcquireImage/SignalRelease interception reverted (bisect-proven
    // killer); GrallocUsage pair + default-fallback query logging retained.
    if (std::strcmp(pName, "vkGetSwapchainGrallocUsageANDROID") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetSwapchainGrallocUsageANDROID);
    if (std::strcmp(pName, "vkGetSwapchainGrallocUsage2ANDROID") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetSwapchainGrallocUsage2ANDROID);
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
    // M17d: query logging with ZERO behavior change (log name + resolved ptr).
    PFN_vkVoidFunction pfn = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        pfn = g_vendor_device->GetInstanceProcAddr(instance, pName);
    }
    if (!pfn && g_vendor_vulkan) {
        pfn = reinterpret_cast<PFN_vkVoidFunction>(dlsym(g_vendor_vulkan, pName));
    }
    {
        static uint32_t s_qlog = 0;
        if (s_qlog < 300) {
            ++s_qlog;
            LOGI("GetInstanceProcAddr default: '%s' -> %p", pName, pfn);
        }
    }
    flight::proclog("GIPA", pName, pfn, __builtin_return_address(0));
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
    if (std::strcmp(pName, "vkCmdDraw") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdDraw);
    if (std::strcmp(pName, "vkCmdDrawIndexed") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdDrawIndexed);
    if (std::strcmp(pName, "vkCmdDrawIndirect") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdDrawIndirect);

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
    if (std::strcmp(pName, "vkCreateGraphicsPipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateGraphicsPipelines);
    if (std::strcmp(pName, "vkCreateComputePipelines") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateComputePipelines);
    if (std::strcmp(pName, "vkGetDeviceQueue") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetDeviceQueue);
    // M17e: AcquireImage/SignalRelease interception reverted (bisect-proven
    // killer); GrallocUsage pair + default-fallback query logging retained.
    if (std::strcmp(pName, "vkGetSwapchainGrallocUsageANDROID") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetSwapchainGrallocUsageANDROID);
    if (std::strcmp(pName, "vkGetSwapchainGrallocUsage2ANDROID") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkGetSwapchainGrallocUsage2ANDROID);
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
    // M17d: query logging with ZERO behavior change.
    PFN_vkVoidFunction pfn = malicompat_vendor_proc(pName);
    if (!pfn && g_vendor_vulkan) {
        pfn = reinterpret_cast<PFN_vkVoidFunction>(dlsym(g_vendor_vulkan, pName));
    }
    {
        static uint32_t s_qlog = 0;
        if (s_qlog < 300) {
            ++s_qlog;
            LOGI("GetDeviceProcAddr default: '%s' -> %p", pName, pfn);
        }
    }
    flight::proclog("GDPA", pName, pfn, __builtin_return_address(0));
    return pfn;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateDescriptorPool(
    VkDevice device,
    const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorPool* pDescriptorPool) {
    InitRealVulkan();
    PFN_vkCreateDescriptorPool realPfn = nullptr;
    // M16: instance-query/dlsym are NULL for this on Mali r15 HAL mode;
    // resolve in the vendor device domain first (same class as M15).
    realPfn = reinterpret_cast<PFN_vkCreateDescriptorPool>(
        malicompat_resolve_vendor_device_proc(device, "vkCreateDescriptorPool"));
    if (!realPfn && g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
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
    malicompat_a1::DumpLimitsOnce(malicompat_a1::g_a1_phys_dev);
    malicompat_a1::ReportOverflow(pCreateInfo);
    if (pCreateInfo) {
        LOGI("dsl: setLayout bindingCount=%u flags=0x%x",
             (unsigned)pCreateInfo->bindingCount, (unsigned)pCreateInfo->flags);
        for (uint32_t i = 0; i < pCreateInfo->bindingCount && i < 16; ++i) {
            const auto& b = pCreateInfo->pBindings[i];
            LOGI("dsl:   binding[%u] binding=%u type=%d count=%u stages=0x%x",
                 i, (unsigned)b.binding, (int)b.descriptorType, (unsigned)b.descriptorCount,
                 (unsigned)b.stageFlags);
        }
        const auto* node = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
        uint32_t depth = 0;
        while (node && depth < 8) {
            LOGI("dsl:   setLayout pNext[%u] sType=%d", depth, (int)node->sType);
            node = reinterpret_cast<const VkBaseInStructure*>(node->pNext);
            ++depth;
        }
    }
    // M13: g_vendor_vkCreateDescriptorSetLayout comes from dlsym on the HAL
    // .so, which is NULL in HAL mode (device procs only via GetProcAddr).
    // Resolve against the live instance first, like the pool wrapper does.
    PFN_vkCreateDescriptorSetLayout realPfn = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
        malicompat_vendor_proc("vkCreateDescriptorSetLayout"));
    if (!realPfn) realPfn = g_vendor_vkCreateDescriptorSetLayout;
    if (!realPfn && g_vendor_vulkan) {
        realPfn = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
            dlsym(g_vendor_vulkan, "vkCreateDescriptorSetLayout"));
    }
    if (!realPfn) {
        LOGE("vkCreateDescriptorSetLayout: no vendor proc (HAL dlsym + GetProcAddr both null)");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorSetLayout(
        device, pCreateInfo, pAllocator, pSetLayout, realPfn);
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets) {
    InitRealVulkan();
    // M16: device-domain first (instance-query may be NULL on Mali r15).
    PFN_vkAllocateDescriptorSets realPfn = reinterpret_cast<PFN_vkAllocateDescriptorSets>(
        malicompat_resolve_vendor_device_proc(device, "vkAllocateDescriptorSets"));
    if (!realPfn) {
        realPfn = reinterpret_cast<PFN_vkAllocateDescriptorSets>(
            malicompat_vendor_proc("vkAllocateDescriptorSets"));
    }
    if (!realPfn) realPfn = g_vendor_vkAllocateDescriptorSets;
    if (!realPfn && g_vendor_vulkan) {
        realPfn = reinterpret_cast<PFN_vkAllocateDescriptorSets>(
            dlsym(g_vendor_vulkan, "vkAllocateDescriptorSets"));
    }
    if (!realPfn) {
        LOGE("vkAllocateDescriptorSets: no vendor proc");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    return malicompat::VulkanDescriptorShim::Instance().InterceptAllocateDescriptorSets(
        device, pAllocateInfo, pDescriptorSets, realPfn);
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkUpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies) {
    InitRealVulkan();
    // M13: resolve update proc via live instance first (dlsym may be NULL).
    // M16: device-domain first (instance-query may be NULL on Mali r15).
    PFN_vkUpdateDescriptorSets realUpdate = reinterpret_cast<PFN_vkUpdateDescriptorSets>(
        malicompat_resolve_vendor_device_proc(device, "vkUpdateDescriptorSets"));
    if (!realUpdate) {
        realUpdate = reinterpret_cast<PFN_vkUpdateDescriptorSets>(
            malicompat_vendor_proc("vkUpdateDescriptorSets"));
    }
    if (!realUpdate) realUpdate = g_vendor_vkUpdateDescriptorSets;
    if (!realUpdate && g_vendor_vulkan) {
        realUpdate = reinterpret_cast<PFN_vkUpdateDescriptorSets>(
            dlsym(g_vendor_vulkan, "vkUpdateDescriptorSets"));
    }
    if (!realUpdate) {
        LOGE("vkUpdateDescriptorSets: no vendor proc");
        return;
    }
    malicompat::VulkanDescriptorShim::Instance().InterceptUpdateDescriptorSets(
        device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies, realUpdate);
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
    // M13: resolve bind proc via live instance first (dlsym may be NULL).
    // M16: command buffers carry no device; use the last created vendor device
    // for domain-correct resolution first.
    PFN_vkCmdBindDescriptorSets realBind = nullptr;
    if (g_last_vendor_device != VK_NULL_HANDLE) {
        realBind = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
            malicompat_resolve_vendor_device_proc(g_last_vendor_device, "vkCmdBindDescriptorSets"));
    }
    if (!realBind) {
        realBind = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
            malicompat_vendor_proc("vkCmdBindDescriptorSets"));
    }
    if (!realBind) realBind = g_vendor_vkCmdBindDescriptorSets;
    if (!realBind && g_vendor_vulkan) {
        realBind = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
            dlsym(g_vendor_vulkan, "vkCmdBindDescriptorSets"));
    }
    if (!realBind) {
        LOGE("vkCmdBindDescriptorSets: no vendor proc");
        return;
    }
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets, realBind);
}

// ---------------------------------------------------------------------------
// A8: draw tracer.
//
// The black screen showed 8 pipelines, thousands of successful vkQueueSubmit,
// and ZERO vkQueuePresentKHR. That leaves exactly two hypotheses, and they
// need opposite fixes:
//   (a) the guest never issues a draw at all  -> the native renderer bailed
//       out early, and the descriptor-limit rejection (cause 53) is the cause
//   (b) the guest issues thousands of draws   -> they are being rejected or
//       the present path is broken, a completely different problem
// Nothing observable so far distinguishes these, because we were only
// tracing binds and submits, never the draws themselves. These counters do.
// ---------------------------------------------------------------------------
namespace {
struct DrawStats {
    uint64_t draw = 0;
    uint64_t drawIndexed = 0;
    uint64_t drawIndirect = 0;
    uint64_t drawIndirectCount = 0;
    uint64_t vertices = 0;
    uint64_t instances = 0;
    uint64_t beginRenderPass = 0;
    uint64_t endRenderPass = 0;
};
DrawStats g_drawStats;
uint64_t g_lastLoggedDraws = 0;
bool g_drawStatsLoggedOnce = false;

void MaybeLogDrawStats() {
    const uint64_t total = g_drawStats.draw + g_drawStats.drawIndexed +
                           g_drawStats.drawIndirect + g_drawStats.drawIndirectCount;
    // Log the first observation immediately: "0 draws ever" is itself the
    // finding we are hunting for, so silence must not swallow it.
    if (!g_drawStatsLoggedOnce) {
        g_drawStatsLoggedOnce = true;
        g_lastLoggedDraws = total;
        LOGW("A8: FIRST SAMPLE draws=%llu indexed=%llu indirect=%llu indirectCount=%llu "
             "verts=%llu instances=%llu renderPass=%llu",
             (unsigned long long)g_drawStats.draw,
             (unsigned long long)g_drawStats.drawIndexed,
             (unsigned long long)g_drawStats.drawIndirect,
             (unsigned long long)g_drawStats.drawIndirectCount,
             (unsigned long long)g_drawStats.vertices,
             (unsigned long long)g_drawStats.instances,
             (unsigned long long)g_drawStats.beginRenderPass);
        return;
    }
    if (total - g_lastLoggedDraws < 2000) return;
    g_lastLoggedDraws = total;
    LOGW("A8: draws=%llu indexed=%llu indirect=%llu indirectCount=%llu verts=%llu "
         "instances=%llu renderPass=%llu",
         (unsigned long long)g_drawStats.draw,
         (unsigned long long)g_drawStats.drawIndexed,
         (unsigned long long)g_drawStats.drawIndirect,
         (unsigned long long)g_drawStats.drawIndirectCount,
         (unsigned long long)g_drawStats.vertices,
         (unsigned long long)g_drawStats.instances,
         (unsigned long long)g_drawStats.beginRenderPass);
}

// Resolve a device-level proc using the same instance-then-device fallback the
// other interceptors use, so the tracer works no matter which domain the guest
// reached the function through. Returns void* rather than being a template:
// this code sits inside extern "C", where templates are ill-formed.
static void* ResolveDrawProcRaw(const char* name) {
    if (g_last_vendor_device != VK_NULL_HANDLE) {
        void* p = malicompat_resolve_vendor_device_proc(g_last_vendor_device, name);
        if (p) return p;
    }
    void* p = malicompat_vendor_proc(name);
    if (p) return p;
    if (g_vendor_vulkan) return dlsym(g_vendor_vulkan, name);
    return nullptr;
}
} // namespace

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdDraw(
    VkCommandBuffer commandBuffer, uint32_t vertexCount, uint32_t instanceCount,
    uint32_t firstVertex, uint32_t firstInstance) {
    InitRealVulkan();
    ++g_drawStats.draw;
    g_drawStats.vertices += vertexCount;
    g_drawStats.instances += instanceCount;
    MaybeLogDrawStats();
    auto real = reinterpret_cast<PFN_vkCmdDraw>(ResolveDrawProcRaw("vkCmdDraw"));
    if (real) real(commandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdDrawIndexed(
    VkCommandBuffer commandBuffer, uint32_t indexCount, uint32_t instanceCount,
    uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    InitRealVulkan();
    ++g_drawStats.drawIndexed;
    g_drawStats.vertices += indexCount;
    g_drawStats.instances += instanceCount;
    MaybeLogDrawStats();
    auto real = reinterpret_cast<PFN_vkCmdDrawIndexed>(ResolveDrawProcRaw("vkCmdDrawIndexed"));
    if (real) real(commandBuffer, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdDrawIndirect(
    VkCommandBuffer commandBuffer, const VkDrawIndirectCommand* pDrawInfo) {
    InitRealVulkan();
    ++g_drawStats.drawIndirect;
    MaybeLogDrawStats();
    auto real = reinterpret_cast<PFN_vkCmdDrawIndirect>(ResolveDrawProcRaw("vkCmdDrawIndirect"));
    if (real) real(commandBuffer, pDrawInfo);
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
    flight::g_swapchain++;
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
    flight::g_swap_images++;
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

    // M9: bounded present tracing (first calls + periodic + errors).
    static uint32_t s_present_count = 0;
    ++s_present_count;
    flight::g_present_q++;
    const bool log_this = (s_present_count <= 5) || (s_present_count % 500 == 0);

    // 1. Cross-Driver alignment: Because VkQueue is allocated on the vendor Mali device (g_vendor_device),
    // passing it to the Android system loader causes VK_ERROR_DEVICE_LOST. First route presentation
    // through the vendor driver/HAL where VkQueue was allocated.
    PFN_vkQueuePresentKHR vendorPresent = nullptr;
    if (g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        vendorPresent = reinterpret_cast<PFN_vkQueuePresentKHR>(
            malicompat_vendor_proc("vkQueuePresentKHR"));
    }
    if (!vendorPresent && g_vendor_vkQueuePresentKHR) {
        vendorPresent = g_vendor_vkQueuePresentKHR;
    }
    if (!vendorPresent && g_vendor_vulkan) {
        vendorPresent = reinterpret_cast<PFN_vkQueuePresentKHR>(dlsym(g_vendor_vulkan, "vkQueuePresentKHR"));
    }
    if (vendorPresent) {
        VkResult res = vendorPresent(queue, pPresentInfo);
        if (res == VK_SUCCESS) flight::g_present_ok++;
        if (log_this || res != VK_SUCCESS) {
            LOGI("vkQueuePresentKHR #%u -> %d (swapchains=%u)", s_present_count, (int)res,
                 pPresentInfo ? pPresentInfo->swapchainCount : 0);
        }
        return res;
    }

    // 2. Fallback to system loader only if vendor driver does not export vkQueuePresentKHR
    if (g_system_vkQueuePresentKHR) {
        VkResult res = g_system_vkQueuePresentKHR(queue, pPresentInfo);
        if (res == VK_SUCCESS) flight::g_present_ok++;
        LOGI("vkQueuePresentKHR #%u -> %d via system loader", s_present_count, (int)res);
        return res;
    }
    if (g_system_vulkan) {
        auto pfn = reinterpret_cast<PFN_vkQueuePresentKHR>(dlsym(g_system_vulkan, "vkQueuePresentKHR"));
        if (pfn) {
            VkResult res = pfn(queue, pPresentInfo);
            if (res == VK_SUCCESS) flight::g_present_ok++;
            LOGI("vkQueuePresentKHR #%u -> %d via system dlsym", s_present_count, (int)res);
            return res;
        }
    }
    LOGE("vkQueuePresentKHR #%u: no implementation!", s_present_count);
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
    // M9: bounded submit tracing.
    static uint32_t s_submit_count = 0;
    ++s_submit_count;
    PFN_vkQueueSubmit realSubmit = nullptr;
    // M15: resolve in the queue's device domain first (instance-query is NULL
    // for device procs on Mali r15; the old fallback silently dropped submits).
    {
        std::lock_guard<std::mutex> lock(g_queue_map_mutex);
        auto it = g_queue_to_device.find(queue);
        if (it != g_queue_to_device.end()) {
            realSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
                malicompat_resolve_vendor_device_proc(it->second, "vkQueueSubmit"));
        }
    }
    if (!realSubmit && g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realSubmit = reinterpret_cast<PFN_vkQueueSubmit>(
            malicompat_vendor_proc("vkQueueSubmit"));
    }
    if (!realSubmit && g_vendor_vkQueueSubmit) {
        realSubmit = g_vendor_vkQueueSubmit;
    }
    if (!realSubmit && g_vendor_vulkan) {
        realSubmit = reinterpret_cast<PFN_vkQueueSubmit>(dlsym(g_vendor_vulkan, "vkQueueSubmit"));
    }

    if (realSubmit) {
        VkResult res = realSubmit(queue, submitCount, pSubmits, fence);
        {
            uint32_t sc = ++flight::g_submit;
            if (res == VK_SUCCESS) flight::g_submit_ok++;
            if (sc == 300) flight::maps_snapshot("submit-300");
            if (sc % 300 == 0) flight::summary();
        }
        if (s_submit_count <= 5 || (s_submit_count % 1000 == 0) || res != VK_SUCCESS) {
            LOGI("vkQueueSubmit #%u: count=%u fence=%p -> %d", s_submit_count,
                 submitCount, (const void*)fence, (int)res);
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
    // M20: device command; NULL-instance GIPA cannot see it. The strong override in
    // spirv_remapper.cpp is what GetDeviceProcAddr actually hands out, so this weak
    // copy is normally dead code, but it carried the same 0x501234 mock handle that
    // M15 was created to remove. Fix the lookup and drop the mock.
    if (device) {
        realCreate = reinterpret_cast<PFN_vkCreateShaderModule>(
            malicompat_resolve_vendor_device_proc(device, "vkCreateShaderModule"));
    }
    if (!realCreate && g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
        realCreate = reinterpret_cast<PFN_vkCreateShaderModule>(
            g_vendor_device->GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateShaderModule"));
    }
    if (!realCreate && g_vendor_vulkan) {
        realCreate = reinterpret_cast<PFN_vkCreateShaderModule>(dlsym(g_vendor_vulkan, "vkCreateShaderModule"));
    }

    if (realCreate) {
        return realCreate(device, pCreateInfo, pAllocator, pShaderModule);
    }
    // M20: never mint a fake handle. The driver dereferences VkShaderModule, and a
    // synthetic pointer kills it inside vkCreateGraphicsPipelines.
    LOGE("malicompat_vkCreateShaderModule: vendor proc unresolvable, failing loud");
    return VK_ERROR_INITIALIZATION_FAILED;
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

    // M23: always report the layout shape. Previously we only logged when clamping,
    // so a layout that was within spec but inconsistent with the shaders (which is
    // what VK_ERROR_INITIALIZATION_FAILED from vkCreateGraphicsPipelines usually
    // means) was completely invisible. Set count alone is not enough: the guest's
    // four-descriptor-set packing moves the cube heap into binding 1 of the 3D set
    // and shifts the UBO set, so we also want the pNext flags that carry
    // VK_DESCRIPTOR_BINDING_* and the push-constant range.
    LOGI("dsl: pipelineLayout setLayoutCount=%u pushConstantRanges=%u",
         sanitizedInfo.setLayoutCount, (unsigned)sanitizedInfo.pushConstantRangeCount);
    {
        const auto* node = reinterpret_cast<const VkBaseInStructure*>(sanitizedInfo.pNext);
        uint32_t depth = 0;
        while (node && depth < 8) {
            LOGI("dsl: pipelineLayout pNext[%u] sType=%d", depth, (int)node->sType);
            node = reinterpret_cast<const VkBaseInStructure*>(node->pNext);
            ++depth;
        }
    }

    PFN_vkCreatePipelineLayout realFunc = reinterpret_cast<PFN_vkCreatePipelineLayout>(
        malicompat_resolve_vendor_device_proc(device, "vkCreatePipelineLayout"));
    if (!realFunc) {
        realFunc = reinterpret_cast<PFN_vkCreatePipelineLayout>(
            malicompat_vendor_proc("vkCreatePipelineLayout"));
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

    // M15: never return mock handles (0x401234 crashed Mali in pipeline init).
    LOGE("malicompat_vkCreatePipelineLayout: vendor proc unresolvable, failing loud");
    return VK_ERROR_INITIALIZATION_FAILED;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreatePipelineCache(
    VkDevice device,
    const VkPipelineCacheCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkPipelineCache* pPipelineCache) {
    InitRealVulkan();
    PFN_vkCreatePipelineCache realFunc = nullptr;
    // M20: vkCreatePipelineCache is a DEVICE command. vkGetInstanceProcAddr with a
    // NULL instance only resolves *global* commands and returns NULL for device
    // ones, so the old lookup below could never succeed and this function fell
    // through to the mock handle 0x301234 on every single call.
    //
    // That mock is what killed the first guest draw. nfsmw's DibujosVulkan creates
    // a pipeline cache (C6 "cache de pipelines activa"), got 0x301234 back, and then
    // passed it as VkGraphicsPipelineCreateInfo::pipelineCache. The Mali driver
    // dereferenced the bogus handle and the process died with SIGSEGV inside
    // vkCreateGraphicsPipelines. Evidence from the run:
    //
    //   22:06:41.738 W malicompat_vkCreatePipelineCache: vendor HAL unresolvable,
    //                     returning mock cache
    //   22:06:41.720 I pipe#6 gfx[0]: ... cache=0x0      -> vendor returned 0   (OK)
    //   22:06:42.677 I pipe#7 gfx[0]: ... cache=0x301234 -> vendor returned ?  (SIGSEGV)
    //   22:06:42.951 Zygote: Process 4200 exited due to signal 11
    //
    // Every pipeline that passed cache=0x0 succeeded; the only pipeline that got
    // the mock handle crashed the process. M15 removed the equivalent mock for
    // shader modules (0x501234) but missed this one.
    //
    // Resolve through the vendor device dispatch table first, which is the only
    // domain where a device command is visible.
    if (device) {
        realFunc = reinterpret_cast<PFN_vkCreatePipelineCache>(
            malicompat_resolve_vendor_device_proc(device, "vkCreatePipelineCache"));
    }
    if (!realFunc && g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
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
    // M20: a null pipeline cache is a legal request and means "no cache". Returning
    // VK_NULL_HANDLE keeps every downstream vkDestroyPipelineCache /
    // vkGetPipelineCacheData call well defined, and is exactly the shape the seven
    // pipelines that already succeeded were using (cache=0x0). Never mint a fake
    // handle again: the driver is handed the pointer and dereferences it.
    LOGW("malicompat_vkCreatePipelineCache: vendor HAL unresolvable, returning "
         "VK_NULL_HANDLE (legal no-cache) instead of a mock handle");
    if (pPipelineCache) {
        *pPipelineCache = VK_NULL_HANDLE;
    }
    return VK_SUCCESS;
}

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkDestroyPipelineCache(
    VkDevice device,
    VkPipelineCache pipelineCache,
    const VkAllocationCallbacks* pAllocator) {
    InitRealVulkan();
    PFN_vkDestroyPipelineCache realFunc = nullptr;
    // M20: device command, so it is only visible through the vendor's device
    // dispatch table. A NULL-instance vkGetInstanceProcAddr never resolves it.
    if (device) {
        realFunc = reinterpret_cast<PFN_vkDestroyPipelineCache>(
            malicompat_resolve_vendor_device_proc(device, "vkDestroyPipelineCache"));
    }
    if (!realFunc && g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
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
    // M20: device command; NULL-instance GIPA cannot see it. This one matters now
    // that vkCreatePipelineCache hands back a real vendor handle: the guest
    // serialises the C6 pipeline cache to nfsmw_nativo_pipelines.bin through this
    // call, and silently returning size 0 would break that persistence.
    if (device) {
        realFunc = reinterpret_cast<PFN_vkGetPipelineCacheData>(
            malicompat_resolve_vendor_device_proc(device, "vkGetPipelineCacheData"));
    }
    if (!realFunc && g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
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
    // M20: device command; NULL-instance GIPA cannot see it.
    if (device) {
        realFunc = reinterpret_cast<PFN_vkMergePipelineCaches>(
            malicompat_resolve_vendor_device_proc(device, "vkMergePipelineCaches"));
    }
    if (!realFunc && g_vendor_device && g_vendor_device->GetInstanceProcAddr) {
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

// M14: graphics/compute pipeline tracer. Logs the full create-info shape of
// every pipeline (the Mali r15 driver SIGSEGVs inside vkCreateGraphicsPipelines
// for the guest-output paint pipeline) then forwards unchanged (tracer only).
namespace {
std::atomic<uint32_t> g_pipeline_log_count{0};

void malicompat_log_pnext_chain(const void* pNext, uint32_t depth = 0) {
    const auto* node = reinterpret_cast<const VkBaseInStructure*>(pNext);
    uint32_t i = 0;
    while (node && i < 8) {
        LOGI("  pipe pNext[%u] sType=%d", depth * 8 + i, static_cast<int>(node->sType));
        node = node->pNext;
        ++i;
    }
}
}  // namespace

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateGraphicsPipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkGraphicsPipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines) {
    InitRealVulkan();
    uint32_t n = g_pipeline_log_count.fetch_add(1);
    if (pCreateInfos) {
        for (uint32_t k = 0; k < createInfoCount; ++k) {
            const auto& ci = pCreateInfos[k];
            LOGI("pipe#%u gfx[%u]: flags=0x%x stages=%u layout=%p renderPass=%p subpass=%u cache=%p baseIdx=%d",
                 n, k, ci.flags, ci.stageCount, (const void*)ci.layout,
                 (const void*)ci.renderPass, ci.subpass, (const void*)pipelineCache,
                 ci.basePipelineIndex);
            malicompat_log_pnext_chain(ci.pNext);
            for (uint32_t s = 0; s < ci.stageCount; ++s) {
                const auto& st = ci.pStages[s];
                LOGI("  pipe#%u stage[%u]: bit=0x%x module=%p name='%s'",
                     n, s, st.stage, (const void*)st.module, st.pName ? st.pName : "(null)");
                malicompat_log_pnext_chain(st.pNext, 1);
            }
            if (ci.pVertexInputState) {
                LOGI("  pipe#%u vertex: bindings=%u attribs=%u", n,
                     ci.pVertexInputState->vertexBindingDescriptionCount,
                     ci.pVertexInputState->vertexAttributeDescriptionCount);
            } else { LOGI("  pipe#%u vertex: NULL", n); }
            if (ci.pInputAssemblyState) {
                LOGI("  pipe#%u inputAsm: topology=%d restart=%d", n,
                     static_cast<int>(ci.pInputAssemblyState->topology),
                     (int)ci.pInputAssemblyState->primitiveRestartEnable);
            } else { LOGI("  pipe#%u inputAsm: NULL", n); }
            if (ci.pTessellationState) {
                LOGI("  pipe#%u tess: patches=%u", n, ci.pTessellationState->patchControlPoints);
            }
            if (ci.pViewportState) {
                LOGI("  pipe#%u viewport: vp=%u sc=%u", n,
                     ci.pViewportState->viewportCount, ci.pViewportState->scissorCount);
            } else { LOGI("  pipe#%u viewport: NULL (dynamic?)", n); }
            if (ci.pRasterizationState) {
                const auto* rs = ci.pRasterizationState;
                LOGI("  pipe#%u raster: polyMode=%d cull=0x%x front=%d clamp=%d discard=%d lineW=%f depthBias=%d",
                     n, static_cast<int>(rs->polygonMode), rs->cullMode,
                     static_cast<int>(rs->frontFace), (int)rs->depthClampEnable,
                     (int)rs->rasterizerDiscardEnable, rs->lineWidth,
                     (int)rs->depthBiasEnable);
                malicompat_log_pnext_chain(rs->pNext, 1);
            } else { LOGI("  pipe#%u raster: NULL", n); }
            if (ci.pMultisampleState) {
                const auto* ms = ci.pMultisampleState;
                LOGI("  pipe#%u msaa: samples=0x%x shading=%d minSh=%f a2c=%d a2o=%d",
                     n, ms->rasterizationSamples, (int)ms->sampleShadingEnable,
                     ms->minSampleShading, (int)ms->alphaToCoverageEnable,
                     (int)ms->alphaToOneEnable);
            } else { LOGI("  pipe#%u msaa: NULL", n); }
            if (ci.pDepthStencilState) {
                const auto* ds = ci.pDepthStencilState;
                LOGI("  pipe#%u depth: test=%d write=%d op=%d stencilTest=%d", n,
                     (int)ds->depthTestEnable, (int)ds->depthWriteEnable,
                     static_cast<int>(ds->depthCompareOp), (int)ds->stencilTestEnable);
            } else { LOGI("  pipe#%u depth: NULL", n); }
            if (ci.pColorBlendState) {
                LOGI("  pipe#%u blend: logicOp=%d attachCount=%u", n,
                     (int)ci.pColorBlendState->logicOpEnable,
                     ci.pColorBlendState->attachmentCount);
                for (uint32_t a = 0; a < ci.pColorBlendState->attachmentCount && a < 4; ++a) {
                    const auto& at = ci.pColorBlendState->pAttachments[a];
                    LOGI("  pipe#%u blend[%u]: en=%d mask=0x%x", n, a,
                         (int)at.blendEnable, at.colorWriteMask);
                }
            } else { LOGI("  pipe#%u blend: NULL", n); }
            if (ci.pDynamicState) {
                LOGI("  pipe#%u dynamic: count=%u", n, ci.pDynamicState->dynamicStateCount);
            }
        }
    }
    PFN_vkCreateGraphicsPipelines realFunc = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
        malicompat_resolve_vendor_device_proc(device, "vkCreateGraphicsPipelines"));
    if (!realFunc) {
        realFunc = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
            malicompat_vendor_proc("vkCreateGraphicsPipelines"));
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
            dlsym(g_vendor_vulkan, "vkCreateGraphicsPipelines"));
    }
    if (!realFunc) {
        LOGE("pipe#%u: no vendor vkCreateGraphicsPipelines", n);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LOGI("pipe#%u: forwarding %u gfx infos to vendor", n, createInfoCount);
    VkResult res = realFunc(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    LOGI("pipe#%u: vendor returned %d", n, res);
    if (res == VK_SUCCESS) flight::g_pipe_gfx++;
    return res;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateComputePipelines(
    VkDevice device,
    VkPipelineCache pipelineCache,
    uint32_t createInfoCount,
    const VkComputePipelineCreateInfo* pCreateInfos,
    const VkAllocationCallbacks* pAllocator,
    VkPipeline* pPipelines) {
    InitRealVulkan();
    uint32_t n = g_pipeline_log_count.fetch_add(1);
    LOGI("pipe#%u: %u compute infos cache=%p", n, createInfoCount, (const void*)pipelineCache);
    PFN_vkCreateComputePipelines realFunc = reinterpret_cast<PFN_vkCreateComputePipelines>(
        malicompat_resolve_vendor_device_proc(device, "vkCreateComputePipelines"));
    if (!realFunc) {
        realFunc = reinterpret_cast<PFN_vkCreateComputePipelines>(
            malicompat_vendor_proc("vkCreateComputePipelines"));
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<PFN_vkCreateComputePipelines>(
            dlsym(g_vendor_vulkan, "vkCreateComputePipelines"));
    }
    if (!realFunc) {
        LOGE("pipe#%u: no vendor vkCreateComputePipelines", n);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult cres = realFunc(device, pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines);
    if (cres == VK_SUCCESS) flight::g_pipe_comp++;
    return cres;
}

// M17: VK_ANDROID_native_buffer tracers (see header). Fail loud when the
// vendor proc is unresolvable — a silent fallback here is exactly the black
// screen (loader cancelBuffer()s on error and nobody logs it).
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAcquireImageANDROID(
    VkDevice device,
    VkImage image,
    int* pNativeFenceFd,
    VkSemaphore semaphore,
    VkFence fence) {
    InitRealVulkan();
    using Pfn = PFN_vkAcquireImageANDROID;
    // M17f: prefer the exact proc saved at query time (identical to default).
    Pfn realFunc = g_exact_AcquireImage;
    if (!realFunc) {
        realFunc = reinterpret_cast<Pfn>(
            malicompat_resolve_vendor_device_proc(device, "vkAcquireImageANDROID"));
    }
    if (!realFunc) {
        realFunc = reinterpret_cast<Pfn>(malicompat_vendor_proc("vkAcquireImageANDROID"));
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<Pfn>(dlsym(g_vendor_vulkan, "vkAcquireImageANDROID"));
    }
    if (!realFunc) {
        // M17c: Mali r15 exports no AcquireImageANDROID (nm-clean); failing
        // here kills swapchain setup (M17 lesson). Emulate: no fence, success.
        // If the image is bogus the driver will crash loudly (tombstone=data).
        LOGW("vkAcquireImageANDROID: no vendor proc, emulating success (fd=-1)");
        if (pNativeFenceFd) *pNativeFenceFd = -1;
        return VK_SUCCESS;
    }
    VkResult res = realFunc(device, image, pNativeFenceFd, semaphore, fence);
    static uint32_t s_acq_count = 0;
    if (++s_acq_count <= 3 || res != VK_SUCCESS) {
        LOGI("vkAcquireImageANDROID #%u: image=%p fenceFd=%d -> %d",
             s_acq_count, (const void*)image,
             pNativeFenceFd ? *pNativeFenceFd : -2, (int)res);
    }
    return res;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkQueueSignalReleaseImageANDROID(
    VkQueue queue,
    uint32_t waitSemaphoreCount,
    const VkSemaphore* pWaitSemaphores,
    VkImage image,
    int* pNativeFenceFd) {
    InitRealVulkan();
    using Pfn = PFN_vkQueueSignalReleaseImageANDROID;
    // M17f: prefer the exact proc saved at query time (identical to default).
    // (falls through to the queue-map/device/instance/dlsym chain below)
    Pfn realFunc = g_exact_SignalRelease;
    if (!realFunc) {
        std::lock_guard<std::mutex> lock(g_queue_map_mutex);
        auto it = g_queue_to_device.find(queue);
        if (it != g_queue_to_device.end()) {
            realFunc = reinterpret_cast<Pfn>(
                malicompat_resolve_vendor_device_proc(it->second, "vkQueueSignalReleaseImageANDROID"));
        }
    }
    if (!realFunc && g_last_vendor_device != VK_NULL_HANDLE) {
        realFunc = reinterpret_cast<Pfn>(
            malicompat_resolve_vendor_device_proc(g_last_vendor_device, "vkQueueSignalReleaseImageANDROID"));
    }
    if (!realFunc) {
        realFunc = reinterpret_cast<Pfn>(malicompat_vendor_proc("vkQueueSignalReleaseImageANDROID"));
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<Pfn>(dlsym(g_vendor_vulkan, "vkQueueSignalReleaseImageANDROID"));
    }
    if (!realFunc) {
        // M17c: same emulation rationale as AcquireImage. Success here makes
        // the loader queueBuffer() instead of cancelBuffer() (black screen).
        LOGW("vkQueueSignalReleaseImageANDROID: no vendor proc, emulating success (fd=-1)");
        if (pNativeFenceFd) *pNativeFenceFd = -1;
        return VK_SUCCESS;
    }
    VkResult res = realFunc(queue, waitSemaphoreCount, pWaitSemaphores, image, pNativeFenceFd);
    static uint32_t s_rel_count = 0;
    ++s_rel_count;
    // Log first few + every failure + periodic heartbeat: this call decides
    // queueBuffer vs cancelBuffer in the loader. Silence here = black screen.
    if (s_rel_count <= 5 || res != VK_SUCCESS || (s_rel_count % 1000 == 0)) {
        LOGI("vkQueueSignalReleaseImageANDROID #%u: queue=%p image=%p outFd=%d -> %d",
             s_rel_count, (const void*)queue, (const void*)image,
             pNativeFenceFd ? *pNativeFenceFd : -2, (int)res);
    }
    return res;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetSwapchainGrallocUsageANDROID(
    VkDevice device,
    VkFormat format,
    VkImageUsageFlags imageUsage,
    int* pGrallocUsage) {
    InitRealVulkan();
    using Pfn = PFN_vkGetSwapchainGrallocUsageANDROID;
    Pfn realFunc = reinterpret_cast<Pfn>(
        malicompat_resolve_vendor_device_proc(device, "vkGetSwapchainGrallocUsageANDROID"));
    if (!realFunc) {
        realFunc = reinterpret_cast<Pfn>(malicompat_vendor_proc("vkGetSwapchainGrallocUsageANDROID"));
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<Pfn>(dlsym(g_vendor_vulkan, "vkGetSwapchainGrallocUsageANDROID"));
    }
    if (!realFunc) {
        LOGE("vkGetSwapchainGrallocUsageANDROID: no vendor proc");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult res = realFunc(device, format, imageUsage, pGrallocUsage);
    LOGI("vkGetSwapchainGrallocUsageANDROID: format=%d usage=0x%x -> %d (gralloc=0x%x)",
         (int)format, imageUsage, (int)res, pGrallocUsage ? *pGrallocUsage : 0);
    return res;
}

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkGetSwapchainGrallocUsage2ANDROID(
    VkDevice device,
    VkFormat format,
    VkImageUsageFlags imageUsage,
    VkSwapchainImageUsageFlagsANDROID swapchainImageUsage,
    uint64_t* pGrallocConsumerUsage,
    uint64_t* pGrallocProducerUsage) {
    InitRealVulkan();
    using Pfn = PFN_vkGetSwapchainGrallocUsage2ANDROID;
    Pfn realFunc = reinterpret_cast<Pfn>(
        malicompat_resolve_vendor_device_proc(device, "vkGetSwapchainGrallocUsage2ANDROID"));
    if (!realFunc) {
        realFunc = reinterpret_cast<Pfn>(malicompat_vendor_proc("vkGetSwapchainGrallocUsage2ANDROID"));
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<Pfn>(dlsym(g_vendor_vulkan, "vkGetSwapchainGrallocUsage2ANDROID"));
    }
    if (!realFunc) {
        LOGE("vkGetSwapchainGrallocUsage2ANDROID: no vendor proc");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkResult res = realFunc(device, format, imageUsage, swapchainImageUsage,
                            pGrallocConsumerUsage, pGrallocProducerUsage);
    LOGI("vkGetSwapchainGrallocUsage2ANDROID: format=%d -> %d", (int)format, (int)res);
    return res;
}

// M15: queue map storage lives near the top with the other globals.

MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL malicompat_vkGetDeviceQueue(
    VkDevice device,
    uint32_t queueFamilyIndex,
    uint32_t queueIndex,
    VkQueue* pQueue) {
    InitRealVulkan();
    PFN_vkGetDeviceQueue realFunc = reinterpret_cast<PFN_vkGetDeviceQueue>(
        malicompat_resolve_vendor_device_proc(device, "vkGetDeviceQueue"));
    if (!realFunc) {
        realFunc = reinterpret_cast<PFN_vkGetDeviceQueue>(
            malicompat_vendor_proc("vkGetDeviceQueue"));
    }
    if (!realFunc && g_vendor_vulkan) {
        realFunc = reinterpret_cast<PFN_vkGetDeviceQueue>(
            dlsym(g_vendor_vulkan, "vkGetDeviceQueue"));
    }
    if (!realFunc || !pQueue) {
        LOGE("vkGetDeviceQueue: no vendor proc (queueFamily=%u index=%u)", queueFamilyIndex, queueIndex);
        return;
    }
    realFunc(device, queueFamilyIndex, queueIndex, pQueue);
    if (*pQueue) {
        std::lock_guard<std::mutex> lock(g_queue_map_mutex);
        g_queue_to_device[*pQueue] = device;
        LOGI("vkGetDeviceQueue: family=%u index=%u queue=%p device=%p",
             queueFamilyIndex, queueIndex, (const void*)*pQueue, (const void*)device);
        flight::g_get_queue++;
        if (queueIndex == 0) flight::g_q0_queues++; else flight::g_q1_queues++;
        {
            static uint32_t s_qlog = 0;
            if (s_qlog < 10) {
                ++s_qlog;
                Dl_info dq{};
                const char* dqn = "?";
                void* cb = __builtin_return_address(0);
                if (cb && dladdr(cb, &dq) && dq.dli_fname) dqn = dq.dli_fname;
                flight::line("[SELF-TEST][QUEUE] vkGetDeviceQueue #%u: family=%u index=%u queue=%p caller=%s",
                             s_qlog, queueFamilyIndex, queueIndex, pQueue ? (void*)*pQueue : nullptr, dqn);
                if (queueIndex == 1) flight::line("[SELF-TEST][QUEUE] CRITICAL: second queue requested (index=1)!");
            }
        }
    }
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
    flight::g_create_instance++;
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

// M11: plain forwarders adrenotools/dlsym users need. Without these exports,
// loading our .so as THE driver yields zero physical devices (and missing
// properties/queues), killing boot before any Vulkan call.
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance i, uint32_t* c, VkPhysicalDevice* p) {
    return malicompat_vkEnumeratePhysicalDevices(i, c, p);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance i, const VkAllocationCallbacks* a) {
    auto pfn = reinterpret_cast<PFN_vkDestroyInstance>(malicompat_vendor_proc("vkDestroyInstance"));
    if (pfn) pfn(i, a);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice d, VkPhysicalDeviceProperties* p) {
    auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(malicompat_vendor_proc("vkGetPhysicalDeviceProperties"));
    if (pfn && p) pfn(d, p);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice d, uint32_t* c, VkQueueFamilyProperties* p) {
    auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(malicompat_vendor_proc("vkGetPhysicalDeviceQueueFamilyProperties"));
    if (pfn && c) pfn(d, c, p);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice d, VkPhysicalDeviceMemoryProperties* p) {
    auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(malicompat_vendor_proc("vkGetPhysicalDeviceMemoryProperties"));
    if (pfn && p) pfn(d, p);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char* l, uint32_t* c, VkExtensionProperties* p) {
    auto pfn = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(malicompat_vendor_proc("vkEnumerateInstanceExtensionProperties"));
    if (!pfn) return VK_ERROR_INITIALIZATION_FAILED;
    return pfn(l, c, p);
}

// M12: ICD entry points. Android's libvulkan loader (and adrenotools when our
// .so is loaded AS the driver) resolves vk_icdGetInstanceProcAddr, NOT plain
// vkGetInstanceProcAddr/vkCreateInstance. Without it the loader gets NULL for
// every entry -> "No Vulkan physical devices available" with zero MaliCompat
// logs (none of our code ever runs). Forward to our instance-proc router.
MALICOMPAT_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* pName) {
    return malicompat_vkGetInstanceProcAddr(instance, pName);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t* pVersion) {
    if (pVersion) {
        // Loader ICD interface: support up to v5, accept whatever loader asks.
        if (*pVersion > 5) *pVersion = 5;
    }
    return VK_SUCCESS;
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

MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* pipes) {
    return malicompat_vkCreateGraphicsPipelines(device, cache, count, infos, alloc, pipes);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo* infos, const VkAllocationCallbacks* alloc, VkPipeline* pipes) {
    return malicompat_vkCreateComputePipelines(device, cache, count, infos, alloc, pipes);
}
MALICOMPAT_EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* q) {
    malicompat_vkGetDeviceQueue(device, family, index, q);
}
// M17: VK_ANDROID_native_buffer direct bindings (GrallocUsage pair only;
// AcquireImage/SignalRelease reverted in M17b/M17e bisect).
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainGrallocUsageANDROID(VkDevice d, VkFormat f, VkImageUsageFlags u, int* g) {
    return malicompat_vkGetSwapchainGrallocUsageANDROID(d, f, u, g);
}
MALICOMPAT_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainGrallocUsage2ANDROID(VkDevice d, VkFormat f, VkImageUsageFlags u, VkSwapchainImageUsageFlagsANDROID s, uint64_t* c, uint64_t* p) {
    return malicompat_vkGetSwapchainGrallocUsage2ANDROID(d, f, u, s, c, p);
}

} // extern "C"
