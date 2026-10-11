#include "spirv_remapper.h"
#include <iostream>
#include <vector>
#include <unordered_map>
#include <cassert>
#include <cstring>

int main() {
    std::cout << "[MaliCompat-Test] Starting SPIR-V 4-Set Remapper C++ Unit Test...\n";

    auto& remapper = malicompat::SpirvRemapper::Instance();

    // Test 1: Validate SPIR-V header detection
    uint32_t invalidHeader[] = { 0x12345678, 0x00010000, 0, 10, 0 };
    assert(!remapper.IsSpirv(invalidHeader, 5));
    assert(remapper.GetSpirvVersion(invalidHeader, 5) == 0);

    // Test 2: SPIR-V 1.3 Downgrade & OpEntryPoint Interface Pruning
    // Construct synthetic SPIR-V 1.5 module with OpVariables (Input, Output, Uniform, UniformConstant)
    std::vector<uint32_t> spv15 = {
        malicompat::SpirvRemapper::kSpirvMagic,
        0x00010500, // SPIR-V 1.5
        1,          // generator
        25,         // bound
        0           // schema
    };

    // OpVariable ID 10: Input (1)
    spv15.push_back((4 << 16) | malicompat::SpirvRemapper::kOpVariable);
    spv15.push_back(1); // type
    spv15.push_back(10); // ID
    spv15.push_back(malicompat::SpirvRemapper::kStorageClassInput);

    // OpVariable ID 11: Output (3)
    spv15.push_back((4 << 16) | malicompat::SpirvRemapper::kOpVariable);
    spv15.push_back(1);
    spv15.push_back(11);
    spv15.push_back(malicompat::SpirvRemapper::kStorageClassOutput);

    // OpVariable ID 12: Uniform (2)
    spv15.push_back((4 << 16) | malicompat::SpirvRemapper::kOpVariable);
    spv15.push_back(1);
    spv15.push_back(12);
    spv15.push_back(2);

    // OpVariable ID 13: UniformConstant (0)
    spv15.push_back((4 << 16) | malicompat::SpirvRemapper::kOpVariable);
    spv15.push_back(1);
    spv15.push_back(13);
    spv15.push_back(0);

    // OpEntryPoint Fragment (4), entry_id=2, name="main\0", interfaces=[10, 11, 12, 13]
    spv15.push_back((9 << 16) | malicompat::SpirvRemapper::kOpEntryPoint);
    spv15.push_back(4); // Fragment
    spv15.push_back(2); // Entry ID
    spv15.push_back(0x6e69616d); // "main"
    spv15.push_back(0x00000000); // "\0\0\0\0"
    spv15.push_back(10);
    spv15.push_back(11);
    spv15.push_back(12);
    spv15.push_back(13);

    std::vector<uint32_t> downgraded;
    bool downPass = remapper.DowngradeToSpirv13(spv15.data(), spv15.size(), downgraded);
    assert(downPass);
    assert(downgraded[1] == malicompat::SpirvRemapper::kSpirvVersion13);

    // Verify OpEntryPoint length and remaining interfaces
    bool entryFound = false;
    for (size_t i = 5; i < downgraded.size();) {
        uint32_t len = downgraded[i] >> 16;
        uint32_t op = downgraded[i] & 0xFFFF;
        if (op == malicompat::SpirvRemapper::kOpEntryPoint) {
            entryFound = true;
            assert(len == 7); // 1 (op) + 1 (exec) + 1 (id) + 2 (name) + 2 (interfaces) = 7
            assert(downgraded[i + 5] == 10);
            assert(downgraded[i + 6] == 11);
        }
        i += len;
    }
    assert(entryFound);
    std::cout << "[MaliCompat-Test] SUCCESS: SPIR-V 1.3 Downgrade & OpEntryPoint interface pruning verified!\n";

    // Test 3: JuntarConjuntos 5-set to 4-set remapping
    std::vector<uint32_t> spvSets = {
        malicompat::SpirvRemapper::kSpirvMagic,
        malicompat::SpirvRemapper::kSpirvVersion13,
        1,
        30,
        0
    };

    auto addDecorate = [&](uint32_t target, uint32_t set, uint32_t binding) {
        spvSets.push_back((4 << 16) | malicompat::SpirvRemapper::kOpDecorate);
        spvSets.push_back(target);
        spvSets.push_back(malicompat::SpirvRemapper::kDecorationDescriptorSet);
        spvSets.push_back(set);

        spvSets.push_back((4 << 16) | malicompat::SpirvRemapper::kOpDecorate);
        spvSets.push_back(target);
        spvSets.push_back(malicompat::SpirvRemapper::kDecorationBinding);
        spvSets.push_back(binding);
    };

    addDecorate(1, 0, 0); // Set 0 Binding 0 (2D texture)
    addDecorate(2, 1, 0); // Set 1 Binding 0 (3D texture)
    addDecorate(3, 2, 0); // Set 2 Binding 0 (Cube map texture)
    addDecorate(4, 3, 0); // Set 3 Binding 0 (Sampler)
    addDecorate(5, 4, 0); // Set 4 Binding 0 (UBO 0)
    addDecorate(6, 4, 1); // Set 4 Binding 1 (UBO 1)
    addDecorate(7, 4, 2); // Set 4 Binding 2 (UBO 2)

    std::vector<uint32_t> remappedSets;
    bool remapPass = remapper.RemapToFourSets(spvSets.data(), spvSets.size(), remappedSets);
    assert(remapPass);

    // Verify remapped sets and bindings
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> results;
    for (size_t i = 5; i < remappedSets.size();) {
        uint32_t len = remappedSets[i] >> 16;
        uint32_t op = remappedSets[i] & 0xFFFF;
        if (op == malicompat::SpirvRemapper::kOpDecorate && len == 4) {
            uint32_t target = remappedSets[i + 1];
            uint32_t dec = remappedSets[i + 2];
            uint32_t val = remappedSets[i + 3];
            if (dec == malicompat::SpirvRemapper::kDecorationDescriptorSet) {
                results[target].first = val;
            } else if (dec == malicompat::SpirvRemapper::kDecorationBinding) {
                results[target].second = val;
            }
        }
        i += len;
    }

    assert(results[1].first == 0 && results[1].second == 0); // Set 0, Binding 0
    assert(results[2].first == 1 && results[2].second == 0); // Set 1, Binding 0
    assert(results[3].first == 1 && results[3].second == 1); // Cube -> Set 1, Binding 1!
    assert(results[4].first == 2 && results[4].second == 0); // Sampler -> Set 2, Binding 0!
    assert(results[5].first == 3 && results[5].second == 0); // UBO 0 -> Set 3, Binding 0!
    assert(results[6].first == 3 && results[6].second == 1); // UBO 1 -> Set 3, Binding 1!
    assert(results[7].first == 3 && results[7].second == 2); // UBO 2 -> Set 3, Binding 2!

    std::cout << "[MaliCompat-Test] SUCCESS: JuntarConjuntos 5-set to 4-set remapping verified!\n";

    // Test 4: Dynamic interception of vkCreateShaderModule
    VkShaderModuleCreateInfo createInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    createInfo.pCode = spv15.data();
    createInfo.codeSize = spv15.size() * sizeof(uint32_t);

    VkShaderModule module{VK_NULL_HANDLE};
    VkResult res = remapper.InterceptCreateShaderModule(
        VK_NULL_HANDLE, &createInfo, nullptr, &module, nullptr);
    assert(res == VK_SUCCESS);
    assert(module != VK_NULL_HANDLE);

    std::cout << "[MaliCompat-Test] SUCCESS: InterceptCreateShaderModule verified!\n";
    std::cout << "[MaliCompat-Test] ALL SPIR-V Remapper Tests PASSED (100% SUCCESS)!\n";
    return 0;
}
