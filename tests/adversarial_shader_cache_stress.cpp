#include "spirv_remapper.h"
#include "vulkan_layer_exports.h"
#include <iostream>
#include <vector>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>
#include <chrono>

enum class TestOutcome {
    PASSED,
    CRASHED,
    TIMED_OUT,
    ASSERTION_FAILED
};

struct TestReport {
    std::string name;
    TestOutcome outcome;
    std::string detail;
};

TestOutcome run_in_subprocess(void (*test_fn)(), int timeout_sec, std::string& out_detail) {
    pid_t pid = fork();
    if (pid < 0) {
        out_detail = "fork failed";
        return TestOutcome::ASSERTION_FAILED;
    }

    if (pid == 0) {
        alarm(timeout_sec + 2);
        test_fn();
        _exit(0);
    }

    int status = 0;
    auto start = std::chrono::steady_clock::now();

    while (true) {
        pid_t res = waitpid(pid, &status, WNOHANG);
        if (res == pid) {
            break;
        } else if (res < 0) {
            out_detail = "waitpid error";
            return TestOutcome::ASSERTION_FAILED;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
        if (elapsed >= timeout_sec) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            out_detail = "Execution timed out after " + std::to_string(timeout_sec) + "s (INFINITE LOOP / HANG detected)";
            return TestOutcome::TIMED_OUT;
        }
        usleep(10000); // 10ms
    }

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        if (code == 0) {
            out_detail = "Clean exit (code 0)";
            return TestOutcome::PASSED;
        } else {
            out_detail = "Child exited with error code " + std::to_string(code);
            return TestOutcome::ASSERTION_FAILED;
        }
    } else if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        out_detail = "Child terminated by signal " + std::to_string(sig) + " (" + strsignal(sig) + ")";
        return TestOutcome::CRASHED;
    }

    out_detail = "Unknown child status";
    return TestOutcome::ASSERTION_FAILED;
}

// ============================================================================
// 1. malicompat_vkCreateShaderModule - Null & Malformed Pointer Stress
// ============================================================================

void test_shader_null_pcreateinfo() {
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, nullptr, nullptr, &module);
    assert(res == VK_ERROR_INITIALIZATION_FAILED);
    assert(module == VK_NULL_HANDLE);
}

void test_shader_null_pshadermodule() {
    uint32_t header[5] = {malicompat::SpirvRemapper::kSpirvMagic, 0x00010300, 1, 10, 0};
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.pCode = header;
    info.codeSize = sizeof(header);
    VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, nullptr);
    assert(res == VK_ERROR_INITIALIZATION_FAILED);
}

void test_shader_null_both() {
    VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, nullptr, nullptr, nullptr);
    assert(res == VK_ERROR_INITIALIZATION_FAILED);
}

void test_shader_null_pcode_with_codesize() {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.pCode = nullptr;
    info.codeSize = 64; // Non-zero size with null code pointer
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, &module);
    // Must not crash or dereference null pCode
}

void test_shader_zero_codesize() {
    uint32_t header[5] = {malicompat::SpirvRemapper::kSpirvMagic, 0x00010300, 1, 10, 0};
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.pCode = header;
    info.codeSize = 0; // Zero size
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, &module);
    // Must not crash
}

void test_shader_unaligned_codesize() {
    uint8_t rawBytes[17] = {0};
    for (size_t sz : {1, 2, 3, 5, 7, 13, 17}) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.pCode = reinterpret_cast<const uint32_t*>(rawBytes);
        info.codeSize = sz; // Unaligned byte sizes
        VkShaderModule module = VK_NULL_HANDLE;
        VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, &module);
        // Must handle without reading past buffer
    }
}

// ============================================================================
// 2. malicompat_vkCreateShaderModule - Invalid SPIR-V Stream Stress
// ============================================================================

void test_shader_corrupted_magic() {
    for (uint32_t badMagic : {0u, 0xFFFFFFFFu, 0x12345678u, 0x03022307u}) {
        uint32_t spv[5] = {badMagic, 0x00010300, 1, 10, 0};
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.pCode = spv;
        info.codeSize = sizeof(spv);
        VkShaderModule module = VK_NULL_HANDLE;
        VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, &module);
        // Non-SPIR-V must fall through safely without crashing
    }
}

