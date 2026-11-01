// SPDX-License-Identifier: GPL-3.0-or-later
// The game window and input. Runs on the process's main thread; the game itself
// runs on its own threads. Keyboard and the first SDL game controller drive
// player 1's Pro Controller.
#include "frontend/window.h"
#include "version.h"
#include "frontend/keyboard_game_input.h"

#include <SDL.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "services/hid.h"
#include "tts_trace.h"

namespace nxrt {
void SetHidReadTraceFrame(u64 frame);
}

namespace nxrt::frontend {

namespace {

SDL_Window* g_window = nullptr;
SDL_GameController* g_pad = nullptr;
InlineKeyboardInput g_inline_keyboard_input;
KeyboardGameInputFilter g_keyboard_game_input_filter;
std::array<bool, SDL_CONTROLLER_BUTTON_MAX> g_inline_captured_pad_buttons{};
std::array<bool, 4> g_inline_captured_pad_axes{};
bool g_inline_captured_pad_left_trigger = false;
bool g_inline_captured_pad_right_trigger = false;
constexpr s32 kPadTriggerPressThreshold = 16000;
constexpr s32 kPadTriggerReleaseThreshold = 8000;
const bool g_trace_keyboard_input = [] {
    const char* value = std::getenv("NXRT_TRACE_KEYBOARD");
    return value && value[0] == '1' && value[1] == '\0';
}();
const bool g_trace_host_input = [] {
    const char* value = std::getenv("NXRT_TRACE_HOST_INPUT");
    return value && value[0] == '1' && value[1] == '\0';
}();

// Typed text for the software keyboard: written by the event loop (main
// thread), consumed by the applet service thread.
constexpr size_t kMaxTypedChars = 64;
constexpr u32 kTypedTextStaleMs = 10000;
std::mutex g_text_lock;
std::u16string g_text;
u32 g_text_tick = 0;

// Appends UTF-8 as UTF-16; invalid sequences are skipped.
void AppendUtf8(std::u16string& out, const char* s) {
    const auto* p = reinterpret_cast<const u8*>(s);
    while (*p) {
        u32 cp, n;
        if (*p < 0x80) { cp = *p; n = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; n = 4; }
        else { p++; continue; }
        u32 i = 1;
        for (; i < n && (p[i] & 0xC0) == 0x80; i++) cp = (cp << 6) | (p[i] & 0x3F);
        p += i;
        if (i != n || cp < 0x20 || cp == 0x7F || cp > 0x10FFFF) continue; // control/invalid
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<char16_t>(cp));
        }
    }
}
std::atomic<u64> g_frames{0};

struct KeyBinding {
    SDL_Scancode key;
    u64 button;
};

// Keyboard layout: arrows = left stick-free d-pad, WASD = left stick.
const KeyBinding kKeys[] = {
    {SDL_SCANCODE_X, hle::kButtonA},      {SDL_SCANCODE_Z, hle::kButtonB},
    {SDL_SCANCODE_S, hle::kButtonX},      {SDL_SCANCODE_A, hle::kButtonY},
    {SDL_SCANCODE_W, hle::kButtonL},      {SDL_SCANCODE_R, hle::kButtonR},
    {SDL_SCANCODE_1, hle::kButtonZL},     {SDL_SCANCODE_3, hle::kButtonZR},
    {SDL_SCANCODE_RETURN, hle::kButtonPlus}, {SDL_SCANCODE_BACKSPACE, hle::kButtonMinus},
    {SDL_SCANCODE_UP, hle::kButtonUp},    {SDL_SCANCODE_DOWN, hle::kButtonDown},
    {SDL_SCANCODE_LEFT, hle::kButtonLeft}, {SDL_SCANCODE_RIGHT, hle::kButtonRight},
};

const struct {
    SDL_GameControllerButton pad;
    u64 button;
} kPadButtons[] = {
    // Nintendo layout: the east face button is A.
    {SDL_CONTROLLER_BUTTON_B, hle::kButtonA},           {SDL_CONTROLLER_BUTTON_A, hle::kButtonB},
    {SDL_CONTROLLER_BUTTON_Y, hle::kButtonX},           {SDL_CONTROLLER_BUTTON_X, hle::kButtonY},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, hle::kButtonL}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, hle::kButtonR},
    {SDL_CONTROLLER_BUTTON_START, hle::kButtonPlus},    {SDL_CONTROLLER_BUTTON_BACK, hle::kButtonMinus},
    {SDL_CONTROLLER_BUTTON_LEFTSTICK, hle::kButtonStickL}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, hle::kButtonStickR},
    {SDL_CONTROLLER_BUTTON_DPAD_UP, hle::kButtonUp},    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, hle::kButtonDown},
    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, hle::kButtonLeft}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, hle::kButtonRight},
};

