// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace nxrt::render {

// NVN stream state stores a stride and divisor for each vertex binding.
inline void AppendVertexStreamStateToPipelineKey(std::vector<std::uint64_t>& key,
                                                 std::span<const std::uint32_t> streams) {
    for (std::size_t i = 0; i < streams.size(); i += 2) {
        key.push_back(streams[i]);
        key.push_back(i + 1 < streams.size() ? streams[i + 1] : 0);
    }
}

} // namespace nxrt::render
