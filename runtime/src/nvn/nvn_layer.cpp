// SPDX-License-Identifier: GPL-3.0-or-later
// The NVN layer. NVN is the Switch's graphics API; the game reaches it only
// through function pointers handed out by nvnBootstrapLoader and
// nvnDeviceGetProcAddress. Intercepting those two gives the runtime every NVN
// call by name. For now each call is counted and forwarded to the game's own
// driver; the renderer replaces the forwarding one function family at a time.
#include "nvn/nvn_layer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "cpu.h"
#include "hle/hooks.h"
#include "memory.h"

namespace nxrt::nvn {

namespace {

struct Handler {
    Observer on_call, on_return;
};

struct Proc {
    std::string name;
    u64 real = 0;
    u64 wrapper = 0;
    const Handler* handler = nullptr;
    bool cmdbuf = false; // nvnCommandBuffer*: x0 is the command buffer
    std::atomic<u64> calls{0};
    std::atomic<u64> host_ns{0}; // NXRT_NVN_TIME=1: time spent in our hooks and handlers (including waiting for the model lock)
};

std::function<void(u64, const std::string&)> g_cb_before;
std::function<void(u64)> g_cb_after;

std::map<std::string, Handler>& Handlers() {
    static std::map<std::string, Handler> h;
    return h;
}

// NXRT_NVN_TIME=1 (diagnostic): per NVN function, the wall time our side spends before handing the call to the driver; the top
// of the table is logged every 5 s as "nvntime".
struct ForwardTimer {
    Proc* p;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ~ForwardTimer() {
        p->host_ns.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                  std::chrono::steady_clock::now() - start).count()),
                             std::memory_order_relaxed);
    }
};
void ForwardImpl(Proc* p, GuestContext& c);
void Forward(Proc* p, GuestContext& c) {
    static const bool timed = std::getenv("NXRT_NVN_TIME") != nullptr;
    if (!timed) return ForwardImpl(p, c);
    ForwardTimer timer{p};
    ForwardImpl(p, c);
}

