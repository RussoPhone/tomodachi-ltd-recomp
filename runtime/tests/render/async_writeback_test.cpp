// NXRT_ASYNC_COMPUTE_FLUSH: ledger ordering, guest-access interception, slot ring and fallback.
#include "memory.h"
#include "render/async_writeback.h"

#include <array>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace nxrt;
using namespace nxrt::render;

namespace {

void Require(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
        std::abort();
    }
}
#define REQUIRE(x) Require((x), #x, __LINE__)

constexpr u64 kBase = 0x100000;

// A fake GPU: every slot has a fence that is either signalled or not; waiting signals it (the GPU "finishes").
struct Fake {
    GuestMemory memory;
    std::array<bool, 16> done{};
    std::vector<u32> waited;                  // wait order
    std::vector<u64> applied;                 // stream offsets in apply order
    std::vector<u64> diverged;                // ranges reported by VERIFY
    std::vector<u8> stream = std::vector<u8>(1 << 16, 0);
    bool renderer_thread = false;
    AsyncWritebackLedger* ledger = nullptr;

    AsyncWritebackHooks Hooks() {
        AsyncWritebackHooks h;
        h.wait_slot = [this](u32 slot) {
            waited.push_back(slot);
            done[slot] = true;
            return true;
        };
        h.slot_done = [this](u32 slot) { return static_cast<bool>(done[slot]); };
        h.apply = [this](const AsyncWriteback& w) {
            applied.push_back(w.stream_offset);
            std::memcpy(memory.RawPtr(w.range.address), stream.data() + w.stream_offset, w.range.size);
            return true;
        };
        h.hash_guest = [this](GuestMemoryRange r) {
            u64 hash = 1469598103934665603ull;
            for (u64 i = 0; i < r.size; ++i) hash = (hash ^ memory.RawPtr(r.address)[i]) * 1099511628211ull;
            return hash;
        };
        h.divert_page = [this](u64 page) { return memory.DivertPage(page); };
        h.restore_page = [this](u64 page) { memory.RestorePage(page); };
        h.in_renderer = [this] { return renderer_thread; };
        h.on_divergence = [this](const AsyncWriteback& w, u64) { diverged.push_back(w.range.address); };
        return h;
    }
    void Fill(u64 offset, u64 size, u8 value) { std::memset(stream.data() + offset, value, size); }
    u64 PageEntry(u64 page) const {
        u64 entry;
        std::memcpy(&entry, static_cast<const u8*>(memory.PageTable()) + (page << kPageEntryStrideLog2), sizeof(entry));
        return entry;
    }
};

AsyncWriteback Entry(u64 address, u64 size, u32 slot, u64 offset) {
    AsyncWriteback w;
    w.range = {address, size};
    w.slot = slot;
    w.stream_offset = offset;
    return w;
}

void Prepare(Fake& f) {
    REQUIRE(f.memory.Init());
    REQUIRE(f.memory.Map(kBase, 0x10000, MemState::Normal, PermRW));
}

// Overlapping writers materialize in dispatch order whichever of them the access hits.
void OrderedMaterialization() {
    Fake f;
    Prepare(f);
    AsyncWritebackLedger ledger(f.Hooks(), false);
    f.memory.SetAccessGuard(&ledger);
    f.Fill(0, 64, 0x11);
    f.Fill(64, 64, 0x22);
    f.Fill(128, 16, 0x33);
    const std::array first{Entry(kBase, 64, 0, 0), Entry(kBase + 0x2000, 16, 0, 128)};
    const std::array second{Entry(kBase, 64, 1, 64)};
    ledger.Add(first, false);
    ledger.Add(second, false);
    REQUIRE(ledger.Pending() == 3);
    // A load of the overlapped range materializes the older writer first, then the newer one.
    u8 byte = f.memory.Read<u8>(kBase + 8);
    REQUIRE(byte == 0x22);
    REQUIRE((f.applied == std::vector<u64>{0, 128, 64}));
    REQUIRE(ledger.Pending() == 0);
    REQUIRE((f.waited == std::vector<u32>{0, 0, 1}));
    f.memory.SetAccessGuard(nullptr);
}

