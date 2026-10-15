#include "vulkan_layer_exports.h"
#include "descriptor_shim.h"
#include <iostream>
#include <vector>
#include <string>
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
            out_detail = "Execution timed out after " + std::to_string(timeout_sec) + "s (HANG)";
            return TestOutcome::TIMED_OUT;
        }
        usleep(5000); // 5ms
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

// -----------------------------------------------------------------------------
// Test Group 1: malicompat_vkQueueSubmit
// -----------------------------------------------------------------------------

// 1.1: submitCount 0, null fence
void test_queue_submit_zero_null_fence() {
    VkResult res = malicompat_vkQueueSubmit(VK_NULL_HANDLE, 0, nullptr, VK_NULL_HANDLE);
    assert(res == VK_SUCCESS);
}

// 1.2: submitCount 0, non-null fence
void test_queue_submit_zero_with_fence() {
    VkFence fence = reinterpret_cast<VkFence>(0x1234);
    VkResult res = malicompat_vkQueueSubmit(VK_NULL_HANDLE, 0, nullptr, fence);
    assert(res == VK_SUCCESS);
}

// 1.3: submitCount extreme (0xFFFFFFFF), null pSubmits
void test_queue_submit_extreme_count_null_submits() {
    // When vendor driver is unresolvable, it should not crash dereferencing pSubmits
    VkResult res = malicompat_vkQueueSubmit(VK_NULL_HANDLE, 0xFFFFFFFF, nullptr, VK_NULL_HANDLE);
    assert(res == VK_SUCCESS);
}

// 1.4: submitCount 1, null pSubmits
void test_queue_submit_one_null_submits() {
    VkResult res = malicompat_vkQueueSubmit(VK_NULL_HANDLE, 1, nullptr, VK_NULL_HANDLE);
    assert(res == VK_SUCCESS);
}

// 1.5: Valid submit info with mock queue
void test_queue_submit_valid_info() {
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.waitSemaphoreCount = 0;
    submitInfo.pWaitSemaphores = nullptr;
    submitInfo.commandBufferCount = 0;
    submitInfo.pCommandBuffers = nullptr;
    submitInfo.signalSemaphoreCount = 0;
    submitInfo.pSignalSemaphores = nullptr;

    VkResult res = malicompat_vkQueueSubmit(reinterpret_cast<VkQueue>(0x10), 1, &submitInfo, VK_NULL_HANDLE);
    assert(res == VK_SUCCESS);
}

// -----------------------------------------------------------------------------
// Test Group 2: malicompat_vkCreatePipelineLayout
// -----------------------------------------------------------------------------

// 2.1: null pCreateInfo
void test_pipeline_layout_null_create_info() {
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreatePipelineLayout(VK_NULL_HANDLE, nullptr, nullptr, &layout);
    assert(res == VK_ERROR_INITIALIZATION_FAILED);
    assert(layout == VK_NULL_HANDLE);
}

// 2.2: setLayoutCount 0
void test_pipeline_layout_set_count_0() {
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 0;
    info.pSetLayouts = nullptr;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreatePipelineLayout(VK_NULL_HANDLE, &info, nullptr, &layout);
    assert(res == VK_SUCCESS);
    assert(layout != VK_NULL_HANDLE);
}

// 2.3: setLayoutCount 4
void test_pipeline_layout_set_count_4() {
    VkDescriptorSetLayout dummyLayouts[4] = {
        reinterpret_cast<VkDescriptorSetLayout>(0x1),
        reinterpret_cast<VkDescriptorSetLayout>(0x2),
        reinterpret_cast<VkDescriptorSetLayout>(0x3),
        reinterpret_cast<VkDescriptorSetLayout>(0x4)
    };
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 4;
    info.pSetLayouts = dummyLayouts;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreatePipelineLayout(VK_NULL_HANDLE, &info, nullptr, &layout);
    assert(res == VK_SUCCESS);
    assert(layout != VK_NULL_HANDLE);
}

// 2.4: setLayoutCount 5 (must be clamped to 4)
void test_pipeline_layout_set_count_5() {
    VkDescriptorSetLayout dummyLayouts[5] = {
        reinterpret_cast<VkDescriptorSetLayout>(0x1),
        reinterpret_cast<VkDescriptorSetLayout>(0x2),
        reinterpret_cast<VkDescriptorSetLayout>(0x3),
        reinterpret_cast<VkDescriptorSetLayout>(0x4),
        reinterpret_cast<VkDescriptorSetLayout>(0x5)
    };
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 5;
    info.pSetLayouts = dummyLayouts;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreatePipelineLayout(VK_NULL_HANDLE, &info, nullptr, &layout);
    assert(res == VK_SUCCESS);
    assert(layout != VK_NULL_HANDLE);
}