void test_shader_truncated_header() {
    uint32_t spv[5] = {malicompat::SpirvRemapper::kSpirvMagic, 0x00010300, 1, 10, 0};
    for (size_t words = 1; words < 5; ++words) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.pCode = spv;
        info.codeSize = words * sizeof(uint32_t);
        VkShaderModule module = VK_NULL_HANDLE;
        VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, &module);
        // Must safely handle truncated header < 5 words
    }
}

void test_shader_zero_length_instruction_hang() {
    // Instruction length = 0 (infinite loop hazard)
    std::vector<uint32_t> spv = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010500, 1, 10, 0,
        (0 << 16) | malicompat::SpirvRemapper::kOpDecorate // Len = 0!
    };
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.pCode = spv.data();
    info.codeSize = spv.size() * sizeof(uint32_t);
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, &module);
    // Must NOT hang in infinite loop!
}

void test_shader_instruction_len_past_eof() {
    // Instruction length = 50 words, but buffer only has 6 words
    std::vector<uint32_t> spv = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010500, 1, 10, 0,
        (50 << 16) | malicompat::SpirvRemapper::kOpDecorate // Len = 50 > buffer
    };
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.pCode = spv.data();
    info.codeSize = spv.size() * sizeof(uint32_t);
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, &module);
    // Must NOT buffer overrun!
}

void test_shader_malformed_opentrypoint_nonterminating() {
    // OpEntryPoint with string that lacks null terminator before EOF
    std::vector<uint32_t> spv = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010500, 1, 20, 0,
        (6 << 16) | malicompat::SpirvRemapper::kOpEntryPoint,
        4, // Fragment
        1, // Entry ID
        0x58585858, // 'XXXX' (no null)
        0x59595959, // 'YYYY' (no null)
        0x5a5a5a5a  // 'ZZZZ' (no null)
    };
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.pCode = spv.data();
    info.codeSize = spv.size() * sizeof(uint32_t);
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, &module);
    assert(res == VK_SUCCESS);
}

// ============================================================================
// 3. malicompat_vkCreateShaderModule - 5-Set Modules & Extreme Set Indices
// ============================================================================

void test_shader_5_set_module_remapping() {
    std::vector<uint32_t> spv = {
        malicompat::SpirvRemapper::kSpirvMagic,
        0x00010500, // SPIR-V 1.5
        1,          // generator
        50,         // bound
        0           // schema
    };

    // OpVariables: Input (10), Output (11), Uniform (12), UniformConstant (13)
    auto addVar = [&](uint32_t id, uint32_t sc) {
        spv.push_back((4 << 16) | malicompat::SpirvRemapper::kOpVariable);
        spv.push_back(1); // type
        spv.push_back(id);
        spv.push_back(sc);
    };
    addVar(10, malicompat::SpirvRemapper::kStorageClassInput);
    addVar(11, malicompat::SpirvRemapper::kStorageClassOutput);
    addVar(12, 2); // Uniform
    addVar(13, 0); // UniformConstant

    // OpEntryPoint Fragment (4), entry_id=2, name="main\0", interfaces=[10, 11, 12, 13]
    spv.push_back((9 << 16) | malicompat::SpirvRemapper::kOpEntryPoint);
    spv.push_back(4);
    spv.push_back(2);
    spv.push_back(0x6e69616d); // "main"
    spv.push_back(0x00000000); // "\0\0\0\0"
    spv.push_back(10);
    spv.push_back(11);
    spv.push_back(12);
    spv.push_back(13);

    // Decorate 5 sets: Set 0, 1, 2, 3, 4
    auto addDecorate = [&](uint32_t target, uint32_t set, uint32_t binding) {
        spv.push_back((4 << 16) | malicompat::SpirvRemapper::kOpDecorate);
        spv.push_back(target);
        spv.push_back(malicompat::SpirvRemapper::kDecorationDescriptorSet);
        spv.push_back(set);

        spv.push_back((4 << 16) | malicompat::SpirvRemapper::kOpDecorate);
        spv.push_back(target);
        spv.push_back(malicompat::SpirvRemapper::kDecorationBinding);
        spv.push_back(binding);
    };

    addDecorate(20, 0, 0); // Set 0, Binding 0 (2D Texture)
    addDecorate(21, 1, 0); // Set 1, Binding 0 (3D Texture)
    addDecorate(22, 2, 0); // Set 2, Binding 0 (Cube Texture) -> Remapped to Set 1, Binding 1
    addDecorate(23, 3, 0); // Set 3, Binding 0 (Sampler)      -> Remapped to Set 2, Binding 0
    addDecorate(24, 4, 0); // Set 4, Binding 0 (UBO 0)        -> Remapped to Set 3, Binding 0
    addDecorate(25, 4, 1); // Set 4, Binding 1 (UBO 1)        -> Remapped to Set 3, Binding 1

    std::vector<uint32_t> outCode;
    bool remapOk = malicompat::SpirvRemapper::Instance().RemapShader(spv.data(), spv.size(), outCode);
    assert(remapOk);
    assert(outCode[1] == malicompat::SpirvRemapper::kSpirvVersion13);

    // Verify all sets <= 3
    for (size_t i = 5; i < outCode.size();) {
        uint32_t len = outCode[i] >> 16;
        uint32_t op = outCode[i] & 0xFFFF;
        if (op == malicompat::SpirvRemapper::kOpDecorate && len == 4) {
            uint32_t dec = outCode[i + 2];
            uint32_t val = outCode[i + 3];
            if (dec == malicompat::SpirvRemapper::kDecorationDescriptorSet) {
                assert(val <= 3); // Must strictly satisfy Mali maxBoundDescriptorSets == 4 (Sets 0..3)!
            }
        }
        i += len;
    }

    // Now test through malicompat_vkCreateShaderModule
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.pCode = spv.data();
    info.codeSize = spv.size() * sizeof(uint32_t);
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, &module);
    assert(res == VK_SUCCESS);
    assert(module != VK_NULL_HANDLE);
}

