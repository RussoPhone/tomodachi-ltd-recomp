// SPDX-License-Identifier: GPL-3.0-or-later
// Nintendo DSP-ADPCM as the audio renderer uses it: 8-byte frames, one header
// byte (high nibble: coefficient pair, low nibble: scale) and 14 signed 4-bit
// samples. Each voice carries a table of 8 coefficient pairs (s16[16]) and the
// two previous output samples as history.
#pragma once

#include <algorithm>

#include "../common.h"

namespace nxrt::audio {

constexpr u32 kAdpcmSamplesPerFrame = 14;
constexpr u32 kAdpcmBytesPerFrame = 8;

// The decoder context the game may attach to a wave buffer (6 bytes): the
// header of the frame being decoded and the history at the buffer's start.
struct AdpcmContext {
    u16 header;
    s16 yn0, yn1;
};
static_assert(sizeof(AdpcmContext) == 6);

// Sign-extends a nibble.
constexpr s32 AdpcmNibble(u32 n) { return (n & 8) ? static_cast<s32>(n & 0xF) - 16 : static_cast<s32>(n & 0xF); }

// Decodes the sample at index `i` of a stream, reading from `frames` (frame 0
// is the first 8 bytes). `coefs` is the voice's s16[16] table; `yn0` is the
// most recent output and `yn1` the one before; both are updated.
inline s16 AdpcmDecode(const u8* frames, u32 i, const s16* coefs, s16& yn0, s16& yn1) {
    const u8* frame = frames + static_cast<size_t>(i / kAdpcmSamplesPerFrame) * kAdpcmBytesPerFrame;
    const u32 n = i % kAdpcmSamplesPerFrame;
    const u8 header = frame[0];
    const u8 byte = frame[1 + n / 2];
    const s32 code = AdpcmNibble((n & 1) ? byte : byte >> 4);
    const u32 pair = (header >> 4) & 7;
    const s32 xn = code * (1 << (header & 0xF));
    const s32 prediction = coefs[pair * 2] * yn0 + coefs[pair * 2 + 1] * yn1;
    const s32 value = ((xn << 11) + 0x400 + prediction) >> 11;
    const s16 out = static_cast<s16>(std::clamp(value, -0x8000, 0x7FFF));
    yn1 = yn0;
    yn0 = out;
    return out;
}

} // namespace nxrt::audio