// 2.5: setLayoutCount 100
void test_pipeline_layout_set_count_100() {
    std::vector<VkDescriptorSetLayout> dummyLayouts(100, reinterpret_cast<VkDescriptorSetLayout>(0x99));
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 100;
    info.pSetLayouts = dummyLayouts.data();

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreatePipelineLayout(VK_NULL_HANDLE, &info, nullptr, &layout);
    assert(res == VK_SUCCESS);
    assert(layout != VK_NULL_HANDLE);
}

// 2.6: setLayoutCount extreme (0xFFFFFFFF)
void test_pipeline_layout_set_count_extreme() {
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 0xFFFFFFFF;
    info.pSetLayouts = nullptr;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkResult res = malicompat_vkCreatePipelineLayout(VK_NULL_HANDLE, &info, nullptr, &layout);
    assert(res == VK_SUCCESS);
    assert(layout != VK_NULL_HANDLE);
}

// 2.7: null pPipelineLayout
void test_pipeline_layout_null_out_handle() {
    VkPipelineLayoutCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    info.setLayoutCount = 2;
    info.pSetLayouts = nullptr;

    // Should not crash when pPipelineLayout is null
    VkResult res = malicompat_vkCreatePipelineLayout(VK_NULL_HANDLE, &info, nullptr, nullptr);
    assert(res == VK_SUCCESS);
}

// -----------------------------------------------------------------------------
// Test Group 3: InterceptCmdBindDescriptorSets
// -----------------------------------------------------------------------------

static uint32_t g_last_first_set = 0;
static uint32_t g_last_set_count = 0;
static bool g_mock_bind_called = false;

static void VKAPI_CALL mock_real_vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t* pDynamicOffsets) {
    g_mock_bind_called = true;
    g_last_first_set = firstSet;
    g_last_set_count = descriptorSetCount;
}

// 3.1: Null realFunc
void test_bind_sets_null_real_func() {
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        reinterpret_cast<VkCommandBuffer>(0x1),
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        reinterpret_cast<VkPipelineLayout>(0x2),
        0, 1, nullptr, 0, nullptr,
        nullptr
    );
}

// 3.2: firstSet 0, count 4 (within budget)
void test_bind_sets_first0_count4() {
    g_mock_bind_called = false;
    VkDescriptorSet dummySets[4] = {
        reinterpret_cast<VkDescriptorSet>(0x1),
        reinterpret_cast<VkDescriptorSet>(0x2),
        reinterpret_cast<VkDescriptorSet>(0x3),
        reinterpret_cast<VkDescriptorSet>(0x4)
    };
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        reinterpret_cast<VkCommandBuffer>(0x1),
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        reinterpret_cast<VkPipelineLayout>(0x2),
        0, 4, dummySets, 0, nullptr,
        mock_real_vkCmdBindDescriptorSets
    );
    assert(g_mock_bind_called == true);
    assert(g_last_first_set == 0);
    assert(g_last_set_count == 4);
}

// 3.3: firstSet 0, count 5 (exceeds budget, clamped to 4)
void test_bind_sets_first0_count5_clamped() {
    g_mock_bind_called = false;
    VkDescriptorSet dummySets[5] = {nullptr};
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        reinterpret_cast<VkCommandBuffer>(0x1),
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        reinterpret_cast<VkPipelineLayout>(0x2),
        0, 5, dummySets, 0, nullptr,
        mock_real_vkCmdBindDescriptorSets
    );
    assert(g_mock_bind_called == true);
    assert(g_last_first_set == 0);
    assert(g_last_set_count == 4);
}

// 3.4: firstSet 3, count 2 (firstSet+count=5 > 4, clamped to 4-3=1)
void test_bind_sets_first3_count2_clamped() {
    g_mock_bind_called = false;
    VkDescriptorSet dummySets[2] = {nullptr};
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        reinterpret_cast<VkCommandBuffer>(0x1),
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        reinterpret_cast<VkPipelineLayout>(0x2),
        3, 2, dummySets, 0, nullptr,
        mock_real_vkCmdBindDescriptorSets
    );
    assert(g_mock_bind_called == true);
    assert(g_last_first_set == 3);
    assert(g_last_set_count == 1);
}

// 3.5: firstSet 4 (at limit, dropped)
void test_bind_sets_first4_dropped() {
    g_mock_bind_called = false;
    VkDescriptorSet dummySet = reinterpret_cast<VkDescriptorSet>(0x1);
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        reinterpret_cast<VkCommandBuffer>(0x1),
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        reinterpret_cast<VkPipelineLayout>(0x2),
        4, 1, &dummySet, 0, nullptr,
        mock_real_vkCmdBindDescriptorSets
    );
    assert(g_mock_bind_called == false);
}

