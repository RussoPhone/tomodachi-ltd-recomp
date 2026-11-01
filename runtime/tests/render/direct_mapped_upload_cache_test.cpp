#include <render/guest_buffer_upload_cache.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #c);   \
            std::abort();                                                 \
        }                                                                 \
    } while (0)

using namespace nxrt::render;

int main() {
    DirectMappedUploadCache c(16);
    std::vector<std::uint8_t> a(64, 1), b(64, 2);
    CHECK(!c.Find(0x1000, a, 1 << 20));
    c.Store(0x1000, a, 128);
    CHECK(c.Find(0x1000, a, 1 << 20) == 128);
    // Same address, different bytes: miss, and the new upload replaces the entry.
    CHECK(!c.Find(0x1000, b, 1 << 20));
    c.Store(0x1000, b, 256);
    CHECK(c.Find(0x1000, b, 1 << 20) == 256);
    CHECK(!c.Find(0x1000, a, 1 << 20));
    // A stored range must lie inside what the stream has already received.
    CHECK(!c.Find(0x1000, b, 256 + 63));
    CHECK(c.Find(0x1000, b, 256 + 64) == 256);
    // Different size is a different entry.
    std::vector<std::uint8_t> shorter(32, 2);
    CHECK(!c.Find(0x1000, shorter, 1 << 20));
    // Many colliding addresses never produce a false hit.
    for (std::uint64_t i = 0; i < 200; ++i) {
        std::vector<std::uint8_t> d(48, static_cast<std::uint8_t>(i));
        c.Store(0x2000 + i * 0x40, d, 1000 + i * 48);
    }
    for (std::uint64_t i = 0; i < 200; ++i) {
        std::vector<std::uint8_t> d(48, static_cast<std::uint8_t>(i));
        std::vector<std::uint8_t> other(48, static_cast<std::uint8_t>(i + 1));
        if (auto h = c.Find(0x2000 + i * 0x40, d, 1 << 20)) CHECK(*h == 1000 + i * 48);
        CHECK(!c.Find(0x2000 + i * 0x40, other, 1 << 20));
    }
    c.Invalidate();
    CHECK(!c.Find(0x1000, b, 1 << 20));
    std::puts("direct mapped upload cache ok");
    return 0;
}
