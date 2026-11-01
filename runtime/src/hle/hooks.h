// SPDX-License-Identifier: GPL-3.0-or-later
// Host functions callable from guest code. Two kinds:
//  - hooks on real guest addresses (a function entry in a module): checked
//    whenever execution enters a module from elsewhere, which is how calls
//    through imports and function pointers arrive;
//  - fake functions: addresses outside every module, handed to the guest (for
//    example from a GetProcAddress) and served entirely by the host.
// A host function either returns to the guest caller (pc = x30) or sets pc to
// continue somewhere else, for instance into the original guest function.
#pragma once

#include <functional>
#include <string>

#include "common.h"
#include "guest_abi.h"

namespace nxrt::hle {

using HostFunction = std::function<void(GuestContext& c)>;

constexpr u64 kFakeFunctionBase = 0x7F'0000'0000ULL; // inside the address space, never mapped

void RegisterHook(u64 guest_address, std::string name, HostFunction fn);
u64 MakeFakeFunction(std::string name, HostFunction fn);
// Runs the host function at `pc` if there is one. Returns false otherwise.
bool DispatchHostCall(GuestContext& c);
bool IsFakeFunction(u64 pc);

// Helpers for host functions.
inline void ReturnToCaller(GuestContext& c) {
    c.pc = c.x[30];
}

} // namespace nxrt::hle
