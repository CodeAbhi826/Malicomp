#include "vulkan_layer_exports.h"
#include "descriptor_shim.h"
#include <iostream>
#include <vector>
#include <thread>
#include <chrono>
#include <cstring>
#include <cassert>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <signal.h>

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

// 1. Circular pNext in Features2
void test_circular_pnext_features2() {
    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceDescriptorIndexingFeatures indexingFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceVulkan12Features v12Feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};

    feat2.pNext = &indexingFeat;
    indexingFeat.pNext = &v12Feat;
    v12Feat.pNext = &indexingFeat; // CIRCULAR!

    vkGetPhysicalDeviceFeatures2(VK_NULL_HANDLE, &feat2);
}

// 2. Circular pNext in Properties2
void test_circular_pnext_properties2() {
    VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    VkPhysicalDeviceDescriptorIndexingProperties indexingProps{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_PROPERTIES};
    VkPhysicalDeviceVulkan12Properties v12Props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};

    props2.pNext = &indexingProps;
    indexingProps.pNext = &v12Props;
    v12Props.pNext = &indexingProps; // CIRCULAR!

    vkGetPhysicalDeviceProperties2(VK_NULL_HANDLE, &props2);
}

// 3. Circular pNext in CreateDevice
void test_circular_pnext_create_device() {
    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceDescriptorIndexingFeatures indexingFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceVulkan12Features v12Feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};

    createInfo.pNext = &indexingFeat;
    indexingFeat.pNext = &v12Feat;
    v12Feat.pNext = &indexingFeat; // CIRCULAR!

    VkDevice dev{VK_NULL_HANDLE};
    vkCreateDevice(VK_NULL_HANDLE, &createInfo, nullptr, &dev);
}

// 4. Nested circular pNext in Features2 within CreateDevice
void test_nested_circular_pnext_create_device() {
    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    VkPhysicalDeviceDescriptorIndexingFeatures indexingFeat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
    VkPhysicalDeviceVulkan12Features v12Feat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};

    createInfo.pNext = &f2;
    f2.pNext = &indexingFeat;
    indexingFeat.pNext = &v12Feat;
    v12Feat.pNext = &indexingFeat; // CIRCULAR in f2->pNext!

    VkDevice dev{VK_NULL_HANDLE};
    vkCreateDevice(VK_NULL_HANDLE, &createInfo, nullptr, &dev);
}

// 5. Deep pNext Chain (1,000 full-sized nodes)
void test_deep_pnext_chain() {
    constexpr int kChainLength = 1000;
    std::vector<VkPhysicalDeviceDescriptorIndexingFeatures> nodes(kChainLength);
    for (int i = 0; i < kChainLength; ++i) {
        nodes[i].sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
        nodes[i].pNext = (i + 1 < kChainLength) ? &nodes[i + 1] : nullptr;
    }

    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feat2.pNext = &nodes[0];
    vkGetPhysicalDeviceFeatures2(VK_NULL_HANDLE, &feat2);

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &nodes[0];
    VkDevice dev{VK_NULL_HANDLE};
    vkCreateDevice(VK_NULL_HANDLE, &createInfo, nullptr, &dev);
}

// 6. Unknown Structure Types in pNext
void test_unknown_stype_pnext() {
    struct UnknownStruct {
        VkStructureType sType;
        void* pNext;
        uint32_t customData[16];
    };

    UnknownStruct u1{static_cast<VkStructureType>(0x7FFFFFFF), nullptr, {0}};
    UnknownStruct u2{static_cast<VkStructureType>(9999999), nullptr, {0}};
    UnknownStruct u3{static_cast<VkStructureType>(0), nullptr, {0}};

    u1.pNext = &u2;
    u2.pNext = &u3;

    VkPhysicalDeviceFeatures2 feat2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    feat2.pNext = &u1;
    vkGetPhysicalDeviceFeatures2(VK_NULL_HANDLE, &feat2);

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = &u1;
    VkDevice dev{VK_NULL_HANDLE};
    vkCreateDevice(VK_NULL_HANDLE, &createInfo, nullptr, &dev);
}

