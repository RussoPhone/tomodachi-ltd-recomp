// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <optional>
#include <limits>
#include "nvn/model.h"
namespace nxrt::nvn {
inline bool SameTextureStorageLayout(const TextureDesc& a, const TextureDesc& b) {
    // IMAGE (0x4) is an access usage; retain every flag that affects storage.
    return (a.flags & ~4u) == (b.flags & ~4u) && a.target == b.target && a.width == b.width && a.height == b.height &&
           a.depth == b.depth && a.levels == b.levels && a.format == b.format && a.samples == b.samples &&
           a.stride == b.stride;
}
struct GuestTextureCopy { u64 source, destination, size; };
inline std::optional<GuestTextureCopy> PlanGuestTextureCopy(const Model& model, const Command& c) {
    if (c.op != Op::CopyTexture || c.words.size() != 22 || c.a[2] || (!c.a[3] && !c.a[4]) ||
        (c.a[3] && c.a[4] && c.a[3] != c.a[4]))
        return std::nullopt;
    // Explicit packaged tiling is part of the layout, even at equal byte sizes.
    // Mixed packaged/default layouts need a conversion and stay unsupported.
    if (c.words[16] != c.words[19] || c.words[16] > 1 ||
        (c.words[16] && (c.words[17] != c.words[20] || c.words[18] != c.words[21]))) return std::nullopt;
    const u64 bytes = c.a[3] ? c.a[3] : c.a[4];
    const auto source = model.textures.find(c.a[0]), destination = model.textures.find(c.a[1]);
    if (source == model.textures.end() || destination == model.textures.end()) return std::nullopt;
    const auto& s = source->second.desc;
    const auto& d = destination->second.desc;
    if (!SameTextureStorageLayout(s, d) || s.target != 1 || !s.width || !s.height ||
        s.depth > 1 || s.levels > 1 || s.samples > 1)
        return std::nullopt;
    for (u32 index : {0u, 6u}) {
        const auto* r = c.words.data() + index;
        if (r[0] || r[1] || r[2] || r[3] != s.width || r[4] != s.height || r[5] != 1) return std::nullopt;
    }
    for (u32 index = 12; index < 16; ++index) if (c.words[index]) return std::nullopt;
    const auto address = [&](const TextureDesc& desc) -> std::optional<u64> {
        const auto it = model.pools.find(desc.pool);
        if (it == model.pools.end()) return std::nullopt;
        const auto& p = it->second;
        if (p.is_virtual || !p.storage || desc.offset > p.size || bytes > p.size - desc.offset ||
            p.storage > std::numeric_limits<u64>::max() - desc.offset ||
            p.storage + desc.offset > std::numeric_limits<u64>::max() - bytes) return std::nullopt;
        return p.storage + desc.offset;
    };
    const auto src = address(s), dst = address(d);
    if (!src || !dst) return std::nullopt;
    return GuestTextureCopy{*src, *dst, bytes};
}
}
