#include "descriptor_virtualizer.h"
#include <unordered_map>

namespace malicompat {

DescriptorVirtualizer::DescriptorVirtualizer(uint32_t maxSets) : maxSets_(maxSets) {}

bool DescriptorVirtualizer::RemapSpirvForFourSets(std::span<uint32_t> spirv) {
    if (spirv.size() < 5 || spirv[0] != 0x07230203) return false;
    std::unordered_map<uint32_t, uint32_t> sets;
    
    // First pass: locate OpDecorate DescriptorSet (34) and Binding (33)
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t len = spirv[i] >> 16;
        const uint32_t op = spirv[i] & 0xFFFF;
        if (!len || len > spirv.size() - i) return false;
        if (op == 71 /* OpDecorate */) {
            if (len >= 4 && (spirv[i + 2] == 33 || spirv[i + 2] == 34)) {
                if (spirv[i + 2] == 34) {
                    sets.emplace(spirv[i + 1], spirv[i + 3]);
                }
            }
        }
        i += len;
    }

    // Second pass: redecorate sets >= 2 down by 1, and shift cubemaps into binding 1 of set 1
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t len = spirv[i] >> 16;
        const uint32_t op = spirv[i] & 0xFFFF;
        if (op == 71 && len == 4) {
            auto it = sets.find(spirv[i + 1]);
            if (it != sets.end()) {
                if (spirv[i + 2] == 34 && it->second >= 2) {
                    spirv[i + 3] = it->second - 1;
                } else if (spirv[i + 2] == 33 && it->second == 2) {
                    ++spirv[i + 3];
                }
            }
        }
        i += len;
    }

    return true;
}

bool DescriptorVirtualizer::NeedsBindlessEmulation(std::span<const uint32_t> spirv) {
    if (spirv.size() < 5 || spirv[0] != 0x07230203) return false;
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t len = spirv[i] >> 16;
        const uint32_t op = spirv[i] & 0xFFFF;
        if (!len || len > spirv.size() - i) return false;
        // Check for OpCapability RuntimeDescriptorArray (5302)
        if (op == 17 /* OpCapability */ && len >= 2 && spirv[i + 1] == 5302) {
            return true;
        }
        i += len;
    }
    return false;
}

} // namespace malicompat
