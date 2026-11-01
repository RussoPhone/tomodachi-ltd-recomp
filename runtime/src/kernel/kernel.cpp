// SPDX-License-Identifier: GPL-3.0-or-later
#include "kernel/kernel.h"

#if defined(_WIN32) && !defined(__MINGW32__)
// MSVC/clang-cl: no <pthread.h>; threads come from the Win32 API.
#include <windows.h>

#include <cwchar>
#else
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdio>
#include <map>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "cpu.h"
#include "memory.h"

namespace nxrt {

Kernel g_kernel;

namespace {
thread_local KThread* t_current = nullptr;
// Guest blocks can chain direct calls on the host stack; give them room.
constexpr size_t kHostStackSize = 64ull << 20;
} // namespace

KThread* CurrentThread() {
    return t_current;
}

// ---------------------------------------------------------------- core baton

namespace {
constexpr int kBatonCores = 3;     // application cores; core 3 belongs to the system
constexpr s32 kBatonMaxPrio = 59;  // 59-63 are not scheduled preemptively on Horizon

struct CoreBaton {
    std::mutex m;  // leaf lock: never take another lock while holding it
    std::condition_variable cv;
    KThread* owner = nullptr;
    std::vector<KThread*> waiters;       // by baton_prio, FIFO within a priority
    std::atomic<s32> best{INT_MAX};      // priority of waiters.front(), for the SVC fast path
};
CoreBaton g_batons[kBatonCores];

void UpdateBest(CoreBaton& b) {
    b.best.store(b.waiters.empty() ? INT_MAX : b.waiters.front()->baton_prio, std::memory_order_relaxed);
}

// Queues `t` and sleeps until the baton is handed to it. `front` puts it ahead of
// its own priority class (a preempted thread keeps its turn).
void BatonEnqueueAndWait(CoreBaton& b, std::unique_lock<std::mutex>& lk, KThread* t, bool front) {
    t->baton_prio = t->priority;
    auto it = std::find_if(b.waiters.begin(), b.waiters.end(), [&](const KThread* o) {
        return front ? o->baton_prio >= t->baton_prio : o->baton_prio > t->baton_prio;
    });
    b.waiters.insert(it, t);
    UpdateBest(b);
    bool warned = false;
    while (b.owner != t) {
        if (b.cv.wait_for(lk, std::chrono::seconds(10), [&] { return b.owner == t; })) break;
        if (!warned) {
            warned = true;
            LOG_WARN("core baton %d: thread %llu waits >10 s, owner thread %llu (pc %s)", t->baton_core,
                     static_cast<unsigned long long>(t->id),
                     static_cast<unsigned long long>(b.owner ? b.owner->id : 0),
                     b.owner ? Symbolize(b.owner->ctx.pc).c_str() : "-");
        }
    }
    t->baton_held = true;
}

// Hands the baton to the first waiter, or frees it. Lock held.
void BatonPassOn(CoreBaton& b) {
    if (b.waiters.empty()) {
        b.owner = nullptr;
    } else {
        b.owner = b.waiters.front();
        b.waiters.erase(b.waiters.begin());
        b.cv.notify_all();
    }
    UpdateBest(b);
}

s32 WantedBatonCore(const KThread* t) {
    if (t->priority >= kBatonMaxPrio) return -1;
    const s32 core = EffectiveCore(t);
    return core >= 0 && core < kBatonCores ? core : -1;
}
} // namespace

bool CoreBatonEnabled() {
    static const bool on = [] {
        const char* e = std::getenv("NXRT_CORE_BATON");
        const bool v = e && *e == '1';
        if (v) LOG_INFO("NXRT_CORE_BATON=1: guest threads of one core run one at a time");
        return v;
    }();
    return on;
}

s32 EffectiveCore(const KThread* t) {
    if (t->ideal_core >= 0 && t->ideal_core < 64 && (t->affinity_mask >> t->ideal_core & 1)) {
        return t->ideal_core;
    }
    if (t->affinity_mask) return static_cast<s32>(__builtin_ctzll(t->affinity_mask));
    return 0;
}

void BatonAcquire(KThread* t) {
    if (!CoreBatonEnabled() || t->baton_held || t->baton_core < 0) return;
    CoreBaton& b = g_batons[t->baton_core];
    std::unique_lock lk(b.m);
    if (!b.owner) {
        b.owner = t;
        t->baton_held = true;
        return;
    }
    BatonEnqueueAndWait(b, lk, t, false);
}

void BatonRelease(KThread* t) {
    if (!t->baton_held) return;
    t->baton_held = false;
    CoreBaton& b = g_batons[t->baton_core];
    std::lock_guard lk(b.m);
    BatonPassOn(b);
}

void BatonSync(KThread* t) {
    if (!CoreBatonEnabled()) return;
    const s32 want = WantedBatonCore(t);
    if (t->baton_held && t->baton_core != want) BatonRelease(t);
    if (!t->baton_held && want >= 0) {
        t->baton_core = want;
        BatonAcquire(t);
    }
}

void BatonYield(KThread* t, bool to_lower) {
    if (!CoreBatonEnabled() || !t->baton_held) return;
    CoreBaton& b = g_batons[t->baton_core];
    if (b.best.load(std::memory_order_relaxed) == INT_MAX) return;
    std::unique_lock lk(b.m);
    if (b.waiters.empty()) return;
    if (!to_lower && b.waiters.front()->baton_prio > t->priority) return;
    t->baton_held = false;
    BatonPassOn(b);
    BatonEnqueueAndWait(b, lk, t, false);
}

void BatonPreemptPoint(KThread* t) {
    if (!t->baton_held) return;
    CoreBaton& b = g_batons[t->baton_core];
    if (b.best.load(std::memory_order_relaxed) >= t->priority) return;
    std::unique_lock lk(b.m);
    if (b.waiters.empty() || b.waiters.front()->baton_prio >= t->priority) return;
    t->baton_held = false;
    BatonPassOn(b);
    BatonEnqueueAndWait(b, lk, t, true);
}

// ---------------------------------------------------------------- handles

u32 KHandleTable::Add(std::shared_ptr<KObject> object) {
    u32 index;
    if (!free_.empty()) {
        index = free_.back();
        free_.pop_back();
    } else {
        index = static_cast<u32>(entries_.size());
        if (index >= 0x8000) return 0;
        entries_.emplace_back();
    }
    const u16 serial = next_serial_;
    next_serial_ = next_serial_ == 0x3FFF ? 1 : next_serial_ + 1;
    entries_[index] = {std::move(object), serial};
    return index | (static_cast<u32>(serial) << 15);
}

std::shared_ptr<KObject> KHandleTable::Get(u32 handle) const {
    if (handle == kPseudoCurrentThread) {
        KThread* t = CurrentThread();
        return t ? t->shared_from_this() : nullptr;
    }
    const u32 index = handle & 0x7FFF;
    const u16 serial = static_cast<u16>((handle >> 15) & 0x3FFF);
    if (index >= entries_.size() || entries_[index].serial != serial) return nullptr;
    return entries_[index].object;
}

bool KHandleTable::Close(u32 handle) {
    const u32 index = handle & 0x7FFF;
    const u16 serial = static_cast<u16>((handle >> 15) & 0x3FFF);
    if (index >= entries_.size() || entries_[index].serial != serial || !entries_[index].object) {
        return false;
    }
    entries_[index] = {};
    free_.push_back(index);
    return true;
}

// ---------------------------------------------------------------- sync stats

namespace syncstat {
bool g_on = false;
namespace {
struct Stat {
    std::atomic<u64> n{0}, sum{0}, max{0};
};
Stat g_stat[kCount];
const char* const kNames[kCount] = {"WaitForAddress", "SignalToAddress", "Sleep(<=0)", "Sleep(>0)", "ArbitrateLock",
                                    "ArbitrateUnlock", "WaitProcessWideKey", "SignalProcessWideKey",
                                    "WaitSynchronization", "klock-wait", "klock-hold", "wake-latency", "sleep-requested", "timed-wait-requested", "timed-wait-expired"};
}  // namespace

u64 NowNs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

void Add(Id id, u64 ns) {
    Stat& s = g_stat[id];
    s.n.fetch_add(1, std::memory_order_relaxed);
    s.sum.fetch_add(ns, std::memory_order_relaxed);
    u64 m = s.max.load(std::memory_order_relaxed);
    while (ns > m && !s.max.compare_exchange_weak(m, ns, std::memory_order_relaxed)) {}
}

void Init() {
    const char* e = std::getenv("NXRT_SYNC_STATS");
    if (!e || *e != '1') return;
    g_on = true;
    std::thread([] {
        u64 last_n[kCount] = {}, last_sum[kCount] = {};
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            for (int i = 0; i < kCount; ++i) {
                Stat& s = g_stat[i];
                const u64 n = s.n.load(), sum = s.sum.load(), mx = s.max.exchange(0);
                const u64 dn = n - last_n[i], dsum = sum - last_sum[i];
                last_n[i] = n;
                last_sum[i] = sum;
                if (dn) {
                    LOG_INFO("syncstat %-20s n=%7llu (%6.0f/s) mean=%9.2f us max=%10.2f us total=%8.3f s", kNames[i],
                             static_cast<unsigned long long>(dn), dn / 5.0, dsum / 1000.0 / dn, mx / 1000.0,
                             dsum / 1e9);
                }
            }
            // Where the guest threads run and what they cost: /proc/<pid>/task/*/stat.
            struct Row { std::string name; u64 ticks; int cpu; char state; };
            std::vector<Row> rows;
            u64 mask_seen = 0;
            if (FILE* d = popen(("cat /proc/" + std::to_string(getpid()) + "/task/*/stat 2>/dev/null").c_str(), "r")) {
                char line[1024];
                while (std::fgets(line, sizeof line, d)) {
                    const char* lp = std::strchr(line, '(');
                    const char* rp = std::strrchr(line, ')');
                    if (!lp || !rp) continue;
                    Row r{std::string(lp + 1, rp) + "/" + std::to_string(std::atol(line)), 0, -1, 0};
                    int field = 3;
                    u64 ut = 0, st = 0;
                    const char* q = rp + 1;
                    while (*q) {
                        while (*q == ' ') ++q;
                        if (!*q) break;
                        if (field == 3) r.state = *q;
                        if (field == 14) ut = std::strtoull(q, nullptr, 10);
                        if (field == 15) st = std::strtoull(q, nullptr, 10);
                        if (field == 39) r.cpu = std::atoi(q);
                        while (*q && *q != ' ') ++q;
                        ++field;
                    }
                    r.ticks = ut + st;
                    if (r.state == 'R' && r.cpu >= 0 && r.cpu < 64) mask_seen |= u64{1} << r.cpu;
                    rows.push_back(std::move(r));
                }
                pclose(d);
            }
            static std::map<std::string, u64> prev;
            std::vector<std::pair<u64, std::string>> top;
            for (const Row& r : rows) {
                const u64 dt = r.ticks - prev[r.name];
                prev[r.name] = r.ticks;
                if (dt) top.emplace_back(dt, r.name);
            }
            std::sort(top.rbegin(), top.rend());
            std::string txt;
            for (size_t i = 0; i < top.size() && i < 8; ++i) {
                txt += top[i].second + "=" + std::to_string(top[i].first * 100 / 500) + "% ";  // 100 ticks/s * 5 s
            }
            LOG_INFO("syncstat host threads=%zu, cpus with a running thread now: %d (mask %llx); cpu%%: %s", rows.size(),
                     __builtin_popcountll(mask_seen), static_cast<unsigned long long>(mask_seen), txt.c_str());
        }
    }).detach();
}
}  // namespace syncstat

