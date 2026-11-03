// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/vk_context.h"

#include <SDL.h>
#include <SDL_vulkan.h>

#include <algorithm>
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace nxrt::render {

namespace {

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name) {
    return std::any_of(list.begin(), list.end(),
                       [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
}

} // namespace

bool VulkanContext::Init(SDL_Window* window) {
    window_ = window;
    std::vector<const char*> instance_extensions;
    if (window) {
        unsigned count = 0;
        SDL_Vulkan_GetInstanceExtensions(window, &count, nullptr);
        instance_extensions.resize(count);
        SDL_Vulkan_GetInstanceExtensions(window, &count, instance_extensions.data());
    }
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO, nullptr, "nxrt", 1, "nxrt", 1, VK_API_VERSION_1_2};
    const VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                   nullptr,
                                   0,
                                   &app,
                                   0,
                                   nullptr,
                                   static_cast<u32>(instance_extensions.size()),
                                   instance_extensions.data()};
    if (vkCreateInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
        LOG_ERROR("vulkan: no instance");
        return false;
    }
    if (window && !SDL_Vulkan_CreateSurface(window, instance, &surface_)) {
        LOG_ERROR("vulkan: no surface: %s", SDL_GetError());
        return false;
    }

    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    // Prefer a discrete GPU that can present to the window.
    int best_score = -1;
    for (VkPhysicalDevice d : devices) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(d, &p);
        u32 nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qs.data());
        for (u32 i = 0; i < nq; ++i) {
            if (!(qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_TRUE;
            if (surface_) vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surface_, &present);
            if (!present) continue;
            const int score = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2
                              : p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 1
                                                                                        : 0;
            if (score > best_score) {
                best_score = score;
                physical = d;
                queue_family = i;
            }
            break;
        }
    }
    if (!physical) {
        LOG_ERROR("vulkan: no usable GPU");
        return false;
    }
    vkGetPhysicalDeviceProperties(physical, &properties);
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_properties);
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    vkGetPhysicalDeviceFeatures2(physical, &f2);
    features = f2.features;
    int8_buffer_access = f12.shaderInt8 && f12.storageBuffer8BitAccess && f12.uniformAndStorageBuffer8BitAccess;
    int16_buffer_access = f2.features.shaderInt16 && f11.storageBuffer16BitAccess &&
                          f11.uniformAndStorageBuffer16BitAccess;
    float_controls.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES;
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &float_controls};
    vkGetPhysicalDeviceProperties2(physical, &p2);

    u32 next = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &next, nullptr);
    std::vector<VkExtensionProperties> available(next);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &next, available.data());
    std::vector<const char*> extensions;
    const char* push_descriptor_env = std::getenv("NXRT_PUSH_DESCRIPTORS");
    const bool request_push_descriptors = push_descriptor_env && std::strcmp(push_descriptor_env, "1") == 0;
    if (surface_) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    has_memory_budget = HasExtension(available, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    if (has_memory_budget) extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    has_index_uint8 = HasExtension(available, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
    has_depth_clip_control = HasExtension(available, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    has_push_descriptors = request_push_descriptors && properties.apiVersion >= VK_API_VERSION_1_1 &&
                           HasExtension(available, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if (has_push_descriptors) {
        VkPhysicalDevicePushDescriptorPropertiesKHR push_properties{};
        push_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR;
        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &push_properties;
        vkGetPhysicalDeviceProperties2(physical, &properties2);
        max_push_descriptors = push_properties.maxPushDescriptors;
        extensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    }
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT uint8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clip{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    void* chain = nullptr;
    if (has_index_uint8) {
        extensions.push_back(VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME);
        uint8.indexTypeUint8 = VK_TRUE;
        uint8.pNext = chain;
        chain = &uint8;
    }
    if (has_depth_clip_control) {
        extensions.push_back(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
        clip.depthClipControl = VK_TRUE;
        clip.pNext = chain;
        chain = &clip;
    }

    // Enable what the shader translator may use, as far as the device offers it.
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, chain};
    e12.shaderInt8 = f12.shaderInt8;
    e12.storageBuffer8BitAccess = f12.storageBuffer8BitAccess;
    e12.uniformAndStorageBuffer8BitAccess = f12.uniformAndStorageBuffer8BitAccess;
    e12.shaderFloat16 = f12.shaderFloat16;
    e12.shaderSampledImageArrayNonUniformIndexing = f12.shaderSampledImageArrayNonUniformIndexing;
    e12.timelineSemaphore = f12.timelineSemaphore;
    VkPhysicalDeviceVulkan11Features e11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &e12};
    e11.storageBuffer16BitAccess = f11.storageBuffer16BitAccess;
    e11.uniformAndStorageBuffer16BitAccess = f11.uniformAndStorageBuffer16BitAccess;
    VkPhysicalDeviceFeatures2 enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e11};
    VkPhysicalDeviceFeatures& e = enable.features;
    // The Switch GPU ignores out-of-range storage/uniform buffer accesses (the descriptor carries a size). Without
    // robustBufferAccess a compute shader that indexes past its buffer is undefined behaviour in Vulkan and can lose
    // the device (the Mii face paint compute 0x20bbac1a88). NXRT_NO_ROBUST_BUFFER_ACCESS=1 turns it off (A/B).
    e.robustBufferAccess = features.robustBufferAccess && !std::getenv("NXRT_NO_ROBUST_BUFFER_ACCESS");
    LOG_INFO("vulkan: robustBufferAccess=%d (device supports %d)", e.robustBufferAccess ? 1 : 0, features.robustBufferAccess ? 1 : 0);
    e.independentBlend = features.independentBlend;
    e.geometryShader = features.geometryShader;
    e.tessellationShader = features.tessellationShader;
    e.depthClamp = features.depthClamp;
    e.depthBiasClamp = features.depthBiasClamp;
    e.fillModeNonSolid = features.fillModeNonSolid;
    e.wideLines = features.wideLines;
    e.largePoints = features.largePoints;
    e.samplerAnisotropy = features.samplerAnisotropy;
    e.textureCompressionBC = features.textureCompressionBC;
    e.textureCompressionASTC_LDR = features.textureCompressionASTC_LDR;
    e.textureCompressionETC2 = features.textureCompressionETC2;
    e.shaderInt16 = features.shaderInt16;
    e.shaderInt64 = features.shaderInt64;
    e.shaderFloat64 = features.shaderFloat64;
    e.shaderClipDistance = features.shaderClipDistance;
    e.shaderCullDistance = features.shaderCullDistance;
    e.shaderStorageImageWriteWithoutFormat = features.shaderStorageImageWriteWithoutFormat;
    e.shaderStorageImageReadWithoutFormat = features.shaderStorageImageReadWithoutFormat;
    e.multiViewport = features.multiViewport;
    e.dualSrcBlend = features.dualSrcBlend;
    e.logicOp = features.logicOp;
    e.vertexPipelineStoresAndAtomics = features.vertexPipelineStoresAndAtomics;
    e.fragmentStoresAndAtomics = features.fragmentStoresAndAtomics;
    e.imageCubeArray = features.imageCubeArray;
    e.sampleRateShading = features.sampleRateShading;
    e.occlusionQueryPrecise = features.occlusionQueryPrecise;
    e.shaderImageGatherExtended = features.shaderImageGatherExtended;
    features = e;

    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, nullptr, 0, queue_family, 1, &priority};
    const VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                 &enable,
                                 0,
                                 1,
                                 &qci,
                                 0,
                                 nullptr,
                                 static_cast<u32>(extensions.size()),
                                 extensions.data(),
                                 nullptr};
    if (vkCreateDevice(physical, &dci, nullptr, &device) != VK_SUCCESS) {
        LOG_ERROR("vulkan: could not create the device");
        return false;
    }
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    if (has_push_descriptors) {
        cmd_push_descriptor_set = reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(
            vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
        has_push_descriptors = cmd_push_descriptor_set != nullptr;
    }
    LOG_INFO("vulkan: %s", properties.deviceName);
    // One line with what a bug report needs about the GPU (the install report and the play log both carry it).
    LOG_INFO("gpu: device=\"%s\" vendor=0x%04x device_id=0x%04x type=%d api=%u.%u.%u driver=0x%x", properties.deviceName,
             properties.vendorID, properties.deviceID, static_cast<int>(properties.deviceType),
             VK_API_VERSION_MAJOR(properties.apiVersion), VK_API_VERSION_MINOR(properties.apiVersion),
             VK_API_VERSION_PATCH(properties.apiVersion), properties.driverVersion);
    if (const char* safe = std::getenv("NXRT_SAFE_MODE"); safe && *safe && std::strcmp(safe, "0") != 0)
        LOG_INFO("gpu: safe mode on (asynchronous submit, asynchronous compute and the GPU texture decoder are off)");
    LOG_INFO("vulkan: push descriptors=%s max=%u", has_push_descriptors ? "yes" : "no", max_push_descriptors);

    if (surface_) {
        const VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                          VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, queue_family};
        vkCreateCommandPool(device, &pci, nullptr, &present_pool_);
        const VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, present_pool_,
                                              VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        vkAllocateCommandBuffers(device, &cai, &present_cmd_);
        const VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(device, &sci, nullptr, &acquired_);
        vkCreateSemaphore(device, &sci, nullptr, &rendered_);
        const VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr, VK_FENCE_CREATE_SIGNALED_BIT};
        vkCreateFence(device, &fci, nullptr, &present_fence_);
        if (!CreateSwapchain()) return false;
    }
    return true;
}

