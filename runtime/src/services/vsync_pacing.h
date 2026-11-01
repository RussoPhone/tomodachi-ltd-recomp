// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nxrt::hle {

struct VsyncPacingPlan {
    std::chrono::steady_clock::time_point next_vsync;
    bool should_sleep = false;
};

// Pace early frames to the 60 Hz deadline. If rendering already missed that
// deadline, present immediately and rebase so the following fast frame still
// waits for one full period instead of letting the producer run above 60 Hz.
inline VsyncPacingPlan PlanVsyncPacing(std::chrono::steady_clock::time_point now,
                                       std::chrono::steady_clock::time_point previous_vsync,
                                       std::chrono::steady_clock::duration period) {
    using clock = std::chrono::steady_clock;
    if (previous_vsync == clock::time_point{}) return {now + period, true};

    const auto deadline = previous_vsync + period;
    if (now < deadline) return {deadline, true};
    return {now, false};
}

// QueueBufferInput is packed, 84 bytes; s32 swap_interval lives at offset 44.
inline constexpr std::size_t kQueueBufferInputSize = 84;
inline constexpr std::size_t kSwapIntervalOffset = 44;
inline constexpr std::int64_t kVsyncPeriodNs = 16'666'667;

// Returns the swap interval (>= 1) or 0 when the payload is not a QueueBufferInput.
inline std::int32_t ParseSwapInterval(const std::uint8_t* data, std::size_t size) {
    if (!data || size != kQueueBufferInputSize) return 0;
    std::int32_t v;
    std::memcpy(&v, data + kSwapIntervalOffset, sizeof(v));
    return v < 1 ? 1 : v;
}

}  // namespace nxrt::hle
