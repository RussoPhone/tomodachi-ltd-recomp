// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace nxrt::render {
// Exact opaque white is a Vulkan core border color. Other colors retain the
// existing behavior until arbitrary guest border colors are supported.
inline bool UseWhiteDepthBorder(const std::uint32_t* words, bool shader_depth, bool shader_integer,
                                bool image_depth) {
    if (!shader_depth || shader_integer || !image_depth || !(words[0] & (1u << 9))) return false;
    for (unsigned i = 4; i < 8; ++i) {
        if (words[i] != 0x3f800000u) return false;
    }
    return true;
}
} // namespace nxrt::render
