// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>
#include <string>

#include "services/input_script.h"

using nxrt::hle::input_script::Apply;
using nxrt::hle::input_script::ParseLine;
using nxrt::hle::input_script::Step;
using nxrt::hle::input_script::Sticks;

int main() {
    Step step;
    assert(ParseLine("12 A", step));
    assert(step.frame == 12 && step.buttons == 1 && !step.has_axes);

    assert(ParseLine("13 B 32767 -32767 123 -456", step));
    assert(step.frame == 13 && step.buttons == 2 && step.has_axes);
    assert(step.lx == 32767 && step.ly == -32767 && step.rx == 123 && step.ry == -456);

    nxrt::hle::input_script::State state{};
    Apply(step, state);
    assert(state.buttons == 2 && state.axes_active);
    assert(state.sticks.lx == 32767 && state.sticks.ly == -32767 && state.sticks.rx == 123 &&
           state.sticks.ry == -456);

    assert(ParseLine("14 -", step));
    Apply(step, state);
    assert(state.buttons == 0 && state.axes_active);
    assert(state.sticks.lx == 32767 && state.sticks.ly == -32767 && state.sticks.rx == 123 &&
           state.sticks.ry == -456);

    assert(ParseLine("15 none 0 0 0 0", step));
    Apply(step, state);
    assert(state.axes_active && state.sticks.lx == 0 && state.sticks.ly == 0 && state.sticks.rx == 0 &&
           state.sticks.ry == 0);

    for (const auto* bad : {"16 A 1 2 3", "17 A 1 2 3 4 extra", "18 A nope 0 0 0",
                            "19 A 32768 0 0 0", "20 A -32768 0 0 0", "21 A 0 0 0 32768"}) {
        assert(!ParseLine(bad, step));
    }
    std::cout << "input script tests passed\n";
}
