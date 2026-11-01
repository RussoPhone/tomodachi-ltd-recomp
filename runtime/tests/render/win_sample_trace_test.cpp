#include <cassert>
#include <cstdint>
#include <vector>

#include "render/win_sample_trace.h"

int main() {
    using nxrt::render::ParseWinSampleCheckpointDrawIds;

    const auto ids = ParseWinSampleCheckpointDrawIds("23,24,27,28");
    assert(ids);
    assert((*ids == std::vector<std::uint64_t>{23, 24, 27, 28}));
    assert(!ParseWinSampleCheckpointDrawIds("23,,28"));
    assert(!ParseWinSampleCheckpointDrawIds("0,24"));
    assert(!ParseWinSampleCheckpointDrawIds("24,invalid"));
}
