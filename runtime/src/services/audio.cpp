// SPDX-License-Identifier: GPL-3.0-or-later
// audren:u for now produces no sound: RequestUpdate answers with a correctly
// sized, all-zero status block and the system event ticks every 5 ms like the
// real renderer. Real output (the audio renderer's mixing commands) comes next.
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <string>
#include <thread>

#include <cstdlib>

#include "audio/output.h"
#include "audio/renderer.h"
#include "services/common.h"

namespace nxrt::hle {

namespace {

using audio::RendererParams;

class IAudioRenderer final : public ServiceObject {
public:
    explicit IAudioRenderer(const RendererParams& p)
        : ServiceObject("IAudioRenderer"), params_(p), renderer_(std::make_shared<audio::Renderer>(p)) {
        // "REV" followed by the revision as a digit byte ("REV0" + n), little-endian.
        rev_ = (p.revision & 0xFFFFFF) == 0x564552 ? (p.revision >> 24) - '0' : 0;
        LOG_INFO("audio renderer: %u Hz, %u samples, %u voices, %u sinks, %u effects, revision %u",
                 p.sample_rate, p.sample_count, p.voices, p.sinks, p.effects, rev_);
        Register(0, "GetSampleRate", [this](Ctx& ctx) { ctx.Push<u32>(params_.sample_rate); });
        Register(1, "GetSampleCount", [this](Ctx& ctx) { ctx.Push<u32>(params_.sample_count); });
        Register(2, "GetMixBufferCount", [this](Ctx& ctx) { ctx.Push<u32>(params_.mixes); });
        Register(3, "GetState", [this](Ctx& ctx) { ctx.Push<u32>(*running_ ? 0 : 1); });
        Register(4, "RequestUpdate", [this](Ctx& ctx) { Update(ctx); });
        Register(5, "Start", [this](Ctx&) { StartTicking(); });
        Register(6, "Stop", [this](Ctx&) { *running_ = false; });
        Register(7, "QuerySystemEvent", [this](Ctx& ctx) { ctx.PushCopyObject(event_); });
        RegisterStub(8, "SetRenderingTimeLimit");
        Register(9, "GetRenderingTimeLimit", [](Ctx& ctx) { ctx.Push<u32>(100); });
        Register(10, "RequestUpdateAuto", [this](Ctx& ctx) { Update(ctx); });
    }

private:
    // Logs, a few times, what the game sends: sections, voice formats, mixes, sinks, splitters.
    void Probe(const std::vector<u8>& in) {
        if (in.size() < 0x40) return;
        u32 h[16];
        std::memcpy(h, in.data(), sizeof(h));
        LOG_INFO("audio probe: rev %08x behaviour %u pools %u voices %u resources %u effects %u mixes %u sinks %u "
                 "perf %u splitter %u render %u total %u",
                 h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8], h[9], h[10], h[15]);
        size_t off = 0x40 + h[1] + h[2] + h[4]; // header, behaviour, pools, voice resources
        const size_t voices = off;
        std::map<std::string, int> formats;
        for (u32 i = 0; i < params_.voices && voices + (i + 1) * 0x188 <= in.size(); ++i) {
            const u8* v = in.data() + voices + i * 0x188;
            if (!v[9]) continue; // in_use
            u32 rate, channels, wbcount, mix_id, splitter;
            std::memcpy(&rate, v + 0xC, 4);
            std::memcpy(&channels, v + 0x18, 4);
            std::memcpy(&wbcount, v + 0x54, 4);
            std::memcpy(&mix_id, v + 0x70, 4);
            std::memcpy(&splitter, v + 0x74, 4);
            char key[96];
            std::snprintf(key, sizeof(key), "fmt%u/%uHz/%uch/state%u/mix%d/split%d", v[0xB], rate, channels, v[0xA],
                          static_cast<s32>(mix_id), static_cast<s32>(splitter));
            ++formats[key];
        }
        for (const auto& [k, n] : formats) LOG_INFO("audio probe:   %d voices %s", n, k.c_str());
        off = voices + h[3] + h[5]; // voices, effects
        if (off + 4 <= in.size()) {
            u32 magic;
            std::memcpy(&magic, in.data() + off, 4);
            LOG_INFO("audio probe:   splitter section %u bytes, magic %08x", h[9], magic);
        }
        off += h[9];
        size_t mixes = off;
        if (off + 4 <= in.size() && std::memcmp(in.data() + off, "MIXD", 4) == 0) mixes += 0x20; // dirty-only header
        for (size_t m = mixes; m + 0x930 <= off + h[6]; m += 0x930) {
            const u8* x = in.data() + m;
            if (!x[0xC]) continue;
            u32 buffers, rate;
            s32 id, dest, dest_split;
            std::memcpy(&rate, x + 4, 4);
            std::memcpy(&buffers, x + 8, 4);
            std::memcpy(&id, x + 0x10, 4);
            std::memcpy(&dest, x + 0x924, 4);
            std::memcpy(&dest_split, x + 0x928, 4);
            LOG_INFO("audio probe:   mix %d: %u buffers, %u Hz, dest mix %d, dest splitter %d", id, buffers, rate, dest,
                     dest_split);
        }
        off += h[6];
        for (u32 i = 0; i < params_.sinks && off + (i + 1) * 0x140 <= in.size(); ++i) {
            const u8* k = in.data() + off + i * 0x140;
            if (!k[1]) continue;
            std::string inputs;
            if (k[0] == 1) {
                u32 n;
                std::memcpy(&n, k + 0x20 + 0x100, 4);
                for (u32 c = 0; c < n && c < 6; ++c) inputs += " " + std::to_string(static_cast<s8>(k[0x20 + 0x104 + c]));
            }
            LOG_INFO("audio probe:   sink %u type %u inputs:%s", i, k[0], inputs.c_str());
        }
    }

