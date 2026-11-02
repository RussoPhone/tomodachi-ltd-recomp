// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <string>
#include <vector>

#include "../common.h"
#include "frontend/inline_keyboard_input.h"

struct SDL_Window;

namespace nxrt::frontend {

bool Init(const char* title);
SDL_Window* Window();
// Counts a presented frame (shown in the window title).
void NotifyFrame();
// Text typed in the window (SDL_TEXTINPUT) since the last call, as UTF-16, and
// clears it. Thread-safe; called by the software keyboard applet. Empty if
// nothing was typed (or the last character is older than ~10 s).
std::u16string TakeTypedText();
u64 BeginInlineKeyboardInput();
// Test/replay hook: confirm the inline keyboard as if Enter was pressed (no-op when inactive).
void ConfirmInlineKeyboardInput();
void TypeInlineKeyboardText(std::u16string text);
void EndInlineKeyboardInput(u64 session);
std::vector<InlineKeyboardInputEvent> TakeInlineKeyboardInput();
std::vector<InlineKeyboardInputEvent> WaitInlineKeyboardInput(u64 session);
// Pumps window events and input forever; must run on the main thread.
[[noreturn]] void RunEventLoop();

} // namespace nxrt::frontend
