#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace malicompat {

/**
 * DescriptorShimTable maintains shadow descriptor states.
 * 
 * When games attempt to bind descriptor sets with dynamic UPDATE_AFTER_BIND
 * or partial bindings on hardware that does not support them (like Mali-G71),
 * this table buffers descriptor writes and records them statically prior to
 * draw submissions.
 */
struct ShadowDescriptorBinding {
    VkDescriptorType type{};
    uint32_t count{0};
    VkShaderStageFlags stageFlags{};
    std::vector<VkDescriptorImageInfo> images;
    std::vector<VkDescriptorBufferInfo> buffers;
};

struct ShadowDescriptorSet {
    VkDescriptorSetLayout layout{VK_NULL_HANDLE};
    std::unordered_map<uint32_t, ShadowDescriptorBinding> bindings;
};

class VulkanDescriptorShim {
public:
    static VulkanDescriptorShim& Instance();

    // Hook entry points for Vulkan loader/driver interceptor
    VkResult InterceptCreateDescriptorSetLayout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorSetLayout* pSetLayout,
        PFN_vkCreateDescriptorSetLayout realFunc);

    VkResult InterceptAllocateDescriptorSets(
        VkDevice device,
        const VkDescriptorSetAllocateInfo* pAllocateInfo,
        VkDescriptorSet* pDescriptorSets,
        PFN_vkAllocateDescriptorSets realFunc);

    void InterceptUpdateDescriptorSets(
        VkDevice device,
        uint32_t descriptorWriteCount,
        const VkWriteDescriptorSet* pDescriptorWrites,
        uint32_t descriptorCopyCount,
        const VkCopyDescriptorSet* pDescriptorCopies,
        PFN_vkUpdateDescriptorSets realFunc);

    void InterceptCmdBindDescriptorSets(
        VkCommandBuffer commandBuffer,
        VkPipelineBindPoint pipelineBindPoint,
        VkPipelineLayout layout,
        uint32_t firstSet,
        uint32_t descriptorSetCount,
        const VkDescriptorSet* pDescriptorSets,
        uint32_t dynamicOffsetCount,
        const uint32_t* pDynamicOffsets,
        PFN_vkCmdBindDescriptorSets realFunc);

private:
    VulkanDescriptorShim() = default;
    std::mutex mutex_;
    std::unordered_map<VkDescriptorSet, ShadowDescriptorSet> shadowSets_;
    std::unordered_map<VkDescriptorSetLayout, VkDescriptorSetLayoutCreateInfo> layoutCache_;
};

} // namespace malicompat