// 3.6: firstSet 10 (exceeds limit, dropped)
void test_bind_sets_first10_dropped() {
    g_mock_bind_called = false;
    VkDescriptorSet dummySet = reinterpret_cast<VkDescriptorSet>(0x1);
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        reinterpret_cast<VkCommandBuffer>(0x1),
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        reinterpret_cast<VkPipelineLayout>(0x2),
        10, 1, &dummySet, 0, nullptr,
        mock_real_vkCmdBindDescriptorSets
    );
    assert(g_mock_bind_called == false);
}

// 3.7: firstSet 0xFFFFFFFF (dropped)
void test_bind_sets_first_extreme_dropped() {
    g_mock_bind_called = false;
    VkDescriptorSet dummySet = reinterpret_cast<VkDescriptorSet>(0x1);
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        reinterpret_cast<VkCommandBuffer>(0x1),
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        reinterpret_cast<VkPipelineLayout>(0x2),
        0xFFFFFFFF, 1, &dummySet, 0, nullptr,
        mock_real_vkCmdBindDescriptorSets
    );
    assert(g_mock_bind_called == false);
}

// 3.8: ADVERSARIAL TEST: firstSet 3, count 0xFFFFFFFF (arithmetic overflow check)
// In Mali-G71 hardware, bound sets MUST NOT exceed 4 (maxBoundDescriptorSets == 4).
// For firstSet = 3, clampedCount MUST be <= 1.
void test_bind_sets_overflow_first3_count_max() {
    g_mock_bind_called = false;
    g_last_set_count = 0;
    VkDescriptorSet dummySet = reinterpret_cast<VkDescriptorSet>(0x1);
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        reinterpret_cast<VkCommandBuffer>(0x1),
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        reinterpret_cast<VkPipelineLayout>(0x2),
        3, 0xFFFFFFFF, &dummySet, 0, nullptr,
        mock_real_vkCmdBindDescriptorSets
    );
    assert(g_mock_bind_called == true);
    // If integer overflow occurs (3 + 0xFFFFFFFF = 2 <= 4), g_last_set_count remains 0xFFFFFFFF!
    // The correct clamped count must be 1 (since 4 - 3 = 1).
    std::cerr << "[TEST-DIAG] firstSet=3, inputCount=0xFFFFFFFF -> received clampedCount=" 
              << g_last_set_count << " (expected 1)\n";
    assert(g_last_set_count == 1);
}

// 3.9: ADVERSARIAL TEST: firstSet 2, count 0xFFFFFFFF (arithmetic overflow check)
void test_bind_sets_overflow_first2_count_max() {
    g_mock_bind_called = false;
    g_last_set_count = 0;
    VkDescriptorSet dummySet = reinterpret_cast<VkDescriptorSet>(0x1);
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        reinterpret_cast<VkCommandBuffer>(0x1),
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        reinterpret_cast<VkPipelineLayout>(0x2),
        2, 0xFFFFFFFF, &dummySet, 0, nullptr,
        mock_real_vkCmdBindDescriptorSets
    );
    assert(g_mock_bind_called == true);
    std::cerr << "[TEST-DIAG] firstSet=2, inputCount=0xFFFFFFFF -> received clampedCount=" 
              << g_last_set_count << " (expected 2)\n";
    assert(g_last_set_count == 2);
}

// 3.10: ADVERSARIAL TEST: firstSet 1, count 0xFFFFFFFF (arithmetic overflow check)
void test_bind_sets_overflow_first1_count_max() {
    g_mock_bind_called = false;
    g_last_set_count = 0;
    VkDescriptorSet dummySet = reinterpret_cast<VkDescriptorSet>(0x1);
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        reinterpret_cast<VkCommandBuffer>(0x1),
        VK_PIPELINE_BIND_POINT_GRAPHICS,
        reinterpret_cast<VkPipelineLayout>(0x2),
        1, 0xFFFFFFFF, &dummySet, 0, nullptr,
        mock_real_vkCmdBindDescriptorSets
    );
    assert(g_mock_bind_called == true);
    std::cerr << "[TEST-DIAG] firstSet=1, inputCount=0xFFFFFFFF -> received clampedCount=" 
              << g_last_set_count << " (expected 3)\n";
    assert(g_last_set_count == 3);
}

// -----------------------------------------------------------------------------
// Test Group 4: malicompat_vkQueuePresentKHR
// -----------------------------------------------------------------------------

// 4.1: null pPresentInfo
void test_queue_present_null_info() {
    VkResult res = malicompat_vkQueuePresentKHR(reinterpret_cast<VkQueue>(0x1), nullptr);
    // On host or unresolvable vendor/system HAL, should return error without crashing
    assert(res == VK_ERROR_INITIALIZATION_FAILED);
}

