// SPDX-License-Identifier: GPL-3.0-or-later
// Supervisor calls. Register conventions follow Horizon's AArch64 ABI: inputs in
// x0..x7 (shifted up by one when the call has an output in w1/x1), result code
// in w0, outputs in x1, x2, ...
#include "kernel/svc.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>

#include "cpu.h"
#include "hle/ipc.h"
#include "kernel/kernel.h"
#include "memory.h"
#include "runtime.h"
#include "tts_trace.h"

namespace nxrt {

namespace {

constexpr u32 kHandleWaitMask = 0x40000000;

// Process core configuration from main.npdm: the default core (header byte
// 0xF, what CreateThread's core -2 selects) and the core mask (ThreadInfo
// kernel capability: minimum/maximum core, what GetInfo(CoreMask) reports).
struct ProcessCores {
    s32 default_core = 0;
    u64 mask = 0x7; // applications own cores 0-2; core 3 belongs to the system
};

const ProcessCores& GetProcessCores() {
    static const ProcessCores cores = [] {
        ProcessCores pc;
        const auto path = g_config.game_dir / "exefs" / "main.npdm";
#ifdef _WIN32
        std::FILE* f = _wfopen(path.c_str(), L"rb");
#else
        std::FILE* f = std::fopen(path.c_str(), "rb");
#endif
        if (!f) return pc;
        u8 raw[0x400];
        const size_t n = std::fread(raw, 1, sizeof(raw), f);
        std::fclose(f);
        if (n < 0x80 || std::memcmp(raw, "META", 4) != 0) return pc;
        pc.default_core = raw[0xF] & 3;
        u32 aci = 0;
        std::memcpy(&aci, raw + 0x70, 4);
        if (aci + 0x38 > n || std::memcmp(raw + aci, "ACI0", 4) != 0) return pc;
        u32 kac_off = 0, kac_size = 0;
        std::memcpy(&kac_off, raw + aci + 0x30, 4);
        std::memcpy(&kac_size, raw + aci + 0x34, 4);
        for (u32 i = 0; i + 4 <= kac_size && aci + kac_off + i + 4 <= n; i += 4) {
            u32 d;
            std::memcpy(&d, raw + aci + kac_off + i, 4);
            if ((d & 0xF) != 0x7) continue; // ThreadInfo: pattern 0111
            const u32 lo = (d >> 16) & 0xFF, hi = (d >> 24) & 0xFF;
            if (lo <= hi && hi < 8) pc.mask = ((u64{1} << (hi + 1)) - 1) & ~((u64{1} << lo) - 1);
            break;
        }
        return pc;
    }();
    return cores;
}

std::array<std::atomic<u64>, 0x80> g_svc_counts{};

std::string Hex(u64 v) {
    char b[24];
    std::snprintf(b, sizeof(b), "0x%llx", static_cast<unsigned long long>(v));
    return b;
}


// NXRT_IPCCHK=<service name substring>: logs the raw request and reply words of
// matching IPC calls (1 = IDatabaseService, * = every service, printing only the
// calls whose reply holds a pointer-like word 0x0000xxxx'0000000y). Diagnostic
// only; it reads the message buffer and never changes the call.
const char* IpcChkFilter() {
    static const char* f = [] {
        const char* e = std::getenv("NXRT_IPCCHK");
        if (!e || !*e) return static_cast<const char*>(nullptr);
        return std::strcmp(e, "1") == 0 ? "IDatabaseService" : e;
    }();
    return f;
}

u32 SendSyncChecked(KThread* t, u32 handle, u64 msg, u64 size) {
    const char* filter = IpcChkFilter();
    if (!filter) return hle::SendSyncRequest(t, handle, msg, size);
    const bool all = std::strcmp(filter, "*") == 0;
    std::string name;
    {
        std::lock_guard lock(g_kernel.lock);
        auto s = g_kernel.handles.GetAs<hle::KClientSession>(handle);
        if (s && s->state && s->state->object) name = s->state->object->Name();
    }
    const bool match = all || name.find(filter) != std::string::npos;
    if (!match) return hle::SendSyncRequest(t, handle, msg, size);
    std::array<u32, 0x40> req{};
    g_memory.ReadBlock(msg, req.data(), req.size() * 4);
    const u32 r = hle::SendSyncRequest(t, handle, msg, size);
    std::array<u32, 0x40> rep{};
    g_memory.ReadBlock(msg, rep.data(), rep.size() * 4);
    auto dump = [&](const char* tag, const std::array<u32, 0x40>& w, size_t n) {
        std::string line;
        for (size_t i = 0; i < n; ++i) {
            char b[16];
            std::snprintf(b, sizeof(b), "%08x ", w[i]);
            line += b;
        }
        LOG_INFO("IPCCHK %s %s h=0x%x t=%llu %s", name.c_str(), tag, handle, (unsigned long long)t->id, line.c_str());
    };
    const size_t n = std::min<size_t>(0x40, 4 + (rep[1] & 0x3FF) + 8);
    bool hit = false;
    for (size_t i = 0; i + 1 < n; ++i) {
        const u64 v = rep[i] | (static_cast<u64>(rep[i + 1]) << 32);
        if ((v >> 44) != 0 && (v >> 48) == 0 && (v & 0xFFFFFFFF) < 0x100) hit = true;
    }
    if (all && !hit) return r;
    dump("req", req, 0x14);
    dump("rep", rep, n);
    LOG_INFO("IPCCHK %s result=0x%x svc_ret=0x%x pointer-like-word=%d", name.c_str(), rep[(n > 6) ? 6 : 0], r, hit);
    return r;
}

// ---------------------------------------------------------------- helpers

std::shared_ptr<KThread> GetThread(u32 handle) {
    return g_kernel.handles.GetAs<KThread>(handle);
}

// Handle lookup for SVCs that run without the kernel lock. Every guest thread shares the table, and
// Add grows it under that lock, so an unlocked read can race with a reallocation.
template <typename T>
std::shared_ptr<T> LookupAs(u32 handle) {
    std::lock_guard lock(g_kernel.lock);
    return g_kernel.handles.GetAs<T>(handle);
}

void InsertByPriority(std::vector<KThread*>& list, KThread* t) {
    auto it = std::find_if(list.begin(), list.end(),
                           [t](const KThread* o) { return o->priority > t->priority; });
    list.insert(it, t);
}

void Erase(std::vector<KThread*>& list, KThread* t) {
    list.erase(std::remove(list.begin(), list.end(), t), list.end());
}

u32 ReadU32(u64 va) {
    return __atomic_load_n(reinterpret_cast<u32*>(g_memory.Ptr(va, 4)), __ATOMIC_SEQ_CST);
}
void WriteU32(u64 va, u32 v) {
    __atomic_store_n(reinterpret_cast<u32*>(g_memory.Ptr(va, 4)), v, __ATOMIC_SEQ_CST);
}

// Takes the kernel lock; with NXRT_SYNC_STATS also records how long the take waited and, via
// Stop() or the destructor, how long it was held.
struct StatLock {
    std::unique_lock<std::mutex> lk;
    u64 got = 0;
    StatLock() {
        if (syncstat::g_on) {
            const u64 t0 = syncstat::NowNs();
            lk = std::unique_lock(g_kernel.lock);
            got = syncstat::NowNs();
            syncstat::Add(syncstat::LockWait, got - t0);
        } else {
            lk = std::unique_lock(g_kernel.lock);
        }
    }
    void Stop() {
        if (got) syncstat::Add(syncstat::LockHold, syncstat::NowNs() - got);
        got = 0;
    }
    ~StatLock() { Stop(); }
};

// ---------------------------------------------------------------- mutexes

void AddWaiter(KThread* owner, KThread* waiter) {
    InsertByPriority(owner->mutex_waiters, waiter);
    waiter->lock_owner = owner;
}

// Picks the next owner of the mutex at `addr` among `owner`'s waiters and hands
// it the rest of them. Kernel lock held.
KThread* RemoveUserWaiterByKey(KThread* owner, bool* has_waiters, u64 addr) {
    KThread* next = nullptr;
    *has_waiters = false;
    for (auto it = owner->mutex_waiters.begin(); it != owner->mutex_waiters.end();) {
        KThread* w = *it;
        if (w->mutex_wait_address != addr) {
            ++it;
            continue;
        }
        it = owner->mutex_waiters.erase(it);
        if (!next) {
            next = w;
            next->lock_owner = nullptr;
        } else {
            AddWaiter(next, w);
            *has_waiters = true;
        }
    }
    return next;
}

// Releases the mutex at `addr` held by the current thread. Kernel lock held.
void UnlockMutex(KThread* cur, u64 addr) {
    bool has_waiters;
    KThread* next = RemoveUserWaiterByKey(cur, &has_waiters, addr);
    u32 value = 0;
    if (next) {
        value = next->address_key_value | (has_waiters ? kHandleWaitMask : 0);
    }
    WriteU32(addr, value);
    if (next) {
        next->mutex_wait_address = 0;
        Wake(next, rc::Success);
    }
}

u32 ArbitrateLock(u32 owner_handle, u64 addr, u32 tag) {
    if (addr & 3) return rc::InvalidAddress;
    KThread* cur = CurrentThread();
    StatLock sl;
    auto& lock = sl.lk;
    if (ReadU32(addr) != (owner_handle | kHandleWaitMask)) return rc::Success;
    auto owner = GetThread(owner_handle);
    if (!owner) return rc::InvalidHandle;
    cur->mutex_wait_address = addr;
    cur->address_key_value = tag;
    cur->wait_done = false;
    AddWaiter(owner.get(), cur);
    cur->wait_what = "mutex " + Hex(addr) + " held by thread " + std::to_string(owner->id);
    sl.Stop();
    return WaitCurrent(lock, -1);
}

u32 ArbitrateUnlock(u64 addr) {
    if (addr & 3) return rc::InvalidAddress;
    StatLock sl;
    UnlockMutex(CurrentThread(), addr);
    return rc::Success;
}

// ---------------------------------------------------------------- condition variables

// A condition variable waiter was signalled: try to take its mutex for it, or
// queue it behind the current owner. Kernel lock held.
void SignalCvWaiter(KThread* t) {
    const u64 addr = t->mutex_wait_address;
    u32* word = reinterpret_cast<u32*>(g_memory.Ptr(addr, 4));
    u32 prev = __atomic_load_n(word, __ATOMIC_SEQ_CST);
    for (;;) {
        const u32 desired = prev == 0 ? t->address_key_value : (prev | kHandleWaitMask);
        if (__atomic_compare_exchange_n(word, &prev, desired, false, __ATOMIC_SEQ_CST,
                                        __ATOMIC_SEQ_CST)) {
            break;
        }
    }
    if (prev == 0) {
        t->mutex_wait_address = 0;
        Wake(t, rc::Success);
        return;
    }
    auto owner = GetThread(prev & ~kHandleWaitMask);
    if (owner) {
        AddWaiter(owner.get(), t);
    } else {
        t->mutex_wait_address = 0;
        Wake(t, rc::InvalidState);
    }
}

u32 WaitProcessWideKeyAtomic(u64 addr, u64 key, u32 tag, s64 timeout) {
    if (addr & 3) return rc::InvalidAddress;
    KThread* cur = CurrentThread();
    std::unique_lock lock(g_kernel.lock);
    // Release the mutex, then wait on the key.
    {
        bool has_waiters;
        KThread* next = RemoveUserWaiterByKey(cur, &has_waiters, addr);
        u32 next_value = 0;
        if (next) {
            next_value = next->address_key_value | (has_waiters ? kHandleWaitMask : 0);
            next->mutex_wait_address = 0;
            Wake(next, rc::Success);
        }
        WriteU32(key, 1);
        WriteU32(addr, next_value);
    }
    if (timeout == 0) return rc::TimedOut;
    cur->cv_key = key;
    cur->mutex_wait_address = addr;
    cur->address_key_value = tag;
    cur->wait_done = false;
    InsertByPriority(g_kernel.cv_waiters, cur);
    cur->wait_what = "condvar " + Hex(key) + " (mutex " + Hex(addr) + ")";
    const u32 result = WaitCurrent(lock, timeout);
    if (!cur->wait_done) {
        // Timed out: leave whatever queue still holds us.
        if (cur->cv_key) {
            Erase(g_kernel.cv_waiters, cur);
            cur->cv_key = 0;
        }
        if (cur->lock_owner) {
            Erase(cur->lock_owner->mutex_waiters, cur);
            cur->lock_owner = nullptr;
        }
        cur->mutex_wait_address = 0;
        cur->wait_done = true;
        return rc::TimedOut;
    }
    return result;
}

// NXRT_TRACE_WAKE=1: logs the first time each (signaling thread -> woken thread, kind) pair happens, with the
// signaler's guest backtrace. Called with the kernel lock held. Diagnostic only.
void NoteWake(const char* kind, KThread* woken, u64 key) {
    static const bool on = std::getenv("NXRT_TRACE_WAKE") != nullptr;
    if (!on) return;
    KThread* cur = CurrentThread();
    static std::set<std::string> seen;
    const std::string k = std::string(kind) + " " + std::to_string(cur ? cur->id : 0) + "->" + std::to_string(woken->id);
    if (!seen.insert(k).second) return;
    LOG_INFO("wake-first %s key=%llx signaler bt: %s", k.c_str(), static_cast<unsigned long long>(key),
             cur ? Backtrace(cur->ctx, 8).c_str() : "?");
}

u32 SignalProcessWideKey(u64 key, s32 count) {
    std::lock_guard lock(g_kernel.lock);
    s32 woken = 0;
    auto& list = g_kernel.cv_waiters;
    for (auto it = list.begin(); it != list.end() && (count <= 0 || woken < count);) {
        KThread* t = *it;
        if (t->cv_key != key) {
            ++it;
            continue;
        }
        it = list.erase(it);
        t->cv_key = 0;
        NoteWake("cv", t, key);
        SignalCvWaiter(t);
        ++woken;
    }
    const bool remaining = std::any_of(list.begin(), list.end(),
                                       [key](const KThread* t) { return t->cv_key == key; });
    if (!remaining) WriteU32(key, 0);
    return rc::Success;
}

// ---------------------------------------------------------------- address arbiter

enum ArbitrationType : u32 { WaitIfLessThan = 0, DecrementAndWaitIfLessThan = 1, WaitIfEqual = 2,
                             WaitIfEqual64 = 3 };
enum SignalType : u32 { Signal = 0, SignalAndIncrementIfEqual = 1,
                        SignalAndModifyByWaitingCountIfEqual = 2 };

u32 WaitForAddress(u64 addr, u32 type, s64 value, s64 timeout) {
    KThread* cur = CurrentThread();
    StatLock sl;
    auto& lock = sl.lk;
    switch (type) {
    case WaitIfLessThan:
    case DecrementAndWaitIfLessThan: {
        u32* word = reinterpret_cast<u32*>(g_memory.Ptr(addr, 4));
        s32 v = static_cast<s32>(__atomic_load_n(word, __ATOMIC_SEQ_CST));
        if (type == DecrementAndWaitIfLessThan) {
            for (;;) {
                if (v >= static_cast<s32>(value)) break;
                u32 expected = static_cast<u32>(v);
                if (__atomic_compare_exchange_n(word, &expected, static_cast<u32>(v - 1), false,
                                                __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                    break;
                }
                v = static_cast<s32>(expected);
            }
        }
        if (v >= static_cast<s32>(value)) return rc::InvalidState;
        break;
    }
    case WaitIfEqual:
        if (static_cast<s32>(ReadU32(addr)) != static_cast<s32>(value)) return rc::InvalidState;
        break;
    case WaitIfEqual64:
        if (g_memory.Read<s64>(addr) != value) return rc::InvalidState;
        break;
    default:
        return rc::InvalidEnumValue;
    }
    if (timeout == 0) return rc::TimedOut;
    cur->arbiter_address = addr;
    cur->wait_done = false;
    InsertByPriority(g_kernel.arbiter_waiters, cur);
    cur->wait_what = "address " + Hex(addr);
    sl.Stop();
    const u32 result = WaitCurrent(lock, timeout);
    if (!cur->wait_done) {
        Erase(g_kernel.arbiter_waiters, cur);
        cur->arbiter_address = 0;
        cur->wait_done = true;
        return rc::TimedOut;
    }
    return result;
}

u32 SignalToAddress(u64 addr, u32 type, s32 value, s32 count) {
    StatLock sl;
    auto& list = g_kernel.arbiter_waiters;
    auto waiting = [&] {
        return std::count_if(list.begin(), list.end(),
                             [addr](const KThread* t) { return t->arbiter_address == addr; });
    };
    switch (type) {
    case Signal:
        break;
    case SignalAndIncrementIfEqual: {
        u32 expected = static_cast<u32>(value);
        if (!__atomic_compare_exchange_n(reinterpret_cast<u32*>(g_memory.Ptr(addr, 4)), &expected,
                                         static_cast<u32>(value + 1), false, __ATOMIC_SEQ_CST,
                                         __ATOMIC_SEQ_CST)) {
            return rc::InvalidState;
        }
        break;
    }
    case SignalAndModifyByWaitingCountIfEqual: {
        const auto n = waiting();
        s32 new_value;
        if (count <= 0) {
            new_value = n > 0 ? value - 2 : value + 1;
        } else if (n > 0) {
            // Mirrors Horizon: the count of waiters beyond the first is compared.
            new_value = (n - 1) < count ? value - 1 : value;
        } else {
            new_value = value + 1;
        }
        u32 expected = static_cast<u32>(value);
        if (new_value != value) {
            if (!__atomic_compare_exchange_n(reinterpret_cast<u32*>(g_memory.Ptr(addr, 4)), &expected,
                                             static_cast<u32>(new_value), false, __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST)) {
                return rc::InvalidState;
            }
        } else if (static_cast<s32>(ReadU32(addr)) != value) {
            return rc::InvalidState;
        }
        break;
    }
    default:
        return rc::InvalidEnumValue;
    }
    s32 woken = 0;
    for (auto it = list.begin(); it != list.end() && (count <= 0 || woken < count);) {
        KThread* t = *it;
        if (t->arbiter_address != addr) {
            ++it;
            continue;
        }
        it = list.erase(it);
        t->arbiter_address = 0;
        NoteWake("addr", t, addr);
        Wake(t, rc::Success);
        ++woken;
    }
    return rc::Success;
}

// ---------------------------------------------------------------- synchronization

u32 WaitSynchronization(s32* out_index, u64 handles_addr, s32 count, s64 timeout) {
    if (count < 0 || count > 64) return rc::OutOfRange;
    KThread* cur = CurrentThread();
    std::vector<std::shared_ptr<KSyncObject>> objects;
    {
        std::lock_guard lock(g_kernel.lock);
        for (s32 i = 0; i < count; ++i) {
            const u32 h = g_memory.Read<u32>(handles_addr + 4 * i);
            auto obj = std::dynamic_pointer_cast<KSyncObject>(g_kernel.handles.Get(h));
            if (!obj) {
                static std::atomic<int> warned{0};
                if (warned++ < 8) {
                    auto any = g_kernel.handles.Get(h);
                    std::string all;
                    for (s32 j = 0; j < count; ++j) all += " " + Hex(g_memory.Read<u32>(handles_addr + 4 * j));
                    LOG_WARN("WaitSynchronization: handle 0x%x is %s (count %d:%s) from %s", h,
                             any ? any->TypeName() : "invalid", count, all.c_str(),
                             Backtrace(cur->ctx).c_str());
                }
                return rc::InvalidHandle;
            }
            objects.push_back(std::move(obj));
        }
    }
    std::unique_lock lock(g_kernel.lock);
    for (s32 i = 0; i < count; ++i) {
        if (objects[i]->IsSignaled()) {
            *out_index = i;
            return rc::Success;
        }
    }
    if (timeout == 0) return rc::TimedOut;
    cur->wait_done = false;
    cur->wait_index = -1;
    cur->waiting_on.clear();
    cur->wait_what = "sync:";
    for (auto& o : objects) {
        cur->waiting_on.push_back(o.get());
        o->waiters.push_back(cur);
        const auto* ev = dynamic_cast<const KEvent*>(o.get());
        const auto* th = dynamic_cast<const KThread*>(o.get());
        cur->wait_what += " " + (ev ? ev->name : th ? "thread " + std::to_string(th->id) : std::string(o->TypeName()));
    }
    u32 result = WaitCurrent(lock, timeout);
    for (auto& o : objects) Erase(o->waiters, cur);
    cur->waiting_on.clear();
    if (!cur->wait_done) {
        cur->wait_done = true;
        return rc::TimedOut;
    }
    *out_index = cur->wait_index;
    return result;
}

void SleepThread(s64 ns) {
    if (ns <= 0) {
        // 0: yield to same priority, -1/-2: yield to others. All just yield.
        BatonYield(CurrentThread(), ns < 0);
        std::this_thread::yield();
        return;
    }
    // A sleeping thread does not own its core.
    KThread* cur = CurrentThread();
    const bool had_baton = cur->baton_held;
    if (had_baton) BatonRelease(cur);
    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
    if (had_baton) BatonAcquire(cur);
}

// ---------------------------------------------------------------- memory

u32 SetHeapSize(u64* out_addr, u64 size) {
    if (size & 0x1FFFFF) return rc::InvalidSize;
    if (size > AddressSpaceLayout::kHeapSize) return rc::OutOfMemory;
    std::lock_guard lock(g_kernel.lock);
    const u64 base = AddressSpaceLayout::kHeapStart;
    if (size > g_kernel.heap_size) {
        g_memory.Map(base + g_kernel.heap_size, size - g_kernel.heap_size, MemState::Normal, PermRW);
    } else if (size < g_kernel.heap_size) {
        g_memory.Unmap(base + size, g_kernel.heap_size - size);
    }
    g_kernel.heap_size = size;
    *out_addr = base;
    return rc::Success;
}

u32 MapMemory(u64 dst, u64 src, u64 size) {
    // Used to map stacks into the stack region. Without page aliasing the
    // contents move to the new place and the source becomes inaccessible, which
    // is what the guest observes on hardware too.
    if (!g_memory.IsMapped(src, size)) return rc::InvalidCurrentMemory;
    if (!g_memory.Map(dst, size, MemState::Stack, PermRW)) return rc::InvalidMemoryRegion;
    std::memcpy(g_memory.Ptr(dst, size), g_memory.Ptr(src, size), size);
    g_memory.SetState(src, size, MemState::Normal, PermNone);
    g_memory.SetAttribute(src, size, 1, 1); // Locked
    return rc::Success;
}

u32 UnmapMemory(u64 dst, u64 src, u64 size) {
    std::memcpy(g_memory.Ptr(src, size), g_memory.Ptr(dst, size), size);
    g_memory.Unmap(dst, size);
    g_memory.SetState(src, size, MemState::Normal, PermRW);
    g_memory.SetAttribute(src, size, 1, 0);
    return rc::Success;
}

u32 QueryMemory(u64 info_addr, u64 addr) {
    const MemoryInfo info = g_memory.Query(addr);
    g_memory.WriteBlock(info_addr, &info, sizeof(info));
    return rc::Success;
}

// ---------------------------------------------------------------- info

u32 GetInfo(u64* out, u32 type, u32 handle, u64 sub) {
    using L = AddressSpaceLayout;
    switch (type) {
    case 0: *out = GetProcessCores().mask; return rc::Success;    // CoreMask (npdm)
    case 1: *out = 0xFFFFFFFFFFFFFFF0ULL; return rc::Success;     // PriorityMask
    case 2: *out = L::kAliasStart; return rc::Success;
    case 3: *out = L::kAliasSize; return rc::Success;
    case 4: *out = L::kHeapStart; return rc::Success;
    case 5: *out = L::kHeapSize; return rc::Success;
    case 6: *out = L::kTotalMemory; return rc::Success;           // TotalMemorySize
    case 7: *out = g_kernel.heap_size + 0x2000000; return rc::Success; // UsedMemorySize (approx.)
    case 8: *out = 0; return rc::Success;                         // DebuggerAttached
    case 9: *out = 0; return rc::Success;                         // ResourceLimit (no handle)
    case 10: *out = 0; return rc::Success;                        // IdleTickCount
    case 11: {                                                    // RandomEntropy
        static std::mutex rng_lock;
        static std::mt19937_64 rng{0x5eed};
        std::lock_guard lock(rng_lock);
        *out = rng();
        return rc::Success;
    }
    case 12: *out = L::kAslrStart; return rc::Success;
    case 13: *out = L::kAslrSize; return rc::Success;
    case 14: *out = L::kStackStart; return rc::Success;
    case 15: *out = L::kStackSize; return rc::Success;
    case 16: *out = 0; return rc::Success;                        // SystemResourceSizeTotal
    case 17: *out = 0; return rc::Success;                        // SystemResourceSizeUsed
    case 18: *out = g_kernel.program_id; return rc::Success;
    case 20: {                                                    // UserExceptionContextAddress
        // The process-local region: one page the SDK uses for dying messages
        // and exception context.
        static const u64 region = [] {
            const u64 va = L::kKernelMapStart + L::kKernelMapSize - kPageSize;
            g_memory.Map(va, kPageSize, MemState::ThreadLocal, PermRW);
            return va;
        }();
        *out = region;
        return rc::Success;
    }
    case 21: *out = L::kTotalMemory; return rc::Success;          // TotalNonSystemMemorySize
    case 22: *out = g_kernel.heap_size + 0x2000000; return rc::Success;
    case 23: *out = 1; return rc::Success;                        // IsApplication
    case 24: *out = 0x100; return rc::Success;                    // FreeThreadCount
    case 25: *out = 0; return rc::Success;                        // ThreadTickCount
    case 26: *out = 1; return rc::Success;                        // IsSvcPermitted
    case 28: *out = 0; return rc::Success;                        // AliasRegionExtraSize
    default:
        LOG_WARN("GetInfo: unknown type %u (handle 0x%x, sub %llu)", type, handle,
                 static_cast<unsigned long long>(sub));
        return rc::InvalidEnumValue;
    }
}

} // namespace

void DumpSvcStats() {
    for (size_t i = 0; i < g_svc_counts.size(); ++i) {
        if (const u64 n = g_svc_counts[i].load()) {
            std::fprintf(stderr, "svc 0x%02zx: %llu\n", i, static_cast<unsigned long long>(n));
        }
    }
}

static void DispatchSvc(KThread* t, u32 imm) {
    GuestContext& c = t->ctx;
    auto& x = c.x;
    if (imm < g_svc_counts.size()) g_svc_counts[imm].fetch_add(1, std::memory_order_relaxed);
    LOG_TRACE("svc 0x%02x from %s x0=%llx x1=%llx x2=%llx x3=%llx", imm, Symbolize(c.x[30]).c_str(),
              (unsigned long long)x[0], (unsigned long long)x[1], (unsigned long long)x[2],
              (unsigned long long)x[3]);
    u32 r = rc::Success;
    switch (imm) {
    case 0x01: { u64 addr = 0; r = SetHeapSize(&addr, x[1]); x[1] = addr; break; }
    case 0x02: g_memory.SetPermission(x[0], x[1], static_cast<u32>(x[2])); break;
    case 0x03: g_memory.SetAttribute(x[0], x[1], static_cast<u32>(x[2]), static_cast<u32>(x[3])); break;
    case 0x04: r = MapMemory(x[0], x[1], x[2]); break;
    case 0x05: r = UnmapMemory(x[0], x[1], x[2]); break;
    case 0x06: r = QueryMemory(x[0], x[2]); x[1] = 0; break;
    case 0x07: LOG_INFO("guest called ExitProcess"); std::fflush(nullptr); std::_Exit(0);
    case 0x08: {
        const s32 prio = static_cast<s32>(x[4]);
        s32 core = static_cast<s32>(x[5]);
        if (core == -2) core = GetProcessCores().default_core; // process default core (main.npdm)
        std::lock_guard lock(g_kernel.lock);
        auto nt = CreateThread(x[1], x[2], x[3], prio, core);
        LOG_DEBUG("CreateThread %s -> handle 0x%x prio %d core %d", Symbolize(x[1]).c_str(), nt->handle,
                  prio, core);
        x[1] = nt->handle;
        break;
    }
    case 0x09: {
        auto th = LookupAs<KThread>(static_cast<u32>(x[0]));
        if (!th) { r = rc::InvalidHandle; break; }
        StartThread(th);
        break;
    }
    case 0x0A:
        ExitCurrentThread();
        return;
    case 0x0B: SleepThread(static_cast<s64>(x[0])); break;
    case 0x0C: {
        auto th = LookupAs<KThread>(static_cast<u32>(x[1]));
        if (!th) { r = rc::InvalidHandle; break; }
        x[1] = static_cast<u32>(th->priority);
        break;
    }
    case 0x0D: {
        auto th = LookupAs<KThread>(static_cast<u32>(x[0]));
        if (!th) { r = rc::InvalidHandle; break; }
        th->priority = static_cast<s32>(x[1]);
        break;
    }
    case 0x0E: {
        auto th = LookupAs<KThread>(static_cast<u32>(x[2]));
        if (!th) { r = rc::InvalidHandle; break; }
        x[1] = static_cast<u32>(th->ideal_core);
        x[2] = th->affinity_mask;
        break;
    }
    case 0x0F: {
        auto th = LookupAs<KThread>(static_cast<u32>(x[0]));
        if (!th) { r = rc::InvalidHandle; break; }
        const s32 core = static_cast<s32>(x[1]);
        if (core >= 0) th->ideal_core = core;
        if (x[2]) th->affinity_mask = x[2];
        break;
    }
    case 0x10: {
        // The core the guest thread was assigned (SetThreadCoreMask), not the
        // host CPU the emulating thread happens to run on: guest code indexes
        // per-core work lists with this number, and a host-dependent value
        // makes two workers share (or one worker change) its list.
        // NXRT_HOSTCORE=1 restores the old sched_getcpu() % 3, for A/B runs.
        static const bool host_core = [] {
            const char* e = std::getenv("NXRT_HOSTCORE");
            return e && *e == '1';
        }();
#ifdef _WIN32
        const u32 host = static_cast<u32>(GetCurrentProcessorNumber() % 3);
#else
        const u32 host = static_cast<u32>(sched_getcpu() % 3);
#endif
        const u32 core = static_cast<u32>(EffectiveCore(t));
        LOG_DEBUG("GetCurrentProcessorNumber thread %llu -> %u (host cpu %% 3 = %u, ideal %d, mask 0x%llx)",
                  static_cast<unsigned long long>(t->id), host_core ? host : core, host, t->ideal_core,
                  static_cast<unsigned long long>(t->affinity_mask));
        x[0] = host_core ? host : core;
        return;
    }
    case 0x11: {
        auto ev = LookupAs<KEvent>(static_cast<u32>(x[0]));
        if (!ev) { r = rc::InvalidHandle; break; }
        ev->Signal();
        break;
    }
    case 0x12:
    case 0x17: {
        auto ev = LookupAs<KEvent>(static_cast<u32>(x[0]));
        if (!ev) { r = rc::InvalidHandle; break; }
        if (imm == 0x17 && !ev->signaled) { r = rc::InvalidState; break; }
        ev->Clear();
        break;
    }
    case 0x13: r = hle::MapSharedMemory(static_cast<u32>(x[0]), x[1], x[2], static_cast<u32>(x[3])); break;
    case 0x14: r = hle::UnmapSharedMemory(static_cast<u32>(x[0]), x[1], x[2]); break;
    case 0x15: {
        auto tm = std::make_shared<KTransferMemory>();
        tm->address = x[1];
        tm->size = x[2];
        std::lock_guard lock(g_kernel.lock);
        x[1] = g_kernel.handles.Add(tm);
        break;
    }
    case 0x16: {
        std::lock_guard lock(g_kernel.lock);
        if (!g_kernel.handles.Close(static_cast<u32>(x[0]))) r = rc::InvalidHandle;
        break;
    }
    case 0x18: {
        s32 index = -1;
        r = WaitSynchronization(&index, x[1], static_cast<s32>(x[2]), static_cast<s64>(x[3]));
        x[1] = static_cast<u32>(index);
        break;
    }
    case 0x19: break; // CancelSynchronization: nothing waits uninterruptibly here
    case 0x1A: r = ArbitrateLock(static_cast<u32>(x[0]), x[1], static_cast<u32>(x[2])); break;
    case 0x1B: r = ArbitrateUnlock(x[0]); break;
    case 0x1C:
        r = WaitProcessWideKeyAtomic(x[0], x[1], static_cast<u32>(x[2]), static_cast<s64>(x[3]));
        break;
    case 0x1D: r = SignalProcessWideKey(x[0], static_cast<s32>(x[1])); break;
    case 0x1E: x[0] = ReadCntpct(); return;
    case 0x1F: {
        const std::string name = g_memory.ReadString(x[1], 12);
        u32 handle = 0;
        r = hle::ConnectToNamedPort(name, &handle);
        x[1] = handle;
        break;
    }
    case 0x21: r = SendSyncChecked(t, static_cast<u32>(x[0]), t->tls_address, 0x100); break;
    case 0x22: r = SendSyncChecked(t, static_cast<u32>(x[2]), x[0], x[1]); break;
    case 0x25: {
        auto th = LookupAs<KThread>(static_cast<u32>(x[1]));
        if (!th) { r = rc::InvalidHandle; break; }
        x[1] = th->id;
        break;
    }
    case 0x26:
        LOG_ERROR("guest Break(reason 0x%llx, 0x%llx, 0x%llx) from %s", (unsigned long long)x[0],
                  (unsigned long long)x[1], (unsigned long long)x[2], Symbolize(c.x[30]).c_str());
        if (!(x[0] & 0x80000000)) Fatal("guest aborted");
        break;
    case 0x27: {
        const std::string s = g_memory.ReadString(x[0], std::min<u64>(x[1], 4096));
        LOG_INFO("guest: %s", s.c_str());
        break;
    }
    case 0x29: {
        u64 out = 0;
        r = GetInfo(&out, static_cast<u32>(x[1]), static_cast<u32>(x[2]), x[3]);
        x[1] = out;
        break;
    }
    case 0x34:
        r = WaitForAddress(x[0], static_cast<u32>(x[1]), static_cast<s64>(x[2]), static_cast<s64>(x[3]));
        break;
    case 0x35:
        r = SignalToAddress(x[0], static_cast<u32>(x[1]), static_cast<s32>(x[2]), static_cast<s32>(x[3]));
        break;
    default:
        LOG_ERROR("unimplemented svc 0x%02x from %s", imm, Symbolize(c.x[30]).c_str());
        r = rc::NotImplemented;
        break;
    }
    x[0] = r;
}

// NXRT_TRACE_TID=<id,id,...>: logs every SVC (number, x0..x3 in, x0/x1 out, caller backtrace) of those guest threads.
static bool TraceTid(u64 id) {
    static const std::set<u64> ids = [] {
        std::set<u64> r;
        if (const char* e = std::getenv("NXRT_TRACE_TID")) {
            std::string tok;
            for (const char* p = e;; ++p) {
                if (*p == ',' || !*p) { if (!tok.empty()) r.insert(std::strtoull(tok.c_str(), nullptr, 10)); tok.clear(); if (!*p) break; }
                else tok.push_back(*p);
            }
        }
        return r;
    }();
    return !ids.empty() && ids.count(id);
}

void CallSvc(KThread* t, u32 imm) {
    const bool trace_tid = TraceTid(t->id);
    u64 in[4] = {};
    if (trace_tid) {
        std::memcpy(in, t->ctx.x, sizeof(in));
        LOG_INFO("svc-trace tid=%llu svc=0x%x in x0=%llx x1=%llx x2=%llx x3=%llx bt %s", static_cast<unsigned long long>(t->id), imm,
                 static_cast<unsigned long long>(in[0]), static_cast<unsigned long long>(in[1]), static_cast<unsigned long long>(in[2]),
                 static_cast<unsigned long long>(in[3]), Backtrace(t->ctx, 8).c_str());
    }
    // Opt-in core baton: own the core's baton while running guest code, and let a
    // higher-priority thread of the same core in at the SVC boundary.
    if (CoreBatonEnabled()) BatonSync(t);
    if (tts_trace::Enabled()) tts_trace::SvcEnter(t, imm, t->ctx.x);
    if (syncstat::g_on) {
        const u64 t0 = syncstat::NowNs();
        const s64 sleep_ns = static_cast<s64>(t->ctx.x[0]);
        const s64 wait_to = static_cast<s64>(t->ctx.x[3]);
        if (imm == 0x0B && sleep_ns > 0) syncstat::Add(syncstat::SleepReq, static_cast<u64>(sleep_ns));
        if (imm == 0x34 && wait_to > 0) syncstat::Add(syncstat::WaitTimedReq, static_cast<u64>(wait_to));
        DispatchSvc(t, imm);
        if (imm == 0x34 && wait_to > 0 && static_cast<u32>(t->ctx.x[0]) == rc::TimedOut) {
            syncstat::Add(syncstat::WaitTimedOut, syncstat::NowNs() - t0);
        }
        const u64 dt = syncstat::NowNs() - t0;
        switch (imm) {
        case 0x0B: syncstat::Add(sleep_ns <= 0 ? syncstat::Sleep0 : syncstat::SleepN, dt); break;
        case 0x18: syncstat::Add(syncstat::WaitSync, dt); break;
        case 0x1A: syncstat::Add(syncstat::Lock, dt); break;
        case 0x1B: syncstat::Add(syncstat::Unlock, dt); break;
        case 0x1C: syncstat::Add(syncstat::WaitCv, dt); break;
        case 0x1D: syncstat::Add(syncstat::SigCv, dt); break;
        case 0x34: syncstat::Add(syncstat::WaitAddr, dt); break;
        case 0x35: syncstat::Add(syncstat::SigAddr, dt); break;
        default: break;
        }
    } else {
        DispatchSvc(t, imm);
    }
    if (trace_tid)
        LOG_INFO("svc-trace tid=%llu svc=0x%x out x0=%llx x1=%llx", static_cast<unsigned long long>(t->id), imm,
                 static_cast<unsigned long long>(t->ctx.x[0]), static_cast<unsigned long long>(t->ctx.x[1]));
    if (tts_trace::Enabled()) tts_trace::SvcLeave(t);
    if (CoreBatonEnabled()) BatonPreemptPoint(t);
}

} // namespace nxrt
