// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <string>

namespace nxrt::hle::input_script {

struct Step {
    std::uint64_t frame{};
    std::uint64_t buttons{};
    std::int32_t lx{}, ly{}, rx{}, ry{};
    bool has_axes{};
    std::string text;
};

struct Sticks {
    std::int32_t lx{}, ly{}, rx{}, ry{};
};

struct State {
    std::uint64_t buttons{};
    Sticks sticks{};
    bool axes_active{};
};

// Accepts `frame buttons` or `frame buttons lx ly rx ry`. A false result
// indicates an invalid, non-empty line.
bool ParseLine(const std::string& line, Step& out);

// Button masks always replace the previous script mask. Stick values update
// only when the line includes all four axes.
void Apply(const Step& step, State& state);

} // namespace nxrt::hle::input_script
