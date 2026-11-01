// SPDX-License-Identifier: GPL-3.0-or-later
// The game window and input. Runs on the process's main thread; the game itself
// runs on its own threads. Keyboard and the first SDL game controller drive
// player 1's Pro Controller.
#include "frontend/window.h"
#include "guest_abi.h"
#include "memory.h"
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
#include <map>
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

// The mouse steers the game's hand pointer. The game updates the hand once a frame in a function at main+0x22dce0 (this = the
// pointer object: a halfword at +0x14 whose bit 0 says the hand is shown, the position in a 1280x720 screen at +0x118/+0x11c). The
// installer puts a call to nxrt_hook_pointer at that function's entry (scripts/slim-export.py); with the mouse in use the hook
// writes the mouse position into the pointer, so hovering, picking and dragging happen where the mouse is. NXRT_MOUSE_HAND=0 off.
std::atomic<bool> g_mouse_hand_on{false};     // the mouse moved over the window and no stick has taken over since
std::atomic<u32> g_mouse_moved_tick{0};
std::atomic<float> g_mouse_hand_x{0.0f}, g_mouse_hand_y{0.0f};
std::atomic<bool> g_hand_shown{false};        // the game's hand is visible (set by the hook every frame)
std::atomic<bool> g_hand_honored{true};       // the game keeps the position the hook wrote (false on the island, where it recentres the hand)
std::atomic<float> g_mouse_gain{0.0f};          // stick units per pixel moved in a frame (F6 / F7 change it while playing)
std::atomic<float> g_hand_game_x{640.0f}, g_hand_game_y{360.0f};  // where the game has the hand (read by the hook)
std::atomic<bool> g_capture{false};           // F8: the mouse is captured by the window and works like an analog stick
std::atomic<int> g_rel_dx{0}, g_rel_dy{0};    // mouse movement since the last frame while captured

void SetCapture(bool on) {
    if (g_capture.exchange(on) == on) return;
    SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
    g_rel_dx.store(0);
    g_rel_dy.store(0);
    if (on) g_mouse_hand_on.store(false);
    LOG_INFO("mouse: %s", on ? "captured by the window: it moves the left stick (F8 releases it)" : "released");
}
std::atomic<u32> g_edge_since{0};             // when the mouse entered an edge zone (0 = not in one)

bool MouseHandEnabled() {
    static const bool on = [] {
        const char* v = std::getenv("NXRT_MOUSE_HAND");
        return !(v && std::strcmp(v, "0") == 0);
    }();
    return on;
}

void NoteMouseMotion(int x, int y) {
    int w = 1280, h = 720;
    SDL_GetWindowSize(g_window, &w, &h);
    g_mouse_hand_x.store(std::clamp(static_cast<float>(x) * 1280.0f / static_cast<float>(std::max(w, 1)), 0.0f, 1279.0f));
    g_mouse_hand_y.store(std::clamp(static_cast<float>(y) * 720.0f / static_cast<float>(std::max(h, 1)), 0.0f, 719.0f));
    g_mouse_moved_tick.store(SDL_GetTicks());
    if (MouseHandEnabled()) g_mouse_hand_on.store(true);
}

// Mouse wheel = the island zoom: scrolling up pushes the right stick up for a moment (zoom in), scrolling down pulls it down
// (NXRT_WHEEL_INVERT=1 swaps them).
// Every wheel tick keeps the button down for about a tenth of a second, so a long scroll is a continuous zoom.
std::atomic<u32> g_wheel_up_until{0}, g_wheel_down_until{0};
constexpr u32 kWheelTickMs = 120, kWheelMaxMs = 400;