// 4.2: empty swapchains in pPresentInfo
void test_queue_present_empty_swapchains() {
    VkPresentInfoKHR presentInfo{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    presentInfo.swapchainCount = 0;
    presentInfo.pSwapchains = nullptr;
    presentInfo.pImageIndices = nullptr;

    VkResult res = malicompat_vkQueuePresentKHR(reinterpret_cast<VkQueue>(0x1), &presentInfo);
    assert(res == VK_ERROR_INITIALIZATION_FAILED);
}

// -----------------------------------------------------------------------------
// Test Suite Runner
// -----------------------------------------------------------------------------

int main() {
    std::cout << "==================================================================" << std::endl;
    std::cout << "  MALICOMPAT ADVERSARIAL VULKAN SUBMISSION & CLAMPING STRESS SUITE " << std::endl;
    std::cout << "==================================================================" << std::endl;

    struct TestCase {
        std::string name;
        void (*fn)();
        int timeout_sec;
    };

    std::vector<TestCase> cases = {
        // Group 1: vkQueueSubmit
        {"1.1 vkQueueSubmit: submitCount 0, null fence", test_queue_submit_zero_null_fence, 2},
        {"1.2 vkQueueSubmit: submitCount 0, non-null fence", test_queue_submit_zero_with_fence, 2},
        {"1.3 vkQueueSubmit: extreme count (0xFFFFFFFF), null pSubmits", test_queue_submit_extreme_count_null_submits, 2},
        {"1.4 vkQueueSubmit: submitCount 1, null pSubmits", test_queue_submit_one_null_submits, 2},
        {"1.5 vkQueueSubmit: valid submit info with mock queue", test_queue_submit_valid_info, 2},

        // Group 2: vkCreatePipelineLayout
        {"2.1 vkCreatePipelineLayout: null pCreateInfo", test_pipeline_layout_null_create_info, 2},
        {"2.2 vkCreatePipelineLayout: setLayoutCount 0", test_pipeline_layout_set_count_0, 2},
        {"2.3 vkCreatePipelineLayout: setLayoutCount 4 (exact limit)", test_pipeline_layout_set_count_4, 2},
        {"2.4 vkCreatePipelineLayout: setLayoutCount 5 (clamped to 4)", test_pipeline_layout_set_count_5, 2},
        {"2.5 vkCreatePipelineLayout: setLayoutCount 100 (clamped to 4)", test_pipeline_layout_set_count_100, 2},
        {"2.6 vkCreatePipelineLayout: setLayoutCount 0xFFFFFFFF (extreme)", test_pipeline_layout_set_count_extreme, 2},
        {"2.7 vkCreatePipelineLayout: null pPipelineLayout output pointer", test_pipeline_layout_null_out_handle, 2},

        // Group 3: InterceptCmdBindDescriptorSets
        {"3.1 InterceptCmdBindDescriptorSets: null realFunc", test_bind_sets_null_real_func, 2},
        {"3.2 InterceptCmdBindDescriptorSets: firstSet 0, count 4 (within limit)", test_bind_sets_first0_count4, 2},
        {"3.3 InterceptCmdBindDescriptorSets: firstSet 0, count 5 (clamped to 4)", test_bind_sets_first0_count5_clamped, 2},
        {"3.4 InterceptCmdBindDescriptorSets: firstSet 3, count 2 (clamped to 1)", test_bind_sets_first3_count2_clamped, 2},
        {"3.5 InterceptCmdBindDescriptorSets: firstSet 4 (boundary, dropped)", test_bind_sets_first4_dropped, 2},
        {"3.6 InterceptCmdBindDescriptorSets: firstSet 10 (exceeded, dropped)", test_bind_sets_first10_dropped, 2},
        {"3.7 InterceptCmdBindDescriptorSets: firstSet 0xFFFFFFFF (extreme, dropped)", test_bind_sets_first_extreme_dropped, 2},
        {"3.8 InterceptCmdBindDescriptorSets: firstSet 3, count 0xFFFFFFFF (arithmetic overflow check)", test_bind_sets_overflow_first3_count_max, 2},
        {"3.9 InterceptCmdBindDescriptorSets: firstSet 2, count 0xFFFFFFFF (arithmetic overflow check)", test_bind_sets_overflow_first2_count_max, 2},
        {"3.10 InterceptCmdBindDescriptorSets: firstSet 1, count 0xFFFFFFFF (arithmetic overflow check)", test_bind_sets_overflow_first1_count_max, 2},

        // Group 4: malicompat_vkQueuePresentKHR
        {"4.1 malicompat_vkQueuePresentKHR: null pPresentInfo", test_queue_present_null_info, 2},
        {"4.2 malicompat_vkQueuePresentKHR: empty swapchains in pPresentInfo", test_queue_present_empty_swapchains, 2}
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
            std::cout << " -> FAIL (HANG)" << std::endl;
            failCount++;
        } else if (outcome == TestOutcome::CRASHED) {
            std::cout << " -> FAIL (CRASH / SIGSEGV)" << std::endl;
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
