// SPDX-License-Identifier: GPL-3.0-or-later
// Pure helpers for the opt-in GPU timestamp profile (NXRT_GPU_TIMESTAMPS=1).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nxrt::render {

// Converts two raw timestamp query values to nanoseconds. valid_bits is the queue's timestampValidBits and
// period_ns is VkPhysicalDeviceLimits::timestampPeriod. Handles counter wrap-around inside valid_bits.
inline double GpuTicksToNs(std::uint64_t begin, std::uint64_t end, std::uint32_t valid_bits, double period_ns) {
    if (valid_bits == 0 || valid_bits > 64) return 0.0;
    const std::uint64_t mask = valid_bits == 64 ? ~std::uint64_t{0} : ((std::uint64_t{1} << valid_bits) - 1);
    const std::uint64_t ticks = ((end & mask) - (begin & mask)) & mask;
    return static_cast<double>(ticks) * period_ns;
}

inline double GpuTicksToMs(std::uint64_t begin, std::uint64_t end, std::uint32_t valid_bits, double period_ns) {
    return GpuTicksToNs(begin, end, valid_bits, period_ns) / 1'000'000.0;
}

// Per flush-site aggregate of GPU execution time versus host wait time.
template <std::size_t N>
struct GpuSiteStats {
    std::array<double, N> gpu_ns{};
    std::array<double, N> wait_ns{};
    std::array<std::uint64_t, N> count{};

    void Add(std::size_t site, double gpu, double wait) {
        if (site >= N) return;
        gpu_ns[site] += gpu;
        wait_ns[site] += wait;
        ++count[site];
    }
    double GpuMs(std::size_t site) const { return site < N ? gpu_ns[site] / 1e6 : 0.0; }
    double WaitMs(std::size_t site) const { return site < N ? wait_ns[site] / 1e6 : 0.0; }
    // Approximation: host wait includes submit latency and fence round trip on top of GPU execution.
    double OverheadMs(std::size_t site) const { return WaitMs(site) - GpuMs(site); }
    double TotalGpuMs() const {
        double total = 0;
        for (double v : gpu_ns) total += v;
        return total / 1e6;
    }
    double TotalWaitMs() const {
        double total = 0;
        for (double v : wait_ns) total += v;
        return total / 1e6;
    }
    void Reset() { *this = GpuSiteStats{}; }
};

}  // namespace nxrt::render
