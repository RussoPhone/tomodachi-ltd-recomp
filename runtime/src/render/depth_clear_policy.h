// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace nxrt::render {

// A guest depth clear is held back until the first use of the depth image. It is dropped only when that first use is
// a draw that (a) tests depth with EQUAL and does not write depth, (b) targets a depth-only image, and (c) follows a
// draw that wrote depth into the same image in the same frame. This is the Mii icon pass pair: a depth pre-pass, then a
// color pass that starts with a depth clear but depends on the pre-pass depth.
// `depth_stencil_word0` is the first word of the NVN depth/stencil state: bit 0 test enable, bit 1 write enable,
// bits 4..7 compare function (3 = EQUAL).
inline bool ShouldDropPendingDepthClear(uint32_t depth_stencil_word0, bool depth_only_image, uint64_t last_depth_write_frame,
                                        uint64_t current_frame) {
    const bool test = depth_stencil_word0 & 1;
    const bool write = (depth_stencil_word0 >> 1) & 1;
    const bool equal = ((depth_stencil_word0 >> 4) & 0xf) == 3;
    return test && !write && equal && depth_only_image && last_depth_write_frame == current_frame;
}

}  // namespace nxrt::render
