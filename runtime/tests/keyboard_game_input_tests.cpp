// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/keyboard_game_input.h"

#include <cassert>
#include <iostream>

int main() {
    using nxrt::frontend::KeyboardGameInputFilter;

    KeyboardGameInputFilter filter;
    constexpr int kLetterS = 22;

    filter.OnKeyDown(kLetterS, true);
    assert(filter.SuppressGameInput(kLetterS, true));
    // The inline applet can finish on this keydown; keep it captured until keyup.
    assert(filter.SuppressGameInput(kLetterS, false));

    filter.OnKeyUp(kLetterS);
    assert(!filter.SuppressGameInput(kLetterS, false));

    filter.OnKeyDown(kLetterS, false);
    assert(!filter.SuppressGameInput(kLetterS, false));

    std::cout << "keyboard game input filter tests passed\n";
}