// ---------------------------------------------------------------- waiting

void Wake(KThread* t, u32 result) {
    if (syncstat::g_on) t->wake_ns = syncstat::NowNs();
    t->wait_result = result;
    t->wait_done = true;
    t->cv.notify_one();
}

u32 WaitCurrent(std::unique_lock<std::mutex>& lock, s64 timeout_ns) {
    KThread* t = CurrentThread();
    struct Clear {
        KThread* t;
        ~Clear() { t->wait_what.clear(); }
    } clear{t};
    // A blocked thread does not own its core; it queues for it again once woken.
    const bool had_baton = t->baton_held;
    if (had_baton) BatonRelease(t);
    bool timed_out = false;
    if (timeout_ns < 0) {
        t->cv.wait(lock, [t] { return t->wait_done; });
    } else {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns);
        timed_out = !t->cv.wait_until(lock, deadline, [t] { return t->wait_done; });
    }
    if (syncstat::g_on && !timed_out && t->wake_ns) {
        syncstat::Add(syncstat::WakeLat, syncstat::NowNs() - t->wake_ns);
        t->wake_ns = 0;
    }
    if (had_baton) {
        lock.unlock(); // the baton is never requested with the kernel lock held
        BatonAcquire(t);
        lock.lock();
        // A wake that raced with the timeout while we queued for the baton counts.
        if (t->wait_done) timed_out = false;
    }
    return timed_out ? rc::TimedOut : t->wait_result;
}

