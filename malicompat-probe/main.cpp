#include "capability_analyzer.h"
#include <iostream>
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
    std::cout << "===============================================================\n\n";

    vkDestroyInstance(instance, nullptr);
    dlclose(libvulkan);
    return 0;
}