s32 Axis(SDL_GameControllerAxis axis, bool invert) {
    if (!g_pad) return 0;
    s32 v = SDL_GameControllerGetAxis(g_pad, axis);
    if (std::abs(v) < 4000) v = 0; // dead zone
    v = std::max(-32767, std::min(32767, v));
    return invert ? -v : v;
}

void CapturePadAxisUntilNeutral(s32& value, bool inline_active, bool& captured) {
    if (inline_active) {
        captured = value != 0;
        return;
    }
    if (!captured) return;
    if (value) value = 0;
    else captured = false;
}

bool PadTriggerPressed(s32 value, bool inline_active, bool& captured) {
    const bool pressed = value > kPadTriggerPressThreshold;
    if (inline_active && pressed) captured = true;
    if (value <= kPadTriggerReleaseThreshold) captured = false;
    return pressed && !(captured && !inline_active);
}

void UpdateInput() {
    const u8* keys = SDL_GetKeyboardState(nullptr);
    u64 buttons = 0;
    const bool keyboard_active = g_inline_keyboard_input.Active();
    for (const auto& k : kKeys) {
        if (g_keyboard_game_input_filter.SuppressGameInput(k.key, keyboard_active)) continue;
        if (keys[k.key]) buttons |= k.button;
    }
    s32 lx = 0, ly = 0;
    if (keys[SDL_SCANCODE_J] &&
        !g_keyboard_game_input_filter.SuppressGameInput(SDL_SCANCODE_J, keyboard_active)) lx -= 32767;
    if (keys[SDL_SCANCODE_L] &&
        !g_keyboard_game_input_filter.SuppressGameInput(SDL_SCANCODE_L, keyboard_active)) lx += 32767;
    if (keys[SDL_SCANCODE_I] &&
        !g_keyboard_game_input_filter.SuppressGameInput(SDL_SCANCODE_I, keyboard_active)) ly += 32767;
    if (keys[SDL_SCANCODE_K] &&
        !g_keyboard_game_input_filter.SuppressGameInput(SDL_SCANCODE_K, keyboard_active)) ly -= 32767;
    // Right stick horizontal on the keyboard: Q = left, E = right.
    s32 rx = 0, ry = 0;
    if (keys[SDL_SCANCODE_Q] &&
        !g_keyboard_game_input_filter.SuppressGameInput(SDL_SCANCODE_Q, keyboard_active)) rx -= 32767;
    if (keys[SDL_SCANCODE_E] &&
        !g_keyboard_game_input_filter.SuppressGameInput(SDL_SCANCODE_E, keyboard_active)) rx += 32767;
    if (g_pad) {
        for (const auto& b : kPadButtons) {
            const bool pressed = SDL_GameControllerGetButton(g_pad, b.pad) != 0;
            bool& captured = g_inline_captured_pad_buttons[static_cast<size_t>(b.pad)];
            if (keyboard_active && pressed) captured = true;
            if (pressed && !(captured && !keyboard_active)) buttons |= b.button;
            if (!pressed) captured = false;
        }
        const s32 left_trigger_value = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        const s32 right_trigger_value = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (PadTriggerPressed(left_trigger_value, keyboard_active, g_inline_captured_pad_left_trigger))
            buttons |= hle::kButtonZL;
        if (PadTriggerPressed(right_trigger_value, keyboard_active, g_inline_captured_pad_right_trigger))
            buttons |= hle::kButtonZR;

        s32 pad_lx = Axis(SDL_CONTROLLER_AXIS_LEFTX, false);
        s32 pad_ly = Axis(SDL_CONTROLLER_AXIS_LEFTY, true);
        s32 pad_rx = Axis(SDL_CONTROLLER_AXIS_RIGHTX, false);
        s32 pad_ry = Axis(SDL_CONTROLLER_AXIS_RIGHTY, true);
        CapturePadAxisUntilNeutral(pad_lx, keyboard_active, g_inline_captured_pad_axes[0]);
        CapturePadAxisUntilNeutral(pad_ly, keyboard_active, g_inline_captured_pad_axes[1]);
        CapturePadAxisUntilNeutral(pad_rx, keyboard_active, g_inline_captured_pad_axes[2]);
        CapturePadAxisUntilNeutral(pad_ry, keyboard_active, g_inline_captured_pad_axes[3]);
        if (!lx && !ly) {
            lx = pad_lx;
            ly = pad_ly;
        }
        if (!rx && !ry) {
            rx = pad_rx;
            ry = pad_ry;
        }
    }
    int mouse_x = 0, mouse_y = 0, window_width = 1280, window_height = 720;
    const u32 mouse_buttons = SDL_GetMouseState(&mouse_x, &mouse_y);
    SDL_GetWindowSize(g_window, &window_width, &window_height);
    const u32 touch_x = static_cast<u32>(
        std::clamp(mouse_x * 1280 / std::max(window_width, 1), 0, 1279));
    const u32 touch_y = static_cast<u32>(
        std::clamp(mouse_y * 720 / std::max(window_height, 1), 0, 719));
    hle::SetHostTouch((mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) != 0, touch_x, touch_y);
    if (g_trace_host_input) {
        static u64 previous_buttons = ~u64{0};
        static s32 previous_lx = 0, previous_ly = 0, previous_rx = 0, previous_ry = 0;
        if (buttons != previous_buttons || lx != previous_lx || ly != previous_ly || rx != previous_rx ||
            ry != previous_ry) {
            LOG_INFO("host input: buttons=0x%llx sticks=(%d,%d,%d,%d)",
                     static_cast<unsigned long long>(buttons), lx, ly, rx, ry);
            previous_buttons = buttons;
            previous_lx = lx;
            previous_ly = ly;
            previous_rx = rx;
            previous_ry = ry;
        }
    }
    hle::SetHostInput(buttons, lx, ly, rx, ry);
}

} // namespace