void KSyncObject::NotifyAvailable() {
    if (!IsSignaled()) return;
    for (KThread* t : waiters) {
        if (t->wait_done) continue;
        const auto it = std::find(t->waiting_on.begin(), t->waiting_on.end(), this);
        t->wait_index = static_cast<s32>(it - t->waiting_on.begin());
        Wake(t, rc::Success);
    }
}

void KEvent::Signal() {
    std::lock_guard lock(g_kernel.lock);
    signaled = true;
    NotifyAvailable();
}

void KEvent::Clear() {
    std::lock_guard lock(g_kernel.lock);
    signaled = false;
}

// ---------------------------------------------------------------- threads

u64 AllocateTls() {
    // Called with the kernel lock held.
    constexpr u64 kTlsSize = 0x200;
    if (g_kernel.tls_next + kTlsSize > g_kernel.tls_page_end) {
        static u64 next_page = AddressSpaceLayout::kKernelMapStart;
        g_memory.Map(next_page, kPageSize, MemState::ThreadLocal, PermRW);
        g_kernel.tls_next = next_page;
        g_kernel.tls_page_end = next_page + kPageSize;
        next_page += kPageSize;
    }
    const u64 tls = g_kernel.tls_next;
    g_kernel.tls_next += kTlsSize;
    g_memory.Zero(tls, kTlsSize);
    return tls;
}

