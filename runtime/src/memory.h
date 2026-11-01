// SPDX-License-Identifier: GPL-3.0-or-later
// The guest address space. One host reservation covers the whole 39-bit space,
// so guest address `va` lives at host address `base + va`; mapping only commits
// or decommits pages. That keeps every access a single addition, and makes the
// generated code's fast-path page table trivial: every mapped page's entry holds
// the same pointer, `base`.
#pragma once
#include <atomic>

#include <cstring>
#include <cstdint>
#include <atomic>
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
#include <source_location>
#endif
#include <map>
#include <mutex>
#include <optional>

#include "common.h"
#include "guest_abi.h"

namespace nxrt {

// svc::MemoryState
enum class MemState : u32 {
    Free = 0x00,
    Io = 0x01,
    Static = 0x02,
    Code = 0x03,
    CodeData = 0x04,
    Normal = 0x05,
    Shared = 0x06,
    Alias = 0x07,
    AliasCode = 0x08,
    AliasCodeData = 0x09,
    Ipc = 0x0A,
    Stack = 0x0B,
    ThreadLocal = 0x0C,
    Transfered = 0x0D,
    SharedTransfered = 0x0E,
    SharedCode = 0x0F,
    Inaccessible = 0x10,
    NonSecureIpc = 0x11,
    NonDeviceIpc = 0x12,
    Kernel = 0x13,
    GeneratedCode = 0x14,
    CodeOut = 0x15,
    Coverage = 0x16,
};

// svc::MemoryPermission
enum MemPerm : u32 {
    PermNone = 0,
    PermR = 1,
    PermW = 2,
    PermX = 4,
    PermRW = PermR | PermW,
    PermRX = PermR | PermX,
    PermDontCare = 1u << 28,
};

// svc::MemoryInfo as written by QueryMemory.
struct MemoryInfo {
    u64 base;
    u64 size;
    u32 state;
    u32 attribute;
    u32 permission;
    u32 ipc_count;
    u32 device_count;
    u32 padding;
};
static_assert(sizeof(MemoryInfo) == 0x28);

// Fixed layout of the 39-bit address space this runtime hands out.
struct AddressSpaceLayout {
    static constexpr u64 kBits = 39;
    static constexpr u64 kEnd = u64{1} << kBits;
    static constexpr u64 kAslrStart = 0x08000000;
    static constexpr u64 kAslrSize = kEnd - kAslrStart;
    static constexpr u64 kCodeBase = 0x80000000;
    static constexpr u64 kAliasStart = 0x10'0000'0000;
    static constexpr u64 kAliasSize = 0x10'0000'0000; // 64 GiB
    static constexpr u64 kHeapStart = 0x20'0000'0000;
    static constexpr u64 kHeapSize = 0x10'0000'0000;  // 64 GiB of room, limited by kTotalMemory
    static constexpr u64 kStackStart = 0x30'0000'0000;
    static constexpr u64 kStackSize = 0x8000'0000;     // 2 GiB
    static constexpr u64 kKernelMapStart = 0x38'0000'0000;
    static constexpr u64 kKernelMapSize = 0x10'0000'0000;
    // What a retail application with the default memory arrangement may use.
    static constexpr u64 kTotalMemory = 0xCD50'0000;
};

// Optional interceptor for accesses to guest memory whose newest contents still live elsewhere (the renderer's
// asynchronous compute write-backs). Every GuestMemory accessor calls BeforeAccess before touching the bytes;
// the AOT fast path reaches it through the page-table slow path (entry value 1 -> HostLoad/HostStore -> Read/Write).
class GuestAccessGuard {
public:
    virtual ~GuestAccessGuard() = default;
    virtual void BeforeAccess(u64 va, u64 size, bool write) noexcept = 0;
    // [va, va+size) is about to be remapped or unmapped: pending data for it is obsolete.
    virtual void OnRemap(u64 va, u64 size) noexcept = 0;
};

class GuestMemory {
public:
    bool Init();

    // At most one guard; install before the renderer starts producing pending data, clear after it is drained.
    void SetAccessGuard(GuestAccessGuard* guard) noexcept { guard_.store(guard, std::memory_order_release); }
    void Guard(u64 va, u64 size, bool write) const noexcept {
        if (GuestAccessGuard* g = guard_.load(std::memory_order_acquire)) g->BeforeAccess(va, size, write);
    }
    // Page-table diversion used by the guard: entry 1 keeps the page mapped but sends AOT loads/stores to
    // HostLoad/HostStore. Divert returns true when this call flipped a fast page; Restore only undoes a diversion
    // (entry 1 -> backing pointer) and leaves unmapped pages (entry 0) alone.
    bool DivertPage(u64 page) noexcept;
    void RestorePage(u64 page) noexcept;

#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    using ReadObserver = void (*)(void* context, u64 address, u64 size, const char* file,
                                  std::uint_least32_t line) noexcept;

    // Optional diagnostic callback. Install before worker threads start; clear it after they stop.
    void SetReadObserver(ReadObserver observer, void* context) noexcept {
        read_observer_context_.store(context, std::memory_order_relaxed);
        read_observer_.store(observer, std::memory_order_release);
    }