// Only the prefix up to the last overlapping entry is copied; later, unrelated entries stay pending.
void PrefixOnly() {
    Fake f;
    Prepare(f);
    AsyncWritebackLedger ledger(f.Hooks(), false);
    f.memory.SetAccessGuard(&ledger);
    f.Fill(0, 16, 1);
    f.Fill(16, 16, 2);
    f.Fill(32, 16, 3);
    const std::array a{Entry(kBase, 16, 0, 0)}, b{Entry(kBase + 0x100, 16, 1, 16)}, c{Entry(kBase + 0x200, 16, 2, 32)};
    ledger.Add(a, false);
    ledger.Add(b, false);
    ledger.Add(c, false);
    u8 bytes[4];
    f.memory.ReadBlock(kBase + 0x104, bytes, 4);
    REQUIRE(bytes[0] == 2);
    REQUIRE((f.applied == std::vector<u64>{0, 16}));
    REQUIRE(ledger.Pending() == 1);
    REQUIRE(f.memory.Read<u8>(kBase + 0x200) == 3);
    REQUIRE(ledger.Pending() == 0);
    f.memory.SetAccessGuard(nullptr);
}

// A guest store on a pending range must not be overwritten by the older GPU result afterwards (WAW).
void StoreOnPendingRange() {
    Fake f;
    Prepare(f);
    AsyncWritebackLedger ledger(f.Hooks(), false);
    f.memory.SetAccessGuard(&ledger);
    f.Fill(0, 16, 0xAA);
    const std::array a{Entry(kBase + 0x40, 16, 0, 0)};
    ledger.Add(a, false);
    f.memory.Write<u32>(kBase + 0x44, 0x12345678);  // what HostStore does for a diverted page
    REQUIRE(ledger.Pending() == 0);                 // materialized before the store
    ledger.MaterializeAll(AsyncMaterializeCause::ExecuteEnd);
    REQUIRE(f.memory.Read<u32>(kBase + 0x44) == 0x12345678);
    REQUIRE(f.memory.Read<u8>(kBase + 0x40) == 0xAA);
    const AsyncWritebackStats st = ledger.Stats();
    REQUIRE(st.intercepted[static_cast<size_t>(AsyncMaterializeCause::GuestStore)] == 1);
    REQUIRE(st.intercepted[static_cast<size_t>(AsyncMaterializeCause::GuestLoad)] == 0);
    f.memory.SetAccessGuard(nullptr);
}

// Every other accessor goes through the same guard: Zero, WriteBlock, raw Ptr users, exclusive-style Ptr(va,size).
void AllAccessors() {
    Fake f;
    Prepare(f);
    AsyncWritebackLedger ledger(f.Hooks(), false);
    f.memory.SetAccessGuard(&ledger);
    const auto pend = [&](u64 at) {
        f.Fill(0, 16, 0x5A);
        const std::array a{Entry(at, 16, 0, 0)};
        ledger.Add(a, false);
        REQUIRE(ledger.Pending() == 1);
    };
    pend(kBase + 0x80);
    f.memory.Zero(kBase + 0x84, 4);
    REQUIRE(ledger.Pending() == 0);
    pend(kBase + 0x80);
    const u32 value = 7;
    f.memory.WriteBlock(kBase + 0x8C, &value, 4);
    REQUIRE(ledger.Pending() == 0);
    pend(kBase + 0x80);
    u8* p = f.memory.Ptr(kBase + 0x7C, 8);  // starts before the range, overlaps its first bytes
    REQUIRE(ledger.Pending() == 0);
    (void)p;
    pend(kBase + 0x80);
    (void)f.memory.ReadString(kBase + 0x70, 0x20);
    REQUIRE(ledger.Pending() == 0);
    pend(kBase + 0x80);
    f.memory.Read<u8>(kBase + 0x90);  // adjacent, not overlapping
    REQUIRE(ledger.Pending() == 1);
    ledger.MaterializeAll(AsyncMaterializeCause::Drain);
    f.memory.SetAccessGuard(nullptr);
}

