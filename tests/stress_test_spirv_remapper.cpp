#include "spirv_remapper.h"
#include <iostream>
#include <vector>
#include <cassert>
#include <cstring>

int main() {
    std::cout << "[Stress-Test-SPIRV] Starting C++ SpirvRemapper Stress Harness...\n";

    auto& remapper = malicompat::SpirvRemapper::Instance();

    // 1. Null / zero-length inputs
    assert(!remapper.IsSpirv(nullptr, 0));
    assert(!remapper.IsSpirv(nullptr, 10));
    uint32_t dummy = 0;
    assert(!remapper.IsSpirv(&dummy, 0));
    assert(!remapper.IsSpirv(&dummy, 4)); // Less than 5 header words
    assert(remapper.GetSpirvVersion(nullptr, 0) == 0);

    std::vector<uint32_t> outCode;
    assert(!remapper.DowngradeToSpirv13(nullptr, 0, outCode));
    assert(!remapper.RemapToFourSets(nullptr, 0, outCode));
    assert(!remapper.RemapShader(nullptr, 0, outCode));

    // 2. Truncated header (word count 1..4)
    uint32_t validMagicHeader[5] = {
        malicompat::SpirvRemapper::kSpirvMagic,
        0x00010500,
        1,
        10,
        0
    };
    for (size_t words = 1; words < 5; ++words) {
        assert(!remapper.IsSpirv(validMagicHeader, words));
        assert(!remapper.DowngradeToSpirv13(validMagicHeader, words, outCode));
        assert(!remapper.RemapToFourSets(validMagicHeader, words, outCode));
    }
    assert(remapper.IsSpirv(validMagicHeader, 5));

    // 3. Instruction word count = 0 (infinite loop check)
    std::vector<uint32_t> zeroLenInst = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010500, 1, 10, 0,
        (0 << 16) | malicompat::SpirvRemapper::kOpVariable // Len = 0!
    };
    assert(!remapper.DowngradeToSpirv13(zeroLenInst.data(), zeroLenInst.size(), outCode));
    assert(!remapper.RemapToFourSets(zeroLenInst.data(), zeroLenInst.size(), outCode));

    // 4. Instruction word count extending past EOF
    std::vector<uint32_t> oobInst = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010500, 1, 10, 0,
        (100 << 16) | malicompat::SpirvRemapper::kOpVariable, // Len 100 exceeds size 7!
        1
    };
    assert(!remapper.DowngradeToSpirv13(oobInst.data(), oobInst.size(), outCode));
    assert(!remapper.RemapToFourSets(oobInst.data(), oobInst.size(), outCode));

    // 5. Target ID >= bound (out-of-bounds target ID)
    std::vector<uint32_t> oobTarget = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010300, 1, 5, 0, // bound = 5
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        10, // Target ID 10 >= bound 5!
        malicompat::SpirvRemapper::kDecorationDescriptorSet,
        0
    };
    assert(!remapper.RemapToFourSets(oobTarget.data(), oobTarget.size(), outCode));

    // 6. High descriptor set indices (Set 5, Set 10, Set 100, Set 0xFFFFFFFF)
    for (uint32_t highSet : {5u, 6u, 10u, 100u, 0xFFFFFFFFu}) {
        std::vector<uint32_t> highSetSpv = {
            malicompat::SpirvRemapper::kSpirvMagic, 0x00010300, 1, 20, 0,
            (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
            1,
            malicompat::SpirvRemapper::kDecorationDescriptorSet,
            highSet,
            (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
            1,
            malicompat::SpirvRemapper::kDecorationBinding,
            0
        };
        bool remapOk = remapper.RemapToFourSets(highSetSpv.data(), highSetSpv.size(), outCode);
        assert(!remapOk); // Must gracefully return false without crashing
    }

    // 7. Duplicate descriptor set decoration on the same target
    std::vector<uint32_t> dupSetSpv = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010300, 1, 20, 0,
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        1,
        malicompat::SpirvRemapper::kDecorationDescriptorSet,
        0,
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        1,
        malicompat::SpirvRemapper::kDecorationDescriptorSet,
        1
    };
    assert(!remapper.RemapToFourSets(dupSetSpv.data(), dupSetSpv.size(), outCode));

    // 8. OpEntryPoint without null-terminator in name string
    std::vector<uint32_t> nonTermSpv = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010500, 1, 20, 0,
        (5 << 16) | malicompat::SpirvRemapper::kOpEntryPoint,
        4, // Fragment
        2, // Entry ID
        0x41414141, // 'AAAA' (no null byte)
        0x42424242  // 'BBBB' (no null byte)
    };
    bool downOk = remapper.DowngradeToSpirv13(nonTermSpv.data(), nonTermSpv.size(), outCode);
    assert(downOk);
    assert(outCode[1] == malicompat::SpirvRemapper::kSpirvVersion13);

    // 9. InterceptCreateShaderModule with null parameters
    VkShaderModule testModule = VK_NULL_HANDLE;
    VkResult resNullInfo = remapper.InterceptCreateShaderModule(
        VK_NULL_HANDLE, nullptr, nullptr, &testModule, nullptr);
    assert(resNullInfo == VK_ERROR_INITIALIZATION_FAILED);

    VkShaderModuleCreateInfo validInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    validInfo.pCode = validMagicHeader;
    validInfo.codeSize = sizeof(validMagicHeader);
    VkResult resNullMod = remapper.InterceptCreateShaderModule(
        VK_NULL_HANDLE, &validInfo, nullptr, nullptr, nullptr);
    assert(resNullMod == VK_ERROR_INITIALIZATION_FAILED);

    // 10. InterceptCreateShaderModule with high set (Set 5) fallback
    std::vector<uint32_t> highSetSpv = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010300, 1, 20, 0,
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        1,
        malicompat::SpirvRemapper::kDecorationDescriptorSet,
        5,
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        1,
        malicompat::SpirvRemapper::kDecorationBinding,
        0
    };
    VkShaderModuleCreateInfo highInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    highInfo.pCode = highSetSpv.data();
    highInfo.codeSize = highSetSpv.size() * sizeof(uint32_t);
    testModule = VK_NULL_HANDLE;
    // When realCreateShaderModule is null, fallback assigns dummy handle 0x501234
    VkResult resFallback = remapper.InterceptCreateShaderModule(
        VK_NULL_HANDLE, &highInfo, nullptr, &testModule, nullptr);
    assert(resFallback == VK_SUCCESS);
    assert(testModule == reinterpret_cast<VkShaderModule>(0x501234));

    std::cout << "[Stress-Test-SPIRV] ALL C++ SpirvRemapper Stress Tests PASSED (10/10 SUCCESS)!\n";
    return 0;
}