    void ObserveRead(u64 va, u64 size,
                     std::source_location location = std::source_location::current()) const noexcept {
        const ReadObserver observer = read_observer_.load(std::memory_order_acquire);
        if (observer) {
            observer(read_observer_context_.load(std::memory_order_relaxed), va, size, location.file_name(),
                     location.line());
        }
    }
#endif

    u8* Base() const { return base_; }
    const void* PageTable() const { return page_table_; }

    // Raw host pointer without the access guard; valid for any va, faults if the page is not mapped. Only for the
    // guard's own materialization and for code that has already called Guard for the same bytes.
    u8* RawPtr(u64 va) const { return base_ + (va & (AddressSpaceLayout::kEnd - 1)); }
    // Guarded host pointer for the `size` bytes the caller is about to read or write through it.
    u8* Ptr(u64 va, u64 size) const {
        Guard(va & (AddressSpaceLayout::kEnd - 1), size, true);
        return RawPtr(va);
    }

    template <typename T>
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    T Read(u64 va, std::source_location location = std::source_location::current()) const {
        ObserveRead(va, sizeof(T), location);
#else
    T Read(u64 va) const {
#endif
        Guard(va, sizeof(T), false);
        T v;
        std::memcpy(&v, RawPtr(va), sizeof(T));
        return v;
    }
    template <typename T>
    void Write(u64 va, T v) {
        Guard(va, sizeof(T), true);
        std::memcpy(RawPtr(va), &v, sizeof(T));
    }
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    void ReadBlock(u64 va, void* dst, u64 size,
                   std::source_location location = std::source_location::current()) const {
        ObserveRead(va, size, location);
#else
    void ReadBlock(u64 va, void* dst, u64 size) const {
#endif
        Guard(va, size, false);
        std::memcpy(dst, RawPtr(va), size);
    }
    void WriteBlock(u64 va, const void* src, u64 size) {
        Guard(va, size, true);
        std::memcpy(RawPtr(va), src, size);
    }
    void Zero(u64 va, u64 size) {
        Guard(va, size, true);
        std::memset(RawPtr(va), 0, size);
    }
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    std::string ReadString(u64 va, u64 max_len,
                           std::source_location location = std::source_location::current()) const;
#else
    std::string ReadString(u64 va, u64 max_len) const;
#endif

    bool IsMapped(u64 va, u64 size = 1) const;

    // Commit [va, va+size) as zeroed anonymous memory with the given state.
    bool Map(u64 va, u64 size, MemState state, u32 perm);
    // Map a host shared-memory object (POSIX fd or Windows section HANDLE) into the guest.
    bool MapFile(u64 va, u64 size, std::intptr_t native_handle, u64 offset, MemState state,
                 u32 perm);
    void Unmap(u64 va, u64 size);
    // Changes bookkeeping only; the host keeps every mapped page read-write.
    void SetPermission(u64 va, u64 size, u32 perm);
    void SetState(u64 va, u64 size, MemState state, u32 perm);
    void SetAttribute(u64 va, u64 size, u32 mask, u32 attr);

    MemoryInfo Query(u64 va) const;
    // Lowest free range of `size` bytes inside [start, start+region_size).
    std::optional<u64> FindFree(u64 start, u64 region_size, u64 size, u64 align = kPageSize) const;

    RecompHostMem& HostMem() { return host_mem_; }

    // Bumped by every Map/MapFile/Unmap. Dirty tracking of the host pages (texture_dirty_tracker.h) is only valid
    // while the layout it verified is still the current one.
    u64 LayoutGeneration() const { return layout_generation_.load(std::memory_order_acquire); }
    // True when [va, va+size) overlaps a MapFile (shared object) mapping. Those objects have other host mappings
    // (HID, time, font ...) whose writes do not touch this mapping's page tables, so dirty tracking must not
    // trust them.
    bool OverlapsFileMapping(u64 va, u64 size) const;

private:
    struct Region {
        u64 size;
        MemState state;
        u32 perm;
        u32 attr;
    };
    void Split(u64 va);
    void Merge();
    void SetPageEntries(u64 va, u64 size, bool mapped);
    void ForgetFileRanges(u64 va, u64 size);  // requires mutex_

    u8* base_ = nullptr;
    u8* page_table_ = nullptr;
    std::atomic<GuestAccessGuard*> guard_{nullptr};
    std::map<u64, Region> regions_; // mapped ranges only, non-overlapping
    mutable std::mutex mutex_;
    std::map<u64, u64> file_ranges_;  // start -> size, MapFile mappings only (guarded by mutex_)
    std::atomic<u64> layout_generation_{0};
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    std::atomic<ReadObserver> read_observer_{nullptr};
    std::atomic<void*> read_observer_context_{nullptr};
#endif
    RecompHostMem host_mem_{};
};

extern GuestMemory g_memory;
// Called before a guest memory fault aborts, to report the guest state.
extern void (*g_fault_hook)();

// The guest's 19.2 MHz physical counter (CNTPCT_EL0).
u64 ReadCntpct();

} // namespace nxrt
