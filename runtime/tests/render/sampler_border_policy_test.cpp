// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <cassert>
#include <cstdint>
#include "render/sampler_border_policy.h"

int main() {
    std::array<std::uint32_t, 8> tsc{};
    tsc[0] = 1u << 9;
    tsc[4] = tsc[5] = tsc[6] = tsc[7] = 0x3f800000u;
    assert(nxrt::render::UseWhiteDepthBorder(tsc.data(), true, false, true));
    assert(!nxrt::render::UseWhiteDepthBorder(tsc.data(), false, false, true));
    assert(!nxrt::render::UseWhiteDepthBorder(tsc.data(), true, true, true));
    assert(!nxrt::render::UseWhiteDepthBorder(tsc.data(), true, false, false));
    tsc[0] = 0;
    assert(!nxrt::render::UseWhiteDepthBorder(tsc.data(), true, false, true));
    tsc[0] = 1u << 9;
    for (unsigned i = 4; i < 8; ++i) {
        for (const std::uint32_t v : {0u, 0x7fc00000u, 0x3f000000u}) {
            tsc[i] = v;
            assert(!nxrt::render::UseWhiteDepthBorder(tsc.data(), true, false, true));
        }
        tsc[i] = 0x3f800000u;
    }
}
