// SPDX-License-Identifier: GPL-3.0-or-later
// Small shared vocabulary for the runtime: integer types, results, logging.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;

namespace nxrt {

// Horizon result codes: module in bits 0-8, description in bits 9-21.
constexpr u32 MakeResult(u32 module, u32 description) {
    return module | (description << 9);
}

namespace rc {
constexpr u32 Success = 0;
// Kernel (module 1).
constexpr u32 OutOfSessions = MakeResult(1, 7);
constexpr u32 InvalidArgument = MakeResult(1, 14);
constexpr u32 InvalidSize = MakeResult(1, 101);
constexpr u32 InvalidAddress = MakeResult(1, 102);
constexpr u32 NotImplemented = MakeResult(1, 33);
constexpr u32 OutOfMemory = MakeResult(1, 104);
constexpr u32 OutOfHandles = MakeResult(1, 105);
constexpr u32 InvalidCurrentMemory = MakeResult(1, 106);
constexpr u32 InvalidNewMemoryPermission = MakeResult(1, 108);
constexpr u32 InvalidMemoryRegion = MakeResult(1, 110);
constexpr u32 InvalidPriority = MakeResult(1, 112);
constexpr u32 InvalidCoreId = MakeResult(1, 113);
constexpr u32 InvalidHandle = MakeResult(1, 114);
constexpr u32 InvalidPointer = MakeResult(1, 115);
constexpr u32 InvalidCombination = MakeResult(1, 116);
constexpr u32 TimedOut = MakeResult(1, 117);
constexpr u32 Cancelled = MakeResult(1, 118);
constexpr u32 OutOfRange = MakeResult(1, 119);
constexpr u32 InvalidEnumValue = MakeResult(1, 120);
constexpr u32 NotFound = MakeResult(1, 121);
constexpr u32 Busy = MakeResult(1, 122);
constexpr u32 SessionClosed = MakeResult(1, 123);
constexpr u32 InvalidState = MakeResult(1, 125);
constexpr u32 LimitReached = MakeResult(1, 132);
// IPC framework (module 10, sf::hipc / cmif).
constexpr u32 UnknownCommand = MakeResult(10, 221);
} // namespace rc

enum class LogLevel { Trace, Debug, Info, Warning, Error };
void Log(LogLevel level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
bool LogEnabled(LogLevel level);
[[noreturn]] void Fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

} // namespace nxrt

#include "log_macros.h"
