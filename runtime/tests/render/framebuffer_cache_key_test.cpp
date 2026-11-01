#include "render/framebuffer_cache_key.h"

#include <array>
int main() {
    using nxrt::render::FramebufferCacheKeyUsesImageView;

    constexpr std::array<std::uint64_t, 4> key{0x10, 0x20, 0x30, 0xfeed};
    if (!FramebufferCacheKeyUsesImageView(key, 0x10)) return 1;
    if (!FramebufferCacheKeyUsesImageView(key, 0x20)) return 2;
    if (!FramebufferCacheKeyUsesImageView(key, 0x30)) return 3;
    if (FramebufferCacheKeyUsesImageView(key, 0xfeed)) return 4;
    if (FramebufferCacheKeyUsesImageView(key, 0x40)) return 5;
    if (FramebufferCacheKeyUsesImageView({}, 0x10)) return 6;
}
