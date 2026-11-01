#include <render/soft_dirty_backend.h>
#include <render/texture_dirty_tracker.h>

#include <cstdio>
#include <cstdlib>
#include <set>
#include <sys/mman.h>
#include <unistd.h>

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #c);   \
            std::abort();                                                 \
        }                                                                 \
    } while (0)

using namespace nxrt::render;

namespace {

// Backend whose "pages" are written by the test.
struct FakeBackend final : DirtyBackend {
    bool clear_ok = true, query_ok = true;
    bool dirty_flag = false;
    int clears = 0;
    bool ClearAll() override {
        if (!clear_ok) return false;
        ++clears;
        dirty_flag = false;
        return true;
    }
    bool RangeDirty(const void*, size_t, bool* dirty) override {
        if (!query_ok) return false;
        *dirty = dirty_flag;
        return true;
    }
};

void LogicTests() {
    FakeBackend be;
    TextureDirtyTracker t(&be, 4);
    char buf[16];
    DirtyStamp s;

    // Before the first BeginFrame there is no epoch: nothing may be skipped.
    CHECK(!t.CanSkip(s, buf, 16, 1, true));
    t.BeginFrame(10);
    CHECK(be.clears == 1 && t.epoch() == 1);
    // Never verified -> hash.
    CHECK(!t.CanSkip(s, buf, 16, 1, true));
    t.MarkVerified(s, 1, true);
    // Verified in this epoch and clean -> skip.
    CHECK(t.CanSkip(s, buf, 16, 1, true));
    // A write after verification forbids the skip, and stays forbidden until a new verification+clear.
    be.dirty_flag = true;
    CHECK(!t.CanSkip(s, buf, 16, 1, true));
    // Layout change (Map/Unmap) invalidates the stamp even when the pages look clean.
    be.dirty_flag = false;
    CHECK(t.CanSkip(s, buf, 16, 1, true));
    CHECK(!t.CanSkip(s, buf, 16, 2, true));
    // Shared-object ranges are never trusted, even when stamped.
    CHECK(!t.CanSkip(s, buf, 16, 1, false));
    DirtyStamp shared;
    t.MarkVerified(shared, 1, false);
    CHECK(shared.epoch == 0);
    // Same frame / frames inside the interval do not clear again.
    t.BeginFrame(10);
    t.BeginFrame(13);
    CHECK(be.clears == 1);
    // A clear starts a new epoch and invalidates every stamp taken before it: the evidence of writes made
    // between a texture's last check and the clear is gone, so the texture is hashed once again.
    t.BeginFrame(14);
    CHECK(be.clears == 2 && t.epoch() == 2);
    CHECK(!t.CanSkip(s, buf, 16, 1, true));
    t.MarkVerified(s, 1, true);
    CHECK(t.CanSkip(s, buf, 16, 1, true));
    // Backend that cannot answer: hash.
    be.query_ok = false;
    CHECK(!t.CanSkip(s, buf, 16, 1, true));
    be.query_ok = true;
    // Empty range is never skippable.
    CHECK(!t.CanSkip(s, buf, 0, 1, true));
    // Frame counter going backwards forces a clear instead of trusting an old epoch.
    t.BeginFrame(2);
    CHECK(be.clears == 3);

    const auto& st = t.stats();
    CHECK(st.skipped == 3 && st.dirty == 1 && st.untrackable >= 2 && st.clears == 3);

    // A failing clear disables the tracker for good: never skip.
    FakeBackend bad;
    bad.clear_ok = false;
    TextureDirtyTracker u(&bad, 1);
    u.BeginFrame(1);
    CHECK(!u.enabled());
    DirtyStamp v;
    u.MarkVerified(v, 1, true);
    CHECK(!u.CanSkip(v, buf, 16, 1, true));
    // No backend at all.
    TextureDirtyTracker none(nullptr, 1);
    none.BeginFrame(1);
    CHECK(!none.enabled());
}

void SoftDirtyTests() {
    SoftDirtyBackend be;
    if (!be.Init()) {
        std::printf("soft-dirty unavailable on this kernel: real-backend tests skipped\n");
        return;
    }
    const size_t ps = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    char* mem = static_cast<char*>(
        mmap(nullptr, ps * 16, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0));
    CHECK(mem != MAP_FAILED);
    // The guest reserves PROT_NONE and mprotect()s on Map(): do the same.
    CHECK(mprotect(mem, ps * 16, PROT_READ | PROT_WRITE) == 0);
    for (size_t i = 0; i < 8; ++i) mem[i * ps] = 1;

    TextureDirtyTracker t(&be, 100);
    t.BeginFrame(1);
    DirtyStamp tex;           // pages 2..5
    char* range = mem + ps * 2;
    const size_t bytes = ps * 4;
    CHECK(!t.CanSkip(tex, range, bytes, 7, true));
    t.MarkVerified(tex, 7, true);
    CHECK(t.CanSkip(tex, range, bytes, 7, true));
    mem[ps * 1 + 5] = 9;      // neighbour below
    mem[ps * 6] = 9;          // neighbour above
    CHECK(t.CanSkip(tex, range, bytes, 7, true));
    mem[ps * 3 + 100] = 9;    // user store inside
    CHECK(!t.CanSkip(tex, range, bytes, 7, true));

    // Last byte of the range, a page that was never touched, and a kernel write.
    t.BeginFrame(1000);
    CHECK(!t.CanSkip(tex, range, bytes, 7, true));  // new epoch
    t.MarkVerified(tex, 7, true);
    CHECK(t.CanSkip(tex, range, bytes, 7, true));
    mem[ps * 6 - 1] = 1;
    CHECK(!t.CanSkip(tex, range, bytes, 7, true));
    t.BeginFrame(2000);
    t.MarkVerified(tex, 7, true);
    DirtyStamp fresh;
    t.MarkVerified(fresh, 7, true);
    CHECK(t.CanSkip(fresh, mem + ps * 10, ps * 2, 7, true));
    mem[ps * 11] = 1;         // page 11 was never populated before
    CHECK(!t.CanSkip(fresh, mem + ps * 10, ps * 2, 7, true));
    t.BeginFrame(3000);
    t.MarkVerified(fresh, 7, true);
    CHECK(t.CanSkip(fresh, range, bytes, 7, true));
    int fds[2];
    CHECK(pipe(fds) == 0);
    CHECK(write(fds[1], "abcd", 4) == 4);
    CHECK(read(fds[0], range + ps + 3, 4) == 4);  // the kernel writes into the range
    CHECK(!t.CanSkip(fresh, range, bytes, 7, true));
    close(fds[0]);
    close(fds[1]);
    munmap(mem, ps * 16);
}

}  // namespace

int main() {
    LogicTests();
    SoftDirtyTests();
    std::printf("texture_dirty_tracker tests passed\n");
    return 0;
}
