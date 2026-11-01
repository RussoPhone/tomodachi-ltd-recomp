// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#ifndef _WIN32
#include <csignal>
#include <execinfo.h>
#include <sys/mman.h>
#endif
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <system_error>

#include "hle/hooks.h"
#include "kernel/kernel.h"
#include "kernel/svc.h"
#include "memory.h"

namespace nxrt {

namespace {
std::vector<RecompModule> g_modules;
u64 g_initial_fpcr = kFpxInhibit;
// How many blocks one direct-call chain may run before returning to the loop.
// Each nested block uses host stack; host threads get 64 MiB.
constexpr int kChainBudget = 1024;
} // namespace

static void DumpCurrentOnFault();
namespace {
void InstallStoreWatch();
void RearmStoreWatch();
void InstallHidReadTrace();
void RearmHidReadTrace();
void InstallFontEngineLock(const std::vector<LoadedModule>& loaded);
bool StoreWatchActive();
} // namespace

bool InitRecompModules(const std::vector<LoadedModule>& loaded) {
    g_fault_hook = DumpCurrentOnFault;
    unsigned count = 0;
    const RecompStaticModule* mods = suyu_recomp_static_modules_v4(&count);
    if (count != loaded.size()) {
        LOG_ERROR("the executable has %u recompiled modules but ExeFS has %zu", count, loaded.size());
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        // Load order is what lines modules up, as in suyu: names differ between
        // the exporter and the NSO files.
        mods[i].set_base(loaded[i].base);
        g_modules.push_back({mods[i].name, loaded[i].base, loaded[i].base + loaded[i].size,
                             mods[i].lookup, mods[i].run_slice});
        LOG_INFO("recompiled module %-5s -> %s at 0x%llx (abi %u)", mods[i].name,
                 loaded[i].file.c_str(), static_cast<unsigned long long>(loaded[i].base),
                 mods[i].image_abi());
    }

    if (!suyu_recomp_static_guard_v2(2)) {
        LOG_ERROR("modules did not accept the guard-v2 contract");
        return false;
    }
    // Code guard generations: blocks verify their instructions against guest
    // memory once per generation instead of on every entry. Nothing in this
    // runtime rewrites code, so one generation, set after loading, is enough.
    RecompGuardGenModule gg[16];
    const unsigned ngg = suyu_recomp_static_guard_gen_v1(2, gg, 16);
    for (unsigned i = 0; i < ngg; ++i) {
        __atomic_store_n(gg[i].word, 1u, __ATOMIC_RELEASE);
    }
    LOG_INFO("code guard: %s", ngg ? "verify once per generation" : "verify on every entry");

    if (!suyu_recomp_static_fastmem_v1(kPageBits, kPageEntryStrideLog2, kPagePointerMask,
                                       offsetof(GuestContext, fm_table),
                                       offsetof(GuestContext, fm_limit))) {
        LOG_ERROR("modules refused the fast-path memory layout");
        return false;
    }
    const unsigned fpx = suyu_recomp_static_fpx_v1(offsetof(GuestContext, fpcr),
                                                   offsetof(GuestContext, fpsr), kFpxInhibit);
    g_initial_fpcr = fpx ? 0 : kFpxInhibit;
    LOG_INFO("native FP fast paths: %s", fpx & 0xff ? "on" : "off");
    InstallStoreWatch();
    InstallHidReadTrace();
    InstallFontEngineLock(loaded);
    return true;
}

u64 InitialFpcr() {
    return g_initial_fpcr;
}

const RecompModule* FindModule(u64 pc) {
    for (const auto& m : g_modules) {
        if (pc >= m.base && pc < m.end) return &m;
    }
    return nullptr;
}

std::string Symbolize(u64 address) {
    char buf[64];
    if (const RecompModule* m = FindModule(address)) {
        std::snprintf(buf, sizeof(buf), "%s+0x%llx", m->name,
                      static_cast<unsigned long long>(address - m->base));
    } else {
        std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(address));
    }
    return buf;
}

static void DumpThread(KThread* t, const char* why);

std::string Backtrace(const GuestContext& c, int max_frames) {
    std::string out = Symbolize(c.x[30]);
    u64 fp = c.x[29];
    for (int i = 0; i < max_frames && fp && g_memory.IsMapped(fp, 16); ++i) {
        const u64 lr = g_memory.Read<u64>(fp + 8);
        if (!lr) break;
        out += " <- " + Symbolize(lr);
        fp = g_memory.Read<u64>(fp);
    }
    return out;
}

[[noreturn]] static void GuestCrash(KThread* t, const char* why) {
    DumpThread(t, why);
    Fatal("guest crashed");
}

static void DumpThread(KThread* t, const char* why) {
    const GuestContext& c = t->ctx;
    if (g_memory.IsMapped(c.x[0], 0x40)) {
        // The object the method is called on: its first words (vtable pointer, then fields).
        std::string words;
        for (int k = 0; k < 8; ++k) {
            char b[24];
            std::snprintf(b, sizeof(b), " %016llx", static_cast<unsigned long long>(g_memory.Read<u64>(c.x[0] + 8 * k)));
            words += b;
        }
        LOG_ERROR("  object at x0:%s", words.c_str());
    }
    if (g_memory.IsMapped(c.x[0], sizeof(u64))) {
        // The vtable the object points to: first words (offset-to-top, RTTI, then method slots).
        const u64 vt = g_memory.Read<u64>(c.x[0]);
        if (g_memory.IsMapped(vt, 0x100)) {
            std::string words;
            for (int k = 0; k < 16; ++k) {
                char b[24];
                std::snprintf(b, sizeof(b), " %llx", static_cast<unsigned long long>(g_memory.Read<u64>(vt + 8 * k)));
                words += b;
            }
            LOG_ERROR("  vtable %llx:%s", static_cast<unsigned long long>(vt), words.c_str());
        }
    }
    for (int r : {0, 25}) {
        const MemoryInfo info = g_memory.Query(c.x[r]);
        LOG_ERROR("  x%d %016llx is in region [%llx, +%llx) state %u perm %u", r,
                  static_cast<unsigned long long>(c.x[r]), static_cast<unsigned long long>(info.base),
                  static_cast<unsigned long long>(info.size), info.state, info.permission);
    }
    if (StoreWatchActive()) {
        // NXRT_WATCHST is on: show the 48-byte elements around x22, as u64 words.
        const u64 e = c.x[22] - 48 * 3;
        for (int i = 0; i < 8 && g_memory.IsMapped(e + 48 * i, 48); ++i) {
            LOG_ERROR("  elem %016llx: %016llx %016llx %016llx %016llx %016llx %016llx%s",
                      static_cast<unsigned long long>(e + 48 * i),
                      static_cast<unsigned long long>(g_memory.Read<u64>(e + 48 * i)),
                      static_cast<unsigned long long>(g_memory.Read<u64>(e + 48 * i + 8)),
                      static_cast<unsigned long long>(g_memory.Read<u64>(e + 48 * i + 16)),
                      static_cast<unsigned long long>(g_memory.Read<u64>(e + 48 * i + 24)),
                      static_cast<unsigned long long>(g_memory.Read<u64>(e + 48 * i + 32)),
                      static_cast<unsigned long long>(g_memory.Read<u64>(e + 48 * i + 40)),
                      e + 48 * i == c.x[22] ? "  <- x22" : "");
        }
    }
    LOG_ERROR("guest thread %llu stopped: %s at %s", static_cast<unsigned long long>(t->id), why,
              Symbolize(c.pc).c_str());
    for (int i = 0; i < 31; i += 2) {
        LOG_ERROR("  x%-2d %016llx  x%-2d %016llx", i, static_cast<unsigned long long>(c.x[i]), i + 1,
                  static_cast<unsigned long long>(c.x[i + 1]));
    }
    LOG_ERROR("  lr  %s  sp %016llx", Symbolize(c.x[30]).c_str(), static_cast<unsigned long long>(c.x[31]));
    LOG_ERROR("  backtrace %s", Backtrace(c).c_str());
}