void NoteWheel(int y, bool flipped) {
    if (flipped) y = -y;
    if (y == 0) return;
    auto& until = y > 0 ? g_wheel_up_until : g_wheel_down_until;
    const u32 now = SDL_GetTicks();
    const u32 base = std::max(until.load(), now);
    until.store(std::min(base + kWheelTickMs * static_cast<u32>(std::abs(y)), now + kWheelMaxMs));
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
    if (!keyboard_active) {
        // Right click = B (back/cancel); the left click is the touch screen (above).
        if (mouse_buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) buttons |= hle::kButtonB;
        static const bool wheel_invert = [] {
            const char* v = std::getenv("NXRT_WHEEL_INVERT");
            return v && *v && std::strcmp(v, "0") != 0;
        }();
        // The wheel is the island zoom, which the game takes from the right stick's vertical axis (up = closer).
        const u32 now = SDL_GetTicks();
        if (now < g_wheel_up_until.load()) ry = wheel_invert ? -32767 : 32767;
        if (now < g_wheel_down_until.load()) ry = wheel_invert ? 32767 : -32767;
    }
    // A stick or the d-pad takes the hand back from the mouse until the mouse moves again.
    if (std::abs(lx) > 8000 || std::abs(ly) > 8000 || (buttons & (hle::kButtonUp | hle::kButtonDown | hle::kButtonLeft | hle::kButtonRight)))
        g_mouse_hand_on.store(false);
    {
        const bool hand_in_use = g_mouse_hand_on.load() && g_hand_shown.load() && g_hand_honored.load() && !keyboard_active;
        if (hand_in_use && (mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT))) buttons |= hle::kButtonA;  // click = A where the hand is
        // With the hand on the mouse, a left click is A and not a touch; otherwise it stays the touch screen.
        if (hand_in_use) hle::SetHostTouch(false, touch_x, touch_y);
        // Edge scrolling, as in a real-time strategy game: with the mouse parked at the edge of the window for a moment, the left
        // stick pushes that way (it pans the island). Off while a button is held, while typing or while the hand follows the mouse.
        static const bool edge_scroll = [] {
            const char* v = std::getenv("NXRT_EDGE_SCROLL");
            return !(v && std::strcmp(v, "0") == 0);
        }();
        // Drag with the middle button to move the camera like a joystick: the island follows the mouse as if it were grabbed (the left
        // stick pans); with Shift held the drag turns (right stick, horizontal) and zooms (right stick, vertical). NXRT_DRAG_GAIN tunes it.
        static int prev_x = 0, prev_y = 0;
        static bool dragging = false;
        static const float drag_gain = [] {
            const char* v = std::getenv("NXRT_DRAG_GAIN");
            const float g = v ? std::strtof(v, nullptr) : 0.0f;
            return g > 0.0f ? g : 1500.0f;  // stick units per pixel of mouse movement per frame
        }();
        if ((mouse_buttons & SDL_BUTTON(SDL_BUTTON_MIDDLE)) && !keyboard_active) {
            if (dragging) {
                const float ddx = static_cast<float>(mouse_x - prev_x), ddy = static_cast<float>(mouse_y - prev_y);
                const auto clamp_stick = [](float v) { return static_cast<s32>(std::clamp(v, -32767.0f, 32767.0f)); };
                if (SDL_GetModState() & KMOD_SHIFT) {
                    rx = clamp_stick(ddx * drag_gain);
                    ry = clamp_stick(-ddy * drag_gain);
                } else if (!lx && !ly) {
                    lx = clamp_stick(-ddx * drag_gain);
                    ly = clamp_stick(ddy * drag_gain);
                }
            }
            dragging = true;
        } else {
            dragging = false;
        }
        prev_x = mouse_x;
        prev_y = mouse_y;
        // What the mouse is doing to the game, once every two seconds while it moves (for bug reports).
        {
            static u32 last_log = 0;
            const u32 t = SDL_GetTicks();
            if (t - g_mouse_moved_tick.load() < 1000 && t - last_log > 2000) {
                last_log = t;
                LOG_INFO("mouse: hand follows=%d shown=%d honored=%d edge-dwell=%d", g_mouse_hand_on.load() ? 1 : 0,
                         g_hand_shown.load() ? 1 : 0, g_hand_honored.load() ? 1 : 0, g_edge_since.load() ? 1 : 0);
            }
        }
        // The game hides an idle hand after a couple of seconds and only shows it again for stick input, so while the mouse moves the
        // left stick gets a nudge too small to move anything else (its sign alternates every frame and cancels out). Not where the game
        // recentres the hand (the island, where the left stick pans the camera): there g_hand_honored is false and no nudge is made.
        if (g_mouse_hand_on.load() && g_hand_honored.load() && SDL_GetTicks() - g_mouse_moved_tick.load() < 600 && !lx && !ly &&
            !keyboard_active) {
            static bool flip = false;
            flip = !flip;
            lx = flip ? 4500 : -4500;
        }
        constexpr int kEdge = 14;          // pixels of the window that count as the edge
        constexpr u32 kEdgeDwellMs = 250;  // the mouse has to stay there this long (so clicking a corner button does not scroll)
        const bool inside = (SDL_GetWindowFlags(g_window) & SDL_WINDOW_MOUSE_FOCUS) != 0;
        int dx = 0, dy = 0;
        if (inside && edge_scroll && !hand_in_use && !keyboard_active && mouse_buttons == 0) {
            if (mouse_x < kEdge) dx = -1;
            else if (mouse_x >= window_width - kEdge) dx = 1;
            if (mouse_y < kEdge) dy = 1;   // up
            else if (mouse_y >= window_height - kEdge) dy = -1;
        }
        const u32 tick = SDL_GetTicks();
        if (dx == 0 && dy == 0) {
            g_edge_since.store(0);
        } else {
            if (g_edge_since.load() == 0) g_edge_since.store(tick | 1);
            if (tick - g_edge_since.load() >= kEdgeDwellMs && !lx && !ly) {
                lx = dx * 32767;
                ly = dy * 32767;
            }
        }
    }
    if (g_capture.load() && !keyboard_active) {
        // Captured mouse = an analog stick: the movement of each frame is the stick (the hand in menus, the camera on the island).
        // Shift turns it into the right stick (rotate and zoom). Left click is A, right click is B, the wheel zooms.
        if (g_mouse_gain.load() <= 0.0f) {
            const char* v = std::getenv("NXRT_MOUSE_GAIN");
            const float g = v ? std::strtof(v, nullptr) : 0.0f;
            g_mouse_gain.store(g > 0.0f ? g : 3500.0f);
        }
        const float gain = g_mouse_gain.load();
        // This runs every ~4 ms but the game reads its input once per frame, so the movement is measured over a window of one frame
        // (33 ms) and the resulting velocity is held until the next window; a lone 4 ms spike would be missed by the game.
        static float acc_x = 0, acc_y = 0, hold_x = 0, hold_y = 0;
        static u32 window_start = 0;
        const u32 now_tick = SDL_GetTicks();
        const float flick_dx = static_cast<float>(g_rel_dx.exchange(0)), flick_dy = static_cast<float>(g_rel_dy.exchange(0));
        acc_x += flick_dx;
        acc_y += flick_dy;
        if (window_start == 0) window_start = now_tick;
        // Acceleration: keep moving the mouse the same way and the push grows (up to NXRT_MOUSE_ACCEL times, default 6, over about a third of
        // a second), so crossing the screen does not take dragging the mouse across the desk. A reversal or a pause starts over.
        static float boost_x = 1.0f, boost_y = 1.0f;
        static int idle_x = 0, idle_y = 0;
        static const float max_boost = [] {
            const char* v = std::getenv("NXRT_MOUSE_ACCEL");
            const float m = v ? std::strtof(v, nullptr) : 0.0f;
            return m >= 1.0f ? m : 6.0f;
        }();
        const auto accelerate = [&](float moved, float& boost, int& idle, float& previous_sign) {
            const float sign = moved > 0 ? 1.0f : moved < 0 ? -1.0f : 0.0f;
            if (sign == 0.0f) {
                if (++idle >= 5) boost = 1.0f;  // about 150 ms without movement
            } else {
                idle = 0;
                if (sign == previous_sign) boost = std::min(max_boost, boost + (max_boost - 1.0f) / 10.0f);
                else boost = 1.0f;
                previous_sign = sign;
            }
        };
        static float sign_x = 0, sign_y = 0;
        if (now_tick - window_start >= 33) {
            const float scale = 33.3f / static_cast<float>(now_tick - window_start);
            accelerate(acc_x, boost_x, idle_x, sign_x);
            accelerate(acc_y, boost_y, idle_y, sign_y);
            hold_x = acc_x * scale * gain * boost_x;
            hold_y = acc_y * scale * gain * boost_y;
            acc_x = acc_y = 0;
            window_start = now_tick;
        }
        // The game's hand barely moves for a small stick push, so the response is bent: small movements are boosted (power curve) and
        // anything that moves at all is pushed past the game's dead zone.
        const auto clamp_stick = [](float v) {
            const float n = std::min(1.0f, std::fabs(v) / 32767.0f);
            if (n <= 0.0f) return s32{0};
            const float out = std::max(0.28f, std::pow(n, 0.7f)) * 32767.0f;
            return static_cast<s32>(v < 0 ? -out : out);
        };
        // Where the game takes the hand position we write (menus), the captured mouse moves a pointer of its own in the 1280x720
        // screen, with the same acceleration, and the hook puts the hand there: fast and exact, no stick speed limit. A small
        // alternating stick nudge keeps the hand shown. Where it does not (the island), the movement is the stick, as above.
        static bool pointer_mode = false;
        static float pointer_x = 640.0f, pointer_y = 360.0f;
        const bool want_pointer = g_hand_shown.load() && g_hand_honored.load() && !(SDL_GetModState() & KMOD_SHIFT);
        if (want_pointer && !pointer_mode) {
            pointer_x = g_hand_game_x.load();
            pointer_y = g_hand_game_y.load();
        }
        pointer_mode = want_pointer;
        static u32 pointer_window = 0;
        if (pointer_mode) {
            if (window_start != pointer_window) {  // a 33 ms window just closed: hold_x / gain is what the mouse moved, boosted
                pointer_window = window_start;
                int w = 1280, h = 720;
                SDL_GetWindowSize(g_window, &w, &h);
                const float speed = gain / 3500.0f * 2.0f;
                pointer_x = std::clamp(pointer_x + hold_x / std::max(gain, 1.0f) * speed * 1280.0f / static_cast<float>(std::max(w, 1)), 0.0f, 1279.0f);
                pointer_y = std::clamp(pointer_y + hold_y / std::max(gain, 1.0f) * speed * 720.0f / static_cast<float>(std::max(h, 1)), 0.0f, 719.0f);
            }
            g_mouse_hand_x.store(pointer_x);
            g_mouse_hand_y.store(pointer_y);
            g_mouse_hand_on.store(true);
            static bool flip = false;
            flip = !flip;
            lx = flip ? 4500 : -4500;
            ly = 0;
        } else if (!g_hand_shown.load() && !(SDL_GetModState() & KMOD_SHIFT)) {
            // No hand on this screen (the Mii creator, some menus): the game moves a selection with stick flicks, so the mouse sends
            // them: every ~45 pixels of movement one quick flick that way (more flicks when the mouse is quicker; the sensitivity
            // changes the distance). A flick is a full push for 70 ms with a pause after it, so the game counts each one.
            g_mouse_hand_on.store(false);
            struct Flick {
                float acc = 0;
                int pending = 0;
                u32 active_until = 0, gap_until = 0;
                int dir = 0;
            };
            static Flick fx, fy;
            static u32 last_acc_tick = 0;
            const u32 now_flick = SDL_GetTicks();
            const float threshold = 45.0f * 3500.0f / std::max(gain, 300.0f);
            const auto drive = [&](Flick& f, float moved, s32& out, int sign_out) {
                f.acc += moved;
                while (std::fabs(f.acc) >= threshold && std::abs(f.pending) < 4) {
                    const int d = f.acc > 0 ? 1 : -1;
                    f.pending += d;
                    f.acc -= static_cast<float>(d) * threshold;
                }
                if (now_flick >= f.active_until && now_flick >= f.gap_until && f.pending != 0) {
                    f.dir = f.pending > 0 ? 1 : -1;
                    f.pending -= f.dir;
                    f.active_until = now_flick + 70;
                    f.gap_until = now_flick + 140;
                }
                if (now_flick < f.active_until) out = sign_out * f.dir * 32767;
            };
            (void)last_acc_tick;
            drive(fx, flick_dx, lx, 1);
            drive(fy, flick_dy, ly, -1);
        } else {
            g_mouse_hand_on.store(false);
            if (SDL_GetModState() & KMOD_SHIFT) {
                rx = clamp_stick(hold_x);
                if (!ry) ry = clamp_stick(-hold_y);
            } else {
                lx = clamp_stick(hold_x);
                ly = clamp_stick(-hold_y);
            }
        }
        if (mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) buttons |= hle::kButtonA;
        hle::SetHostTouch(false, touch_x, touch_y);
    }
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
            case SDL_MOUSEMOTION:
                if (g_capture.load()) {
                    g_rel_dx += e.motion.xrel;
                    g_rel_dy += e.motion.yrel;
                } else {
                    NoteMouseMotion(e.motion.x, e.motion.y);
                }
                break;
            case SDL_MOUSEWHEEL:
                NoteWheel(e.wheel.y, e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                // The Switch's HOME button has no key of its own on a PC: the Guide/Home button of a controller closes the game, the
                // same way pressing HOME and closing the software does on the console (the game asks for it after some dialogs).
                if (e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE && !g_inline_keyboard_input.Active()) {
                    LOG_INFO("window: HOME (controller guide button) pressed, closing the game");
                    std::fflush(nullptr);
                    std::_Exit(0);
                }
                break;
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
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) SetCapture(false);
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
                if (e.key.keysym.scancode == SDL_SCANCODE_HOME && !e.key.repeat && !g_inline_keyboard_input.Active()) {
                    LOG_INFO("window: HOME key pressed, closing the game");
                    std::fflush(nullptr);
                    std::_Exit(0);
                }
                if (e.key.keysym.scancode == SDL_SCANCODE_F8 && !e.key.repeat) SetCapture(!g_capture.load());
                if ((e.key.keysym.scancode == SDL_SCANCODE_F6 || e.key.keysym.scancode == SDL_SCANCODE_F7) && !e.key.repeat) {
                    float g = g_mouse_gain.load() > 0.0f ? g_mouse_gain.load() : 3500.0f;
                    g = std::clamp(e.key.keysym.scancode == SDL_SCANCODE_F7 ? g * 1.25f : g / 1.25f, 300.0f, 40000.0f);
                    g_mouse_gain.store(g);
                    LOG_INFO("mouse: sensitivity %.0f (F6 slower, F7 faster)", g);
                }
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

