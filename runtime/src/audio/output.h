// SPDX-License-Identifier: GPL-3.0-or-later
// Host audio output: 48 kHz stereo float through SDL.
#pragma once

#include <vector>

namespace nxrt::audio {

// Opens the device on first use; false if there is no audio device.
bool OutputReady();
// Queues interleaved stereo samples; drops them if the queue is already long,
// so a stalled game never builds up latency.
void OutputQueue(const std::vector<float>& stereo);

} // namespace nxrt::audio
