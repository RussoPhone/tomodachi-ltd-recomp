// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <vulkan/vulkan.h>

namespace nxrt::render {

constexpr bool CanRecycleAfterFlush(VkResult submit_result, VkResult wait_result) noexcept {
    return submit_result == VK_SUCCESS && wait_result == VK_SUCCESS;
}

} // namespace nxrt::render
