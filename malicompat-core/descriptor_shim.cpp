#include "descriptor_shim.h"
#include "vulkan_layer_exports.h"
#include <vector>
#include <cstring>
#include <dlfcn.h>

namespace {
// M6: resolve device-level functions inside the vendor dispatch domain.
// dlsym(RTLD_DEFAULT, ...) can land on a different loader copy (memfd) that
// never saw our (vendor) VkDevice and then crashes in its dispatch.
PFN_vkVoidFunction shim_device_proc(VkDevice device, const char* name) {
    PFN_vkVoidFunction pfn = malicompat_resolve_vendor_device_proc(device, name);
    if (!pfn) {
        pfn = reinterpret_cast<PFN_vkVoidFunction>(dlsym(RTLD_DEFAULT, name));
    }
    return pfn;
}
}  // namespace

#if __has_include(<android/log.h>)
#include <android/log.h>
#define LOG_TAG "MaliCompat-Shim"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#include <cstdio>
#define LOG_TAG "MaliCompat-Shim"
#define LOGI(...) do { printf("[INFO] " LOG_TAG ": " __VA_ARGS__); printf("\n"); } while(0)
#define LOGW(...) do { printf("[WARN] " LOG_TAG ": " __VA_ARGS__); printf("\n"); } while(0)
#define LOGE(...) do { fprintf(stderr, "[ERROR] " LOG_TAG ": " __VA_ARGS__); fprintf(stderr, "\n"); } while(0)
#endif

namespace malicompat {

VulkanDescriptorShim& VulkanDescriptorShim::Instance() {
    static VulkanDescriptorShim instance;
    return instance;
}

uint32_t VulkanDescriptorShim::FindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    if (!physicalDevice) return 0;
    
    // Attempt dynamic lookup of vkGetPhysicalDeviceMemoryProperties
    auto pfnGetMemProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        dlsym(RTLD_DEFAULT, "vkGetPhysicalDeviceMemoryProperties"));
    if (!pfnGetMemProps) {
        return 0;
    }

    VkPhysicalDeviceMemoryProperties memProperties{};
    pfnGetMemProps(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    // Fallback: any matching type index
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; ++i) {
        if (typeFilter & (1 << i)) {
            return i;
        }
    }
    return 0;
}