// The AOT fast path is diverted to the slow path for pending pages and restored afterwards.
void PageDiversion() {
    Fake f;
    Prepare(f);
    const u64 fast = f.PageEntry(kBase >> kPageBits);
    REQUIRE(fast != 0 && fast != 1);
    AsyncWritebackLedger ledger(f.Hooks(), false);
    f.memory.SetAccessGuard(&ledger);
    f.Fill(0, 64, 9);
    // 0x1F00..0x2100 touches pages kBase+0x1000 and kBase+0x2000; a second entry shares the first page.
    const std::array a{Entry(kBase + 0x1F00, 0x200, 0, 0)};
    const std::array b{Entry(kBase + 0x1800, 16, 1, 0)};
    ledger.Add(a, false);
    ledger.Add(b, false);
    const u64 page1 = (kBase + 0x1000) >> kPageBits, page2 = (kBase + 0x2000) >> kPageBits;
    REQUIRE(f.PageEntry(page1) == 1 && f.PageEntry(page2) == 1);
    REQUIRE(f.PageEntry(kBase >> kPageBits) == fast);  // untouched page stays on the fast path
    // Materializing only `a` (prefix) must keep page1 slow because `b` still lives there.
    f.memory.Read<u8>(kBase + 0x2010);
    REQUIRE(ledger.Pending() == 1);
    REQUIRE(f.PageEntry(page1) == 1);
    REQUIRE(f.PageEntry(page2) == fast);
    ledger.MaterializeAll(AsyncMaterializeCause::Drain);
    REQUIRE(f.PageEntry(page1) == fast);
    // A page somebody else already put on the slow path is never flipped back.
    REQUIRE(f.memory.DivertPage(page2));
    const std::array c{Entry(kBase + 0x2000, 16, 2, 0)};
    ledger.Add(c, false);
    ledger.MaterializeAll(AsyncMaterializeCause::Drain);
    REQUIRE(f.PageEntry(page2) == 1);
    f.memory.SetAccessGuard(nullptr);
}

void PollCopiesOnlyTheSignalledPrefix() {
    Fake f;
    Prepare(f);
    AsyncWritebackLedger ledger(f.Hooks(), false);
    f.memory.SetAccessGuard(&ledger);
    f.Fill(0, 48, 4);
    const std::array a{Entry(kBase, 16, 0, 0)}, b{Entry(kBase + 0x100, 16, 1, 16)}, c{Entry(kBase + 0x200, 16, 2, 32)};
    ledger.Add(a, false);
    ledger.Add(b, false);
    ledger.Add(c, false);
    REQUIRE(ledger.Poll() == 0);
    f.done[0] = f.done[1] = true;
    f.done[2] = false;
    REQUIRE(ledger.Poll() == 2);
    REQUIRE(ledger.Pending() == 1);
    REQUIRE(f.waited.size() == 2);
    f.done[0] = f.done[1] = false;  // a later slot signalled alone must not be copied before an unsignalled earlier one
    f.done[2] = true;
    REQUIRE(ledger.Poll() == 1);
    REQUIRE(ledger.Stats().intercepted[static_cast<size_t>(AsyncMaterializeCause::Poll)] == 2);
    f.memory.SetAccessGuard(nullptr);
}

void ForwardAndSupersede() {
    Fake f;
    Prepare(f);
    AsyncWritebackLedger ledger(f.Hooks(), false);
    f.memory.SetAccessGuard(&ledger);
    f.Fill(0, 64, 1);
    const std::array a{Entry(kBase + 0x40, 32, 0, 256)};
    ledger.Add(a, false);
    REQUIRE(ledger.FindForward({kBase + 0x40, 32}, 256) == 256);
    REQUIRE(!ledger.FindForward({kBase + 0x40, 32}, 512).has_value());  // stream offset not aligned for the binding
    REQUIRE(!ledger.FindForward({kBase + 0x40, 16}, 1).has_value());    // partial
    REQUIRE(!ledger.FindForward({kBase + 0x30, 32}, 1).has_value());    // shifted
    REQUIRE(!ledger.FindForward({kBase + 0x80, 32}, 1).has_value());    // not pending
    // A newer partial writer hides the exact one: the older alias would be stale.
    const std::array partial{Entry(kBase + 0x48, 8, 1, 512)};
    ledger.Add(partial, false);
    REQUIRE(!ledger.FindForward({kBase + 0x40, 32}, 1).has_value());
    ledger.MaterializeAll(AsyncMaterializeCause::Drain);
    f.applied.clear();
    // In-place update of the same stream range: the older entry is dropped without a copy.
    const std::array first{Entry(kBase + 0x40, 32, 0, 256)};
    ledger.Add(first, false);
    const std::array second{Entry(kBase + 0x40, 32, 1, 256)};
    ledger.Add(second, true);
    REQUIRE(ledger.Pending() == 1);
    ledger.MaterializeAll(AsyncMaterializeCause::ExecuteEnd);
    REQUIRE((f.applied == std::vector<u64>{256}));
    REQUIRE(ledger.Stats().entries_superseded == 1);
    f.memory.SetAccessGuard(nullptr);
}

