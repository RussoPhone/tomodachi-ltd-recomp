// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt shim: the few suyu settings the shader recompiler reads, fixed to the
// values a native-resolution renderer wants.
#pragma once

#include "common/common_types.h"

namespace Settings {

template <typename T>
struct Setting {
    T value;
    T GetValue() const { return value; }
};

struct ResolutionScalingInfo {
    u32 up_scale = 1;
    u32 down_shift = 0;
    f32 up_factor = 1.0f;
    f32 down_factor = 1.0f;
    bool active = false;
};

struct Values {
    ResolutionScalingInfo resolution_info;
    Setting<bool> rescale_hack{false};
    Setting<bool> fix_bloom_effects{false};
    bool renderer_debug = false;
    bool disable_shader_loop_safety_checks = false;
};

inline Values values;

} // namespace Settings
