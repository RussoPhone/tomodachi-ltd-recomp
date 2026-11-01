// SPDX-License-Identifier: GPL-3.0-or-later
// Numeric comparison of the renderer's mix arithmetic with suyu's MixRampCommand
// (src/audio_core/renderer/command/mix/mix_ramp.cpp, GPL-2.0-or-later).
//
// NOTE: the renderer's mix lives in private Renderer methods (src/audio/renderer.cpp,
// AddToMix / final stage) that need guest memory and an audren parameter blob, so this test
// re-states ITS formula (`dst[n] += src[n] * gain`, float, clamp at the sink) next to the suyu
// one. It checks the arithmetic, not the linkage; if renderer.cpp's mix changes, update here.
//
// Build: clang++ -std=c++20 tests/audio/mix_suyu_ref_test.cpp -o /tmp/mix_suyu_ref_test
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

namespace {

// suyu ApplyMixRamp<Q>: volume and sample as signed fixed point with Q fractional bits,
// to_int() rounds the fraction (round_up adds half of the fractional part before truncating).
int32_t SuyuMix(int32_t out, int32_t in, double volume, int Q) {
    const int64_t vol = static_cast<int64_t>(volume * (1LL << Q));
    int64_t sample = static_cast<int64_t>(in) * vol; // Q fractional bits
    int64_t data = sample + out * (1LL << Q);
    const int64_t frac = data & ((1LL << Q) - 1);
    data += frac >> 1; // round_up()
    return static_cast<int32_t>(data >> Q);
}

} // namespace

int main() {
    std::mt19937 rng(7);
    int fails = 0;
    double worst[2] = {0, 0};
    for (int q = 0; q < 2; ++q) {
        const int Q = q == 0 ? 15 : 23;
        for (int i = 0; i < 200000; ++i) {
            const int16_t x = static_cast<int16_t>(rng());
            const double g = (rng() % 20001) / 10000.0; // 0 .. 2.0
            const int32_t suyu = SuyuMix(0, x, g, Q);
            const float ours = (x / 32768.0f) * static_cast<float>(g); // float pipeline, 1.0 = full scale
            const double diff = std::fabs(ours * 32768.0 - suyu);      // in s16 LSBs
            worst[q] = std::max(worst[q], diff);
        }
        // suyu truncates the gain to Q fractional bits (error up to |x| * 2^-Q = 1 LSB at Q15), and its
        // FixedPoint::round_up() adds HALF OF THE FRACTION (not 0.5) before truncating: floor(n + 1.5 f),
        // which is off by up to ~0.67 LSB from the exact product. Neither is audible (< -90 dBFS).
        const double budget = 0.67 + 32768.0 / (1 << Q) + 1e-3;
        std::printf("Q%d: worst |float - suyu| = %.3f LSB (budget %.3f)\n", Q, worst[q], budget);
        if (worst[q] > budget) ++fails;
    }

    // Stage count: three accumulating stages (voice -> mix -> final) can add three roundings in suyu
    // (each up to ~1.7 LSB at Q15); the float path adds none. Show the worst case of three chained gains.
    double chain = 0;
    for (int i = 0; i < 100000; ++i) {
        const int16_t x = static_cast<int16_t>(rng());
        const double g1 = (rng() % 10001) / 10000.0, g2 = (rng() % 10001) / 10000.0, g3 = (rng() % 10001) / 10000.0;
        const int32_t a = SuyuMix(0, x, g1, 15), b = SuyuMix(0, a, g2, 15), c = SuyuMix(0, b, g3, 15);
        const double ours = x * g1 * g2 * g3;
        chain = std::max(chain, std::fabs(ours - c));
    }
    std::printf("3 chained stages (Q15): worst |float - suyu| = %.3f LSB\n", chain);
    if (chain > 5.1) ++fails;

    // What the renderer does NOT do: suyu ramps each gain linearly from the previous frame's
    // value to the new one over the frame (ramp = (volume - prev) / sample_count). The renderer
    // applies the new gain from the first sample. Size of the step at a frame boundary for the
    // fade seen in the game (1.0 -> 0.85, 0.15 steps on a 0.1 amplitude sound):
    const double step = (1.0 - 0.85) * 0.1;
    std::printf("gain step without ramp: %.4f (%.1f dBFS) for a 0.15 gain change on a 0.1 sample\n", step,
                20 * std::log10(step));
    std::printf("mix_suyu_ref_test: %s\n", fails ? "FAIL" : "ok");
    return fails ? 1 : 0;
}
