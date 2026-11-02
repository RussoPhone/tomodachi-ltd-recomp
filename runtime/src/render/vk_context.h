// SPDX-License-Identifier: GPL-3.0-or-later
// The Vulkan device the renderer draws with, and the swapchain of the game
// window. Deliberately small: one graphics queue, one allocation per resource.
#pragma once

#include <vulkan/vulkan.h>

#include <vector>

#include "../common.h"

struct SDL_Window;

namespace nxrt::render {

struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    u8* mapped = nullptr;
    VkDeviceSize size = 0;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize allocation_size = 0;
};

class VulkanContext {
public:
    bool Init(SDL_Window* window); // window may be null: off-screen only
    void Shutdown();

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 queue_family = 0;
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures features{};
    // Native 8/16-bit storage/uniform buffer access (enabled at device creation when the device offers it).
    bool int8_buffer_access = false;
    bool int16_buffer_access = false;
    VkPhysicalDeviceFloatControlsProperties float_controls{};
    VkPhysicalDeviceMemoryProperties memory_properties{};
    bool has_index_uint8 = false;
    bool has_depth_clip_control = false;
    bool has_push_descriptors = false;
    u32 max_push_descriptors = 0;
    PFN_vkCmdPushDescriptorSetKHR cmd_push_descriptor_set = nullptr;

    Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host_visible, VkMemoryPropertyFlags extra = 0);
    void Destroy(Buffer& b);
    // Device memory held by images made through CreateImage, and whether an allocation has failed since the last call (the
    // renderer frees cached textures when it has).
    static u64 ImageBytesHeld();
    static bool TakeAllocationFailure();
    u64 DeviceLocalHeapBytes() const;
    Image CreateImage(const VkImageCreateInfo& info);
    void Destroy(Image& i);
    bool FormatSupports(VkFormat format, VkFormatFeatureFlags features) const;

    // Swapchain (only with a window).
    bool HasSwapchain() const { return swapchain_ != VK_NULL_HANDLE; }
    // Copies `src` (in TRANSFER_SRC_OPTIMAL) scaled into the next swapchain
    // image and presents it. Records into and submits its own command buffer.
    // With NXRT_SCREENSHOT=<file.ppm> the swapchain image of frame
    // NXRT_SCREENSHOT_FRAME (default 1440) is read back and written as PPM.
    void PresentImage(VkImage src, u32 width, u32 height, u64 frame = 0);

private:
    u32 FindMemoryType(u32 bits, VkMemoryPropertyFlags props) const;
    bool CreateSwapchain();
    void DestroySwapchain();
    void WriteScreenshot(const Buffer& buffer, const char* path) const;

    SDL_Window* window_ = nullptr;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat swapchain_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchain_extent_{};
    std::vector<VkImage> swapchain_images_;
    VkCommandPool present_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer present_cmd_ = VK_NULL_HANDLE;
    VkSemaphore acquired_ = VK_NULL_HANDLE;
    VkSemaphore rendered_ = VK_NULL_HANDLE;
    VkFence present_fence_ = VK_NULL_HANDLE;
    bool can_capture_ = false; // swapchain created with TRANSFER_SRC (NXRT_SCREENSHOT)
    bool captured_ = false;
};

} // namespace nxrt::render
