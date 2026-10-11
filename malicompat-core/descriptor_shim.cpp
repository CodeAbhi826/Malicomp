#include "descriptor_shim.h"
#include <vector>
#include <cstring>

namespace malicompat {

VulkanDescriptorShim& VulkanDescriptorShim::Instance() {
    static VulkanDescriptorShim instance;
    return instance;
}

VkResult VulkanDescriptorShim::InterceptCreateDescriptorSetLayout(
    VkDevice device,
    const VkDescriptorSetLayoutCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorSetLayout* pSetLayout,
    PFN_vkCreateDescriptorSetLayout realFunc) {

    if (!pCreateInfo || !realFunc) return VK_ERROR_INITIALIZATION_FAILED;

    std::lock_guard<std::mutex> lock(mutex_);

    // Sanitize binding flags: remove UPDATE_AFTER_BIND and PARTIALLY_BOUND if driver lacks support
    VkDescriptorSetLayoutCreateInfo sanitizedInfo = *pCreateInfo;
    
    // Check if pNext contains VkDescriptorSetLayoutBindingFlagsCreateInfo
    const auto* nextHeader = reinterpret_cast<const VkBaseInStructure*>(pCreateInfo->pNext);
    std::vector<VkDescriptorBindingFlags> sanitizedBindingFlags;
    VkDescriptorSetLayoutBindingFlagsCreateInfo sanitizedFlagsInfo{};

    while (nextHeader) {
        if (nextHeader->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO) {
            const auto* flagsInfo = reinterpret_cast<const VkDescriptorSetLayoutBindingFlagsCreateInfo*>(nextHeader);
            if (flagsInfo->pBindingFlags && flagsInfo->bindingCount) {
                sanitizedBindingFlags.assign(flagsInfo->pBindingFlags, flagsInfo->pBindingFlags + flagsInfo->bindingCount);
                for (auto& flag : sanitizedBindingFlags) {
                    // Strip unsupported flags for Mali-G71
                    flag &= ~(VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                              VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                              VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT);
                }
                sanitizedFlagsInfo = *flagsInfo;
                sanitizedFlagsInfo.pBindingFlags = sanitizedBindingFlags.data();
                sanitizedInfo.pNext = &sanitizedFlagsInfo;
            }
            break;
        }
        nextHeader = nextHeader->pNext;
    }

    // Strip UPDATE_AFTER_BIND from set layout create flags if present
    sanitizedInfo.flags &= ~VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;

    VkResult result = realFunc(device, &sanitizedInfo, pAllocator, pSetLayout);
    if (result == VK_SUCCESS && pSetLayout && *pSetLayout) {
        layoutCache_[*pSetLayout] = sanitizedInfo;
    }
    return result;
}

VkResult VulkanDescriptorShim::InterceptAllocateDescriptorSets(
    VkDevice device,
    const VkDescriptorSetAllocateInfo* pAllocateInfo,
    VkDescriptorSet* pDescriptorSets,
    PFN_vkAllocateDescriptorSets realFunc) {

    if (!pAllocateInfo || !realFunc) return VK_ERROR_INITIALIZATION_FAILED;

    VkResult result = realFunc(device, pAllocateInfo, pDescriptorSets);
    if (result == VK_SUCCESS && pDescriptorSets) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
            ShadowDescriptorSet shadow{};
            if (pAllocateInfo->pSetLayouts) {
                shadow.layout = pAllocateInfo->pSetLayouts[i];
            }
            shadowSets_[pDescriptorSets[i]] = shadow;
        }
    }
    return result;
}

void VulkanDescriptorShim::InterceptUpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies,
    PFN_vkUpdateDescriptorSets realFunc) {

    if (!realFunc) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
            const auto& write = pDescriptorWrites[i];
            auto it = shadowSets_.find(write.dstSet);
            if (it != shadowSets_.end()) {
                auto& binding = it->second.bindings[write.dstBinding];
                binding.type = write.descriptorType;
                binding.count = write.descriptorCount;
                if (write.pImageInfo) {
                    binding.images.assign(write.pImageInfo, write.pImageInfo + write.descriptorCount);
                }
                if (write.pBufferInfo) {
                    binding.buffers.assign(write.pBufferInfo, write.pBufferInfo + write.descriptorCount);
                }
            }
        }
    }

    // Dispatch update to real driver
    realFunc(device, descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies);
}

void VulkanDescriptorShim::InterceptCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer,
    VkPipelineBindPoint pipelineBindPoint,
    VkPipelineLayout layout,
    uint32_t firstSet,
    uint32_t descriptorSetCount,
    const VkDescriptorSet* pDescriptorSets,
    uint32_t dynamicOffsetCount,
    const uint32_t* pDynamicOffsets,
    PFN_vkCmdBindDescriptorSets realFunc) {

    if (!realFunc) return;

    // Direct invocation: shadow state is already flushed prior to binding
    realFunc(commandBuffer, pipelineBindPoint, layout, firstSet, descriptorSetCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}

} // namespace malicompat