void test_shader_extreme_set_indices() {
    auto& remapper = malicompat::SpirvRemapper::Instance();

    // Extreme set indices: Set 5, Set 10, Set 100, Set 1000, Set 65535, Set 0xFFFFFFFF
    for (uint32_t extremeSet : {5u, 6u, 10u, 100u, 1000u, 65535u, 0xFFFFFFFFu}) {
        std::vector<uint32_t> spv = {
            malicompat::SpirvRemapper::kSpirvMagic, 0x00010300, 1, 30, 0,
            (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
            1,
            malicompat::SpirvRemapper::kDecorationDescriptorSet,
            extremeSet,
            (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
            1,
            malicompat::SpirvRemapper::kDecorationBinding,
            0
        };

        std::vector<uint32_t> outCode;
        bool remapOk = remapper.RemapToFourSets(spv.data(), spv.size(), outCode);
        assert(!remapOk); // Must gracefully refuse to remap sets > 4 without crashing

        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        info.pCode = spv.data();
        info.codeSize = spv.size() * sizeof(uint32_t);
        VkShaderModule module = VK_NULL_HANDLE;
        VkResult res = malicompat_vkCreateShaderModule(VK_NULL_HANDLE, &info, nullptr, &module);
        // Must safely handle without crashing
    }
}

void test_shader_duplicate_decorations() {
    // Same target ID decorated twice with different descriptor sets
    std::vector<uint32_t> spv = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010300, 1, 20, 0,
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        1, malicompat::SpirvRemapper::kDecorationDescriptorSet, 0,
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        1, malicompat::SpirvRemapper::kDecorationDescriptorSet, 2
    };

    std::vector<uint32_t> outCode;
    bool remapOk = malicompat::SpirvRemapper::Instance().RemapToFourSets(spv.data(), spv.size(), outCode);
    assert(!remapOk); // Conflict detected
}

void test_shader_massive_binding_index() {
    // Binding 0xFFFFFFFF on Set 2 (Cube map -> binding += 1)
    std::vector<uint32_t> spv = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010300, 1, 20, 0,
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        1, malicompat::SpirvRemapper::kDecorationDescriptorSet, 2,
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        1, malicompat::SpirvRemapper::kDecorationBinding, 0xFFFFFFFFu
    };

    std::vector<uint32_t> outCode;
    bool remapOk = malicompat::SpirvRemapper::Instance().RemapToFourSets(spv.data(), spv.size(), outCode);
    assert(remapOk);
    // Verify outCode binding wrapped or preserved without crash/UBSan error
    for (size_t i = 5; i < outCode.size();) {
        uint32_t len = outCode[i] >> 16;
        uint32_t op = outCode[i] & 0xFFFF;
        if (op == malicompat::SpirvRemapper::kOpDecorate && len == 4) {
            uint32_t dec = outCode[i + 2];
            uint32_t val = outCode[i + 3];
            if (dec == malicompat::SpirvRemapper::kDecorationBinding) {
                // val is (0xFFFFFFFF + 1) == 0 (well-defined unsigned wrap)
                assert(val == 0);
            }
        }
        i += len;
    }
}

