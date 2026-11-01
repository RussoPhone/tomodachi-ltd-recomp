// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>

#include "render/gpu_timestamps.h"

using namespace nxrt::render;

static bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main() {
    // 1000 ticks at 1000 ns/tick = 1 ms.
    assert(Near(GpuTicksToMs(5000, 6000, 64, 1000.0), 1.0));
    // 36-bit counter wrap: begin near the top, end after wrap.
    const std::uint64_t top = (std::uint64_t{1} << 36) - 100;
    assert(Near(GpuTicksToNs(top, 900, 36, 1.0), 1000.0));
    // Unsupported queue.
    assert(GpuTicksToNs(0, 10, 0, 1.0) == 0.0);

    GpuSiteStats<3> stats;
    stats.Add(0, 2e6, 5e6);
    stats.Add(0, 1e6, 3e6);
    stats.Add(2, 4e6, 4.5e6);
    stats.Add(7, 1e9, 1e9);  // out of range ignored
    assert(Near(stats.GpuMs(0), 3.0) && Near(stats.WaitMs(0), 8.0) && Near(stats.OverheadMs(0), 5.0));
    assert(stats.count[0] == 2 && stats.count[1] == 0 && stats.count[2] == 1);
    assert(Near(stats.TotalGpuMs(), 7.0) && Near(stats.TotalWaitMs(), 12.5));
    stats.Reset();
    assert(stats.count[0] == 0 && Near(stats.TotalGpuMs(), 0.0));
    return 0;
}
