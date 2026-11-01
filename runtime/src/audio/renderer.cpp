// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <functional>

#include "audio/adpcm.h"
#include "memory.h"

namespace nxrt::audio {

namespace {

template <typename T>
T Get(const u8* p, size_t offset) {
    T v;
    std::memcpy(&v, p + offset, sizeof(T));
    return v;
}

enum Format : u8 { kPcm8 = 1, kPcm16 = 2, kPcmFloat = 5, kAdpcm = 6 };

} // namespace

Renderer::Renderer(const RendererParams& p) : params_(p) {
    revision_ = (p.revision & 0xFFFFFF) == 0x564552 ? (p.revision >> 24) - '0' : 0;
    voices_.resize(p.voices);
    resources_.resize(p.voices);
    mixes_.resize(p.mixes + 1);
    splitters_.resize(p.splitter_infos);
    destinations_.resize(std::max(0, p.splitter_destinations));
}

// ------------------------------------------------------------------ update

void Renderer::UpdateVoice(Voice& v, const u8* in) {
    const bool is_new = in[0x08];
    const u32 play_state = in[0x0A];
    v.format = in[0x0B];
    v.sample_rate = Get<u32>(in, 0x0C);
    v.channels = std::clamp<u32>(Get<u32>(in, 0x18), 1, kMaxChannels);
    v.pitch = Get<float>(in, 0x1C);
    {
        static const bool vtrace = std::getenv("NXRT_AUDIO_TRACE") != nullptr;
        const float nv = Get<float>(in, 0x20);
        if (vtrace && (in[0x09] || v.in_use) && (nv != v.volume || in[0x09] != v.in_use)) {
            LOG_INFO("audio trace: frame %llu voice@%p volume %.4f -> %.4f in_use %u->%u pitch %.3f state %u", (unsigned long long)frames_,
                     (void*)&v, v.volume, nv, v.in_use, in[0x09], Get<float>(in, 0x1C), in[0x0A]);
        }
    }
    const bool was_in_use = v.in_use;
    v.in_use = in[0x09];
    v.volume = Get<float>(in, 0x20);
    // A new voice starts at its own volume (no fade-in); an old one ramps from the last frame's.
    if (is_new || (!was_in_use && v.in_use)) v.prev_volume = v.volume;
    v.adpcm_params = Get<u64>(in, 0x60);
    v.mix_id = Get<s32>(in, 0x70);
    v.splitter_id = Get<s32>(in, 0x74);
    for (int c = 0; c < kMaxChannels; ++c) v.resources[c] = Get<s32>(in, 0x158 + 4 * c);

    if (is_new) {
        v.valid.fill(false);
        v.index = 0;
        v.offset = 0;
        v.loops_done = 0;
        v.played = 0;
        v.consumed = 0;
        v.state = {};
    }
    // Buffers the game has not marked as received are new: take them.
    for (int i = 0; i < kWaveBuffers; ++i) {
        const u8* w = in + 0x78 + i * 0x38;
        static const bool trace = std::getenv("NXRT_AUDIO_TRACE") != nullptr;
        if (trace && Get<u64>(w, 0) != 0 && (w[0x1A] != v.last_sent[i] || Get<u64>(w, 0) != v.buffers[i].address)) {
            LOG_INFO("audio trace: frame %llu voice@%p wb %d addr %llx size %llu end %d sent %u valid %d is_new %d state %u",
                     (unsigned long long)frames_, (void*)&v, i, (unsigned long long)Get<u64>(w, 0),
                     (unsigned long long)Get<u64>(w, 8), Get<s32>(w, 0x14), w[0x1A], v.valid[i], is_new, play_state);
        }
        v.last_sent[i] = w[0x1A];
        if (w[0x1A]) continue; // sent_to_DSP
        WaveBuffer& b = v.buffers[i];
        b.address = Get<u64>(w, 0x00);
        b.size = Get<u64>(w, 0x08);
        b.start = Get<s32>(w, 0x10);
        b.end = Get<s32>(w, 0x14);
        b.loop = w[0x18];
        b.stream_ended = w[0x19];
        b.loop_count = Get<s32>(w, 0x1C);
        b.context_address = Get<u64>(w, 0x20);
        b.loop_start = Get<u32>(w, 0x30);
        b.loop_end = Get<u32>(w, 0x34);
        if (trace && b.address && g_memory.IsMapped(b.address, 8)) {
            const u64 n = std::min<u64>(b.size, 8192);
            u32 nz = 0, maxscale = 0;
            u8 hdr = 0;
            for (u64 k = 0; k + 8 <= n; k += 8) {
                const u8 h0 = g_memory.Read<u8>(b.address + k);
                if (k == 0) hdr = h0;
                maxscale = std::max<u32>(maxscale, h0 & 0xF);
                for (int q = 1; q < 8; ++q) nz += g_memory.Read<u8>(b.address + k + q) != 0;
            }
            int pcm_peak = 0;
            for (u64 k = 0; k + 2 <= n; k += 2) pcm_peak = std::max(pcm_peak, std::abs(static_cast<int>(g_memory.Read<s16>(b.address + k))));
            LOG_INFO("audio trace: take wb %d hdr0 %02x maxscale %u nonzero_bytes %u of %llu ctx %llx fmt %u rate %u ch %u pcm16peak %d", i,
                     hdr, maxscale, nz, (unsigned long long)n, (unsigned long long)b.context_address, v.format, v.sample_rate, v.channels,
                     pcm_peak);
        }
        const bool was_valid = v.valid[i];
        v.valid[i] = b.address != 0 && b.end > b.start;
        if (!was_valid && v.valid[i] && i == static_cast<int>(v.index)) v.offset = b.start;
    }
    // Stopping releases every queued buffer.
    if (play_state == 1 && v.play_state != 1) {
        for (int i = 0; i < kWaveBuffers; ++i) {
            if (v.valid[i]) {
                v.valid[i] = false;
                ++v.consumed;
            }
        }
        v.played = 0;
        v.offset = 0;
        v.state = {};
    }
    v.play_state = play_state;
}

const Renderer::Destination* Renderer::SplitterDestination(s32 splitter, u32 index) const {
    if (splitter < 0 || static_cast<size_t>(splitter) >= splitters_.size()) return nullptr;
    const auto& ids = splitters_[splitter];
    if (index >= ids.size()) return nullptr;
    const s32 id = ids[index];
    if (id < 0 || static_cast<size_t>(id) >= destinations_.size()) return nullptr;
    return &destinations_[id];
}

void Renderer::Layout() {
    // Distance from the final mix (id 0): sources are processed before their destinations.
    std::vector<int> distance(mixes_.size(), -1);
    std::function<int(s32, int)> dist = [&](s32 id, int depth) -> int {
        if (id == 0) return 0;
        if (id < 0 || static_cast<size_t>(id) >= mixes_.size() || depth > 64) return 0;
        if (distance[id] >= 0) return distance[id];
        const Mix& m = mixes_[id];
        int d = 1;
        if (m.dest_mix != kUnusedMix) {
            d = 1 + dist(m.dest_mix, depth + 1);
        } else if (const Destination* dest = SplitterDestination(m.dest_splitter, 0)) {
            d = 1 + dist(dest->mix_id, depth + 1);
        }
        return distance[id] = d;
    };
    order_.clear();
    for (size_t i = 0; i < mixes_.size(); ++i) {
        if (mixes_[i].in_use) order_.push_back(static_cast<s32>(i));
    }
    std::stable_sort(order_.begin(), order_.end(),
                     [&](s32 a, s32 b) { return dist(a, 0) > dist(b, 0); });
    u32 offset = 0;
    for (s32 id : order_) {
        mixes_[id].offset = offset;
        offset += mixes_[id].buffer_count;
    }
    pool_.assign(static_cast<size_t>(offset) * params_.sample_count, 0.0f);
}

void Renderer::Update(const std::vector<u8>& in, std::vector<u8>& out) {
    std::lock_guard lock(mutex_);
    const u32 mem_pools = params_.effects + params_.voices * 4;
    const u32 effect_out = revision_ >= 9 ? 0x90 : 0x10;
    const u32 sizes[7] = {mem_pools * 0x10, params_.voices * 0x10, params_.effects * effect_out, params_.sinks * 0x20,
                          0x10, 0xB0, revision_ >= 5 ? 0x10u : 0u};
    u32 total = 0x40;
    for (u32 s : sizes) total += s;
    out.assign(std::max<size_t>(total, out.size()), 0);
    u32 oh[16] = {};
    oh[0] = params_.revision;
    oh[1] = sizes[5];
    oh[2] = sizes[0];
    oh[3] = sizes[1];
    oh[5] = sizes[2];
    oh[7] = sizes[3];
    oh[8] = sizes[4];
    oh[10] = sizes[6];
    oh[15] = total;
    std::memcpy(out.data(), oh, sizeof(oh));
    if (in.size() < 0x40) return;
    u32 h[16];
    std::memcpy(h, in.data(), sizeof(h));
    const u8* p = in.data();
    const size_t end = in.size();
    // The header must describe the input: otherwise the sections below would read past it.
    const size_t declared = 0x40 + size_t{h[1]} + h[2] + h[3] + h[4] + h[5] + h[6] + h[7] + h[8] + h[9] + h[10];
    if (declared > end) {
        if (std::getenv("NXRT_AUDIO_PROBE")) LOG_WARN("audio: input declares %zu bytes but has %zu", declared, end);
        return;
    }
    size_t off = 0x40 + h[1]; // behaviour

    // Memory pools: attach and detach requests succeed.
    // Pools always report "attached" (as before): the game then stops retrying.
    u8* o = out.data() + 0x40;
    for (u32 i = 0; i < mem_pools; ++i) o[i * 0x10] = 5;
    off += h[2];

    // Voice channel resources: per-channel volumes into each mix buffer.
    for (size_t r = off; r + 0x70 <= off + h[4] && r + 0x70 <= end; r += 0x70) {
        const u32 id = Get<u32>(p, r);
        if (id < resources_.size() && p[r + 0x64]) std::memcpy(resources_[id].data(), p + r + 4, 4 * kMaxMixBuffers);
    }
    off += h[4];
    if (std::getenv("NXRT_AUDIO_PROBE") && frames_ % 1000 == 0) {
        LOG_INFO("audio render: voices section at 0x%zx, input %zu bytes, h[3]=%u voices_in=%zu", off, end, h[3],
                 voices_.size());
        int shown = 0;
        for (u32 i = 0; i < voices_.size() && shown < 3; ++i) {
            const size_t at = off + i * 0x188;
            if (at + 0x188 > end || !p[at + 9]) continue;
            ++shown;
            LOG_INFO("audio render:   active voice %u: state %u fmt %u rate %u ch %u mix %d split %d wb %u", i, p[at + 0xA],
                     p[at + 0xB], Get<u32>(p, at + 0xC), Get<u32>(p, at + 0x18), Get<s32>(p, at + 0x70),
                     Get<s32>(p, at + 0x74), Get<u32>(p, at + 0x54));
            for (int b = 0; b < 4; ++b) {
                const size_t w = at + 0x78 + b * 0x38;
                LOG_INFO("audio render:     wb %d addr %llx size %llu start %d end %d sent %u", b,
                         (unsigned long long)Get<u64>(p, w), (unsigned long long)Get<u64>(p, w + 8), Get<s32>(p, w + 0x10),
                         Get<s32>(p, w + 0x14), p[w + 0x1A]);
            }
        }
        if (!shown) LOG_INFO("audio render:   no active voices in this update");
    }

    // Voices (0x188-byte layout, revision 15+; 0x170 before).
    const u32 stride = revision_ >= 15 ? 0x188 : 0x170;
    u8* vo = out.data() + 0x40 + sizes[0];
    if (stride == 0x188) {
        for (u32 i = 0; i < voices_.size() && off + (i + 1) * stride <= end; ++i) {
            Voice& v = voices_[i];
            UpdateVoice(v, p + off + i * stride);
            const bool is_new = p[off + i * stride + 8];
            // The game paces its wave buffers by these counters: reported as zero (the old default) it never
            // sees a buffer consumed, stops refilling, and music/effects go silent after a few seconds.
            // NXRT_AUDIO_NO_STATUS=1 restores the zeros.
            static const bool real_status = std::getenv("NXRT_AUDIO_NO_STATUS") == nullptr;
            const bool fresh = is_new || !real_status;
            const u64 played = fresh ? 0 : v.played;
            const u32 consumed = fresh ? 0 : v.consumed;
            {
                static const bool vtrace2 = std::getenv("NXRT_AUDIO_TRACE") != nullptr;
                static u32 shown_updates = 0;
                if (vtrace2 && v.in_use && v.sample_rate == 32000 && v.format == kPcm16) {
                    ++shown_updates;
                    if (shown_updates <= 300 || shown_updates % 200 == 0)
                        LOG_INFO("audio trace: pcm32k voice %u upd %u is_new %d fresh %d play_state %u consumed %u played %llu idx %u off %d valid %d%d%d%d "
                                 "ended %d vol %.2f mix %d split %d", i, shown_updates, is_new, fresh, v.play_state, consumed,
                                 (unsigned long long)played, v.index, v.offset, v.valid[0], v.valid[1], v.valid[2], v.valid[3],
                                 v.state[0].resampler.ended, v.volume, v.mix_id, v.splitter_id);
                }
            }
            std::memcpy(vo + i * 0x10, &played, 8);
            std::memcpy(vo + i * 0x10 + 8, &consumed, 4);
        }
    }
    off += h[3];
    off += h[5]; // effects: not processed yet

    // Splitters: header 'SNDH', then infos 'SNDI' with destination ids, then destinations 'SNDD'.
    if (h[9] >= 0x20 && off + h[9] <= end && std::memcmp(p + off, "SNDH", 4) == 0) {
        const s32 info_count = Get<s32>(p, off + 4);
        const s32 dest_count = Get<s32>(p, off + 8);
        size_t s = off + 0x20;
        const size_t limit = off + h[9];
        for (s32 i = 0; i < info_count && s + 0x10 <= limit; ++i) {
            if (std::memcmp(p + s, "SNDI", 4) != 0) break;
            const s32 id = Get<s32>(p, s + 4);
            const u32 count = Get<u32>(p, s + 0xC);
            if (id >= 0 && static_cast<size_t>(id) < splitters_.size() && s + 0x10 + count * 4 <= limit) {
                auto& ids = splitters_[id];
                ids.resize(count);
                for (u32 k = 0; k < count; ++k) ids[k] = Get<s32>(p, s + 0x10 + 4 * k);
            }
            s += 0x10 + 3 * 4 + count * 4;
        }
        const size_t dest_size = revision_ >= 15 ? 0xA8 : revision_ >= 12 ? 0x90 : 0x70;
        const size_t in_use_at = revision_ >= 15 ? 0x9C : revision_ >= 12 ? 0x84 : 0x6C;
        for (s32 i = 0; i < dest_count && s + dest_size <= limit; ++i, s += dest_size) {
            if (std::memcmp(p + s, "SNDD", 4) != 0) break;
            const s32 id = Get<s32>(p, s + 4);
            if (id < 0 || static_cast<size_t>(id) >= destinations_.size()) continue;
            Destination& d = destinations_[id];
            std::memcpy(d.volumes.data(), p + s + 8, 4 * kMaxMixBuffers);
            d.mix_id = Get<s32>(p, s + 0x68);
            d.in_use = p[s + in_use_at];
        }
    }
    off += h[9];

    // Mixes: all of them, or only the changed ones behind a 0x20-byte header.
    {
        size_t m = off;
        u32 count = h[6] / 0x930;
        bool dirty_only = false;
        if (h[6] % 0x930 == 0x20) {
            count = Get<u32>(p, off + 4);
            m += 0x20;
            dirty_only = true;
        }
        bool changed = false;
        for (u32 i = 0; i < count && m + 0x930 <= end; ++i, m += 0x930) {
            const s32 id = dirty_only ? Get<s32>(p, m + 0x10) : static_cast<s32>(i);
            if (id < 0 || static_cast<size_t>(id) >= mixes_.size()) continue;
            Mix& x = mixes_[id];
            x.volume = Get<float>(p, m);
            x.buffer_count = std::min<u32>(Get<u32>(p, m + 8), kMaxMixBuffers);
            x.in_use = p[m + 0xC];
            std::memcpy(x.volumes.data(), p + m + 0x24, sizeof(x.volumes));
            x.dest_mix = Get<s32>(p, m + 0x924);
            x.dest_splitter = Get<s32>(p, m + 0x928);
            changed = true;
        }
        if (changed || pool_.empty()) Layout();
    }
    off += h[6];

    // Sinks: the device sink's inputs are final-mix buffers.
    for (size_t k = off; k + 0x140 <= off + h[7] && k + 0x140 <= end; k += 0x140) {
        if (p[k] != 1 || !p[k + 1]) continue;
        const u32 n = std::min<u32>(Get<u32>(p, k + 0x20 + 0x100), kMaxChannels);
        sink_inputs_.assign(p + k + 0x20 + 0x104, p + k + 0x20 + 0x104 + n);
    }

    if (sizes[6]) {
        const u64 frames = frames_;
        std::memcpy(out.data() + total - 0x10, &frames, 8);
    }
}

// ------------------------------------------------------------------ rendering

s16 Renderer::NextSourceSample(Voice& v, u32 channel, bool& ended) {
    (void)channel;
    ended = false;
    for (int guard = 0; guard < kWaveBuffers + 1; ++guard) {
        if (!v.valid[v.index]) {
            ended = true;
            return 0;
        }
        WaveBuffer& b = v.buffers[v.index];
        if (v.offset < b.end) break;
        // This buffer is done: loop it, or release it and move on.
        if (b.loop && (b.loop_count < 0 || v.loops_done < b.loop_count)) {
            ++v.loops_done;
            v.offset = b.loop_end > b.loop_start ? static_cast<s32>(b.loop_start) : b.start;
            if (b.loop_end > b.loop_start) b.end = static_cast<s32>(b.loop_end);
            continue;
        }
        v.valid[v.index] = false;
        ++v.consumed;
        v.loops_done = 0;
        v.index = (v.index + 1) % kWaveBuffers;
        v.offset = v.valid[v.index] ? v.buffers[v.index].start : 0;
        if (b.stream_ended) {
            ended = true;
            return 0;
        }
    }
    if (!v.valid[v.index]) {
        ended = true;
        return 0;
    }
    WaveBuffer& b = v.buffers[v.index];
    const s32 i = v.offset;
    s16 sample = 0;
    switch (v.format) {
    case kPcm16: {
        const u64 a = b.address + (static_cast<u64>(i) * v.channels + channel) * 2;
        if (g_memory.IsMapped(a, 2)) sample = g_memory.Read<s16>(a);
        break;
    }
    case kPcm8: {
        const u64 a = b.address + static_cast<u64>(i) * v.channels + channel;
        if (g_memory.IsMapped(a, 1)) sample = static_cast<s16>(static_cast<s8>(g_memory.Read<u8>(a)) << 8);
        break;
    }
    case kPcmFloat: {
        const u64 a = b.address + (static_cast<u64>(i) * v.channels + channel) * 4;
        if (g_memory.IsMapped(a, 4)) {
            sample = static_cast<s16>(std::clamp(g_memory.Read<float>(a), -1.0f, 1.0f) * 32767.0f);
        }
        break;
    }
    case kAdpcm: {
        const u64 frame = b.address + static_cast<u64>(i / kAdpcmSamplesPerFrame) * kAdpcmBytesPerFrame;
        if (!g_memory.IsMapped(frame, kAdpcmBytesPerFrame) || !g_memory.IsMapped(v.adpcm_params, sizeof(s16) * 16)) {
            break;
        }
        ChannelState& st = v.state[channel];
        // The game may attach the decoder state at the start of each pass over the buffer: its
        // first one, and the loop point of every repeat.
        const bool looped = v.loops_done > 0 && b.loop_end > b.loop_start;
        const s32 pass_start = looped ? static_cast<s32>(b.loop_start) : b.start;
        if (i == pass_start && b.context_address && g_memory.IsMapped(b.context_address, sizeof(AdpcmContext))) {
            st.hist1 = g_memory.Read<s16>(b.context_address + offsetof(AdpcmContext, yn0));
            st.hist2 = g_memory.Read<s16>(b.context_address + offsetof(AdpcmContext, yn1));
        }
        u8 raw[kAdpcmBytesPerFrame];
        s16 coefs[16];
        g_memory.ReadBlock(frame, raw, sizeof(raw));
        g_memory.ReadBlock(v.adpcm_params, coefs, sizeof(coefs));
        // AdpcmDecode indexes frames from the stream start: hand it this frame as frame 0.
        sample = AdpcmDecode(raw, static_cast<u32>(i) % kAdpcmSamplesPerFrame, coefs, st.hist1, st.hist2);
        break;
    }
    default:
        break;
    }
    return sample;
}

void Renderer::RenderVoiceChannel(Voice& v, u32 channel, float* out, u32 count, bool) {
    // Linear resampling from the voice's rate (times pitch) to 48 kHz.
    const double step = static_cast<double>(v.sample_rate) * std::max(0.0f, v.pitch) / std::max<u32>(params_.sample_rate, 1);
    ChannelState& st = v.state[channel];
    for (u32 n = 0; n < count; ++n) {
        out[n] = ResampleLinear(st.resampler, step, [&](bool& ended) {
            const s16 sample = NextSourceSample(v, channel, ended);
            if (!ended) ++v.offset, ++v.played;
            return sample / 32768.0f;
        });
    }
}

void Renderer::AddToMix(const float* src, u32 count, s32 mix_id, const float* volumes, float scale,
                        float scale_end) {
    if (mix_id < 0 || static_cast<size_t>(mix_id) >= mixes_.size()) return;
    const Mix& m = mixes_[mix_id];
    if (!m.in_use) return;
    for (u32 j = 0; j < m.buffer_count; ++j) {
        const float g = volumes[j] * scale;
        const float g_end = volumes[j] * scale_end;
        if (g == 0.0f && g_end == 0.0f) continue;
        float* dst = pool_.data() + static_cast<size_t>(m.offset + j) * count;
        if (g == g_end) {
            for (u32 n = 0; n < count; ++n) dst[n] += src[n] * g;
        } else {
            const float step = (g_end - g) / static_cast<float>(count);
            for (u32 n = 0; n < count; ++n) dst[n] += src[n] * (g + step * static_cast<float>(n));
        }
    }
}

void Renderer::Render(std::vector<float>& stereo) {
    std::lock_guard lock(mutex_);
    if (std::getenv("NXRT_AUDIO_PROBE") && frames_ % 1000 == 0) {
        for (size_t i = 0; i < voices_.size(); ++i) {
            const Voice& v = voices_[i];
            if (!v.in_use) continue;
            LOG_INFO("audio render: voice %zu state %u fmt %u rate %u ch %u mix %d split %d valid %d%d%d%d idx %u off %d", i,
                     v.play_state, v.format, v.sample_rate, v.channels, v.mix_id, v.splitter_id, v.valid[0], v.valid[1],
                     v.valid[2], v.valid[3], v.index, v.offset);
            for (int b = 0; b < kWaveBuffers; ++b) {
                LOG_INFO("audio render:   buffer %d addr %llx size %llu start %d end %d", b,
                         (unsigned long long)v.buffers[b].address, (unsigned long long)v.buffers[b].size,
                         v.buffers[b].start, v.buffers[b].end);
            }
        }
    }
    const u32 count = params_.sample_count;
    stereo.assign(static_cast<size_t>(count) * 2, 0.0f);
    ++frames_;
    if (pool_.empty()) return;
    std::fill(pool_.begin(), pool_.end(), 0.0f);
    std::vector<float> tmp(count);

    for (Voice& v : voices_) {
        // suyu: a voice that stopped playing restarts its ramp from silence.
        if (v.in_use && v.play_state != 0) v.prev_volume = 0.0f;
        if (!v.in_use || v.play_state != 0) continue;
        if (v.mix_id == kUnusedMix && v.splitter_id == kUnusedSplitter) continue;
        if (v.format == kAdpcm && v.channels != 1) v.channels = 1;
        // Channels share one wave buffer position: each renders from the same
        // starting point and the position after the last one is kept.
        const u32 channels = v.channels;
        const s32 offset = v.offset;
        const u32 index = v.index;
        const auto valid = v.valid;
        const u64 played = v.played;
        const u32 consumed = v.consumed;
        const s32 loops = v.loops_done;
        for (u32 c = 0; c < channels; ++c) {
            if (c > 0) { // every channel reads the same source frames: start each from the same position
                v.offset = offset;
                v.index = index;
                v.valid = valid;
                v.played = played;
                v.consumed = consumed;
                v.loops_done = loops;
            }
            RenderVoiceChannel(v, c, tmp.data(), count, true);
            static const bool trace = std::getenv("NXRT_AUDIO_TRACE") != nullptr;
            if (trace) {
                float pk = 0;
                for (u32 n = 0; n < count; ++n) pk = std::max(pk, std::fabs(tmp[n]));
                float dv = 0, rv = 0;
                u32 dn = 0;
                for (u32 i = 0; const Destination* d = SplitterDestination(v.splitter_id, i); ++i) {
                    ++dn;
                    if (d->in_use) for (float x : d->volumes) dv = std::max(dv, std::fabs(x));
                }
                const s32 r = v.resources[c];
                if (r >= 0 && static_cast<size_t>(r) < resources_.size())
                    for (float x : resources_[r]) rv = std::max(rv, std::fabs(x));
                static int shown = 0;
                if (v.volume > 0 && shown < 400) {
                    ++shown;
                    LOG_INFO("audio trace: loud voice %zu frame %llu in-peak %.4f vol %.3f destvol %.3f off %d idx %u fmt %u",
                             static_cast<size_t>(&v - voices_.data()), (unsigned long long)frames_, pk, v.volume, dv, v.offset, v.index, v.format);
                }
                static u64 acc[96] = {};
                const size_t vi = static_cast<size_t>(&v - voices_.data());
                if (vi < 96) {
                    acc[vi] = std::max<u64>(acc[vi], static_cast<u64>(pk * 1e6f));
                    if (frames_ % 200 == 0) {
                        LOG_INFO("audio trace: voice %zu peak %.4f (200f max) vol %.3f pitch %.3f dests %u destvol %.3f resvol %.3f "
                                 "mix %d off %d idx %u", vi, acc[vi] / 1e6, v.volume, v.pitch, dn, dv, rv, v.mix_id, v.offset, v.index);
                        acc[vi] = 0;
                    }
                }
            }
            if (v.mix_id != kUnusedMix) {
                const s32 r = v.resources[c];
                if (r >= 0 && static_cast<size_t>(r) < resources_.size()) {
                    AddToMix(tmp.data(), count, v.mix_id, resources_[r].data(), v.prev_volume, v.volume);
                }
            } else {
                for (u32 i = c; const Destination* d = SplitterDestination(v.splitter_id, i); i += channels) {
                    if (d->in_use) AddToMix(tmp.data(), count, d->mix_id, d->volumes.data(), v.prev_volume, v.volume);
                }
            }
        }
        v.prev_volume = v.volume;
    }

    // Sub mixes into their destinations, sources first.
    for (s32 id : order_) {
        if (id == 0) continue;
        const Mix& m = mixes_[id];
        const float* src = pool_.data() + static_cast<size_t>(m.offset) * count;
        if (m.dest_mix != kUnusedMix) {
            if (m.dest_mix < 0 || static_cast<size_t>(m.dest_mix) >= mixes_.size()) continue;
            const Mix& d = mixes_[m.dest_mix];
            for (u32 i = 0; i < m.buffer_count; ++i) {
                for (u32 j = 0; j < d.buffer_count; ++j) {
                    const float g = m.volume * m.volumes[i][j];
                    if (g == 0.0f) continue;
                    float* dst = pool_.data() + static_cast<size_t>(d.offset + j) * count;
                    const float* s = src + static_cast<size_t>(i) * count;
                    for (u32 n = 0; n < count; ++n) dst[n] += s[n] * g;
                }
            }
        } else if (m.dest_splitter != kUnusedSplitter && m.buffer_count) {
            for (u32 k = 0; const Destination* dest = SplitterDestination(m.dest_splitter, k); ++k) {
                if (!dest->in_use) continue;
                const float* s = src + static_cast<size_t>(k % m.buffer_count) * count;
                AddToMix(s, count, dest->mix_id, dest->volumes.data(), m.volume, m.volume);
            }
        }
    }

    // Final mix -> device sink channels -> stereo.
    const Mix& f = mixes_[0];
    if (!f.in_use || !f.buffer_count) return;
    const size_t inputs = sink_inputs_.empty() ? std::min<u32>(f.buffer_count, 2) : sink_inputs_.size();
    for (u32 n = 0; n < count; ++n) {
        float c[kMaxChannels] = {};
        for (size_t k = 0; k < inputs && k < kMaxChannels; ++k) {
            const s32 b = sink_inputs_.empty() ? static_cast<s32>(k) : sink_inputs_[k];
            if (b >= 0 && static_cast<u32>(b) < f.buffer_count) {
                c[k] = pool_[static_cast<size_t>(f.offset + b) * count + n] * f.volume;
            }
        }
        float l = c[0], r = c[1];
        if (inputs >= 6) { // 5.1: FL FR FC LFE BL BR
            l += 0.707f * c[2] + 0.5f * c[4];
            r += 0.707f * c[2] + 0.5f * c[5];
        }
        stereo[2 * n] = std::clamp(l, -1.0f, 1.0f);
        stereo[2 * n + 1] = std::clamp(r, -1.0f, 1.0f);
    }
}

} // namespace nxrt::audio