void ForwardImpl(Proc* p, GuestContext& c) {
    p->calls.fetch_add(1, std::memory_order_relaxed);
    static const u64 trace_cmd = std::getenv("NXRT_TRACE_CMD") ? std::strtoull(std::getenv("NXRT_TRACE_CMD"), nullptr, 16) : 0;
    static const bool trace_draws = std::getenv("NXRT_TRACE_DRAWS") != nullptr;
    if (trace_draws && (p->name.find("Draw") != std::string::npos || p->name.find("Dispatch") != std::string::npos ||
                        p->name.find("Copy") != std::string::npos || p->name.find("Blit") != std::string::npos)) {
        static std::atomic<int> shown{0};
        const int n = shown.fetch_add(1);
        if (n < 200000) LOG_INFO("trace-draw: %s(cmd %llx, %llx, %llx, %llx) lr=%llx", p->name.c_str(),
                                             (unsigned long long)c.x[0], (unsigned long long)c.x[1],
                                             (unsigned long long)c.x[2], (unsigned long long)c.x[3],
                                             (unsigned long long)c.x[30]);
    }
    // NXRT_TRACE_COMPUTE=1: once a command buffer dispatched compute, log every later call on it, so the
    // program/texture/image/storage bindings that feed each dispatch are visible (the renderer drops dispatches).
    static const bool trace_compute = std::getenv("NXRT_TRACE_COMPUTE") != nullptr;
    if (trace_compute && p->cmdbuf) {
        static std::mutex compute_mutex;
        static std::set<u64> dispatch_cmds;
        static u32 shown = 0;
        std::lock_guard lock(compute_mutex);
        if (p->name.find("DispatchCompute") != std::string::npos) dispatch_cmds.insert(c.x[0]);
        if (dispatch_cmds.count(c.x[0]) && shown < 20000) {
            ++shown;
            LOG_INFO("trace-compute: %s(cmd %llx, %llx, %llx, %llx, %llx) lr=%llx", p->name.c_str(),
                     (unsigned long long)c.x[0], (unsigned long long)c.x[1], (unsigned long long)c.x[2],
                     (unsigned long long)c.x[3], (unsigned long long)c.x[4], (unsigned long long)c.x[30]);
        }
    }
    if (trace_cmd && c.x[0] == trace_cmd) {
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1) < 200000) LOG_INFO("trace-cmd: %s(%llx, %llx, %llx) lr=%llx", p->name.c_str(),
                                               (unsigned long long)c.x[1], (unsigned long long)c.x[2],
                                               (unsigned long long)c.x[3], (unsigned long long)c.x[30]);
    }
    // The game writes Maxwell method words straight into command memory (main+0x2b70f0, guard read through the GOT
    // slot main+0x32d8a70) when this nnSdk flag is 1. The inline decoder misses the scene's mesh draws on that path
    // (K250: black 3D scene; with the flag cleared, sky, sea and depth passes render), so the flag is cleared by
    // default and the game takes the NVN calls the recorder hooks. NXRT_NVN_INLINE=1 keeps the inline path (A/B).
    static const bool no_inline = std::getenv("NXRT_NVN_INLINE") == nullptr;
    if (no_inline && p->name == "nvnCommandBufferBeginRecording") {
        const u64 ptr = g_memory.Read<u64>(0x80006000ull + 0x32d8a70ull);
        if (ptr) g_memory.Write<u32>(ptr, 0);
    }
    static const bool trace_bt = std::getenv("NXRT_TRACE_RT_BT") != nullptr;
    if (trace_bt && p->name == "nvnCommandBufferSetRenderTargets") {
        static std::atomic<int> shown{0};
        if (shown.fetch_add(1) < 400) {
            // Guest frame-pointer chain: [x29] = caller's x29, [x29+8] = return address.
            std::string bt;
            u64 fp = c.x[29];
            char tmp[64];
            std::snprintf(tmp, sizeof tmp, "lr=%llx", (unsigned long long)c.x[30]);
            bt += tmp;
            for (int i = 0; i < 8 && fp; ++i) {
                const u64 ret = g_memory.Read<u64>(fp + 8), next = g_memory.Read<u64>(fp);
                std::snprintf(tmp, sizeof tmp, " <- %llx", (unsigned long long)ret);
                bt += tmp;
                if (next <= fp) break;
                fp = next;
            }
            if (shown.load() <= 3) {
                // Static GOT slot main+0x32d8a70 -> bool read by main+0x2b70f0 (inline command-word writer guard).
                const u64 slot = 0x80006000ull + 0x32d8a70ull;
                const u64 ptr = g_memory.Read<u64>(slot);
                LOG_INFO("trace-rt-bt: inline-guard slot=%llx ptr=%llx value=%u", (unsigned long long)slot,
                         (unsigned long long)ptr, ptr ? g_memory.Read<u32>(ptr) : 0xdeadu);
            }
            LOG_INFO("trace-rt-bt: cmd %llx n=%llx: %s", (unsigned long long)c.x[0],
                     (unsigned long long)c.x[1], bt.c_str());
        }
    }
    const Handler* h = p->handler;
    const bool hooked = p->cmdbuf && g_cb_before;
    if (!h && !hooked) {
        c.pc = p->real; // tail call into the driver; x30 still returns to the game
        return;
    }
    const u64 cmd = c.x[0];
    if (hooked) g_cb_before(cmd, p->name);
    Call call;
    for (int i = 0; i < 8; ++i) {
        call.x[i] = c.x[i];
        call.v[i] = c.vreg[i][0];
    }
    if (h && h->on_call) h->on_call(call);
    if (!hooked && !h->on_return) {
        c.pc = p->real;
        return;
    }
    CallGuestInPlace(p->real);
    call.ret = c.x[0];
    if (h && h->on_return) h->on_return(call);
    if (hooked) g_cb_after(cmd);
}

std::mutex g_mutex;
std::map<u64, std::unique_ptr<Proc>> g_by_real; // real address -> wrapper
u64 g_real_get_proc_address = 0;
u64 g_driver_base = 0;

u64 Wrap(const std::string& name, u64 real);