// Called by the recompiled game at the entry of the hand-pointer update (see above); `c->x[0]` is the pointer object.
namespace nxrt::frontend {
extern "C" void nxrt_hook_pointer(GuestContext* c) {
    // NXRT_MOUSE_TEST="<x>,<y>" (test only): a mouse parked at that point of the 1280x720 screen, for runs without a window.
    static const bool tested = [] {
        float x = 0, y = 0;
        if (const char* v = std::getenv("NXRT_MOUSE_TEST"); v && std::sscanf(v, "%f,%f", &x, &y) == 2) {
            g_mouse_hand_x.store(x);
            g_mouse_hand_y.store(y);
            g_mouse_hand_on.store(true);
            LOG_INFO("mouse hand: test position %g,%g", x, y);
            return true;
        }
        return false;
    }();
    (void)tested;
    const u64 self = c->x[0];
    if (!self || !g_memory.IsMapped(self, 0x120)) return;
    const bool shown = (g_memory.Read<u16>(self + 0x14) & 1) != 0;
    static bool logged_hook = false;
    if (!logged_hook) {
        logged_hook = true;
        LOG_INFO("mouse hand: the pointer hook is running (hand shown=%d)", shown ? 1 : 0);
    }
    {
        // Census of the pointer objects the game updates through this function (the menus may use another one than the island):
        // every three seconds the log lists each with how often it ran, whether its hand is shown and where it is.
        struct Census {
            u32 calls = 0;
            bool shown = false;
            float pos[2] = {0, 0};
        };
        static std::mutex census_lock;
        static std::map<u64, Census> census;
        static u32 last_report = 0;
        float pos_now[2] = {0, 0};
        g_memory.ReadBlock(self + 0x118, pos_now, sizeof(pos_now));
        std::lock_guard lock(census_lock);
        Census& entry = census[self];
        ++entry.calls;
        entry.shown = shown;
        entry.pos[0] = pos_now[0];
        entry.pos[1] = pos_now[1];
        const u32 now = SDL_GetTicks();
        if (now - last_report >= 3000) {
            last_report = now;
            for (auto& [address, e] : census) {
                if (e.calls) {
                    LOG_INFO("pointers: self=%llx calls=%u shown=%d pos=%g,%g", static_cast<unsigned long long>(address), e.calls,
                             e.shown ? 1 : 0, e.pos[0], e.pos[1]);
                }
                e.calls = 0;
            }
        }
    }
    g_hand_shown.store(shown);
    if (shown && g_mouse_hand_on.load()) {
        // Does the game keep what was written? On the island it recentres the hand every frame, and then the mouse cannot steer it.
        static float last[2] = {-1.0f, -1.0f};
        static int mismatched = 0;
        float now_pos[2];
        g_memory.ReadBlock(self + 0x118, now_pos, sizeof(now_pos));
        g_hand_game_x.store(now_pos[0]);
        g_hand_game_y.store(now_pos[1]);
        if (last[0] >= 0.0f) {
            const bool kept = std::fabs(now_pos[0] - last[0]) < 40.0f && std::fabs(now_pos[1] - last[1]) < 40.0f;
            mismatched = kept ? 0 : mismatched + 1;
            g_hand_honored.store(mismatched < 8);
        }
        const float xy[2] = {g_mouse_hand_x.load(), g_mouse_hand_y.load()};
        last[0] = xy[0];
        last[1] = xy[1];
        static const bool debug = std::getenv("NXRT_MOUSE_DEBUG") != nullptr;
        if (debug) {
            static u32 n = 0;
            float before[2];
            g_memory.ReadBlock(self + 0x118, before, sizeof(before));
            if (n++ % 60 == 0) LOG_INFO("mouse hand: self=%llx pos was %g,%g -> %g,%g", static_cast<unsigned long long>(self), before[0], before[1], xy[0], xy[1]);
        }
        g_memory.WriteBlock(self + 0x118, xy, sizeof(xy));
    }
}
} // namespace nxrt::frontend