void RemapDropsPending() {
    Fake f;
    Prepare(f);
    AsyncWritebackLedger ledger(f.Hooks(), false);
    f.memory.SetAccessGuard(&ledger);
    const u64 fast = f.PageEntry(kBase >> kPageBits);
    const std::array a{Entry(kBase, 16, 0, 0)}, b{Entry(kBase + 0x4000, 16, 0, 16)};
    ledger.Add(a, false);
    ledger.Add(b, false);
    f.memory.Unmap(kBase, 0x1000);
    REQUIRE(ledger.Pending() == 1);
    REQUIRE(f.PageEntry(kBase >> kPageBits) == 0);
    REQUIRE(ledger.Stats().entries_dropped == 1);
    ledger.MaterializeAll(AsyncMaterializeCause::Drain);
    REQUIRE(f.PageEntry((kBase + 0x4000) >> kPageBits) == fast);
    f.memory.SetAccessGuard(nullptr);
}

// VERIFY: a change that bypasses the guard is reported at materialization; guarded changes and ordered overlapping
// copies are not.
void VerifyDetectsUnguardedChange() {
    Fake f;
    Prepare(f);
    AsyncWritebackLedger ledger(f.Hooks(), true);
    f.memory.SetAccessGuard(&ledger);
    f.Fill(0, 16, 6);
    f.Fill(16, 16, 7);
    const std::array a{Entry(kBase + 0x40, 16, 0, 0)}, b{Entry(kBase + 0x40, 16, 1, 16)};
    ledger.Add(a, false);
    ledger.Add(b, false);
    ledger.MaterializeAll(AsyncMaterializeCause::Drain);  // a is copied, then b's baseline is rebased onto it
    REQUIRE(ledger.Stats().verify_divergences == 0);
    REQUIRE(ledger.Stats().verify_checked == 2);
    const std::array c{Entry(kBase + 0x80, 16, 2, 0)};
    ledger.Add(c, false);
    f.memory.RawPtr(kBase + 0x84)[0] ^= 0xFF;  // a store that skipped the interception
    ledger.MaterializeAll(AsyncMaterializeCause::ExecuteEnd);
    REQUIRE(ledger.Stats().verify_divergences == 1);
    REQUIRE((f.diverged == std::vector<u64>{kBase + 0x80}));
    const std::array d{Entry(kBase + 0xC0, 16, 3, 0)};
    ledger.Add(d, false);
    f.memory.Write<u8>(kBase + 0xC4, 1);  // guarded: materializes first, no divergence
    REQUIRE(ledger.Stats().verify_divergences == 1);
    f.memory.SetAccessGuard(nullptr);
}

void CausesAreClassified() {
    Fake f;
    Prepare(f);
    AsyncWritebackLedger ledger(f.Hooks(), false);
    f.memory.SetAccessGuard(&ledger);
    const std::array a{Entry(kBase, 16, 0, 0)};
    ledger.Add(a, false);
    f.renderer_thread = true;
    f.memory.Read<u8>(kBase);
    f.renderer_thread = false;
    ledger.Add(a, false);
    f.memory.Read<u8>(kBase);
    const AsyncWritebackStats st = ledger.Stats();
    REQUIRE(st.intercepted[static_cast<size_t>(AsyncMaterializeCause::Renderer)] == 1);
    REQUIRE(st.intercepted[static_cast<size_t>(AsyncMaterializeCause::GuestLoad)] == 1);
    f.memory.SetAccessGuard(nullptr);
}

