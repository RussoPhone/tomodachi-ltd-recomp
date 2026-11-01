// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "render/depth_clear_policy.h"

using nxrt::render::ShouldDropPendingDepthClear;

int main() {
    // Observed icon-pass states: pre-pass 0x43 (test, write, LEQUAL), color pass 0x31 (test, no write, EQUAL).
    assert(ShouldDropPendingDepthClear(0x31, true, 1700, 1700));
    assert(!ShouldDropPendingDepthClear(0x31, true, 1699, 1700));  // depth written in an earlier frame: clear it
    assert(!ShouldDropPendingDepthClear(0x31, true, 0, 1700));     // never written
    assert(!ShouldDropPendingDepthClear(0x43, true, 1700, 1700));  // writes depth: normal clear
    assert(!ShouldDropPendingDepthClear(0x41, true, 1700, 1700));  // LEQUAL no write
    assert(!ShouldDropPendingDepthClear(0x30, true, 1700, 1700));  // test disabled
    assert(!ShouldDropPendingDepthClear(0x31, false, 1700, 1700)); // image has stencil
    return 0;
}
