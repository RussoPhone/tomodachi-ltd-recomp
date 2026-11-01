// SPDX-License-Identifier: GPL-3.0-or-later
// A small Horizon kernel for one process. Guest threads are host threads, one
// to one; there is no scheduler of our own. Everything the kernel owns - handle
// table, wait lists, arbiter queues - is guarded by one lock, and a blocked
// guest thread sleeps on its own condition variable under that lock.
#pragma once

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.h"
#include "guest_abi.h"

namespace nxrt {

class KThread;

class KObject : public std::enable_shared_from_this<KObject> {
public:
    virtual ~KObject() = default;
    virtual const char* TypeName() const = 0;
};

// Something WaitSynchronization can wait on.
class KSyncObject : public KObject {
public:
    // Both called with the kernel lock held.
    virtual bool IsSignaled() const = 0;
    void NotifyAvailable();

    std::vector<KThread*> waiters;
};

// An event as user mode sees it: one object, signalled until reset. Services
// hand out copies of it as the "readable event" handle.
class KEvent final : public KSyncObject {
public:
    explicit KEvent(std::string name = {}) : name(std::move(name)) {}
    const char* TypeName() const override { return "KEvent"; }
    bool IsSignaled() const override { return signaled; }
    void Signal();  // takes the kernel lock
    void Clear();   // takes the kernel lock

    std::string name;
    bool signaled = false;
};

// Memory the guest lends to a service (audio work buffers, applet storage...).
// Services run in this process, so they simply use the guest address.
class KTransferMemory final : public KObject {
public:
    const char* TypeName() const override { return "KTransferMemory"; }
    u64 address = 0;
    u64 size = 0;
};

class KThread final : public KSyncObject {
public:
    const char* TypeName() const override { return "KThread"; }
    bool IsSignaled() const override { return exited; }

    GuestContext ctx{};
    u64 id = 0;
    u32 handle = 0;     // this thread's own handle, as the guest knows it
    s32 priority = 44;
    s32 ideal_core = 0;
    u64 affinity_mask = 1;
    u64 tls_address = 0;
    u64 entry = 0;
    u64 arg0 = 0;     // initial x0 (debug dumps)
    u64 stack_top = 0;
    bool started = false;
    bool exited = false;
    bool exit_requested = false;
    std::thread host;

    // Wait state, all under the kernel lock.
    std::condition_variable cv;
    bool wait_done = false;
    u32 wait_result = 0;
    s32 wait_index = -1;                   // which object woke a WaitSynchronization
    std::vector<KSyncObject*> waiting_on;  // objects we are registered with
    std::string wait_what;                 // for thread dumps

    // Arbitration (mutexes, condition variables, address arbiter).
    u64 mutex_wait_address = 0;  // the mutex word this thread waits to own
    u32 address_key_value = 0;   // the tag it will write when it gets it
    u64 cv_key = 0;              // condition variable it waits on, 0 if none
    u64 arbiter_address = 0;     // WaitForAddress target, 0 if none
    KThread* lock_owner = nullptr;
    std::vector<KThread*> mutex_waiters; // threads waiting for a mutex we hold

    // NXRT_TRACE_TTS only: most recent SVC of this thread (written by the thread, read by the reporter).
    std::atomic<u64> trace_svc_start_ns{0}, trace_svc_end_ns{0}, trace_svc_count{0};
    std::atomic<u32> trace_svc_imm{0};
    std::atomic<s64> trace_svc_timeout{-1};  // ns, -1 = none/infinite
    std::atomic<u64> trace_svc_addr{0};      // wait address / sync key, 0 if none

    // Core baton (NXRT_CORE_BATON=1), see kernel.cpp. Touched only by this thread.
    u64 wake_ns = 0;  // NXRT_SYNC_STATS: when Wake() was called, under the kernel lock
    bool baton_held = false;
    s32 baton_core = -1;   // core whose baton we hold or wait for, -1 if none
    s32 baton_prio = 0;    // priority snapshot used for queue order
};

class KHandleTable {
public:
    static constexpr u32 kPseudoCurrentThread = 0xFFFF8000;
    static constexpr u32 kPseudoCurrentProcess = 0xFFFF8001;

