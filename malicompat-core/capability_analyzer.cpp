#include "capability_analyzer.h"
#include <cstring>
#include <vector>

namespace malicompat {

GpuCapabilities CapabilityAnalyzer::Inspect(VkPhysicalDevice physicalDevice, const PFN_vkGetInstanceProcAddr getProcAddr, VkInstance instance) {
    GpuCapabilities caps{};
    if (!physicalDevice || !getProcAddr || !instance) return caps;

    auto vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(getProcAddr(instance, "vkGetPhysicalDeviceProperties"));
    auto vkGetPhysicalDeviceFeatures = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(getProcAddr(instance, "vkGetPhysicalDeviceFeatures"));
    auto vkEnumerateDeviceExtensionProperties = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(getProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));

    if (!vkGetPhysicalDeviceProperties || !vkGetPhysicalDeviceFeatures) return caps;

    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physicalDevice, &props);
    std::strncpy(caps.deviceName, props.deviceName, sizeof(caps.deviceName) - 1);
    caps.apiVersion = props.apiVersion;
    caps.driverVersion = props.driverVersion;
    caps.vendorId = props.vendorID;
    caps.deviceId = props.deviceID;
    caps.maxBoundDescriptorSets = props.limits.maxBoundDescriptorSets;
    caps.isMali = (props.vendorID == 0x13B5);

    VkPhysicalDeviceFeatures feats{};
    vkGetPhysicalDeviceFeatures(physicalDevice, &feats);
    caps.independentBlend = feats.independentBlend;
    caps.shaderInt64 = feats.shaderInt64;

    uint32_t extCount = 0;
    if (vkEnumerateDeviceExtensionProperties && vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr) == VK_SUCCESS && extCount > 0) {
        std::vector<VkExtensionProperties> exts(extCount);
        if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, exts.data()) == VK_SUCCESS) {
            for (const auto& e : exts) {
                if (std::strcmp(e.extensionName, VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME) == 0) {
                    caps.hasDescriptorIndexingExt = true;
                    break;
                }
            }
        }
    }

    if (props.apiVersion >= VK_API_VERSION_1_2 || caps.hasDescriptorIndexingExt) {
        auto query = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(getProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
        if (query) {
            VkPhysicalDeviceDescriptorIndexingFeatures indexing{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
            VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            f2.pNext = &indexing;
            query(physicalDevice, &f2);
            caps.runtimeDescriptorArray = indexing.runtimeDescriptorArray;
            caps.descriptorBindingPartiallyBound = indexing.descriptorBindingPartiallyBound;
            caps.descriptorBindingSampledImageUpdateAfterBind = indexing.descriptorBindingSampledImageUpdateAfterBind;
            caps.descriptorBindingUpdateUnusedWhilePending = indexing.descriptorBindingUpdateUnusedWhilePending;
        }
    }

    return caps;
}

CompatibilityRoute CapabilityAnalyzer::RecommendRoute(const GpuCapabilities& caps) {
    if (caps.runtimeDescriptorArray && caps.descriptorBindingPartiallyBound && caps.maxBoundDescriptorSets >= 5) {
        return CompatibilityRoute::NativeDirect;
    }
    if (caps.runtimeDescriptorArray && caps.descriptorBindingPartiallyBound && caps.maxBoundDescriptorSets == 4) {
        return CompatibilityRoute::FourDescriptorPacking;
    }
    if (!caps.runtimeDescriptorArray || !caps.descriptorBindingPartiallyBound) {
        return CompatibilityRoute::DescriptorVirtualization;
    }
    return CompatibilityRoute::Unsupported;
}

bool CapabilityAnalyzer::ShouldDisableOcclusionQueries(uint32_t vendorId) {
    return vendorId == 0x13B5; // ARM Mali
}

} // namespace malicompat
