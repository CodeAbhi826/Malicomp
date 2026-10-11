#include "vulkan_layer_exports.h"
#include "descriptor_shim.h"
#include <cstring>
#include <dlfcn.h>
#include <unordered_map>
#include <mutex>

namespace {

void* g_real_vulkan = nullptr;
PFN_vkGetInstanceProcAddr g_real_vkGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr g_real_vkGetDeviceProcAddr = nullptr;

PFN_vkCreateDescriptorSetLayout g_real_vkCreateDescriptorSetLayout = nullptr;
PFN_vkAllocateDescriptorSets g_real_vkAllocateDescriptorSets = nullptr;
PFN_vkUpdateDescriptorSets g_real_vkUpdateDescriptorSets = nullptr;
PFN_vkCmdBindDescriptorSets g_real_vkCmdBindDescriptorSets = nullptr;
PFN_vkCreateQueryPool g_real_vkCreateQueryPool = nullptr;

std::once_flag g_init_flag;

void InitRealVulkan() {
    std::call_once(g_init_flag, []() {
        g_real_vulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (g_real_vulkan) {
            g_real_vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(g_real_vulkan, "vkGetInstanceProcAddr"));
            g_real_vkGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(dlsym(g_real_vulkan, "vkGetDeviceProcAddr"));
            g_real_vkCreateDescriptorSetLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(dlsym(g_real_vulkan, "vkCreateDescriptorSetLayout"));
            g_real_vkAllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(dlsym(g_real_vulkan, "vkAllocateDescriptorSets"));
            g_real_vkUpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(dlsym(g_real_vulkan, "vkUpdateDescriptorSets"));
            g_real_vkCmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(dlsym(g_real_vulkan, "vkCmdBindDescriptorSets"));
            g_real_vkCreateQueryPool = reinterpret_cast<PFN_vkCreateQueryPool>(dlsym(g_real_vulkan, "vkCreateQueryPool"));
        }
    });
}

} // namespace

extern "C" {

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_vkGetInstanceProcAddr(VkInstance instance, const char* pName) {
    InitRealVulkan();
    if (!pName) return nullptr;

    if (std::strcmp(pName, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorSetLayout);
    if (std::strcmp(pName, "vkAllocateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkAllocateDescriptorSets);
    if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkUpdateDescriptorSets);
    if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdBindDescriptorSets);
    if (std::strcmp(pName, "vkCreateQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateQueryPool);

    if (g_real_vkGetInstanceProcAddr) {
        return g_real_vkGetInstanceProcAddr(instance, pName);
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL malicompat_vkGetDeviceProcAddr(VkDevice device, const char* pName) {
    InitRealVulkan();
    if (!pName) return nullptr;

    if (std::strcmp(pName, "vkCreateDescriptorSetLayout") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateDescriptorSetLayout);
    if (std::strcmp(pName, "vkAllocateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkAllocateDescriptorSets);
    if (std::strcmp(pName, "vkUpdateDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkUpdateDescriptorSets);
    if (std::strcmp(pName, "vkCmdBindDescriptorSets") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCmdBindDescriptorSets);
    if (std::strcmp(pName, "vkCreateQueryPool") == 0) return reinterpret_cast<PFN_vkVoidFunction>(malicompat_vkCreateQueryPool);

    if (g_real_vkGetDeviceProcAddr) {
        return g_real_vkGetDeviceProcAddr(device, pName);
    }
    return nullptr;
}

VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateDescriptorSetLayout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout) {
    InitRealVulkan();
    return malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorSetLayout(
        device, pCreateInfo, pAllocator, pSetLayout, g_real_vkCreateDescriptorSetLayout);
}

VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets) {
    InitRealVulkan();
    return malicompat::VulkanDescriptorShim::Instance().InterceptAllocateDescriptorSets(
        device, pAllocateInfo, pDescriptorSets, g_real_vkAllocateDescriptorSets);
}

VKAPI_ATTR void VKAPI_CALL malicompat_vkUpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies) {
    InitRealVulkan();
    malicompat::VulkanDescriptorShim::Instance().InterceptUpdateDescriptorSets(
        device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies, g_real_vkUpdateDescriptorSets);
}

VKAPI_ATTR void VKAPI_CALL malicompat_vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t* pDynamicOffsets) {
    InitRealVulkan();
    malicompat::VulkanDescriptorShim::Instance().InterceptCmdBindDescriptorSets(
        commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets, g_real_vkCmdBindDescriptorSets);
}

VKAPI_ATTR VkResult VKAPI_CALL malicompat_vkCreateQueryPool(
    VkDevice device,
    const VkQueryPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkQueryPool* pQueryPool) {
    InitRealVulkan();
    if (!pCreateInfo || !pQueryPool) return VK_ERROR_INITIALIZATION_FAILED;

    // Quirk: Neuter occlusion queries on ARM Mali to prevent driver resets
    if (pCreateInfo->queryType == VK_QUERY_TYPE_OCCLUSION) {
        *pQueryPool = reinterpret_cast<VkQueryPool>(0xDEADBEEF);
        return VK_SUCCESS;
    }

    if (g_real_vkCreateQueryPool) {
        return g_real_vkCreateQueryPool(device, pCreateInfo, pAllocator, pQueryPool);
    }
    return VK_SUCCESS;
}

// Standard LD_PRELOAD aliases for direct transparent interception
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice d, const VkDescriptorSetLayoutCreateInfo* c, const VkAllocationCallbacks* a, VkDescriptorSetLayout* l) {
    return malicompat_vkCreateDescriptorSetLayout(d, c, a, l);
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice d, const VkDescriptorSetAllocateInfo* a, VkDescriptorSet* s) {
    return malicompat_vkAllocateDescriptorSets(d, a, s);
}
VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice d, uint32_t w, const VkWriteDescriptorSet* pw, uint32_t c, const VkCopyDescriptorSet* pc) {
    malicompat_vkUpdateDescriptorSets(d, w, pw, c, pc);
}
VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer b, VkPipelineBindPoint p, VkPipelineLayout l, uint32_t f, uint32_t c, const VkDescriptorSet* s, uint32_t o, const uint32_t* po) {
    malicompat_vkCmdBindDescriptorSets(b, p, l, f, c, s, o, po);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice d, const VkQueryPoolCreateInfo* c, const VkAllocationCallbacks* a, VkQueryPool* q) {
    return malicompat_vkCreateQueryPool(d, c, a, q);
}

} // extern "C"