std::shared_ptr<KThread> CreateThread(u64 entry, u64 arg, u64 stack_top, s32 priority, s32 core) {
    // Called with the kernel lock held.
    auto t = std::make_shared<KThread>();
    t->id = g_kernel.next_thread_id++;
    t->priority = priority;
    t->ideal_core = core;
    t->affinity_mask = u64{1} << (core < 0 ? 0 : core);
    t->entry = entry;
    t->arg0 = arg;
    t->stack_top = stack_top;
    t->tls_address = AllocateTls();
    GuestContext& c = t->ctx;
    c.x[0] = arg;
    c.x[18] = 0x1234'5678'9abc'def1ULL; // platform register: random, odd
    c.x[31] = stack_top;
    c.pc = entry;
    c.pending_svc = kNoPendingSvc;
    c.tpidrro_el0 = t->tls_address;
    c.host_mem = &g_memory.HostMem();
    c.fm_table = static_cast<const unsigned char*>(g_memory.PageTable());
    c.fm_limit = AddressSpaceLayout::kEnd;
    t->handle = g_kernel.handles.Add(t);
    g_kernel.threads.push_back(t);
    return t;
}

#if defined(_WIN32) && !defined(__MINGW32__)
namespace {
DWORD WINAPI HostThreadEntry(LPVOID arg) {
    auto* t = static_cast<KThread*>(arg);
    RunGuest(t);
    BatonRelease(t);
    return 0;
}
} // namespace

