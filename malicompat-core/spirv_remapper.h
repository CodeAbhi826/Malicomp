#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>
#include <vector>

namespace malicompat {

/**
 * SpirvRemapper - Runtime SPIR-V 4-Set Remapper & Vulkan 1.1 Compatibility Engine
 *
 * Implements:
 *  1. JuntarConjuntos 5-set to 4-set remapping (Sets 0..4 -> Sets 0..3)
 *     - Set 2 (Cube map textures) -> Set 1, Binding 1
 *     - Set 3 (Samplers) -> Set 2, Binding 0
 *     - Set 4 (UBO constants) -> Set 3
 *  2. SPIR-V 1.3 downgrade (0x00010300) with OpEntryPoint interface pruning
 *     (keeping only Input and Output variables for Vulkan 1.1 drivers).
 *  3. Dynamic interception for vkCreateShaderModule.
 */
class SpirvRemapper {
public:
    static constexpr uint32_t kSpirvMagic = 0x07230203;
    static constexpr uint32_t kSpirvVersion13 = 0x00010300;
    static constexpr uint32_t kOpEntryPoint = 15;
    static constexpr uint32_t kOpVariable = 59;
    static constexpr uint32_t kOpDecorate = 71;
    static constexpr uint32_t kDecorationBinding = 33;
    static constexpr uint32_t kDecorationDescriptorSet = 34;
    static constexpr uint32_t kStorageClassInput = 1;
    static constexpr uint32_t kStorageClassOutput = 3;

    static SpirvRemapper& Instance();

    bool IsSpirv(const uint32_t* pCode, size_t wordCount) const;
    uint32_t GetSpirvVersion(const uint32_t* pCode, size_t wordCount) const;

    // SPIR-V 1.3 downgrade and OpEntryPoint cleaning
    bool DowngradeToSpirv13(const uint32_t* pCode, size_t wordCount, std::vector<uint32_t>& outCode);

    // JuntarConjuntos 4-set remapping algorithm
    bool RemapToFourSets(const uint32_t* pCode, size_t wordCount, std::vector<uint32_t>& outCode);

    // Combined remapping: downgrade + 4-set reduction
    bool RemapShader(const uint32_t* pCode, size_t wordCount, std::vector<uint32_t>& outCode);

    // Interception for vkCreateShaderModule
    VkResult InterceptCreateShaderModule(
        VkDevice device,
        const VkShaderModuleCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkShaderModule* pShaderModule,
        PFN_vkCreateShaderModule realCreateShaderModule);

private:
    SpirvRemapper() = default;
};

} // namespace malicompat

#ifdef __cplusplus
extern "C" {
#endif

// Exported C hook for vkCreateShaderModule
VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateShaderModule(
    VkDevice device,
    const VkShaderModuleCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkShaderModule* pShaderModule);

#ifdef __cplusplus
}
#endif
