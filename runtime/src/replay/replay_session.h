// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdexcept>
#include "nvn/recorder.h"

namespace nxrt::replay {
// Renderer diagnostics count Present callbacks internally. Empty presents prime
// that counter without executing game code or inventing prior GPU contents.
inline void PrimeReplayFrameCounter(nvn::Renderer& renderer, u64 first_frame) {
    if (first_frame > 10000000) throw std::runtime_error("capture first frame exceeds replay limit");
    for (u64 frame = 1; frame < first_frame; ++frame) renderer.Present(0, frame);
}
} // namespace nxrt::replay