void StartTimeReporter() {
    static const bool timed = std::getenv("NXRT_NVN_TIME") != nullptr;
    static std::once_flag once;
    if (!timed) return;
    std::call_once(once, [] {
        std::thread([] {
            std::map<const Proc*, std::pair<u64, u64>> last;  // calls, ns at the previous report
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                std::vector<std::tuple<u64, u64, std::string>> rows;  // ns, calls (this window), name
                u64 total = 0;
                {
                    std::lock_guard lock(g_mutex);
                    for (const auto& [real, p] : g_by_real) {
                        auto& prev = last[p.get()];
                        const u64 calls = p->calls.load(), ns = p->host_ns.load();
                        if (ns > prev.second) rows.emplace_back(ns - prev.second, calls - prev.first, p->name);
                        total += ns - prev.second;
                        prev = {calls, ns};
                    }
                }
                std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return std::get<0>(a) > std::get<0>(b); });
                LOG_INFO("nvntime: our side of the NVN calls took %.1f ms/s in total", total / 5e6);
                for (size_t i = 0; i < rows.size() && i < 12; ++i)
                    LOG_INFO("nvntime: %7.1f ms/s %8.0f calls/s %6.2f us/call  %s", std::get<0>(rows[i]) / 5e6,
                             std::get<1>(rows[i]) / 5.0, std::get<0>(rows[i]) / 1e3 / std::max<u64>(1, std::get<1>(rows[i])),
                             std::get<2>(rows[i]).c_str());
            }
        }).detach();
    });
}

void GetProcAddressHook(GuestContext& c) {
    const u64 device = c.x[0], name_ptr = c.x[1];
    const std::string name = g_memory.ReadString(name_ptr, 128);
    const u64 real = CallGuest(g_real_get_proc_address, {device, name_ptr});
    c.x[0] = real ? Wrap(name, real) : 0;
    hle::ReturnToCaller(c);
}

u64 Wrap(const std::string& name, u64 real) {
    std::lock_guard lock(g_mutex);
    auto& slot = g_by_real[real];
    if (slot) {
        if (slot->name != name) {
            LOG_INFO("nvn: %s resolves to the same driver address %llx as %s (calls will be attributed to the latter)",
                     name.c_str(), static_cast<unsigned long long>(real), slot->name.c_str());
        }
        return slot->wrapper;
    }
    slot = std::make_unique<Proc>();
    Proc* p = slot.get();
    StartTimeReporter();
    p->name = name;
    p->real = real;
    if (name == "nvnDeviceGetProcAddress") {
        g_real_get_proc_address = real;
        p->wrapper = hle::MakeFakeFunction(name, GetProcAddressHook);
    } else {
        p->cmdbuf = name.rfind("nvnCommandBuffer", 0) == 0;
        const auto h = Handlers().find(name);
        p->handler = h == Handlers().end() ? nullptr : &h->second;
        p->wrapper = hle::MakeFakeFunction(name, [p](GuestContext& c) { Forward(p, c); });
    }
    return p->wrapper;
}

} // namespace

float Call::F(int i) const {
    float f;
    const u32 bits = static_cast<u32>(v[i]);
    std::memcpy(&f, &bits, 4);
    return f;
}

void Observe(const std::string& name, Observer on_call, Observer on_return) {
    Handlers()[name] = {std::move(on_call), std::move(on_return)};
}

void SetCommandBufferHooks(std::function<void(u64, const std::string&)> before, std::function<void(u64)> after) {
    g_cb_before = std::move(before);
    g_cb_after = std::move(after);
}

bool InstallLayer(const std::vector<LoadedModule>& modules) {
    for (const auto& m : modules) {
        const u64 bootstrap = FindExport(m, "nvnBootstrapLoader");
        if (!bootstrap) continue;
        LOG_INFO("NVN: nvnBootstrapLoader at %s", Symbolize(bootstrap).c_str());
        g_driver_base = m.base;
        hle::RegisterHook(bootstrap, "nvnBootstrapLoader", [bootstrap](GuestContext& c) {
            const u64 name_ptr = c.x[0];
            const std::string name = g_memory.ReadString(name_ptr, 128);
            const u64 real = CallGuest(bootstrap, {name_ptr});
            c.x[0] = real ? Wrap(name, real) : 0;
            hle::ReturnToCaller(c);
        });
        return true;
    }
    LOG_WARN("NVN: no nvnBootstrapLoader export found");
    return false;
}

u64 DriverModuleBase() {
    return g_driver_base;
}

void RestoreDriverModuleBaseForReplay(u64 base) {
    g_driver_base = base;
}

void DumpStats() {
    std::lock_guard lock(g_mutex);
    std::multimap<u64, const Proc*, std::greater<>> sorted;
    for (const auto& [real, p] : g_by_real) sorted.emplace(p->calls.load(), p.get());
    for (const auto& [n, p] : sorted) {
        std::fprintf(stderr, "nvn\t%llu\t%s\n", static_cast<unsigned long long>(n), p->name.c_str());
    }
}

} // namespace nxrt::nvn