// Ring: slots are consumed in order and recycled only together; the last slot is kept for the synchronous flush,
// so exhaustion makes the caller fall back (and the fallback recycles everything).
void RingRecyclesByDrainAndFallsBack() {
    AsyncSlotRing ring(10);
    size_t submits = 0, fallbacks = 0;
    for (int dispatch = 0; dispatch < 30; ++dispatch) {
        if (ring.CanSubmitAsync()) {
            REQUIRE(ring.Current() == ring.InFlight());
            ring.MarkSubmitted();
            ++submits;
        } else {
            ++fallbacks;      // synchronous Flush: waits every fence of the ring, then resets it
            ring.RecycleAll();
        }
    }
    REQUIRE(submits == 27 && fallbacks == 3);  // 9 async submits, then one fallback, repeated
    REQUIRE(ring.InFlight() <= ring.Slots() - 2);
    AsyncSlotRing small(2);
    REQUIRE(small.CanSubmitAsync());   // one async submit, then the second slot is reserved for the synchronous flush
    small.MarkSubmitted();
    REQUIRE(!small.CanSubmitAsync());
    AsyncSlotRing one(1);
    REQUIRE(!one.CanSubmitAsync());
    AsyncSlotRing three(3);
    REQUIRE(three.CanSubmitAsync());
    three.MarkSubmitted();
    REQUIRE(three.CanSubmitAsync());
    three.MarkSubmitted();
    REQUIRE(!three.CanSubmitAsync());
    three.RecycleAll();
    REQUIRE(three.CanSubmitAsync() && three.InFlight() == 0);
}


void ProfileIsOptInAndPreservesHooks() {
    for (bool enabled : {false, true}) {
        Fake f;
        Prepare(f);
        unsigned classifications = 0;
        auto hooks = f.Hooks();
        hooks.in_renderer = [&] { ++classifications; return false; };
        AsyncWritebackLedger ledger(std::move(hooks), false, enabled);
        f.memory.SetAccessGuard(&ledger);
        f.Fill(0, 16, 0x42);
        f.Fill(16, 32, 0x51);
        const std::array entries{Entry(kBase, 16, 0, 0), Entry(kBase + 0x2000, 32, 1, 16)};
        ledger.Add(entries, false);
        // Non-overlap must not call classification; its lock timing is counted separately.
        ledger.BeforeAccess(kBase + 0x100, 1, false);
        REQUIRE(classifications == 0);
        const auto no_overlap = ledger.Stats().non_overlap;
        REQUIRE(no_overlap.lock_calls == (enabled ? 1u : 0u));
        REQUIRE(no_overlap.pending_page_calls == (enabled ? 1u : 0u));
        if (!enabled) REQUIRE(no_overlap.lock_wait_ns == 0);
        REQUIRE(f.memory.Read<u8>(kBase) == 0x42);
        REQUIRE(classifications == 1);
        f.done[1] = true;
        REQUIRE(ledger.Poll() == 1);
        REQUIRE((f.applied == std::vector<u64>{0, 16}));
        const auto stats = ledger.Stats();
        const auto& guest = stats.profile[static_cast<size_t>(AsyncMaterializeCause::GuestLoad)];
        const auto& poll = stats.profile[static_cast<size_t>(AsyncMaterializeCause::Poll)];
        REQUIRE(guest.lock_calls == (enabled ? 1u : 0u));
        REQUIRE(guest.wait_calls == (enabled ? 1u : 0u));
        REQUIRE(guest.apply_calls == (enabled ? 1u : 0u));
        REQUIRE(guest.applied_bytes == (enabled ? 16u : 0u));
        REQUIRE(poll.lock_calls == (enabled ? 1u : 0u));
        REQUIRE(poll.wait_calls == (enabled ? 1u : 0u));
        REQUIRE(poll.apply_calls == (enabled ? 1u : 0u));
        REQUIRE(poll.applied_bytes == (enabled ? 32u : 0u));
        if (!enabled) {
            REQUIRE(guest.lock_wait_ns == 0 && guest.wait_ns == 0 && guest.apply_ns == 0);
            REQUIRE(poll.lock_wait_ns == 0 && poll.wait_ns == 0 && poll.apply_ns == 0);
        }
        f.memory.SetAccessGuard(nullptr);
    }
}