    void Update(Ctx& ctx) {
        const std::vector<u8> input = ctx.ReadBuffer(0);
        static int probes = 0;
        if (std::getenv("NXRT_AUDIO_PROBE") && (probes == 0 || probes == 600 || probes == 3000)) Probe(input);
        ++probes;
        if (std::getenv("NXRT_AUDIO_PROBE") && input.size() >= 0x40) {
            u32 h[16];
            std::memcpy(h, input.data(), sizeof(h));
            const size_t voices = 0x40 + h[1] + h[2] + h[4];
            u32 active = 0;
            for (u32 i = 0; i < params_.voices && voices + (i + 1) * 0x188 <= input.size(); ++i) {
                active += input[voices + i * 0x188 + 9] != 0;
            }
            static u32 last_active = ~0u;
            if (active != last_active) {
                LOG_INFO("audio probe: %u voices in use (update %d)", active, probes);
                if (active && last_active == 0) Probe(input);
                last_active = active;
            }
        }
        std::vector<u8> out(ctx.OutBufferSize(0));
        renderer_->Update(input, out);
        ctx.WriteBuffer(out.data(), out.size(), 0);
    }

    void StartTicking() {
        if (running_->exchange(true)) return;
        auto event = event_;
        auto renderer = renderer_;
        auto running = running_;
        if (std::getenv("NXRT_NO_TICK")) {
            LOG_INFO("audio: tick thread disabled (NXRT_NO_TICK)");
            return;
        }
        // Every 5 ms: render one frame to the host output, then tell the game.
        std::thread([renderer, event, running] {
            std::vector<float> stereo;
            auto next = std::chrono::steady_clock::now();
            while (*running) {
                next += std::chrono::milliseconds(5);
                std::this_thread::sleep_until(next);
                renderer->Render(stereo);
                audio::OutputQueue(stereo);
                event->Signal();
            }
        }).detach();
    }

    RendererParams params_;
    u32 rev_ = 0;
    std::shared_ptr<audio::Renderer> renderer_;
    std::shared_ptr<std::atomic<bool>> running_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<KEvent> event_ = MakeEvent("audren:system", false);
};

class IAudioDevice final : public ServiceObject {
public:
    IAudioDevice() : ServiceObject("IAudioDevice") {
        auto list = [](Ctx& ctx) {
            char name[0x100] = "AudioTvOutput";
            ctx.WriteBuffer(name, std::min<size_t>(sizeof(name), ctx.OutBufferSize()));
            ctx.Push<u32>(1);
        };
        auto active = [](Ctx& ctx) {
            char name[0x100] = "AudioTvOutput";
            ctx.WriteBuffer(name, std::min<size_t>(sizeof(name), ctx.OutBufferSize()));
        };
        Register(0, "ListAudioDeviceName", list);
        RegisterStub(1, "SetAudioDeviceOutputVolume");
        Register(2, "GetAudioDeviceOutputVolume", [](Ctx& ctx) { ctx.Push<float>(1.0f); });
        Register(3, "GetActiveAudioDeviceName", active);
        Register(4, "QueryAudioDeviceSystemEvent", [this](Ctx& ctx) { ctx.PushCopyObject(event_); });
        Register(5, "GetActiveChannelCount", [](Ctx& ctx) { ctx.Push<u32>(2); });
        Register(6, "ListAudioDeviceNameAuto", list);
        RegisterStub(7, "SetAudioDeviceOutputVolumeAuto");
        Register(8, "GetAudioDeviceOutputVolumeAuto", [](Ctx& ctx) { ctx.Push<float>(1.0f); });
        Register(10, "GetActiveAudioDeviceNameAuto", active);
        Register(11, "QueryAudioDeviceInputEvent", [this](Ctx& ctx) { ctx.PushCopyObject(event_); });
        Register(12, "QueryAudioDeviceOutputEvent", [this](Ctx& ctx) { ctx.PushCopyObject(event_); });
        Register(13, "GetActiveAudioOutputDeviceName", active);
        Register(14, "ListAudioOutputDeviceName", list);
    }

private:
    std::shared_ptr<KEvent> event_ = MakeEvent("audio:device", true);
};

class AudRenU final : public ServiceObject {
public:
    AudRenU() : ServiceObject("audren:u") {
        Register(0, "OpenAudioRenderer", [](Ctx& ctx) {
            const RendererParams p = ctx.Pop<RendererParams>();
            ctx.PushInterface(std::make_shared<IAudioRenderer>(p));
        });
        Register(1, "GetWorkBufferSize", [](Ctx& ctx) {
            const RendererParams p = ctx.Pop<RendererParams>();
            // Generous: the work buffer is only ever ours to use.
            const u64 size = 0x100000 + static_cast<u64>(p.voices) * 0x1000 + static_cast<u64>(p.mixes) * 0x8000 +
                             static_cast<u64>(p.sample_count) * 0x800;
            ctx.Push<u64>((size + 0xFFF) & ~u64{0xFFF});
        });
        Register(2, "GetAudioDeviceService", [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IAudioDevice>()); });
        Register(4, "GetAudioDeviceServiceWithRevisionInfo",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IAudioDevice>()); });
    }
};

} // namespace

void RegisterAudioServices() {
    RegisterService("audren:u", [] { return std::make_shared<AudRenU>(); });
}

} // namespace nxrt::hle
