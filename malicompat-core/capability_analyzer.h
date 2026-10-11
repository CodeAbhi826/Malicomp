#pragma once
#include <cstdint>
#include <string_view>
#include <vulkan/vulkan.h>

namespace malicompat {

struct GpuCapabilities {
    char deviceName[256]{};
    uint32_t apiVersion{0};
    uint32_t driverVersion{0};
    uint32_t vendorId{0};
    uint32_t deviceId{0};
    uint32_t maxBoundDescriptorSets{0};
    
    // Feature flags
    bool hasDescriptorIndexingExt{false};
    bool runtimeDescriptorArray{false};
    bool descriptorBindingPartiallyBound{false};
    bool descriptorBindingSampledImageUpdateAfterBind{false};
    bool descriptorBindingUpdateUnusedWhilePending{false};
    bool shaderInt64{false};
    bool independentBlend{false};
    bool bcLinearFiltering{false};
    bool isMali{false};
};

enum class CompatibilityRoute {
    NativeDirect,           // Fully compliant Vulkan 1.2+ device
    FourDescriptorPacking,  // Device supports 4 sets but needs binding consolidation
    DescriptorVirtualization, // Device lacks descriptor indexing (e.g. Mali-G71 r26p0)
    Unsupported
};

class CapabilityAnalyzer {
public:
    static GpuCapabilities Inspect(VkPhysicalDevice physicalDevice, const PFN_vkGetInstanceProcAddr getProcAddr, VkInstance instance);
    static CompatibilityRoute RecommendRoute(const GpuCapabilities& caps);
    static bool ShouldDisableOcclusionQueries(uint32_t vendorId);
};

} // namespace malicompat