void VulkanContext::Shutdown() {
    if (device) vkDeviceWaitIdle(device);
}

u32 VulkanContext::FindMemoryType(u32 bits, VkMemoryPropertyFlags props) const {
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (memory_properties.memoryTypes[i].propertyFlags & props) == props) return i;
    }
    return ~0u;
}

Buffer VulkanContext::CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible,
                                   VkMemoryPropertyFlags extra) {
    Buffer b;
    b.size = size;
    const VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, nullptr, 0, size, usage,
                                 VK_SHARING_MODE_EXCLUSIVE, 0, nullptr};
    vkCreateBuffer(device, &bci, nullptr, &b.buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    const VkMemoryPropertyFlags props = host_visible
                                            ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                                            : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    u32 type = extra ? FindMemoryType(req.memoryTypeBits, props | extra) : ~0u;
    if (type == ~0u) type = FindMemoryType(req.memoryTypeBits, props);
    const VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size, type};
    vkAllocateMemory(device, &mai, nullptr, &b.memory);
    vkBindBufferMemory(device, b.buffer, b.memory, 0);
    if (host_visible) vkMapMemory(device, b.memory, 0, size, 0, reinterpret_cast<void**>(&b.mapped));
    return b;
}

void VulkanContext::Destroy(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = {};
}

