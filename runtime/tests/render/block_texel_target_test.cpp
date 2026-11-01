// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <vector>
#include "render/block_texel_target.h"
#include "render/maxwell.h"

int main() {
    using namespace nxrt::render;
    auto plan = PlanBc3Target(256, 256, 1, 0, true, true);
    assert(plan && plan->width == 64 && plan->height == 64);
    plan = PlanBc3Target(2, 1, 1, 0, true, true);
    assert(plan && plan->width == 1 && plan->height == 1);
    assert(!PlanBc3Target(256, 256, 2, 0, true, true));
    assert(!PlanBc3Target(256, 256, 1, 4, true, true));
    assert(!PlanBc3Target(256, 256, 1, 0, false, true));
    assert(!PlanBc3Target(256, 256, 1, 0, true, false));
    for (auto [width, height] : {BlockTexelExtent{1, 1}, {2, 2}, {7, 7}, {16, 16}, {64, 64},
                                {7, 31}, {65, 9}, {1, 64}}) {
        for (unsigned bh : {0u, 1u, 2u, 3u}) {
          for (unsigned tw : {0u, 1u, 2u}) {
            std::vector<std::uint8_t> input(width * height * 16);
            // Arbitrary integer words must survive without float conversion.
            for (std::size_t i = 0; i < input.size(); ++i) input[i] = (i * 73 + i / 4 + 0x7f) & 255;
            const std::uint32_t words[]{0x7fc00001u, 0xff800001u, 0xffffffffu, 0x80000000u};
            std::memcpy(input.data(), words, sizeof(words));
            std::vector<std::uint8_t> tiled(BlockLinearSize(16, width, height, bh, tw), 0xcc), output(input.size());
            assert(StoreBlockLinear128(input, tiled, width, height, bh, tw));
            UnswizzleBlockLinear(tiled.data(), tiled.size(), output.data(), 16, width, height, bh, tw);
            assert(input == output);
            tiled.resize(tiled.size() - 1);
            assert(!StoreBlockLinear128(input, tiled, width, height, bh, tw));
          }
        }
    }
}
