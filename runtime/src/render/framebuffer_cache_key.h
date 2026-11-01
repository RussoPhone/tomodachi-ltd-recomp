// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <span>

namespace nxrt::render {

// Framebuffer cache keys store attachment image views followed by the render pass.
inline bool FramebufferCacheKeyUsesImageView(std::span<const std::uint64_t> key,
                                             std::uint64_t image_view) {
    return key.size() > 1 && std::find(key.begin(), key.end() - 1, image_view) != key.end() - 1;
}

} // namespace nxrt::render
