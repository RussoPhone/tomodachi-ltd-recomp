// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>

#include "common.h"

namespace nxrt::hle {

// Gesture LIFO of the HID shared memory (libnx HidGestureSharedMemoryFormat, hid+0x3BA00).
// The HID sysmodule derives it from the touch screen; UI code that uses nn::hid gestures
// (tap, press, pan) never looks at the raw touch LIFO.
enum class GestureType : u32 {
    Idle = 0,
    Complete = 1,
    Cancel = 2,
    Touch = 3,
    Press = 4,
    Tap = 5,
    Pan = 6,
    Swipe = 7,
    Pinch = 8,
    Rotate = 9,
};

constexpr u32 kGestureAttributeNewTouch = 1u << 4;

struct GesturePoint {
    s32 x{};
    s32 y{};
};

struct GestureState {
    s64 sampling_number{};
    s64 sampling_number2{};
    u64 context_number{};
    u32 type{};
    u32 direction{};
    s32 x{};
    s32 y{};
    s32 delta_x{};
    s32 delta_y{};
    float velocity_x{};
    float velocity_y{};
    u32 attributes{};
    float scale{};
    float rotation_angle{};
    s32 point_count{};
    std::array<GesturePoint, 4> points{};
};
static_assert(sizeof(GestureState) == 0x68);

struct GestureLifo {
    s64 timestamp{};
    s64 total_buffer_count{17};
    s64 buffer_tail{};
    s64 buffer_count{};
    std::array<GestureState, 17> entries{};
    u8 padding[0xF8]{};
};
static_assert(sizeof(GestureLifo) == 0x800);

class GestureSampler {
public:
    // One call per 1/60 s HID sample. `x`/`y` are only meaningful while `pressed`
    // (or on the release sample, where they are the last position).
    void Write(GestureLifo& lifo, s64 sampling, bool pressed, u32 x, u32 y) {
        GestureState g{};
        // Same convention as the npad/touch LIFOs: the leading counter is twice the sample.
        g.sampling_number = sampling << 1;
        g.sampling_number2 = sampling;
        g.context_number = context_;
        g.x = static_cast<s32>(x);
        g.y = static_cast<s32>(y);

        if (pressed && !was_pressed_) {
            ++context_;
            g.context_number = context_;
            start_x_ = last_x_ = static_cast<s32>(x);
            start_y_ = last_y_ = static_cast<s32>(y);
            held_ = 0;
            panning_ = false;
            g.type = static_cast<u32>(GestureType::Touch);
            g.attributes = kGestureAttributeNewTouch;
            Fill(g, x, y);
        } else if (pressed) {
            ++held_;
            const s32 dx = static_cast<s32>(x) - start_x_;
            const s32 dy = static_cast<s32>(y) - start_y_;
            if (panning_ || dx * dx + dy * dy > kPanThreshold * kPanThreshold) {
                panning_ = true;
                g.type = static_cast<u32>(GestureType::Pan);
                g.delta_x = static_cast<s32>(x) - last_x_;
                g.delta_y = static_cast<s32>(y) - last_y_;
            } else if (held_ >= kPressSamples) {
                g.type = static_cast<u32>(GestureType::Press);
            } else {
                g.type = static_cast<u32>(GestureType::Touch);
            }
            last_x_ = static_cast<s32>(x);
            last_y_ = static_cast<s32>(y);
            Fill(g, x, y);
        } else if (was_pressed_) {
            // Finger lifted: a short, still touch is a tap.
            g.type = static_cast<u32>(!panning_ && held_ < kPressSamples ? GestureType::Tap
                                                                           : GestureType::Complete);
            Fill(g, x, y);
            complete_pending_ = g.type == static_cast<u32>(GestureType::Tap);
        } else if (complete_pending_) {
            complete_pending_ = false;
            g.type = static_cast<u32>(GestureType::Complete);
        } else {
            g.type = static_cast<u32>(GestureType::Idle);
        }

        lifo.total_buffer_count = lifo.entries.size();
        if (lifo.buffer_count < static_cast<s64>(lifo.entries.size()) - 1) ++lifo.buffer_count;
        const s64 next_tail = (lifo.buffer_tail + 1) % static_cast<s64>(lifo.entries.size());
        lifo.entries[static_cast<size_t>(next_tail)] = g;
        __atomic_store_n(&lifo.buffer_tail, next_tail, __ATOMIC_RELEASE);
        lifo.timestamp = sampling;
        was_pressed_ = pressed;
    }

private:
    static constexpr s32 kPanThreshold = 20;
    static constexpr s32 kPressSamples = 30;

    static void Fill(GestureState& g, u32 x, u32 y) {
        g.point_count = 1;
        g.points[0] = {static_cast<s32>(x), static_cast<s32>(y)};
        g.scale = 1.0f;
    }

    bool was_pressed_{};
    bool panning_{};
    bool complete_pending_{};
    u64 context_{};
    s32 start_x_{}, start_y_{}, last_x_{}, last_y_{};
    s32 held_{};
};

} // namespace nxrt::hle
