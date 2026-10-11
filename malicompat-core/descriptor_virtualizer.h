#pragma once
#include <cstdint>
#include <vector>
#include <span>

namespace malicompat {

/**
 * DescriptorVirtualizer manages traditional descriptor bindings
 * on hardware that lacks VK_EXT_descriptor_indexing or UPDATE_AFTER_BIND.
 * 
 * Instead of exposing a giant dynamic array to the shader, it virtualizes
 * descriptor slots into compact batches pre-recorded before draw calls.
 */
class DescriptorVirtualizer {
public:
    DescriptorVirtualizer(uint32_t maxSets = 4);
    ~DescriptorVirtualizer() = default;

    // Checks if SPIR-V needs instruction transformation for 4-set packing
    static bool RemapSpirvForFourSets(std::span<uint32_t> spirvWords);
    
    // Checks if shader uses bindless indexing and requires static expansion
    static bool NeedsBindlessEmulation(std::span<const uint32_t> spirvWords);

private:
    uint32_t maxSets_{4};
};

} // namespace malicompat
