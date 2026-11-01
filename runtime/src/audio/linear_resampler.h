// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace nxrt::audio {

struct LinearResamplerState {
    float last = 0.0f;
    float current = 0.0f;
    double fraction = 0.0;
    bool primed = false;
    bool ended = false;
};

// Read returns a normalized source sample and sets ended when no sample exists.
template <typename ReadSample>
float ResampleLinear(LinearResamplerState& state, double step, ReadSample&& read) {
    if (!state.primed) {
        bool ended = false;
        state.last = state.current = read(ended);
        state.ended = ended;
        // `fraction` is the position within [last,current]. Starting at 1 makes
        // the first output the first source sample while keeping one sample of
        // phase headroom for the first rate conversion step.
        state.fraction = ended ? 0.0 : 1.0;
        state.primed = true;
    }

    const float out = state.last + (state.current - state.last) * static_cast<float>(state.fraction);
    state.fraction += step;
    while (state.fraction >= 1.0) {
        state.fraction -= 1.0;
        state.last = state.current;
        bool ended = false;
        state.current = read(ended);
        if (ended) {
            // Starved: play silence, but ask the source again on the next step. A streaming voice
            // (TTS speech, started before its first wave buffer is appended) must pick up buffers that
            // arrive later; latching "ended" for good left such a voice mute forever.
            const bool was_ended = state.ended;
            state.current = 0.0f;
            state.ended = true;
            if (was_ended) {
                state.last = 0.0f;
                state.fraction = 0.0;
                break;
            }
        } else {
            state.ended = false;
        }
    }
    return out;
}

} // namespace nxrt::audio