// ============================================================================
// 4. Pipeline Cache Functions - Null Handles, Empty, & Corrupted Payloads
// ============================================================================

void test_pipeline_cache_create_null_handles() {
    VkPipelineCache cache = VK_NULL_HANDLE;
    VkPipelineCacheCreateInfo createInfo{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    createInfo.initialDataSize = 0;
    createInfo.pInitialData = nullptr;

    // Null device
    VkResult res1 = malicompat_vkCreatePipelineCache(VK_NULL_HANDLE, &createInfo, nullptr, &cache);
    assert(res1 == VK_SUCCESS);
    assert(cache != VK_NULL_HANDLE);

    // Null pPipelineCache pointer
    VkResult res2 = malicompat_vkCreatePipelineCache(VK_NULL_HANDLE, &createInfo, nullptr, nullptr);
    assert(res2 == VK_SUCCESS);
}

void test_pipeline_cache_create_corrupted_payload() {
    VkPipelineCache cache = VK_NULL_HANDLE;

    // 1. Corrupted truncated header (< 16 bytes)
    uint8_t garbage4[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    VkPipelineCacheCreateInfo info1{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info1.initialDataSize = sizeof(garbage4);
    info1.pInitialData = garbage4;
    VkResult res1 = malicompat_vkCreatePipelineCache(VK_NULL_HANDLE, &info1, nullptr, &cache);
    assert(res1 == VK_SUCCESS);

    // 2. Corrupted 32-byte header with invalid magic/vendor
    uint32_t fakeHeader[8] = {
        32,         // Header length
        1,          // Header version (VK_PIPELINE_CACHE_HEADER_VERSION_ONE)
        0x13B5,     // Vendor ID ARM (or corrupt vendor 0xFFFF)
        0x7100,     // Device ID
        0x12345678, // Pipeline cache UUID byte 0..3
        0x9abcdef0,
        0x11223344,
        0x55667788
    };
    fakeHeader[1] = 0xBADF00D; // Corrupted header version!
    VkPipelineCacheCreateInfo info2{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info2.initialDataSize = sizeof(fakeHeader);
    info2.pInitialData = fakeHeader;
    VkResult res2 = malicompat_vkCreatePipelineCache(VK_NULL_HANDLE, &info2, nullptr, &cache);
    assert(res2 == VK_SUCCESS);

    // 3. Null pInitialData with non-zero initialDataSize
    VkPipelineCacheCreateInfo info3{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info3.initialDataSize = 1024;
    info3.pInitialData = nullptr; // NULL with size 1024!
    VkResult res3 = malicompat_vkCreatePipelineCache(VK_NULL_HANDLE, &info3, nullptr, &cache);
    assert(res3 == VK_SUCCESS);
}

void test_pipeline_cache_get_data_nulls() {
    VkPipelineCache cache = reinterpret_cast<VkPipelineCache>(0x301234);

    // Null pDataSize pointer
    VkResult res1 = malicompat_vkGetPipelineCacheData(VK_NULL_HANDLE, cache, nullptr, nullptr);
    assert(res1 == VK_SUCCESS);

    // Valid pDataSize, null pData (Standard Vulkan size inquiry)
    size_t dataSize = 999;
    VkResult res2 = malicompat_vkGetPipelineCacheData(VK_NULL_HANDLE, cache, &dataSize, nullptr);
    assert(res2 == VK_SUCCESS);
    assert(dataSize == 0); // Mock HAL returns size 0

    // Valid pDataSize and valid pData buffer
    uint8_t buffer[64] = {0};
    dataSize = sizeof(buffer);
    VkResult res3 = malicompat_vkGetPipelineCacheData(VK_NULL_HANDLE, cache, &dataSize, buffer);
    assert(res3 == VK_SUCCESS);
    assert(dataSize == 0);
}

void test_pipeline_cache_destroy() {
    // Null handles
    malicompat_vkDestroyPipelineCache(VK_NULL_HANDLE, VK_NULL_HANDLE, nullptr);

    // Mock handle
    malicompat_vkDestroyPipelineCache(VK_NULL_HANDLE, reinterpret_cast<VkPipelineCache>(0x301234), nullptr);

    // Arbitrary invalid handle
    malicompat_vkDestroyPipelineCache(VK_NULL_HANDLE, reinterpret_cast<VkPipelineCache>(0xDEADBEEFULL), nullptr);
}

void test_pipeline_cache_merge() {
    VkPipelineCache dst = reinterpret_cast<VkPipelineCache>(0x301234);
    VkPipelineCache srcs[3] = {
        reinterpret_cast<VkPipelineCache>(0x301234),
        VK_NULL_HANDLE,
        reinterpret_cast<VkPipelineCache>(0x501234)
    };

    // Count = 0, null array
    VkResult res1 = malicompat_vkMergePipelineCaches(VK_NULL_HANDLE, dst, 0, nullptr);
    assert(res1 == VK_SUCCESS);

    // Count = 3, null array (invalid Vulkan usage)
    VkResult res2 = malicompat_vkMergePipelineCaches(VK_NULL_HANDLE, dst, 3, nullptr);
    assert(res2 == VK_SUCCESS);

    // Count = 3, array with nulls
    VkResult res3 = malicompat_vkMergePipelineCaches(VK_NULL_HANDLE, dst, 3, srcs);
    assert(res3 == VK_SUCCESS);
}

// ============================================================================
// 5. Critical Real Driver Forwarding & Linkage Verification
// ============================================================================

void test_driver_forwarding_contract() {
    // Verify whether realCreateShaderModule is invoked when provided
    static bool realCreateCalled = false;
    static uint32_t interceptedWordCount = 0;
    static uint32_t interceptedVersion = 0;

    auto mockRealCreate = [](VkDevice, const VkShaderModuleCreateInfo* pCreateInfo, const VkAllocationCallbacks*, VkShaderModule* pShaderModule) -> VkResult {
        realCreateCalled = true;
        if (pCreateInfo && pCreateInfo->pCode) {
            interceptedWordCount = pCreateInfo->codeSize / sizeof(uint32_t);
            interceptedVersion = pCreateInfo->pCode[1];
        }
        if (pShaderModule) {
            *pShaderModule = reinterpret_cast<VkShaderModule>(0x778899);
        }
        return VK_SUCCESS;
    };

    // SPIR-V 1.5 shader with Set 4
    std::vector<uint32_t> spv = {
        malicompat::SpirvRemapper::kSpirvMagic, 0x00010500, 1, 20, 0,
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        1, malicompat::SpirvRemapper::kDecorationDescriptorSet, 4,
        (4 << 16) | malicompat::SpirvRemapper::kOpDecorate,
        1, malicompat::SpirvRemapper::kDecorationBinding, 0
    };

    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.pCode = spv.data();
    info.codeSize = spv.size() * sizeof(uint32_t);
    VkShaderModule module = VK_NULL_HANDLE;

    // Test InterceptCreateShaderModule directly with mock real driver
    VkResult res = malicompat::SpirvRemapper::Instance().InterceptCreateShaderModule(
        VK_NULL_HANDLE, &info, nullptr, &module, mockRealCreate);

    assert(res == VK_SUCCESS);
    assert(realCreateCalled == true);
    assert(module == reinterpret_cast<VkShaderModule>(0x778899));
    assert(interceptedVersion == malicompat::SpirvRemapper::kSpirvVersion13);
}

// ============================================================================
// Main Stress Test Runner
// ============================================================================

int main() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "   MALICOMPAT CHALLENGER ADVERSARIAL SHADER & CACHE HARNESS       " << std::endl;
    std::cout << "==================================================================" << std::endl;

    struct TestCase {
        std::string name;
        void (*fn)();
        int timeout_sec;
    };

    std::vector<TestCase> cases = {
        // Pointer & Size Robustness
        {"1. malicompat_vkCreateShaderModule: null pCreateInfo", test_shader_null_pcreateinfo, 2},
        {"2. malicompat_vkCreateShaderModule: null pShaderModule", test_shader_null_pshadermodule, 2},
        {"3. malicompat_vkCreateShaderModule: null pCreateInfo & pShaderModule", test_shader_null_both, 2},
        {"4. malicompat_vkCreateShaderModule: null pCode with non-zero codeSize", test_shader_null_pcode_with_codesize, 2},
        {"5. malicompat_vkCreateShaderModule: zero codeSize", test_shader_zero_codesize, 2},
        {"6. malicompat_vkCreateShaderModule: unaligned codeSize (1, 2, 3, 5, 7 bytes)", test_shader_unaligned_codesize, 2},

        // Invalid SPIR-V Stream
        {"7. malicompat_vkCreateShaderModule: corrupted magic bytes", test_shader_corrupted_magic, 2},
        {"8. malicompat_vkCreateShaderModule: truncated headers (1..4 words)", test_shader_truncated_header, 2},
        {"9. malicompat_vkCreateShaderModule: zero length instruction (infinite loop)", test_shader_zero_length_instruction_hang, 2},
        {"10. malicompat_vkCreateShaderModule: instruction length exceeding buffer", test_shader_instruction_len_past_eof, 2},
        {"11. malicompat_vkCreateShaderModule: non-null-terminated OpEntryPoint name", test_shader_malformed_opentrypoint_nonterminating, 2},

        // 5-Set SPIR-V & Extreme Sets
        {"12. malicompat_vkCreateShaderModule: full 5-set (Sets 0..4) remapping to Sets 0..3", test_shader_5_set_module_remapping, 3},
        {"13. malicompat_vkCreateShaderModule: extreme descriptor sets (Set 5..0xFFFFFFFF)", test_shader_extreme_set_indices, 3},
        {"14. malicompat_vkCreateShaderModule: duplicate descriptor set decorations", test_shader_duplicate_decorations, 2},
        {"15. malicompat_vkCreateShaderModule: massive binding index (0xFFFFFFFF)", test_shader_massive_binding_index, 2},

        // Pipeline Cache Functions
        {"16. vkCreatePipelineCache: null handles & null pPipelineCache", test_pipeline_cache_create_null_handles, 2},
        {"17. vkCreatePipelineCache: corrupted & truncated initial cache payloads", test_pipeline_cache_create_corrupted_payload, 2},
        {"18. vkGetPipelineCacheData: null handles, null pDataSize, inquiry queries", test_pipeline_cache_get_data_nulls, 2},
        {"19. vkDestroyPipelineCache: null & invalid handles", test_pipeline_cache_destroy, 2},
        {"20. vkMergePipelineCaches: null arrays, null handles, count > 0", test_pipeline_cache_merge, 2},

        // Real Driver Forwarding & Linkage Contract
        {"21. Real driver interception & SPIR-V remapping forwarding", test_driver_forwarding_contract, 2}
    };

    std::vector<TestReport> reports;
    int passCount = 0;
    int failCount = 0;

    for (const auto& tc : cases) {
        std::cout << "\n[RUNNING] " << tc.name << "..." << std::flush;
        std::string detail;
        TestOutcome outcome = run_in_subprocess(tc.fn, tc.timeout_sec, detail);
        reports.push_back({tc.name, outcome, detail});

        if (outcome == TestOutcome::PASSED) {
            std::cout << " -> PASS" << std::endl;
            passCount++;
        } else if (outcome == TestOutcome::TIMED_OUT) {
            std::cout << " -> FAIL (INFINITE LOOP / HANG)" << std::endl;
            failCount++;
        } else if (outcome == TestOutcome::CRASHED) {
            std::cout << " -> FAIL (CRASH / SIGSEGV / EXCEPTION)" << std::endl;
            failCount++;
        } else {
            std::cout << " -> FAIL (" << detail << ")" << std::endl;
            failCount++;
        }
    }

    std::cout << "\n==================================================================" << std::endl;
    std::cout << "                      DETAILED TEST REPORT                        " << std::endl;
    std::cout << "==================================================================" << std::endl;
    for (const auto& r : reports) {
        const char* st = (r.outcome == TestOutcome::PASSED) ? "[PASS]" :
                         (r.outcome == TestOutcome::TIMED_OUT) ? "[HANG]" :
                         (r.outcome == TestOutcome::CRASHED) ? "[CRASH]" : "[FAIL]";
        std::cout << st << " " << r.name << "\n       Detail: " << r.detail << std::endl;
    }
    std::cout << "==================================================================" << std::endl;
    std::cout << "Total: " << cases.size() << " | Passed: " << passCount << " | Failed: " << failCount << std::endl;
    std::cout << "==================================================================" << std::endl;

    return (failCount == 0) ? 0 : 1;
}
