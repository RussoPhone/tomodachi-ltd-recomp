// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace nxrt::render {

struct GpuCpuRange {
    std::uint64_t gpu;
    std::uint64_t cpu;
    std::uint64_t size;
};

struct CpuBackingSpan {
    std::uint64_t cpu;
    std::uint64_t size;
};

// Resolves an address against mapping ranges. The returned span is
// the accessible backing range, not the semantic length of a shader. Adjacent
// ranges are included only when both GPU and CPU addresses are contiguous.
inline std::optional<CpuBackingSpan> ResolveGpuCpuRange(std::uint64_t address,
                                                        std::span<const GpuCpuRange> ranges,
                                                        bool first_match = false) {
    std::size_t first = ranges.size();
    for (std::size_t i = 0; i < ranges.size(); ++i) {
        const auto& candidate = ranges[i];
        if (candidate.size == 0 || candidate.gpu > std::numeric_limits<std::uint64_t>::max() - candidate.size ||
            candidate.cpu > std::numeric_limits<std::uint64_t>::max() - candidate.size) {
            continue;
        }
        if (candidate.gpu <= address && address - candidate.gpu < candidate.size) {
            first = i;
            if (first_match) break;
        }
    }
    if (first == ranges.size()) return std::nullopt;

    const GpuCpuRange& range = ranges[first];
    const std::uint64_t delta = address - range.gpu;
    std::uint64_t span_size = range.size - delta;
    std::uint64_t gpu_end = range.gpu + range.size;
    std::uint64_t cpu_end = range.cpu + range.size;

    while (true) {
        const GpuCpuRange* adjacent = nullptr;
        for (const GpuCpuRange& candidate : ranges) {
            if (candidate.size == 0 || candidate.gpu > std::numeric_limits<std::uint64_t>::max() - candidate.size ||
                candidate.cpu > std::numeric_limits<std::uint64_t>::max() - candidate.size ||
                candidate.gpu != gpu_end || candidate.cpu != cpu_end ||
                span_size > std::numeric_limits<std::uint64_t>::max() - candidate.size) {
                continue;
            }
            adjacent = &candidate;
            break;
        }
        if (!adjacent) break;
        span_size += adjacent->size;
        gpu_end += adjacent->size;
        cpu_end += adjacent->size;
    }
    return CpuBackingSpan{range.cpu + delta, span_size};
}

inline std::optional<CpuBackingSpan> ResolveGpuBacking(std::uint64_t address,
                                                       std::span<const GpuCpuRange> virtual_ranges,
                                                       std::span<const GpuCpuRange> pool_ranges) {
    if (const auto mapped = ResolveGpuCpuRange(address, virtual_ranges)) return mapped;
    return ResolveGpuCpuRange(address, pool_ranges, true);
}

// Checks an access relative to the resolved backing span without overflowing.
inline bool ContainsBackingAccess(const CpuBackingSpan& span, std::uint64_t offset, std::uint64_t size) {
    return offset <= span.size && size <= span.size - offset;
}

} // namespace nxrt::render
