#include <render/guest_buffer_upload_cache.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

int main() {
    nxrt::render::GuestBufferUploadCache cache;
    std::array<std::uint8_t, 4> current{1, 2, 3, 4};
    cache.Store(0x123400, 16, current, 16);
    const auto reused = cache.Find(0x123400, 16, current, 64);
    if (!reused || *reused != 16) {
        std::cerr << "identical vertex bytes did not reuse the upload\n";
        return EXIT_FAILURE;
    }
    current[1] = 99;
    if (cache.Find(0x123400, 16, current, 64)) {
        std::cerr << "changed bytes at the same guest address reused stale vertices\n";
        return EXIT_FAILURE;
    }
    cache.Store(0x123400, 16, current, 32);
    if (cache.Find(0x123400, 16, current, 64) != 32) {
        std::cerr << "replacement bytes did not point at the newer upload\n";
        return EXIT_FAILURE;
    }
    current[1] = 2;
    if (cache.Find(0x123400, 16, current, 64)) {
        std::cerr << "replacement upload preserved stale guest bytes\n";
        return EXIT_FAILURE;
    }
    current[1] = 2;
    if (cache.Find(0x123400, 256, current, 64) ||
        cache.Find(0x123401, 16, current, 64) ||
        cache.Find(0x123400, 16, std::span(current).first(3), 64) ||
        cache.Find(0x123400, 16, current, 18)) {
        std::cerr << "different range, alignment, or truncated upload reused stale vertices\n";
        return EXIT_FAILURE;
    }
    cache.Store(0x123400, 16, current, std::numeric_limits<std::uint64_t>::max());
    if (cache.Find(0x123400, 16, current, 64)) {
        std::cerr << "overflowing upload offset was accepted\n";
        return EXIT_FAILURE;
    }
    cache.Store(0x123400, 16, current, 16);
    cache.Invalidate();
    if (cache.Find(0x123400, 16, current, 64)) {
        std::cerr << "invalidated upload remained reusable\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