static void DumpCurrentOnFault() {
    KThread* t = CurrentThread();
    if (!t) return;
    DumpThread(t, "memory fault (pc is the start of the running block)");
    // Other threads may be running guest code on the same data right now.
    DumpThreads();
}

namespace {
// Address a nested guest call returns to; never mapped, never registered.
constexpr u64 kReturnTrap = hle::kFakeFunctionBase - 16;
} // namespace

// Debug aid, NXRT_WATCH=<hex guest va>: keeps a copy of the page holding that
// address and compares it at every SVC, before and after the call. A change
// seen after a call was made by that call's host code (services, kernel); a
// change seen before it happened between the previous call and this one. It
// names windows, not the instruction that wrote.
namespace {
struct WatchState {
    u64 va = 0;
    u64 page = 0;
    bool mapped = false;
    std::vector<u8> copy;
    u64 word = 0;
    u64 prev_thread = 0;
    u32 prev_svc = 0;
    u64 prev_pc = 0;
};
WatchState g_watch;
std::mutex g_watch_lock;

void WatchCheck(KThread* t, u32 svc, u64 svc_pc, bool after) {
    static const u64 va = [] {
        const char* s = std::getenv("NXRT_WATCH");
        return s ? std::strtoull(s, nullptr, 16) : u64{0};
    }();
    RearmStoreWatch();
    if (!va) return;
    std::lock_guard lock(g_watch_lock);
    if (!g_watch.va) {
        g_watch.va = va;
        g_watch.page = va & ~(kPageSize - 1);
        g_watch.copy.resize(kPageSize);
    }
    const u64 page = g_watch.page;
    const u64 tid = t->id;
    if (!g_memory.IsMapped(page, kPageSize)) {
        if (g_watch.mapped) {
            LOG_ERROR("watch: page 0x%llx unmapped by svc 0x%02x of thread %llu at %s",
                      static_cast<unsigned long long>(page), svc, static_cast<unsigned long long>(tid),
                      Symbolize(svc_pc).c_str());
        }
        g_watch.mapped = false;
    } else {
        const u8* now = g_memory.Ptr(page, kPageSize);
        const u64 word = g_memory.Read<u64>(va);
        if (!g_watch.mapped) {
            LOG_ERROR("watch: page 0x%llx mapped by svc 0x%02x of thread %llu", static_cast<unsigned long long>(page),
                      svc, static_cast<unsigned long long>(tid));
        } else if (std::memcmp(now, g_watch.copy.data(), kPageSize) != 0) {
            u64 lo = 0, hi = 0;
            for (u64 i = 0; i < kPageSize; ++i) {
                if (now[i] != g_watch.copy[i]) {
                    if (!lo) lo = i + 1;
                    hi = i;
                }
            }
            const char* when = after ? "during" : "before";
            LOG_ERROR("watch: %s svc 0x%02x of thread %llu at %s: page bytes [0x%llx..0x%llx] changed, word %016llx -> %016llx; "
                      "previous call svc 0x%02x of thread %llu at %s",
                      when, svc, static_cast<unsigned long long>(tid), Symbolize(svc_pc).c_str(),
                      static_cast<unsigned long long>(page + lo - 1), static_cast<unsigned long long>(page + hi),
                      static_cast<unsigned long long>(g_watch.word), static_cast<unsigned long long>(word),
                      g_watch.prev_svc, static_cast<unsigned long long>(g_watch.prev_thread),
                      Symbolize(g_watch.prev_pc).c_str());
        }
        std::memcpy(g_watch.copy.data(), now, kPageSize);
        g_watch.word = word;
        g_watch.mapped = true;
    }
    if (after) {
        g_watch.prev_thread = tid;
        g_watch.prev_svc = svc;
        g_watch.prev_pc = svc_pc;
    }
}
} // namespace

