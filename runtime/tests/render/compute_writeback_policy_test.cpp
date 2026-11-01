#include "render/compute_writeback_policy.h"

#include <cstdlib>

using nxrt::render::MustFlushForUntrackedComputeTextures;

int main() {
    const auto require = [](bool condition) {
        if (!condition) std::abort();
    };

    require(!MustFlushForUntrackedComputeTextures(false, true, false));
    require(!MustFlushForUntrackedComputeTextures(true, false, false));
    require(MustFlushForUntrackedComputeTextures(true, true, false));
    require(!MustFlushForUntrackedComputeTextures(true, true, true));
}
