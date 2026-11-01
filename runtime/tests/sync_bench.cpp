// SPDX-License-Identifier: GPL-3.0-or-later
// Address-arbiter ping-pong between two guest threads (WaitForAddress/SignalToAddress through
// CallSvc) against a plain Linux futex ping-pong. Usage: nxrt_sync_bench [N] [mode: svc|futex|both]
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "kernel/kernel.h"
#include "kernel/svc.h"
#include "memory.h"

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace nxrt;

namespace {
constexpr u64 kBase = 0x100000;
using Clock = std::chrono::steady_clock;

u32 Svc(KThread* t, u32 imm, u64 a, u64 b, u64 c, u64 d) {
    t->ctx.x[0] = a; t->ctx.x[1] = b; t->ctx.x[2] = c; t->ctx.x[3] = d;
    CallSvc(t, imm);
    return static_cast<u32>(t->ctx.x[0]);
}

void Side(KThread* t, u64 mine, u64 other, int n, bool first) {
    SetCurrentThread(t);
    auto* wm = reinterpret_cast<std::atomic<u32>*>(g_memory.Ptr(mine, 4));
    auto* wo = reinterpret_cast<std::atomic<u32>*>(g_memory.Ptr(other, 4));
    for (int i = 0; i < n; ++i) {
        if (!first || i > 0) {
            // wait for my word to reach i+1 (WaitIfEqual on the previous value)
            while (wm->load() != static_cast<u32>(i + (first ? 0 : 1))) {
                Svc(t, 0x34, mine, 2, wm->load(), static_cast<u64>(-1));
            }
        }
        wo->store(static_cast<u32>(i + 1));
        Svc(t, 0x35, other, 0, 0, 1);
    }
}

double RunSvc(int n) {
    auto a = std::make_shared<KThread>(); a->id = 1;
    auto b = std::make_shared<KThread>(); b->id = 2;
    auto* wa = reinterpret_cast<std::atomic<u32>*>(g_memory.Ptr(kBase, 4)); wa->store(0);
    auto* wb = reinterpret_cast<std::atomic<u32>*>(g_memory.Ptr(kBase + 64, 4)); wb->store(0);
    const auto t0 = Clock::now();
    std::thread ta(Side, a.get(), kBase, kBase + 64, n, true);
    std::thread tb(Side, b.get(), kBase + 64, kBase, n, false);
    ta.join(); tb.join();
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

#if defined(__linux__)
long Futex(std::atomic<u32>* w, int op, u32 v) {
    return syscall(SYS_futex, reinterpret_cast<u32*>(w), op | FUTEX_PRIVATE_FLAG, v, nullptr, nullptr, 0);
}
void FSide(std::atomic<u32>* mine, std::atomic<u32>* other, int n, bool first) {
    for (int i = 0; i < n; ++i) {
        if (!first || i > 0) {
            for (;;) {
                const u32 v = mine->load();
                if (v == static_cast<u32>(i + (first ? 0 : 1))) break;
                Futex(mine, FUTEX_WAIT, v);
            }
        }
        other->store(static_cast<u32>(i + 1));
        Futex(other, FUTEX_WAKE, 1);
    }
}
double RunFutex(int n) {
    alignas(64) std::atomic<u32> wa{0};
    alignas(64) std::atomic<u32> wb{0};
    const auto t0 = Clock::now();
    std::thread ta(FSide, &wa, &wb, n, true);
    std::thread tb(FSide, &wb, &wa, n, false);
    ta.join(); tb.join();
    return std::chrono::duration<double>(Clock::now() - t0).count();
}
#endif
} // namespace

int main(int argc, char** argv) {
    const int n = argc > 1 ? std::atoi(argv[1]) : 200000;
    const char* mode = argc > 2 ? argv[2] : "both";
    if (!g_memory.Init() || !g_memory.Map(kBase, 0x1000, MemState::Normal, PermRW)) return 1;
    // one exchange = one signal + one wake of the peer (n exchanges per side, 2n one-way hops)
    if (!std::strcmp(mode, "svc") || !std::strcmp(mode, "both")) {
        const double s = RunSvc(n);
        std::printf("svc   : %d round trips in %.3f s -> %.2f us/hop\n", n, s, s * 1e6 / (2.0 * n));
    }
#if defined(__linux__)
    if (!std::strcmp(mode, "futex") || !std::strcmp(mode, "both")) {
        const double s = RunFutex(n);
        std::printf("futex : %d round trips in %.3f s -> %.2f us/hop\n", n, s, s * 1e6 / (2.0 * n));
    }
#endif
    return 0;
}
