// SPDX-License-Identifier: GPL-3.0-or-later
// A sampled texture recreated at the same guest object address can reuse its TIC.
// Its cache identity must still change with the recorder's lifecycle serial.
#include <array>
#include <cassert>
#include <cstdint>

#include "render/texture_cache_key.h"

int main() {
    constexpr std::uint64_t descriptor_hash = 0x123456789abcdef0ULL;
    constexpr std::uint64_t texture_object = 0x80001234ULL;
    const auto original = nxrt::render::TextureCacheKey(descriptor_hash, texture_object, 17);
    assert(original == nxrt::render::TextureCacheKey(descriptor_hash, texture_object, 17));
    assert(original != nxrt::render::TextureCacheKey(descriptor_hash, texture_object, 18));
    assert(original != nxrt::render::TextureCacheKey(descriptor_hash, texture_object + 8, 17));
}