// Debug aid, NXRT_WATCHST=<lo>-<hi> (hex guest va): pages in [lo, hi) are put on
// the slow memory path (page entry 1: nonzero for IsMapped, zero once masked by
// the fast path) so every guest store to them reaches HostStore, which logs a
// store that looks like the corrupted list-element word, {u32 3, u32 0x1804}:
// an 8-byte store whose high half is 0x1804, a 4-byte store of 0x1804 at +4, or
// a value in NXRT_WATCHVAL (comma-separated hex). The slow path does not know
// the instruction: it reports the thread, the start of the block that is
// running (ctx.pc) and the last SVC window, which is enough to find the block.
// Entries are re-armed at every SVC because Map/Unmap rewrite them.
namespace {
struct StoreWatch {
    u64 lo = 0, hi = 0;
    std::vector<u64> values;
    void (*orig_store)(void*, u64, u32, u64) = nullptr;
    u32 (*orig_excl_store)(void*, u64, u32, u64) = nullptr;
    std::mutex lock;
    std::map<std::pair<u64, u32>, u32> per_site; // (block pc, size) -> hits
    bool protect = false;                        // NXRT_WATCHPROT=1: host writers fault
    std::atomic<bool> unprotected{false};        // a fault opened a page; re-protect at the next SVC
    std::atomic<u32> faults{0};
};
StoreWatch g_sw;
bool StoreWatchActive() {
    return g_sw.orig_store != nullptr;
}

bool StoreMatches(u64 va, u32 size, u64 value) {
    if (size > 8) return true; // wide stores: report every site once, whatever the value
    if (size == 8 && (value >> 32) == 0x1804) return true;
    // First word of the foreign 48-byte layout seen over the list: {u32 0x23, u32 small}.
    if (size == 8 && static_cast<u32>(value) == 0x23 && (value >> 32) < 0x10000) return true;
    if (size == 4 && value == 0x1804 && (va & 7) == 4) return true;
    for (u64 v : g_sw.values) {
        if ((size == 8 && value == v) || (size == 4 && static_cast<u32>(value) == static_cast<u32>(v))) return true;
    }
    return false;
}

void StoreWatchReport(u64 va, u32 size, u64 value, const char* kind) {
    KThread* t = CurrentThread();
    {
        // One line per (block, size) site, the first three times: the builder
        // loop alone would otherwise fill the log.
        std::lock_guard lock(g_sw.lock);
        u32& n = g_sw.per_site[{t ? t->ctx.pc : 0, size}];
        if (++n > 3) return;
    }
    if (!t) {
        LOG_ERROR("storewatch: %s %u bytes at 0x%llx = 0x%llx from a host thread", kind, size,
                  static_cast<unsigned long long>(va), static_cast<unsigned long long>(value));
        return;
    }
    const GuestContext& c = t->ctx;
    LOG_ERROR("storewatch: %s %u bytes at 0x%llx = 0x%llx, thread %llu, block %s, x30 %s, backtrace %s", kind, size,
              static_cast<unsigned long long>(va), static_cast<unsigned long long>(value),
              static_cast<unsigned long long>(t->id), Symbolize(c.pc).c_str(), Symbolize(c.x[30]).c_str(),
              Backtrace(c).c_str());
}

// Guest stores are the expected writers: open their pages for the duration of
// the store so that only host-side writers (IPC buffers, MapMemory, GPU, futex
// words) fault. Not atomic across threads; a fault inside a bracket is benign.
void OpenPages(u64 va, u32 size) {
#ifndef _WIN32
    if (!g_sw.protect) return;
    const u64 first = va & ~(kPageSize - 1), last = (va + size - 1) & ~(kPageSize - 1);
    mprotect(g_memory.RawPtr(first), last - first + kPageSize, PROT_READ | PROT_WRITE);
#else
    (void)va;
    (void)size;
#endif
}

void ClosePages(u64 va, u32 size) {
#ifndef _WIN32
    if (!g_sw.protect) return;
    const u64 first = va & ~(kPageSize - 1), last = (va + size - 1) & ~(kPageSize - 1);
    mprotect(g_memory.RawPtr(first), last - first + kPageSize, PROT_READ);
#else
    (void)va;
    (void)size;
#endif
}

void WatchedStore(void* user, u64 va, u32 size, u64 value) {
    const u64 a = va & 0xffff'ffff'ffffULL;
    const bool in = a >= g_sw.lo && a + size <= g_sw.hi;
    if (in && StoreMatches(a, size, value)) StoreWatchReport(a, size, value, "store");
    if (in) OpenPages(a, size);
    g_sw.orig_store(user, va, size, value);
    if (in) ClosePages(a, size);
}

u32 WatchedExclStore(void* user, u64 va, u32 size, u64 value) {
    const bool in = va >= g_sw.lo && va + size <= g_sw.hi;
    if (in && StoreMatches(va, size, value)) StoreWatchReport(va, size, value, "excl");
    if (in) OpenPages(va, size);
    const u32 r = g_sw.orig_excl_store(user, va, size, value);
    if (in) ClosePages(va, size);
    return r;
}

#ifndef _WIN32
void ProtFaultHandler(int, siginfo_t* info, void*) {
    const u64 a = reinterpret_cast<u64>(info->si_addr);
    const u64 base = reinterpret_cast<u64>(g_memory.RawPtr(0));
    const u64 va = a - base;
    if (a < base || va < g_sw.lo || va >= g_sw.hi) {
        signal(SIGSEGV, SIG_DFL); // not ours: let the retry crash normally
        return;
    }
    if (g_sw.faults.fetch_add(1) < 60) {
        void* frames[24];
        const int n = backtrace(frames, 24);
        std::string host;
        for (int i = 0; i < n; ++i) {
            char b[24];
            std::snprintf(b, sizeof(b), " %p", frames[i]);
            host += b;
        }
        KThread* t = CurrentThread();
        LOG_ERROR("storewatch: HOST write at guest 0x%llx, guest thread %llu, in block %s, x30 %s; host frames:%s",
                  static_cast<unsigned long long>(va), static_cast<unsigned long long>(t ? t->id : 0),
                  Symbolize(t ? t->ctx.pc : 0).c_str(), Symbolize(t ? t->ctx.x[30] : 0).c_str(), host.c_str());
    }
    mprotect(reinterpret_cast<void*>(a & ~(kPageSize - 1)), kPageSize, PROT_READ | PROT_WRITE);
    g_sw.unprotected.store(true);
}
#endif

void InstallStoreWatch() {
#ifdef _WIN32
    if (const char* p = std::getenv("NXRT_WATCHPROT"); p && *p == '1') {
        LOG_WARN("NXRT_WATCHPROT is unavailable on Windows; host writes will not be trapped");
    }
#endif
    const char* s = std::getenv("NXRT_WATCHST");
    if (!s) return;
    char* end = nullptr;
    g_sw.lo = std::strtoull(s, &end, 16);
    if (*end == '-') g_sw.hi = std::strtoull(end + 1, nullptr, 16);
    if (g_sw.hi <= g_sw.lo) return;
    if (const char* v = std::getenv("NXRT_WATCHVAL")) {
        while (*v) {
            char* e = nullptr;
            g_sw.values.push_back(std::strtoull(v, &e, 16));
            if (e == v) break;
            v = *e == ',' ? e + 1 : e;
        }
    }
    RecompHostMem& hm = g_memory.HostMem();
    g_sw.orig_store = hm.store;
    g_sw.orig_excl_store = hm.excl_store;
    hm.store = WatchedStore;
    hm.excl_store = WatchedExclStore;
#ifndef _WIN32
    if (const char* p = std::getenv("NXRT_WATCHPROT"); p && *p == '1') {
        g_sw.protect = true;
        struct sigaction sa {};
        sa.sa_sigaction = ProtFaultHandler;
        sa.sa_flags = SA_SIGINFO | SA_NODEFER;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGSEGV, &sa, nullptr);
    }
#endif
    LOG_INFO("storewatch: [0x%llx, 0x%llx) on the slow path, %zu extra values", static_cast<unsigned long long>(g_sw.lo),
             static_cast<unsigned long long>(g_sw.hi), g_sw.values.size());
}

