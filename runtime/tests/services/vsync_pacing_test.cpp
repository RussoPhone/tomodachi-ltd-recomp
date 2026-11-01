// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <iostream>

#include "services/vsync_pacing.h"

using nxrt::hle::PlanVsyncPacing;
using nxrt::hle::ParseSwapInterval;
using nxrt::hle::kVsyncPeriodNs;

int main() {
    using clock = std::chrono::steady_clock;
    using namespace std::chrono_literals;

    constexpr auto period = 16'666'667ns;
    const auto start = clock::time_point{100s};

    const auto first = PlanVsyncPacing(start, {}, period);
    assert(first.should_sleep);
    assert(first.next_vsync == start + period);

    // A frame that finishes before its next deadline waits only for the remainder.
    const auto early_now = start + period + 15ms;
    const auto early = PlanVsyncPacing(early_now, first.next_vsync, period);
    assert(early.should_sleep);
    assert(early.next_vsync == start + 2 * period);

    // A missed deadline skips this wait instead of sleeping another full period.
    const auto late_now = start + period + 40ms;
    const auto late = PlanVsyncPacing(late_now, first.next_vsync, period);
    assert(!late.should_sleep);
    assert(late.next_vsync == late_now);

    // Rebase after a late frame, then pace a fast frame to the 60 Hz ceiling.
    const auto following_fast_now = late_now + 10ms;
    const auto following_fast = PlanVsyncPacing(following_fast_now, late.next_vsync, period);
    assert(following_fast.should_sleep);
    assert(following_fast.next_vsync == late_now + period);

    // Swap interval parsing: exactly 84 bytes, s32 at offset 44, 0 -> 1.
    {
        std::uint8_t qbi[84]{};
        assert(ParseSwapInterval(qbi, 84) == 1);
        for (int iv : {1, 2, 3}) {
            std::memcpy(qbi + 44, &iv, 4);
            assert(ParseSwapInterval(qbi, 84) == iv);
        }
        assert(ParseSwapInterval(qbi, 83) == 0);
        assert(ParseSwapInterval(qbi, 85) == 0);
    }

    // Periods scale with the interval: 30 FPS waits two vsyncs, 20 FPS three.
    for (int iv : {1, 2, 3}) {
        const auto p = std::chrono::nanoseconds(kVsyncPeriodNs * iv);
        const auto t0 = clock::time_point{200s};
        const auto f = PlanVsyncPacing(t0, {}, p);
        assert(f.should_sleep && f.next_vsync == t0 + p);
        const auto e = PlanVsyncPacing(t0 + 5ms, f.next_vsync, p);
        assert(e.should_sleep && e.next_vsync == t0 + 2 * p);
        const auto l = PlanVsyncPacing(t0 + p + 100ms, f.next_vsync, p);
        assert(!l.should_sleep && l.next_vsync == t0 + p + 100ms);
    }

    // Interval change mid-stream: the new period counts from the last vsync
    // (no extra full-period sleep); a late frame still presents immediately.
    {
        const auto p2 = std::chrono::nanoseconds(kVsyncPeriodNs * 2);
        const auto p3 = std::chrono::nanoseconds(kVsyncPeriodNs * 3);
        const auto t0 = clock::time_point{300s};
        const auto a = PlanVsyncPacing(t0, {}, p2);
        const auto b = PlanVsyncPacing(t0 + 3ms, a.next_vsync, p2);
        assert(b.should_sleep && b.next_vsync == t0 + 2 * p2);
        const auto c = PlanVsyncPacing(t0 + p2 * 2 + 1ms, b.next_vsync, p3);
        assert(c.should_sleep && c.next_vsync == b.next_vsync + p3);
        const auto d = PlanVsyncPacing(c.next_vsync + p2 + 1ms, c.next_vsync, p2);
        assert(!d.should_sleep && d.next_vsync == c.next_vsync + p2 + 1ms);
    }

    std::cout << "VI vsync pacing tests passed\n";
}
