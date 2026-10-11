#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>

namespace malicompat {

/**
 * DescriptorShimTable maintains shadow descriptor states and fallback bindings.
 * 
 * When games attempt to bind descriptor sets with dynamic UPDATE_AFTER_BIND
 * or partial bindings on hardware that does not support them (like Mali-G71),
 * this table buffers descriptor writes, sanitizes layout/pool flags, and pre-binds
 * 1x1 dummy 2D/Cube textures into unwritten slots to prevent GPU MMU page faults.
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

struct CachedLayoutInfo {
    VkDescriptorSetLayoutCreateInfo createInfo{};
    std::vector<VkDescriptorSetLayoutBinding> bindings;
};

struct DeviceDummyResources {
    VkImage image2D{VK_NULL_HANDLE};
    VkDeviceMemory memory2D{VK_NULL_HANDLE};
    VkImageView view2D{VK_NULL_HANDLE};

    VkImage imageCube{VK_NULL_HANDLE};
    VkDeviceMemory memoryCube{VK_NULL_HANDLE};
    VkImageView viewCube{VK_NULL_HANDLE};

    VkSampler sampler{VK_NULL_HANDLE};

    VkBuffer buffer{VK_NULL_HANDLE};
    VkDeviceMemory memoryBuffer{VK_NULL_HANDLE};

    bool initialized{false};
};

class VulkanDescriptorShim {
public:
    static VulkanDescriptorShim& Instance();

    // Device lifecycle hooks for fallback resource allocation
    void InitializeDeviceResources(VkDevice device, VkPhysicalDevice physicalDevice);
    void CleanupDeviceResources(VkDevice device);

    // Hook entry points for Vulkan loader/driver interceptor
    VkResult InterceptCreateDescriptorPool(
        VkDevice device,
        const VkDescriptorPoolCreateInfo* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDescriptorPool* pDescriptorPool,
        PFN_vkCreateDescriptorPool realFunc);

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

    uint32_t FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties);

    std::mutex mutex_;
    std::unordered_map<VkDescriptorSet, ShadowDescriptorSet> shadowSets_;
    std::unordered_map<VkDescriptorSetLayout, CachedLayoutInfo> layoutCache_;
    std::unordered_map<VkDevice, DeviceDummyResources> deviceResources_;
};

} // namespace malicompat
