// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

namespace nxrt::render {
struct BlockTexelExtent { std::uint32_t width, height; };

// Observed BC3 encoder: one RGBA32_UINT texel stores one 4x4 BC3 block.
inline std::optional<BlockTexelExtent> PlanBc3Target(std::uint32_t width, std::uint32_t height,
                                                    std::uint32_t layers, std::uint32_t samples,
                                                    bool bc3, bool rgba32_uint_view) {
    if (!bc3 || !rgba32_uint_view || layers != 1 || samples > 1 || !width || !height ||
        width > 16384 || height > 16384) return std::nullopt;
    return BlockTexelExtent{(width + 3) / 4, (height + 3) / 4};
}

// Inverse of the renderer's 2D GOB unswizzle. Copy integer bytes verbatim,
// including bit patterns that would be destroyed by float conversion.
inline bool StoreBlockLinear128(std::span<const std::uint8_t> linear, std::span<std::uint8_t> tiled,
                                std::uint32_t width, std::uint32_t height,
                                std::uint32_t block_height, std::uint32_t tile_width) {
    if (!width || !height || width > 4096 || height > 4096 || block_height > 5 || tile_width > 5 ||
        linear.size() != std::uint64_t{width} * height * 16) return false;
    const std::uint32_t gob_h = 1u << block_height, align = 1u << tile_width;
    const std::uint32_t gobs_x = ((width * 16 + 63) / 64 + align - 1) / align * align;
    const std::size_t block_size = std::size_t{gob_h} * 512;
    const std::size_t required = std::size_t{(height + 8 * gob_h - 1) / (8 * gob_h)} * block_size * gobs_x;
    if (tiled.size() < required) return false;
    for (std::uint32_t y = 0; y < height; ++y) {
        const std::size_t base_y = (y / (8 * gob_h)) * block_size * gobs_x + ((y / 8) % gob_h) * 512;
        const auto yy = y % 8;
        for (std::uint32_t x = 0; x < width * 16; x += 16) {
            const std::size_t offset = base_y + (x / 64) * block_size + ((x % 64) / 32) * 256 +
                                       (yy / 2) * 64 + (yy % 2) * 16 + ((x % 32) / 16) * 32;
            std::memcpy(tiled.data() + offset, linear.data() + std::size_t{y} * width * 16 + x, 16);
        }
    }
    return true;
}
} // namespace nxrt::render
