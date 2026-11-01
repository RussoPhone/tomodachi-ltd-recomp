// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdlib>

#include "common.h"

namespace nxrt::hle {

struct TouchState {
    u64 delta_time{};
    u32 attribute{};
    u32 finger{};
    u32 position_x{};
    u32 position_y{};
    u32 diameter_x{};
    u32 diameter_y{};
    s32 rotation_angle{};
};
static_assert(sizeof(TouchState) == 0x28);

struct TouchScreenState {
    s64 sampling_number{};
    s32 entry_count{};
    u8 reserved[4]{};
    std::array<TouchState, 16> states{};
};
static_assert(sizeof(TouchScreenState) == 0x290);

struct TouchScreenEntry {
    s64 sampling_number{};
    TouchScreenState state{};
};

struct TouchScreenLifo {
    s64 timestamp{};
    s64 total_buffer_count{17};
    s64 buffer_tail{};
    s64 buffer_count{};
    std::array<TouchScreenEntry, 17> entries{};
};
static_assert(sizeof(TouchScreenLifo) == 0x2C38);

class TouchInputSampler {
public:
    void Write(TouchScreenLifo& lifo, s64 sampling, bool pressed, u32 x, u32 y) {
        TouchScreenState state{};
        state.sampling_number = sampling;
        if (pressed || was_pressed_) {
            state.entry_count = 1;
            TouchState& touch = state.states[0];
            if (pressed && !was_pressed_) {
                ++finger_id_;
                touch_start_sampling_ = sampling;
            }
            touch.attribute = pressed ? (was_pressed_ ? 0u : 1u) : 2u;
            touch.finger = finger_id_;
            // One sample is 1/60 s. T1 experiment: 0 = since touch start (ns), 1 = since previous sample.
            static const int mode = [] {
                const char* v = std::getenv("NXRT_TOUCH_DELTA");
                return v ? std::atoi(v) : 0;
            }();
            touch.delta_time = mode == 1 ? 16666667ull
                                         : static_cast<u64>(sampling - touch_start_sampling_) * 16666667ull;
            touch.position_x = x;
            touch.position_y = y;
            touch.diameter_x = 15;
            touch.diameter_y = 15;
        }

        lifo.total_buffer_count = lifo.entries.size();
        if (lifo.buffer_count < static_cast<s64>(lifo.entries.size()) - 1) ++lifo.buffer_count;
        const s64 next_tail = (lifo.buffer_tail + 1) % static_cast<s64>(lifo.entries.size());
        auto& entry = lifo.entries[static_cast<size_t>(next_tail)];
        entry.sampling_number = sampling << 1;
        entry.state = state;
        __atomic_store_n(&lifo.buffer_tail, next_tail, __ATOMIC_RELEASE);
        lifo.timestamp = sampling;
        was_pressed_ = pressed;
    }

    static TouchScreenState Current(const TouchScreenLifo& lifo) {
        return lifo.entries[static_cast<size_t>(lifo.buffer_tail)].state;
    }

private:
    bool was_pressed_{};
    u32 finger_id_{};
    s64 touch_start_sampling_{};
};

} // namespace nxrt::hle
