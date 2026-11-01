// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdint>
#include <iostream>

#include "frontend/inline_keyboard_protocol.h"

int main() {
    using namespace nxrt::frontend::inline_keyboard_protocol;

    const auto cancel = BuildCancelReply(3);
    assert(kReplyMovedTab == 0x4);
    assert(kReplyDecidedEnter == 0x5);
    assert(kReplyDecidedCancel == 0x6);
    assert(cancel.size() == 8); // Cancellation has only state and reply type.
    assert(cancel[0] == 3 && cancel[1] == 0 && cancel[2] == 0 && cancel[3] == 0);
    assert(cancel[4] == 0x6 && cancel[5] == 0 && cancel[6] == 0 && cancel[7] == 0);

    std::cout << "inline keyboard protocol tests passed\n";
}
