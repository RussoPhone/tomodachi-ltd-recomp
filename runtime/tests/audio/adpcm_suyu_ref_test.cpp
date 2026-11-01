// SPDX-License-Identifier: GPL-3.0-or-later
// Cross-check of src/audio/adpcm.h against the public suyu/yuzu audio renderer
// (src/audio_core/renderer/command/data_source/decode.cpp, DecodeAdpcm; GPL-2.0-or-later,
// compatible with this project's GPL-3.0-or-later).
//
// RefDecode below follows that function's *structure* (nibble cursor `position_in_frame`,
// header taken from a carried context when a call starts mid-frame, 16-entry step table, per
// call update of header/yn0/yn1) so that it is a different algorithm shape from AdpcmDecode,
// which indexes by absolute sample number. Both are driven with the same random streams, the
// same coefficient tables and the same random call chunking (the renderer asks for one
// 240-sample frame at a time, but an offset can start anywhere).
//
// Build: clang++ -std=c++20 -I src tests/audio/adpcm_suyu_ref_test.cpp -o /tmp/adpcm_suyu_ref_test
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#include "audio/adpcm.h"

using namespace nxrt::audio;

namespace {

struct RefContext {
    uint16_t header;
    int16_t yn0, yn1;
};

// suyu DecodeAdpcm, minus the guest-memory plumbing: `buf` is the whole wave buffer,
// `start_pos` the first sample to produce (start_offset + offset), `count` the samples to read.
void RefDecode(const std::vector<uint8_t>& buf, const int16_t* coefficients, RefContext& ctx, uint32_t start_pos,
               uint32_t count, std::vector<int16_t>& out) {
    constexpr uint32_t SamplesPerFrame = 14;
    constexpr uint32_t NibblesPerFrame = 16;
    static constexpr int32_t Steps[16] = {0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1};

    uint32_t frame_pos = start_pos % SamplesPerFrame;
    uint32_t position_in_frame = (start_pos / SamplesPerFrame) * NibblesPerFrame + frame_pos;
    if (frame_pos) position_in_frame += 2;
    // `wavebuffer[read_index]` in suyu is the byte at buffer + position_in_frame / 2.
    size_t base = position_in_frame / 2;
    uint32_t read_index = 0;

    uint32_t header = ctx.header;
    uint32_t coeff_index = (header >> 4) & 0xF;
    uint32_t scale = header & 0xF;
    int32_t coeff0 = coefficients[coeff_index * 2], coeff1 = coefficients[coeff_index * 2 + 1];
    int32_t yn0 = ctx.yn0, yn1 = ctx.yn1;

    auto decode_sample = [&](int32_t code) -> int16_t {
        const int32_t xn = code * (1 << scale);
        const int32_t prediction = coeff0 * yn0 + coeff1 * yn1;
        const int32_t sample = ((xn << 11) + 0x400 + prediction) >> 11;
        yn1 = yn0;
        yn0 = static_cast<int16_t>(std::clamp<int32_t>(sample, -0x8000, 0x7FFF));
        return static_cast<int16_t>(yn0);
    };

    uint32_t left = count;
    while (left > 0) {
        if ((position_in_frame % NibblesPerFrame) == 0) {
            header = buf[base + read_index++];
            coeff_index = (header >> 4) & 0xF;
            scale = header & 0xF;
            coeff0 = coefficients[coeff_index * 2];
            coeff1 = coefficients[coeff_index * 2 + 1];
            position_in_frame += 2;
            if (left >= SamplesPerFrame) {
                for (uint32_t i = 0; i < SamplesPerFrame / 2; ++i) {
                    const uint8_t b = buf[base + read_index++];
                    out.push_back(decode_sample(Steps[(b >> 4) & 0xF]));
                    out.push_back(decode_sample(Steps[b & 0xF]));
                }
                position_in_frame += SamplesPerFrame;
                left -= SamplesPerFrame;
                continue;
            }
        }
        uint8_t code = buf[base + read_index];
        if (position_in_frame & 1) {
            code &= 0xF;
            ++read_index;
        } else {
            code >>= 4;
        }
        out.push_back(decode_sample(Steps[code]));
        ++position_in_frame;
        --left;
    }
    ctx.header = static_cast<uint16_t>(header);
    ctx.yn0 = static_cast<int16_t>(yn0);
    ctx.yn1 = static_cast<int16_t>(yn1);
}

int g_fails = 0;

} // namespace

int main() {
    std::mt19937 rng(0xA11D10u);
    auto rnd = [&](uint32_t lo, uint32_t hi) { return lo + rng() % (hi - lo + 1); };

    uint64_t compared = 0;
    for (int stream = 0; stream < 400; ++stream) {
        const uint32_t frames = rnd(1, 40);
        std::vector<uint8_t> buf(frames * kAdpcmBytesPerFrame);
        for (auto& b : buf) b = static_cast<uint8_t>(rng());
        // Mostly legal headers (pair 0..7); a quarter of the streams use big scales to force clipping.
        const bool loud = stream % 4 == 0;
        for (uint32_t f = 0; f < frames; ++f) {
            const uint32_t pair = rnd(0, 7);
            const uint32_t scale = loud ? rnd(8, 15) : rnd(0, 11);
            buf[f * 8] = static_cast<uint8_t>((pair << 4) | scale);
        }
        int16_t coefs[16];
        for (auto& c : coefs) c = static_cast<int16_t>(rng() % 4096 * (rng() & 1 ? 1 : -1)); // |c| < 2.0 in Q11
        if (stream % 7 == 0) std::fill(std::begin(coefs), std::end(coefs), int16_t{0});

        const uint32_t total = frames * kAdpcmSamplesPerFrame;
        const uint32_t start = rnd(0, total - 1);
        // The context a game attaches at the buffer start: header of the frame holding `start`, plus history.
        RefContext ctx{buf[(start / 14) * 8], static_cast<int16_t>(rng()), static_cast<int16_t>(rng())};
        int16_t y0 = ctx.yn0, y1 = ctx.yn1;

        uint32_t pos = start;
        while (pos < total) {
            const uint32_t n = std::min<uint32_t>(rnd(1, 90), total - pos);
            std::vector<int16_t> want;
            RefDecode(buf, coefs, ctx, pos, n, want);
            for (uint32_t k = 0; k < n; ++k) {
                const int16_t got = AdpcmDecode(buf.data(), pos + k, coefs, y0, y1);
                ++compared;
                if (got != want[k]) {
                    if (g_fails < 10)
                        std::printf("FAIL stream %d sample %u: got %d, suyu ref %d\n", stream, pos + k, got, want[k]);
                    ++g_fails;
                }
            }
            if (y0 != ctx.yn0 || y1 != ctx.yn1) {
                if (g_fails < 10) std::printf("FAIL stream %d history after chunk at %u\n", stream, pos);
                ++g_fails;
            }
            pos += n;
        }
    }
    std::printf("adpcm_suyu_ref_test: %llu samples compared, %d failures\n", static_cast<unsigned long long>(compared),
                g_fails);
    return g_fails == 0 ? 0 : 1;
}
