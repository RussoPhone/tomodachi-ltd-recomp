// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

#include "render/flush_policy.h"

using nxrt::render::CanRecycleAfterFlush;

int main() {
    assert(CanRecycleAfterFlush(VK_SUCCESS, VK_SUCCESS));
    assert(!CanRecycleAfterFlush(VK_SUCCESS, VK_TIMEOUT));
    assert(!CanRecycleAfterFlush(VK_SUCCESS, VK_ERROR_DEVICE_LOST));
    assert(!CanRecycleAfterFlush(VK_ERROR_DEVICE_LOST, VK_ERROR_DEVICE_LOST));
    std::cout << "renderer flush policy tests passed\n";
}
