// SPDX-License-Identifier: GPL-3.0-or-later
#include "audio/output.h"

#include <SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <string>

#include "common.h"
#include "runtime.h"

namespace nxrt::audio {

namespace {

SDL_AudioDeviceID g_device = 0;
std::once_flag g_once;
// Steady-state depth on SDL/PipeWire is ~75-85 ms (the device starts consuming late and the producer keeps real
// time), and a late producer tick is followed by a catch-up burst of about 20 ms; 100 ms dropped chunks in a 240 s
// run. 150 ms keeps latency bounded while leaving room for both.
constexpr u32 kMaxQueuedBytes = 48000 * 2 * sizeof(float) * 15 / 100; // 150 ms

void Open() {
    // On in windowed mode; NXRT_NO_AUDIO=1 turns it off. Offscreen/headless runs (regression replays) stay silent
    // unless NXRT_AUDIO is set.
    if (std::getenv("NXRT_NO_AUDIO")) return;
    if ((g_config.offscreen || g_config.headless) && !std::getenv("NXRT_AUDIO")) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        LOG_WARN("audio: SDL audio unavailable: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 512;
    g_device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_device) {
        LOG_WARN("audio: no output device: %s", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(g_device, 0);
    LOG_INFO("audio: output open (%d Hz, %d channels)", have.freq, have.channels);
}

} // namespace

bool OutputReady() {
    std::call_once(g_once, Open);
    return g_device != 0;
}

void OutputQueue(const std::vector<float>& stereo) {
    if (!OutputReady()) return;
    static std::mutex stats_mutex;
    static u64 frames_generated = 0;
    static u64 queue_limit_drops = 0;
    static u64 queue_limit_drop_frames = 0;
    static u64 queue_failures = 0;
    static std::string last_queue_error;
    static float peak = 0;
    // Latency/underrun stats: the queue depth seen on entry is how far ahead of the device we are.
    static u64 calls = 0;
    static u64 underruns = 0;
    static u32 queued_min = ~0u, queued_max = 0;
    static u64 queued_sum = 0;
    static float max_step = 0; // largest left-channel sample-to-sample jump: a click detector
    static float last_left = 0;
    const u32 queued_now = SDL_GetQueuedAudioSize(g_device);
    const bool queue_limit_drop = queued_now > kMaxQueuedBytes;
    bool queue_failed = false;
    std::string queue_error;
    if (!queue_limit_drop &&
        SDL_QueueAudio(g_device, stereo.data(), static_cast<u32>(stereo.size() * sizeof(float))) != 0) {
        queue_failed = true;
        queue_error = SDL_GetError();
    }

    bool report = false;
    u64 report_frames = 0, report_drops = 0, report_drop_frames = 0, report_failures = 0;
    float report_peak = 0, report_step = 0;
    u64 report_calls = 0, report_underruns = 0, report_qsum = 0;
    u32 report_qmin = 0, report_qmax = 0;
    std::string report_error;
    {
        std::lock_guard stats_lock(stats_mutex);
        frames_generated += stereo.size() / 2;
        // An empty queue on entry (after the first call) means the device ran dry: an audible gap.
        if (calls > 0 && queued_now == 0) ++underruns;
        ++calls;
        queued_min = std::min(queued_min, queued_now);
        queued_max = std::max(queued_max, queued_now);
        queued_sum += queued_now;
        for (float v : stereo) peak = std::max(peak, std::abs(v));
        for (size_t i = 0; i + 1 < stereo.size(); i += 2) {
            max_step = std::max(max_step, std::abs(stereo[i] - last_left));
            last_left = stereo[i];
        }
        if (queue_limit_drop) {
            ++queue_limit_drops;
            queue_limit_drop_frames += stereo.size() / 2;
        } else if (queue_failed) {
            if (queue_failures == 0) last_queue_error = queue_error;
            ++queue_failures;
        }
        if (frames_generated >= 48000 * 5) {
            report = true;
            report_frames = frames_generated;
            report_drops = queue_limit_drops;
            report_drop_frames = queue_limit_drop_frames;
            report_failures = queue_failures;
            report_peak = peak;
            report_calls = calls;
            report_step = max_step;
            max_step = 0;
            report_underruns = underruns;
            report_qmin = queued_min;
            report_qmax = queued_max;
            report_qsum = queued_sum;
            calls = underruns = queued_sum = 0;
            queued_min = ~0u;
            queued_max = 0;
            report_error = last_queue_error;
            frames_generated = 0;
            queue_limit_drops = 0;
            queue_limit_drop_frames = 0;
            queue_failures = 0;
            last_queue_error.clear();
            peak = 0;
        }
    }
    if (report) {
        // Bytes -> ms at 48 kHz stereo float (8 bytes per frame).
        const auto ms = [](double bytes) { return bytes / 8.0 / 48.0; };
        if (std::getenv("NXRT_AUDIO_STATUS")) {
            LOG_INFO("audio: output queue %llu calls, depth min %.1f / avg %.1f / max %.1f ms, underruns %llu, max step %.4f",
                     static_cast<unsigned long long>(report_calls), ms(report_qmin),
                     ms(report_calls ? static_cast<double>(report_qsum) / report_calls : 0), ms(report_qmax),
                     static_cast<unsigned long long>(report_underruns), report_step);
        }
        if (report_drops || report_failures) {
            LOG_WARN("audio: %llu frames generated, peak %.3f in the last 5 s; queue-limit drops %llu (%llu frames), "
                     "SDL_QueueAudio failures %llu%s%s",
                     static_cast<unsigned long long>(report_frames), report_peak,
                     static_cast<unsigned long long>(report_drops),
                     static_cast<unsigned long long>(report_drop_frames),
                     static_cast<unsigned long long>(report_failures),
                     report_error.empty() ? "" : "; first error: ", report_error.c_str());
        } else {
            LOG_INFO("audio: %llu frames generated, peak %.3f in the last 5 s",
                     static_cast<unsigned long long>(report_frames), report_peak);
        }
    }
}

} // namespace nxrt::audio