namespace {
std::atomic<u64> g_image_bytes{0};  // device memory held by images made through CreateImage (diagnostic for allocation failures)
std::atomic<u64> g_image_count{0};
std::atomic<bool> g_allocation_failed{false};
} // namespace

u64 VulkanContext::ImageBytesHeld() { return g_image_bytes.load(); }
bool VulkanContext::TakeAllocationFailure() { return g_allocation_failed.exchange(false); }
u64 VulkanContext::DeviceLocalHeapBytes() const {
    u64 best = 0;
    for (u32 i = 0; i < memory_properties.memoryHeapCount; ++i) {
        if (memory_properties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) best = std::max<u64>(best, memory_properties.memoryHeaps[i].size);
    }
    return best;
}

// What the driver says this process may still allocate from the biggest device-local heap, counting what other programs (browser,
// compositor) already hold. 0 when the driver cannot tell (no VK_EXT_memory_budget).
u64 VulkanContext::DeviceLocalBudgetLeft() const {
    if (!has_memory_budget) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget};
    vkGetPhysicalDeviceMemoryProperties2(physical, &props);
    u32 best_heap = 0;
    u64 best_size = 0;
    for (u32 i = 0; i < props.memoryProperties.memoryHeapCount; ++i) {
        const auto& heap = props.memoryProperties.memoryHeaps[i];
        if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && heap.size > best_size) {
            best_size = heap.size;
            best_heap = i;
        }
    }
    return budget.heapBudget[best_heap] > budget.heapUsage[best_heap] ? budget.heapBudget[best_heap] - budget.heapUsage[best_heap] : 0;
}

