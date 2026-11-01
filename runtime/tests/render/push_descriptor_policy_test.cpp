#include "render/push_descriptor_policy.h"

#include <array>
#include <cstdlib>

using nxrt::render::CanUsePushDescriptorLayout;

int main() {
    const std::array<VkDescriptorSetLayoutBinding, 2> within_limit{{
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 4, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 12, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
    }};
    const auto require = [](bool condition) {
        if (!condition) std::abort();
    };
    const auto count = static_cast<std::uint32_t>(within_limit.size());
    require(CanUsePushDescriptorLayout(true, 32, within_limit.data(), count));
    require(!CanUsePushDescriptorLayout(true, 15, within_limit.data(), count));
    require(!CanUsePushDescriptorLayout(false, 32, within_limit.data(), count));
    require(!CanUsePushDescriptorLayout(true, 0, within_limit.data(), count));

    const VkDescriptorSetLayoutBinding dynamic_buffer{
        0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    require(!CanUsePushDescriptorLayout(true, 32, &dynamic_buffer, 1));

    const VkDescriptorSetLayoutBinding inline_uniform{
        0, VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    require(!CanUsePushDescriptorLayout(true, 32, &inline_uniform, 1));

    require(!CanUsePushDescriptorLayout(true, 32, nullptr, 1));
    require(!CanUsePushDescriptorLayout(true, 32, nullptr, 0));
}