void RearmStoreWatch() {
    if (g_sw.orig_store) {
        auto* table = static_cast<u8*>(const_cast<void*>(g_memory.PageTable()));
        const bool reprotect_all = g_sw.unprotected.exchange(false);
        for (u64 va = g_sw.lo & ~(kPageSize - 1); va < g_sw.hi; va += kPageSize) {
            auto* entry = reinterpret_cast<u64*>(table + (va >> kPageBits) * (u64{1} << kPageEntryStrideLog2));
            const u64 e = __atomic_load_n(entry, __ATOMIC_RELAXED);
            if (e > 3) __atomic_store_n(entry, 1, __ATOMIC_RELEASE);
#ifndef _WIN32
            if (g_sw.protect && (e > 3 || (e == 1 && reprotect_all))) mprotect(g_memory.RawPtr(va), kPageSize, PROT_READ);
#endif
        }
    }
    RearmHidReadTrace();
}
} // namespace

namespace {

constexpr u64 kHidTouchOffset = 0x400;
constexpr u64 kHidTouchSize = 0x2C38;
constexpr u64 kHidGestureOffset = 0x3BA00;
constexpr u64 kHidGestureSize = 0x800;
constexpr u64 kHidGestureHeaderSize = 0x20;
constexpr u64 kHidGestureEntrySize = 0x68;
constexpr u64 kHidGestureTraceMaxLines = 512;
constexpr u64 kHidNpadButtonsOffset = 0x9A58;
constexpr u64 kHidNpadButtonStride = 0x30;
constexpr u64 kHidNpadButtonCount = 17;
constexpr u64 kHidNpadButtonsEnd = kHidNpadButtonsOffset +
                                  (kHidNpadButtonCount - 1) * kHidNpadButtonStride + sizeof(u64);
constexpr u64 kHidNpadTraceFirstFrame = 2640;
constexpr u64 kHidNpadTraceLastFrame = 2700;
constexpr u64 kHidNpadTraceMaxLines = 4096;
constexpr u64 kHidTraceMaxLines = 32;
constexpr u64 kHidTraceTouchLines = 2048;

struct HidReadTrace {
    bool enabled = false;
    bool trace_touch = false;
    bool trace_gesture = false;
    u64 gesture_from_frame = 0;
    std::atomic<u64> lo{0};
    std::atomic<u64> hi{0};
    std::atomic<u64> npad_lo{0};
    std::atomic<u64> gesture_lo{0};
    std::atomic<u64> mapping_end{0};
    std::atomic<u64> vi_frame{0};
    std::atomic<u64> npad_reads{0};
    std::atomic<u64> gesture_reads{0};
    u64 (*orig_load)(void*, u64, u32) = nullptr;
    u64 (*orig_excl_load)(void*, u64, u32) = nullptr;
    void (*orig_excl_load_pair)(void*, u64, u32, u64*, u64*) = nullptr;
    std::atomic<u64> reads{0};
    std::atomic<u64> max_lines{kHidTraceMaxLines};
};

HidReadTrace g_hid_read_trace;

bool ParseStrictU64(const char* text, u64& parsed) {
    if (!text || !*text) return false;
    for (const char* p = text; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < '0' || c > '9') return false;
    }
    u64 value = 0;
    const char* end = text + std::strlen(text);
    const auto result = std::from_chars(text, end, value, 10);
    if (result.ec != std::errc{} || result.ptr != end) return false;
    parsed = value;
    return true;
}

bool HidTracePageOverlapsStoreWatch(u64 page) {
    return g_sw.orig_store && page < g_sw.hi && page + kPageSize > g_sw.lo;
}

void SetHidTracePages(bool slow_path) {
    const u64 lo = g_hid_read_trace.lo.load(std::memory_order_acquire);
    const u64 hi = g_hid_read_trace.hi.load(std::memory_order_acquire);
    if (!g_hid_read_trace.enabled || !lo) return;
    auto* table = static_cast<u8*>(const_cast<void*>(g_memory.PageTable()));
    const u64 npad_lo = g_hid_read_trace.npad_lo.load(std::memory_order_acquire);
    const u64 gesture_lo = g_hid_read_trace.gesture_lo.load(std::memory_order_acquire);
    const u64 ranges[][2] = {
        {g_hid_read_trace.trace_touch ? lo : 0, g_hid_read_trace.trace_touch ? hi : 0},
        {g_hid_read_trace.trace_touch ? npad_lo : 0,
         g_hid_read_trace.trace_touch && npad_lo ? npad_lo + kHidNpadButtonsEnd - kHidNpadButtonsOffset : 0},
        {g_hid_read_trace.trace_gesture ? gesture_lo : 0,
         g_hid_read_trace.trace_gesture && gesture_lo ? gesture_lo + kHidGestureSize : 0},
    };
    for (const auto& range : ranges) {
        if (!range[0]) continue;
        const u64 first = range[0] & ~(kPageSize - 1);
        const u64 end = (range[1] + kPageSize - 1) & ~(kPageSize - 1);
        for (u64 va = first; va < end; va += kPageSize) {
            auto* entry = reinterpret_cast<u64*>(table + (va >> kPageBits) * (u64{1} << kPageEntryStrideLog2));
            const u64 e = __atomic_load_n(entry, __ATOMIC_RELAXED);
            if (slow_path) {
                if (e > 3) __atomic_store_n(entry, 1, __ATOMIC_RELEASE);
            } else if (e == 1 && !HidTracePageOverlapsStoreWatch(va)) {
                __atomic_store_n(entry, reinterpret_cast<u64>(g_memory.Base()), __ATOMIC_RELEASE);
            }
        }
    }
}

