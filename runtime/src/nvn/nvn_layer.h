// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "guest_abi.h"
#include "loader.h"

namespace nxrt::nvn {

// Arguments of an intercepted NVN call, and its result once it returned.
struct Call {
    u64 x[8];
    u64 v[8]; // low 64 bits of v0-v7 (float arguments)
    u64 ret = 0;
    u64 X(int i) const { return x[i]; }
    float F(int i) const;
};

using Observer = std::function<void(Call&)>;

// Watches an NVN entry point. on_call runs before the game's driver does,
// on_return after it, with the result. Register before the game starts.
void Observe(const std::string& name, Observer on_call, Observer on_return = {});

// Hooks around every nvnCommandBuffer* call, keyed by the command buffer (x0). before runs ahead of the
// recording handler and the driver, after once the driver returned: the words in command memory between one
// call's after and the next call's before were written by the game itself (inline writer), not by the driver.
// Install before the game starts; with no hooks the calls are forwarded as before.
void SetCommandBufferHooks(std::function<void(u64 cmd, const std::string& name)> before, std::function<void(u64 cmd)> after);

// Puts the runtime between the game and its NVN driver: every NVN entry point
// the game obtains goes through a host function first. Returns false if the
// game has no NVN bootstrap export.
bool InstallLayer(const std::vector<LoadedModule>& modules);

// Base address of the module holding the game's NVN driver (its SDK), or 0.
u64 DriverModuleBase();
// Replay restores this captured global without loading or executing the driver.
void RestoreDriverModuleBaseForReplay(u64 base);

// Per-function call counts, printed at exit when NXRT_NVN_STATS is set.
void DumpStats();

} // namespace nxrt::nvn
