// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>

#include "frontend/inline_keyboard_input.h"
#include "frontend/inline_keyboard_text.h"

using nxrt::frontend::InlineKeyboardInput;
using nxrt::frontend::InlineKeyboardInputEvent;
using nxrt::frontend::InlineKeyboardInputEventType;

int main() {
    using namespace nxrt::frontend::inline_keyboard_text;
    assert(Utf8ToUtf16("M\xC3\xA1rio \xF0\x9F\x8E\xAE") == u"Mário 🎮");
    assert(Utf16ToUtf8(u"Mário 🎮") == "M\xC3\xA1rio \xF0\x9F\x8E\xAE");
    assert(Utf8ToUtf16("\xF0\x9F") == u"\uFFFD");
    assert(Utf8ToUtf16("\xE0\x80\x80") == u"\uFFFD");
    assert(Utf16ToUtf8(u"\xD800") == "\xEF\xBF\xBD");
    assert(Truncate(u"A🎮B", 2) == u"A");
    assert(Truncate(u"A🎮B", 3) == u"A🎮");
    std::u16string editable = u"A🎮B";
    std::int32_t cursor = 1;
    Insert(editable, cursor, u"é", 6);
    assert(editable == u"Aé🎮B" && cursor == 2);
    Insert(editable, cursor, u"🎮", 5);
    assert(editable == u"Aé🎮B" && cursor == 2); // Never exceed the UTF-16 unit limit.
    Backspace(editable, cursor);
    assert(editable == u"A🎮B" && cursor == 1);
    cursor = 3; // Past the surrogate pair, before B.
    Backspace(editable, cursor);
    assert(editable == u"AB" && cursor == 1);
    assert(NormalizeCursor(u"A🎮B", 2) == 1); // Never split the pair.

    InlineKeyboardInput input;
    input.PushText(u"ignored");
    input.PushKey(InlineKeyboardInputEventType::Confirm);
    assert(input.Drain().empty());

    const auto session = input.Begin();
    assert(input.Active());
    input.PushText(u"João");
    input.PushKey(InlineKeyboardInputEventType::Backspace);
    input.PushText(u"o");
    input.PushKey(InlineKeyboardInputEventType::Confirm);
    auto events = input.Drain();
    assert(events.size() == 4);
    assert(events[0].type == InlineKeyboardInputEventType::Text && events[0].text == u"João");
    assert(events[1].type == InlineKeyboardInputEventType::Backspace);
    assert(events[2].type == InlineKeyboardInputEventType::Text && events[2].text == u"o");
    assert(events[3].type == InlineKeyboardInputEventType::Confirm);

    input.PushKey(InlineKeyboardInputEventType::Cancel);
    input.End(session);
    assert(!input.Active());
    assert(input.Drain().empty());
    input.PushText(u"ignored after close");
    assert(input.Drain().empty());

    input.Begin();
    input.PushKey(InlineKeyboardInputEventType::Cancel);
    events = input.Drain();
    assert(events.size() == 1 && events[0].type == InlineKeyboardInputEventType::Cancel);

    const auto wake_session = input.Begin();
    auto waiting = std::async(std::launch::async, [&] { return input.WaitAndDrain(wake_session); });
    assert(waiting.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    input.PushText(u"Mio");
    assert(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    events = waiting.get();
    assert(events.size() == 1 && events[0].text == u"Mio");

    const auto end_session = input.Begin();
    auto ending = std::async(std::launch::async, [&] { return input.WaitAndDrain(end_session); });
    assert(ending.wait_for(std::chrono::milliseconds(20)) == std::future_status::timeout);
    input.End(end_session);
    assert(ending.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(ending.get().empty());
    std::cout << "inline keyboard input tests passed\n";
}