Image VulkanContext::CreateImage(const VkImageCreateInfo& info) {
    Image i;
    if (const VkResult r = vkCreateImage(device, &info, nullptr, &i.image); r != VK_SUCCESS) {
        LOG_ERROR("vulkan: vkCreateImage failed (%d): format=%d %ux%u layers=%u mips=%u usage=0x%x flags=0x%x; %llu images, %llu MiB held",
                  static_cast<int>(r), static_cast<int>(info.format), info.extent.width, info.extent.height, info.arrayLayers,
                  info.mipLevels, info.usage, info.flags, static_cast<unsigned long long>(g_image_count.load()),
                  static_cast<unsigned long long>(g_image_bytes.load() >> 20));
        return {};
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, i.image, &req);
    i.allocation_size = req.size;
    const VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr, req.size,
                                   FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    // Binding an image to a null memory object crashes the NVIDIA driver, so an allocation that fails (device memory exhausted)
    // has to stop here instead of being ignored.
    if (const VkResult r = vkAllocateMemory(device, &mai, nullptr, &i.memory); r != VK_SUCCESS) {
        g_allocation_failed.store(true);
        LOG_ERROR("vulkan: image memory allocation failed (%d): %llu KiB for format=%d %ux%u layers=%u; %llu images, %llu MiB held",
                  static_cast<int>(r), static_cast<unsigned long long>(req.size >> 10), static_cast<int>(info.format),
                  info.extent.width, info.extent.height, info.arrayLayers,
                  static_cast<unsigned long long>(g_image_count.load()), static_cast<unsigned long long>(g_image_bytes.load() >> 20));
        vkDestroyImage(device, i.image, nullptr);
        return {};
    }
    if (const VkResult r = vkBindImageMemory(device, i.image, i.memory, 0); r != VK_SUCCESS) {
        LOG_ERROR("vulkan: vkBindImageMemory failed (%d)", static_cast<int>(r));
        vkFreeMemory(device, i.memory, nullptr);
        vkDestroyImage(device, i.image, nullptr);
        return {};
    }
    const u64 before = g_image_bytes.fetch_add(req.size);
    ++g_image_count;
    if ((before >> 28) != ((before + req.size) >> 28)) {  // every 256 MiB of growth: a timeline of device memory held by images
        LOG_INFO("vulkan: images hold %llu MiB (%llu images)", static_cast<unsigned long long>((before + req.size) >> 20),
                 static_cast<unsigned long long>(g_image_count.load()));
    }
    return i;
}

void VulkanContext::Destroy(Image& i) {
    if (i.image) vkDestroyImage(device, i.image, nullptr);
    if (i.memory) {
        vkFreeMemory(device, i.memory, nullptr);
        g_image_bytes -= std::min<u64>(g_image_bytes.load(), i.allocation_size);
        if (g_image_count.load()) --g_image_count;
    }
    i = {};
}

bool VulkanContext::FormatSupports(VkFormat format, VkFormatFeatureFlags wanted) const {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(physical, format, &p);
    return (p.optimalTilingFeatures & wanted) == wanted;
}