void VulkanDescriptorShim::InitializeDeviceResources(VkDevice device, VkPhysicalDevice physicalDevice) {
    if (!device) return;
    std::lock_guard<std::mutex> lock(mutex_);

    if (deviceResources_.find(device) != deviceResources_.end()) {
        return; // Already initialized
    }

    DeviceDummyResources res{};

    auto pfnCreateImage = reinterpret_cast<PFN_vkCreateImage>(shim_device_proc(device, "vkCreateImage"));
    auto pfnGetImageMemReqs = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(shim_device_proc(device, "vkGetImageMemoryRequirements"));
    auto pfnAllocMem = reinterpret_cast<PFN_vkAllocateMemory>(shim_device_proc(device, "vkAllocateMemory"));
    auto pfnBindImageMem = reinterpret_cast<PFN_vkBindImageMemory>(shim_device_proc(device, "vkBindImageMemory"));
    auto pfnCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(shim_device_proc(device, "vkCreateImageView"));
    auto pfnCreateSampler = reinterpret_cast<PFN_vkCreateSampler>(shim_device_proc(device, "vkCreateSampler"));
    auto pfnCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(shim_device_proc(device, "vkCreateBuffer"));
    auto pfnGetBufferMemReqs = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(shim_device_proc(device, "vkGetBufferMemoryRequirements"));
    auto pfnBindBufferMem = reinterpret_cast<PFN_vkBindBufferMemory>(shim_device_proc(device, "vkBindBufferMemory"));

    if (pfnCreateImage && pfnGetImageMemReqs && pfnAllocMem && pfnBindImageMem && pfnCreateImageView) {
        // 1. Create 1x1 2D Dummy Image (RGBA8)
        VkImageCreateInfo imgInfo2D{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imgInfo2D.imageType = VK_IMAGE_TYPE_2D;
        imgInfo2D.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfo2D.extent = {1, 1, 1};
        imgInfo2D.mipLevels = 1;
        imgInfo2D.arrayLayers = 1;
        imgInfo2D.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfo2D.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfo2D.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfo2D.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imgInfo2D.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (pfnCreateImage(device, &imgInfo2D, nullptr, &res.image2D) == VK_SUCCESS) {
            VkMemoryRequirements memReqs{};
            pfnGetImageMemReqs(device, res.image2D, &memReqs);

            VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocInfo.allocationSize = memReqs.size > 0 ? memReqs.size : 256;
            allocInfo.memoryTypeIndex = FindMemoryType(physicalDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            if (pfnAllocMem(device, &allocInfo, nullptr, &res.memory2D) == VK_SUCCESS) {
                pfnBindImageMem(device, res.image2D, res.memory2D, 0);

                VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                viewInfo.image = res.image2D;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
                viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                viewInfo.subresourceRange.baseMipLevel = 0;
                viewInfo.subresourceRange.levelCount = 1;
                viewInfo.subresourceRange.baseArrayLayer = 0;
                viewInfo.subresourceRange.layerCount = 1;

                pfnCreateImageView(device, &viewInfo, nullptr, &res.view2D);
            }
        }

        // 2. Create 1x1 Cube Dummy Image
        VkImageCreateInfo imgInfoCube{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imgInfoCube.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
        imgInfoCube.imageType = VK_IMAGE_TYPE_2D;
        imgInfoCube.format = VK_FORMAT_R8G8B8A8_UNORM;
        imgInfoCube.extent = {1, 1, 1};
        imgInfoCube.mipLevels = 1;
        imgInfoCube.arrayLayers = 6;
        imgInfoCube.samples = VK_SAMPLE_COUNT_1_BIT;
        imgInfoCube.tiling = VK_IMAGE_TILING_OPTIMAL;
        imgInfoCube.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imgInfoCube.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imgInfoCube.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (pfnCreateImage(device, &imgInfoCube, nullptr, &res.imageCube) == VK_SUCCESS) {
            VkMemoryRequirements memReqs{};
            pfnGetImageMemReqs(device, res.imageCube, &memReqs);

            VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocInfo.allocationSize = memReqs.size > 0 ? memReqs.size : 1024;
            allocInfo.memoryTypeIndex = FindMemoryType(physicalDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

            if (pfnAllocMem(device, &allocInfo, nullptr, &res.memoryCube) == VK_SUCCESS) {
                pfnBindImageMem(device, res.imageCube, res.memoryCube, 0);

                VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                viewInfo.image = res.imageCube;
                viewInfo.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
                viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
                viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                viewInfo.subresourceRange.baseMipLevel = 0;
                viewInfo.subresourceRange.levelCount = 1;
                viewInfo.subresourceRange.baseArrayLayer = 0;
                viewInfo.subresourceRange.layerCount = 6;

                pfnCreateImageView(device, &viewInfo, nullptr, &res.viewCube);
            }
        }
    }

    // 3. Create Default Dummy Sampler
    if (pfnCreateSampler) {
        VkSamplerCreateInfo samplerInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        pfnCreateSampler(device, &samplerInfo, nullptr, &res.sampler);
    }

    // 4. Create 64-byte Dummy Buffer
    if (pfnCreateBuffer && pfnGetBufferMemReqs && pfnAllocMem && pfnBindBufferMem) {
        VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bufferInfo.size = 64;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (pfnCreateBuffer(device, &bufferInfo, nullptr, &res.buffer) == VK_SUCCESS) {
            VkMemoryRequirements memReqs{};
            pfnGetBufferMemReqs(device, res.buffer, &memReqs);

            VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocInfo.allocationSize = memReqs.size > 0 ? memReqs.size : 64;
            allocInfo.memoryTypeIndex = FindMemoryType(physicalDevice, memReqs.memoryTypeBits, 0);

            if (pfnAllocMem(device, &allocInfo, nullptr, &res.memoryBuffer) == VK_SUCCESS) {
                pfnBindBufferMem(device, res.buffer, res.memoryBuffer, 0);
            }
        }
    }

    res.initialized = true;
    deviceResources_[device] = res;
    LOGI("Allocated dummy fallback resources for device=%p (2D view=%p, Cube view=%p, Sampler=%p, Buffer=%p)",
         device, res.view2D, res.viewCube, res.sampler, res.buffer);
}

void VulkanDescriptorShim::CleanupDeviceResources(VkDevice device) {
    if (!device) return;
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = deviceResources_.find(device);
    if (it == deviceResources_.end()) return;

    auto& res = it->second;

    auto pfnDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(shim_device_proc(device, "vkDestroyImageView"));
    auto pfnDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(shim_device_proc(device, "vkDestroyImage"));
    auto pfnFreeMem = reinterpret_cast<PFN_vkFreeMemory>(shim_device_proc(device, "vkFreeMemory"));
    auto pfnDestroySampler = reinterpret_cast<PFN_vkDestroySampler>(shim_device_proc(device, "vkDestroySampler"));
    auto pfnDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(shim_device_proc(device, "vkDestroyBuffer"));

    if (pfnDestroyImageView) {
        if (res.view2D) pfnDestroyImageView(device, res.view2D, nullptr);
        if (res.viewCube) pfnDestroyImageView(device, res.viewCube, nullptr);
    }
    if (pfnDestroyImage) {
        if (res.image2D) pfnDestroyImage(device, res.image2D, nullptr);
        if (res.imageCube) pfnDestroyImage(device, res.imageCube, nullptr);
    }
    if (pfnDestroySampler && res.sampler) {
        pfnDestroySampler(device, res.sampler, nullptr);
    }
    if (pfnDestroyBuffer && res.buffer) {
        pfnDestroyBuffer(device, res.buffer, nullptr);
    }
    if (pfnFreeMem) {
        if (res.memory2D) pfnFreeMem(device, res.memory2D, nullptr);
        if (res.memoryCube) pfnFreeMem(device, res.memoryCube, nullptr);
        if (res.memoryBuffer) pfnFreeMem(device, res.memoryBuffer, nullptr);
    }

    deviceResources_.erase(it);
    LOGI("Cleaned up dummy fallback resources for device=%p", device);
}

VkResult VulkanDescriptorShim::InterceptCreateDescriptorPool(
    VkDevice device,
    const VkDescriptorPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDescriptorPool* pDescriptorPool,
    PFN_vkCreateDescriptorPool realFunc) {

    if (!pCreateInfo || !realFunc) return VK_ERROR_INITIALIZATION_FAILED;

    VkDescriptorPoolCreateInfo sanitizedInfo = *pCreateInfo;
    // Strip UPDATE_AFTER_BIND flag from descriptor pool to prevent Mali HAL rejection
    sanitizedInfo.flags &= ~VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;

    return realFunc(device, &sanitizedInfo, pAllocator, pDescriptorPool);
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
        CachedLayoutInfo cached{};
        cached.createInfo = sanitizedInfo;
        if (pCreateInfo->pBindings && pCreateInfo->bindingCount > 0) {
            cached.bindings.assign(pCreateInfo->pBindings, pCreateInfo->pBindings + pCreateInfo->bindingCount);
        }
        layoutCache_[*pSetLayout] = cached;
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

        auto devResIt = deviceResources_.find(device);
        const DeviceDummyResources* pDummyRes = (devResIt != deviceResources_.end() && devResIt->second.initialized) ? &devResIt->second : nullptr;

        for (uint32_t i = 0; i < pAllocateInfo->descriptorSetCount; ++i) {
            ShadowDescriptorSet shadow{};
            VkDescriptorSetLayout layoutHandle = VK_NULL_HANDLE;
            if (pAllocateInfo->pSetLayouts) {
                layoutHandle = pAllocateInfo->pSetLayouts[i];
                shadow.layout = layoutHandle;
            }

            // Pre-populate dummy fallback descriptors for all bindings in the layout (F8)
            auto layoutIt = layoutCache_.find(layoutHandle);
            if (layoutIt != layoutCache_.end() && pDummyRes) {
                std::vector<VkWriteDescriptorSet> writes;
                std::vector<std::vector<VkDescriptorImageInfo>> imageInfos;
                std::vector<std::vector<VkDescriptorBufferInfo>> bufferInfos;

                for (const auto& binding : layoutIt->second.bindings) {
                    if (binding.descriptorCount == 0) continue;

                    ShadowDescriptorBinding sBinding{};
                    sBinding.type = binding.descriptorType;
                    sBinding.count = binding.descriptorCount;
                    sBinding.stageFlags = binding.stageFlags;

                    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    write.dstSet = pDescriptorSets[i];
                    write.dstBinding = binding.binding;
                    write.dstArrayElement = 0;
                    write.descriptorCount = binding.descriptorCount;
                    write.descriptorType = binding.descriptorType;

                    if (binding.descriptorType == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER ||
                        binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
                        std::vector<VkDescriptorImageInfo> imgVec(binding.descriptorCount);
                        for (uint32_t elem = 0; elem < binding.descriptorCount; ++elem) {
                            imgVec[elem].sampler = pDummyRes->sampler;
                            // Set 1 Binding 1 is typically Cube maps on NFSMW
                            bool isCube = (binding.binding == 1 && pDummyRes->viewCube != VK_NULL_HANDLE);
                            imgVec[elem].imageView = isCube ? pDummyRes->viewCube : pDummyRes->view2D;
                            imgVec[elem].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        }
                        sBinding.images = imgVec;
                        imageInfos.push_back(imgVec);
                        write.pImageInfo = imageInfos.back().data();
                        writes.push_back(write);
                    } else if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                               binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
                        std::vector<VkDescriptorBufferInfo> bufVec(binding.descriptorCount);
                        for (uint32_t elem = 0; elem < binding.descriptorCount; ++elem) {
                            bufVec[elem].buffer = pDummyRes->buffer;
                            bufVec[elem].offset = 0;
                            bufVec[elem].range = 64;
                        }
                        sBinding.buffers = bufVec;
                        bufferInfos.push_back(bufVec);
                        write.pBufferInfo = bufferInfos.back().data();
                        writes.push_back(write);
                    } else if (binding.descriptorType == VK_DESCRIPTOR_TYPE_SAMPLER) {
                        std::vector<VkDescriptorImageInfo> sampVec(binding.descriptorCount);
                        for (uint32_t elem = 0; elem < binding.descriptorCount; ++elem) {
                            sampVec[elem].sampler = pDummyRes->sampler;
                        }
                        sBinding.images = sampVec;
                        imageInfos.push_back(sampVec);
                        write.pImageInfo = imageInfos.back().data();
                        writes.push_back(write);
                    }

                    shadow.bindings[binding.binding] = sBinding;
                }

                // Push dummy pre-bindings to driver so unwritten slots are safe from GPU faults
                if (!writes.empty()) {
                    auto pfnUpdateDescSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(
                        shim_device_proc(device, "vkUpdateDescriptorSets"));
                    if (pfnUpdateDescSets) {
                        pfnUpdateDescSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
                    }
                }
            }

            shadowSets_[pDescriptorSets[i]] = shadow;
        }
    }
    return result;
}

// A2: descriptor-write density probe.
//
// Option A hinges on one question we can answer at runtime. The guest declares a
// 4096-entry bindless texture heap, but maxPerStageDescriptorSampledImages is 256
// on this Mali-G71, so a single shader stage can never address more than 256
// descriptors no matter how the layout is split. That forces a dense physical
// array plus a logical->physical map.
//
// If the guest allocates its heap slots densely (0,1,2,...N), then a plain clamp
// of descriptorCount 4096 -> 256 plus an SPIR-V index mask of `idx & 255` is the
// whole fix: no per-draw mapping, no lookup table. If the guest allocates
// sparsely, we need a dense allocator and an indirection LUT in the shader.
//
// So: record which logical indices the guest actually writes, and report the
// observed maximum and the number of distinct slots. Dense max ~ N is the answer
// we want.
namespace {
struct HeapDensity {
    bool reporting = false;
    uint32_t writes = 0;
    uint32_t maxIndex = 0;
    uint32_t lastReported = 0;
};
HeapDensity g_density;
} // namespace

void VulkanDescriptorShim::InterceptUpdateDescriptorSets(
    VkDevice device,
    uint32_t descriptorWriteCount,
    const VkWriteDescriptorSet* pDescriptorWrites,
    uint32_t descriptorCopyCount,
    const VkCopyDescriptorSet* pDescriptorCopies,
    PFN_vkUpdateDescriptorSets realFunc) {

    if (!realFunc) return;

    if (descriptorWriteCount > 0 && !pDescriptorWrites) {
        LOGE("InterceptUpdateDescriptorSets: pDescriptorWrites is NULL with descriptorWriteCount > 0");
        return;
    }
    if (descriptorCopyCount > 0 && !pDescriptorCopies) {
        LOGE("InterceptUpdateDescriptorSets: pDescriptorCopies is NULL with descriptorCopyCount > 0");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (uint32_t i = 0; i < descriptorWriteCount; ++i) {
            const auto& write = pDescriptorWrites[i];

            // A2: density probe. Track the highest logical index the guest ever
            // writes per binding, so we can tell a dense allocator (max ~ live
            // count) from a sparse one (max in the thousands). That decides whether
            // Option A is a simple clamp+mask or needs a dense map plus shader
            // indirection.
            {
                const uint32_t end = write.dstArrayElement + write.descriptorCount;
                if (!g_density.reporting && end > 256) {
                    g_density.reporting = true;  // only the big bindless heaps matter
                }
                if (g_density.reporting && write.descriptorCount == 1) {
                    ++g_density.writes;
                    if (write.dstArrayElement > g_density.maxIndex) {
                        g_density.maxIndex = write.dstArrayElement;
                    }
                    if (g_density.writes - g_density.lastReported >= 500) {
                        g_density.lastReported = g_density.writes;
                        LOGI("A2: HEAP DENSITY binding=%u writes=%u maxLogicalIndex=%u "
                             "(live textures ~26; dense allocator => clamp+mask is viable)",
                             (unsigned)write.dstBinding, g_density.writes, g_density.maxIndex);
                    }
                }
            }

            auto it = shadowSets_.find(write.dstSet);
            if (it != shadowSets_.end()) {
                auto& binding = it->second.bindings[write.dstBinding];
                binding.type = write.descriptorType;
                if (write.dstArrayElement + write.descriptorCount > binding.count) {
                    binding.count = write.dstArrayElement + write.descriptorCount;
                }
                if (write.pImageInfo) {
                    if (binding.images.size() < binding.count) {
                        binding.images.resize(binding.count);
                    }
                    for (uint32_t elem = 0; elem < write.descriptorCount; ++elem) {
                        binding.images[write.dstArrayElement + elem] = write.pImageInfo[elem];
                    }
                }
                if (write.pBufferInfo) {
                    if (binding.buffers.size() < binding.count) {
                        binding.buffers.resize(binding.count);
                    }
                    for (uint32_t elem = 0; elem < write.descriptorCount; ++elem) {
                        binding.buffers[write.dstArrayElement + elem] = write.pBufferInfo[elem];
                    }
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

    // Mali-G71 hardware limit: maxBoundDescriptorSets == 4 (Sets 0..3)
    if (firstSet >= 4) {
        LOGW("InterceptCmdBindDescriptorSets: firstSet %u >= 4 exceeds Mali maxBoundDescriptorSets limit, dropping binding call", firstSet);
        return;
    }

    uint32_t clampedCount = descriptorSetCount;
    if (firstSet + clampedCount > 4) {
        LOGW("InterceptCmdBindDescriptorSets: firstSet (%u) + descriptorSetCount (%u) > 4, clamping to %u sets",
             firstSet, descriptorSetCount, 4 - firstSet);
        clampedCount = 4 - firstSet;
    }

    // Direct invocation with clamped sets: shadow state is already flushed prior to binding
    realFunc(commandBuffer, pipelineBindPoint, layout, firstSet, clampedCount, pDescriptorSets, dynamicOffsetCount, pDynamicOffsets);
}

} // namespace malicompat
