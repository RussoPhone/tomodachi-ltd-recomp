// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nxrt::render {

// The TIC describes texture storage and sampling state, but is unchanged when
// NVN finalizes and reinitializes the same guest texture object in place.
inline std::uint64_t TextureCacheKey(std::uint64_t descriptor_hash, std::uint64_t texture_object,
                                     std::uint64_t texture_serial) {
    const std::array<std::uint64_t, 2> lifecycle{texture_object, texture_serial};
    std::uint64_t hash = descriptor_hash;
    for (std::size_t i = 0; i < sizeof(lifecycle); ++i) {
        hash = (hash ^ reinterpret_cast<const std::uint8_t*>(lifecycle.data())[i]) * 0x100000001b3ULL;
    }
    return hash;
}

} // namespace nxrt::render
