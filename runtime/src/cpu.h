// SPDX-License-Identifier: GPL-3.0-or-later
// The recompiled modules, and the loop that runs them for one guest thread.
#pragma once

#include <initializer_list>
#include <vector>

#include "common.h"
#include "guest_abi.h"
#include "loader.h"

namespace nxrt {

struct RecompModule {
    const char* name;
    u64 base;
    u64 end;
    RecompBlockFn (*lookup)(u64);
    RecompBlockFn run_slice;
};

// Pairs each generated module with where its NSO was loaded, then negotiates
// the generated code's optional features (code guard generations, fast-path
// memory, exact native FP). Returns false if the image does not fit this host.
bool InitRecompModules(const std::vector<LoadedModule>& loaded);

// Optional diagnostic support for observing guest reads from the HID touch LIFO.
void OnSharedMemoryMapped(const std::string& name, u64 address, u64 size);
void OnSharedMemoryUnmapped(u64 address, u64 size);
// Restart the bounded HID read trace after a host touch state transition.
void ResetHidReadTraceOnTouchTransition();

// Initial fpcr for new threads: 0 when the FP fast paths were negotiated,
// otherwise the inhibit bit that keeps them off.
u64 InitialFpcr();

const RecompModule* FindModule(u64 pc);
// Module-relative name for an address, for logs: "main+0x1234".
std::string Symbolize(u64 address);
// Return addresses found by walking the guest frame-pointer chain.
std::string Backtrace(const GuestContext& c, int max_frames = 12);

// Calls a guest function from a host function on the current guest thread and
// returns its x0. Everything else the call changes in the context is undone.
u64 CallGuest(u64 function, std::initializer_list<u64> args);
// Runs a guest function with the registers as they are (the arguments of the
// call being intercepted), leaving its results in place and pc at the original
// caller. For host functions that observe a call and its result.
void CallGuestInPlace(u64 function);

} // namespace nxrt