bool HidReadIntersects(u64 va, u32 size) {
    if (!g_hid_read_trace.trace_touch) return false;
    const u64 lo = g_hid_read_trace.lo.load(std::memory_order_acquire);
    const u64 hi = g_hid_read_trace.hi.load(std::memory_order_acquire);
    if (!size || !lo) return false;
    va &= 0xffff'ffff'ffffULL;
    return va < hi && (va >= lo || size > lo - va);
}

void TraceHidRead(u64 va, u32 size, u64 value, u64 high_value, const char* kind) {
    if (!HidReadIntersects(va, size)) return;
    const u64 ordinal = g_hid_read_trace.reads.fetch_add(1, std::memory_order_relaxed) + 1;
    const u64 max_lines = g_hid_read_trace.max_lines.load(std::memory_order_relaxed);
    if (ordinal <= max_lines) {
        KThread* thread = CurrentThread();
        LOG_INFO("hid-read: #%llu kind=%s va=0x%llx size=%u value=0x%llx high=0x%llx thread=%llu block_pc=%s",
                 static_cast<unsigned long long>(ordinal), kind, static_cast<unsigned long long>(va), size,
                 static_cast<unsigned long long>(value), static_cast<unsigned long long>(high_value),
                 static_cast<unsigned long long>(thread ? thread->id : 0),
                 Symbolize(thread ? thread->ctx.pc : 0).c_str());
    } else if (ordinal == max_lines + 1) {
        LOG_INFO("hid-read: further matching loads suppressed; total continues counting");
    }
}

bool HidGestureReadIntersects(u64 va, u32 size) {
    const u64 lo = g_hid_read_trace.gesture_lo.load(std::memory_order_acquire);
    if (!g_hid_read_trace.trace_gesture || !size || !lo) return false;
    va &= 0xffff'ffff'ffffULL;
    const u64 hi = lo + kHidGestureSize;
    return va >= lo && va < hi && size <= hi - va;
}

const char* HidGestureFieldName(u64 offset, u32 size) {
    if (offset < kHidGestureHeaderSize) {
        if (size != sizeof(u64) || (offset & 7)) return "header-bytes";
        switch (offset) {
        case 0x00: return "timestamp";
        case 0x08: return "total_buffer_count";
        case 0x10: return "buffer_tail";
        case 0x18: return "buffer_count";
        default: return "header-bytes";
        }
    }
    const u64 entry_offset = (offset - kHidGestureHeaderSize) % kHidGestureEntrySize;
    if (size == sizeof(u64) && !(entry_offset & 7)) {
        switch (entry_offset) {
        case 0x00: return "sampling_number";
        case 0x08: return "sampling_number2";
        case 0x10: return "context_number";
        default: break;
        }
    }
    if (size == sizeof(u32) && !(entry_offset & 3)) {
        switch (entry_offset) {
        case 0x18: return "type";
        case 0x1C: return "direction";
        case 0x20: return "x";
        case 0x24: return "y";
        case 0x28: return "delta_x";
        case 0x2C: return "delta_y";
        case 0x30: return "velocity_x_bits";
        case 0x34: return "velocity_y_bits";
        case 0x38: return "attributes";
        case 0x3C: return "scale_bits";
        case 0x40: return "rotation_angle_bits";
        case 0x44: return "point_count";
        default:
            if (entry_offset >= 0x48 && entry_offset < kHidGestureEntrySize)
                return ((entry_offset - 0x48) & 7) ? "point_y" : "point_x";
            break;
        }
    }
    return "entry-bytes";
}

void TraceHidGestureRead(u64 va, u32 size, u64 value, u64 high_value, const char* kind) {
    if (!HidGestureReadIntersects(va, size)) return;
    const u64 frame = g_hid_read_trace.vi_frame.load(std::memory_order_acquire);
    if (frame < g_hid_read_trace.gesture_from_frame) return;
    const u64 ordinal = g_hid_read_trace.gesture_reads.fetch_add(1, std::memory_order_relaxed) + 1;
    if (ordinal > kHidGestureTraceMaxLines) {
        if (ordinal == kHidGestureTraceMaxLines + 1)
            LOG_INFO("hid-gesture-read: event limit %llu reached; further loads suppressed",
                     static_cast<unsigned long long>(kHidGestureTraceMaxLines));
        return;
    }
    const u64 lo = g_hid_read_trace.gesture_lo.load(std::memory_order_acquire);
    const u64 normalized_va = va & 0xffff'ffff'ffffULL;
    const u64 access_end = normalized_va + size;
    const bool fully_inside = access_end >= normalized_va && normalized_va >= lo &&
                              access_end <= lo + kHidGestureSize;
    const char* field = fully_inside ? HidGestureFieldName(normalized_va - lo, size) : "boundary-bytes";
    KThread* thread = CurrentThread();
    LOG_INFO("hid-gesture-read: #%llu q=%llu kind=%s va=0x%llx size=%u field=%s value=0x%llx high=0x%llx thread=%llu block_pc=%s",
             static_cast<unsigned long long>(ordinal),
             static_cast<unsigned long long>(frame),
             kind, static_cast<unsigned long long>(va), size, field,
             static_cast<unsigned long long>(value), static_cast<unsigned long long>(high_value),
             static_cast<unsigned long long>(thread ? thread->id : 0),
             Symbolize(thread ? thread->ctx.pc : 0).c_str());
}

