// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

namespace nxrt::hle {

// A frame the game queued for display.
struct PresentedBuffer {
    u32 width, height, stride, format;
    u32 nvmap_handle; // memory holding the image
    u32 offset;
};

// Called for every queued frame.
void OnPresent(const PresentedBuffer& buffer);

void RegisterViServices();

} // namespace nxrt::hle
