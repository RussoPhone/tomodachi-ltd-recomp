// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "common.h"

namespace nxrt::gpu {

struct GpfifoSnapshot {
    u32 channel = 0;
    std::vector<std::vector<u32>> command_lists;
};

// Copies command words so later guest writes cannot rewrite the owned list.
inline std::vector<u32> OwnCommandWords(std::span<const u32> words) {
    return {words.begin(), words.end()};
}

// Explicitly captures all supplied lists now; execution later uses those owned
// words. SubmitGpfifo instead reads guest words inline as it executes.
GpfifoSnapshot CaptureGpfifo(u32 channel, const u64* entries, size_t count);
void ExecuteGpfifo(const GpfifoSnapshot& snapshot);

// Synchronously reads and applies methods in order on `channel`, preserving
// earlier writes to later words/lists and channel registers across submits.
void SubmitGpfifo(u32 channel, const u64* entries, size_t count);

} // namespace nxrt::gpu