bool Init(const char* title) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        LOG_ERROR("SDL: %s", SDL_GetError());
        return false;
    }
    // NXRT_WINDOW_SIZE=<W>x<H> (default 1280x720) and NXRT_FULLSCREEN=1 (borderless desktop fullscreen); F11 toggles it.
    int window_w = 1280, window_h = 720;
    if (const char* size = std::getenv("NXRT_WINDOW_SIZE")) {
        int w = 0, h = 0;
        if (std::sscanf(size, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 180 && w <= 16384 && h <= 16384) {
            window_w = w;
            window_h = h;
        } else {
            LOG_WARN("window: ignoring NXRT_WINDOW_SIZE=%s (expected <W>x<H>, e.g. 1920x1080)", size);
        }
    }
    const char* fullscreen_env = std::getenv("NXRT_FULLSCREEN");
    const bool start_fullscreen = fullscreen_env && *fullscreen_env && std::strcmp(fullscreen_env, "0") != 0;
    g_window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, window_w, window_h,
                                SDL_WINDOW_RESIZABLE | SDL_WINDOW_VULKAN |
                                    (start_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0));
    if (!g_window) {
        LOG_ERROR("SDL window: %s", SDL_GetError());
        return false;
    }
    {
        int w = 0, h = 0;
        SDL_GetWindowSize(g_window, &w, &h);
        LOG_INFO("window: %dx%d fullscreen=%d", w, h, start_fullscreen ? 1 : 0);
    }
    SDL_StartTextInput();
    return true;
}

SDL_Window* Window() {
    return g_window;
}

std::u16string TakeTypedText() {
    std::lock_guard lock(g_text_lock);
    std::u16string t;
    t.swap(g_text);
    if (SDL_GetTicks() - g_text_tick > kTypedTextStaleMs) t.clear();
    return t;
}

u64 BeginInlineKeyboardInput() {
    return g_inline_keyboard_input.Begin();
}

void ConfirmInlineKeyboardInput() {
    g_inline_keyboard_input.PushKey(InlineKeyboardInputEventType::Confirm);
}

void TypeInlineKeyboardText(std::u16string text) {
    g_inline_keyboard_input.PushText(std::move(text));
}

void EndInlineKeyboardInput(u64 session) {
    g_inline_keyboard_input.End(session);
}

std::vector<InlineKeyboardInputEvent> TakeInlineKeyboardInput() {
    return g_inline_keyboard_input.Drain();
}

std::vector<InlineKeyboardInputEvent> WaitInlineKeyboardInput(u64 session) {
    return g_inline_keyboard_input.WaitAndDrain(session);
}

void NotifyFrame() {
    const u64 frame = ++g_frames;
    hle::OnInputFrame(frame);
    nxrt::SetHidReadTraceFrame(frame);
}