    u32 Add(std::shared_ptr<KObject> object);
    std::shared_ptr<KObject> Get(u32 handle) const;
    template <typename T>
    std::shared_ptr<T> GetAs(u32 handle) const {
        return std::dynamic_pointer_cast<T>(Get(handle));
    }
    bool Close(u32 handle);

private:
    struct Entry {
        std::shared_ptr<KObject> object;
        u16 serial = 0;
    };
    std::vector<Entry> entries_;
    std::vector<u32> free_;
    u16 next_serial_ = 1;
};

// NXRT_SYNC_STATS=1: per-SVC-kind counters (count / mean / max latency), kernel lock wait and
// hold times and wake latency, printed every 5 s. Costs nothing when unset.
namespace syncstat {
enum Id : int { WaitAddr, SigAddr, Sleep0, SleepN, Lock, Unlock, WaitCv, SigCv, WaitSync, LockWait, LockHold,
                WakeLat, SleepReq, WaitTimedReq, WaitTimedOut, kCount };
extern bool g_on;
inline bool Enabled() { return g_on; }
u64 NowNs();
void Add(Id id, u64 ns);
void Init();  // reads the environment, starts the reporter
}

struct Kernel {
    std::mutex lock;
    KHandleTable handles;
    std::vector<std::shared_ptr<KThread>> threads;
    std::atomic<u64> next_thread_id{0x50};
    u64 program_id = 0;
    u64 heap_size = 0;
    // Condition variable and address arbiter waiters, in priority order.
    std::vector<KThread*> cv_waiters;
    std::vector<KThread*> arbiter_waiters;
    // Bump allocator for thread-local regions (0x200 bytes each).
    u64 tls_next = 0;
    u64 tls_page_end = 0;
};

extern Kernel g_kernel;
KThread* CurrentThread();
void SetCurrentThread(KThread* t);

// Blocks the current thread (kernel lock held through `lock`) until woken or
// the timeout expires. timeout_ns < 0 waits forever. Returns the wake result,
// or rc::TimedOut.
u32 WaitCurrent(std::unique_lock<std::mutex>& lock, s64 timeout_ns);
// Wakes `t` with `result`; kernel lock held.
void Wake(KThread* t, u32 result);

u64 AllocateTls();
std::shared_ptr<KThread> CreateThread(u64 entry, u64 arg, u64 stack_top, s32 priority, s32 core);
void StartThread(const std::shared_ptr<KThread>& thread);
// Marks the current thread exited; its dispatcher loop returns afterwards.
void ExitCurrentThread();

// Core baton: with NXRT_CORE_BATON=1 guest threads assigned to the same virtual core
// (0-2) run one at a time, like on Horizon. Priorities 59+ and core 3 are exempt.
// Everything below is a no-op when the variable is unset. None of these may be
// called with the kernel lock held, except BatonRelease.
bool CoreBatonEnabled();
// The core GetCurrentProcessorNumber reports for `t` (ideal core, else lowest bit of the mask).
s32 EffectiveCore(const KThread* t);
// Brings the baton in line with the thread's current core/priority; blocks until owned.
void BatonSync(KThread* t);
// Gives the baton away (a wait or exit); safe to call under the kernel lock and when not held.
void BatonRelease(KThread* t);
// Takes back the baton of the core it released (after a wait).
void BatonAcquire(KThread* t);
// Yield: pass the baton to the first waiter with the same priority (or any, if to_lower).
void BatonYield(KThread* t, bool to_lower);
// Preemption point: pass the baton to a waiter of strictly higher priority, if any.
// Called at every SVC return; a recompiler slice boundary could call it too.
void BatonPreemptPoint(KThread* t);

// Prints every guest thread and what it waits for (SIGUSR1).
void DumpThreads();

// Runs guest code on the calling host thread until the thread exits.
void RunGuest(KThread* t);

} // namespace nxrt
