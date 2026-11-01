// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <array>
#include <bit>
#include <cassert>
#include <cstdint>
#include <type_traits>
#include <vulkan/vulkan.h>

#include "render/graphics_state_cache.h"

template <typename Handle>
Handle FakeHandle(std::uintptr_t value) {
    if constexpr (std::is_pointer_v<Handle>) return reinterpret_cast<Handle>(value);
    else return static_cast<Handle>(value);
}

int main() {
    using nxrt::render::GraphicsStateCache;
    const VkPipeline a = FakeHandle<VkPipeline>(1);
    const VkPipeline b = FakeHandle<VkPipeline>(2);
    VkViewport viewport{0, 0, 1920, -1080, 0, 1};
    VkRect2D scissor{{0, 0}, {1920, 1080}};
    std::array<float, 4> blend{0, 0, 0, 1};
    GraphicsStateCache cache;

    assert(cache.Pipeline(a));
    assert(!cache.Pipeline(a));
    assert(cache.Viewport(viewport));
    assert(!cache.Viewport(viewport));
    assert(cache.Scissor(scissor));
    assert(!cache.Scissor(scissor));
    assert(cache.Blend(blend));
    assert(!cache.Blend(blend));

    viewport.width = 1280;
    assert(cache.Viewport(viewport));
    assert(!cache.Scissor(scissor));
    scissor.extent.width = 1280;
    assert(cache.Scissor(scissor));
    blend[0] = -0.0f; // Compare representation, including signed zero.
    assert(cache.Blend(blend));
    blend[1] = std::bit_cast<float>(std::uint32_t{0x7fc00001});
    assert(cache.Blend(blend));
    assert(!cache.Blend(blend)); // Equal NaN bits are unchanged.
    blend[1] = std::bit_cast<float>(std::uint32_t{0x7fc00002});
    assert(cache.Blend(blend));

    assert(cache.Pipeline(b));
    assert(cache.Viewport(viewport)); // Pipeline changes invalidate dynamics.
    assert(cache.Scissor(scissor));
    assert(cache.Blend(blend));
    cache.Reset(); // New command-buffer recording or render-pass boundary.
    assert(cache.Pipeline(b));
    assert(cache.Viewport(viewport));
    assert(cache.Scissor(scissor));
    assert(cache.Blend(blend));
}
