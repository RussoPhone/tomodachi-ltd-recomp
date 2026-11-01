// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nxrt::frontend::inline_keyboard_protocol {

constexpr std::uint32_t kReplyMovedTab = 0x4;
constexpr std::uint32_t kReplyDecidedEnter = 0x5;
constexpr std::uint32_t kReplyDecidedCancel = 0x6;

inline std::array<std::uint8_t, 8> BuildCancelReply(std::uint32_t state) {
    std::array<std::uint8_t, 8> reply{};
    for (std::size_t i = 0; i < sizeof(state); ++i) {
        reply[i] = static_cast<std::uint8_t>(state >> (i * 8));
        reply[sizeof(state) + i] = static_cast<std::uint8_t>(kReplyDecidedCancel >> (i * 8));
    }
    return reply;
}

} // namespace nxrt::frontend::inline_keyboard_protocol
