// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include "memory.h"
#include "guest_clock.h"

#include <chrono>
#include <ctime>
#include <vector>

#ifdef _WIN32
#include "platform/win32_memory.h"
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace nxrt {

GuestMemory g_memory;
// Diagnostic counters: guest accesses that missed the page-table fast path (NXRT_TRACE_COMPUTE_FLUSH).
std::atomic<u64> g_slow_path_loads{0}, g_slow_path_stores{0};
void (*g_fault_hook)() = nullptr;

#ifdef _WIN32
namespace {
win32::HostSpace g_host_space;
win32::LazyTable g_host_table;
} // namespace
#endif

namespace {

constexpr u64 kPageEntrySize = u64{1} << kPageEntryStrideLog2;
constexpr u64 kNumPages = AddressSpaceLayout::kEnd >> kPageBits;

[[noreturn]] void Fault(const char* what, u64 va, u32 size) {
    if (g_fault_hook) g_fault_hook();
    Fatal("guest %s fault at 0x%llx (%u bytes): address not mapped", what,
          static_cast<unsigned long long>(va), size);
}

u64 HostLoad(void* user, u64 va, u32 size) {
    g_slow_path_loads.fetch_add(1, std::memory_order_relaxed);
    auto* mem = static_cast<GuestMemory*>(user);
    va &= 0xffff'ffff'ffffULL;
    if (size == 0) {
        // Guard-v2 probe: bit 32 says whether the word is mapped.
        if (va >= AddressSpaceLayout::kEnd || !mem->IsMapped(va, 4)) return 0;
        return (u64{1} << 32) | mem->Read<u32>(va);
    }
    if (va >= AddressSpaceLayout::kEnd || !mem->IsMapped(va, size)) Fault("read", va, size);
    switch (size) {
    case 1: return mem->Read<u8>(va);
    case 2: return mem->Read<u16>(va);
    case 4: return mem->Read<u32>(va);
    default: return mem->Read<u64>(va);
    }
}

void HostStore(void* user, u64 va, u32 size, u64 value) {
    g_slow_path_stores.fetch_add(1, std::memory_order_relaxed);
    auto* mem = static_cast<GuestMemory*>(user);
    va &= 0xffff'ffff'ffffULL;
    if (va >= AddressSpaceLayout::kEnd || !mem->IsMapped(va, size)) Fault("write", va, size);
    switch (size) {
    case 1: mem->Write<u8>(va, static_cast<u8>(value)); break;
    case 2: mem->Write<u16>(va, static_cast<u16>(value)); break;
    case 4: mem->Write<u32>(va, static_cast<u32>(value)); break;
    default: mem->Write<u64>(va, value); break;
    }
}

// Exclusive monitor. Each host thread remembers what its load-exclusive saw;
// the store-exclusive is a compare-and-swap against that value. This is the
// usual emulator approximation: it only misses an A-B-A change between the two
// instructions, which lock-free guest code tolerates.
struct ExclusiveState {
    u64 addr = 0;
    u32 size = 0;
    bool valid = false;
    u64 lo = 0, hi = 0;
};
thread_local ExclusiveState t_excl;

u64 AtomicLoad(u8* p, u32 size) {
    switch (size) {
    case 1: return __atomic_load_n(p, __ATOMIC_SEQ_CST);
    case 2: return __atomic_load_n(reinterpret_cast<u16*>(p), __ATOMIC_SEQ_CST);
    case 4: return __atomic_load_n(reinterpret_cast<u32*>(p), __ATOMIC_SEQ_CST);
    default: return __atomic_load_n(reinterpret_cast<u64*>(p), __ATOMIC_SEQ_CST);
    }
}

bool AtomicCas(u8* p, u32 size, u64 expected, u64 desired) {
    switch (size) {
    case 1: {
        u8 e = static_cast<u8>(expected);
        return __atomic_compare_exchange_n(p, &e, static_cast<u8>(desired), false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_SEQ_CST);
    }
    case 2: {
        u16 e = static_cast<u16>(expected);
        return __atomic_compare_exchange_n(reinterpret_cast<u16*>(p), &e, static_cast<u16>(desired),
                                           false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    case 4: {
        u32 e = static_cast<u32>(expected);
        return __atomic_compare_exchange_n(reinterpret_cast<u32*>(p), &e, static_cast<u32>(desired),
                                           false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    default: {
        u64 e = expected;
        return __atomic_compare_exchange_n(reinterpret_cast<u64*>(p), &e, desired, false,
                                           __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }
    }
}

u64 ExclLoad(void* user, u64 va, u32 size) {
    auto* mem = static_cast<GuestMemory*>(user);
    if (!mem->IsMapped(va, size)) Fault("exclusive read", va, size);
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    mem->ObserveRead(va, size, std::source_location::current());
#endif
    const u64 v = AtomicLoad(mem->Ptr(va, size), size);
    t_excl = {va, size, true, v, 0};
    return v;
}

u32 ExclStore(void* user, u64 va, u32 size, u64 value) {
    auto* mem = static_cast<GuestMemory*>(user);
    if (!t_excl.valid || t_excl.addr != va || t_excl.size != size) return 1;
    t_excl.valid = false;
    if (!mem->IsMapped(va, size)) Fault("exclusive write", va, size);
    return AtomicCas(mem->Ptr(va, size), size, t_excl.lo, value) ? 0 : 1;
}

void ClearExcl(void*) {
    t_excl.valid = false;
}

void ExclLoadPair(void* user, u64 va, u32 size, u64* lo, u64* hi) {
    auto* mem = static_cast<GuestMemory*>(user);
    if (!mem->IsMapped(va, size * 2)) Fault("exclusive pair read", va, size * 2);
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    mem->ObserveRead(va, size * 2, std::source_location::current());
#endif
    if (size == 4) {
        const u64 v = AtomicLoad(mem->Ptr(va, 8), 8);
        *lo = static_cast<u32>(v);
        *hi = v >> 32;
    } else {
        unsigned __int128 v = __atomic_load_n(reinterpret_cast<unsigned __int128*>(mem->Ptr(va, 16)),
                                              __ATOMIC_SEQ_CST);
        *lo = static_cast<u64>(v);
        *hi = static_cast<u64>(v >> 64);
    }
    t_excl = {va, size * 2, true, *lo, *hi};
}

u32 ExclStorePair(void* user, u64 va, u32 size, u64 lo, u64 hi) {
    auto* mem = static_cast<GuestMemory*>(user);
    if (!t_excl.valid || t_excl.addr != va || t_excl.size != size * 2) return 1;
    t_excl.valid = false;
    if (size == 4) {
        const u64 expected = (t_excl.lo & 0xffffffff) | (t_excl.hi << 32);
        return AtomicCas(mem->Ptr(va, 8), 8, expected, (lo & 0xffffffff) | (hi << 32)) ? 0 : 1;
    }
    unsigned __int128 expected = (static_cast<unsigned __int128>(t_excl.hi) << 64) | t_excl.lo;
    const unsigned __int128 desired = (static_cast<unsigned __int128>(hi) << 64) | lo;
    return __atomic_compare_exchange_n(reinterpret_cast<unsigned __int128*>(mem->Ptr(va, 16)), &expected,
                                       desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
               ? 0
               : 1;
}

} // namespace

// The Switch's physical counter runs at 19.2 MHz.
u64 CntpctBaseTicks() {
    static const u64 base = [] {
        const s64 wall = static_cast<s64>(::time(nullptr));
        return wall > kUserClockOffset ? static_cast<u64>(wall - kUserClockOffset) * 19'200'000ull : 0ull;
    }();
    return base;
}

u64 ReadCntpct() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();
    const u64 ns = duration_cast<nanoseconds>(steady_clock::now() - start).count();
    // ns * 19.2e6 / 1e9 = ns * 12 / 625, split to avoid overflow.
    return CntpctBaseTicks() + (ns / 625) * 12 + (ns % 625) * 12 / 625;
}

namespace {
u64 ReadCntpctCallback(void*) {
    return ReadCntpct();
}
} // namespace

bool GuestMemory::Init() {
#ifdef _WIN32
    if (!g_host_space.Init(AddressSpaceLayout::kEnd)) {
        LOG_ERROR("could not reserve the 512 GiB guest address space (needs Windows 10 1803+)");
        return false;
    }
    base_ = g_host_space.Base();
    if (!g_host_table.Init(kNumPages * kPageEntrySize)) {
        LOG_ERROR("could not reserve the guest page table");
        return false;
    }
    page_table_ = g_host_table.Base();
#else
    void* p = mmap(nullptr, AddressSpaceLayout::kEnd, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        LOG_ERROR("could not reserve the 512 GiB guest address space");
        return false;
    }
    base_ = static_cast<u8*>(p);
    void* t = mmap(nullptr, kNumPages * kPageEntrySize, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (t == MAP_FAILED) {
        LOG_ERROR("could not reserve the guest page table");
        return false;
    }
    page_table_ = static_cast<u8*>(t);
#endif

    host_mem_.user = this;
    host_mem_.load = HostLoad;
    host_mem_.store = HostStore;
    host_mem_.excl_load = ExclLoad;
    host_mem_.excl_store = ExclStore;
    host_mem_.clear_excl = ClearExcl;
    host_mem_.read_cntpct = ReadCntpctCallback;
    host_mem_.excl_load_pair = ExclLoadPair;
    host_mem_.excl_store_pair = ExclStorePair;
    host_mem_.page_entries = page_table_;
    host_mem_.page_entry_stride = kPageEntrySize;
    host_mem_.page_bits = kPageBits;
    host_mem_.pointer_mask = kPagePointerMask;
    host_mem_.address_space_max = AddressSpaceLayout::kEnd;
    host_mem_.guard_generation = nullptr;
    return true;
}

#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
std::string GuestMemory::ReadString(u64 va, u64 max_len, std::source_location location) const {
#else
std::string GuestMemory::ReadString(u64 va, u64 max_len) const {
#endif
    std::string s;
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    u64 bytes_read = 0;
#endif
    if (max_len) Guard(va, max_len, false);
    for (u64 i = 0; i < max_len; ++i) {
        if (!IsMapped(va + i)) break;
        const char c = static_cast<char>(*RawPtr(va + i));
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        ++bytes_read;
#endif
        if (!c) break;
        s.push_back(c);
    }
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    if (bytes_read) ObserveRead(va, bytes_read, location);
#endif
    return s;
}

bool GuestMemory::IsMapped(u64 va, u64 size) const {
    if (va >= AddressSpaceLayout::kEnd || size > AddressSpaceLayout::kEnd - va) return false;
    const u64 first = va >> kPageBits;
    const u64 last = (va + (size ? size - 1 : 0)) >> kPageBits;
    for (u64 page = first; page <= last; ++page) {
        u64 entry;
        std::memcpy(&entry, page_table_ + page * kPageEntrySize, sizeof(entry));
        if (!entry) return false;
    }
    return true;
}

bool GuestMemory::DivertPage(u64 page) noexcept {
    if (page >= kNumPages) return false;
    auto* entry = reinterpret_cast<u64*>(page_table_ + page * kPageEntrySize);
    std::atomic_ref<u64> atomic_entry(*entry);
    u64 current = atomic_entry.load(std::memory_order_acquire);
    // Only a fast page (real backing pointer) can be flipped; 0 (unmapped) and 1 (already slow) stay as they are.
    while ((current & kPagePointerMask) != 0) {
        if (atomic_entry.compare_exchange_weak(current, 1, std::memory_order_acq_rel)) return true;
    }
    return false;
}

void GuestMemory::RestorePage(u64 page) noexcept {
    if (page >= kNumPages) return;
    auto* entry = reinterpret_cast<u64*>(page_table_ + page * kPageEntrySize);
    std::atomic_ref<u64> atomic_entry(*entry);
    u64 expected = 1;
    atomic_entry.compare_exchange_strong(expected, reinterpret_cast<u64>(base_), std::memory_order_acq_rel);
}

void GuestMemory::SetPageEntries(u64 va, u64 size, bool mapped) {
    layout_generation_.fetch_add(1, std::memory_order_acq_rel);
    const u64 value = mapped ? reinterpret_cast<u64>(base_) : 0;
#ifdef _WIN32
    if (!g_host_table.Touch((va >> kPageBits) * kPageEntrySize,
                            (size >> kPageBits) * kPageEntrySize)) {
        Fatal("could not commit the guest page table");
    }
#endif
    for (u64 page = va >> kPageBits; page < (va + size) >> kPageBits; ++page) {
        u8* entry = page_table_ + page * kPageEntrySize;
        __atomic_store_n(reinterpret_cast<u64*>(entry), value, __ATOMIC_RELEASE);
    }
}

void GuestMemory::Split(u64 va) {
    auto it = regions_.upper_bound(va);
    if (it == regions_.begin()) return;
    --it;
    const u64 start = it->first;
    if (start == va || start + it->second.size <= va) return;
    Region tail = it->second;
    tail.size = start + it->second.size - va;
    it->second.size = va - start;
    regions_.emplace(va, tail);
}

void GuestMemory::Merge() {
    for (auto it = regions_.begin(); it != regions_.end();) {
        auto next = std::next(it);
        if (next == regions_.end()) break;
        if (it->first + it->second.size == next->first && it->second.state == next->second.state &&
            it->second.perm == next->second.perm && it->second.attr == next->second.attr) {
            it->second.size += next->second.size;
            regions_.erase(next);
        } else {
            it = next;
        }
    }
}

bool GuestMemory::Map(u64 va, u64 size, MemState state, u32 perm) {
    if ((va | size) & (kPageSize - 1) || va + size > AddressSpaceLayout::kEnd) return false;
    if (GuestAccessGuard* g = guard_.load(std::memory_order_acquire)) g->OnRemap(va, size);
#ifdef _WIN32
    if (!g_host_space.Commit(va, size)) return false;
#else
    if (mprotect(base_ + va, size, PROT_READ | PROT_WRITE) != 0) return false;
#endif
    std::lock_guard lock(mutex_);
    Split(va);
    Split(va + size);
    regions_.erase(regions_.lower_bound(va), regions_.lower_bound(va + size));
    regions_.emplace(va, Region{size, state, perm, 0});
    Merge();
    SetPageEntries(va, size, true);
    return true;
}

bool GuestMemory::MapFile(u64 va, u64 size, std::intptr_t native_handle, u64 offset, MemState state,
                          u32 perm) {
    if ((va | size) & (kPageSize - 1) || va + size > AddressSpaceLayout::kEnd) return false;
    if (GuestAccessGuard* g = guard_.load(std::memory_order_acquire)) g->OnRemap(va, size);
#ifdef _WIN32
    if (!g_host_space.MapView(va, size, reinterpret_cast<HANDLE>(native_handle),
                              offset)) {
        return false;
    }
#else
    void* p = mmap(base_ + va, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                   static_cast<int>(native_handle),
                   static_cast<off_t>(offset));
    if (p == MAP_FAILED) return false;
#endif
    std::lock_guard lock(mutex_);
    Split(va);
    Split(va + size);
    regions_.erase(regions_.lower_bound(va), regions_.lower_bound(va + size));
    regions_.emplace(va, Region{size, state, perm, 0});
    ForgetFileRanges(va, size);
    file_ranges_[va] = size;
    Merge();
    SetPageEntries(va, size, true);
    return true;
}

void GuestMemory::ForgetFileRanges(u64 va, u64 size) {
    const u64 end = va + size;
    std::vector<std::pair<u64, u64>> keep;
    auto it = file_ranges_.upper_bound(va);
    if (it != file_ranges_.begin()) --it;
    while (it != file_ranges_.end() && it->first < end) {
        const u64 s = it->first, e = it->first + it->second;
        if (e > va) {
            if (s < va) keep.emplace_back(s, va - s);
            if (e > end) keep.emplace_back(end, e - end);
            it = file_ranges_.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [s, n] : keep) file_ranges_[s] = n;
}

bool GuestMemory::OverlapsFileMapping(u64 va, u64 size) const {
    std::lock_guard lock(mutex_);
    auto it = file_ranges_.upper_bound(va);
    if (it != file_ranges_.begin() && std::prev(it)->first + std::prev(it)->second > va) return true;
    return it != file_ranges_.end() && it->first < va + size;
}

void GuestMemory::Unmap(u64 va, u64 size) {
    if (GuestAccessGuard* g = guard_.load(std::memory_order_acquire)) g->OnRemap(va, size);
    std::lock_guard lock(mutex_);
    ForgetFileRanges(va, size);
    SetPageEntries(va, size, false);
    // Replace with a fresh inaccessible reservation; this also drops file mappings.
#ifdef _WIN32
    g_host_space.Release(va, size);
#else
    mmap(base_ + va, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0);
#endif
    Split(va);
    Split(va + size);
    regions_.erase(regions_.lower_bound(va), regions_.lower_bound(va + size));
}

void GuestMemory::SetPermission(u64 va, u64 size, u32 perm) {
    std::lock_guard lock(mutex_);
    Split(va);
    Split(va + size);
    for (auto it = regions_.lower_bound(va); it != regions_.end() && it->first < va + size; ++it) {
        it->second.perm = perm;
    }
    Merge();
}

void GuestMemory::SetState(u64 va, u64 size, MemState state, u32 perm) {
    std::lock_guard lock(mutex_);
    Split(va);
    Split(va + size);
    for (auto it = regions_.lower_bound(va); it != regions_.end() && it->first < va + size; ++it) {
        it->second.state = state;
        it->second.perm = perm;
    }
    Merge();
}

void GuestMemory::SetAttribute(u64 va, u64 size, u32 mask, u32 attr) {
    std::lock_guard lock(mutex_);
    Split(va);
    Split(va + size);
    for (auto it = regions_.lower_bound(va); it != regions_.end() && it->first < va + size; ++it) {
        it->second.attr = (it->second.attr & ~mask) | (attr & mask);
    }
    Merge();
}

MemoryInfo GuestMemory::Query(u64 va) const {
    std::lock_guard lock(mutex_);
    MemoryInfo info{};
    if (va >= AddressSpaceLayout::kEnd) {
        info.base = AddressSpaceLayout::kEnd;
        info.size = 0 - AddressSpaceLayout::kEnd;
        info.state = static_cast<u32>(MemState::Inaccessible);
        return info;
    }
    auto it = regions_.upper_bound(va);
    if (it != regions_.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second.size > va) {
            info.base = prev->first;
            info.size = prev->second.size;
            info.state = static_cast<u32>(prev->second.state);
            info.permission = prev->second.perm;
            info.attribute = prev->second.attr;
            return info;
        }
        info.base = prev->first + prev->second.size;
    } else {
        info.base = 0;
    }
    const u64 end = it == regions_.end() ? AddressSpaceLayout::kEnd : it->first;
    info.size = end - info.base;
    info.state = static_cast<u32>(MemState::Free);
    return info;
}

std::optional<u64> GuestMemory::FindFree(u64 start, u64 region_size, u64 size, u64 align) const {
    u64 va = (start + align - 1) & ~(align - 1);
    const u64 end = start + region_size;
    while (va + size <= end) {
        const MemoryInfo info = Query(va);
        if (info.state == static_cast<u32>(MemState::Free) && info.base + info.size >= va + size) {
            return va;
        }
        va = ((info.base + info.size) + align - 1) & ~(align - 1);
    }
    return std::nullopt;
}

} // namespace nxrt