void TraceNpadButtonRead(u64 va, u32 size, u64 value, u64 high_value, const char* kind) {
    const u64 npad_lo = g_hid_read_trace.npad_lo.load(std::memory_order_acquire);
    if (!g_hid_read_trace.trace_touch || !npad_lo || !size || size > 16) return;
    const u64 frame = g_hid_read_trace.vi_frame.load(std::memory_order_acquire);
    if (frame < kHidNpadTraceFirstFrame || frame > kHidNpadTraceLastFrame) return;
    va &= 0xffff'ffff'ffffULL;
    const u64 access_end = va + size;
    if (access_end < va) return;

    for (u64 i = 0; i < kHidNpadButtonCount; ++i) {
        const u64 button_va = npad_lo + i * kHidNpadButtonStride;
        const u64 read_begin = std::max(va, button_va);
        const u64 read_end = std::min(access_end, button_va + sizeof(u64));
        if (read_begin >= read_end) continue;

        u64 mask = 0;
        for (u64 byte = read_begin; byte < read_end; ++byte) {
            const u64 source_offset = byte - va;
            const u64 source = source_offset < 8 ? value : high_value;
            const u64 source_byte = source_offset < 8 ? source_offset : source_offset - 8;
            mask |= ((source >> (source_byte * 8)) & 0xFF) << ((byte - button_va) * 8);
        }

        const u64 ordinal = g_hid_read_trace.npad_reads.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ordinal <= kHidNpadTraceMaxLines) {
            KThread* thread = CurrentThread();
            LOG_INFO("hid-read: npad-buttons #%llu q=%llu kind=%s va=0x%llx size=%u word=0x%llx mask_size=%llu mask=0x%llx thread=%llu block_pc=%s",
                     static_cast<unsigned long long>(ordinal), static_cast<unsigned long long>(frame), kind,
                     static_cast<unsigned long long>(va), size, static_cast<unsigned long long>(button_va),
                     static_cast<unsigned long long>(read_end - read_begin), static_cast<unsigned long long>(mask),
                     static_cast<unsigned long long>(thread ? thread->id : 0),
                     Symbolize(thread ? thread->ctx.pc : 0).c_str());
        } else if (ordinal == kHidNpadTraceMaxLines + 1) {
            LOG_INFO("hid-read: npad-buttons further matching loads suppressed");
        }
    }
}

u64 TracedHidLoad(void* user, u64 va, u32 size) {
    const u64 value = g_hid_read_trace.orig_load(user, va, size);
    TraceNpadButtonRead(va, size, value, 0, "load");
    TraceHidRead(va, size, value, 0, "load");
    TraceHidGestureRead(va, size, value, 0, "load");
    return value;
}

u64 TracedHidExclLoad(void* user, u64 va, u32 size) {
    const u64 value = g_hid_read_trace.orig_excl_load(user, va, size);
    TraceNpadButtonRead(va, size, value, 0, "exclusive-load");
    TraceHidRead(va, size, value, 0, "exclusive-load");
    TraceHidGestureRead(va, size, value, 0, "exclusive-load");
    return value;
}

void TracedHidExclLoadPair(void* user, u64 va, u32 size, u64* lo, u64* hi) {
    g_hid_read_trace.orig_excl_load_pair(user, va, size, lo, hi);
    TraceNpadButtonRead(va, size * 2, *lo, *hi, "exclusive-pair-load");
    TraceHidRead(va, size * 2, *lo, *hi, "exclusive-pair-load");
    TraceHidGestureRead(va, size * 2, *lo, *hi, "exclusive-pair-load");
}

void InstallHidReadTrace() {
    const char* touch = std::getenv("NXRT_TRACE_HID_READS");
    const char* gesture = std::getenv("NXRT_TRACE_HID_GESTURES");
    g_hid_read_trace.trace_touch = touch && std::strcmp(touch, "1") == 0;
    g_hid_read_trace.trace_gesture = gesture && std::strcmp(gesture, "1") == 0;
    const char* from_frame = std::getenv("NXRT_TRACE_HID_GESTURES_FROM_FRAME");
    if (from_frame && !ParseStrictU64(from_frame, g_hid_read_trace.gesture_from_frame)) {
        LOG_WARN("hid-gesture-read: invalid NXRT_TRACE_HID_GESTURES_FROM_FRAME; expected unsigned decimal u64, gesture trace disabled");
        g_hid_read_trace.trace_gesture = false;
        g_hid_read_trace.gesture_from_frame = 0;
    }
    if (!g_hid_read_trace.trace_touch && !g_hid_read_trace.trace_gesture) return;
    g_hid_read_trace.enabled = true;
    RecompHostMem& host_mem = g_memory.HostMem();
    g_hid_read_trace.orig_load = host_mem.load;
    g_hid_read_trace.orig_excl_load = host_mem.excl_load;
    g_hid_read_trace.orig_excl_load_pair = host_mem.excl_load_pair;
    host_mem.load = TracedHidLoad;
    host_mem.excl_load = TracedHidExclLoad;
    host_mem.excl_load_pair = TracedHidExclLoadPair;
    LOG_INFO("hid-read: enabled touch=%u gesture=%u gesture_from_frame=%llu; waiting for HID shared-memory mapping",
             g_hid_read_trace.trace_touch, g_hid_read_trace.trace_gesture,
             static_cast<unsigned long long>(g_hid_read_trace.gesture_from_frame));
}

void RearmHidReadTrace() {
    SetHidTracePages(true);
}

} // namespace

void ResetHidReadTraceOnTouchTransition() {
    if (!g_hid_read_trace.enabled) return;
    g_hid_read_trace.max_lines.store(kHidTraceTouchLines, std::memory_order_relaxed);
    const u64 previous = g_hid_read_trace.reads.exchange(0, std::memory_order_acq_rel);
    LOG_INFO("hid-read: tracing next %llu loads after touch state change; previous window had %llu reads",
             static_cast<unsigned long long>(kHidTraceTouchLines),
             static_cast<unsigned long long>(previous));
}

void OnSharedMemoryMapped(const std::string& name, u64 address, u64 size) {
    if (!g_hid_read_trace.enabled || name != "hid") return;
    if (size < (g_hid_read_trace.trace_gesture ? kHidGestureOffset + kHidGestureSize
                                               : kHidTouchOffset + kHidTouchSize)) {
        LOG_WARN("hid-read: HID shared memory at 0x%llx is too small (%llu bytes)",
                 static_cast<unsigned long long>(address), static_cast<unsigned long long>(size));
        return;
    }

    if (g_hid_read_trace.lo.load(std::memory_order_acquire)) SetHidTracePages(false);
    g_hid_read_trace.mapping_end.store(address + size, std::memory_order_relaxed);
    g_hid_read_trace.npad_lo.store(g_hid_read_trace.trace_touch && size >= kHidNpadButtonsEnd
                                       ? address + kHidNpadButtonsOffset
                                       : 0,
                                   std::memory_order_release);
    const u64 lo = address + kHidTouchOffset;
    g_hid_read_trace.hi.store(lo + kHidTouchSize, std::memory_order_relaxed);
    g_hid_read_trace.gesture_lo.store(g_hid_read_trace.trace_gesture
                                         ? address + kHidGestureOffset
                                         : 0,
                                     std::memory_order_release);
    g_hid_read_trace.lo.store(lo, std::memory_order_release);
    SetHidTracePages(true);
    LOG_INFO("hid-read: HID mapped at 0x%llx; touch=[0x%llx, 0x%llx) gesture=[0x%llx, 0x%llx)",
             static_cast<unsigned long long>(address), static_cast<unsigned long long>(lo),
             static_cast<unsigned long long>(lo + kHidTouchSize),
             static_cast<unsigned long long>(g_hid_read_trace.gesture_lo.load(std::memory_order_acquire)),
             static_cast<unsigned long long>(g_hid_read_trace.gesture_lo.load(std::memory_order_relaxed) +
                                             (g_hid_read_trace.trace_gesture ? kHidGestureSize : 0)));
}

