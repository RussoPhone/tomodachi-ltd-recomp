// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>

namespace nxrt {

struct RuntimeConfig {
    std::filesystem::path game_dir; // installed game: exefs/, exefs/romfs.bin, control/
    std::filesystem::path user_dir; // saves and settings
    std::filesystem::path font_dir; // replacement system fonts (third_party/suyu/fonts)
    bool headless = false;          // no window: run the game on the main thread only
    bool offscreen = false;         // Vulkan renderer without an SDL window or swapchain
};

extern RuntimeConfig g_config;

} // namespace nxrt
