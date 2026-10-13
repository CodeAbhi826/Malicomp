#include "capability_analyzer.h"
#include <cstring>
#include <iostream>
#include <vector>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

int main() {
    std::cout << "[MaliCompat-Probe] Starting standalone GPU probe...\n";

    void* libvulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!libvulkan) {
        std::cerr << "[MaliCompat-Probe] Error: Cannot load libvulkan.so: " << dlerror() << "\n";
        return 1;
    }

    auto vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(libvulkan, "vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr) {
        std::cerr << "[MaliCompat-Probe] Error: vkGetInstanceProcAddr not found\n";
        dlclose(libvulkan);
        return 1;
    }

    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));
    if (!vkCreateInstance) {
        std::cerr << "[MaliCompat-Probe] Error: vkCreateInstance not found\n";
        dlclose(libvulkan);
        return 1;
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "MaliCompat-Probe";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;

    VkInstance instance{VK_NULL_HANDLE};
    VkResult res = vkCreateInstance(&createInfo, nullptr, &instance);
    if (res != VK_SUCCESS) {
        std::cerr << "[MaliCompat-Probe] Error: vkCreateInstance failed with code " << res << "\n";
        dlclose(libvulkan);
        return 1;
    }

    auto vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    auto vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(vkGetInstanceProcAddr(instance, "vkDestroyInstance"));

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (count == 0) {
        std::cerr << "[MaliCompat-Probe] Error: No Vulkan physical devices found\n";
        vkDestroyInstance(instance, nullptr);
        dlclose(libvulkan);
        return 1;
    }

    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());

    VkPhysicalDevice gpu = devices[0];
    auto caps = malicompat::CapabilityAnalyzer::Inspect(gpu, vkGetInstanceProcAddr, instance);

    std::cout << "\n================ MaliCompat Hardware Telemetry ================\n";
    std::cout << "Device Name:            " << caps.deviceName << "\n";
    std::cout << "Vulkan API:             " << VK_VERSION_MAJOR(caps.apiVersion) << "." 
              << VK_VERSION_MINOR(caps.apiVersion) << "." << VK_VERSION_PATCH(caps.apiVersion) << "\n";
    std::cout << "Driver Version:         " << caps.driverVersion << "\n";
    std::cout << "Vendor ID:              0x" << std::hex << caps.vendorId << std::dec << "\n";
    std::cout << "maxBoundDescriptorSets: " << caps.maxBoundDescriptorSets << "\n";
    std::cout << "Descriptor Indexing:    " << (caps.hasDescriptorIndexingExt ? "Supported (Extension)" : "None") << "\n";
    std::cout << "runtimeDescriptorArray: " << (caps.runtimeDescriptorArray ? "Supported" : "Unsupported") << "\n";
    std::cout << "partiallyBound:         " << (caps.descriptorBindingPartiallyBound ? "Supported" : "Unsupported") << "\n";
    std::cout << "updateAfterBind:        " << (caps.descriptorBindingSampledImageUpdateAfterBind ? "Supported" : "Unsupported") << "\n";
    
    auto route = malicompat::CapabilityAnalyzer::RecommendRoute(caps);
    std::cout << "\nCompatibility Route:    ";
    switch (route) {
        case malicompat::CompatibilityRoute::NativeDirect:
            std::cout << "Native Direct (Fully Compliant)\n"; break;
        case malicompat::CompatibilityRoute::FourDescriptorPacking:
            std::cout << "Four-Descriptor Packing\n"; break;
        case malicompat::CompatibilityRoute::DescriptorVirtualization:
            std::cout << "Descriptor Virtualization (Shim Required)\n"; break;
        default:
            std::cout << "Unsupported\n"; break;
    }
    std::cout << "Occlusion Queries:      " << (malicompat::CapabilityAnalyzer::ShouldDisableOcclusionQueries(caps.vendorId) ? "Disabled (Quirk Active)" : "Enabled") << "\n";

    // M3 gate telemetry: the exact CreateIfSupported gates for this GPU.
    {
        auto vkGetPhysicalDeviceFeatures =
            reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures"));
        auto vkEnumerateDeviceExtensionProperties =
            reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(vkGetInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));
        auto vkGetPhysicalDeviceQueueFamilyProperties =
            reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
        std::cout << "--- CreateIfSupported gates ---\n";
        if (vkGetPhysicalDeviceFeatures) {
            VkPhysicalDeviceFeatures f{};
            vkGetPhysicalDeviceFeatures(gpu, &f);
            std::cout << "independentBlend: " << (f.independentBlend ? "yes" : "NO") << "\n";
            std::cout << "fragmentStoresAndAtomics: " << (f.fragmentStoresAndAtomics ? "yes" : "NO") << "\n";
            std::cout << "vertexPipelineStoresAndAtomics: " << (f.vertexPipelineStoresAndAtomics ? "yes" : "NO") << "\n";
            std::cout << "geometryShader: " << (f.geometryShader ? "yes" : "NO") << "\n";
            std::cout << "fillModeNonSolid: " << (f.fillModeNonSolid ? "yes" : "NO") << "\n";
            std::cout << "samplerAnisotropy: " << (f.samplerAnisotropy ? "yes" : "NO") << "\n";
            std::cout << "sparseBinding: " << (f.sparseBinding ? "yes" : "NO") << "\n";
        }
        if (vkEnumerateDeviceExtensionProperties) {
            uint32_t n = 0;
            if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, nullptr) == VK_SUCCESS) {
                std::vector<VkExtensionProperties> exts(n);
                if (vkEnumerateDeviceExtensionProperties(gpu, nullptr, &n, exts.data()) == VK_SUCCESS) {
                    bool swap = false;
                    std::cout << "device extensions (" << n << "):\n";
                    for (const auto& e : exts) {
                        if (std::strcmp(e.extensionName, "VK_KHR_swapchain") == 0) swap = true;
                        std::cout << "  " << e.extensionName << "\n";
                    }
                    std::cout << "VK_KHR_swapchain: " << (swap ? "yes" : "MISSING") << "\n";
                }
            }
        }
        if (vkGetPhysicalDeviceQueueFamilyProperties) {
            uint32_t n = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
            std::vector<VkQueueFamilyProperties> qs(n);
            vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, qs.data());
            std::cout << "queue families (" << n << "):\n";
            for (uint32_t i = 0; i < n; ++i) {
                std::cout << "  [" << i << "] flags=0x" << std::hex << qs[i].queueFlags << std::dec
                          << " count=" << qs[i].queueCount << "\n";
            }
        }
    }
    std::cout << "===============================================================\n\n";

    vkDestroyInstance(instance, nullptr);
    dlclose(libvulkan);
    return 0;
}