void SetHidReadTraceFrame(u64 frame) {
    g_hid_read_trace.vi_frame.store(frame, std::memory_order_release);
}

void OnSharedMemoryUnmapped(u64 address, u64 size) {
    if (!g_hid_read_trace.enabled || !g_hid_read_trace.lo.load(std::memory_order_acquire) || !size) return;
    const u64 end = address + size;
    const u64 mapping_lo = g_hid_read_trace.lo.load(std::memory_order_acquire) - kHidTouchOffset;
    const u64 mapping_end = g_hid_read_trace.mapping_end.load(std::memory_order_acquire);
    if (address >= mapping_end || end <= mapping_lo) return;
    SetHidTracePages(false);
    g_hid_read_trace.lo.store(0, std::memory_order_release);
    g_hid_read_trace.hi.store(0, std::memory_order_relaxed);
    g_hid_read_trace.npad_lo.store(0, std::memory_order_release);
    g_hid_read_trace.gesture_lo.store(0, std::memory_order_release);
    g_hid_read_trace.mapping_end.store(0, std::memory_order_relaxed);
    LOG_INFO("hid-read: HID mapping at 0x%llx unmapped; trace disarmed",
             static_cast<unsigned long long>(address));
}

namespace {
// ------------------------------------------------------------ nn::font engine lock
// The game's nn::font drives Monotype iType through nn::fontll::ScalableFontEngine
// objects that have no lock: its texture cache was set up with a single engine
// (the per-core engine option is off), and the game only uses fonts from threads
// of core 0, which on the console never run at the same time. Here two of them
// (prio 44 and 50) do run in parallel and corrupt the engine (K233/A2: faults in
// map_font, FSS_set_cmap and the glyph lookup, all under main+0x10b2838).
// Instead of serializing whole cores (NXRT_CORE_BATON), only the engine is
// serialized: every ScalableFontEngine method the game calls is hooked; a thread
// that enters one without holding the lock takes it and keeps it until the
// nn::font function that drives the engine returns (its saved return address is
// pointed at a host function that unlocks). NXRT_FONT_LOCK=0 turns it off.
constexpr const char* kFontEngineMethods[] = {
    "_ZN2nn6fontll18ScalableFontEngine8GetErrorEv",
    "_ZN2nn6fontll18ScalableFontEngine8LoadFontEPcPKvjj",
    "_ZN2nn6fontll18ScalableFontEngine7SetFontEPc",
    "_ZN2nn6fontll18ScalableFontEngine13SetBoldWeightEi",
    "_ZN2nn6fontll18ScalableFontEngine15SetOutlineWidthEt",
    "_ZN2nn6fontll18ScalableFontEngine8SetFlagsENS1_5FlagsE",
    "_ZN2nn6fontll18ScalableFontEngine14GetFontMetricsEPNS0_7MetricsE",
    "_ZN2nn6fontll18ScalableFontEngine11SetAutoHintEb",
    "_ZN2nn6fontll18ScalableFontEngine15CheckGlyphExistEj",
    "_ZN2nn6fontll18ScalableFontEngine8SetScaleEiiii",
    "_ZN2nn6fontll18ScalableFontEngine15AcquireGlyphmapEjt",
    "_ZN2nn6fontll18ScalableFontEngine13ReleasesGlyphEPv",
    "_ZN2nn6fontll18ScalableFontEngine10GetAdvanceEPsS2_PiS3_jt",
    "_ZN2nn6fontll18ScalableFontEngine22AcquireOtfKerningFirstEPKNS0_15OtfKerningTableEjj",
    "_ZN2nn6fontll18ScalableFontEngine21AcquireOtfKerningLastEPKNS0_15OtfKerningTableEjj",
    "_ZN2nn6fontll18ScalableFontEngine17AcquireOtfKerningEPKNS0_15OtfKerningTableEjjj",
    "_ZN2nn6fontll18ScalableFontEngine10GetKerningEPiS2_jj",
};
// The game's nn::font functions that call the engine and each other (main+0x10b676c
// to main+0x10b78fc): the lock is held until the outermost of them returns, so a
// SetFont and the glyph calls after it stay together.
constexpr u64 kFontFunctionsBegin = 0x10b676c;
constexpr u64 kFontFunctionsEnd = 0x10b78fc;
constexpr u32 kFontFunctionsFirstWord = 0xb95ba808; // ldr w8, [x0, #0x1ba8]

struct FontEngineHold {
    bool held = false;
    u64 return_to = 0;
};
thread_local FontEngineHold t_font_hold;
std::mutex g_font_engine_lock;
std::atomic<u64> g_font_engine_holds{0};
std::atomic<u64> g_font_engine_waits{0};
u64 g_font_unlock = 0;

void FontEngineEnter(GuestContext& c, u64 font_begin, u64 font_end) {
    FontEngineHold& h = t_font_hold;
    if (h.held) return; // pc unchanged: the real method runs
    // x29 is the caller's frame record: [fp] = its caller's fp, [fp+8] = its return address.
    u64 fp = c.x[29];
    if (!g_memory.IsMapped(fp, 16)) return;
    for (int depth = 0; depth < 8; ++depth) {
        const u64 next = g_memory.Read<u64>(fp);
        const u64 ret = g_memory.Read<u64>(fp + 8);
        if (ret < font_begin || ret >= font_end || !g_memory.IsMapped(next, 16)) break;
        fp = next;
    }
    if (!g_font_engine_lock.try_lock()) {
        const u64 n = g_font_engine_waits.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((n & (n - 1)) == 0) {
            LOG_INFO("font engine lock: thread %llu waits (%llu waits so far)",
                     static_cast<unsigned long long>(CurrentThread()->id), static_cast<unsigned long long>(n));
        }
        g_font_engine_lock.lock();
    }
    const u64 holds = g_font_engine_holds.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((holds & (holds - 1)) == 0 && holds >= 1024) {
        LOG_INFO("font engine lock: %llu holds, %llu waits", static_cast<unsigned long long>(holds),
                 static_cast<unsigned long long>(g_font_engine_waits.load(std::memory_order_relaxed)));
    }
    h.held = true;
    h.return_to = g_memory.Read<u64>(fp + 8);
    g_memory.Write<u64>(fp + 8, g_font_unlock);
}

void InstallFontEngineLock(const std::vector<LoadedModule>& loaded) {
    const char* e = std::getenv("NXRT_FONT_LOCK");
    if (e && e[0] == '0') return;
    const LoadedModule* main_module = nullptr;
    const LoadedModule* sdk_module = nullptr;
    for (const auto& m : loaded) {
        if (m.file == "main") main_module = &m;
        if (m.file == "sdk") sdk_module = &m;
    }
    if (!main_module || !sdk_module) return;
    const u64 font_begin = main_module->base + kFontFunctionsBegin;
    const u64 font_end = main_module->base + kFontFunctionsEnd;
    if (g_memory.Read<u32>(font_begin) != kFontFunctionsFirstWord) {
        LOG_INFO("font engine lock: nn::font code not where expected, not installed");
        return;
    }
    g_font_unlock = hle::MakeFakeFunction("nn::font engine unlock", [](GuestContext& c) {
        FontEngineHold& h = t_font_hold;
        h.held = false;
        c.x[30] = h.return_to;
        c.pc = h.return_to;
        g_font_engine_lock.unlock();
    });
    unsigned hooked = 0;
    for (const char* name : kFontEngineMethods) {
        const u64 method = FindExport(*sdk_module, name);
        if (!method) continue;
        hle::RegisterHook(method, name, [font_begin, font_end](GuestContext& c) {
            FontEngineEnter(c, font_begin, font_end);
        });
        ++hooked;
    }
    LOG_INFO("font engine lock: %u ScalableFontEngine methods hooked", hooked);
}
} // namespace