bool VulkanContext::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface_, &caps);
    u32 nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &nf, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(nf);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface_, &nf, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                                     VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                                                : formats[0];
    for (const auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = f;
            break;
        }
    }
    const bool prefer_srgb = [] {
        const char* value = std::getenv("NXRT_SWAPCHAIN_SRGB");
        return !value || std::strcmp(value, "0") != 0;
    }();
    if (prefer_srgb) {
        const auto srgb = std::find_if(formats.begin(), formats.end(), [](const VkSurfaceFormatKHR& f) {
            return f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                   (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB);
        });
        if (srgb != formats.end()) {
            chosen = *srgb;
        } else {
            LOG_WARN("render: sRGB output preferred, but the surface has no sRGB nonlinear format");
        }
    }
    LOG_INFO("render: swapchain format %d color-space %d (sRGB preferred %d)", static_cast<int>(chosen.format),
             static_cast<int>(chosen.colorSpace), prefer_srgb ? 1 : 0);
    swapchain_format_ = chosen.format;
    int w = 0, h = 0;
    SDL_Vulkan_GetDrawableSize(window_, &w, &h);
    swapchain_extent_ = caps.currentExtent.width != 0xFFFFFFFF
                            ? caps.currentExtent
                            : VkExtent2D{static_cast<u32>(std::max(1, w)), static_cast<u32>(std::max(1, h))};
    if (swapchain_extent_.width == 0 || swapchain_extent_.height == 0) return false;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    can_capture_ = false;
    if (const char* shot = std::getenv("NXRT_SCREENSHOT"); shot && *shot) {
        if (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
            usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            can_capture_ = true;
        } else {
            LOG_WARN("render: swapchain has no TRANSFER_SRC; NXRT_SCREENSHOT disabled");
        }
    }
    const u32 images = std::max(caps.minImageCount, std::min(3u, caps.maxImageCount ? caps.maxImageCount : 3u));
    const VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                       nullptr,
                                       0,
                                       surface_,
                                       images,
                                       chosen.format,
                                       chosen.colorSpace,
                                       swapchain_extent_,
                                       1,
                                       usage,
                                       VK_SHARING_MODE_EXCLUSIVE,
                                       0,
                                       nullptr,
                                       caps.currentTransform,
                                       VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                       VK_PRESENT_MODE_FIFO_KHR,
                                       VK_TRUE,
                                       swapchain_};
    VkSwapchainKHR created;
    if (vkCreateSwapchainKHR(device, &sci, nullptr, &created) != VK_SUCCESS) return false;
    if (swapchain_) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    swapchain_ = created;
    u32 n = 0;
    vkGetSwapchainImagesKHR(device, swapchain_, &n, nullptr);
    swapchain_images_.resize(n);
    vkGetSwapchainImagesKHR(device, swapchain_, &n, swapchain_images_.data());
    return true;
}

