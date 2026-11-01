// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <unordered_set>

namespace nxrt::frontend {

// Keyboard keys used by the inline software keyboard stay captured until the
// host releases them, even if the guest closes the applet during key handling.
class KeyboardGameInputFilter {
public:
    void OnKeyDown(int scancode, bool inline_keyboard_active) {
        if (inline_keyboard_active) captured_.insert(scancode);
    }

    void OnKeyUp(int scancode) { captured_.erase(scancode); }

    bool SuppressGameInput(int scancode, bool inline_keyboard_active) const {
        return inline_keyboard_active || captured_.contains(scancode);
    }

private:
    std::unordered_set<int> captured_;
};

} // namespace nxrt::frontend
