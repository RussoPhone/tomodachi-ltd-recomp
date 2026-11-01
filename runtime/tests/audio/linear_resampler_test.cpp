// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include "audio/linear_resampler.h"

using namespace nxrt::audio;

namespace {

int failures = 0;

void CheckNear(float got, float expected, int line) {
    if (std::fabs(got - expected) > 0.00001f) {
        std::printf("FAIL line %d: got %.6f, expected %.6f\n", line, got, expected);
        ++failures;
    }
}

#define CHECK_NEAR(got, expected) CheckNear((got), (expected), __LINE__)

struct Source {
    std::vector<std::vector<float>> buffers;
    size_t buffer = 0;
    size_t sample = 0;
    size_t consumed = 0;

    float Read(bool& ended) {
        while (buffer < buffers.size() && sample == buffers[buffer].size()) {
            ++buffer;
            ++consumed;
            sample = 0;
        }
        if (buffer == buffers.size()) {
            ended = true;
            return 0.0f;
        }
        ended = false;
        return buffers[buffer][sample++];
    }
};

void TestSameRateRamp() {
    LinearResamplerState state;
    Source source{{{0.0f, 1000.0f / 32768.0f, 2000.0f / 32768.0f}}};
    size_t played = 0;
    auto read = [&](bool& ended) {
        const float sample = source.Read(ended);
        if (!ended) ++played;
        return sample;
    };
    CHECK_NEAR(ResampleLinear(state, 1.0, read), 0.0f);
    CHECK_NEAR(ResampleLinear(state, 1.0, read), 1000.0f / 32768.0f);
    CHECK_NEAR(ResampleLinear(state, 1.0, read), 2000.0f / 32768.0f);
    if (played != 3) {
        std::printf("FAIL line %d: played %zu samples, expected 3\n", __LINE__, played);
        ++failures;
    }
}

void TestUpsamplingRamp() {
    LinearResamplerState state;
    Source source{{{0.0f, 1000.0f / 32768.0f, 2000.0f / 32768.0f}}};
    size_t played = 0;
    auto read = [&](bool& ended) {
        const float sample = source.Read(ended);
        if (!ended) ++played;
        return sample;
    };
    constexpr std::array<float, 5> expected{0.0f, 500.0f / 32768.0f, 1000.0f / 32768.0f,
                                             1500.0f / 32768.0f, 2000.0f / 32768.0f};
    for (float value : expected) {
        CHECK_NEAR(ResampleLinear(state, 0.5, read), value);
    }
    if (played != 3) {
        std::printf("FAIL line %d: played %zu samples, expected 3\n", __LINE__, played);
        ++failures;
    }
}

void TestDownsamplingRamp() {
    LinearResamplerState state;
    Source source{{{0.0f, 1000.0f / 32768.0f, 2000.0f / 32768.0f, 3000.0f / 32768.0f,
                    4000.0f / 32768.0f}}};
    constexpr std::array<float, 3> expected{0.0f, 2000.0f / 32768.0f, 4000.0f / 32768.0f};
    size_t played = 0;
    auto read = [&](bool& ended) {
        const float sample = source.Read(ended);
        if (!ended) ++played;
        return sample;
    };
    for (float value : expected) CHECK_NEAR(ResampleLinear(state, 2.0, read), value);
    if (played != 5) {
        std::printf("FAIL line %d: played %zu samples, expected 5\n", __LINE__, played);
        ++failures;
    }
}

void TestBufferBoundaryAndEnd() {
    LinearResamplerState state;
    Source source{{{1000.0f / 32768.0f}, {2000.0f / 32768.0f}}};
    size_t played = 0;
    auto read = [&](bool& ended) {
        const float sample = source.Read(ended);
        if (!ended) ++played;
        return sample;
    };
    CHECK_NEAR(ResampleLinear(state, 1.0, read), 1000.0f / 32768.0f);
    CHECK_NEAR(ResampleLinear(state, 1.0, read), 2000.0f / 32768.0f);
    CHECK_NEAR(ResampleLinear(state, 1.0, read), 0.0f);
    if (played != 2) {
        std::printf("FAIL line %d: played %zu samples, expected 2\n", __LINE__, played);
        ++failures;
    }
    if (source.consumed != 2) {
        std::printf("FAIL line %d: consumed %zu buffers, expected 2\n", __LINE__, source.consumed);
        ++failures;
    }
}

// A streaming voice (TTS speech) is started before its first wave buffer arrives: the starved
// resampler must play silence and then pick up buffers appended later, not stay ended forever.
void TestResumesAfterStarvation() {
    LinearResamplerState state;
    Source source{{}};
    auto read = [&](bool& ended) { return source.Read(ended); };
    for (int i = 0; i < 4; ++i) CHECK_NEAR(ResampleLinear(state, 1.0, read), 0.0f);
    source.buffers.push_back({1000.0f / 32768.0f, 2000.0f / 32768.0f});
    float peak = 0.0f;
    for (int i = 0; i < 4; ++i) peak = std::max(peak, std::fabs(ResampleLinear(state, 1.0, read)));
    CHECK_NEAR(peak, 2000.0f / 32768.0f);
    if (source.consumed != 1) {
        std::printf("FAIL line %d: consumed %zu buffers, expected 1\n", __LINE__, source.consumed);
        ++failures;
    }
}

} // namespace

int main() {
    TestSameRateRamp();
    TestUpsamplingRamp();
    TestDownsamplingRamp();
    TestBufferBoundaryAndEnd();
    TestResumesAfterStarvation();
    std::printf(failures ? "FAILED: %d\n" : "ALL OK\n", failures);
    return failures != 0;
}
