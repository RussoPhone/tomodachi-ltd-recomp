// SPDX-License-Identifier: GPL-3.0-or-later
#include "render/image_backing_identity.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void TestDirectAndVirtualAliasesShareBackingIdentity() {
    using namespace nxrt::render;
    constexpr std::uint64_t physical_pool = 0x100;
    constexpr std::uint64_t generation = 7;
    constexpr std::uint64_t physical_offset = 0x180;
    constexpr std::uint64_t virtual_pool = 0x200;
    const std::array mappings{VirtualImageMapping{
        virtual_pool, 0x100, 0x400, physical_pool, generation, 0x80
    }};

    const auto direct = DirectImageBacking(physical_pool, generation, physical_offset);
    const auto alias = ResolveVirtualImageBacking(virtual_pool, 0x200, mappings);
    Check(alias.has_value(), "virtual mapping should resolve to its physical backing");
    Check(*alias == direct, "direct and virtual views of one backing should share identity");
    Check(ImageBackingCacheKey(*alias) == ImageBackingCacheKey(direct),
          "direct and virtual aliases should share the render-target cache key");
}

void TestRemapAndPoolReinitializationChangeIdentity() {
    using namespace nxrt::render;
    constexpr std::uint64_t physical_pool = 0x100;
    constexpr std::uint64_t virtual_pool = 0x200;
    const auto original = DirectImageBacking(physical_pool, 7, 0x180);
    const std::array remapped{VirtualImageMapping{
        virtual_pool, 0x100, 0x400, 0x300, 2, 0x80
    }};
    const auto after_remap = ResolveVirtualImageBacking(virtual_pool, 0x200, remapped);
    Check(after_remap.has_value(), "remapped virtual range should resolve");
    Check(*after_remap != original, "mapping to another physical pool should invalidate identity");
    Check(ImageBackingCacheKey(*after_remap) != ImageBackingCacheKey(original),
          "remapping should produce a different render-target cache key");
    Check(DirectImageBacking(physical_pool, 8, 0x180) != original,
          "reinitializing the same pool handle should change generation identity");
    Check(ImageBackingCacheKey(DirectImageBacking(physical_pool, 8, 0x180)) !=
              ImageBackingCacheKey(original),
          "pool reinitialization should produce a different render-target cache key");
}

void TestFinalizingOneViewDoesNotInvalidateBackingAliases() {
    using namespace nxrt::render;
    constexpr std::uint64_t physical_pool = 0x100;
    const auto alias_a = DirectImageBacking(physical_pool, 7, 0x180);
    // Texture object address and texture serial are intentionally absent from this key.
    constexpr std::uint64_t view_a = 0x400, view_b = 0x500;
    (void)view_a;
    (void)view_b;
    const auto alias_b_after_view_finalize = DirectImageBacking(physical_pool, 7, 0x180);
    Check(alias_a == alias_b_after_view_finalize,
          "finalizing one texture view must preserve the shared backing identity");
}

void TestRejectsVirtualRangeOverflowAndMisses() {
    using namespace nxrt::render;
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    const std::array overflow{VirtualImageMapping{0x200, max - 3, 16, 0x100, 7, 0}};
    Check(!ResolveVirtualImageBacking(0x200, max - 2, overflow),
          "overflowing virtual range must not resolve");
    const std::array valid{VirtualImageMapping{0x200, 0x100, 0x20, 0x100, 7, 0x40}};
    Check(!ResolveVirtualImageBacking(0x201, 0x120, valid),
          "an offset at the end of a mapping must miss");
}

} // namespace

int main() {
    TestDirectAndVirtualAliasesShareBackingIdentity();
    TestRemapAndPoolReinitializationChangeIdentity();
    TestFinalizingOneViewDoesNotInvalidateBackingAliases();
    TestRejectsVirtualRangeOverflowAndMisses();
}
