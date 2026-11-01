#include "render/pipeline_cache_key.h"

#include <cstdint>
#include <vector>

int main() {
    using nxrt::render::AppendVertexStreamStateToPipelineKey;

    std::vector<std::uint64_t> per_vertex{0xabc};
    std::vector<std::uint64_t> per_instance{0xabc};
    constexpr std::uint32_t vertex_rate_state[]{24, 0};
    constexpr std::uint32_t instance_rate_state[]{24, 1};

    AppendVertexStreamStateToPipelineKey(per_vertex, vertex_rate_state);
    AppendVertexStreamStateToPipelineKey(per_instance, instance_rate_state);
    if (per_vertex == per_instance) return 1;

    std::vector<std::uint64_t> changed_stride{0xabc};
    constexpr std::uint32_t changed_stride_state[]{32, 0};
    AppendVertexStreamStateToPipelineKey(changed_stride, changed_stride_state);
    if (per_vertex == changed_stride) return 2;

    std::vector<std::uint64_t> two_streams{0xabc};
    constexpr std::uint32_t two_stream_state[]{24, 0, 8, 1};
    AppendVertexStreamStateToPipelineKey(two_streams, two_stream_state);
    if (two_streams.size() != 5) return 3;
    if (two_streams[1] != 24 || two_streams[2] != 0 || two_streams[3] != 8 || two_streams[4] != 1) return 4;
}
