// SPDX-License-Identifier: GPL-3.0-or-later
#include "hle/hooks.h"

#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <deque>

namespace nxrt::hle {

namespace {

struct Entry {
    std::string name;
    HostFunction fn;
};

std::shared_mutex g_mutex;
std::unordered_map<u64, Entry> g_hooks;
std::deque<Entry> g_fakes; // deque: entries keep their address as it grows

} // namespace

void RegisterHook(u64 guest_address, std::string name, HostFunction fn) {
    std::unique_lock lock(g_mutex);
    g_hooks[guest_address] = {std::move(name), std::move(fn)};
}

u64 MakeFakeFunction(std::string name, HostFunction fn) {
    std::unique_lock lock(g_mutex);
    g_fakes.push_back({std::move(name), std::move(fn)});
    return kFakeFunctionBase + (g_fakes.size() - 1) * 16;
}

bool IsFakeFunction(u64 pc) {
    return pc >= kFakeFunctionBase && pc < kFakeFunctionBase + 0x1'0000'0000ULL;
}

bool DispatchHostCall(GuestContext& c) {
    const HostFunction* fn = nullptr;
    {
        std::shared_lock lock(g_mutex);
        if (IsFakeFunction(c.pc)) {
            const u64 index = (c.pc - kFakeFunctionBase) / 16;
            if (index < g_fakes.size()) fn = &g_fakes[index].fn;
        } else if (!g_hooks.empty()) {
            const auto it = g_hooks.find(c.pc);
            if (it != g_hooks.end()) fn = &it->second.fn;
        }
    }
    if (!fn) return false;
    // Entries are never removed, so the function outlives the lock.
    (*fn)(c);
    return true;
}

} // namespace nxrt::hle