void ProfileRecordsFailureWithoutChangingBehavior() {
    {
        Fake f;
        Prepare(f);
        auto hooks = f.Hooks();
        unsigned attempts = 0, applies = 0;
        hooks.wait_slot = [&](u32) { ++attempts; return false; };
        hooks.apply = [&](const AsyncWriteback&) { ++applies; return true; };
        AsyncWritebackLedger ledger(std::move(hooks), false, true);
        const std::array entries{Entry(kBase, 16, 0, 0)};
        ledger.Add(entries, false);
        ledger.MaterializeAll(AsyncMaterializeCause::ExecuteEnd);
        const auto stats = ledger.Stats();
        const auto& p = stats.profile[static_cast<size_t>(AsyncMaterializeCause::ExecuteEnd)];
        REQUIRE(attempts == 1 && applies == 0 && ledger.Pending() == 0);
        REQUIRE(stats.wait_failures == 1 && stats.entries_dropped == 1);
        REQUIRE(p.lock_calls == 1 && p.wait_calls == 1 && p.apply_calls == 0 && p.applied_bytes == 0);
    }
    {
        Fake f;
        Prepare(f);
        auto hooks = f.Hooks();
        unsigned applies = 0;
        hooks.apply = [&](const AsyncWriteback&) { ++applies; return false; };
        AsyncWritebackLedger ledger(std::move(hooks), false, true);
        const std::array entries{Entry(kBase, 16, 0, 0)};
        ledger.Add(entries, false);
        ledger.MaterializeAll(AsyncMaterializeCause::Drain);
        const auto stats = ledger.Stats();
        const auto& p = stats.profile[static_cast<size_t>(AsyncMaterializeCause::Drain)];
        REQUIRE(applies == 1 && ledger.Pending() == 0 && stats.apply_skipped == 1);
        REQUIRE(p.wait_calls == 1 && p.apply_calls == 1 && p.applied_bytes == 0);
    }
}


// Synchronize the two callers while the first materialization holds the ledger lock.
// This tests order and attribution without a wall-clock lower bound. The second caller
// announces that it is about to call BeforeAccess, not that it has entered mutex::lock.
void ProfileConcurrentAccess() {
    Fake f;
    Prepare(f);
    std::mutex sync;
    std::condition_variable cv;
    bool first_inside_wait = false, second_about_to_enter = false, release_first = false;
    auto hooks = f.Hooks();
    auto wait_slot = hooks.wait_slot;
    hooks.wait_slot = [&](u32 slot) {
        if (slot == 0) {
            std::unique_lock lock(sync);
            first_inside_wait = true;
            cv.notify_all();
            cv.wait(lock, [&] { return release_first; });
        }
        return wait_slot(slot);
    };
    AsyncWritebackLedger ledger(std::move(hooks), false, true);
    const std::array entries{Entry(kBase, 16, 0, 0), Entry(kBase + 0x2000, 32, 1, 16)};
    ledger.Add(entries, false);
    std::thread first([&] { ledger.BeforeAccess(kBase, 1, false); });
    {
        std::unique_lock lock(sync);
        cv.wait(lock, [&] { return first_inside_wait; });
    }
    std::thread second([&] {
        {
            std::lock_guard lock(sync);
            second_about_to_enter = true;
            cv.notify_all();
        }
        ledger.BeforeAccess(kBase + 0x2000, 1, false);
    });
    {
        std::unique_lock lock(sync);
        cv.wait(lock, [&] { return second_about_to_enter; });
        release_first = true;
        cv.notify_all();
    }
    first.join();
    second.join();
    REQUIRE((f.applied == std::vector<u64>{0, 16}));
    const auto stats = ledger.Stats();
    const auto& guest = stats.profile[static_cast<size_t>(AsyncMaterializeCause::GuestLoad)];
    REQUIRE(guest.lock_calls == 2 && guest.wait_calls == 2 && guest.apply_calls == 2);
    REQUIRE(guest.applied_bytes == 48 && ledger.Pending() == 0);
}

}  // namespace

int main() {
    ProfileIsOptInAndPreservesHooks();
    ProfileRecordsFailureWithoutChangingBehavior();
    ProfileConcurrentAccess();
    OrderedMaterialization();
    PrefixOnly();
    StoreOnPendingRange();
    AllAccessors();
    PageDiversion();
    PollCopiesOnlyTheSignalledPrefix();
    ForwardAndSupersede();
    RemapDropsPending();
    VerifyDetectsUnguardedChange();
    CausesAreClassified();
    RingRecyclesByDrainAndFallsBack();
    return EXIT_SUCCESS;
}
