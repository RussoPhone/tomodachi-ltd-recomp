// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "common.h"

namespace nxrt::hle {

// Npad button bits (nn::hid::NpadButton).
enum NpadButton : u64 {
    kButtonA = 1ull << 0, kButtonB = 1ull << 1, kButtonX = 1ull << 2, kButtonY = 1ull << 3,
    kButtonStickL = 1ull << 4, kButtonStickR = 1ull << 5, kButtonL = 1ull << 6, kButtonR = 1ull << 7,
    kButtonZL = 1ull << 8, kButtonZR = 1ull << 9, kButtonPlus = 1ull << 10, kButtonMinus = 1ull << 11,
    kButtonLeft = 1ull << 12, kButtonUp = 1ull << 13, kButtonRight = 1ull << 14, kButtonDown = 1ull << 15,
};

// Player 1's current state; sticks range -32767..32767.
void SetHostInput(u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry);
struct HostInputState {
    u64 buttons = 0;
    s32 lx = 0, ly = 0, rx = 0, ry = 0;
};
HostInputState GetHostInput();
void SetHostTouch(bool pressed, u32 x, u32 y);

// Called once per presented VI frame; drives NXRT_INPUT_SCRIPT.
void OnInputFrame(u64 frame);

// NXRT_INPUT_RECORD only (no-ops otherwise): F9 in the window writes a marker line and requests a frame dump.
bool InputRecordEnabled();
void InputRecordMarker(u64 frame);

void RegisterHidServices();

} // namespace nxrt::hle