// Runs guest code until the thread exits or, if stop_pc is nonzero, until it
// reaches stop_pc. Re-entered by CallGuest from host functions.
static void RunUntil(KThread* t, u64 stop_pc, u64 bypass_pc = 0) {
    GuestContext& c = t->ctx;
    u64 hook_bypass_pc = bypass_pc; // CallGuest enters the real function, not its hook
    const RecompModule* m = nullptr;
    while (!t->exit_requested) {
        if (!m || c.pc < m->base || c.pc >= m->end) {
            if (stop_pc && c.pc == stop_pc) return;
            // Entering a module from elsewhere (or a fake address): host
            // functions get the first look. A host function that leaves pc
            // where it was asks to continue into the guest code there.
            const u64 entry_pc = c.pc;
            if (entry_pc != hook_bypass_pc && hle::DispatchHostCall(c)) {
                hook_bypass_pc = c.pc == entry_pc ? entry_pc : 0;
                m = nullptr;
                continue;
            }
            hook_bypass_pc = 0;
            m = FindModule(c.pc);
            if (!m) GuestCrash(t, "jump outside every module");
        }
        c.chain_budget = kChainBudget;
        const u64 pc_before = c.pc;
        m->run_slice(&c);

        if (c.halted) {
            switch (c.halted) {
            case kHaltIcIvau:
                c.pending_svc = kNoPendingSvc;
                c.pc += 4;
                c.halted = 0;
                continue;
            case kHaltBreakpoint:
                GuestCrash(t, "breakpoint (BRK)");
            case kHaltUnhandled:
                GuestCrash(t, "instruction the recompiler did not translate");
            default:
                GuestCrash(t, "halted");
            }
        }
        if (c.pending_svc != kNoPendingSvc) {
            const u32 imm = static_cast<u32>(c.pending_svc);
            c.pending_svc = kNoPendingSvc;
            const u64 svc_pc = c.pc;
            WatchCheck(t, imm, svc_pc, false);
            CallSvc(t, imm);
            WatchCheck(t, imm, svc_pc, true);
            continue;
        }
        // run_slice returns without progress only when the PC has no block.
        if (c.pc == pc_before && c.pc >= m->base && c.pc < m->end && !m->lookup(c.pc)) {
            GuestCrash(t, "no recompiled block for this address");
        }
    }
}

void RunGuest(KThread* t) {
    SetCurrentThread(t);
    t->ctx.fpcr = InitialFpcr();
    RunUntil(t, 0);
}

void CallGuestInPlace(u64 function) {
    KThread* t = CurrentThread();
    GuestContext& c = t->ctx;
    const u64 return_to = c.x[30];
    c.x[30] = kReturnTrap;
    c.pc = function;
    RunUntil(t, kReturnTrap, function);
    c.x[30] = return_to;
    c.pc = return_to;
}

u64 CallGuest(u64 function, std::initializer_list<u64> args) {
    KThread* t = CurrentThread();
    GuestContext& c = t->ctx;
    // Callee-saved state is preserved by the guest ABI; save what the call
    // clobbers so the interrupted host function sees its own registers again.
    u64 saved[32];
    std::memcpy(saved, c.x, sizeof(saved));
    const u64 saved_pc = c.pc;
    u64 saved_v[32][2];
    std::memcpy(saved_v, c.vreg, sizeof(saved_v));
    const u8 n = c.n, z = c.z, cf = c.c, v = c.v;
    int i = 0;
    for (u64 a : args) c.x[i++] = a;
    c.x[30] = kReturnTrap;
    c.pc = function;
    RunUntil(t, kReturnTrap, function);
    const u64 result = c.x[0];
    std::memcpy(c.x, saved, sizeof(saved));
    std::memcpy(c.vreg, saved_v, sizeof(saved_v));
    c.pc = saved_pc;
    c.n = n;
    c.z = z;
    c.c = cf;
    c.v = v;
    return result;
}

} // namespace nxrt
