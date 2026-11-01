// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace nxrt::render {

struct ImageBackingIdentity {
    std::uint64_t physical_pool;
    std::uint64_t generation;
    std::uint64_t offset;

    friend bool operator==(const ImageBackingIdentity&, const ImageBackingIdentity&) = default;
};

struct VirtualImageMapping {
    std::uint64_t virtual_pool;
    std::uint64_t virtual_offset;
    std::uint64_t size;
    std::uint64_t physical_pool;
    std::uint64_t physical_generation;
    std::uint64_t physical_offset;
};

inline ImageBackingIdentity DirectImageBacking(std::uint64_t physical_pool,
                                               std::uint64_t generation,
                                               std::uint64_t offset) {
    return {physical_pool, generation, offset};
}

inline std::optional<ImageBackingIdentity> ResolveVirtualImageBacking(
    std::uint64_t virtual_pool, std::uint64_t offset, std::span<const VirtualImageMapping> mappings) {
    for (const VirtualImageMapping& mapping : mappings) {
        if (mapping.virtual_pool != virtual_pool || mapping.size == 0 || mapping.physical_pool == 0 ||
            mapping.physical_generation == 0 ||
            mapping.virtual_offset > std::numeric_limits<std::uint64_t>::max() - mapping.size ||
            mapping.physical_offset > std::numeric_limits<std::uint64_t>::max() - mapping.size ||
            offset < mapping.virtual_offset || offset - mapping.virtual_offset >= mapping.size) {
            continue;
        }
        const std::uint64_t delta = offset - mapping.virtual_offset;
        return DirectImageBacking(mapping.physical_pool, mapping.physical_generation,
                                  mapping.physical_offset + delta);
    }
    return std::nullopt;
}

inline std::uint64_t ImageBackingCacheKey(const ImageBackingIdentity& identity) {
    const std::array<std::uint64_t, 3> fields{
        identity.physical_pool, identity.generation, identity.offset
    };
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (std::size_t i = 0; i < sizeof(fields); ++i) {
        hash = (hash ^ reinterpret_cast<const std::uint8_t*>(fields.data())[i]) * 0x100000001b3ULL;
    }
    return hash;
}

} // namespace nxrt::render