void VulkanContext::PresentImage(VkImage src, u32 width, u32 height, u64 frame) {
    if (!swapchain_) return;
    vkWaitForFences(device, 1, &present_fence_, VK_TRUE, UINT64_MAX);
    u32 index = 0;
    VkResult r = vkAcquireNextImageKHR(device, swapchain_, 1'000'000'000, acquired_, VK_NULL_HANDLE, &index);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(device);
        CreateSwapchain();
        if (r == VK_ERROR_OUT_OF_DATE_KHR) return;
    } else if (r != VK_SUCCESS) {
        return;
    }
    vkResetFences(device, 1, &present_fence_);
    VkImage dst = swapchain_images_[index];
    vkResetCommandBuffer(present_cmd_, 0);
    const VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
    vkBeginCommandBuffer(present_cmd_, &bi);
    VkImageMemoryBarrier to_dst{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, nullptr, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dst,
                                {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(present_cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_dst);
    const VkClearColorValue black{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(present_cmd_, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
    if (src) {
        // Fit the frame into the window, keeping its aspect ratio.
        const float sx = static_cast<float>(swapchain_extent_.width) / width;
        const float sy = static_cast<float>(swapchain_extent_.height) / height;
        const float s = std::min(sx, sy);
        const s32 w = static_cast<s32>(width * s), h = static_cast<s32>(height * s);
        const s32 x = (static_cast<s32>(swapchain_extent_.width) - w) / 2;
        const s32 y = (static_cast<s32>(swapchain_extent_.height) - h) / 2;
        const VkImageBlit blit{{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               {{0, 0, 0}, {static_cast<s32>(width), static_cast<s32>(height), 1}},
                               {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                               {{x, y, 0}, {x + w, y + h, 1}}};
        vkCmdBlitImage(present_cmd_, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    static const u64 shot_frame = std::getenv("NXRT_SCREENSHOT_FRAME")
                                      ? std::strtoull(std::getenv("NXRT_SCREENSHOT_FRAME"), nullptr, 0)
                                      : 1440;
    const char* shot_path = std::getenv("NXRT_SCREENSHOT");
    const bool capture = can_capture_ && !captured_ && shot_path && *shot_path && frame >= shot_frame;
    Buffer shot;
    VkImageLayout last_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (capture) {
        shot = CreateBuffer(VkDeviceSize{swapchain_extent_.width} * swapchain_extent_.height * 4,
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        VkImageMemoryBarrier to_src = to_dst;
        to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(present_cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &to_src);
        const VkBufferImageCopy copy{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                                     {swapchain_extent_.width, swapchain_extent_.height, 1}};
        vkCmdCopyImageToBuffer(present_cmd_, dst, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, shot.buffer, 1, &copy);
        last_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    VkImageMemoryBarrier to_present = to_dst;
    to_present.srcAccessMask = capture ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = last_layout;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(present_cmd_, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &to_present);
    vkEndCommandBuffer(present_cmd_);
    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 1, &acquired_, &wait_stage, 1, &present_cmd_, 1,
                          &rendered_};
    vkQueueSubmit(queue, 1, &si, present_fence_);
    if (capture) {
        vkWaitForFences(device, 1, &present_fence_, VK_TRUE, UINT64_MAX);
        WriteScreenshot(shot, shot_path);
        Destroy(shot);
        captured_ = true;
    }
    const VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, nullptr, 1, &rendered_, 1, &swapchain_, &index,
                              nullptr};
    r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(device);
        CreateSwapchain();
    }
}

void VulkanContext::WriteScreenshot(const Buffer& buffer, const char* path) const {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        LOG_WARN("render: screenshot: cannot open %s", path);
        return;
    }
    const u32 w = swapchain_extent_.width, h = swapchain_extent_.height;
    const bool bgr = swapchain_format_ == VK_FORMAT_B8G8R8A8_UNORM || swapchain_format_ == VK_FORMAT_B8G8R8A8_SRGB;
    std::fprintf(f, "P6\n%u %u\n255\n", w, h);
    std::vector<u8> row(static_cast<size_t>(w) * 3);
    u64 lit = 0;
    for (u32 y = 0; y < h; ++y) {
        const u8* src = buffer.mapped + static_cast<size_t>(y) * w * 4;
        for (u32 x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + (bgr ? 2 : 0)];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + (bgr ? 0 : 2)];
            lit += (row[x * 3] | row[x * 3 + 1] | row[x * 3 + 2]) != 0;
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    LOG_INFO("render: screenshot of the swapchain written to %s (%ux%u, format %d, %llu non-black pixels)", path, w, h,
             static_cast<int>(swapchain_format_), static_cast<unsigned long long>(lit));
}

void VulkanContext::DestroySwapchain() {
    if (swapchain_) vkDestroySwapchainKHR(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
}

} // namespace nxrt::render