// 7. Read-Only Memory pNext in vkCreateDevice (in-place const_cast mutation)
void test_readonly_pnext_mutation() {
    size_t pageSize = sysconf(_SC_PAGESIZE);
    void* mem = mmap(nullptr, pageSize, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    assert(mem != MAP_FAILED);

    auto* f2 = reinterpret_cast<VkPhysicalDeviceFeatures2*>(mem);
    f2->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2->pNext = nullptr;

    int mres = mprotect(mem, pageSize, PROT_READ);
    assert(mres == 0);

    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.pNext = f2;

    VkDevice dev{VK_NULL_HANDLE};
    vkCreateDevice(VK_NULL_HANDLE, &createInfo, nullptr, &dev);
    munmap(mem, pageSize);
}

// 8. Null Pointers in ProcAddr
void test_null_proc_addr() {
    PFN_vkVoidFunction fn1 = vkGetInstanceProcAddr(VK_NULL_HANDLE, nullptr);
    assert(fn1 == nullptr);

    PFN_vkVoidFunction fn2 = vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice");
    assert(fn2 != nullptr);

    PFN_vkVoidFunction fn3 = vkGetInstanceProcAddr(VK_NULL_HANDLE, "nonExistentSymbol_XYZ");
    assert(fn3 == nullptr);

    PFN_vkVoidFunction fn4 = vkGetDeviceProcAddr(VK_NULL_HANDLE, nullptr);
    assert(fn4 == nullptr);

    PFN_vkVoidFunction fn5 = vkGetDeviceProcAddr(VK_NULL_HANDLE, "vkCreateDescriptorPool");
    assert(fn5 != nullptr);
}

// 9. Null Pointers in vkCreateDescriptorPool
void test_null_create_descriptor_pool() {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCreateDescriptorPool(VK_NULL_HANDLE, nullptr, nullptr, &pool);

    auto mockCreate = [](VkDevice, const VkDescriptorPoolCreateInfo*, const VkAllocationCallbacks*, VkDescriptorPool* pPool) -> VkResult {
        if (pPool) *pPool = reinterpret_cast<VkDescriptorPool>(0x1234);
        return VK_SUCCESS;
    };

    VkResult res2 = malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorPool(
        VK_NULL_HANDLE, nullptr, nullptr, &pool, mockCreate);
    assert(res2 == VK_ERROR_INITIALIZATION_FAILED);

    VkDescriptorPoolCreateInfo validInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    validInfo.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    validInfo.maxSets = 10;

    malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorPool(
        VK_NULL_HANDLE, &validInfo, nullptr, nullptr, mockCreate);
}

// 10. Null ppEnabledExtensionNames with enabledExtensionCount > 0 in vkCreateDevice
void test_null_extension_names_array() {
    VkDeviceCreateInfo createInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    createInfo.enabledExtensionCount = 5;
    createInfo.ppEnabledExtensionNames = nullptr; // NULL pointer dereference trigger

    VkDevice dev{VK_NULL_HANDLE};
    vkCreateDevice(VK_NULL_HANDLE, &createInfo, nullptr, &dev);
}

// 11. Zero Descriptor Count in Layout Creation and Allocation
void test_zero_descriptor_count() {
    auto mockCreateLayout = [](VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* pLayout) -> VkResult {
        static uint64_t handle = 0x2000;
        if (pLayout) *pLayout = reinterpret_cast<VkDescriptorSetLayout>(++handle);
        return VK_SUCCESS;
    };
    auto mockAllocSets = [](VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet* pSets) -> VkResult {
        if (pSets) pSets[0] = reinterpret_cast<VkDescriptorSet>(0x6000);
        return VK_SUCCESS;
    };

    VkDescriptorSetLayoutBinding zeroBinding{};
    zeroBinding.binding = 0;
    zeroBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    zeroBinding.descriptorCount = 0; // ZERO
    zeroBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo zeroLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    zeroLayoutInfo.bindingCount = 1;
    zeroLayoutInfo.pBindings = &zeroBinding;

    VkDescriptorSetLayout zeroLayout = VK_NULL_HANDLE;
    VkResult r1 = malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorSetLayout(
        reinterpret_cast<VkDevice>(0x1), &zeroLayoutInfo, nullptr, &zeroLayout, mockCreateLayout);
    assert(r1 == VK_SUCCESS);

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &zeroLayout;
    VkDescriptorSet setHandle = VK_NULL_HANDLE;

    VkResult r2 = malicompat::VulkanDescriptorShim::Instance().InterceptAllocateDescriptorSets(
        reinterpret_cast<VkDevice>(0x1), &allocInfo, &setHandle, mockAllocSets);
    assert(r2 == VK_SUCCESS);
}

// 12. Null pDescriptorWrites with descriptorWriteCount > 0 in InterceptUpdateDescriptorSets
void test_null_descriptor_writes_array() {
    auto mockUpdateSets = [](VkDevice, uint32_t, const VkWriteDescriptorSet*, uint32_t, const VkCopyDescriptorSet*) {};
    malicompat::VulkanDescriptorShim::Instance().InterceptUpdateDescriptorSets(
        reinterpret_cast<VkDevice>(0x1), 3, nullptr, 0, nullptr, mockUpdateSets);
}

// 13. Massive Descriptor Count (0xFFFFFFFF) in AllocateDescriptorSets
void test_massive_descriptor_count_alloc() {
    auto mockCreateLayout = [](VkDevice, const VkDescriptorSetLayoutCreateInfo*, const VkAllocationCallbacks*, VkDescriptorSetLayout* pLayout) -> VkResult {
        static uint64_t handle = 0x3000;
        if (pLayout) *pLayout = reinterpret_cast<VkDescriptorSetLayout>(++handle);
        return VK_SUCCESS;
    };
    auto mockAllocSets = [](VkDevice, const VkDescriptorSetAllocateInfo*, VkDescriptorSet* pSets) -> VkResult {
        if (pSets) pSets[0] = reinterpret_cast<VkDescriptorSet>(0x7000);
        return VK_SUCCESS;
    };

    VkDescriptorSetLayoutBinding massiveBinding{};
    massiveBinding.binding = 1;
    massiveBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    massiveBinding.descriptorCount = 0xFFFFFFFF; // 4 Billion descriptors!
    massiveBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo massiveLayoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    massiveLayoutInfo.bindingCount = 1;
    massiveLayoutInfo.pBindings = &massiveBinding;

    VkDescriptorSetLayout massiveLayout = VK_NULL_HANDLE;
    VkResult r1 = malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorSetLayout(
        reinterpret_cast<VkDevice>(0x1), &massiveLayoutInfo, nullptr, &massiveLayout, mockCreateLayout);
    assert(r1 == VK_SUCCESS);

    VkDescriptorSetAllocateInfo allocInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &massiveLayout;
    VkDescriptorSet setHandle = VK_NULL_HANDLE;

    // This will attempt: std::vector<VkDescriptorImageInfo> imgVec(0xFFFFFFFF)
    malicompat::VulkanDescriptorShim::Instance().InterceptAllocateDescriptorSets(
        reinterpret_cast<VkDevice>(0x1), &allocInfo, &setHandle, mockAllocSets);
}

// 14. Multithreaded Virtual Query Pool Stress (0xDEADBEEFULL)
void test_multithreaded_virtual_query_pool() {
    constexpr int kNumThreads = 16;
    constexpr int kIterations = 2000;
    std::vector<std::thread> threads;
    VkQueryPool qpool = VIRTUAL_QUERY_POOL_HANDLE;

    for (int t = 0; t < kNumThreads; ++t) {
        threads.emplace_back([qpool, t]() {
            std::vector<uint64_t> results(16, 0);
            for (int i = 0; i < kIterations; ++i) {
                vkCmdResetQueryPool(VK_NULL_HANDLE, qpool, 0, 16);
                vkCmdBeginQuery(VK_NULL_HANDLE, qpool, i % 16, 0);
                vkCmdEndQuery(VK_NULL_HANDLE, qpool, i % 16);
                vkCmdCopyQueryPoolResults(VK_NULL_HANDLE, qpool, 0, 16, VK_NULL_HANDLE, 0, 8, 0);
                vkCmdWriteTimestamp(VK_NULL_HANDLE, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, qpool, i % 16);

                VkResult res = vkGetQueryPoolResults(
                    VK_NULL_HANDLE,
                    qpool,
                    0,
                    16,
                    results.size() * sizeof(uint64_t),
                    results.data(),
                    sizeof(uint64_t),
                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT
                );
                assert(res == VK_SUCCESS);
                assert(results[1] == 1ULL);
            }
        });
    }

    for (auto& th : threads) {
        th.join();
    }
    vkDestroyQueryPool(VK_NULL_HANDLE, qpool, nullptr);
}

int main() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "   MALICOMPAT ADVERSARIAL VULKAN CONTRACT & LAYOUT FUZZING SUITE   " << std::endl;
    std::cout << "==================================================================" << std::endl;

    struct TestCase {
        std::string name;
        void (*fn)();
        int timeout_sec;
    };

    std::vector<TestCase> cases = {
        {"1. Circular pNext in vkGetPhysicalDeviceFeatures2", test_circular_pnext_features2, 2},
        {"2. Circular pNext in vkGetPhysicalDeviceProperties2", test_circular_pnext_properties2, 2},
        {"3. Circular pNext in vkCreateDevice", test_circular_pnext_create_device, 2},
        {"4. Nested circular pNext in Features2 in vkCreateDevice", test_nested_circular_pnext_create_device, 2},
        {"5. Deep pNext chain (1000 nodes)", test_deep_pnext_chain, 3},
        {"6. Unknown structure types in pNext", test_unknown_stype_pnext, 2},
        {"7. Read-only memory pNext in vkCreateDevice", test_readonly_pnext_mutation, 2},
        {"8. Null pointers in vkGetInstanceProcAddr / vkGetDeviceProcAddr", test_null_proc_addr, 2},
        {"9. Null pointers in vkCreateDescriptorPool", test_null_create_descriptor_pool, 2},
        {"10. Null ppEnabledExtensionNames with count > 0 in vkCreateDevice", test_null_extension_names_array, 2},
        {"11. Zero descriptor count in layout & allocation", test_zero_descriptor_count, 2},
        {"12. Null pDescriptorWrites with writeCount > 0 in UpdateDescriptorSets", test_null_descriptor_writes_array, 2},
        {"13. Massive descriptor count (0xFFFFFFFF) in AllocateDescriptorSets", test_massive_descriptor_count_alloc, 2},
        {"14. Multithreaded virtual query pool (0xDEADBEEFULL) stress", test_multithreaded_virtual_query_pool, 5}
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
