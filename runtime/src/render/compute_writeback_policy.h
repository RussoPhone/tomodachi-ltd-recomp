#pragma once

namespace nxrt::render {

// A compute shader with texture inputs may need guest-memory ordering if one
// of those inputs aliases a pending SSBO writeback. Without an address-range
// proof, keep the conservative flush unless a diagnostic replay explicitly
// opts into checking for actual reads.
constexpr bool MustFlushForUntrackedComputeTextures(bool has_pending_writebacks, bool has_texture_inputs,
                                                    bool skip_conservative_texture_flush) noexcept {
    return has_pending_writebacks && has_texture_inputs && !skip_conservative_texture_flush;
}

} // namespace nxrt::render
