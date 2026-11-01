// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

#include <vulkan/vulkan.h>

namespace nxrt::render {

// Push descriptors are optional. Keep the ordinary descriptor-set path for
// unsupported devices, unsupported descriptor types, and layouts over the
// device's maxPushDescriptors limit.
constexpr bool CanUsePushDescriptorLayout(bool extension_enabled, std::uint32_t max_push_descriptors,
                                          const VkDescriptorSetLayoutBinding* bindings,
                                          std::uint32_t binding_count) noexcept {
    if (!extension_enabled || max_push_descriptors == 0 || binding_count == 0 || !bindings) return false;
    std::uint64_t descriptor_count = 0;
    for (std::uint32_t i = 0; i < binding_count; ++i) {
        const VkDescriptorSetLayoutBinding& binding = bindings[i];
        if (binding.descriptorCount == 0 || binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC ||
            binding.descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
            binding.descriptorType == VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK) {
            return false;
        }
        descriptor_count += binding.descriptorCount;
        if (descriptor_count > max_push_descriptors) return false;
    }
    return descriptor_count != 0;
}

} // namespace nxrt::render