void RunEventLoop() {
    u64 last_frames = 0;
    u32 last_title = SDL_GetTicks();
    for (;;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            switch (e.type) {
            case SDL_QUIT:
                LOG_INFO("window closed");
                std::fflush(nullptr);
                std::_Exit(0);
            case SDL_CONTROLLERDEVICEADDED:
                if (!g_pad) {
                    g_pad = SDL_GameControllerOpen(e.cdevice.which);
                    if (g_pad) {
                        g_inline_captured_pad_buttons.fill(false);
                        g_inline_captured_pad_axes.fill(false);
                        g_inline_captured_pad_left_trigger = false;
                        g_inline_captured_pad_right_trigger = false;
                        LOG_INFO("controller: %s", SDL_GameControllerName(g_pad));
                    }
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (g_pad && e.cdevice.which == SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_pad))) {
                    SDL_GameControllerClose(g_pad);
                    g_pad = nullptr;
                    g_inline_captured_pad_buttons.fill(false);
                    g_inline_captured_pad_axes.fill(false);
                    g_inline_captured_pad_left_trigger = false;
                    g_inline_captured_pad_right_trigger = false;
                }
                break;
            case SDL_WINDOWEVENT:
                if (g_trace_keyboard_input &&
                    (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ||
                     e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)) {
                    LOG_INFO("keyboard trace: window focus %s",
                             e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED ? "gained" : "lost");
                }
                break;
            case SDL_TEXTINPUT: {
                if (g_trace_keyboard_input) {
                    LOG_INFO("keyboard trace: SDL_TEXTINPUT active=%u utf8_bytes=%zu",
                             g_inline_keyboard_input.Active() ? 1u : 0u, std::strlen(e.text.text));
                }
                if (g_inline_keyboard_input.Active()) {
                    std::u16string typed;
                    AppendUtf8(typed, e.text.text);
                    g_inline_keyboard_input.PushText(std::move(typed));
                } else {
                    std::lock_guard lock(g_text_lock);
                    const u32 t = SDL_GetTicks();
                    if (t - g_text_tick > kTypedTextStaleMs) g_text.clear();
                    g_text_tick = t;
                    AppendUtf8(g_text, e.text.text);
                    if (g_text.size() > kMaxTypedChars) g_text.resize(kMaxTypedChars);
                }
                break;
            }
            case SDL_KEYDOWN:
                if (e.key.keysym.scancode == SDL_SCANCODE_F11 && !e.key.repeat) {
                    const bool is_fullscreen = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
                    SDL_SetWindowFullscreen(g_window, is_fullscreen ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    LOG_INFO("window: fullscreen %s", is_fullscreen ? "off" : "on");
                }
                if (e.key.keysym.scancode == SDL_SCANCODE_F9 && !e.key.repeat && hle::InputRecordEnabled())
                    hle::InputRecordMarker(g_frames.load());
                if (e.key.keysym.scancode == SDL_SCANCODE_F10 && !e.key.repeat && tts_trace::Enabled())
                    tts_trace::Mark(g_frames.load());
                g_keyboard_game_input_filter.OnKeyDown(
                    static_cast<int>(e.key.keysym.scancode), g_inline_keyboard_input.Active());
                if (g_trace_keyboard_input && g_inline_keyboard_input.Active()) {
                    LOG_INFO("keyboard trace: SDL_KEYDOWN scancode=%d repeat=%u",
                             static_cast<int>(e.key.keysym.scancode), e.key.repeat ? 1u : 0u);
                }
                if (g_inline_keyboard_input.Active()) {
                    if (e.key.keysym.scancode == SDL_SCANCODE_RETURN ||
                        e.key.keysym.scancode == SDL_SCANCODE_KP_ENTER) {
                        if (!e.key.repeat)
                            g_inline_keyboard_input.PushKey(InlineKeyboardInputEventType::Confirm);
                    } else if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                        if (!e.key.repeat)
                            g_inline_keyboard_input.PushKey(InlineKeyboardInputEventType::Cancel);
                    } else if (e.key.keysym.scancode == SDL_SCANCODE_BACKSPACE) {
                        g_inline_keyboard_input.PushKey(InlineKeyboardInputEventType::Backspace);
                    }
                }
                break;
            case SDL_KEYUP:
                g_keyboard_game_input_filter.OnKeyUp(static_cast<int>(e.key.keysym.scancode));
                break;
            default:
                break;
            }
        }
        UpdateInput();
        const u32 now = SDL_GetTicks();
        if (now - last_title >= 1000) {
            const u64 f = g_frames.load();
            char title[128];
            std::snprintf(title, sizeof(title), "Tomodachi Life (nxrt " NXRT_VERSION ") - %llu fps",
                          static_cast<unsigned long long>(f - last_frames));
            SDL_SetWindowTitle(g_window, title);
            last_frames = f;
            last_title = now;
        }
        SDL_Delay(4);
    }
}

} // namespace nxrt::frontend
