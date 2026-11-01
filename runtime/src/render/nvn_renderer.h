// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <memory>

#include "nvn/recorder.h"

struct SDL_Window;

namespace nxrt::render {

// Vulkan renderer for the recorded NVN command lists; presents into `window`
// (or renders off screen when null). Returns null if there is no usable GPU.
std::unique_ptr<nvn::Renderer> CreateNvnRenderer(SDL_Window* window);

} // namespace nxrt::render
