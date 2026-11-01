// SPDX-License-Identifier: GPL-3.0-or-later
// Standalone test for src/audio/adpcm.h. Every expected value was worked out by
// hand from  out = ((code << scale << 11) + 0x400 + c[2p]*yn0 + c[2p+1]*yn1) >> 11
// (arithmetic shift, saturated to s16); the arithmetic is in the comments.
#include <algorithm>
#include <cstdio>
#include <vector>

#include "audio/adpcm.h"

using namespace nxrt::audio;

static int g_fails = 0;
#define CHECK_EQ(got, want)                                                                      \
    do {                                                                                         \
        const long g_ = (got), w_ = (want);                                                      \
        if (g_ != w_) {                                                                          \
            std::printf("FAIL line %d: %s = %ld, expected %ld\n", __LINE__, #got, g_, w_);       \
            ++g_fails;                                                                           \
        }                                                                                        \
    } while (0)

int main() {
    // --- nibble sign extension: 0..7 -> 0..7, 8..F -> -8..-1
    for (u32 n = 0; n < 16; ++n) CHECK_EQ(AdpcmNibble(n), n < 8 ? static_cast<long>(n) : static_cast<long>(n) - 16);
    CHECK_EQ(sizeof(AdpcmContext), 6);

    const s16 zero[16] = {};

    // --- 1. zero coefficients, scale 3 (header 0x03), nibbles 1,F,7,0
    //   1:  (1*8 <<11) + 1024 = 17408 >> 11 = 8      (8.5 floors to 8)
    //   F: -1*8: -16384 + 1024 = -15360 >> 11 = -8   (-7.5 floors to -8)
    //   7:  7*8 = 56 -> (56 << 11) + 1024 = 115712 >> 11 = 56
    //   0:  1024 >> 11 = 0
    {
        const u8 f[8] = {0x03, 0x1F, 0x70};
        s16 y0 = 0, y1 = 0;
        CHECK_EQ(AdpcmDecode(f, 0, zero, y0, y1), 8);
        CHECK_EQ(AdpcmDecode(f, 1, zero, y0, y1), -8);
        CHECK_EQ(AdpcmDecode(f, 2, zero, y0, y1), 56);
        CHECK_EQ(AdpcmDecode(f, 3, zero, y0, y1), 0);
        CHECK_EQ(y0, 0);   // history after the last sample
        CHECK_EQ(y1, 56);
    }

    // --- 2. one-tap prediction, pair 1 = (0x800, 0) = 1.0 * yn0; header 0x10 (pair 1, scale 0)
    //   yn0=100, code 1: 2048 + 1024 + 2048*100 = 207872 >> 11 = 101 (101.5)
    //   yn0=101, code 1: 2048 + 1024 + 2048*101 = 209920 >> 11 = 102 (102.5)
    {
        s16 c[16] = {};
        c[2] = 0x800;
        const u8 f[8] = {0x10, 0x11};
        s16 y0 = 100, y1 = 0;
        CHECK_EQ(AdpcmDecode(f, 0, c, y0, y1), 101);
        CHECK_EQ(AdpcmDecode(f, 1, c, y0, y1), 102);
        CHECK_EQ(y0, 102);
        CHECK_EQ(y1, 101);
    }

    // --- 3. two taps, pair 0 = (3000, -1000), header 0x00, all codes 0, yn0=10, yn1=20
    //   30000 - 20000 = 10000; +1024 = 11024 >> 11 = 5   (5.38)
    //   yn0=5,  yn1=10: 15000 - 10000 = 5000;  +1024 = 6024 >> 11 = 2 (2.94)
    //   yn0=2,  yn1=5 :  6000 -  5000 = 1000;  +1024 = 2024 >> 11 = 0 (0.99)
    {
        s16 c[16] = {};
        c[0] = 3000;
        c[1] = -1000;
        const u8 f[8] = {0x00};
        s16 y0 = 10, y1 = 20;
        CHECK_EQ(AdpcmDecode(f, 0, c, y0, y1), 5);
        CHECK_EQ(AdpcmDecode(f, 1, c, y0, y1), 2);
        CHECK_EQ(AdpcmDecode(f, 2, c, y0, y1), 0);
    }

    // --- 4. edge: last sample of a frame, then first of the next (frame layout)
    //   sample 13 is the LOW nibble of byte 7 of frame 0: header 0x00, zero coefs, code 5 -> 5
    //   sample 14 is the HIGH nibble of byte 1 of frame 1: header 0x01 (scale 1), code E=-2,
    //   pair 0 = (0x800, 0), yn0=5:  (-2*2 <<11) = -8192; +1024 + 2048*5 = 10240 -> 3072 >> 11 = 1 (1.5)
    //   sample 15 is the low nibble of byte 1 of frame 1: code 3: (3*2<<11)=12288 +1024 +2048*1 = 15360 >> 11 = 7
    {
        s16 c[16] = {};
        c[0] = 0x800;
        const u8 f[16] = {0x00, 0, 0, 0, 0, 0, 0, 0x05, //
                          0x01, 0xE3};
        s16 y0 = 0, y1 = 0;
        for (u32 i = 0; i < 13; ++i) CHECK_EQ(AdpcmDecode(f, i, c, y0, y1), 0);
        CHECK_EQ(AdpcmDecode(f, 13, c, y0, y1), 5);
        CHECK_EQ(AdpcmDecode(f, 14, c, y0, y1), 1);
        CHECK_EQ(AdpcmDecode(f, 15, c, y0, y1), 7);
    }

    // --- 5. edge: zero coefficients ignore history (but history still shifts)
    //   yn0=yn1=12345, header 0x00, code 0: 1024 >> 11 = 0
    {
        const u8 f[8] = {0x00};
        s16 y0 = 12345, y1 = 12345;
        CHECK_EQ(AdpcmDecode(f, 0, zero, y0, y1), 0);
        CHECK_EQ(y0, 0);
        CHECK_EQ(y1, 12345);
    }

    // --- 6. pair selection uses header bits 6..4: header 0x70 = pair 7 -> c[14], c[15]
    //   c[14]=0x800, yn0=7, code 0: 1024 + 2048*7 = 15360 >> 11 = 7 (7.5)
    {
        s16 c[16] = {};
        c[14] = 0x800;
        const u8 f[8] = {0x70};
        s16 y0 = 7, y1 = 0;
        CHECK_EQ(AdpcmDecode(f, 0, c, y0, y1), 7);
    }

    // --- 7. saturation, scale 15 (header 0x0F)
    //   code 7: 7*32768 = 229376 -> clamps to 32767;  code 8 (-8): -262144 -> clamps to -32768
    {
        const u8 f[8] = {0x0F, 0x78};
        s16 y0 = 0, y1 = 0;
        CHECK_EQ(AdpcmDecode(f, 0, zero, y0, y1), 32767);
        CHECK_EQ(AdpcmDecode(f, 1, zero, y0, y1), -32768);
    }

    // Differential regression against the per-sample ADPCM path in suyu's
    // audio_core/renderer/command/data_source/decode.cpp (DecodeAdpcm).
    // The reference uses independent byte/nibble extraction and 64-bit math.
    {
        constexpr s32 steps[16] = {0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1};
        auto next_random = [](u32& state) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            return state;
        };
        auto reference_sample = [&](const u8* data, u32 i, const s16* coefs, s16& y0, s16& y1) {
            const u32 frame_index = i / 14;
            const u32 in_frame = i % 14;
            const u8* frame = data + frame_index * 8;
            const u8 header = frame[0];
            const u8 packed = frame[1 + in_frame / 2];
            const u32 nibble = (in_frame & 1) ? (packed & 0x0F) : (packed >> 4);
            const s32 code = steps[nibble];
            const u32 pair = (header >> 4) & 7;
            const s64 xn = static_cast<s64>(code) * (s64{1} << (header & 0x0F));
            const s64 prediction = static_cast<s64>(coefs[pair * 2]) * y0 +
                                   static_cast<s64>(coefs[pair * 2 + 1]) * y1;
            const s64 numerator = xn * 2048 + 0x400 + prediction;
            s64 decoded = numerator / 2048;
            if (numerator < 0 && numerator % 2048) --decoded; // arithmetic right shift: floor
            const s16 sample = static_cast<s16>(std::clamp<s64>(decoded, -0x8000, 0x7FFF));
            y1 = y0;
            y0 = sample;
            return sample;
        };

        u32 random = 0x6D2B79F5;
        for (int stream = 0; stream < 128; ++stream) {
            constexpr u32 frame_count = 6;
            std::vector<u8> frames(frame_count * kAdpcmBytesPerFrame);
            for (u32 frame = 0; frame < frame_count; ++frame) {
                const u32 pair = next_random(random) % 8;
                const u32 scale = next_random(random) & 0x0F;
                frames[frame * 8] = static_cast<u8>((pair << 4) | scale);
                for (u32 byte = 1; byte < 8; ++byte)
                    frames[frame * 8 + byte] = static_cast<u8>(next_random(random));
            }
            s16 coefs[16];
            for (s16& coef : coefs)
                coef = static_cast<s16>(static_cast<s32>(next_random(random) % 8193) - 4096);
            s16 actual_y0 = static_cast<s16>(static_cast<s32>(next_random(random) & 0xFFFF) - 32768);
            s16 actual_y1 = static_cast<s16>(static_cast<s32>(next_random(random) & 0xFFFF) - 32768);
            s16 expected_y0 = actual_y0;
            s16 expected_y1 = actual_y1;
            for (u32 i = 0; i < frame_count * kAdpcmSamplesPerFrame; ++i) {
                const s16 expected = reference_sample(frames.data(), i, coefs, expected_y0, expected_y1);
                const s16 actual = AdpcmDecode(frames.data(), i, coefs, actual_y0, actual_y1);
                CHECK_EQ(actual, expected);
            }
            CHECK_EQ(actual_y0, expected_y0);
            CHECK_EQ(actual_y1, expected_y1);
        }
    }

    std::printf(g_fails ? "FAILED: %d\n" : "ALL OK\n", g_fails);
    return g_fails != 0;
}