void StartThread(const std::shared_ptr<KThread>& thread) {
    thread->started = true;
    // The size is a reserve, not a commit, like the 64 MiB pthread stack.
    HANDLE h = CreateThread(nullptr, kHostStackSize, HostThreadEntry, thread.get(),
                            STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) {
        Fatal("could not start a host thread for guest thread %llu",
              static_cast<unsigned long long>(thread->id));
    }
    // SetThreadDescription needs Windows 10 1607; skip the name when missing.
    using SetDescFn = HRESULT(WINAPI*)(HANDLE, PCWSTR);
    static const auto set_desc = reinterpret_cast<SetDescFn>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription")));
    if (set_desc) {
        wchar_t name[16];
        std::swprintf(name, 16, L"guest-%llu", static_cast<unsigned long long>(thread->id));
        set_desc(h, name);
    }
    CloseHandle(h); // detached
}
#else
namespace {
void* HostThreadEntry(void* arg) {
    auto* t = static_cast<KThread*>(arg);
#if defined(__linux__)
    // The default 50 us timer slack turns a guest SleepThread(20 us) into ~70 us.
    static const unsigned long slack = [] {
        const char* e = std::getenv("NXRT_TIMER_SLACK_NS");
        return e ? std::strtoul(e, nullptr, 10) : 0ul;
    }();
    if (slack) prctl(PR_SET_TIMERSLACK, slack, 0, 0, 0);
#endif
    RunGuest(t);
    BatonRelease(t);
    return nullptr;
}
} // namespace

void StartThread(const std::shared_ptr<KThread>& thread) {
    thread->started = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kHostStackSize);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t tid;
    if (pthread_create(&tid, &attr, HostThreadEntry, thread.get()) != 0) {
        Fatal("could not start a host thread for guest thread %llu",
              static_cast<unsigned long long>(thread->id));
    }
    pthread_attr_destroy(&attr);
    char name[16];
    std::snprintf(name, sizeof(name), "guest-%llu", static_cast<unsigned long long>(thread->id));
    pthread_setname_np(tid, name);
}
#endif

void ExitCurrentThread() {
    KThread* t = CurrentThread();
    std::lock_guard lock(g_kernel.lock);
    t->exited = true;
    t->exit_requested = true;
    t->NotifyAvailable();
    BatonRelease(t);
}

void DumpThreads() {
    // Debug aid: reads thread state without the kernel lock.
    for (const auto& t : g_kernel.threads) {
        LOG_INFO("thread %llu prio %d core %d %s pc %s lr %s %s", static_cast<unsigned long long>(t->id), t->priority,
                 EffectiveCore(t.get()), t->exited ? "exited" : (t->started ? "running" : "created"), Symbolize(t->ctx.pc).c_str(),
                 Symbolize(t->ctx.x[30]).c_str(), t->wait_what.empty() ? "" : ("waits: " + t->wait_what).c_str());
        // A thread not in a wait is running guest code: its frame chain says what it is doing.
        static const bool all_bt = std::getenv("NXRT_DUMP_THREADS_BT") != nullptr;
        if (t->started && !t->exited && (all_bt || t->wait_what.empty())) LOG_INFO("  backtrace %s", Backtrace(t->ctx).c_str());
        if (all_bt && t->started && !t->exited && t->arg0) {
            // nn::os::ThreadType holds the real thread function: show every code pointer in its head.
            std::string fns;
            for (u64 off = 0; off < 0x100; off += 8) {
                const u64 v = g_memory.Read<u64>(t->arg0 + off);
                const std::string sym = Symbolize(v);
                if (!sym.empty() && (sym.rfind("main+", 0) == 0 || sym.rfind("sdk+", 0) == 0))
                    fns += " +" + std::to_string(off) + "=" + sym;
            }
            LOG_INFO("  threadtype@%llx%s", static_cast<unsigned long long>(t->arg0), fns.c_str());
        }
    }
}

void SetCurrentThread(KThread* t) {
    t_current = t;
}

} // namespace nxrt
