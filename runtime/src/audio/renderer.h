// SPDX-License-Identifier: GPL-3.0-or-later
// The Switch audio renderer (audren), reimplemented: the game describes voices,
// mixes, splitters and sinks in RequestUpdate; every 5 ms the renderer decodes
// the voices' wave buffers, routes them through the mixes and hands the final
// mix to the host output. Effects are not applied yet (they pass through).
#pragma once

#include <array>
#include <mutex>
#include <vector>

#include "../common.h"
#include "audio/linear_resampler.h"

namespace nxrt::audio {

struct RendererParams {
    u32 sample_rate, sample_count, mixes, sub_mixes, voices, sinks, effects, perf_frames;
    u8 voice_drop_enabled, unk21, rendering_device, execution_mode;
    u32 splitter_infos;
    s32 splitter_destinations;
    u32 external_context_size;
    u32 revision; // "REV" + number
};
static_assert(sizeof(RendererParams) == 0x34);

class Renderer {
public:
    explicit Renderer(const RendererParams& params);

    // Parses a RequestUpdate input and writes its output. Thread-safe.
    void Update(const std::vector<u8>& in, std::vector<u8>& out);
    // Renders one 5 ms frame (params.sample_count samples) into interleaved
    // stereo float. Thread-safe.
    void Render(std::vector<float>& stereo);

    u32 Revision() const { return revision_; }

private:
    static constexpr int kMaxMixBuffers = 24;
    static constexpr int kMaxChannels = 6;
    static constexpr int kWaveBuffers = 4;
    static constexpr s32 kUnusedMix = 0x7FFFFFFF;
    static constexpr s32 kUnusedSplitter = -1;

    struct WaveBuffer {
        u64 address = 0, size = 0;
        s32 start = 0, end = 0;
        bool loop = false, stream_ended = false;
        s32 loop_count = 0;
        u64 context_address = 0;
        u32 loop_start = 0, loop_end = 0;
    };

    struct ChannelState {
        s16 hist1 = 0, hist2 = 0; // ADPCM history
        LinearResamplerState resampler;
    };

    struct Voice {
        bool in_use = false;
        u32 play_state = 1; // 0 started, 1 stopped, 2 paused (game's view)
        bool stopping = false;
        u8 format = 0;
        u32 sample_rate = 48000;
        u32 channels = 1;
        float pitch = 1, volume = 1;
        float prev_volume = 1; // volume of the previous frame: the mix ramps from it to `volume`
        u64 adpcm_params = 0;
        s32 mix_id = kUnusedMix, splitter_id = kUnusedSplitter;
        std::array<s32, kMaxChannels> resources{};
        std::array<WaveBuffer, kWaveBuffers> buffers{};
        std::array<bool, kWaveBuffers> valid{};
        std::array<u8, kWaveBuffers> last_sent{};
        u32 index = 0;     // wave buffer being played
        s32 offset = 0;    // sample within it
        s32 loops_done = 0;
        u64 played = 0;
        u32 consumed = 0;
        std::array<ChannelState, kMaxChannels> state{};
    };

    struct Mix {
        bool in_use = false;
        float volume = 1;
        u32 buffer_count = 0;
        s32 dest_mix = kUnusedMix, dest_splitter = kUnusedSplitter;
        std::array<std::array<float, kMaxMixBuffers>, kMaxMixBuffers> volumes{};
        u32 offset = 0; // first buffer in the mix buffer pool
    };

    struct Destination {
        s32 mix_id = kUnusedMix;
        bool in_use = false;
        std::array<float, kMaxMixBuffers> volumes{};
    };

    void UpdateVoice(Voice& v, const u8* in);
    void Layout();
    // Decodes `count` output-rate samples of one channel of `v` into `out`.
    void RenderVoiceChannel(Voice& v, u32 channel, float* out, u32 count, bool advance);
    s16 NextSourceSample(Voice& v, u32 channel, bool& ended);
    // `scale` ramps linearly to `scale_end` over the `count` samples (suyu's MixRamp: no step at frame edges).
    void AddToMix(const float* src, u32 count, s32 mix_id, const float* volumes, float scale, float scale_end);
    const Destination* SplitterDestination(s32 splitter, u32 index) const;

    std::mutex mutex_;
    RendererParams params_;
    u32 revision_ = 0;
    std::vector<Voice> voices_;
    std::vector<std::array<float, kMaxMixBuffers>> resources_; // voice channel resources
    std::vector<Mix> mixes_;
    std::vector<std::vector<s32>> splitters_; // destination ids per splitter
    std::vector<Destination> destinations_;
    std::vector<s8> sink_inputs_;
    std::vector<s32> order_; // mixes, sources before destinations
    std::vector<float> pool_; // mix buffers, sample_count each
    u64 frames_ = 0;
};

} // namespace nxrt::audio
