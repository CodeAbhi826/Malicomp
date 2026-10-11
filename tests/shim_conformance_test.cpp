#include "descriptor_shim.h"
#include <iostream>
#include <vector>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

int main() {
    std::cout << "[MaliCompat-Test] Starting Descriptor Shim Conformance Test...\n";

    void* libvulkan = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!libvulkan) {
        std::cerr << "Failed to open libvulkan.so\n";
        return 1;
    }

    auto vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(libvulkan, "vkGetInstanceProcAddr"));
    auto vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr(nullptr, "vkCreateInstance"));

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "MaliCompat-ShimTest";
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    createInfo.pApplicationInfo = &appInfo;

    VkInstance instance{VK_NULL_HANDLE};
    if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
        std::cerr << "Failed to create instance\n";
        dlclose(libvulkan);
        return 1;
    }

    auto vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(vkGetInstanceProcAddr(instance, "vkEnumeratePhysicalDevices"));
    auto vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(vkGetInstanceProcAddr(instance, "vkCreateDevice"));
    auto vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(vkGetInstanceProcAddr(instance, "vkDestroyDevice"));
    auto vkDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(vkGetInstanceProcAddr(instance, "vkDestroyInstance"));

    uint32_t count = 1;
    VkPhysicalDevice gpu{VK_NULL_HANDLE};
    vkEnumeratePhysicalDevices(instance, &count, &gpu);

    float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueCount = 1;
    queueInfo.queueFamilyIndex = 0;
    queueInfo.pQueuePriorities = &priority;

    VkDeviceCreateInfo devInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    devInfo.queueCreateInfoCount = 1;
    devInfo.pQueueCreateInfos = &queueInfo;

    VkDevice device{VK_NULL_HANDLE};
    if (vkCreateDevice(gpu, &devInfo, nullptr, &device) != VK_SUCCESS) {
        std::cerr << "Failed to create logical device\n";
        vkDestroyInstance(instance, nullptr);
        dlclose(libvulkan);
        return 1;
    }

    auto realCreateLayout = reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(dlsym(libvulkan, "vkCreateDescriptorSetLayout"));
    auto realDestroyLayout = reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(dlsym(libvulkan, "vkDestroyDescriptorSetLayout"));

    // Test 1: Try creating a layout requesting UPDATE_AFTER_BIND and PARTIALLY_BOUND (which Mali-G71 normally rejects)
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount = 16;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorBindingFlags flags = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flagsInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flagsInfo.bindingCount = 1;
    flagsInfo.pBindingFlags = &flags;

    VkDescriptorSetLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    layoutInfo.pNext = &flagsInfo;
    layoutInfo.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;

    std::cout << "[MaliCompat-Test] 1. Passing bindless flags through VulkanDescriptorShim::InterceptCreateDescriptorSetLayout...\n";

    VkDescriptorSetLayout shimmedLayout{VK_NULL_HANDLE};
    VkResult res = malicompat::VulkanDescriptorShim::Instance().InterceptCreateDescriptorSetLayout(
        device, &layoutInfo, nullptr, &shimmedLayout, realCreateLayout);

    if (res == VK_SUCCESS && shimmedLayout != VK_NULL_HANDLE) {
        std::cout << "[MaliCompat-Test] SUCCESS: Layout successfully created on Mali-G71 without driver crash!\n";
        realDestroyLayout(device, shimmedLayout, nullptr);
    } else {
        std::cerr << "[MaliCompat-Test] FAILED: Driver returned error " << res << "\n";
    }

    vkDestroyDevice(device, nullptr);
    vkDestroyInstance(instance, nullptr);
    dlclose(libvulkan);
    return (res == VK_SUCCESS) ? 0 : 1;
}
