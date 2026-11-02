// SPDX-License-Identifier: GPL-3.0-or-later
// Draws the game's NVN command lists with Vulkan. State comes from the
// recorder's model (objects followed through their NVN calls, state blocks read
// from guest memory when bound); shaders are the game's own Maxwell programs,
// translated to SPIR-V; every frame the window texture the game presents is
// shown in the window.
//
// First version, deliberately simple: one command buffer, submitted and waited
// for at the end of each submitted list; resources uploaded from guest memory
// per draw. Correctness first, speed later.
#include "render/astc_decoder_spv.h"
#include "render/counter_trace.h"
#include "render/nvn_renderer.h"
#include "render/depth_clear_policy.h"
#include "render/draw_census.h"
#include "render/graphics_state_cache.h"
#include "render/sampler_border_policy.h"
#include "render/rendered_srgb_content.h"
#include "render/block_texel_target.h"
#include "render/program_trace.h"
#include "services/hid.h"
#include "render/async_writeback.h"
#include "render/descriptor_info_storage.h"
#include "render/flush_policy.h"
#include "render/push_descriptor_policy.h"
#include "render/texture_descriptor_policy.h"
#include "render/guest_buffer_upload_cache.h"
#include "render/guest_memory_range.h"
#include "render/compute_writeback_policy.h"
#include "render/execute_lock_scope.h"
#include "render/resolution_model_boundary.h"
#include "nvn/execution_model_snapshot.h"
#include "nvn/texture_copy.h"
#include "render/gpu_timestamps.h"
#include "services/input_record.h"

#include <algorithm>
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
#include <atomic>
#endif
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <future>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <filesystem>
#include <iterator>
#include <system_error>
#include <deque>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "astcenc.h"
#include "nvn/nvn_layer.h"

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/ir/modifiers.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/instruction.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/runtime_info.h"

#include "../common.h" // after the vendored headers: our LOG_* macros win
#include "memory.h"
#include "nvn/nvn_layer.h"
#include "nvn/recorder.h"
#include "render/gpu_code_range.h"
#include "render/framebuffer_cache_key.h"
#include "render/image_backing_identity.h"
#include "render/image_row_transform.h"
#include "render/maxwell.h"
#include "render/compute_chain_cache.h"
#include "render/pipeline_cache_key.h"
#include "render/shader_translation_cache.h"
#include "render/process_memory_status.h"
#include "render/soft_dirty_backend.h"
#include "render/texture_dirty_tracker.h"
#include "render/sample_profiler.h"
#include "render/xxhash64.h"
#include "render/texture_cache_key.h"
#include "render/shader_failure_trace.h"
#include "render/vk_context.h"
#include "render/win_sample_trace.h"
#include "log_macros.h" // last: ours, not the shader translator's

namespace nxrt {
extern std::atomic<u64> g_slow_path_loads, g_slow_path_stores; // memory.cpp, diagnostic
}

namespace nxrt::render {

namespace {

using nvn::Command;
using nvn::CommandList;
using nvn::GpuAddr;
using nvn::GuestAddr;
using nvn::Op;

constexpr size_t kStages = 5; // pipeline order: vertex, tess control, tess eval, geometry, fragment
// NVN numbers stages vertex 0, fragment 1, geometry 2, tess control 3, tess eval 4.
constexpr std::array<size_t, kStages> kNvnStageOfPipelineStage{0, 3, 4, 2, 1};
constexpr size_t kComputeNvnStage = 5; // NVN_SHADER_STAGE_COMPUTE: the stage index of its bindings
constexpr u32 kTextureBoundBuffer = 2; // the driver's per-stage handle buffer (c2)
constexpr u32 kStorageBoundBuffer = 0; // the driver's c0: storage buffer descriptors from StorageBase
constexpr u32 kUniformSlotBase = 3;    // NVN uniform buffer i is Maxwell const buffer i + 3
// NXRT_STREAM_MIB sizes the per-flush upload stream (default 64). The heavy island scene uploads ~50 MiB per frame, which
// crosses the flush headroom every frame at 64 MiB and costs a synchronous StreamFull flush each time.
// NXRT_NO_ASYNC_GRAPHICS=1 opts out: the scene is submitted at the end of Execute/Present without waiting for the GPU; the
// wait happens when the stream, the descriptor pool or the slot ring is used up. Those are sized up to match.
// NXRT_SAFE_MODE=1: the conservative path for GPUs/drivers nobody has tested. It turns off the newer, faster paths: asynchronous graphics
// submit, asynchronous compute flush and the GPU ASTC decoder (textures are decoded on the CPU instead).
bool SafeMode() {
    static const bool on = [] {
        const char* v = std::getenv("NXRT_SAFE_MODE");
        return v && *v && std::strcmp(v, "0") != 0;
    }();
    return on;
}

bool AsyncGraphicsEnabled() {
    // On by default since Alpha 1.6.9; NXRT_NO_ASYNC_GRAPHICS=1 (or NXRT_SAFE_MODE=1) waits for the GPU at the end of every Execute/Present again.
    static const bool on = std::getenv("NXRT_NO_ASYNC_GRAPHICS") == nullptr && !SafeMode();
    return on;
}
size_t StreamSizeFromEnv() {
    if (AsyncGraphicsEnabled() && !std::getenv("NXRT_STREAM_MIB")) return size_t{192} << 20;
    if (const char* v = std::getenv("NXRT_STREAM_MIB")) {
        const long n = std::strtol(v, nullptr, 10);
        if (n >= 32 && n <= 512) return static_cast<size_t>(n) << 20;
    }
    return size_t{64} << 20;
}
const size_t kStreamSize = StreamSizeFromEnv();
// NXRT_TRACE_HITCH_MS=<ms> (opt-in): when the wall time between two presents exceeds <ms>, log where the render thread
// spent it. Categories are inclusive wall time of the scoped regions (pipeline compile includes shader translation).
enum HitchCat : u32 { kHitchPipeline, kHitchImageCreate, kHitchSampled, kHitchRefresh, kHitchFlush, kHitchWaitAll,
                      kHitchCopyToBuffer, kHitchCopy, kHitchWriteback, kHitchStream, kHitchAstc, kHitchBlockSync, kHitchUpload, kHitchDispatch, kHitchDescriptors, kHitchSetup, kHitchUbo, kHitchSsbo, kHitchSsboRead, kHitchSsboFwd, kHitchSsboGuard, kHitchPipeKey, kHitchPipeFind, kHitchDriverPipe, kHitchDrawPipe, kHitchDrawDesc, kHitchRecord, kHitchPost, kHitchPoll, kHitchAsyncFlush, kHitchSubmit, kHitchLedgerAdd, kHitchPipelineLookup,
                      kHitchCount };
constexpr const char* kHitchNames[kHitchCount] = {"pipeline", "image-create", "sampled", "refresh", "flush", "wait-all",
                                                  "copy-to-buffer", "copy", "writeback", "stream", "astc-decode", "block-sync", "upload", "dispatch", "descriptors", "setup", "ubo", "ssbo", "ssbo-read", "ssbo-fwd", "ssbo-guard", "pipe-key", "pipe-find", "driver-pipe", "draw-pipe", "draw-desc", "record", "post", "poll", "async-flush", "submit", "ledger-add", "pipeline-lookup"};
const double kHitchMs = [] {
    const char* v = std::getenv("NXRT_TRACE_HITCH_MS");
    return v && *v ? std::strtod(v, nullptr) : 0.0;
}();
struct HitchStats {
    double ms[kHitchCount]{};
    u32 n[kHitchCount]{};
    u32 depth[kHitchCount]{};
    double worst_ms[kHitchCount]{};  // slowest single outermost call per region this present
    u64 worst_tag[kHitchCount]{};
    std::unordered_map<u64, u32> dispatches;  // compute dispatches per bound program this present
    u32 dispatch_total = 0;
    u64 ssbo_ro_bytes = 0, ssbo_ro_n = 0, ssbo_ro_hit_bytes = 0, ssbo_ro_hit_n = 0;
    std::unordered_set<u64> ro_seen;
    // Pending-overlap class of each storage-buffer read that was not forwarded: ms spent in Guard and count.
    std::map<std::string, std::pair<double, u32>> guard_class;
    u64 ssbo_read_bytes = 0, ssbo_read_n = 0, ssbo_fwd_n = 0, ubo_read_bytes = 0;
    double reason_ms[8]{};  // Flush() wall time per FlushReason
    u32 reason_n[8]{};
};
HitchStats g_hitch;
struct HitchReasonScope {
    size_t reason;
    std::chrono::steady_clock::time_point start;
    explicit HitchReasonScope(size_t r) : reason(r) {
        if (kHitchMs > 0.0) start = std::chrono::steady_clock::now();
    }
    ~HitchReasonScope() {
        if (kHitchMs <= 0.0 || reason >= 8) return;
        g_hitch.reason_ms[reason] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        ++g_hitch.reason_n[reason];
    }
};
struct HitchScope {
    HitchCat cat;
    std::chrono::steady_clock::time_point start;
    u64 tag;
    explicit HitchScope(HitchCat c, u64 t = 0) : cat(c), tag(t) {
        if (kHitchMs > 0.0 && g_hitch.depth[cat]++ == 0) start = std::chrono::steady_clock::now();
    }
    ~HitchScope() {
        if (kHitchMs <= 0.0) return;
        if (--g_hitch.depth[cat] == 0) {
            const double d = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            g_hitch.ms[cat] += d;
            ++g_hitch.n[cat];
            if (d > g_hitch.worst_ms[cat]) g_hitch.worst_ms[cat] = d, g_hitch.worst_tag[cat] = tag;
        }
    }
};
// The environment is fixed for the run: read the diagnostic switches that sit on per-draw / per-sample paths once.
const bool kEnvTraceDraws = std::getenv("NXRT_TRACE_DRAWS") != nullptr;
const bool kEnvProbeFloorD32Mirror = std::getenv("NXRT_PROBE_FLOOR_D32_Y_MIRROR") != nullptr;
const char* const kEnvProbeDrawTarget = std::getenv("NXRT_PROBE_DRAW_TARGET");
// NXRT_ASYNC_COMPUTE_FLUSH budgets. Past any of them the asynchronous path falls back to the synchronous flush,
// which waits for everything and resets the stream, descriptor pool and retired images.
constexpr size_t kMaxAsyncRingSlots = 32;
const size_t kAsyncRingSlots = AsyncGraphicsEnabled() ? 24 : 10;
const size_t kAsyncStreamLimit = kStreamSize - (16 << 20);
const u64 kAsyncDescriptorSetLimit = AsyncGraphicsEnabled() ? 24000 : 6000; // pool maxSets is 8192 (32768)

// GPU ASTC decode (suyu astc_decoder.comp), default on (NXRT_ASTC_GPU=0 = CPU): decodes block-linear ASTC textures in a compute pass
// instead of astcenc on the CPU (29-38 ms per 2048x2048 texture, the main scene-transition hitch).
bool AstcGpuEnabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("NXRT_ASTC_GPU");
        return !SafeMode() && !(v && std::strcmp(v, "0") == 0);
    }();
    return enabled;
}

struct AstcGpuPipeline {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool tried = false;
};
struct AstcGpuPush {
    u32 block_dims[2], layer_stride, block_size, x_shift, block_height, block_height_mask;
};

AstcGpuPipeline& AstcPipeline(const VulkanContext& vk) {
    static AstcGpuPipeline astc_pipe;
    if (!astc_pipe.tried) {
        astc_pipe.tried = true;
        const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
            {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
        const VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
                                                  2, bindings.data()};
        const VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AstcGpuPush)};
        const VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                           sizeof(kAstcDecoderSpv), kAstcDecoderSpv};
        VkShaderModule module = VK_NULL_HANDLE;
        bool ok = vkCreateDescriptorSetLayout(vk.device, &dsl, nullptr, &astc_pipe.set_layout) == VK_SUCCESS;
        if (ok) {
            const VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1,
                                                 &astc_pipe.set_layout, 1, &range};
            ok = vkCreatePipelineLayout(vk.device, &pli, nullptr, &astc_pipe.layout) == VK_SUCCESS;
        }
        ok = ok && vkCreateShaderModule(vk.device, &smi, nullptr, &module) == VK_SUCCESS;
        if (ok) {
            const VkComputePipelineCreateInfo cpi{
                VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
                {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT,
                 module, "main", nullptr},
                astc_pipe.layout, VK_NULL_HANDLE, 0};
            ok = vkCreateComputePipelines(vk.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &astc_pipe.pipeline) ==
                 VK_SUCCESS;
        }
        if (module) vkDestroyShaderModule(vk.device, module, nullptr);
        LOG_INFO("renderer: ASTC GPU decoder pipeline %s", ok ? "ready" : "FAILED");
    }
    return astc_pipe;
}

// The GPU decoder needs the pipeline to build and RGBA8 storage images (the sRGB images are written through a UNORM view).
bool AstcGpuUsable(const VulkanContext& vk) {
    static const bool usable = [&] {
        const bool formats = vk.FormatSupports(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) &&
                             vk.FormatSupports(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
        if (!formats) LOG_WARN("renderer: GPU lacks RGBA8 storage images; ASTC textures are decoded on the CPU");
        return formats && AstcPipeline(vk).pipeline != VK_NULL_HANDLE;
    }();
    return usable;
}

thread_local int t_renderer_depth = 0;         // >0 while the calling thread is inside Execute/Present
struct RendererScope {
    SamplerScope sampler;
    RendererScope() { ++t_renderer_depth; }
    ~RendererScope() { --t_renderer_depth; }
};
constexpr size_t kMaxShaderFailureCbufReads = 256;
constexpr u64 kHardwareGpuWaitTimeoutNs = 2'000'000'000ULL;
constexpr u64 kSoftwareGpuWaitTimeoutNs = 30'000'000'000ULL;

#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
class ComputeReadbackTrace {
public:
    static ComputeReadbackTrace& Instance() {
        static ComputeReadbackTrace trace;
        return trace;
    }

    static bool Enabled() { return Instance().enabled_; }

    static void OnRead(void* context, u64 address, u64 size, const char* file,
                       std::uint_least32_t line) noexcept {
        // NXRT_TRACE_PROGRAM: guest reads of the traced program's SSBO range (the renderer's own reads are not).
        if (t_renderer_depth == 0) ProgramTrace::Instance().OnGuestRead(address, size);
        if (auto* trace = static_cast<ComputeReadbackTrace*>(context)) trace->ObserveRead(address, size, file, line);
    }

    void SetFrame(u64 frame) noexcept {
        if (enabled_) frame_.store(frame, std::memory_order_relaxed);
    }

    void PrepareWriteback(u64 program, u64 frame, u64 address, u64 size) noexcept {
        if (!enabled_ || !size) return;
        try {
            const size_t slow_path_pages = MarkGuestMemoryRangeSlowPath(
                g_memory.PageTable(), {address, size}, AddressSpaceLayout::kEnd);
            if (slow_path_pages) {
                LOG_INFO("renderer: compute-readback q=%llu writer=0x%llx range=0x%llx/%llu "
                         "marked %zu guest page(s) for callback reads",
                         static_cast<unsigned long long>(frame), static_cast<unsigned long long>(program),
                         static_cast<unsigned long long>(address), static_cast<unsigned long long>(size),
                         slow_path_pages);
            }
        } catch (...) {
            // The trace is diagnostic-only; never let its logging affect rendering.
        }
    }

    void RecordWriteback(u64 program, u64 frame, u64 address, u64 size) noexcept {
        if (!enabled_ || !size) return;
        try {
            std::lock_guard lock(mutex_);
            tracker_.Record({program, frame, {address, size}});
        } catch (...) {
            // The trace is diagnostic-only; never let its allocation failure affect rendering.
        }
    }

    // Opt-in timing (NXRT_TRACE_COMPUTE_FLUSH): a compute result is "in flight" from the moment its dispatch is
    // recorded until the guest copy is written after the synchronous flush. Reads of an in-flight range, and the
    // first read after completion, are logged with their distance in time. Observation only.
    using TraceClock = std::chrono::steady_clock;
    struct TimedWriteback {
        u64 program, frame, address, size;
        u32 flush_index;
        TraceClock::time_point recorded, submit, done;
    };

    static bool TimingEnabled() {
        static const bool on = std::getenv("NXRT_TRACE_COMPUTE_FLUSH") != nullptr;
        return on;
    }

    void NoteInFlight(u64 program, u64 frame, u64 address, u64 size, u32 flush_index) noexcept {
        if (!enabled_ || !TimingEnabled() || !size) return;
        try {
            std::lock_guard lock(mutex_);
            renderer_thread_ = std::this_thread::get_id();
            in_flight_.push_back({program, frame, address, size, flush_index, TraceClock::now(), {}, {}});
        } catch (...) {
        }
    }

    void NoteDone(u64 program, u64 frame, u64 address, u64 size, TraceClock::time_point submit) noexcept {
        if (!enabled_ || !TimingEnabled() || !size) return;
        try {
            std::lock_guard lock(mutex_);
            for (auto it = in_flight_.rbegin(); it != in_flight_.rend(); ++it) {
                if (it->address != address || it->size != size || it->frame != frame || it->done != TraceClock::time_point{})
                    continue;
                it->submit = submit;
                it->done = TraceClock::now();
                done_.erase(std::remove_if(done_.begin(), done_.end(),
                                           [&](const TimedWriteback& d) { return d.address == address && d.size == size; }),
                            done_.end());
                done_.push_back(*it);
                if (done_.size() > 64) done_.erase(done_.begin());
                in_flight_.erase(std::next(it).base());
                return;
            }
        } catch (...) {
        }
    }

    void QueuePendingWriteback(u64 program, u64 frame, u64 address, u64 size) noexcept {
        if (!enabled_ || !size) return;
        PrepareWriteback(program, frame, address, size);
        try {
            std::lock_guard lock(mutex_);
            pending_writebacks_.erase(
                std::remove_if(pending_writebacks_.begin(), pending_writebacks_.end(), [&](const GuestWritebackRecord& r) {
                    return r.range.address == address && r.range.size == size && r.frame <= frame;
                }),
                pending_writebacks_.end());
            pending_writebacks_.push_back({program, frame, {address, size}});
        } catch (...) {
            // The trace is diagnostic-only; never let its allocation failure affect rendering.
        }
    }

    void CompletePendingWriteback(u64 program, u64 frame, u64 address, u64 size) noexcept {
        if (!enabled_ || !size) return;
        try {
            std::lock_guard lock(mutex_);
            pending_writebacks_.erase(
                std::remove_if(pending_writebacks_.begin(), pending_writebacks_.end(), [&](const GuestWritebackRecord& r) {
                    return r.program == program && r.frame == frame && r.range.address == address &&
                           r.range.size == size;
                }),
                pending_writebacks_.end());
        } catch (...) {
            // The trace is diagnostic-only; never let its allocation failure affect rendering.
        }
    }

private:
    void ObserveRead(u64 address, u64 size, const char* file, std::uint_least32_t line) noexcept {
        if (!enabled_ || !size) return;
        try {
            const u64 frame = frame_.load(std::memory_order_relaxed);
            if (trace_from_frame_ && frame < trace_from_frame_) return;
            std::lock_guard lock(mutex_);
            if (TimingEnabled()) {
                const auto now = TraceClock::now();
                const bool by_renderer = std::this_thread::get_id() == renderer_thread_;
                const auto tid = static_cast<unsigned long long>(std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xffff);
                // Reads of a range whose result is still on the GPU (dispatch recorded, guest copy not written).
                for (const auto& w : in_flight_) {
                    if (!GuestMemoryRangesOverlap({w.address, w.size}, {address, size})) continue;
                    const auto key = std::make_tuple(frame, w.frame, w.program, w.address, w.size,
                                                     file ? std::string_view(file) : std::string_view(), line);
                    if (inflight_seen_.size() >= 4 * kEventLimit || !inflight_seen_.insert(key).second) continue;
                    LOG_INFO("renderer: cflush-read-inflight q=%llu writer=0x%llx write=0x%llx/%llu flush_idx=%u "
                             "reader=0x%llx/%llu by=%s tid=%llx since_record=%.3fms at=%s:%u",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(w.program),
                             static_cast<unsigned long long>(w.address), static_cast<unsigned long long>(w.size),
                             w.flush_index, static_cast<unsigned long long>(address),
                             static_cast<unsigned long long>(size), by_renderer ? "renderer" : "other", tid,
                             std::chrono::duration<double, std::milli>(now - w.recorded).count(), file ? file : "?",
                             static_cast<unsigned>(line));
                }
                for (const auto& w : done_) {
                    if (!GuestMemoryRangesOverlap({w.address, w.size}, {address, size})) continue;
                    const auto key = std::make_tuple(frame, w.frame, w.program, w.address, w.size,
                                                     file ? std::string_view(file) : std::string_view(), line);
                    if (timed_seen_.size() >= 4 * kEventLimit || !timed_seen_.insert(key).second) continue;
                    LOG_INFO("renderer: cflush-read q=%llu writer_q=%llu writer=0x%llx write=0x%llx/%llu flush_idx=%u "
                             "reader=0x%llx/%llu by=%s tid=%llx since_submit=%.3fms since_done=%.3fms at=%s:%u",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(w.frame),
                             static_cast<unsigned long long>(w.program), static_cast<unsigned long long>(w.address),
                             static_cast<unsigned long long>(w.size), w.flush_index,
                             static_cast<unsigned long long>(address), static_cast<unsigned long long>(size),
                             by_renderer ? "renderer" : "other", tid,
                             std::chrono::duration<double, std::milli>(now - w.submit).count(),
                             std::chrono::duration<double, std::milli>(now - w.done).count(), file ? file : "?",
                             static_cast<unsigned>(line));
                }
            }
            if (pending_trace_only_) {
                for (const auto& write : pending_writebacks_) {
                    if (frame < write.frame || frame - write.frame > 1 ||
                        !GuestMemoryRangesOverlap(write.range, {address, size})) {
                        continue;
                    }
                    if (pending_events_ >= kEventLimit) {
                        if (!pending_truncated_) {
                            LOG_INFO("renderer: compute-forwarding read trace reached its %zu-event cap", kEventLimit);
                            pending_truncated_ = true;
                        }
                        break;
                    }
                    const auto key = std::make_tuple(frame, write.frame, write.program, write.range.address,
                                                     write.range.size, file ? std::string_view(file) : std::string_view(),
                                                     line);
                    if (!pending_observed_reads_.insert(key).second) continue;
                    LOG_INFO("renderer: compute-forwarding-read q=%llu writer_q=%llu writer=0x%llx "
                             "write=0x%llx/%llu reader=0x%llx/%llu at=%s:%u",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(write.frame),
                             static_cast<unsigned long long>(write.program),
                             static_cast<unsigned long long>(write.range.address),
                             static_cast<unsigned long long>(write.range.size), static_cast<unsigned long long>(address),
                             static_cast<unsigned long long>(size), file ? file : "?", static_cast<unsigned>(line));
                    ++pending_events_;
                }
                return;
            }
            const auto matches = tracker_.FindOverlappingWrites(frame, {address, size});
            if (matches.empty()) return;
            for (const auto& write : matches) {
                if (events_ >= kEventLimit) {
                    if (!truncated_) {
                        LOG_INFO("renderer: compute-readback trace reached its %zu-event cap", kEventLimit);
                        truncated_ = true;
                    }
                    break;
                }
                const auto key = std::make_tuple(frame, write.frame, write.program, write.range.address,
                                                 write.range.size, file ? std::string_view(file) : std::string_view(),
                                                 line);
                // Generated guest code may read a compute result one word at a time. Keep one
                // event per writer/read callsite/frame so a large sequential read cannot fill
                // the diagnostic cap before other writeback ranges are observed.
                if (!observed_reads_.insert(key).second) continue;
                LOG_INFO("renderer: compute-readback q=%llu writer_q=%llu writer=0x%llx write=0x%llx/%llu "
                         "reader=0x%llx/%llu at=%s:%u",
                         static_cast<unsigned long long>(frame),
                         static_cast<unsigned long long>(write.frame),
                         static_cast<unsigned long long>(write.program),
                         static_cast<unsigned long long>(write.range.address),
                         static_cast<unsigned long long>(write.range.size),
                         static_cast<unsigned long long>(address), static_cast<unsigned long long>(size),
                         file ? file : "?", static_cast<unsigned>(line));
                ++events_;
            }
        } catch (...) {
            // The trace is diagnostic-only; never let its allocation failure affect rendering.
        }
    }

    static constexpr size_t kEventLimit = 256;
    const bool enabled_ = std::getenv("NXRT_TRACE_COMPUTE_READBACKS") != nullptr;
    const bool pending_trace_only_ = std::getenv("NXRT_FORWARD_COMPUTE_WRITEBACKS") != nullptr;
    const u64 trace_from_frame_ = [] {
        const char* value = std::getenv("NXRT_TRACE_COMPUTE_READBACKS_FROM_Q");
        return value && *value ? std::strtoull(value, nullptr, 0) : 0;
    }();
    std::mutex mutex_;
    GuestMemoryReadbackTracker tracker_;
    std::vector<GuestWritebackRecord> pending_writebacks_;
    std::vector<TimedWriteback> in_flight_, done_;
    std::thread::id renderer_thread_;
    std::set<std::tuple<u64, u64, u64, u64, u64, std::string_view, std::uint_least32_t>> inflight_seen_, timed_seen_;
    std::atomic<u64> frame_{0};
    std::set<std::tuple<u64, u64, u64, u64, u64, std::string_view, std::uint_least32_t>> observed_reads_;
    std::set<std::tuple<u64, u64, u64, u64, u64, std::string_view, std::uint_least32_t>> pending_observed_reads_;
    size_t events_ = 0;
    bool truncated_ = false;
    size_t pending_events_ = 0;
    bool pending_truncated_ = false;
};
#endif

void AccumulateDescriptorInfoCapacities(const Shader::Info& info, size_t& buffer_infos, size_t& image_infos) {
    for (const auto& desc : info.constant_buffer_descriptors) buffer_infos += desc.count;
    for (const auto& desc : info.storage_buffers_descriptors) buffer_infos += desc.count;
    for (const auto& desc : info.texture_descriptors) image_infos += desc.count;
}

u64 GpuWaitTimeoutNs(VkPhysicalDeviceType device_type) {
    const char* value = std::getenv("NXRT_GPU_WAIT_MS");
    if (!value || !*value) {
        return device_type == VK_PHYSICAL_DEVICE_TYPE_CPU ? kSoftwareGpuWaitTimeoutNs : kHardwareGpuWaitTimeoutNs;
    }
    char* end = nullptr;
    const unsigned long long parsed_ms = std::strtoull(value, &end, 10);
    if (end == value || *end != '\0') {
        return device_type == VK_PHYSICAL_DEVICE_TYPE_CPU ? kSoftwareGpuWaitTimeoutNs : kHardwareGpuWaitTimeoutNs;
    }
    return std::clamp<u64>(parsed_ms, 1, 120'000) * 1'000'000ULL;
}

// NXRT_TRACE_PROGRAM hooks around the guest copy of a compute write-back (no-ops when the trace is off).
inline void ProgramTraceBeforeWriteback(u64 program, u64 cpu, u64 size) {
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    auto& trace = ProgramTrace::Instance();
    if (trace.Enabled() && program == trace.Program())
        MarkGuestMemoryRangeSlowPath(g_memory.PageTable(), {cpu, size}, AddressSpaceLayout::kEnd);
#else
    (void)program; (void)cpu; (void)size;
#endif
}
inline void ProgramTraceAfterWriteback(u64 program, u64 dispatch, u64 cpu, const u8* data, size_t size) {
    auto& trace = ProgramTrace::Instance();
    if (trace.Enabled() && program == trace.Program()) trace.Writeback(dispatch, cpu, data, size);
}

struct WinSampleTraceConfig {
    bool enabled = false;
    bool valid = true;
    u64 target_key = 0;
    std::optional<u64> frame;
    std::optional<u64> serial;
    std::optional<u64> program;
    std::optional<u64> index_count;
    std::optional<u64> target_width;
    std::optional<u64> target_height;
    std::vector<u64> checkpoint_draw_ids;
};

std::optional<u64> ParseTraceNumber(const char* value, bool allow_q_prefix) {
    if (!value || !*value) return std::nullopt;
    std::string_view text(value);
    if (allow_q_prefix && (text.front() == 'q' || text.front() == 'Q')) text.remove_prefix(1);
    u64 parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed, 10);
    if (text.empty() || error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
    return parsed;
}

// Events kept per traced frame (inputs and draw results of the NXRT_TRACE_WIN_SAMPLE target).
// Keep enough room for the complete set of inputs and draw results when a
// target is traced for one frame. This remains bounded and opt-in.
constexpr size_t kWinTraceCap = 512;

const WinSampleTraceConfig& GetWinSampleTraceConfig() {
    static const WinSampleTraceConfig config = [] {
        WinSampleTraceConfig parsed;
        const char* target = std::getenv("NXRT_TRACE_WIN_SAMPLE");
        if (!target || !*target) return parsed;
        parsed.enabled = true;
        char* target_end = nullptr;
        parsed.target_key = std::strtoull(target, &target_end, 16);
        if (target_end == target || *target_end != '\0') parsed.valid = false;

        const char* frame = std::getenv("NXRT_TRACE_WIN_SAMPLE_FRAME");
        if (frame) {
            parsed.frame = ParseTraceNumber(frame, true);
            if (!parsed.frame) parsed.valid = false;
        }
        const char* serial = std::getenv("NXRT_TRACE_WIN_SAMPLE_SERIAL");
        if (serial) {
            parsed.serial = ParseTraceNumber(serial, false);
            if (!parsed.serial) parsed.valid = false;
        }
        const char* program = std::getenv("NXRT_TRACE_WIN_SAMPLE_PROGRAM");
        if (program) {
            char* program_end = nullptr;
            const u64 program_id = std::strtoull(program, &program_end, 16);
            if (program_end == program || *program_end != '\0') {
                parsed.valid = false;
            } else {
                parsed.program = program_id;
            }
        }
        const char* index_count = std::getenv("NXRT_TRACE_WIN_SAMPLE_INDEX_COUNT");
        if (index_count) {
            parsed.index_count = ParseTraceNumber(index_count, false);
            if (!parsed.index_count) parsed.valid = false;
        }
        const char* target_width = std::getenv("NXRT_TRACE_WIN_SAMPLE_TARGET_WIDTH");
        if (target_width) {
            parsed.target_width = ParseTraceNumber(target_width, false);
            if (!parsed.target_width) parsed.valid = false;
        }
        const char* target_height = std::getenv("NXRT_TRACE_WIN_SAMPLE_TARGET_HEIGHT");
        if (target_height) {
            parsed.target_height = ParseTraceNumber(target_height, false);
            if (!parsed.target_height) parsed.valid = false;
        }
        if (const char* draws = std::getenv("NXRT_TRACE_WIN_CHECKPOINT_DRAWS")) {
            const auto ids = ParseWinSampleCheckpointDrawIds(draws);
            if (!ids || !parsed.frame) {
                parsed.valid = false;
            } else {
                parsed.checkpoint_draw_ids.assign(ids->begin(), ids->end());
            }
        }
        return parsed;
    }();
    return config;
}

// Diagnostic-only draw census (opt-in). NXRT_PROBE_DRAW_CENSUS=<frame> logs one line per draw of that
// frame with its targets, program, fixed-function state and outcome. NXRT_PROBE_CENSUS_CHECKPOINTS lists
// census sequence numbers ("12,40,41") or "every:N"; after each listed queued draw, color target 0 and the
// depth target are read back to NXRT_DUMP_DIR. Nothing changes when the variables are unset.
struct DrawCensusConfig {
    u64 frame = 0;
    u64 frame_end = 0; // NXRT_PROBE_DRAW_CENSUS=<lo>-<hi> covers a frame range (default: the single frame)
    u64 every = 0;
    std::vector<u64> checkpoints;
    bool Checkpoint(u64 seq) const {
        if (every && seq % every == 0) return true;
        return std::find(checkpoints.begin(), checkpoints.end(), seq) != checkpoints.end();
    }
};

const DrawCensusConfig& GetDrawCensusConfig() {
    static const DrawCensusConfig config = [] {
        DrawCensusConfig parsed;
        if (const char* frame = std::getenv("NXRT_PROBE_DRAW_CENSUS"); frame && *frame) {
            char* end = nullptr;
            parsed.frame = std::strtoull(frame, &end, 0);
            if (end && *end == '-') parsed.frame_end = std::strtoull(end + 1, nullptr, 0);
            if (parsed.frame_end < parsed.frame) parsed.frame_end = parsed.frame;
        }
        if (const char* list = std::getenv("NXRT_PROBE_CENSUS_CHECKPOINTS"); parsed.frame && list && *list) {
            const std::string text(list);
            if (text.rfind("every:", 0) == 0) {
                parsed.every = std::strtoull(text.c_str() + 6, nullptr, 0);
            } else {
                size_t begin = 0;
                while (begin < text.size()) {
                    const size_t end = std::min(text.find(',', begin), text.size());
                    const u64 value = std::strtoull(text.substr(begin, end - begin).c_str(), nullptr, 0);
                    if (value) parsed.checkpoints.push_back(value);
                    begin = end + 1;
                }
            }
        }
        return parsed;
    }();
    return config;
}

bool IsAstcFormat(u32 format) {
    switch (format) {
    case 0x40: case 0x41: case 0x42: case 0x44: case 0x45: case 0x46:
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        return true;
    default:
        return false;
    }
}

const unsigned kAstcThreads = [] {
    const char* v = std::getenv("NXRT_ASTC_THREADS");
    if (v && *v) return static_cast<unsigned>(std::clamp<long>(std::strtol(v, nullptr, 10), 1, 16));
    return std::clamp(std::thread::hardware_concurrency() / 2u, 2u, 8u);
}();

bool DecodeAstc(const std::vector<u8>& compressed, u32 width, u32 height, u32 block_width, u32 block_height,
                bool srgb, std::vector<u8>& rgba, std::string& error) {
    using ContextKey = std::tuple<u32, u32, bool>;
    struct Context {
        astcenc_context* value = nullptr;
        ~Context() { astcenc_context_free(value); }
    };
    static std::mutex mutex;
    static std::map<ContextKey, std::unique_ptr<Context>> contexts;
    std::lock_guard lock(mutex);

    const ContextKey key{block_width, block_height, srgb};
    auto& context = contexts[key];
    if (!context) {
        astcenc_config config{};
        const auto profile = srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_LDR;
        auto status = astcenc_config_init(profile, block_width, block_height, 1, ASTCENC_PRE_MEDIUM,
                                          ASTCENC_FLG_DECOMPRESS_ONLY, &config);
        if (status != ASTCENC_SUCCESS) {
            error = astcenc_get_error_string(status);
            contexts.erase(key);
            return false;
        }
        auto created = std::make_unique<Context>();
        status = astcenc_context_alloc(&config, kAstcThreads, &created->value, nullptr);
        if (status != ASTCENC_SUCCESS) {
            error = astcenc_get_error_string(status);
            contexts.erase(key);
            return false;
        }
        context = std::move(created);
    }

    rgba.resize(static_cast<size_t>(width) * height * 4);
    void* slices[] = {rgba.data()};
    astcenc_image image{width, height, 1, ASTCENC_TYPE_U8, slices};
    const astcenc_swizzle identity{ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
    // Big images are split across worker threads (each takes blocks from a shared counter); small ones are not worth
    // the thread start. NXRT_ASTC_THREADS=1 decodes on the calling thread only.
    astcenc_error status = ASTCENC_SUCCESS;
    if (kAstcThreads > 1 && compressed.size() >= (size_t{64} << 10)) {
        std::vector<astcenc_error> results(kAstcThreads, ASTCENC_SUCCESS);
        std::vector<std::thread> workers;
        workers.reserve(kAstcThreads - 1);
        for (unsigned t = 1; t < kAstcThreads; ++t) {
            workers.emplace_back([&, t] {
                results[t] = astcenc_decompress_image(context->value, compressed.data(), compressed.size(), &image,
                                                      &identity, t);
            });
        }
        results[0] = astcenc_decompress_image(context->value, compressed.data(), compressed.size(), &image, &identity, 0);
        for (std::thread& w : workers) w.join();
        for (astcenc_error r : results) {
            if (r != ASTCENC_SUCCESS) status = r;
        }
    } else {
        status = astcenc_decompress_image(context->value, compressed.data(), compressed.size(), &image, &identity, 0);
    }
    astcenc_decompress_reset(context->value);
    if (status != ASTCENC_SUCCESS) {
        error = astcenc_get_error_string(status);
        rgba.clear();
        return false;
    }
    return true;
}

// Driver data inside the game's SDK (Tomodachi Life v1.0.0); overridable for other games.
u64 EnvOffset(const char* name, u64 fallback) {
    const char* e = std::getenv(name);
    return e && *e ? std::strtoull(e, nullptr, 0) : fallback;
}

u64 Pack(u32 lo, u32 hi) {
    return lo | (static_cast<u64>(hi) << 32);
}

u64 Hash(const void* data, size_t size, u64 seed = 0xcbf29ce484222325ULL) {
    const u8* p = static_cast<const u8*>(data);
    u64 h = seed;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

// Allocator whose value-initialization leaves trivial elements alone: resize() of a scratch byte buffer that the next
// statement overwrites (ReadBlock) must not memset up to 16 MiB first.
template <class T>
struct DefaultInitAllocator : std::allocator<T> {
    template <class U>
    struct rebind {
        using other = DefaultInitAllocator<U>;
    };
    using std::allocator<T>::allocator;
    template <class U>
    void construct(U* ptr) noexcept(std::is_nothrow_default_constructible_v<U>) {
        ::new (static_cast<void*>(ptr)) U;
    }
    template <class U, class... Args>
    void construct(U* ptr, Args&&... args) {
        ::new (static_cast<void*>(ptr)) U(std::forward<Args>(args)...);
    }
};
using ScratchBytes = std::vector<u8, DefaultInitAllocator<u8>>;

// Word-at-a-time multiplicative hash for the per-draw keys (pipeline key, TIC and TSC contents). Not stable across
// versions: it only indexes in-memory caches.
u64 FastHash(const void* data, size_t size, u64 seed = 0xcbf29ce484222325ULL) {
    constexpr u64 kMul = 0x9E3779B97F4A7C15ULL;
    const u8* p = static_cast<const u8*>(data);
    u64 h = seed ^ (size * kMul);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h = (h ^ w) * kMul;
        h ^= h >> 29;
    }
    if (i < size) {
        u64 w = 0;
        std::memcpy(&w, p + i, size - i);
        h = (h ^ w) * kMul;
        h ^= h >> 29;
    }
    return h;
}

// Host image key of one mip level of a texture; level 0 keeps the texture's own key.
u64 LevelKey(u64 image_key, u32 level) {
    return level ? Hash(&level, sizeof(level), image_key) : image_key;
}

struct FormatInfo {
    u32 render_target = 0, tic_word0 = 0, bytes_per_block = 0, vertex_attribute = 0;
    bool valid = false;
};

// How the descriptor's sRGB bit applies to RGBA8 targets the game later samples.
// Default: only to the Mii skin (faceline) targets, 1:2 in size. Measured on the "Which Mii" screen against the
// console's own frame: decoding every such target squares the hair and shirt colours (brown hair came out black),
// ignoring the bit everywhere leaves the skin double-encoded (pale), and decoding only the faceline target matches
// hair, skin and shirt. NXRT_TIC_SRGB_ALL=1 restores decoding on every RGBA8 target (the previous default),
// NXRT_LEGACY_IGNORE_TIC_SRGB=1 ignores the bit everywhere.
inline bool TicSrgbAllTargets() { return std::getenv("NXRT_TIC_SRGB_ALL") != nullptr; }
inline bool FacelineSrgbSampling() {
    return std::getenv("NXRT_EXPERIMENT_FACELINE_SRGB") != nullptr ||
           (std::getenv("NXRT_LEGACY_IGNORE_TIC_SRGB") == nullptr && !TicSrgbAllTargets());
}

u32 EffectiveColorWriteMask(u32 masks, u32 slot) {
    // Diagnostic (opt-in): NXRT_FORCE_COLOR_MASK=<0..f> replaces the guest color write mask of every attachment (A/B only).
    static const std::optional<u32> force_color_mask = [] () -> std::optional<u32> {
        if (const char* v = std::getenv("NXRT_FORCE_COLOR_MASK"); v && *v) return static_cast<u32>(std::strtoul(v, nullptr, 16) & 0xf);
        return std::nullopt;
    }();
    return force_color_mask ? *force_color_mask : ((masks >> (4 * slot)) & 0xf);
}

struct HostImage {
    Image image;
    u64 last_use_frame = 0;  // present count of the last draw that sampled it (cache eviction)
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageViewType view_type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageAspectFlags view_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    u32 view_base_layer = 0, view_layer_count = 1;
    VkResult view_create_result = VK_SUCCESS;
    u32 width = 0, height = 0, layers = 1;
    bool cube = false;                      // render target created cube compatible (6 faces as layers)
    std::vector<VkImageView> layer_views;   // render target view of each layer, made on first use
    std::unordered_map<u32, VkImageView> rendered_r8_swizzle_views;
    VkImageView array_view = VK_NULL_HANDLE; // the layers sampled as one cube or 2D array, made on first use
    bool mutable_format = false;            // created with MUTABLE_FORMAT so UNORM/SRGB sample views are possible
    VkImageView srgb_view = VK_NULL_HANDLE;  // R8G8B8A8_SRGB sample view of a UNORM target, made on first use
    VkImageView unorm_view = VK_NULL_HANDLE; // R8G8B8A8_UNORM sample view of an SRGB target, made on first use
    VkImageViewType array_view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    VkImageAspectFlags array_view_aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    u32 array_view_base_layer = 0, array_view_layer_count = 0;
    VkResult array_view_create_result = VK_SUCCESS;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    u32 nvn_format = 0;
    bool rendered = false;
    RenderedSrgbContent srgb_content;
    bool block_texel_target = false;
    u64 block_write_generation = 0, block_synced_generation = 0;
    u64 depth_write_frame = 0;  // present_count_ + 1 of the last frame a draw wrote depth into this image
    bool recorded_use = false;
    u64 draws = 0;
};

// Pitch-linear texture the guest CPU may rewrite without any NVN call (the font glyph cache is R8 pitch-linear,
// plotted byte by byte into a CpuUncached pool). Compared with a shadow copy at most once per presented frame; only
// the band of rows that changed is uploaded again.
struct PitchSource {
    GuestAddr cpu = 0;
    u32 pitch = 0, row_bytes = 0, rows = 0, layers = 1, format = 0;
    std::vector<u8> shadow;  // last uploaded contents, tightly packed
    u64 checked_frame = 0;
    u64 uploads = 0;
    u64 uses = 0;  // samplings since the last NXRT_TRACE_GLYPH report
    DirtyStamp dirty_stamp;  // NXRT_TEXTURE_DIRTY_TRACKING: guest pages unwritten since `shadow` was verified
};

std::unique_ptr<SoftDirtyBackend> MakeDirtyBackend() {
    // On by default; NXRT_NO_TEXTURE_DIRTY_TRACKING=1 (or NXRT_TEXTURE_DIRTY_TRACKING=0) opts out.
    const char* off = std::getenv("NXRT_NO_TEXTURE_DIRTY_TRACKING");
    const char* v = std::getenv("NXRT_TEXTURE_DIRTY_TRACKING");
    if ((off && std::string_view(off) == "1") || (v && std::string_view(v) == "0")) return nullptr;
    auto backend = std::make_unique<SoftDirtyBackend>();
    if (!backend->Init()) {
        LOG_WARN("texture dirty tracking: kernel soft-dirty self-test failed: hashing as usual");
        return nullptr;
    }
    LOG_INFO("texture dirty tracking: soft-dirty backend verified");
    return backend;
}

std::unique_ptr<TextureDirtyTracker> MakeDirtyTracker(SoftDirtyBackend* backend) {
    if (!backend) return nullptr;
    u32 interval = 120;
    if (const char* v = std::getenv("NXRT_TEXTURE_DIRTY_CLEAR_FRAMES")) {
        const long n = std::strtol(v, nullptr, 10);
        if (n > 0 && n < 100000) interval = static_cast<u32>(n);
    }
    LOG_INFO("texture dirty tracking: clearing soft-dirty bits every %u frames", interval);
    return std::make_unique<TextureDirtyTracker>(backend, interval);
}

size_t SwizzledHash(const u8* data, size_t size) {
    return static_cast<size_t>(XxHash64(data, size));
}

// A block-linear texture uploaded from guest memory. The title can rewrite such a texture after it was first
// sampled (CPU-filled volumes and lookup tables); the raw swizzled bytes are kept to notice that.
struct SwizzledSource {
    GuestAddr cpu = 0;
    size_t layer_size = 0;  // swizzled bytes per array layer, or of the whole volume for a 3D texture
    u32 bytes_per_block = 0, width_blocks = 0, height_blocks = 0, layers = 1;
    u32 block_height = 0, block_depth = 0, tile_width = 0;
    bool is_3d = false;
    size_t bytes = 0;        // swizzled bytes of the whole texture
    size_t content_hash = 0; // of the swizzled contents at the last upload
    u64 checked_frame = 0;
    u64 uploads = 0;
    // An ASTC texture the host decodes in software: the refresh decodes it again (base level, one layer).
    bool astc = false, astc_srgb = false;
    u32 astc_block_width = 0, astc_block_height = 0, pixel_width = 0, pixel_height = 0;
    DirtyStamp dirty_stamp;  // NXRT_TEXTURE_DIRTY_TRACKING: guest pages unwritten since `content_hash` was verified
};

struct BoundState {
    std::array<u32, 2> blend_words[8]{};
    bool blend_set[8]{};
    u32 color_word = 0x08030000;
    u32 channel_mask = 0xffffffff;
    std::array<u32, 2> depth_stencil{0x22, 0x11181118};
    u32 polygon = 0x14;
    std::vector<u32> attribs;
    std::vector<u32> streams;
    GuestAddr program = 0;
    u64 program_serial = 0;
    std::array<std::pair<GpuAddr, u64>, 16> vertex_buffers{};
    std::array<std::array<std::pair<GpuAddr, u64>, 18>, 6> uniform_buffers{};
    std::array<std::array<nvn::UniformBufferBindOrigin, 18>, 6> uniform_buffer_bind_origin{};
    std::array<std::array<std::pair<GpuAddr, u64>, 16>, 6> storage_buffers{};
    nvn::TextureBindings textures{};
    std::array<float, 4> viewport{0, 0, 1, 1};
    std::array<u32, 4> scissor{0, 0, 0x7fffffff, 0x7fffffff};
    std::array<float, 2> depth_range{0, 1};
    std::array<float, 4> blend_color{};
    GuestAddr texture_pool = 0, sampler_pool = 0;
};

struct ShaderPools {
    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};

    [[nodiscard]] size_t ReservedStorageBytes() const noexcept {
        return inst.ReservedStorageBytes() + block.ReservedStorageBytes() + flow_block.ReservedStorageBytes();
    }

    void ReleaseContents() {
        // IR blocks own intrusive lists of instructions, so destroy them before
        // destroying the instruction nodes those lists reference.
        block.ReleaseContents();
        inst.ReleaseContents();
        flow_block.ReleaseContents();
    }
};

struct Pipeline {
    std::array<Shader::Info, kStages> shader_infos{};
    std::array<const Shader::Info*, kStages> infos{};
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool uses_push_descriptors = false;
    bool ok = false;
    std::string error;
};

VkShaderStageFlagBits StageBit(Shader::Stage stage) {
    switch (stage) {
    case Shader::Stage::VertexB: return VK_SHADER_STAGE_VERTEX_BIT;
    case Shader::Stage::TessellationControl: return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    case Shader::Stage::TessellationEval: return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    case Shader::Stage::Geometry: return VK_SHADER_STAGE_GEOMETRY_BIT;
    default: return VK_SHADER_STAGE_FRAGMENT_BIT;
    }
}

size_t StageIndex(Shader::Stage stage) {
    switch (stage) {
    case Shader::Stage::VertexB: return 0;
    case Shader::Stage::TessellationControl: return 1;
    case Shader::Stage::TessellationEval: return 2;
    case Shader::Stage::Geometry: return 3;
    default: return 4;
    }
}

Shader::AttributeType AttributeType(u32 type) {
    switch (type) {
    case 3: return Shader::AttributeType::SignedInt;
    case 4: return Shader::AttributeType::UnsignedInt;
    case 5: return Shader::AttributeType::UnsignedScaled;
    case 6: return Shader::AttributeType::SignedScaled;
    default: return Shader::AttributeType::Float;
    }
}

Shader::TextureType ShaderTextureType(const Tic& tic) {
    switch (tic.Type()) {
    case kTex1D: return Shader::TextureType::Color1D;
    case kTex2D:
    case kTex2DNoMip: return tic.NormalizedCoords() ? Shader::TextureType::Color2D : Shader::TextureType::Color2DRect;
    case kTex3D: return Shader::TextureType::Color3D;
    case kTexCube: return Shader::TextureType::ColorCube;
    case kTex1DArray: return Shader::TextureType::ColorArray1D;
    case kTex2DArray: return Shader::TextureType::ColorArray2D;
    case kTex1DBuffer: return Shader::TextureType::Buffer;
    case kTexCubeArray: return Shader::TextureType::ColorArrayCube;
    default: return Shader::TextureType::Color2D;
    }
}

class NvnRenderer;

// What the launch descriptor of a compute dispatch carries, taken from the program's NVN control section.
struct ComputeLaunch {
    std::array<u32, 3> workgroup{1, 1, 1};
    u32 shared_memory = 0, local_memory = 0;
};

struct ComputePipeline {
    Shader::Info info{};
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool ok = false;
    std::string error;
    // Hash of the instruction bytes the translator read plus the launch parameters (no addresses): identifies the shader
    // across sessions whatever its GPU address (used by the stored compute-run cache).
    u64 content_hash = 0;
};

// The game's shader as the Maxwell translator sees it: code and constants from
// guest memory, texture handles from the draw being translated.
class NvnEnvironment final : public Shader::Environment {
public:
    NvnEnvironment(NvnRenderer& r, GuestAddr code, u64 backing_span, u32 start, size_t nvn_stage,
                   bool trace_c1_specialization);
    bool TraceShaderFailure() const override { return trace_shader_failure_; }
    void TraceCbufReadFailure(u32 cbuf_index, u32 offset) override;
    // When set, every guest value the translation consumes is appended here (see shader_translation_cache.h).
    struct DepRecorder {
        std::vector<u32> instruction_offsets;
        std::vector<ShaderCbufDep> cbufs;
        std::vector<ShaderTextureDep> textures;
        std::set<u32> handle_offsets;  // handle-buffer words some texture query was derived from
        // The newest handle-buffer read whose low 20 bits are `handle` (identity derivation), or -1.
        std::int64_t SourceOffset(u32 handle) {
            for (auto it = cbufs.rbegin(); it != cbufs.rend(); ++it) {
                if (it->index == kTextureBoundBuffer && (it->value & 0xFFFFF) == handle) {
                    handle_offsets.insert(it->offset);
                    return it->offset;
                }
            }
            return -1;
        }
    };
    DepRecorder* recorder = nullptr;
    u64 ReadInstruction(u32 address) override {
        if (recorder) recorder->instruction_offsets.push_back(address);
        last_address = address;
        has_last_address = true;
        if (!ContainsBackingAccess(CpuBackingSpan{code_, backing_span_}, address, sizeof(u64))) {
            throw Shader::InvalidArgument("shader instruction read outside mapped backing at offset +0x{:x} "
                                          "(span=0x{:x}, access=0x{:x})",
                                          address, backing_span_, static_cast<u64>(sizeof(u64)));
        }
        return g_memory.Read<u64>(code_ + address);
    }
    u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;
    Shader::TextureType ReadTextureType(u32 raw_handle) override;
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32 raw_handle) override;
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return kTextureBoundBuffer; }
    u32 LocalMemorySize() const override {
        if (compute_) return compute_launch_.local_memory;
        return static_cast<u32>(sph.LocalMemorySize()) + sph.common3.shader_local_memory_crs_size;
    }
    u32 SharedMemorySize() const override { return compute_ ? compute_launch_.shared_memory : 0; }
    std::array<u32, 3> WorkgroupSize() const override {
        return compute_ ? compute_launch_.workgroup : std::array<u32, 3>{1, 1, 1};
    }
    // A compute shader has no program header: the code starts right after the NVN data header.
    void SetCompute(u32 start, const ComputeLaunch& launch) {
        compute_ = true;
        compute_launch_ = launch;
        sph = {};
        stage = Shader::Stage::Compute;
        start_address = start;
    }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

    u32 last_address = 0; // last instruction offset observed by the translator
    bool has_last_address = false;
    std::vector<CbufReadProvenance> cbuf_reads;
    std::vector<CbufReadProvenance> c1_specialization_reads;
    u32 cbuf_reads_omitted = 0;
    std::optional<CbufReadProvenance> failed_cbuf_read;
    bool has_failed_cbuf_request = false;
    u32 failed_cbuf_index = 0;
    u32 failed_cbuf_offset = 0;

private:
    bool compute_ = false;
    ComputeLaunch compute_launch_{};
    NvnRenderer& r_;
    GuestAddr code_;
    u64 backing_span_;
    size_t nvn_stage_;
    const bool trace_shader_failure_ = std::getenv("NXRT_TRACE_SHADER_FAILURE") != nullptr;
    const bool trace_c1_specialization_;
};

class NvnRenderer final : public nvn::Renderer {
public:
    ~NvnRenderer() override {
        if (ledger_) g_memory.SetAccessGuard(nullptr);
    }
    bool Init(SDL_Window* window);
    void Execute(const CommandList& list) override;
    void ExecuteSubmission(const nvn::SubmissionSnapshot& submission) override;
    void Present(GuestAddr texture, u64 frame) override;
    bool InvalidateMappedRange(GuestAddr pool, u64 offset, u64 size) override;

    // used by the shader environment
    u32 ReadCbuf(size_t nvn_stage, u32 cbuf, u32 offset, CbufReadProvenance* provenance = nullptr);
    std::optional<Tic> ReadTic(u32 id);

private:
    enum class FlushReason : size_t { ImageRecreate, Execute, Present, DumpImage, DumpTarget, Compute, StreamFull, MappedReadback, Count };
    // Submits the recorded work early when the upload stream is nearly full, so the next draw's uploads fit.
    bool ReserveStream();
    struct PoolGpuRange {
        GpuAddr start = 0;
        GuestAddr cpu = 0;
        u64 size = 0;
        u64 order = 0;
    };
    struct PendingComputeWriteback {
        u64 program = 0;
        u64 frame = 0;
        u64 dispatch = 0;
        u64 stream_offset = 0;
        GuestAddr cpu = 0;
        size_t size = 0;
    };

    void ExecuteList(const CommandList& list, int depth, u64 list_handle);
    std::optional<GuestAddr> GpuToCpu(GpuAddr va);
    std::optional<GuestAddr> GpuToCpuUncached(GpuAddr va);
    void RefreshGpuPoolLookup();
    std::optional<CpuBackingSpan> GpuToCpuBacking(GpuAddr va);
    struct TscAddressMemo { u64 pool = 0; u32 id = 0; u64 epoch = 0; u64 address = 0; };
    std::array<TscAddressMemo, 256> tsc_memo_{};
    std::array<TscAddressMemo, 1024> tic_memo_{};  // same idea for texture descriptors (address per pool/id/epoch)
    struct TexturesByPoolCache {
        const void* model = nullptr;
        u64 epoch = ~u64{0};
        std::unordered_map<GuestAddr, std::vector<GuestAddr>> by_pool;
    } textures_by_pool_;
    // Translated-shader cache (NXRT_NO_SHADER_CACHE=1 turns it off).
    ShaderTranslationCache shader_cache_;
    const bool shader_cache_enabled_ = std::getenv("NXRT_NO_SHADER_CACHE") == nullptr;
    // NXRT_NO_SHADER_CACHE_RELAX=1 requires texture-handle words to match exactly (one entry per handle value).
    const bool shader_cache_relax_handles_ = std::getenv("NXRT_NO_SHADER_CACHE_RELAX") == nullptr;
    // NXRT_SHADER_CACHE_VERIFY=1: every cache hit is translated again and the SPIR-V compared (diagnostic).
    const bool shader_cache_verify_ = std::getenv("NXRT_SHADER_CACHE_VERIFY") != nullptr;
    u64 shader_verify_checked_ = 0, shader_verify_mismatch_ = 0;
    u64 shader_cache_hits_ = 0, shader_cache_misses_ = 0, shader_cache_variant_hits_ = 0;
    u64 shader_dep_fail_[4]{};  // ranges, cbufs, textures, entry list full
    const bool shader_dep_debug_ = std::getenv("NXRT_TRACE_SHADER_CACHE") != nullptr;
    std::map<u64, std::pair<u64, std::set<u32>>> shader_dep_debug_hist_;
    ShaderEntry* LookupShaderEntry(GpuAddr code);
    bool ValidateShaderEntry(const ShaderEntry& entry, GpuAddr code);
    struct BackingRangeCache {
        const void* model = nullptr;
        u64 epoch = ~u64{0};
        bool complete = false;
        std::vector<GpuCpuRange> virtual_ranges, pool_ranges;
        std::vector<u64> unresolved_pools;  // pool addresses whose GPU base was still 0 at the last rebuild
        u64 rebuilds = 0;
    } backing_cache_;
    FormatInfo Format(u32 nvn_format);
    u64 ImageKey(GuestAddr texture);
    u64 ImageKeyUncached(GuestAddr texture);
    HostImage* ImageForUncached(GuestAddr texture, u32 level);
    bool SyncBlockTexelTargets(GuestAddr texture, const Tic& tic);
    HostImage* ImageFor(GuestAddr texture, u32 level = 0);
    VkImageView TargetView(HostImage& img, u32 layer);
    VkImageView SampleView(HostImage& img, u32 texture_id, const Tic* sampled_tic = nullptr);
    bool InitNull2D();
    VkCommandBuffer Cmd();
    bool Flush(FlushReason reason);
    void InitGpuTimestamps();
    void InitAsyncCompute();
    void MaterializeComputeWritebacks(FlushReason reason, u64 wait_ns);
    void Transition(HostImage& img, VkImageLayout layout);
    // A guest depth clear is held back until the first use of the image, so a following EQUAL-tested pass that relies
    // on the depth of an earlier pass of the same frame can keep it (see the ClearDepthStencil case).
    struct PendingDepthClear {
        HostImage* img = nullptr;
        float depth = 1.0f;
        u32 stencil = 0;
        u32 layer = 0;
    } pending_depth_clear_;
    bool applying_pending_depth_clear_ = false;
    void ApplyPendingDepthClear();
    void EndRenderPass();
    u64 Upload(const void* data, size_t size, size_t align);
    std::optional<Tsc> ReadTsc(u32 id);
    u32 StorageBase(size_t nvn_stage);
    bool Translate(GpuAddr code, size_t nvn_stage, Shader::IR::Program& out, Shader::Stage& stage,
                   std::unique_ptr<ShaderEntry>* record = nullptr);
    HostImage* SampledImage(u32 texture_id, u32 sample_handle, bool trace_win_sample,
                            u32 trace_input_index = ~u32{0}, std::optional<Tic>* resolved_tic = nullptr);
    HostImage* MirrorFloorD32Sample(HostImage& source, u64 draw_id);
    HostImage* SnapshotFeedbackTarget(HostImage& source);
    VkSampler SamplerFor(u32 sampler_id, std::optional<VkCompareOp> compare_override = std::nullopt,
                         bool shader_depth = false, bool shader_integer = false, bool image_depth = false);
    Pipeline* GetPipeline(u32 topology);
    VkRenderPass RenderPassFor(const std::vector<u32>& key);
    void Draw(const Command& c, u64 list_handle, int depth);
    void Dispatch(const Command& c);
    void TraceDispatch(const Command& c);
    ComputePipeline* GetComputePipeline();
    void ReportCounter(const Command& c);
    // NXRT_DRAW_CENSUS: every discarded draw goes through CensusDrop (no-op when the census is off).
    void CensusDrop(const std::string& reason);
    u64 CensusProgramHash() const;
    void CopyTexture(const Command& c);
    void CopyTextureToBuffer(const Command& c);
    void DestroyImage(HostImage& img);
    void DumpImage(HostImage& img, u64 frame, const char* suffix = nullptr);
    std::string dump_path_override_;
    void DumpWinSampleCheckpoint(HostImage& img, u64 frame, u64 draw_id, const char* tag = "draw");
    static void ReadPitch(const PitchSource& src, u8* dst);
    void RefreshPitch(u64 key, HostImage& img, PitchSource& src);
    void RefreshSwizzled(u64 key, HostImage& img, SwizzledSource& src);

    void ExecuteImpl(const CommandList& list, nvn::Model* metadata);
    nvn::Model& model_ = nvn::GetModel();
    nvn::Model* execution_model_ = nullptr;  // selected only under mutex_, while ExecuteImpl is active
    nvn::Model& RenderModel() { return execution_model_ ? *execution_model_ : model_; }
    const nvn::Model& RenderModel() const { return execution_model_ ? *execution_model_ : model_; }
    VulkanContext vk_;
    std::mutex mutex_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptors_ = VK_NULL_HANDLE;
    Buffer stream_;
    // NXRT_SSBO_VRAM=1: device-local mirror of the stream at the same offsets. Storage buffers are copied in by DMA,
    // run from VRAM, and written ranges are copied back to the stream for the write-back to guest memory.
    Buffer dstream_;
    const bool ssbo_vram_ = std::getenv("NXRT_SSBO_VRAM") != nullptr;
    // Opt-in batching keeps independent compute outputs in the upload stream until a
    // guest-memory dependency or command-list boundary requires a CPU-visible readback.
    std::vector<PendingComputeWriteback> pending_compute_writebacks_;
    // Descriptor staging is consumed synchronously by vkUpdateDescriptorSets,
    // so reuse its CPU-side allocations across draws.
    std::vector<VkWriteDescriptorSet> draw_descriptor_writes_;
    DescriptorInfoStorage<VkDescriptorBufferInfo> draw_buffer_infos_;
    DescriptorInfoStorage<VkDescriptorImageInfo> draw_image_infos_;
    ScratchBytes draw_descriptor_scratch_;
    std::vector<HostImage*> draw_colors_;
    std::vector<u64> draw_targets_, draw_fb_key_;
    std::vector<VkImageView> draw_views_;
    std::vector<u32> draw_rp_key_;
    // Keep readbacks alive when a submission may still reference them after a wait timeout.
    std::vector<Buffer> retained_readbacks_;
    // Images replaced while recording stay alive until that command buffer completes.
    std::vector<HostImage> retired_images_;
    // Host-visible transfer-destination buffers for GPU->CPU readbacks are pooled by power-of-two size instead of being
    // created and freed per call (vkAllocateMemory/vkFreeMemory are kernel calls). A buffer goes back to the pool only
    // after its readback was waited for. NXRT_NO_READBACK_POOL=1 restores create/destroy.
    std::vector<Buffer> readback_pool_;
    const bool readback_pool_enabled_ = std::getenv("NXRT_NO_READBACK_POOL") == nullptr;
    Buffer AcquireReadback(VkDeviceSize size);
    void ReleaseReadback(Buffer& buffer);
    // The game aliases several texture objects over the same pool storage with different shapes and flips between them
    // every frame; recreating the Vulkan image each time (vkDestroyImage/vkCreateImage and their framebuffers) cost
    // ~20% of the render thread on the island. A replaced image is parked here instead and revived when the same
    // shape comes back. NXRT_NO_IMAGE_SPARES=1 restores destroy-and-create.
    struct SpareImage {
        u64 key = 0;
        HostImage img;
    };
    std::deque<SpareImage> image_spares_;
    const bool image_spares_enabled_ = std::getenv("NXRT_NO_IMAGE_SPARES") == nullptr;
    static constexpr size_t kMaxImageSpares = 48;
    u64 image_spare_hits_ = 0, image_spare_parks_ = 0;
    // NXRT_ASYNC_COMPUTE_FLUSH=1 (opt-in): compute flushes submit without waiting; see render/async_writeback.h.
    struct AsyncSlot {
        VkCommandBuffer cb = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
    };
    std::vector<AsyncSlot> async_slots_; // slot 0 is cmd_/fence_
    AsyncSlotRing async_ring_{0};
    std::unique_ptr<AsyncWritebackLedger> ledger_;
    bool async_compute_ = false, async_verify_ = false;
    std::atomic<bool> async_wait_failed_{false};
    u64 sets_in_pool_ = 0; // descriptor sets allocated since the last pool reset
    u64 async_submits_ = 0, async_polled_ = 0, async_forwards_ = 0, async_end_violations_ = 0;
    std::array<u64, 4> async_fallbacks_{}; // ring, stream, sets, submit-failed
    u64 async_poll_tick_ = 0, async_last_report_ = 0;
    bool TryFlushAsyncCompute(std::vector<PendingComputeWriteback>& write_backs, bool supersede);
    // End of Execute / Present: submit the recorded scene without waiting for the GPU when nothing guest-visible is
    // pending (no compute write-back); otherwise (or when a limit is hit) the ordinary submit-and-wait.
    bool FlushEndOfWork(FlushReason reason);
    // Write-backs of consecutive async dispatches recorded but not yet submitted (NXRT_ASYNC_BATCH=<n>, off by
    // default: on the facepaint loop the GPU, not the submit count, is the limit, and batching measured no gain).
    std::vector<PendingComputeWriteback> async_batch_;
    size_t async_batch_dispatches_ = 0;
    const size_t async_batch_max_ = [] {
        const char* v = std::getenv("NXRT_ASYNC_BATCH");
        return v && *v ? static_cast<size_t>(std::strtoull(v, nullptr, 10)) : size_t{1};
    }();
    u64 async_batch_submits_ = 0;
    bool SubmitAsyncBatch();
    // NXRT_COMPUTE_CHAIN_TRACE=1 (diagnostic): fingerprints a long run of consecutive compute dispatches (the facepaint loop) -
    // its command sequence, the contents of every buffer the first time the run touches it, and the final contents of
    // every buffer it wrote - to learn whether the run is reproducible and could be replaced by a stored result.
    struct ChainTrace {
        u64 count = 0, chain_hash = 0, input_hash = 0, fp = 0, first_q = 0, loads0 = 0, stores0 = 0;
        u64 prefix_input_hash = 0, skip_index = 0, key = 0;
        bool mixed = false, skipping = false, no_save = false;
        std::map<GuestAddr, u64> covered;  // disjoint guest intervals the run has touched so far (start -> end)
        std::vector<ChainInput> inputs;    // each stretch in the state it was in when first touched
        std::map<GuestAddr, u64> written;  // buffers some dispatch wrote
        std::vector<u64> fps;              // fingerprint of every dispatch of the run so far
        std::map<GuestAddr, std::vector<u8>> snapshot;  // the written buffers as they were when the last dispatch's Execute ended
        u64 snapshot_count = 0;            // dispatches covered by `snapshot`
        std::unique_ptr<ChainEntry> entry; // stored run being matched while `skipping`
    } chain_;
    const bool chain_trace_ = std::getenv("NXRT_COMPUTE_CHAIN_TRACE") != nullptr;
    // On by default; NXRT_COMPUTE_CHAIN_CACHE=0 turns the stored-run mechanism off (see compute_chain_cache.h).
    const bool chain_cache_enabled_ = [] {
        const char* v = std::getenv("NXRT_COMPUTE_CHAIN_CACHE");
        return !(v && *v == '0');
    }();
    const bool chain_on_ = chain_trace_ || chain_cache_enabled_;
    // Dispatches executed before a stored run is recognised. Record and replay must use the same value (it is part of the key).
    const u64 kChainPrefix = [] {
        const char* v = std::getenv("NXRT_COMPUTE_CHAIN_PREFIX");
        const u64 n = v && *v ? std::strtoull(v, nullptr, 10) : 128;
        return std::clamp<u64>(n, 16, 100000);
    }();
    u64 chain_skipped_ = 0, chain_applied_ = 0, chain_fp_ns_ = 0, chain_prefix_ns_ = 0;
    std::chrono::steady_clock::time_point chain_started_{};
    u64 DispatchFingerprint(const Shader::Info& info, const Command& c, u64 shader_content, bool* ok);
    bool ChainOnDispatch(u64 fp);   // true: this dispatch is covered by a stored run and must not execute
    void ChainAbandonSkip(const char* why);
    void ChainApplyEntry();
    void ChainSnapshot();
    void ChainNoteSsbo(GuestAddr cpu, size_t used, bool written, bool mapped);
    void ChainFinish();
    bool AsyncBatchOverlaps(GuestAddr cpu, size_t size) const {
        for (const PendingComputeWriteback& w : async_batch_)
            if (GuestMemoryRangesOverlap({cpu, size}, {w.cpu, w.size})) return true;
        return false;
    }
    bool DrainAsync(FlushReason reason);
    void ResetAsyncSlots();
    void PollAsync();
    void ReportAsync(bool force);
    bool WaitAllInFlight(const VkFence* extra);
    size_t stream_offset_ = 0;
    u64 last_flushed_draws_ = 0;
    GuestBufferUploadCache frame_vertex_uploads_;
    // Draws that bind a uniform buffer whose bytes equal one already uploaded since the last flush reuse that stream
    // range instead of copying it again. On by default; NXRT_NO_UBO_DEDUP=1 (or NXRT_UBO_DEDUP=0) turns it off.
    DirectMappedUploadCache frame_ubo_uploads_;
    const bool ubo_dedup_ = [] {
        const char* off = std::getenv("NXRT_NO_UBO_DEDUP");
        const char* v = std::getenv("NXRT_UBO_DEDUP");
        return !(off && std::strcmp(off, "1") == 0) && !(v && std::strcmp(v, "0") == 0);
    }();
    u64 win_ubo_hits_ = 0, win_ubo_misses_ = 0, win_ubo_hit_bytes_ = 0;
    // Zero-copy dedup hits (with the dedup): see the uniform-buffer loop in Draw(). On by default;
    // NXRT_NO_UBO_DIRECT=1 (or NXRT_UBO_DIRECT=0) turns it off.
    const bool ubo_direct_ = [] {
        const char* off = std::getenv("NXRT_NO_UBO_DIRECT");
        const char* v = std::getenv("NXRT_UBO_DIRECT");
        return !(off && std::strcmp(off, "1") == 0) && !(v && std::strcmp(v, "0") == 0);
    }();
    // Vertex data compared in place against the cached upload (copy only on a miss). NXRT_NO_VERTEX_DIRECT=1 opts out.
    const bool vertex_direct_ = std::getenv("NXRT_NO_VERTEX_DIRECT") == nullptr && ubo_direct_;
    bool recording_ = false, in_render_pass_ = false, broken_ = false;
    u64 gpu_wait_timeout_ns_ = kHardwareGpuWaitTimeoutNs;
    Shader::Profile profile_{};
    Shader::Profile compute_profile_{}; // profile_ plus the opt-in NXRT_NATIVE_SMALL_STORES overrides
    Shader::HostTranslateInfo host_info_{};
    std::unique_ptr<ShaderPools> shader_pools_;
    std::unordered_map<u32, FormatInfo> formats_;
    std::unordered_map<u64, u64> block_sample_versions_;
    u64 next_block_sample_version_ = 0;
    std::unordered_map<u64, HostImage> images_;
    std::unordered_map<u64, HostImage> sampled_;
    // Memoized address and image resolution, each entry valid for one nvn::Model::epoch (the recorder bumps it whenever
    // a pool, a virtual mapping, a texture object or a texture-pool entry changes). NXRT_NO_RESOLVE_MEMO=1 disables.
    const bool resolve_memo_ = std::getenv("NXRT_NO_RESOLVE_MEMO") == nullptr;
    struct ImageKeyMemo { GuestAddr texture = 0; u64 epoch = 0; u64 key = 0; };
    std::array<ImageKeyMemo, 512> image_key_memo_{};
    struct GpuCpuMemo { GpuAddr va = 0; u64 epoch = 0; GuestAddr cpu = 0; bool found = false; };
    std::array<GpuCpuMemo, 1024> gpu_cpu_memo_{};
    struct ImageForMemo {
        GuestAddr texture = 0; u32 level = 0; u64 epoch = 0; HostImage* img = nullptr;
        u32 width = 0, height = 0, layers = 0, nvn_format = 0;
    };
    std::array<ImageForMemo, 256> image_for_memo_{};
    struct SampledMemo {
        GuestAddr pool = 0; u32 id = 0; u64 epoch = 0; bool in_pool = false;
        GuestAddr texture = 0; u64 serial = 0; u64 host_key = 0; HostImage* image = nullptr;
    };
    std::array<SampledMemo, 2048> sampled_memo_{};
    struct SampledHitMemo { u64 key = 0; HostImage* image = nullptr; PitchSource* pitch = nullptr; SwizzledSource* swizzled = nullptr; };
    std::array<SampledHitMemo, 4096> sampled_hit_memo_{};
    ResolutionModelBoundary resolution_model_boundary_;
    void SelectResolutionModel(const nvn::Model& selected) {
        if (!resolution_model_boundary_.Changed(selected)) return;
        gpu_pool_lookup_generation_ = ~u64{0};
        gpu_pool_lookup_count_ = ~size_t{0};
        gpu_cpu_memo_.fill({});
        image_key_memo_.fill({});
        image_for_memo_.fill({});
        sampled_memo_.fill({});
        sampled_hit_memo_.fill({});
    }
    std::vector<u64> pipeline_key_;
    // The game re-sets the shaders of the same Program objects many times (every call gets a new serial). A pipeline is
    // keyed by what the program points to, not by the serial: NXRT_NO_PROGRAM_CONTENT_KEY=1 keys it by serial again.
    // On by default since Alpha 1.6.5: keyed by serial the cache never hits (23788 builds for 482 programs on the
    // island route, 955 frames over 50 ms); by content: 3224 builds, 244 such frames, ~30 FPS.
    const bool program_content_key_ = std::getenv("NXRT_NO_PROGRAM_CONTENT_KEY") == nullptr;
    std::unordered_map<u64, u64> program_identity_;  // program serial -> identity of its shader stages
    u64 ProgramIdentity(const nvn::Program& program);

    // Execute holds the NVN model lock, which every recorder call of the other guest threads also takes. Between two
    // commands it lets waiting threads in (NXRT_NO_MODEL_LOCK_YIELD=1 keeps the lock for the whole Execute).
    ExecuteLockScope<nvn::TimedMutex>* execute_locks_ = nullptr;
    std::chrono::steady_clock::time_point model_lock_taken_{};
    u32 yield_tick_ = 0;
    u64 model_lock_yields_ = 0;
    const bool model_lock_yield_ = std::getenv("NXRT_MODEL_LOCK_YIELD") != nullptr;  // opt-in: ABBA showed no gain
    void MaybeYieldModelLock();
    void EvictSampledTextures();
    u64 lock_wait_ns_seen_ = 0, lock_contended_seen_ = 0;
    // Feedback-loop copies of a render target, one per shape: alternating shapes used to destroy and recreate the single
    // snapshot image on every switch.
    std::deque<std::unique_ptr<HostImage>> feedback_snapshots_;
    static constexpr size_t kMaxFeedbackSnapshots = 8;
    HostImage floor_d32_mirror_probe_;
    HostImage null_2d_;
    VkSampler null_2d_sampler_ = VK_NULL_HANDLE;
    const bool resolve_invalid_textures_ = std::getenv("NXRT_LEGACY_INVALID_TEXTURE_SKIP") == nullptr;
    const bool feedback_snapshot_enabled_ = std::getenv("NXRT_NO_FEEDBACK_SNAPSHOT") == nullptr;
    std::unordered_map<u64, std::string> failed_textures_;
    std::unordered_map<u64, PitchSource> pitch_sources_;
    std::unordered_map<u64, SwizzledSource> swizzled_sources_;
    // NXRT_NO_SAMPLED_REFRESH=1 keeps the first upload of a block-linear guest texture for the whole run.
    const bool refresh_swizzled_ = std::getenv("NXRT_NO_SAMPLED_REFRESH") == nullptr;
    // NXRT_TEXTURE_DIRTY_TRACKING=1 skips the per-frame re-read of a guest texture when the kernel's soft-dirty
    // bits prove nothing wrote to its pages (see render/texture_dirty_tracker.h). Off by default.
    std::unique_ptr<SoftDirtyBackend> dirty_backend_ = MakeDirtyBackend();
    std::unique_ptr<TextureDirtyTracker> dirty_tracker_ = MakeDirtyTracker(dirty_backend_.get());
    // Per-window (60 frames) texture refresh census for the NXRT_FRAME_PROFILE log.
    u64 win_resolves_ = 0, win_swz_calls_ = 0, win_swz_bytes_ = 0, win_swz_big_calls_ = 0, win_swz_big_bytes_ = 0;
    // NXRT_TEXTURE_DIRTY_VERIFY=1 (with tracking): still hash/compare when the tracker says "unwritten" and count the
    // cases where the content had in fact changed (a missed write). Diagnostic only; costs what tracking saves.
    const bool dirty_verify_ = std::getenv("NXRT_TEXTURE_DIRTY_VERIFY") != nullptr;
    u64 dirty_verify_miss_ = 0, dirty_verify_claims_ = 0;
    u64 win_swz_hashed_ = 0, win_swz_hashed_bytes_ = 0, win_pitch_calls_ = 0, win_pitch_skipped_ = 0;
    const bool push_descriptors_enabled_ = [] {
        const char* value = std::getenv("NXRT_PUSH_DESCRIPTORS");
        return value && std::strcmp(value, "1") == 0;
    }();
    u64 present_count_ = 0;
    const bool reupload_ = std::getenv("NXRT_NO_TEXT_REUPLOAD") == nullptr;
    const HostImage* glyph_image_ = nullptr;  // NXRT_TRACE_GLYPH: the R8 pitch-linear glyph cache
    std::array<std::unordered_map<u64, VkSampler>, 2> samplers_;
    std::unordered_map<u64, std::unique_ptr<Pipeline>> pipelines_;
    std::map<std::pair<GpuAddr, size_t>, std::vector<CbufReadProvenance>> c1_specialization_reads_;
    std::unordered_map<u64, std::unique_ptr<ComputePipeline>> compute_pipelines_;
    // NXRT_NO_COMPUTE=1: dispatches are dropped (old behaviour, A/B).
    const bool compute_enabled_ = std::getenv("NXRT_NO_COMPUTE") == nullptr;
    // NXRT_NO_SSBO_DIRECT_READ=1 goes back to staging each storage buffer through a temporary before the upload.
    const bool ssbo_direct_read_ = std::getenv("NXRT_NO_SSBO_DIRECT_READ") == nullptr;
    // A storage buffer that overlaps results still in flight is assembled on the GPU (guest bytes, then the pending
    // results copied over them in age order) instead of waiting for the GPU and reading the guest back.
    // NXRT_SSBO_GPU_COMPOSE=1 enables it (no wall-time gain yet: the GPU itself is the limit on the facepaint loop).
    const bool ssbo_gpu_compose_ = std::getenv("NXRT_SSBO_GPU_COMPOSE") != nullptr;  // opt-in until it shortens the stall
    u64 ssbo_composed_ = 0;
    const bool batch_compute_writebacks_ = [] {
        const char* value = std::getenv("NXRT_BATCH_COMPUTE_WRITEBACKS");
        return value && std::strcmp(value, "1") == 0;
    }();
    const bool forward_compute_writebacks_ = [] {
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        const char* value = std::getenv("NXRT_FORWARD_COMPUTE_WRITEBACKS");
        return value && std::strcmp(value, "1") == 0 &&
               std::getenv("NXRT_TRACE_COMPUTE_READBACKS") != nullptr;
#else
        return false;
#endif
    }();
    // Diagnostic-only: requires the read observer and queued writebacks so any
    // actual guest-memory read from a pending output is logged. Default keeps
    // the conservative texture barrier.
    const bool skip_compute_texture_flush_ = [] {
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        const char* skip = std::getenv("NXRT_SKIP_COMPUTE_TEXTURE_FLUSH");
        const char* batch = std::getenv("NXRT_BATCH_COMPUTE_WRITEBACKS");
        const char* forward = std::getenv("NXRT_FORWARD_COMPUTE_WRITEBACKS");
        return skip && std::strcmp(skip, "1") == 0 && batch && std::strcmp(batch, "1") == 0 && forward &&
               std::strcmp(forward, "1") == 0 && std::getenv("NXRT_TRACE_COMPUTE_READBACKS") != nullptr;
#else
        return false;
#endif
    }();
    u64 compute_skipped_ = 0;
    u64 copies_ = 0, copies_done_ = 0;
    // NXRT_RENDER_CONDITIONAL=1: SetRenderEnable / SetRenderEnableConditional turn draws, clears and dispatches off.
    // Off by default: the counters compared are reports this renderer synthesizes. The outcome is counted either way.
    const bool conditional_render_ = std::getenv("NXRT_RENDER_CONDITIONAL") != nullptr;
    bool render_enabled_ = true;
    // The game wraps its render-target clears in SetRenderEnableConditional(report at X vs report at X+16, render if
    // not equal) so that a target is cleared only once per frame: the CPU stores 1 in the report slot, the first
    // conditional passes, a type-16 ReportCounter then overwrites the slot (0 on the hardware, so both reports are
    // equal) and the later "clear" of the same target (e.g. the snapshot that the tutorial highlight samples) is
    // skipped. The reports this renderer synthesizes for CPU readers are nonzero and growing, so the gate is
    // evaluated with those synthesized reports counted as 0. It gates clears only (draws are never gated by default).
    // NXRT_LEGACY_CONDITIONAL_CLEAR=1 restores unconditional clears.
    bool clear_gate_open_ = true;
    std::unordered_map<u64, u64> synthetic_reports_; // guest GPU address -> value written by ReportCounter (types != 0)
    const bool legacy_conditional_clear_ = std::getenv("NXRT_LEGACY_CONDITIONAL_CLEAR") != nullptr;
    std::array<u64, 2> conditional_stats_{};  // [0] would disable, [1] would enable
    u64 pipeline_cache_hits_ = 0, pipeline_cache_misses_ = 0;
    std::map<std::vector<u32>, VkRenderPass> render_passes_;
    std::map<std::vector<u64>, VkFramebuffer> framebuffers_;
    std::array<std::optional<u32>, 6> storage_base_{};
    std::array<GuestAddr, 8> color_targets_{};
    std::array<u32, 8> color_levels_{};  // mip level of each color target, from its NVNtextureView
    std::array<u32, 8> color_view_formats_{};
    u32 depth_level_ = 0;
    std::array<u32, 8> color_layers_{};  // array layer (cube face) of each color target, from its NVNtextureView
    u32 depth_layer_ = 0;
    // NXRT_NO_TARGET_LEVELS=1 ignores texture-view mip levels (every level renders to and samples level 0).
    const bool target_levels_ = std::getenv("NXRT_NO_TARGET_LEVELS") == nullptr;
    // NXRT_NO_NULL_TARGETS=1: a draw with a null color slot is skipped as "target" (old behaviour, A/B).
    const bool null_targets_ = std::getenv("NXRT_NO_NULL_TARGETS") == nullptr;
    const bool stream_flush_ = std::getenv("NXRT_NO_STREAM_FLUSH") == nullptr;
    u64 stream_flushes_ = 0;
    // NXRT_NO_TARGET_LAYERS=1: every layer of a cube/array target lands in one 2D image (old behaviour, A/B).
    const bool target_layers_ = std::getenv("NXRT_NO_TARGET_LAYERS") == nullptr;
    u32 num_color_targets_ = 0;
    GuestAddr depth_target_ = 0;
    std::vector<std::string> pending_win_sample_traces_;
    bool pending_win_sample_trace_capped_ = false;
    u64 selected_trace_target_key_ = 0;
    bool selected_trace_target_pending_ = false;
    bool selected_trace_signature_seen_ = false;
    // NXRT_PROBE_DRAW_CENSUS: per-frame draw sequence number of the census frame.
    u64 census_seq_ = 0;
    u64 census_seq_frame_ = 0;
    std::vector<HostImage*> census_sampled_; // diagnostic: images sampled by the current census draw
    VkQueryPool census_queries_ = VK_NULL_HANDLE;
    std::vector<u64> census_query_seqs_;
    std::vector<u64> active_targets_;
    BoundState bound_;
    // Experimental: cache only graphics commands emitted in this recording.
    const bool graphics_state_cache_enabled_ = [] {
        const char* v = std::getenv("NXRT_GRAPHICS_STATE_CACHE");
        return v && std::strcmp(v, "1") == 0;
    }();
    GraphicsStateCache graphics_state_cache_;
    std::array<u64, 4> graphics_state_skipped_{}; // pipeline, viewport, scissor, blend
    u64 draws_ = 0, skipped_ = 0;
    std::map<std::string, u64> skip_reasons_;
    std::vector<std::string> recent_dispatches_; // dispatches recorded since the last successful flush (diagnostic)
    const bool render_stats_ = std::getenv("NXRT_RENDER_STATS") != nullptr;
    // NXRT_FRAME_PROFILE=<anything but 0> turns the per-draw timers on. They cost ~10% of the renderer thread
    // (clock_gettime), so leave it off when measuring or playing.
    const bool frame_profile_ = [] {
        const char* v = std::getenv("NXRT_FRAME_PROFILE");
        return v && std::strcmp(v, "0") != 0;
    }();
    // Opt-in draw census (NXRT_DRAW_CENSUS=<file.csv>); null = off, no cost on the draw path.
    std::unique_ptr<DrawCensus> census_ = [] {
        const char* path = std::getenv("NXRT_DRAW_CENSUS");
        if (!path || !*path) return std::unique_ptr<DrawCensus>{};
        auto census = std::make_unique<DrawCensus>(path);
        if (!census->Ok()) {
            LOG_INFO("renderer: NXRT_DRAW_CENSUS cannot open %s", path);
            return std::unique_ptr<DrawCensus>{};
        }
        census->SetInputProvider([] {
            const auto in = hle::GetHostInput();
            return CensusInput{in.lx, in.ly, in.rx, in.ry, in.buttons};
        });
        LOG_INFO("renderer: NXRT_DRAW_CENSUS -> %s (+ .counters.csv, .frames.csv)", path);
        return census;
    }();
    const bool release_model_at_execute_end_ = [] {
        const char* value = std::getenv("NXRT_RELEASE_MODEL_AT_EXECUTE_END");
        return value && std::strcmp(value, "1") == 0;
    }();
    // Opt-in GPU timestamps (NXRT_GPU_TIMESTAMPS=1, requires NXRT_FRAME_PROFILE). Off = no Vulkan calls added.
    bool gpu_ts_ = false;
    VkQueryPool ts_pool_ = VK_NULL_HANDLE;
    u32 ts_valid_bits_ = 0;
    double ts_period_ns_ = 0.0;
    bool ts_recorded_ = false; // begin timestamp written into the currently recording command buffer
    GpuSiteStats<static_cast<size_t>(FlushReason::Count)> frame_gpu_sites_, profile_gpu_sites_;
    // Per-dispatch timestamps (same opt-in): queries 2+2k / 3+2k bracket the k-th dispatch of a command buffer.
    static constexpr u32 kMaxDispatchTs = 1024;
    struct DispatchTsRecord {
        u64 program;
        u64 groups[3];
    };
    struct DispatchTsStat {
        double gpu_ns = 0.0;
        u64 count = 0;
        u64 threads_groups = 0; // sum of x*y*z workgroups
        u64 groups[3] = {0, 0, 0}; // last grid seen
    };
    std::vector<DispatchTsRecord> dispatch_ts_records_;
    std::unordered_map<u64, DispatchTsStat> profile_dispatch_ts_;
    u64 profile_dispatch_ts_skipped_ = 0;
    const bool trace_render_memory_ = std::getenv("NXRT_TRACE_RENDER_MEMORY") != nullptr;
    const bool trace_image_recreate_ = std::getenv("NXRT_TRACE_IMAGE_RECREATE") != nullptr;
    std::chrono::steady_clock::time_point last_profile_present_{};
    u64 frame_flush_wait_ns_ = 0, frame_flush_count_ = 0;
    std::array<u64, static_cast<size_t>(FlushReason::Count)> frame_flush_reason_ns_{};
    std::array<u64, static_cast<size_t>(FlushReason::Count)> frame_flush_reason_count_{};
    std::array<u64, static_cast<size_t>(FlushReason::Count)> profile_flush_reason_count_{};
    std::array<u64, static_cast<size_t>(FlushReason::Count)> profile_flush_reason_ns_{};
    u64 frame_stream_list_bytes_ = 0, frame_descriptor_sets_ = 0, frame_push_descriptor_sets_ = 0;
    // NXRT_TRACE_COMPUTE_FLUSH[=<from q>] (opt-in, observation only): one line per flush with its content.
    const bool trace_cflush_ = std::getenv("NXRT_TRACE_COMPUTE_FLUSH") != nullptr;
    u64 trace_from_q_ = [] {
        const char* v = std::getenv("NXRT_TRACE_COMPUTE_FLUSH");
        return v && *v ? std::strtoull(v, nullptr, 0) : 0;
    }();
    std::chrono::steady_clock::time_point trace_execute_start_{}, trace_submit_{};
    u64 trace_flush_frame_ = ~u64{0}, trace_sets_ = 0, trace_sets_at_flush_ = 0, trace_lines_ = 0;
    u32 trace_flush_index_ = 0;
    u64 trace_slow_loads_ = 0, trace_slow_stores_ = 0;
    double trace_last_gpu_ms_ = -1.0;
    std::vector<std::pair<GuestAddr, size_t>> trace_flush_ranges_;
    u64 profile_stream_list_peak_bytes_ = 0, profile_descriptor_sets_peak_ = 0, profile_push_descriptor_sets_peak_ = 0;
    u64 frame_execute_ns_ = 0, frame_execute_count_ = 0, frame_draw_ns_ = 0, frame_draw_count_ = 0;
    u64 frame_draw_pipeline_ns_ = 0, frame_draw_descriptors_ns_ = 0;
    u64 frame_draw_buffers_ns_ = 0, frame_draw_commands_ns_ = 0;
    u64 frame_draw_texture_resolve_ns_ = 0, frame_vertex_read_ns_ = 0, frame_index_read_ns_ = 0;
    u64 frame_sampled_swizzled_hash_ns_ = 0, frame_sampled_swizzled_hash_bytes_ = 0, frame_sampled_swizzled_hash_calls_ = 0;
    u64 frame_sampled_pitch_refresh_ns_ = 0, frame_sampled_pitch_refresh_calls_ = 0;
    u64 frame_vertex_bytes_ = 0, frame_index_bytes_ = 0;
    u64 frame_vertex_range_uses_ = 0, frame_vertex_range_unique_ = 0, frame_vertex_reused_bytes_ = 0;
    u64 frame_upload_ns_ = 0, frame_upload_bytes_ = 0, frame_upload_calls_ = 0;
    u64 frame_gpu_to_cpu_ns_ = 0, frame_gpu_to_cpu_calls_ = 0, frame_map_check_ns_ = 0;
    u64 compute_dispatches_ = 0;
    u64 report_counter_ = 0;
    std::set<u64> reported_counter_types_;
    u64 frame_uniform_read_ns_ = 0, frame_storage_read_ns_ = 0;
    u64 profile_interval_ns_ = 0, profile_render_ns_ = 0, profile_present_ns_ = 0;
    u64 profile_execute_ns_ = 0, profile_draw_ns_ = 0, profile_flush_wait_ns_ = 0;
    u64 profile_draw_pipeline_ns_ = 0, profile_draw_descriptors_ns_ = 0;
    u64 profile_draw_buffers_ns_ = 0, profile_draw_commands_ns_ = 0;
    u64 profile_draw_texture_resolve_ns_ = 0, profile_vertex_read_ns_ = 0, profile_index_read_ns_ = 0;
    u64 profile_sampled_swizzled_hash_ns_ = 0, profile_sampled_swizzled_hash_bytes_ = 0;
    u64 profile_sampled_swizzled_hash_calls_ = 0, profile_sampled_pitch_refresh_ns_ = 0;
    u64 profile_sampled_pitch_refresh_calls_ = 0;
    u64 profile_vertex_bytes_ = 0, profile_index_bytes_ = 0;
    u64 profile_vertex_range_uses_ = 0, profile_vertex_range_unique_ = 0, profile_vertex_reused_bytes_ = 0;
    u64 profile_upload_ns_ = 0, profile_upload_bytes_ = 0, profile_upload_calls_ = 0;
    u64 profile_gpu_to_cpu_ns_ = 0, profile_gpu_to_cpu_calls_ = 0, profile_map_check_ns_ = 0;
    u64 profile_uniform_read_ns_ = 0, profile_storage_read_ns_ = 0;
    u64 profile_flush_count_ = 0, profile_execute_count_ = 0, profile_draw_count_ = 0, profile_samples_ = 0;
    u64 profile_model_tail_releases_ = 0, profile_model_tail_fallbacks_ = 0;
    std::map<std::string, u64> pass_stats_; // NXRT_RENDER_STATS: clears / render-target sets / draws by target
    std::vector<PoolGpuRange> gpu_pool_ranges_;
    std::vector<GpuAddr> gpu_pool_prefix_max_end_;
    u64 gpu_pool_lookup_generation_ = ~u64{0};
    size_t gpu_pool_lookup_count_ = std::numeric_limits<size_t>::max();

    std::string TargetLabel(GuestAddr texture, u32 level = 0) {
        char buf[112];
        HostImage* img = texture ? ImageFor(texture, level) : nullptr;
        std::snprintf(buf, sizeof buf, "%llx(%ux%u,f%d)%s%.0u", static_cast<unsigned long long>(ImageKey(texture)),
                      img ? img->width : 0, img ? img->height : 0, img ? static_cast<int>(img->format) : 0,
                      level ? "/mip" : "", level);
        return buf;
    }
    std::string texture_error_;
};

NvnEnvironment::NvnEnvironment(NvnRenderer& r, GuestAddr code, u64 backing_span, u32 start, size_t nvn_stage,
                               bool trace_c1_specialization)
    : r_(r), code_(code), backing_span_(backing_span), nvn_stage_(nvn_stage),
      trace_c1_specialization_(trace_c1_specialization) {
    g_memory.ReadBlock(code + start, &sph, sizeof(sph));
    start_address = start;
    is_proprietary_driver = true;
    switch (sph.common0.shader_type) {
    case 1: stage = Shader::Stage::VertexB; break;
    case 2: stage = Shader::Stage::TessellationControl; break;
    case 3: stage = Shader::Stage::TessellationEval; break;
    case 4: stage = Shader::Stage::Geometry; break;
    default: stage = Shader::Stage::Fragment; break;
    }
}

u32 NvnEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    if (recorder) {
        const u32 value = r_.ReadCbuf(nvn_stage_, cbuf_index, cbuf_offset);
        recorder->cbufs.push_back({cbuf_index, cbuf_offset, value});
        return value;
    }
    if (trace_c1_specialization_ && cbuf_index == 1) {
        CbufReadProvenance provenance;
        const u32 value = r_.ReadCbuf(nvn_stage_, cbuf_index, cbuf_offset, &provenance);
        if (c1_specialization_reads.size() < kMaxShaderFailureCbufReads) {
            c1_specialization_reads.push_back(provenance);
        }
        return value;
    }
    if (trace_shader_failure_) {
        CbufReadProvenance provenance;
        const u32 value = r_.ReadCbuf(nvn_stage_, cbuf_index, cbuf_offset, &provenance);
        if (cbuf_reads.size() < kMaxShaderFailureCbufReads) {
            cbuf_reads.push_back(provenance);
        } else {
            ++cbuf_reads_omitted;
        }
        return value;
    }
    return r_.ReadCbuf(nvn_stage_, cbuf_index, cbuf_offset);
}

void NvnEnvironment::TraceCbufReadFailure(u32 cbuf_index, u32 offset) {
    if (!trace_shader_failure_) return;
    has_failed_cbuf_request = true;
    failed_cbuf_index = cbuf_index;
    failed_cbuf_offset = offset;
    if (const auto* const read = FindCbufReadProvenance(cbuf_reads, cbuf_index, offset)) {
        failed_cbuf_read = *read;
    } else {
        failed_cbuf_read.reset();
    }
}

Shader::TextureType NvnEnvironment::ReadTextureType(u32 raw_handle) {
    const auto tic = r_.ReadTic(raw_handle & 0xFFFFF);
    const Shader::TextureType type = tic ? ShaderTextureType(*tic) : Shader::TextureType::Color2D;
    if (recorder) {
        recorder->textures.push_back({recorder->SourceOffset(raw_handle & 0xFFFFF), raw_handle & 0xFFFFF,
                                      static_cast<u32>(type) | 0x80000000u, false});
    }
    return type;
}

bool NvnEnvironment::IsTexturePixelFormatInteger(u32 raw_handle) {
    const auto tic = r_.ReadTic(raw_handle & 0xFFFFF);
    const bool integer = tic && TicFormat(*tic).integer;
    if (recorder) recorder->textures.push_back({recorder->SourceOffset(raw_handle & 0xFFFFF), raw_handle & 0xFFFFF, 0, integer});
    return integer;
}

// ------------------------------------------------------------------ setup

bool NvnRenderer::Init(SDL_Window* window) {
    if (!vk_.Init(window)) return false;
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    if (ComputeReadbackTrace::Enabled() || ProgramTrace::Instance().Enabled()) {
        auto& trace = ComputeReadbackTrace::Instance();
        g_memory.SetReadObserver(&ComputeReadbackTrace::OnRead, &trace);
        LOG_INFO("renderer: guest-memory readback trace enabled");
    }
#endif
    gpu_wait_timeout_ns_ = GpuWaitTimeoutNs(vk_.properties.deviceType);
    LOG_INFO("renderer: GPU fence wait timeout=%llu ms (Vulkan device type=%d)",
             static_cast<unsigned long long>(gpu_wait_timeout_ns_ / 1'000'000ULL),
             static_cast<int>(vk_.properties.deviceType));
    const VkDevice dev = vk_.device;
    const VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, nullptr,
                                      VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, vk_.queue_family};
    vkCreateCommandPool(dev, &pci, nullptr, &pool_);
    const VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_,
                                          VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    vkAllocateCommandBuffers(dev, &cai, &cmd_);
    const VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fci, nullptr, &fence_);
    InitGpuTimestamps();
    stream_ = vk_.CreateBuffer(kStreamSize,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                   VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               true,
                               // NXRT_STREAM_VRAM=1 (experiment): put the upload stream in host-visible VRAM (ReBAR).
                               std::getenv("NXRT_STREAM_VRAM") ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT : 0);
    if (ssbo_vram_) {
        dstream_ = vk_.CreateBuffer(kStreamSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                    false);
        LOG_INFO("renderer: storage buffers run from a %zu MiB VRAM mirror of the stream", kStreamSize >> 20);
    }
    const u32 pool_scale = AsyncGraphicsEnabled() ? 4 : 1;
    const std::array<VkDescriptorPoolSize, 6> sizes{{
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 16384 * pool_scale},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4096 * pool_scale},
        {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1024 * pool_scale},
        {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024 * pool_scale},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384 * pool_scale},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024 * pool_scale},
    }};
    const VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, nullptr, 0, 8192 * pool_scale,
                                         static_cast<u32>(sizes.size()), sizes.data()};
    vkCreateDescriptorPool(dev, &dpi, nullptr, &descriptors_);
    InitAsyncCompute();

    const auto& fc = vk_.float_controls;
    profile_.supported_spirv = 0x00010500;
    profile_.unified_descriptor_binding = true;
    profile_.support_int16 = vk_.features.shaderInt16;
    profile_.support_int64 = vk_.features.shaderInt64;
    profile_.support_float_controls = true;
    profile_.support_separate_denorm_behavior =
        fc.denormBehaviorIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
    profile_.support_separate_rounding_mode = fc.roundingModeIndependence == VK_SHADER_FLOAT_CONTROLS_INDEPENDENCE_ALL;
    profile_.support_fp16_denorm_preserve = fc.shaderDenormPreserveFloat16;
    profile_.support_fp32_denorm_preserve = fc.shaderDenormPreserveFloat32;
    profile_.support_fp16_denorm_flush = fc.shaderDenormFlushToZeroFloat16;
    profile_.support_fp32_denorm_flush = fc.shaderDenormFlushToZeroFloat32;
    profile_.support_fp16_signed_zero_nan_preserve = fc.shaderSignedZeroInfNanPreserveFloat16;
    profile_.support_fp32_signed_zero_nan_preserve = fc.shaderSignedZeroInfNanPreserveFloat32;
    profile_.support_fp64_signed_zero_nan_preserve = fc.shaderSignedZeroInfNanPreserveFloat64;
    profile_.support_vote = true;
    profile_.supported_subgroup_stages = 0;
    profile_.support_demote_to_helper_invocation = true;
    profile_.support_derivative_control = true;
    profile_.support_native_ndc = false;
    profile_.support_scaled_attributes = true;
    profile_.support_multi_viewport = vk_.features.multiViewport;
    profile_.warp_size_potentially_larger_than_guest = true;
    profile_.min_ssbo_alignment = vk_.properties.limits.minStorageBufferOffsetAlignment;
    profile_.max_user_clip_distances = vk_.properties.limits.maxClipDistances;
    // Opt-in (compute programs only): native 8/16-bit SSBO/UBO access instead of the compare-exchange loops for
    // sub-word stores. The recompiler only declares the narrow aliases of a binding when descriptor aliasing is on.
    compute_profile_ = profile_;
    if (const char* v = std::getenv("NXRT_NATIVE_SMALL_STORES"); v && std::strcmp(v, "1") == 0) {
        compute_profile_.support_int8 = vk_.int8_buffer_access;
        compute_profile_.support_uniform_and_storage_buffer_8bit = vk_.int8_buffer_access;
        compute_profile_.support_int16 = profile_.support_int16 && vk_.int16_buffer_access;
        compute_profile_.support_uniform_and_storage_buffer_16bit = vk_.int16_buffer_access;
        compute_profile_.support_descriptor_aliasing = vk_.int8_buffer_access || vk_.int16_buffer_access;
        LOG_INFO("renderer: NXRT_NATIVE_SMALL_STORES=1 int8=%d int16=%d aliasing=%d",
                 compute_profile_.support_int8 ? 1 : 0, compute_profile_.support_int16 ? 1 : 0,
                 compute_profile_.support_descriptor_aliasing ? 1 : 0);
    }

    const auto& lim = vk_.properties.limits;
    host_info_.min_ssbo_alignment = lim.minStorageBufferOffsetAlignment;
    host_info_.max_per_stage_descriptor_sampled_images = lim.maxPerStageDescriptorSampledImages;
    host_info_.max_per_stage_resources = lim.maxPerStageResources;
    host_info_.max_descriptor_set_samplers = lim.maxDescriptorSetSamplers;
    host_info_.max_descriptor_set_uniform_buffers = lim.maxDescriptorSetUniformBuffers;
    host_info_.max_descriptor_set_uniform_buffers_dynamic = lim.maxDescriptorSetUniformBuffersDynamic;
    host_info_.max_descriptor_set_storage_buffers = lim.maxDescriptorSetStorageBuffers;
    host_info_.max_descriptor_set_storage_buffers_dynamic = lim.maxDescriptorSetStorageBuffersDynamic;
    host_info_.max_descriptor_set_sampled_images = lim.maxDescriptorSetSampledImages;
    host_info_.max_descriptor_set_storage_images = lim.maxDescriptorSetStorageImages;
    host_info_.max_descriptor_set_input_attachements = lim.maxDescriptorSetInputAttachments;
    host_info_.support_float64 = vk_.features.shaderFloat64;
    host_info_.support_float16 = true;
    host_info_.support_int64 = vk_.features.shaderInt64;
    host_info_.support_snorm_render_buffer = true;
    host_info_.support_conditional_barrier = true;
    host_info_.ApplyDescriptorLimitPolicy();
    return InitNull2D();
}

// A typed transparent image represents an invalid guest Color2D descriptor.
// Valid guest textures keep their existing views and cache entries.
bool NvnRenderer::InitNull2D() {
    const VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr, 0, VK_IMAGE_TYPE_2D,
                                 VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                                 VK_IMAGE_TILING_OPTIMAL,
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                 VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
    HostImage& img = null_2d_;
    img.image = vk_.CreateImage(info);
    if (!img.image.image) return false;
    img.format = VK_FORMAT_R8G8B8A8_UNORM;
    img.width = img.height = 1;
    const VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, img.image.image,
                                     VK_IMAGE_VIEW_TYPE_2D, img.format, {},
                                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(vk_.device, &view, nullptr, &img.view) != VK_SUCCESS) {
        vk_.Destroy(img.image);
        img = {};
        return false;
    }
    VkSamplerCreateInfo sampler{};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (vkCreateSampler(vk_.device, &sampler, nullptr, &null_2d_sampler_) != VK_SUCCESS) {
        DestroyImage(img);
        return false;
    }
    Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearColorValue zero{};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(Cmd(), img.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    Transition(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return Flush(FlushReason::Execute);
}

// ------------------------------------------------------------------ memory and formats

void NvnRenderer::RefreshGpuPoolLookup() {
    if (gpu_pool_lookup_generation_ == RenderModel().pool_generation && gpu_pool_lookup_count_ == RenderModel().pools.size()) return;
    gpu_pool_ranges_.clear();
    u64 order = 0;
    for (auto& [addr, pool] : RenderModel().pools) {
        if (!pool.storage || !pool.size) continue;
        if (!pool.gpu && !execution_model_) {
            pool.gpu = g_memory.Read<u64>(addr + 0x18);
            pool.gpu2 = g_memory.Read<u64>(addr + 0x20);
        }
        if (pool.gpu) gpu_pool_ranges_.push_back({pool.gpu, pool.storage, pool.size, order++});
        if (pool.gpu2) gpu_pool_ranges_.push_back({pool.gpu2, pool.storage, pool.size, order++});
    }
    std::sort(gpu_pool_ranges_.begin(), gpu_pool_ranges_.end(), [](const PoolGpuRange& a, const PoolGpuRange& b) {
        return a.start == b.start ? a.order < b.order : a.start < b.start;
    });
    gpu_pool_prefix_max_end_.resize(gpu_pool_ranges_.size());
    GpuAddr max_end = 0;
    for (size_t i = 0; i < gpu_pool_ranges_.size(); ++i) {
        const auto& range = gpu_pool_ranges_[i];
        const GpuAddr end = range.size > std::numeric_limits<GpuAddr>::max() - range.start
                                ? std::numeric_limits<GpuAddr>::max()
                                : range.start + range.size;
        max_end = std::max(max_end, end);
        gpu_pool_prefix_max_end_[i] = max_end;
    }
    gpu_pool_lookup_generation_ = RenderModel().pool_generation;
    gpu_pool_lookup_count_ = RenderModel().pools.size();
}

std::optional<GuestAddr> NvnRenderer::GpuToCpu(GpuAddr va) {
    if (!resolve_memo_) return GpuToCpuUncached(va);
    const u64 epoch = RenderModel().epoch.load(std::memory_order_acquire);
    GpuCpuMemo& memo = gpu_cpu_memo_[(va >> 8 ^ va >> 20 ^ va) & (gpu_cpu_memo_.size() - 1)];
    if (memo.va == va && memo.epoch == epoch) {
        if (frame_profile_) ++frame_gpu_to_cpu_calls_;
        return memo.found ? std::optional<GuestAddr>{memo.cpu} : std::nullopt;
    }
    const std::optional<GuestAddr> result = GpuToCpuUncached(va);
    memo = {va, epoch, result.value_or(0), result.has_value()};
    return result;
}

std::optional<GuestAddr> NvnRenderer::GpuToCpuUncached(GpuAddr va) {
    const auto lookup_start = frame_profile_ ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
    const auto finish_lookup = [this, &lookup_start](std::optional<GuestAddr> result) {
        if (frame_profile_) {
            frame_gpu_to_cpu_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - lookup_start)
                                        .count();
            ++frame_gpu_to_cpu_calls_;
        }
        return result;
    };
    if (auto it = RenderModel().virtual_mappings.upper_bound(va); it != RenderModel().virtual_mappings.begin()) {
        --it;
        if (va < it->first + it->second.size) return finish_lookup(it->second.cpu + (va - it->first));
    }
    RefreshGpuPoolLookup();
    size_t i = static_cast<size_t>(std::upper_bound(
        gpu_pool_ranges_.begin(), gpu_pool_ranges_.end(), va,
        [](GpuAddr address, const PoolGpuRange& range) { return address < range.start; }) - gpu_pool_ranges_.begin());
    const PoolGpuRange* match = nullptr;
    u64 match_order = std::numeric_limits<u64>::max();
    while (i > 0) {
        --i;
        const PoolGpuRange& range = gpu_pool_ranges_[i];
        if (va >= range.start && va - range.start < range.size && range.order < match_order) {
            match = &range;
            match_order = range.order;
        }
        if (i == 0 || gpu_pool_prefix_max_end_[i - 1] <= va) break;
    }
    if (match) return finish_lookup(match->cpu + (va - match->start));
    return finish_lookup(std::nullopt);
}

std::optional<CpuBackingSpan> NvnRenderer::GpuToCpuBacking(GpuAddr va) {
    // The range tables only change with the model epoch (the recorder bumps it on every pool, mapping or texture
    // change). A pool whose GPU address is not known yet is re-read on the next call, so such a table is not kept.
    const u64 epoch = RenderModel().epoch.load(std::memory_order_acquire);
    bool stale = backing_cache_.model != &RenderModel() || backing_cache_.epoch != epoch || !resolve_memo_;
    // A pool without a GPU base yet used to make the whole table uncacheable, so every lookup rebuilt it (thousands per
    // frame while a scene loads). Only those pools are re-read now; the table is rebuilt once one of them is mapped.
    if (!stale && !backing_cache_.complete && !execution_model_) {
        for (const u64 addr : backing_cache_.unresolved_pools) {
            if (g_memory.Read<u64>(addr + 0x18) || g_memory.Read<u64>(addr + 0x20)) {
                stale = true;
                break;
            }
        }
    }
    if (stale) {
        ++backing_cache_.rebuilds;
        backing_cache_.unresolved_pools.clear();
        backing_cache_.virtual_ranges.clear();
        backing_cache_.pool_ranges.clear();
        backing_cache_.virtual_ranges.reserve(RenderModel().virtual_mappings.size());
        for (const auto& [gpu, mapping] : RenderModel().virtual_mappings) {
            backing_cache_.virtual_ranges.push_back({gpu, mapping.cpu, mapping.size});
        }
        backing_cache_.pool_ranges.reserve(RenderModel().pools.size() * 2);
        bool complete = true;
        for (auto& [addr, pool] : RenderModel().pools) {
            if (!pool.storage) continue;
            if (!pool.gpu && !execution_model_) {
                pool.gpu = g_memory.Read<u64>(addr + 0x18);
                pool.gpu2 = g_memory.Read<u64>(addr + 0x20);
            }
            if (!pool.gpu && !pool.gpu2) {
                complete = false;
                backing_cache_.unresolved_pools.push_back(addr);
            }
            for (const GpuAddr base : {pool.gpu, pool.gpu2}) {
                if (base) backing_cache_.pool_ranges.push_back({base, pool.storage, pool.size});
            }
        }
        backing_cache_.model = &RenderModel();
        backing_cache_.epoch = epoch;
        backing_cache_.complete = complete;
    }
    return ResolveGpuBacking(va, backing_cache_.virtual_ranges, backing_cache_.pool_ranges);
}

FormatInfo NvnRenderer::Format(u32 nvn_format) {
    if (auto it = formats_.find(nvn_format); it != formats_.end()) return it->second;
    FormatInfo info;
    if (const u64 base = nvn::DriverModuleBase()) {
        // The driver's per-format table: 0x30-byte entries (TIC word 0 at +0x24,
        // vertex attribute bits at +0x14, render target format at +0x18).
        const u64 table = g_memory.Read<u64>(base + EnvOffset("NXRT_NVN_FORMAT_TABLE", 0xb69208));
        if (table && g_memory.IsMapped(table + u64{nvn_format} * 0x30, 0x30)) {
            const u64 e = table + u64{nvn_format} * 0x30;
            info.bytes_per_block = g_memory.Read<u32>(e + 0x10) & 0xff;
            info.vertex_attribute = g_memory.Read<u32>(e + 0x14);
            info.render_target = g_memory.Read<u32>(e + 0x18);
            info.tic_word0 = g_memory.Read<u32>(e + 0x24);
            info.valid = true;
        }
    }
    if (std::getenv("NXRT_TRACE_FORMATS")) {
        Tic probe{};
        probe.w[0] = info.tic_word0;
        const TextureFormat tf = TicFormat(probe);
        LOG_INFO("format-table nvn=0x%x valid=%d rt=0x%x tic_w0=0x%08x vtx=0x%x bpb=%u -> vk=%d", nvn_format, info.valid,
                 info.render_target, info.tic_word0, info.vertex_attribute, info.bytes_per_block,
                 static_cast<int>(tf.format));
    }
    formats_.emplace(nvn_format, info);
    return info;
}

u64 NvnRenderer::ImageKey(GuestAddr texture) {
    if (!resolve_memo_) return ImageKeyUncached(texture);
    const u64 epoch = RenderModel().epoch.load(std::memory_order_acquire);
    ImageKeyMemo& memo = image_key_memo_[(texture >> 4 ^ texture >> 13) & (image_key_memo_.size() - 1)];
    if (memo.texture == texture && memo.epoch == epoch) return memo.key;
    const u64 key = ImageKeyUncached(texture);
    memo = {texture, epoch, key};
    return key;
}

u64 NvnRenderer::ImageKeyUncached(GuestAddr texture) {
    auto t = RenderModel().textures.find(texture);
    if (t == RenderModel().textures.end() || !t->second.desc.pool) return texture;
    const nvn::TextureDesc& desc = t->second.desc;
    const auto pool = RenderModel().pools.find(desc.pool);
    if (pool == RenderModel().pools.end()) return texture;

    if (pool->second.is_virtual) {
        std::vector<VirtualImageMapping> mappings;
        for (const auto& [_, mapping] : RenderModel().virtual_mappings) {
            if (mapping.virtual_pool != desc.pool) continue;
            mappings.push_back({mapping.virtual_pool, mapping.virtual_offset, mapping.size,
                                mapping.physical_pool, mapping.physical_generation, mapping.physical_offset});
        }
        if (const auto backing = ResolveVirtualImageBacking(desc.pool, desc.offset, mappings)) {
            return ImageBackingCacheKey(*backing);
        }
        // An unmapped virtual range has no stable physical identity to alias.
        return texture;
    }

    return ImageBackingCacheKey(DirectImageBacking(desc.pool, pool->second.generation, desc.offset));
}

Buffer NvnRenderer::AcquireReadback(VkDeviceSize size) {
    if (!readback_pool_enabled_) return vk_.CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    VkDeviceSize rounded = 4096;
    while (rounded < size) rounded <<= 1;
    for (size_t i = 0; i < readback_pool_.size(); ++i) {
        if (readback_pool_[i].size == rounded) {
            Buffer b = readback_pool_[i];
            readback_pool_[i] = readback_pool_.back();
            readback_pool_.pop_back();
            return b;
        }
    }
    return vk_.CreateBuffer(rounded, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
}

void NvnRenderer::ReleaseReadback(Buffer& buffer) {
    if (!buffer.buffer) {
        vk_.Destroy(buffer);
        return;
    }
    if (readback_pool_enabled_ && buffer.mapped && readback_pool_.size() < 16) {
        readback_pool_.push_back(buffer);
        buffer = {};
        return;
    }
    vk_.Destroy(buffer);
}

void NvnRenderer::DestroyImage(HostImage& img) {
    if (pending_depth_clear_.img == &img) pending_depth_clear_ = {};
    std::vector<VkImageView> extra = img.layer_views;
    for (const auto& [_, view] : img.rendered_r8_swizzle_views) extra.push_back(view);
    extra.push_back(img.array_view);
    extra.push_back(img.srgb_view);
    extra.push_back(img.unorm_view);
    for (VkImageView v : extra) {
        if (!v || v == img.view) continue;
        const u64 view_key = reinterpret_cast<u64>(v);
        for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
            if (!FramebufferCacheKeyUsesImageView(it->first, view_key)) {
                ++it;
                continue;
            }
            if (it->second) vkDestroyFramebuffer(vk_.device, it->second, nullptr);
            it = framebuffers_.erase(it);
        }
        vkDestroyImageView(vk_.device, v, nullptr);
    }
    if (img.view) {
        const u64 view_key = reinterpret_cast<u64>(img.view);
        for (auto it = framebuffers_.begin(); it != framebuffers_.end();) {
            if (!FramebufferCacheKeyUsesImageView(it->first, view_key)) {
                ++it;
                continue;
            }
            if (it->second) vkDestroyFramebuffer(vk_.device, it->second, nullptr);
            it = framebuffers_.erase(it);
        }
        vkDestroyImageView(vk_.device, img.view, nullptr);
    }
    vk_.Destroy(img.image);
    img = {};
}

HostImage* NvnRenderer::ImageFor(GuestAddr texture, u32 level) {
    if (!resolve_memo_) return ImageForUncached(texture, level);
    const u64 epoch = RenderModel().epoch.load(std::memory_order_acquire);
    ImageForMemo& memo = image_for_memo_[(texture >> 4 ^ texture >> 12 ^ level * 7) & (image_for_memo_.size() - 1)];
    if (memo.img && memo.texture == texture && memo.level == level && memo.epoch == epoch) {
        const HostImage& img = *memo.img;
        if (img.image.image && img.width == memo.width && img.height == memo.height &&
            img.nvn_format == memo.nvn_format && img.layers == memo.layers) {
            return memo.img;
        }
    }
    HostImage* result = ImageForUncached(texture, level);
    if (result) memo = {texture, level, epoch, result, result->width, result->height, result->layers, result->nvn_format};
    return result;
}

// BC3 generation uses an integer attachment: one 128-bit texel per compressed
// block. Materialize that output into its guest layout before a BC3 sampler
// uploads it. Ordinary textures and unchanged block targets never wait here.
bool NvnRenderer::SyncBlockTexelTargets(GuestAddr texture, const Tic& tic) {
    if (!tic.IsBlockLinear() || (tic.Type() != kTex2D && tic.Type() != kTex2DNoMip) ||
        tic.BlockHeight() > 5 || tic.TileWidthSpacing() > 5) return false;
    const auto format = TicFormat(tic);
    if (format.format != VK_FORMAT_BC3_UNORM_BLOCK && format.format != VK_FORMAT_BC3_SRGB_BLOCK) return false;
    const auto backing = GpuToCpuBacking(tic.Address());
    if (!backing) return false;
    struct Read { HostImage* image; size_t guest_offset, tiled_size, buffer_offset; u32 block_height; };
    std::vector<Read> reads;
    size_t guest_offset = 0, packed_size = 0;
    u32 block_height = tic.BlockHeight();
    const u64 image_key = ImageKey(texture);
    for (u32 level = 0; level <= std::min(15u, tic.MaxMipLevel()); ++level) {
        const u32 width = std::max(1u, tic.Width() >> level), height = std::max(1u, tic.Height() >> level);
        const u32 wb = (width + 3) / 4, hb = (height + 3) / 4;
        const size_t tiled_size = BlockLinearSize(16, wb, hb, block_height, tic.TileWidthSpacing());
        if (!ContainsBackingAccess(*backing, guest_offset, tiled_size) ||
            !g_memory.IsMapped(backing->cpu + guest_offset, tiled_size)) return false;
        if (auto it = images_.find(LevelKey(image_key, level)); it != images_.end()) {
            auto& image = it->second;
            if (image.block_texel_target && image.rendered && image.width == wb && image.height == hb &&
                image.block_write_generation != image.block_synced_generation) {
                reads.push_back({&image, guest_offset, tiled_size, packed_size, block_height});
                packed_size += size_t{wb} * hb * 16;
            }
        }
        guest_offset += tiled_size;
        const u32 next_hb = (std::max(1u, tic.Height() >> (level + 1)) + 3) / 4;
        while (block_height > 0 && next_hb <= (1u << (block_height - 1)) * 8) --block_height;
    }
    if (reads.empty()) return true;
    Buffer buffer = AcquireReadback(packed_size);
    if (!buffer.buffer || !buffer.mapped) { ReleaseReadback(buffer); return false; }
    EndRenderPass();
    for (const auto& read : reads) {
        auto& image = *read.image;
        const auto layout = image.layout;
        Transition(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkBufferImageCopy copy{read.buffer_offset, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     {0,0,0}, {image.width, image.height, 1}};
        vkCmdCopyImageToBuffer(Cmd(), image.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &copy);
        Transition(image, layout);
    }
    const VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        buffer.buffer, 0, packed_size};
    vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0,nullptr,1,&barrier,0,nullptr);
    // Descriptors preceding this texture may already refer to stream uploads.
    // Flush resets the arena; preserve those bytes until this draw is submitted.
    const size_t pending_size = stream_offset_;
    std::vector<u8> pending_stream(stream_.mapped, stream_.mapped + pending_size);
    if (!Flush(FlushReason::MappedReadback)) { retained_readbacks_.push_back(buffer); return false; }
    if (pending_size) std::memcpy(stream_.mapped, pending_stream.data(), pending_size);
    stream_offset_ = pending_size;
    for (const auto& read : reads) {
        auto& image = *read.image;
        std::vector<u8> tiled(read.tiled_size);
        g_memory.ReadBlock(backing->cpu + read.guest_offset, tiled.data(), tiled.size());
        const size_t linear_size = size_t{image.width} * image.height * 16;
        if (!StoreBlockLinear128(std::span<const u8>(buffer.mapped + read.buffer_offset, linear_size), tiled,
                                  image.width, image.height, read.block_height, tic.TileWidthSpacing())) {
            ReleaseReadback(buffer); return false;
        }
        g_memory.WriteBlock(backing->cpu + read.guest_offset, tiled.data(), tiled.size());
        image.block_synced_generation = image.block_write_generation;
    }
    // A new cache key also refreshes mip uploads and avoids per-frame dirty
    // guards hiding a second write in the same frame. Old GPU views stay alive.
    block_sample_versions_[image_key] = ++next_block_sample_version_;
    if (std::getenv("NXRT_TRACE_TARGET_FAILURES"))
        LOG_INFO("block-target-sync q=%llu texture=%llx cpu=%llx levels=%zu bytes=%zu",
                 (unsigned long long)(present_count_+1), (unsigned long long)texture,
                 (unsigned long long)backing->cpu, reads.size(), packed_size);
    ReleaseReadback(buffer);
    return true;
}

HostImage* NvnRenderer::ImageForUncached(GuestAddr texture, u32 level) {
    HitchScope hitch_scope(kHitchImageCreate, texture);
    auto t = RenderModel().textures.find(texture);
    if (t == RenderModel().textures.end()) return nullptr;
    const nvn::TextureDesc& d = t->second.desc;
    if (level >= std::max(1u, d.levels)) level = 0;
    // Each rendered mip level is a host image of its own, so a downsample chain can read level N-1 while it
    // renders level N without both sharing one image layout.
    u32 width = std::max(1u, d.width >> level), height = std::max(1u, d.height >> level);
    // Cube maps and 2D arrays keep their layers (faces) in one host image; each layer is rendered through its own
    // view. NVN texture targets: 4 = 2D array, 8 = cube map, 9 = cube map array.
    u32 layers = 1;
    if (target_layers_ && (d.target == 4 || d.target == 8 || d.target == 9)) {
        layers = d.target == 8 ? 6 : std::clamp<u32>(d.depth, 1, 256);
    }
    const bool cube = (d.target == 8 || d.target == 9) && layers % 6 == 0 && width == height;
    HostImage& img = images_[LevelKey(ImageKey(texture), level)];
    const FormatInfo fi = Format(d.format);
    if (!fi.valid) return nullptr;
    Tic tic{};
    tic.w[0] = fi.tic_word0;
    // The driver's per-format table keeps the sRGB flag only in the Maxwell render-target code (0xd6 =
    // A8B8G8R8_SRGB, 0xd0 = A8R8G8B8_SRGB); TIC word 0 is identical to the linear format. A descriptor built from word
    // 0 alone always came out UNORM, so sRGB render targets neither encoded on write nor decoded on sample.
    // OPT-IN (NXRT_SRGB_TARGETS=1): it follows the hardware semantics but changes every RGBA8_SRGB target of the
    // game and was only validated in the q1800 room; it did not change the Mii face tone.
    static const bool srgb_targets = std::getenv("NXRT_SRGB_TARGETS") != nullptr;
    if (srgb_targets && (fi.render_target == 0xd6 || fi.render_target == 0xd0)) {
        tic.w[4] |= 1u << 22;
    }
    TextureFormat tf = TicFormat(tic);
    bool block_target = false;
    if (tf.block_width != 1) {
        u32 view_format = 0;
        for (u32 i = 0; i < num_color_targets_; ++i)
            if (color_targets_[i] == texture) view_format = color_view_formats_[i];
        Tic view_tic{}; view_tic.w[0] = Format(view_format).tic_word0;
        const auto view = TicFormat(view_tic);
        const auto plan = PlanBc3Target(width, height, layers, d.samples,
            tf.format == VK_FORMAT_BC3_UNORM_BLOCK || tf.format == VK_FORMAT_BC3_SRGB_BLOCK,
            view_format && view.format == VK_FORMAT_R32G32B32A32_UINT);
        if (!plan || d.target != 1 || d.stride || d.depth > 1) return nullptr;
        width = plan->width; height = plan->height;
        tf = view;
        block_target = true;
    }
    if (tf.format == VK_FORMAT_UNDEFINED) return nullptr;
    if (img.image.image && img.width == width && img.height == height && img.nvn_format == d.format &&
        img.layers == layers && img.format == tf.format && img.block_texel_target == block_target) return &img;
    if (img.image.image) {
        const auto& cen = GetDrawCensusConfig();
        const bool census_frame = cen.frame && present_count_ + 1 >= cen.frame && present_count_ + 1 <= cen.frame_end;
        if (trace_image_recreate_ || census_frame) {
            static u64 recreate_traces = 0;
            if (census_frame || recreate_traces++ < 96) {
                LOG_INFO("trace image-recreate #%llu texture=%llx serial=%llu key=%llx storage=%llx+%llx target=%x levels=%u samples=%u flags=%x cached=%ux%u/fmt=%x requested=%ux%u/fmt=%x recorded_use=%d rendered=%d layout=%u",
                         static_cast<unsigned long long>(recreate_traces),
                         static_cast<unsigned long long>(texture), static_cast<unsigned long long>(t->second.serial),
                         static_cast<unsigned long long>(ImageKey(texture)), static_cast<unsigned long long>(d.pool),
                         static_cast<unsigned long long>(d.offset), d.target, d.levels, d.samples, d.flags, img.width,
                         img.height, img.nvn_format, std::max(1u, d.width), std::max(1u, d.height), d.format,
                         img.recorded_use, img.rendered, static_cast<unsigned>(img.layout));
            }
        }
        if (in_render_pass_) EndRenderPass();
        if (image_spares_enabled_ && img.layers == 1 && !img.cube) {
            ++image_spare_parks_;
            image_spares_.push_back({ImageKey(texture), std::move(img)});
            while (image_spares_.size() > kMaxImageSpares) {
                HostImage victim = std::move(image_spares_.front().img);
                image_spares_.pop_front();
                if (recording_ || async_ring_.InFlight() > 0) retired_images_.push_back(std::move(victim));
                else DestroyImage(victim);
            }
        } else if (recording_ || async_ring_.InFlight() > 0) {
            retired_images_.push_back(std::move(img));
        } else {
            DestroyImage(img);
        }
        img = {};
    }
    if (image_spares_enabled_ && layers == 1 && !cube) {
        const u64 spare_key = ImageKey(texture);
        for (auto it = image_spares_.begin(); it != image_spares_.end(); ++it) {
            const HostImage& c = it->img;
            if (it->key != spare_key || c.width != width || c.height != height || c.nvn_format != d.format ||
                c.format != tf.format || c.block_texel_target != block_target || c.layers != 1 || c.cube) {
                continue;
            }
            img = std::move(it->img);
            image_spares_.erase(it);
            ++image_spare_hits_;
            // A revived image behaves like a new one: no content, undefined layout, nothing recorded or drawn yet.
            img.layout = VK_IMAGE_LAYOUT_UNDEFINED;
            img.rendered = false;
            img.recorded_use = false;
            img.srgb_content = {};
            img.block_write_generation = img.block_synced_generation = 0;
            img.depth_write_frame = 0;
            img.draws = 0;
            return &img;
        }
    }
    img.format = tf.format;
    img.block_texel_target = block_target;
    img.width = width;
    img.height = height;
    img.layers = layers;
    img.cube = cube;
    img.nvn_format = d.format;
    img.aspect = !tf.depth ? VK_IMAGE_ASPECT_COLOR_BIT
                 : tf.stencil ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                              : VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkFormatFeatureFlags attach = tf.depth ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                 : VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (vk_.FormatSupports(tf.format, attach)) {
        usage |= tf.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    // A render target the game later samples with the TIC sRGB bit set needs a view in the other 8-bit format: the
    // descriptor, not the render target format, decides whether the sampler decodes sRGB. Single-layer RGBA8 only.
    // NXRT_EXPERIMENT_FACELINE_SRGB=1: sample 1:2 (w:h) RGBA8 targets, the Mii skin/faceline textures, as sRGB.
    static const bool experiment_faceline_srgb = FacelineSrgbSampling();
    // The game renders RGBA8 (UNORM) targets and later samples them through descriptors whose sRGB bit is set
    // (view format reinterpretation): the Mii skin/faceline and face-part textures. The sampler must decode them,
    // so the sampling view follows the descriptor's sRGB bit. Default since the Mii face looked pale otherwise.
    // NXRT_LEGACY_IGNORE_TIC_SRGB=1 restores the old behaviour (always the target's own format) for A/B.
    static const bool honor_tic_srgb = TicSrgbAllTargets();
    const bool sample_srgb_views = (srgb_targets || experiment_faceline_srgb || honor_tic_srgb) && !tf.depth && layers == 1 && !cube &&
                                   (tf.format == VK_FORMAT_R8G8B8A8_UNORM || tf.format == VK_FORMAT_R8G8B8A8_SRGB);
    img.mutable_format = sample_srgb_views;
    const VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                nullptr,
                                (cube ? VkImageCreateFlags{VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT} : 0) |
                                    (sample_srgb_views ? VkImageCreateFlags{VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT} : 0),
                                VK_IMAGE_TYPE_2D,
                                img.format,
                                {img.width, img.height, 1},
                                1,
                                layers,
                                VK_SAMPLE_COUNT_1_BIT,
                                VK_IMAGE_TILING_OPTIMAL,
                                usage,
                                VK_SHARING_MODE_EXCLUSIVE,
                                0,
                                nullptr,
                                VK_IMAGE_LAYOUT_UNDEFINED};
    img.image = vk_.CreateImage(ici);
    if (!img.image.image) return nullptr;
    if (layers > 1 && render_stats_) {
        LOG_INFO("renderer: layered target %s: %u layers%s", TargetLabel(texture, level).c_str(), layers,
                 cube ? " (cube)" : "");
    }
    const VkImageAspectFlags view_aspect =
        (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT} : img.aspect;
    const VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, img.image.image,
                                    VK_IMAGE_VIEW_TYPE_2D, img.format, {}, {view_aspect, 0, 1, 0, 1}};
    img.view_type = vci.viewType;
    img.view_aspect = vci.subresourceRange.aspectMask;
    img.view_base_layer = vci.subresourceRange.baseArrayLayer;
    img.view_layer_count = vci.subresourceRange.layerCount;
    img.view_create_result = vkCreateImageView(vk_.device, &vci, nullptr, &img.view);
    return &img;
}

VkImageView NvnRenderer::TargetView(HostImage& img, u32 layer) {
    if (img.layers <= 1 || layer == 0 || layer >= img.layers) return img.view;
    img.layer_views.resize(img.layers, VK_NULL_HANDLE);
    VkImageView& v = img.layer_views[layer];
    if (!v) {
        const VkImageAspectFlags aspect =
            (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT} : img.aspect;
        const VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, img.image.image,
                                        VK_IMAGE_VIEW_TYPE_2D, img.format, {}, {aspect, 0, 1, layer, 1}};
        vkCreateImageView(vk_.device, &vci, nullptr, &v);
    }
    return v;
}

// The view a shader samples a rendered target through: its TIC type decides between the plain 2D view (layer 0)
// and the layers as one cube map or 2D array.
VkImageView NvnRenderer::SampleView(HostImage& img, u32 texture_id, const Tic* sampled_tic) {
    if (&img == &null_2d_) return img.view;
    std::optional<Tic> owned_tic;
    if (!sampled_tic) {
        owned_tic = ReadTic(texture_id);
        if (!owned_tic) return img.view;
        sampled_tic = &*owned_tic;
    }
    const Tic& tic = *sampled_tic;
    const u32 type = tic.Type();
    const bool as_cube = (type == kTexCube || type == kTexCubeArray) && img.cube;
    if (!as_cube && type != kTex2DArray) {
        // Render targets bypass the texture upload path that normally installs the guest TIC swizzle.
        // Recreate that view mapping when a rendered R8 image is sampled.
        if (img.rendered && img.format == VK_FORMAT_R8_UNORM) {
            const u32 key = (tic.Swizzle(0) & 7u) | ((tic.Swizzle(1) & 7u) << 3) |
                            ((tic.Swizzle(2) & 7u) << 6) | ((tic.Swizzle(3) & 7u) << 9);
            const VkComponentMapping components{Swizzle(tic.Swizzle(0)), Swizzle(tic.Swizzle(1)),
                                                Swizzle(tic.Swizzle(2)), Swizzle(tic.Swizzle(3))};
            const VkComponentMapping identity{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G,
                                              VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
            if (components.r != identity.r || components.g != identity.g || components.b != identity.b ||
                components.a != identity.a) {
                auto [it, inserted] = img.rendered_r8_swizzle_views.try_emplace(key, VK_NULL_HANDLE);
                if (inserted) {
                    const VkImageViewCreateInfo vci{
                        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, img.image.image,
                        VK_IMAGE_VIEW_TYPE_2D, img.format, components,
                        {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                    if (vkCreateImageView(vk_.device, &vci, nullptr, &it->second) != VK_SUCCESS)
                        img.rendered_r8_swizzle_views.erase(it);
                }
                if (const auto view = img.rendered_r8_swizzle_views.find(key);
                    view != img.rendered_r8_swizzle_views.end() && view->second) {
                    return view->second;
                }
            }
        }
        // Honour the descriptor's sRGB bit for 2D RGBA8 render targets (see ImageFor).
        if (img.mutable_format && img.aspect == VK_IMAGE_ASPECT_COLOR_BIT && img.layers == 1) {
            static const bool experiment_faceline_srgb = FacelineSrgbSampling();
            static const bool srgb_targets_view = std::getenv("NXRT_SRGB_TARGETS") != nullptr || TicSrgbAllTargets();
            const bool faceline_shape = img.height == 2 * img.width;
            static const bool legacy_ignore = std::getenv("NXRT_LEGACY_IGNORE_TIC_SRGB") != nullptr;
            const bool encoded_color = img.srgb_content.Decode(tic.Srgb(), legacy_ignore);
            const bool want_srgb = (experiment_faceline_srgb && faceline_shape) ? true :
                                   ((srgb_targets_view || encoded_color) ? tic.Srgb() : false);
            if (!srgb_targets_view && !encoded_color && !(experiment_faceline_srgb && faceline_shape)) return img.view;
            const VkFormat want = want_srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
            if (want != img.format) {
                VkImageView& slot = want_srgb ? img.srgb_view : img.unorm_view;
                if (!slot) {
                    const VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0,
                                                    img.image.image, VK_IMAGE_VIEW_TYPE_2D, want, {},
                                                    {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
                    if (vkCreateImageView(vk_.device, &vci, nullptr, &slot) != VK_SUCCESS) slot = VK_NULL_HANDLE;
                }
                if (std::getenv("NXRT_TRACE_SAMPLE_SRGB")) {
                    static u32 traced = 0;
                    if (traced++ < 64) {
                        LOG_INFO("sample-srgb texture=%u image=%ux%u target_format=%d tic_srgb=%d view=%p", texture_id,
                                 img.width, img.height, static_cast<int>(img.format), want_srgb ? 1 : 0,
                                 static_cast<void*>(slot));
                    }
                }
                if (slot) return slot;
            }
        }
        return img.view;
    }
    if (as_cube && img.layers < 6) return img.view;
    if (!img.array_view) {
        const VkImageAspectFlags aspect =
            (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT} : img.aspect;
        const VkImageViewType view_type = !as_cube              ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                          : img.layers == 6      ? VK_IMAGE_VIEW_TYPE_CUBE
                                                                 : VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        const VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, nullptr, 0, img.image.image,
                                        view_type, img.format, {}, {aspect, 0, 1, 0, img.layers}};
        img.array_view_type = vci.viewType;
        img.array_view_aspect = vci.subresourceRange.aspectMask;
        img.array_view_base_layer = vci.subresourceRange.baseArrayLayer;
        img.array_view_layer_count = vci.subresourceRange.layerCount;
        img.array_view_create_result = vkCreateImageView(vk_.device, &vci, nullptr, &img.array_view);
    }
    return img.array_view;
}

// ------------------------------------------------------------------ command buffer

void NvnRenderer::InitGpuTimestamps() {
    const char* want = std::getenv("NXRT_GPU_TIMESTAMPS");
    if (!want || std::strcmp(want, "1") != 0) return;
    LOG_INFO("renderer: GPU device '%s'", vk_.properties.deviceName);
    if (!frame_profile_) {
        LOG_INFO("renderer: NXRT_GPU_TIMESTAMPS ignored (needs NXRT_FRAME_PROFILE=1)");
        return;
    }
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk_.physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(vk_.physical, &count, families.data());
    const u32 bits = vk_.queue_family < count ? families[vk_.queue_family].timestampValidBits : 0;
    const double period = vk_.properties.limits.timestampPeriod;
    if (bits == 0 || period <= 0.0) {
        LOG_INFO("renderer: GPU timestamps unsupported (validBits=%u period=%.3f ns); disabled", bits, period);
        return;
    }
    const VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0, VK_QUERY_TYPE_TIMESTAMP, 2 + 2 * kMaxDispatchTs, 0};
    if (vkCreateQueryPool(vk_.device, &qci, nullptr, &ts_pool_) != VK_SUCCESS) {
        LOG_INFO("renderer: GPU timestamps: vkCreateQueryPool failed; disabled");
        return;
    }
    ts_valid_bits_ = bits;
    ts_period_ns_ = period;
    gpu_ts_ = true;
    LOG_INFO("renderer: GPU timestamps enabled (validBits=%u period=%.3f ns)", bits, period);
}

void NvnRenderer::InitAsyncCompute() {
    // On by default; NXRT_NO_ASYNC_COMPUTE_FLUSH=1 (or NXRT_ASYNC_COMPUTE_FLUSH=0) opts out.
    const char* off = std::getenv("NXRT_NO_ASYNC_COMPUTE_FLUSH");
    const char* flag = std::getenv("NXRT_ASYNC_COMPUTE_FLUSH");
    if (SafeMode()) return;
    if ((off && std::strcmp(off, "1") == 0) || (flag && std::strcmp(flag, "0") == 0)) return;
    const bool explicit_on = flag && std::strcmp(flag, "1") == 0;
    const char* verify = std::getenv("NXRT_ASYNC_COMPUTE_VERIFY");
    const char* conflict = nullptr;
    if (gpu_ts_) conflict = "NXRT_GPU_TIMESTAMPS";
    else if (batch_compute_writebacks_) conflict = "NXRT_BATCH_COMPUTE_WRITEBACKS";
    else if (std::getenv("NXRT_SYNC_DISPATCH")) conflict = "NXRT_SYNC_DISPATCH";
    else if (trace_cflush_) conflict = "NXRT_TRACE_COMPUTE_FLUSH";
    else if (kEnvProbeFloorD32Mirror) conflict = "NXRT_PROBE_FLOOR_D32_Y_MIRROR";
#ifdef _WIN32
    else conflict = "Windows (page-table diversion untested)";
#endif
    if (conflict) {
        if (explicit_on) LOG_WARN("renderer: NXRT_ASYNC_COMPUTE_FLUSH ignored: incompatible with %s", conflict);
        else LOG_INFO("renderer: async compute flush off: incompatible with %s", conflict);
        return;
    }
    const VkDevice dev = vk_.device;
    async_slots_.resize(kAsyncRingSlots);
    async_slots_[0] = {cmd_, fence_};
    for (size_t i = 1; i < kAsyncRingSlots; ++i) {
        const VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, nullptr, pool_,
                                              VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
        const VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        if (vkAllocateCommandBuffers(dev, &cai, &async_slots_[i].cb) != VK_SUCCESS ||
            vkCreateFence(dev, &fci, nullptr, &async_slots_[i].fence) != VK_SUCCESS) {
            LOG_ERROR("renderer: NXRT_ASYNC_COMPUTE_FLUSH disabled: could not create the command buffer ring");
            async_slots_.resize(1);
            async_slots_.clear();
            return;
        }
    }
    async_verify_ = verify && std::strcmp(verify, "1") == 0;
    async_ring_ = AsyncSlotRing(kAsyncRingSlots);
    AsyncWritebackHooks hooks;
    hooks.wait_slot = [this](u32 slot) {
        if (vkWaitForFences(vk_.device, 1, &async_slots_[slot].fence, VK_TRUE, gpu_wait_timeout_ns_) == VK_SUCCESS)
            return true;
        async_wait_failed_.store(true);
        return false;
    };
    hooks.slot_done = [this](u32 slot) { return vkGetFenceStatus(vk_.device, async_slots_[slot].fence) == VK_SUCCESS; };
    hooks.apply = [this](const AsyncWriteback& w) {
        if (!g_memory.IsMapped(w.range.address, w.range.size)) return false;
        std::memcpy(g_memory.RawPtr(w.range.address), stream_.mapped + w.stream_offset, w.range.size);
        return true;
    };
    hooks.hash_guest = [](GuestMemoryRange range) -> u64 {
        if (!g_memory.IsMapped(range.address, range.size)) return 0;
        return XxHash64(g_memory.RawPtr(range.address), range.size);
    };
    hooks.divert_page = [](u64 page) { return g_memory.DivertPage(page); };
    hooks.restore_page = [](u64 page) { g_memory.RestorePage(page); };
    hooks.in_renderer = [] { return t_renderer_depth > 0; };
    hooks.on_divergence = [](const AsyncWriteback& w, u64 now) {
        LOG_ERROR("renderer: async-compute VERIFY divergence: guest range 0x%llx/%llu (program 0x%llx q=%llu slot=%u) "
                  "changed between submit and materialization without interception (hash %016llx -> %016llx)",
                  static_cast<unsigned long long>(w.range.address), static_cast<unsigned long long>(w.range.size),
                  static_cast<unsigned long long>(w.program), static_cast<unsigned long long>(w.frame), w.slot,
                  static_cast<unsigned long long>(w.baseline_hash), static_cast<unsigned long long>(now));
    };
    const char* ledger_profile = std::getenv("NXRT_ASYNC_LEDGER_PROFILE");
    const bool profile_ledger = ledger_profile && std::strcmp(ledger_profile, "1") == 0;
    ledger_ = std::make_unique<AsyncWritebackLedger>(std::move(hooks), async_verify_, profile_ledger);
    g_memory.SetAccessGuard(ledger_.get());
    async_compute_ = true;
    LOG_INFO("renderer: async compute flush ON (ring=%zu slots, stream limit=%zu MiB, sets limit=%llu, verify=%d)",
             kAsyncRingSlots, kAsyncStreamLimit >> 20, static_cast<unsigned long long>(kAsyncDescriptorSetLimit),
             async_verify_ ? 1 : 0);
}

VkCommandBuffer NvnRenderer::Cmd() {
    if (broken_) throw std::runtime_error("renderer disabled after Vulkan submission failure");
    if (!recording_) {
        const VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, nullptr,
                                          VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, nullptr};
        const VkResult result = vkBeginCommandBuffer(cmd_, &bi);
        if (result != VK_SUCCESS) {
            broken_ = true;
            throw std::runtime_error("vkBeginCommandBuffer failed");
        }
        if (graphics_state_cache_enabled_) graphics_state_cache_.Reset();
        recording_ = true;
        if (async_ring_.InFlight() > 0) {
            // Earlier command buffers are still running: submission order alone gives no memory dependency.
            const VkMemoryBarrier all{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                      VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
            vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1,
                                 &all, 0, nullptr, 0, nullptr);
        }
        if (gpu_ts_) {
            vkCmdResetQueryPool(cmd_, ts_pool_, 0, 2 + 2 * kMaxDispatchTs);
            vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts_pool_, 0);
            ts_recorded_ = true;
        }
    }
    return cmd_;
}

void NvnRenderer::EndRenderPass() {
    if (!in_render_pass_) return;
    vkCmdEndRenderPass(cmd_);
    if (graphics_state_cache_enabled_) graphics_state_cache_.Reset();
    in_render_pass_ = false;
    active_targets_.clear();
}

bool NvnRenderer::Flush(FlushReason reason) {
    HitchScope hitch_scope(kHitchFlush);
    HitchReasonScope hitch_reason_scope(static_cast<size_t>(reason));
    if (broken_) return false;
    if (!async_batch_.empty() && !SubmitAsyncBatch()) return false;
    if (!recording_) return async_ring_.InFlight() > 0 ? DrainAsync(reason) : true;
    EndRenderPass();
    if (gpu_ts_ && ts_recorded_) vkCmdWriteTimestamp(cmd_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts_pool_, 1);
    const VkResult end_result = vkEndCommandBuffer(cmd_);
    if (end_result != VK_SUCCESS) {
        LOG_ERROR("renderer: vkEndCommandBuffer failed (result=%d, reason=%u); rendering disabled",
                  static_cast<int>(end_result), static_cast<unsigned>(reason));
        broken_ = true;
        recording_ = false;
        return false;
    }
    const VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd_, 0, nullptr};
    const auto wait_start = (frame_profile_ || trace_cflush_) ? std::chrono::steady_clock::now()
                                                              : std::chrono::steady_clock::time_point{};
    trace_submit_ = wait_start;
    const VkResult submit_result = vkQueueSubmit(vk_.queue, 1, &si, fence_);
    const VkResult wait_result = submit_result == VK_SUCCESS
                                     ? (async_ring_.InFlight() > 0
                                            ? (WaitAllInFlight(&fence_) ? VK_SUCCESS : VK_TIMEOUT)
                                            : vkWaitForFences(vk_.device, 1, &fence_, VK_TRUE, gpu_wait_timeout_ns_))
                                     : submit_result;
    const u64 wait_ns = (frame_profile_ || trace_cflush_)
                            ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - wait_start)
                                  .count()
                            : 0;
    if (frame_profile_) {
        frame_flush_wait_ns_ += wait_ns;
        ++frame_flush_count_;
        const size_t reason_index = static_cast<size_t>(reason);
        if (gpu_ts_ && ts_recorded_ && wait_result == VK_SUCCESS) {
            // The fence above already signalled, so the results are available without any extra wait.
            std::array<u64, 2> ticks{};
            if (vkGetQueryPoolResults(vk_.device, ts_pool_, 0, 2, sizeof(ticks), ticks.data(), sizeof(u64),
                                      VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                frame_gpu_sites_.Add(reason_index, GpuTicksToNs(ticks[0], ticks[1], ts_valid_bits_, ts_period_ns_),
                                     static_cast<double>(wait_ns));
                trace_last_gpu_ms_ = GpuTicksToNs(ticks[0], ticks[1], ts_valid_bits_, ts_period_ns_) / 1e6;
            }
            if (!dispatch_ts_records_.empty()) {
                std::vector<u64> dt(dispatch_ts_records_.size() * 2);
                if (vkGetQueryPoolResults(vk_.device, ts_pool_, 2, static_cast<u32>(dt.size()),
                                          dt.size() * sizeof(u64), dt.data(), sizeof(u64),
                                          VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
                    for (size_t k = 0; k < dispatch_ts_records_.size(); ++k) {
                        const DispatchTsRecord& r = dispatch_ts_records_[k];
                        DispatchTsStat& st = profile_dispatch_ts_[r.program];
                        st.gpu_ns += GpuTicksToNs(dt[2 * k], dt[2 * k + 1], ts_valid_bits_, ts_period_ns_);
                        ++st.count;
                        st.threads_groups += r.groups[0] * r.groups[1] * r.groups[2];
                        for (int a = 0; a < 3; ++a) st.groups[a] = r.groups[a];
                    }
                }
            }
        }
        frame_flush_reason_ns_[reason_index] += wait_ns;
        ++frame_flush_reason_count_[reason_index];
        if (reason == FlushReason::ImageRecreate || reason == FlushReason::Execute) {
            frame_stream_list_bytes_ += stream_offset_;
        }
        if (reason == FlushReason::Execute) {
            profile_stream_list_peak_bytes_ = std::max(profile_stream_list_peak_bytes_, frame_stream_list_bytes_);
            profile_descriptor_sets_peak_ = std::max(profile_descriptor_sets_peak_, frame_descriptor_sets_);
            profile_push_descriptor_sets_peak_ = std::max(profile_push_descriptor_sets_peak_, frame_push_descriptor_sets_);
            frame_stream_list_bytes_ = 0;
            frame_descriptor_sets_ = 0;
            frame_push_descriptor_sets_ = 0;
        }
    }
    if (!CanRecycleAfterFlush(submit_result, wait_result)) {
        LOG_ERROR("renderer: GPU submit/wait failed (submit=%d wait=%d, reason=%u); rendering disabled after %llu draws (%llu since last flush)",
                  static_cast<int>(submit_result), static_cast<int>(wait_result),
                  static_cast<unsigned>(reason),
                  static_cast<unsigned long long>(draws_),
                  static_cast<unsigned long long>(draws_ - last_flushed_draws_));
        for (const std::string& d : recent_dispatches_) LOG_ERROR("renderer: pending dispatch %s", d.c_str());
        broken_ = true;
        if (ledger_) ledger_->DropAll();
        // A timed-out wait can leave the command buffer, fence, descriptor pool, and stream in use.
        // Stop recording and keep those resources intact rather than recycling them while pending.
        recording_ = false;
        return false;
    }
    if (trace_cflush_) {
        const u64 q = present_count_ + 1;
        if (q != trace_flush_frame_) {
            trace_flush_frame_ = q;
            trace_flush_index_ = 0;
        }
        const double since_exec_ms =
            trace_execute_start_ == std::chrono::steady_clock::time_point{}
                ? -1.0
                : std::chrono::duration<double, std::milli>(trace_submit_ - trace_execute_start_).count();
        if (q >= trace_from_q_ && q < trace_from_q_ + 120 && trace_lines_++ < 2000) {
            size_t wb_bytes = 0;
            std::string ranges;
            for (const auto& [a, n] : trace_flush_ranges_) {
                wb_bytes += n;
                ranges += fmt::format("{}{:x}/{}", ranges.empty() ? "" : ",", static_cast<u64>(a), n);
            }
            LOG_INFO("renderer: cflush q=%llu idx=%u reason=%u draws=%llu sets=%llu stream_bytes=%zu "
                     "since_execute=%.3fms submit_wait=%.3fms gpu=%.3fms wb_buffers=%zu wb_bytes=%zu slow_loads=%llu "
                     "slow_stores=%llu ranges=%s",
                     static_cast<unsigned long long>(q), trace_flush_index_, static_cast<unsigned>(reason),
                     static_cast<unsigned long long>(draws_ - last_flushed_draws_),
                     static_cast<unsigned long long>(trace_sets_ - trace_sets_at_flush_), stream_offset_, since_exec_ms,
                     static_cast<double>(wait_ns) / 1e6, trace_last_gpu_ms_, trace_flush_ranges_.size(), wb_bytes,
                     static_cast<unsigned long long>(g_slow_path_loads.load() - trace_slow_loads_),
                     static_cast<unsigned long long>(g_slow_path_stores.load() - trace_slow_stores_), ranges.c_str());
        }
        ++trace_flush_index_;
        trace_slow_loads_ = g_slow_path_loads.load();
        trace_slow_stores_ = g_slow_path_stores.load();
        trace_sets_at_flush_ = trace_sets_;
        trace_flush_ranges_.clear();
        trace_last_gpu_ms_ = -1.0;
    }
    MaterializeComputeWritebacks(reason, wait_ns);
    if (ledger_) {
        // Every fence has been waited: copy what is still pending, in dispatch order, before anything is recycled.
        ledger_->MaterializeAll(reason == FlushReason::Execute ? AsyncMaterializeCause::ExecuteEnd
                                                               : AsyncMaterializeCause::Drain);
        ResetAsyncSlots();
    }
    if (trace_image_recreate_) {
        for (auto& [_, img] : images_) img.recorded_use = false;
    }
    vkResetFences(vk_.device, 1, &fence_);
    vkResetCommandBuffer(cmd_, 0);
    vkResetDescriptorPool(vk_.device, descriptors_, 0);
    sets_in_pool_ = 0;
    recent_dispatches_.clear();
    for (HostImage& img : retired_images_) DestroyImage(img);
    retired_images_.clear();
    recording_ = false;
    ts_recorded_ = false;
    dispatch_ts_records_.clear();
    stream_offset_ = 0;
    frame_vertex_uploads_.Invalidate();
    frame_ubo_uploads_.Invalidate();
    last_flushed_draws_ = draws_;
    return true;
}

// Waits for every submitted asynchronous command buffer (and `extra`, the one being submitted now).
bool NvnRenderer::WaitAllInFlight(const VkFence* extra) {
    HitchScope hitch_scope(kHitchWaitAll);
    std::array<VkFence, kMaxAsyncRingSlots + 1> fences{};
    u32 count = 0;
    for (size_t i = 0; i < async_ring_.InFlight(); ++i) fences[count++] = async_slots_[i].fence;
    if (extra) fences[count++] = *extra;
    if (!count) return true;
    return vkWaitForFences(vk_.device, count, fences.data(), VK_TRUE, gpu_wait_timeout_ns_) == VK_SUCCESS;
}

// Back to slot 0 with every fence and command buffer of the ring reset. The ledger must already be empty.
// The slot being recorded is the last one used; its fence is reset by the caller of the synchronous path.
void NvnRenderer::ResetAsyncSlots() {
    if (async_ring_.InFlight() == 0) return;
    for (size_t i = 0; i < async_ring_.InFlight(); ++i) {
        vkResetFences(vk_.device, 1, &async_slots_[i].fence);
        vkResetCommandBuffer(async_slots_[i].cb, 0);
    }
    // The synchronous caller resets the current slot (fence_/cmd_ at index InFlight()); afterwards slot 0 records.
    if (recording_) {
        // cmd_/fence_ still name the slot that was just submitted synchronously: reset it here, then switch.
        vkResetFences(vk_.device, 1, &fence_);
        vkResetCommandBuffer(cmd_, 0);
    }
    async_ring_.RecycleAll();
    cmd_ = async_slots_[0].cb;
    fence_ = async_slots_[0].fence;
}

// Flush with nothing recording but async command buffers in flight: wait, copy, recycle.
bool NvnRenderer::DrainAsync(FlushReason reason) {
    const auto wait_start = (frame_profile_ || trace_cflush_) ? std::chrono::steady_clock::now()
                                                              : std::chrono::steady_clock::time_point{};
    const bool ok = WaitAllInFlight(nullptr);
    const u64 wait_ns = (frame_profile_ || trace_cflush_)
                            ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                  std::chrono::steady_clock::now() - wait_start)
                                  .count()
                            : 0;
    if (frame_profile_) {
        frame_flush_wait_ns_ += wait_ns;
        ++frame_flush_count_;
        frame_flush_reason_ns_[static_cast<size_t>(reason)] += wait_ns;
        ++frame_flush_reason_count_[static_cast<size_t>(reason)];
        if (reason == FlushReason::ImageRecreate || reason == FlushReason::Execute) {
            frame_stream_list_bytes_ += stream_offset_;
        }
        if (reason == FlushReason::Execute) {
            profile_stream_list_peak_bytes_ = std::max(profile_stream_list_peak_bytes_, frame_stream_list_bytes_);
            profile_descriptor_sets_peak_ = std::max(profile_descriptor_sets_peak_, frame_descriptor_sets_);
            profile_push_descriptor_sets_peak_ = std::max(profile_push_descriptor_sets_peak_, frame_push_descriptor_sets_);
            frame_stream_list_bytes_ = 0;
            frame_descriptor_sets_ = 0;
            frame_push_descriptor_sets_ = 0;
        }
    }
    if (!ok) {
        LOG_ERROR("renderer: GPU wait failed while draining %zu asynchronous command buffers (reason=%u); rendering disabled",
                  async_ring_.InFlight(), static_cast<unsigned>(reason));
        broken_ = true;
        if (ledger_) ledger_->DropAll();
        return false;
    }
    ledger_->MaterializeAll(reason == FlushReason::Execute ? AsyncMaterializeCause::ExecuteEnd
                                                           : AsyncMaterializeCause::Drain);
    ResetAsyncSlots();
    vkResetDescriptorPool(vk_.device, descriptors_, 0);
    sets_in_pool_ = 0;
    recent_dispatches_.clear();
    for (HostImage& img : retired_images_) DestroyImage(img);
    retired_images_.clear();
    stream_offset_ = 0;
    frame_vertex_uploads_.Invalidate();
    frame_ubo_uploads_.Invalidate();
    last_flushed_draws_ = draws_;
    return true;
}

// Submit the recording command buffer without waiting. Returns false (nothing submitted) when a budget is exhausted;
// the caller then takes the synchronous path, which drains and resets everything.
bool NvnRenderer::FlushEndOfWork(FlushReason reason) {
    // Write-backs already in the ledger may stay pending: the guest's own loads and stores of those ranges are
    // intercepted and wait for the slot's fence first.
    if (AsyncGraphicsEnabled() && async_compute_ && recording_ && !broken_ && pending_compute_writebacks_.empty() &&
        async_batch_.empty()) {
        std::vector<PendingComputeWriteback> none;
        if (TryFlushAsyncCompute(none, false)) return true;
    }
    return Flush(reason);
}

u64 NvnRenderer::DispatchFingerprint(const Shader::Info& info, const Command& c, u64 shader_content, bool* ok) {
    // Everything the guest decides about one dispatch, without reading the data in the buffers: which program, the grid,
    // every constant buffer's bytes and the address, size and write flag of every storage buffer.
    const auto prog = RenderModel().programs.find(bound_.program);
    if (prog == RenderModel().programs.end()) {
        *ok = false;
        return 0;
    }
    *ok = true;
    const u64 id[4] = {shader_content, c.a[0], c.a[1], c.a[2]};
    u64 fp = FastHash(id, sizeof(id), 0x1234);
    static const bool trace_fp = std::getenv("NXRT_COMPUTE_CHAIN_TRACE_FP") != nullptr;
    const bool log_fp = trace_fp && !chain_.skipping && (chain_.count < 12 || chain_.count == 42 || chain_.count == 80);
    if (log_fp) {
        LOG_INFO("chain-fp: dispatch=%llu program=%llx identity=%llx grid=%llux%llux%llu",
                 static_cast<unsigned long long>(chain_.count), static_cast<unsigned long long>(bound_.program),
                 static_cast<unsigned long long>(id[0]), static_cast<unsigned long long>(c.a[0]),
                 static_cast<unsigned long long>(c.a[1]), static_cast<unsigned long long>(c.a[2]));
    }
    constexpr size_t s = kComputeNvnStage;
    std::vector<u8> bytes;
    for (const auto& desc : info.constant_buffer_descriptors) {
        for (u32 k = 0; k < desc.count; ++k) {
            const u32 slot = desc.index + k;
            bytes.assign(256, 0);
            if (slot == kTextureBoundBuffer) {
                bytes.assign(0x20 + 8 * 32, 0);
                std::memcpy(bytes.data() + 0x20, bound_.textures[s].data(), 8 * 32);
            } else if (slot < 18) {
                const auto [va, size] = bound_.uniform_buffers[s][slot];
                const auto cpu = va ? GpuToCpu(va) : std::nullopt;
                if (cpu && size) {
                    bytes.resize(std::min<u64>(size, 0x10000));
                    if (g_memory.IsMapped(*cpu, bytes.size())) g_memory.ReadBlock(*cpu, bytes.data(), bytes.size());
                }
            }
            fp = FastHash(bytes.data(), bytes.size(), fp + slot);
            if (log_fp) {
                LOG_INFO("chain-fp:   ubo slot=%u size=%zu hash=%016llx first=%08x", slot, bytes.size(),
                         static_cast<unsigned long long>(FastHash(bytes.data(), bytes.size(), 7)),
                         bytes.size() >= 4 ? *reinterpret_cast<const u32*>(bytes.data()) : 0u);
            }
        }
    }
    for (const auto& desc : info.storage_buffers_descriptors) {
        for (u32 k = 0; k < desc.count; ++k) {
            const u32 off = desc.cbuf_offset + k * 16;
            const GpuAddr va = Pack(ReadCbuf(s, desc.cbuf_index, off), ReadCbuf(s, desc.cbuf_index, off + 4));
            const u32 size = ReadCbuf(s, desc.cbuf_index, off + 8);
            const auto cpu = va ? GpuToCpu(va) : std::nullopt;
            const u64 meta[3] = {cpu ? u64{*cpu} : 0, std::min<size_t>(size, 8 << 20), desc.is_written ? u64{1} : u64{0}};
            fp = FastHash(meta, sizeof(meta), fp);
            if (log_fp) {
                LOG_INFO("chain-fp:   ssbo va=%llx cpu=%llx size=%zx written=%d", static_cast<unsigned long long>(va),
                         static_cast<unsigned long long>(meta[0]), static_cast<size_t>(meta[1]), desc.is_written ? 1 : 0);
            }
        }
    }
    return fp;
}

void NvnRenderer::ChainNoteSsbo(GuestAddr cpu, size_t used, bool written, bool mapped) {
    if (!mapped || !used) return;
    // The parts of [cpu, cpu+used) no earlier dispatch of the run has touched still hold what the guest put there:
    // that is the run's input. Parts already touched hold results of the run itself.
    const u64 lo = cpu, hi = cpu + used;
    std::vector<std::pair<u64, u64>> fresh;
    u64 at = lo;
    for (auto it = chain_.covered.begin(); it != chain_.covered.end() && at < hi; ++it) {
        if (it->second <= at) continue;
        if (it->first >= hi) break;
        if (it->first > at) fresh.emplace_back(at, it->first);
        at = std::max(at, it->second);
    }
    if (at < hi) fresh.emplace_back(at, hi);
    for (const auto& [from, to] : fresh) {
        std::vector<u8> bytes(to - from);
        // Diagnostic: NXRT_DIAG_SCRAMBLE_SCRATCH=<seed> fills stretches first touched by a written binding with pseudo-random
        // bytes, simulating the different leftovers real sessions have (tests the learning and that the result does not
        // depend on them).
        static const u64 scramble_seed = [] {
            const char* v = std::getenv("NXRT_DIAG_SCRAMBLE_SCRATCH");
            return v && *v ? std::strtoull(v, nullptr, 10) : u64{0};
        }();
        if (scramble_seed && written) {
            u64 x = scramble_seed * 0x9E3779B97F4A7C15ULL ^ from;
            for (u8& b : bytes) {
                x ^= x << 13; x ^= x >> 7; x ^= x << 17;
                b = static_cast<u8>(x);
            }
            g_memory.WriteBlock(from, bytes.data(), bytes.size());
        }
        g_memory.ReadBlock(from, bytes.data(), bytes.size());  // guarded: pending results are materialized first
        ChainInput in;
        in.address = from;
        in.size = to - from;
        in.hash = FastHash(bytes.data(), bytes.size(), from);
        in.first_dispatch = chain_.count;
        // A stretch first touched by a written binding holds leftovers: filling every such stretch with random bytes, run after run,
        // left the facepaint images and the save identical. Its content is not matched (the entry is found by the command
        // sequence and the read-only inputs). Read-only stretches always are.
        in.flags = written ? (kChainInputWritten | kChainInputVolatile) : 0;
        chain_.input_hash = FastHash(&in.hash, sizeof(in.hash), chain_.input_hash);
        chain_.inputs.push_back(in);
        // merge [from, to) into the covered map
        u64 start = from, end = to;
        auto it = chain_.covered.lower_bound(start);
        if (it != chain_.covered.begin() && std::prev(it)->second >= start) --it;
        while (it != chain_.covered.end() && it->first <= end) {
            start = std::min(start, it->first);
            end = std::max(end, it->second);
            it = chain_.covered.erase(it);
        }
        chain_.covered[start] = end;
    }
    if (written) chain_.written[cpu] = std::max<u64>(chain_.written[cpu], used);
}

void NvnRenderer::ChainSnapshot() {
    // The guest runs between Executes and may consume or reuse the results: what a later run must reproduce is the state at the
    // end of the Execute that held the last dispatch.
    for (const auto& [addr, size] : chain_.written) {
        std::vector<u8>& bytes = chain_.snapshot[addr];
        bytes.resize(size);
        g_memory.ReadBlock(addr, bytes.data(), size);
    }
    chain_.snapshot_count = chain_.count;
}

void NvnRenderer::ChainAbandonSkip(const char* why) {
    if (!chain_.skipping) return;
    LOG_ERROR("renderer: stored compute run abandoned (%s) after %llu of %llu dispatches; the cached entry is removed and "
              "this run's result may be incomplete",
              why, static_cast<unsigned long long>(chain_.skip_index),
              static_cast<unsigned long long>(chain_.entry ? chain_.entry->count : 0));
    if (chain_.entry) RemoveChainEntry(chain_.entry->key);
    chain_ = ChainTrace{};
    chain_.no_save = true;
}

void NvnRenderer::ChainApplyEntry() {
    u64 bytes = 0;
    for (const ChainRange& r : chain_.entry->ranges) {
        if (!g_memory.IsMapped(r.address, r.bytes.size())) continue;
        g_memory.WriteBlock(r.address, r.bytes.data(), r.bytes.size());
        bytes += r.bytes.size();
    }
    ++chain_applied_;
    LOG_INFO("chain-cache: timing total=%.0fms prefix=%.0fms fingerprints-while-skipping=%.0fms",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - chain_started_).count(),
             chain_prefix_ns_ / 1e6, chain_fp_ns_ / 1e6);
    LOG_INFO("chain-cache: applied stored run q=%llu key=%016llx dispatches=%llu (executed %llu, skipped %llu) bytes=%llu",
             static_cast<unsigned long long>(present_count_ + 1), static_cast<unsigned long long>(chain_.entry->key),
             static_cast<unsigned long long>(chain_.entry->count), static_cast<unsigned long long>(kChainPrefix),
             static_cast<unsigned long long>(chain_.entry->count - kChainPrefix), static_cast<unsigned long long>(bytes));
    chain_ = ChainTrace{};
}

bool NvnRenderer::ChainOnDispatch(u64 fp) {
    chain_.fp = fp;
    if (!chain_.skipping) return false;
    const ChainEntry& e = *chain_.entry;
    if (chain_.skip_index < e.count && e.fingerprints[chain_.skip_index] == fp) {
        ++chain_skipped_;
        if (++chain_.skip_index == e.count) ChainApplyEntry();
        return true;
    }
    ChainAbandonSkip("dispatch differs from the stored run");
    return false;
}

void NvnRenderer::ChainFinish() {
    if (chain_.count == 0 && !chain_.skipping) return;
    if (chain_.skipping) {
        ChainAbandonSkip("run ended early");
        return;
    }
    if (chain_.count >= kChainPrefix) {
        u64 out = 0, bytes = 0;
        ChainEntry entry;
        if (chain_.snapshot_count != chain_.count) ChainSnapshot();  // dispatches of the current Execute are not covered yet
        for (auto& [addr, data] : chain_.snapshot) {
            const u64 size = data.size();
            ChainRange r;
            r.address = addr;
            r.bytes = data;
            const u64 h = FastHash(r.bytes.data(), size, addr);
            out = FastHash(&h, sizeof(h), out);
            bytes += size;
            if (chain_cache_enabled_) entry.ranges.push_back(std::move(r));
            if (chain_trace_ && std::getenv("NXRT_COMPUTE_CHAIN_TRACE_RANGES")) {
                LOG_INFO("chain-range: %llx+%llx hash=%016llx", static_cast<unsigned long long>(addr),
                         static_cast<unsigned long long>(size), static_cast<unsigned long long>(h));
            }
        }
        if (chain_trace_) {
            const AsyncWritebackStats st = ledger_ ? ledger_->Stats() : AsyncWritebackStats{};
            const u64 loads = st.intercepted[static_cast<size_t>(AsyncMaterializeCause::GuestLoad)] - chain_.loads0;
            const u64 stores = st.intercepted[static_cast<size_t>(AsyncMaterializeCause::GuestStore)] - chain_.stores0;
            LOG_INFO("chain-trace: q=%llu dispatches=%llu chain=%016llx inputs=%016llx outputs=%016llx ranges=%zu written=%zu "
                     "written_bytes=%llu mixed=%d guest_loads=%llu guest_stores=%llu",
                     static_cast<unsigned long long>(chain_.first_q), static_cast<unsigned long long>(chain_.count),
                     static_cast<unsigned long long>(chain_.chain_hash), static_cast<unsigned long long>(chain_.input_hash),
                     static_cast<unsigned long long>(out), chain_.inputs.size(), chain_.written.size(),
                     static_cast<unsigned long long>(bytes), chain_.mixed ? 1 : 0, static_cast<unsigned long long>(loads),
                     static_cast<unsigned long long>(stores));
        }
        if (chain_cache_enabled_ && !chain_.no_save && chain_.key && chain_.fps.size() == chain_.count && !entry.ranges.empty()) {
            entry.key = chain_.key;
            entry.count = chain_.count;
            entry.fingerprints = chain_.fps;
            entry.inputs = chain_.inputs;
            // A stretch first touched by a written binding whose content differs from the previous recording of the same
            // command sequence holds leftovers, not input: remember that. Stretches first touched by a read-only
            // binding are always inputs.
            ChainEntry previous;
            u64 learned = 0;
            if (LoadChainEntry(entry.key, previous) && previous.count == entry.count && previous.fingerprints == entry.fingerprints) {
                for (ChainInput& in : entry.inputs) {
                    for (const ChainInput& old : previous.inputs) {
                        if (old.address != in.address || old.size != in.size) continue;
                        const bool was_volatile = (old.flags & kChainInputVolatile) != 0;
                        if (was_volatile || (old.hash != in.hash && (in.flags & kChainInputWritten) && (old.flags & kChainInputWritten))) {
                            if (!(in.flags & kChainInputVolatile)) ++learned;
                            in.flags |= kChainInputVolatile;
                        }
                        break;
                    }
                }
                if (learned) LOG_INFO("chain-cache: learned %llu scratch stretches whose content varies between runs",
                                      static_cast<unsigned long long>(learned));
            }
            if (SaveChainEntry(entry)) {
                LOG_INFO("chain-cache: stored run q=%llu key=%016llx dispatches=%llu ranges=%zu bytes=%llu",
                         static_cast<unsigned long long>(chain_.first_q), static_cast<unsigned long long>(entry.key),
                         static_cast<unsigned long long>(entry.count), entry.ranges.size(), static_cast<unsigned long long>(bytes));
            }
        }
    }
    chain_ = ChainTrace{};
}

bool NvnRenderer::SubmitAsyncBatch() {
    if (async_batch_.empty()) return true;
    std::vector<PendingComputeWriteback> batch;
    batch.swap(async_batch_);
    async_batch_dispatches_ = 0;
    ++async_batch_submits_;
    if (TryFlushAsyncCompute(batch, true)) return true;
    // The asynchronous path is unavailable (ring, stream or descriptor limits): submit, wait and copy in order.
    if (!Flush(FlushReason::Compute)) return false;
    for (const PendingComputeWriteback& w : batch) g_memory.WriteBlock(w.cpu, stream_.mapped + w.stream_offset, w.size);
    return true;
}

bool NvnRenderer::TryFlushAsyncCompute(std::vector<PendingComputeWriteback>& write_backs, bool supersede) {
    HitchScope hitch_scope(kHitchAsyncFlush);
    if (!async_compute_ || broken_ || !recording_ || async_wait_failed_.load()) return false;
    if (!async_ring_.CanSubmitAsync()) {
        ++async_fallbacks_[0];
        return false;
    }
    if (stream_offset_ > kAsyncStreamLimit) {
        ++async_fallbacks_[1];
        return false;
    }
    if (sets_in_pool_ > kAsyncDescriptorSetLimit) {
        ++async_fallbacks_[2];
        return false;
    }
    EndRenderPass();
    if (vkEndCommandBuffer(cmd_) != VK_SUCCESS) {
        LOG_ERROR("renderer: vkEndCommandBuffer failed in the asynchronous flush; rendering disabled");
        broken_ = true;
        recording_ = false;
        return true;
    }
    const VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &cmd_, 0, nullptr};
    const auto t0 = frame_profile_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    VkResult submit;
    {
        HitchScope submit_scope(kHitchSubmit);
        submit = vkQueueSubmit(vk_.queue, 1, &si, fence_);
    }
    if (frame_profile_) {
        const u64 ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
        frame_flush_wait_ns_ += ns;
        ++frame_flush_count_;
        frame_flush_reason_ns_[static_cast<size_t>(FlushReason::Compute)] += ns;
        ++frame_flush_reason_count_[static_cast<size_t>(FlushReason::Compute)];
    }
    if (submit != VK_SUCCESS) {
        LOG_ERROR("renderer: asynchronous vkQueueSubmit failed (%d); rendering disabled", static_cast<int>(submit));
        ++async_fallbacks_[3];
        broken_ = true;
        recording_ = false;
        return true;
    }
    ++async_submits_;
    std::vector<AsyncWriteback> entries;
    entries.reserve(write_backs.size());
    const u32 slot = static_cast<u32>(async_ring_.Current());
    for (const PendingComputeWriteback& w : write_backs) {
        entries.push_back({w.program, w.frame, w.dispatch, w.stream_offset, {w.cpu, w.size}, slot, 0});
    }
    if (!entries.empty()) {
        HitchScope add_scope(kHitchLedgerAdd);
        ledger_->Add(entries, supersede);
    }
    const size_t next = async_ring_.MarkSubmitted();
    cmd_ = async_slots_[next].cb;
    fence_ = async_slots_[next].fence;
    recording_ = false;
    recent_dispatches_.clear();
    frame_vertex_uploads_.Invalidate();
    frame_ubo_uploads_.Invalidate();
    last_flushed_draws_ = draws_;
    return true;
}

// Cheap, non-blocking: copy the pending results whose fence already signalled so that guest threads see fresh data.
void NvnRenderer::PollAsync() {
    HitchScope hitch_scope(kHitchPoll);
    if (!ledger_ || !ledger_->Pending()) return;
    if ((++async_poll_tick_ & 15) != 0) return;
    async_polled_ += ledger_->Poll();
}

void NvnRenderer::ReportAsync(bool force) {
    if (!ledger_) return;
    if (!force && present_count_ - async_last_report_ < 150) return;
    async_last_report_ = present_count_;
    const AsyncWritebackStats st = ledger_->Stats();
    if (shader_dep_debug_) {
        std::vector<std::pair<u64, u64>> top;
        for (const auto& [k, v] : shader_dep_debug_hist_) top.emplace_back(v.first, k);
        std::sort(top.rbegin(), top.rend());
        for (size_t i = 0; i < top.size() && i < 6; ++i) {
            const auto& v = shader_dep_debug_hist_[top[i].second];
            LOG_INFO("renderer: shader-cache cbuf-fail c%llu[0x%llx] fails=%llu distinct-values=%zu",
                     static_cast<unsigned long long>(top[i].second >> 32),
                     static_cast<unsigned long long>(top[i].second & 0xffffffff),
                     static_cast<unsigned long long>(top[i].first), v.second.size());
        }
    }
    if (shader_cache_enabled_) {
        LOG_INFO("renderer: shader-cache hits=%llu misses=%llu variant-hits=%llu shaders=%zu size=%zu KiB clears=%llu depfail{range=%llu cbuf=%llu tex=%llu} verify{checked=%llu mismatch=%llu}",
                 static_cast<unsigned long long>(shader_cache_hits_), static_cast<unsigned long long>(shader_cache_misses_),
                 static_cast<unsigned long long>(shader_cache_variant_hits_), shader_cache_.shaders(),
                 shader_cache_.bytes() >> 10, static_cast<unsigned long long>(shader_cache_.clears()),
                 static_cast<unsigned long long>(shader_dep_fail_[0]), static_cast<unsigned long long>(shader_dep_fail_[1]),
                 static_cast<unsigned long long>(shader_dep_fail_[2]), static_cast<unsigned long long>(shader_verify_checked_),
                 static_cast<unsigned long long>(shader_verify_mismatch_));
    }
    const auto at = [&](AsyncMaterializeCause c) {
        return static_cast<unsigned long long>(st.intercepted[static_cast<size_t>(c)]);
    };
    if (ledger_ && ledger_->BloomChecked()) {
        LOG_INFO("renderer: ledger-bloom verify checked=%llu violations=%llu",
                 static_cast<unsigned long long>(ledger_->BloomChecked()), static_cast<unsigned long long>(ledger_->BloomViolations()));
    }
    LOG_INFO("renderer: async-compute q=%llu submits=%llu forwards=%llu added=%llu materialized=%llu superseded=%llu dropped=%llu "
             "intercept{guest_load=%llu guest_store=%llu renderer=%llu poll=%llu execute_end=%llu drain=%llu} "
             "polled=%llu page_hits_no_overlap=%llu fallback{ring=%llu stream=%llu sets=%llu submit=%llu} "
             "execute_end_pending_violations=%llu wait_failures=%llu apply_skipped=%llu verify{checked=%llu divergences=%llu}",
             static_cast<unsigned long long>(present_count_), static_cast<unsigned long long>(async_submits_),
             static_cast<unsigned long long>(async_forwards_), static_cast<unsigned long long>(st.entries_added),
             static_cast<unsigned long long>(st.entries_materialized),
             static_cast<unsigned long long>(st.entries_superseded), static_cast<unsigned long long>(st.entries_dropped),
             at(AsyncMaterializeCause::GuestLoad), at(AsyncMaterializeCause::GuestStore),
             at(AsyncMaterializeCause::Renderer), at(AsyncMaterializeCause::Poll),
             at(AsyncMaterializeCause::ExecuteEnd), at(AsyncMaterializeCause::Drain),
             static_cast<unsigned long long>(async_polled_), static_cast<unsigned long long>(st.page_hits_without_overlap),
             static_cast<unsigned long long>(async_fallbacks_[0]), static_cast<unsigned long long>(async_fallbacks_[1]),
             static_cast<unsigned long long>(async_fallbacks_[2]), static_cast<unsigned long long>(async_fallbacks_[3]),
             static_cast<unsigned long long>(async_end_violations_), static_cast<unsigned long long>(st.wait_failures),
             static_cast<unsigned long long>(st.apply_skipped), static_cast<unsigned long long>(st.verify_checked),
             static_cast<unsigned long long>(st.verify_divergences));
    static const bool ledger_profile = [] {
        const char* e = std::getenv("NXRT_ASYNC_LEDGER_PROFILE");
        return e && std::strcmp(e, "1") == 0;
    }();
    if (ledger_profile) {
        LOG_INFO("renderer: async-ledger-profile q=%llu cause=non_overlap lock_calls=%llu lock_ns=%llu pending_page_calls=%llu",
                 static_cast<unsigned long long>(present_count_),
                 static_cast<unsigned long long>(st.non_overlap.lock_calls),
                 static_cast<unsigned long long>(st.non_overlap.lock_wait_ns),
                 static_cast<unsigned long long>(st.non_overlap.pending_page_calls));
        constexpr const char* names[] = {"guest_load", "guest_store", "renderer", "poll", "execute_end", "drain"};
        for (size_t i = 0; i < st.profile.size(); ++i) {
            const auto& p = st.profile[i];
            LOG_INFO("renderer: async-ledger-profile q=%llu cause=%s lock_calls=%llu lock_ns=%llu "
                     "wait_calls=%llu wait_ns=%llu apply_calls=%llu apply_ns=%llu applied_bytes=%llu",
                     static_cast<unsigned long long>(present_count_), names[i],
                     static_cast<unsigned long long>(p.lock_calls), static_cast<unsigned long long>(p.lock_wait_ns),
                     static_cast<unsigned long long>(p.wait_calls), static_cast<unsigned long long>(p.wait_ns),
                     static_cast<unsigned long long>(p.apply_calls), static_cast<unsigned long long>(p.apply_ns),
                     static_cast<unsigned long long>(p.applied_bytes));
        }
    }
}

void NvnRenderer::MaterializeComputeWritebacks(FlushReason reason, u64 wait_ns) {
    HitchScope hitch_scope(kHitchWriteback);
    if (pending_compute_writebacks_.empty()) return;

    size_t bytes = 0;
    u64 first_frame = pending_compute_writebacks_.front().frame;
    u64 last_frame = first_frame;
    u64 previous_dispatch = ~u64{0};
    size_t dispatches = 0;
    for (const PendingComputeWriteback& writeback : pending_compute_writebacks_) {
        bytes += writeback.size;
        last_frame = writeback.frame;
        if (writeback.dispatch != previous_dispatch) {
            ++dispatches;
            previous_dispatch = writeback.dispatch;
        }
        ProgramTraceBeforeWriteback(writeback.program, writeback.cpu, writeback.size);
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        ComputeReadbackTrace::Instance().PrepareWriteback(writeback.program, writeback.frame, writeback.cpu,
                                                          writeback.size);
#endif
        g_memory.WriteBlock(writeback.cpu, stream_.mapped + writeback.stream_offset, writeback.size);
        ProgramTraceAfterWriteback(writeback.program, writeback.dispatch, writeback.cpu,
                                   stream_.mapped + writeback.stream_offset, writeback.size);
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        if (forward_compute_writebacks_) {
            ComputeReadbackTrace::Instance().CompletePendingWriteback(writeback.program, writeback.frame,
                                                                        writeback.cpu, writeback.size);
        }
        ComputeReadbackTrace::Instance().RecordWriteback(writeback.program, writeback.frame, writeback.cpu,
                                                         writeback.size);
#endif
    }
    if (batch_compute_writebacks_ && frame_profile_) {
        LOG_INFO("renderer: compute-writeback-batch q=%llu..%llu dispatches=%zu buffers=%zu bytes=%zu reason=%u "
                 "flush_wait=%.3fms",
                 static_cast<unsigned long long>(first_frame), static_cast<unsigned long long>(last_frame), dispatches,
                 pending_compute_writebacks_.size(), bytes, static_cast<unsigned>(reason),
                 static_cast<double>(wait_ns) / 1'000'000.0);
    }
    pending_compute_writebacks_.clear();
}

void NvnRenderer::ApplyPendingDepthClear() {
    if (!pending_depth_clear_.img || applying_pending_depth_clear_) return;
    applying_pending_depth_clear_ = true;
    HostImage& img = *pending_depth_clear_.img;
    const PendingDepthClear clear = pending_depth_clear_;
    pending_depth_clear_ = {};
    Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkClearDepthStencilValue value{clear.depth, clear.stencil};
    const VkImageSubresourceRange range{img.aspect, 0, 1, clear.layer < img.layers ? clear.layer : 0, 1};
    vkCmdClearDepthStencilImage(Cmd(), img.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1, &range);
    img.rendered = true;
    applying_pending_depth_clear_ = false;
}

void NvnRenderer::Transition(HostImage& img, VkImageLayout layout) {
    if (pending_depth_clear_.img == &img) ApplyPendingDepthClear();
    if (trace_image_recreate_) img.recorded_use = true;
    if (img.layout == layout) return;
    EndRenderPass();
    const VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                 nullptr,
                                 VK_ACCESS_MEMORY_WRITE_BIT,
                                 VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                 img.layout,
                                 layout,
                                 VK_QUEUE_FAMILY_IGNORED,
                                 VK_QUEUE_FAMILY_IGNORED,
                                 img.image.image,
                                 {img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    img.layout = layout;
}

bool NvnRenderer::ReserveStream() {
    HitchScope hitch_scope(kHitchStream);
    constexpr size_t kStreamHeadroom = 16 << 20;
    if (!stream_flush_) return true;
    const bool async_sets_exhausted = async_compute_ && sets_in_pool_ > kAsyncDescriptorSetLimit;
    if (stream_offset_ + kStreamHeadroom <= kStreamSize && !async_sets_exhausted) return true;
    ++stream_flushes_;
    return Flush(FlushReason::StreamFull);
}

u64 NvnRenderer::Upload(const void* data, size_t size, size_t align) {
    const auto upload_start = frame_profile_ ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
    align = std::max<size_t>(align, 4);
    stream_offset_ = (stream_offset_ + align - 1) / align * align;
    if (stream_offset_ + size > kStreamSize) return ~u64{0};
    std::memcpy(stream_.mapped + stream_offset_, data, size);
    const u64 offset = stream_offset_;
    stream_offset_ += size;
    if (frame_profile_) {
        frame_upload_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now() - upload_start)
                                .count();
        frame_upload_bytes_ += size;
        ++frame_upload_calls_;
    }
    return offset;
}

// ------------------------------------------------------------------ descriptors from guest memory

std::optional<Tic> NvnRenderer::ReadTic(u32 id) {
    // Where entry `id` of the bound texture pool lives only changes with the model epoch; the descriptor bytes
    // themselves are read every time (the guest may rewrite them without any NVN call).
    const u64 epoch = RenderModel().epoch.load(std::memory_order_acquire);
    TscAddressMemo& memo = tic_memo_[(bound_.texture_pool >> 4 ^ id * 0x9E3779B1u) & (tic_memo_.size() - 1)];
    u64 address = 0;
    if (resolve_memo_ && memo.address && memo.pool == bound_.texture_pool && memo.id == id && memo.epoch == epoch) {
        address = memo.address;
    } else {
        const auto descriptor = ResolveNvnPoolDescriptor(bound_.texture_pool, id,
            [](u64 address, void* out, size_t size) {
                if (!g_memory.IsMapped(address, size)) return false;
                g_memory.ReadBlock(address, out, size);
                return true;
            });
        if (descriptor.status != DescriptorPoolStatus::Ready) return std::nullopt;
        address = descriptor.address;
        memo = {bound_.texture_pool, id, epoch, address};
    }
    if (!g_memory.IsMapped(address, sizeof(Tic))) return std::nullopt;
    Tic tic{};
    g_memory.ReadBlock(address, &tic, sizeof(tic));
    return tic;
}

std::optional<Tsc> NvnRenderer::ReadTsc(u32 id) {
    // Where entry `id` of the bound sampler pool lives only changes with the model epoch; the 32 bytes of the
    // descriptor itself are read every time (the guest may rewrite them without any NVN call).
    const u64 epoch = RenderModel().epoch.load(std::memory_order_acquire);
    TscAddressMemo& memo = tsc_memo_[(bound_.sampler_pool >> 4 ^ id * 0x9E3779B1u) & (tsc_memo_.size() - 1)];
    u64 address = 0;
    if (resolve_memo_ && memo.address && memo.pool == bound_.sampler_pool && memo.id == id && memo.epoch == epoch) {
        address = memo.address;
    } else {
        const auto descriptor = ResolveNvnPoolDescriptor(bound_.sampler_pool, id,
            [](u64 address, void* out, size_t size) {
                if (!g_memory.IsMapped(address, size)) return false;
                g_memory.ReadBlock(address, out, size);
                return true;
            });
        if (descriptor.status != DescriptorPoolStatus::Ready) return std::nullopt;
        address = descriptor.address;
        memo = {bound_.sampler_pool, id, epoch, address};
    }
    if (!g_memory.IsMapped(address, sizeof(Tsc))) return std::nullopt;
    Tsc tsc{};
    g_memory.ReadBlock(address, &tsc, sizeof(tsc));
    return tsc;
}

u32 NvnRenderer::StorageBase(size_t nvn_stage) {
    if (!storage_base_[nvn_stage]) {
        // nvnCommandBufferBindStorageBuffer: slot = table[stage] + index (driver data).
        const u64 base = nvn::DriverModuleBase();
        storage_base_[nvn_stage] =
            base ? g_memory.Read<u32>(base + EnvOffset("NXRT_NVN_SSBO_TABLE", 0x915550) + 4 * nvn_stage) : 0;
        // The driver's table has no entry for the compute stage (it reads 0), but a compute shader finds its storage
        // buffer descriptors in c0 at 0x310 (slot 49), the same slot the table gives the last graphics stage. With a
        // base of 0 every descriptor read as zero and each compute pass ran on empty buffers (Mii face paint).
        // NXRT_LEGACY_COMPUTE_SSBO_BASE=1 restores the old behaviour (A/B).
        static const bool legacy_compute_base = std::getenv("NXRT_LEGACY_COMPUTE_SSBO_BASE") != nullptr;
        if (nvn_stage == kComputeNvnStage && !*storage_base_[nvn_stage] && !legacy_compute_base) {
            storage_base_[nvn_stage] = 49;
        }
    }
    return *storage_base_[nvn_stage];
}

u32 NvnRenderer::ReadCbuf(size_t nvn_stage, u32 cbuf, u32 offset, CbufReadProvenance* provenance) {
    if (provenance) {
        *provenance = {};
        provenance->cbuf_index = cbuf;
        provenance->offset = offset;
    }
    const auto finish = [&](u32 value) {
        if (provenance) provenance->value = value;
        return value;
    };
    if (nvn_stage >= 6) return finish(0);
    if (cbuf == kStorageBoundBuffer) {
        // NVN storage buffer descriptors live in the driver's c0 as {address, size} in 16-byte slots starting at
        // StorageBase (0x110 for the vertex stage). The game never binds c0 itself (its uniform buffers start at
        // kUniformSlotBase), so these slots always come from nvnCommandBufferBindStorageBuffer. The nn::font text
        // shader reads its per-character data through c0[0x110].
        const u32 base = StorageBase(nvn_stage);
        const u32 slot = offset / 16;
        if (base && slot >= base && slot - base < 16) {
            const auto [va, size] = bound_.storage_buffers[nvn_stage][slot - base];
            switch (offset % 16) {
            case 0: return finish(static_cast<u32>(va));
            case 4: return finish(static_cast<u32>(va >> 32));
            case 8: return finish(static_cast<u32>(size));
            default: return finish(0);
            }
        }
    }
    if (cbuf == kTextureBoundBuffer) {
        // Texture handles at 0x20 + 8 * index; storage buffers as {address, size} in 16-byte slots.
        if (offset >= 0x20 && offset < 0x20 + 8 * 32) {
            const u64 handle = bound_.textures[nvn_stage][(offset - 0x20) / 8];
            return finish((offset & 4) ? static_cast<u32>(handle >> 32) : static_cast<u32>(handle));
        }
        const u32 base = StorageBase(nvn_stage);
        const u32 slot = offset / 16;
        if (slot >= base && slot - base < 16) {
            const auto [va, size] = bound_.storage_buffers[nvn_stage][slot - base];
            switch (offset % 16) {
            case 0: return finish(static_cast<u32>(va));
            case 4: return finish(static_cast<u32>(va >> 32));
            case 8: return finish(static_cast<u32>(size));
            default: return finish(0);
            }
        }
        return finish(0);
    }
    if (cbuf >= 18) return finish(0);
    const auto [va, size] = bound_.uniform_buffers[nvn_stage][cbuf];
    if (provenance) {
        provenance->has_uniform_binding = va != 0 || size != 0;
        provenance->bind_origin = bound_.uniform_buffer_bind_origin[nvn_stage][cbuf];
        provenance->bound_gpu_va = va;
        provenance->bound_size = size;
    }
    if (!va || offset + 4 > size) return finish(0);
    const GpuAddr read_va = va + offset;
    if (provenance) provenance->read_gpu_va = read_va;
    const auto cpu = GpuToCpu(read_va);
    if (!cpu) return finish(0);
    if (provenance) {
        provenance->cpu_mapping_found = true;
        provenance->cpu_address = *cpu;
    }
    const bool guest_mapped = g_memory.IsMapped(*cpu, 4);
    if (provenance) provenance->guest_memory_mapped = guest_mapped;
    return finish(guest_mapped ? g_memory.Read<u32>(*cpu) : 0);
}

// ------------------------------------------------------------------ shaders and pipelines

bool NvnRenderer::Translate(GpuAddr code, size_t nvn_stage, Shader::IR::Program& out, Shader::Stage& stage,
                            std::unique_ptr<ShaderEntry>* record) {
    const auto backing = GpuToCpuBacking(code);
    if (!backing) return false;
    const GuestAddr cpu = backing->cpu;
    // NVN shader data: a 0x30-byte header (magic 0x12345678), then the Maxwell
    // program header; instruction addresses are relative to the data itself.
    if (!ContainsBackingAccess(*backing, 0, sizeof(u32))) {
        LOG_ERROR("renderer: shader at 0x%llx has insufficient mapped backing for NVN magic "
                  "(backing_span=0x%llx)",
                  static_cast<unsigned long long>(code), static_cast<unsigned long long>(backing->size));
        return false;
    }
    const u32 start = g_memory.Read<u32>(cpu) == 0x12345678 ? 0x30 : 0;
    if (!ContainsBackingAccess(*backing, start, sizeof(Shader::ProgramHeader))) {
        LOG_ERROR("renderer: shader at 0x%llx has insufficient mapped backing for program header "
                  "at offset +0x%x (size=0x%zx, backing_span=0x%llx)",
                  static_cast<unsigned long long>(code), start, sizeof(Shader::ProgramHeader),
                  static_cast<unsigned long long>(backing->size));
        return false;
    }
    if (!shader_pools_) shader_pools_ = std::make_unique<ShaderPools>();
    const WinSampleTraceConfig& win_trace = GetWinSampleTraceConfig();
    const bool trace_c1_specialization = win_trace.enabled && win_trace.program && bound_.program == *win_trace.program;
    NvnEnvironment env{*this, cpu, backing->size, start, nvn_stage, trace_c1_specialization};
    NvnEnvironment::DepRecorder recorder;
    if (record && nvn_stage != nvn::kInvalidNvnStage) env.recorder = &recorder;
    try {
        stage = env.ShaderStage();
        if (nvn_stage == nvn::kInvalidNvnStage) return true;
        const u32 cfg_offset = start + static_cast<u32>(sizeof(Shader::ProgramHeader));
        Shader::Maxwell::Flow::CFG cfg(env, shader_pools_->flow_block, cfg_offset);
        out = Shader::Maxwell::TranslateProgram(shader_pools_->inst, shader_pools_->block, env, cfg, host_info_);
        if (env.recorder) {
            auto entry = std::make_unique<ShaderEntry>();
            entry->stage = stage;
            entry->nvn_stage = nvn_stage;
            const auto add_range = [&](u32 offset, u32 size) {
                ShaderBytesDep dep;
                dep.offset = offset;
                dep.bytes.resize(size);
                g_memory.ReadBlock(cpu + offset, dep.bytes.data(), size);
                entry->bytes += size + sizeof(ShaderBytesDep);
                entry->ranges.push_back(std::move(dep));
            };
            add_range(0, sizeof(u32));  // the NVN magic decides where the program header starts
            add_range(start, static_cast<u32>(sizeof(Shader::ProgramHeader)));
            std::vector<u32>& offs = recorder.instruction_offsets;
            std::sort(offs.begin(), offs.end());
            offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
            for (size_t i = 0; i < offs.size();) {
                size_t j = i + 1;
                while (j < offs.size() && offs[j] == offs[j - 1] + 8) ++j;
                add_range(offs[i], static_cast<u32>(offs[j - 1] + 8 - offs[i]));
                i = j;
            }
            auto by_key = [](const auto& a, const auto& b) { return std::tie(a.index, a.offset) < std::tie(b.index, b.offset); };
            std::sort(recorder.cbufs.begin(), recorder.cbufs.end(), by_key);
            recorder.cbufs.erase(std::unique(recorder.cbufs.begin(), recorder.cbufs.end(),
                                             [](const auto& a, const auto& b) { return a.index == b.index && a.offset == b.offset; }),
                                 recorder.cbufs.end());
            entry->cbufs = std::move(recorder.cbufs);
            if (shader_cache_relax_handles_) {
                for (ShaderCbufDep& c : entry->cbufs) {
                    c.handle_only = c.index == kTextureBoundBuffer && recorder.handle_offsets.count(c.offset) != 0;
                }
            }
            entry->bytes += entry->cbufs.size() * sizeof(ShaderCbufDep);
            auto& tx = recorder.textures;
            std::sort(tx.begin(), tx.end(), [](const auto& a, const auto& b) { return std::tie(a.from_cbuf_offset, a.handle, a.type, a.integer) < std::tie(b.from_cbuf_offset, b.handle, b.type, b.integer); });
            tx.erase(std::unique(tx.begin(), tx.end(), [](const auto& a, const auto& b) { return a.handle == b.handle && a.type == b.type && a.integer == b.integer && a.from_cbuf_offset == b.from_cbuf_offset; }), tx.end());
            if (!shader_cache_relax_handles_) {
                for (ShaderTextureDep& t : tx) t.from_cbuf_offset = -1;
            }
            entry->textures = std::move(tx);
            entry->bytes += entry->textures.size() * sizeof(ShaderTextureDep);
            *record = std::move(entry);
        }
        if (trace_c1_specialization) {
            auto& recorded = c1_specialization_reads_[{code, nvn_stage}];
            recorded.clear();
            for (const CbufReadProvenance& read : env.c1_specialization_reads) {
                if (!FindCbufReadProvenance(recorded, 1, read.offset)) recorded.push_back(read);
            }
            std::vector<u32> words;
            words.reserve(recorded.size() * 2);
            for (const CbufReadProvenance& read : recorded) {
                words.push_back(read.offset);
                words.push_back(read.value);
            }
            const u64 fingerprint = Hash(words.data(), words.size() * sizeof(u32));
            std::ostringstream values;
            const size_t shown = std::min<size_t>(recorded.size(), 12);
            for (size_t i = 0; i < shown; ++i) {
                values << (i ? "," : "") << std::hex << recorded[i].offset << ":" << recorded[i].value;
            }
            LOG_INFO("trace-c1-compile q=%llu program=0x%llx code=0x%llx stage=%zu reads=%zu omitted=%u "
                     "fingerprint=%llx first=%s",
                     static_cast<unsigned long long>(present_count_ + 1),
                     static_cast<unsigned long long>(bound_.program), static_cast<unsigned long long>(code), nvn_stage,
                     recorded.size(), static_cast<unsigned>(env.c1_specialization_reads.size() - recorded.size()),
                     static_cast<unsigned long long>(fingerprint), values.str().c_str());
        }
        if (const char* selected = std::getenv("NXRT_DUMP_SHADER_PROGRAM"); selected && *selected) {
            bool matches = false;
            for (const char* token = selected; token && *token;) {
                char* end = nullptr;
                const u64 wanted = std::strtoull(token, &end, 16);
                if (end == token || (*end != ',' && *end != '\0')) break;
                matches |= wanted == bound_.program;
                token = *end ? end + 1 : nullptr;
            }
            if (const char* target = std::getenv("NXRT_PROBE_DRAW_TARGET")) {
                const u64 wanted_target = std::strtoull(target, nullptr, 16);
                matches |= wanted_target && num_color_targets_ && ImageKey(color_targets_[0]) == wanted_target;
                unsigned width = 0, height = 0;
                if (std::sscanf(target, "%ux%u", &width, &height) == 2 && num_color_targets_) {
                    if (auto* img = ImageFor(color_targets_[0], color_levels_[0]))
                        matches |= img->width == width && img->height == height;
                }
            }
            if (matches) {
                std::ostringstream path;
                path << "/tmp/nxrt-shader-" << std::hex << bound_.program << "-" << code << "-" << nvn_stage
                     << ".ir";
                std::ofstream dump(path.str(), std::ios::out | std::ios::trunc);
                if (dump) {
                    dump << "guest_program=0x" << std::hex << bound_.program << "\nshader_code=0x" << code
                         << "\nnvn_stage=" << std::dec << nvn_stage << "\ntranslated_stage="
                         << static_cast<u32>(stage) << "\n\n" << Shader::IR::DumpProgram(out);
                    dump << "\ntexture_descriptors=" << out.info.texture_descriptors.size() << "\n";
                    for (size_t i = 0; i < out.info.texture_descriptors.size(); ++i) {
                        const auto& desc = out.info.texture_descriptors[i];
                        dump << "texture_descriptor index=" << i << " type=" << static_cast<u32>(desc.type)
                             << " depth=" << desc.is_depth << " integer=" << desc.is_integer
                             << " cbuf=" << desc.cbuf_index << " offset=0x" << std::hex << desc.cbuf_offset
                             << std::dec << " count=" << desc.count << " size_shift=" << desc.size_shift
                             << " shift_left=" << desc.shift_left << " secondary=" << desc.has_secondary;
                        if (desc.has_secondary) {
                            dump << " secondary_cbuf=" << desc.secondary_cbuf_index << " secondary_offset=0x"
                                 << std::hex << desc.secondary_cbuf_offset << std::dec
                                 << " secondary_shift_left=" << desc.secondary_shift_left;
                        }
                        dump << '\n';
                    }
                    size_t texture_op = 0;
                    for (const Shader::IR::Block* block : out.blocks) {
                        for (const Shader::IR::Inst& inst : block->Instructions()) {
                            const auto opcode = inst.GetOpcode();
                            if (opcode != Shader::IR::Opcode::ImageSampleImplicitLod &&
                                opcode != Shader::IR::Opcode::ImageSampleExplicitLod &&
                                opcode != Shader::IR::Opcode::ImageGather) {
                                continue;
                            }
                            const auto info = inst.Flags<Shader::IR::TextureInstInfo>();
                            dump << "texture_op index=" << texture_op++ << " opcode="
                                 << static_cast<u32>(opcode)
                                 << " descriptor=" << info.descriptor_index.Value()
                                 << " type=" << static_cast<u32>(info.type.Value())
                                 << " depth=" << info.is_depth.Value()
                                 << " bias=" << info.has_bias.Value()
                                 << " lod_clamp=" << info.has_lod_clamp.Value()
                                 << " gather_component=" << info.gather_component.Value() << '\n';
                        }
                    }
                    LOG_INFO("renderer: shader IR written to %s", path.str().c_str());
                }
            }
        }
        return true;
    } catch (const std::exception& e) {
        // GetPipeline records this as a pipeline error, and Draw skips only draws using this shader.
        // Keep diagnostics to address and mapping metadata; never log or dump guest shader bytes.
        const u32 at = env.last_address;
        static std::mutex failed_mutex;
        static std::map<std::tuple<GpuAddr, size_t, u32, std::string>, bool> failed;
        const auto failure_key = std::tuple{code, nvn_stage, at, std::string(e.what())};
        {
            std::lock_guard lock(failed_mutex);
            if (failed[failure_key]) return false;
            failed[failure_key] = true;
        }
        const bool instruction_aligned = (at & 7) == 0;
        const bool instruction_mapped = env.has_last_address && g_memory.IsMapped(cpu + at, sizeof(u64));
        if (std::getenv("NXRT_TRACE_SHADER_FAILURE")) {
            const CpuBackingSpan resolved_span{cpu, backing->size};
            const auto pc_status = GetShaderTracePcStatus(resolved_span, at);
            std::string previous_kind = "unavailable";
            std::string previous_target = "none";
            const auto previous = PreviousMaxwellInstructionOffset(at);
            bool previous_available = false;
            if (previous && ContainsBackingAccess(resolved_span, *previous, sizeof(u64))) {
                previous_available = true;
                try {
                    const auto instruction = Shader::Maxwell::Instruction{g_memory.Read<u64>(cpu + *previous)};
                    using Shader::Maxwell::Opcode;
                    switch (Shader::Maxwell::Decode(instruction.raw)) {
                    case Opcode::BRA:
                        previous_kind = "BRA";
                        if (const auto target = RelativeMaxwellBranchTarget(*previous, instruction.branch.Offset())) {
                            previous_target = fmt::format("+0x{:x}", *target);
                        }
                        break;
                    case Opcode::JMP:
                        previous_kind = "JMP";
                        previous_target = fmt::format("+0x{:x}", instruction.branch.Absolute());
                        break;
                    case Opcode::BRX: previous_kind = "BRX(indirect)"; break;
                    case Opcode::JMX: previous_kind = "JMX(indirect)"; break;
                    case Opcode::CAL:
                        previous_kind = "CAL";
                        if (const auto target = RelativeMaxwellBranchTarget(*previous, instruction.branch.Offset())) {
                            previous_target = fmt::format("+0x{:x}", *target);
                        }
                        break;
                    case Opcode::JCAL:
                        previous_kind = "JCAL";
                        previous_target = fmt::format("+0x{:x}", instruction.branch.Absolute());
                        break;
                    default: previous_kind = "non-branch-or-unclassified"; break;
                    }
                } catch (...) {
                    previous_kind = "unrecognized";
                }
            }
            std::string last_kind = "unavailable";
            std::string last_target = "none";
            if (env.has_last_address) {
                const auto instruction = InspectMaxwellTraceInstruction(
                    resolved_span, at, [&] { return g_memory.Read<u64>(cpu + at); },
                    [](u64 raw) { return Shader::Maxwell::Decode(raw); });
                last_kind = std::string(ShaderTraceFlowName(instruction.flow));
                if (instruction.target_status == ShaderTraceTargetStatus::Direct) {
                    last_target = fmt::format("+0x{:x}", instruction.target);
                } else {
                    last_target = std::string(ShaderTraceTargetStatusName(instruction.target_status));
                }
            }
            LOG_INFO("renderer: shader failure metadata: pc=+0x%x pc_in_backing=%u read_fits=%u "
                     "backing_span=0x%llx adjacent_previous_present=%u adjacent_previous=+0x%x kind=%s target=%s "
                     "last_instruction=%s last_target=%s",
                     at, static_cast<unsigned>(pc_status.pc_inside),
                     static_cast<unsigned>(pc_status.instruction_fits),
                     static_cast<unsigned long long>(backing->size), static_cast<unsigned>(previous_available),
                     previous.value_or(0), previous_kind.c_str(), previous_target.c_str(), last_kind.c_str(),
                     last_target.c_str());
            if (env.has_failed_cbuf_request) {
                LOG_INFO("renderer: shader failure cbuf provenance: program=0x%llx shader_code=0x%llx "
                         "nvn_stage=%zu shader_stage=%u requested_cbuf=%u requested_offset=0x%x "
                         "captured=%zu omitted=%u matched=%u",
                         static_cast<unsigned long long>(bound_.program), static_cast<unsigned long long>(code),
                         nvn_stage, static_cast<u32>(stage), env.failed_cbuf_index, env.failed_cbuf_offset,
                         env.cbuf_reads.size(),
                         env.cbuf_reads_omitted, static_cast<unsigned>(env.failed_cbuf_read.has_value()));
                if (env.failed_cbuf_read) {
                    LOG_INFO("renderer: shader failure cbuf read: %s",
                             FormatCbufReadProvenance(*env.failed_cbuf_read).c_str());
                }
            }
        }
        LOG_ERROR("renderer: shader at 0x%llx (cpu 0x%llx, program 0x%llx, nvn stage %zu, shader stage %u, "
                  "header %s, backing_span=0x%llx) did not translate at offset +0x%x "
                  "(read=%u aligned=%u mapped=%u): %s",
                  static_cast<unsigned long long>(code), static_cast<unsigned long long>(cpu),
                  static_cast<unsigned long long>(bound_.program), nvn_stage,
                  static_cast<u32>(stage), start ? "nvn" : "none",
                  static_cast<unsigned long long>(backing->size), at,
                  static_cast<unsigned>(env.has_last_address), static_cast<unsigned>(instruction_aligned),
                  static_cast<unsigned>(instruction_mapped), e.what());
        return false;
    }
}

VkRenderPass NvnRenderer::RenderPassFor(const std::vector<u32>& key) {
    auto& rp = render_passes_[key];
    if (rp) return rp;
    const u32 colors = key[0];
    std::vector<VkAttachmentDescription> descs;
    std::vector<VkAttachmentReference> refs;
    for (u32 i = 0; i < colors; ++i) {
        if (!key[1 + i]) { // unbound color slot
            refs.push_back({VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED});
            continue;
        }
        refs.push_back({static_cast<u32>(descs.size()), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
        descs.push_back({0, static_cast<VkFormat>(key[1 + i]), VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD,
                         VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
    }
    const VkFormat ds = static_cast<VkFormat>(key[1 + colors]);
    const VkAttachmentReference depth_ref{static_cast<u32>(descs.size()), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    if (ds) {
        descs.push_back({0, ds, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                         VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});
    }
    const VkSubpassDescription sub{0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, nullptr, static_cast<u32>(refs.size()),
                                   refs.data(), nullptr, ds ? &depth_ref : nullptr, 0, nullptr};
    const VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, nullptr, 0,
                                    static_cast<u32>(descs.size()), descs.data(), 1, &sub, 0, nullptr};
    vkCreateRenderPass(vk_.device, &ci, nullptr, &rp);
    return rp;
}

u64 NvnRenderer::ProgramIdentity(const nvn::Program& program) {
    if (const auto it = program_identity_.find(program.serial); it != program_identity_.end()) return it->second;
    u64 id = FastHash(&program.serial, 0, 0x9ae16a3b2f90404fULL);  // seed only; the serial itself is not part of it
    for (const nvn::ShaderStage& stage : program.stages) {
        const u64 addresses[2] = {stage.code, stage.control};
        id = FastHash(addresses, sizeof addresses, id);
        // The first bytes of the shader data (NVN header, program header, first instructions) and of its control
        // section: a different shader written to the same address is caught by this.
        for (const auto [gpu, bytes] : {std::pair<GpuAddr, size_t>{stage.code, 0x200}, {stage.control, 0x80}}) {
            const auto cpu = gpu ? GpuToCpu(gpu) : std::nullopt;
            if (!cpu || !g_memory.IsMapped(*cpu, bytes)) {
                id = FastHash(&gpu, 0, id + 1);
                continue;
            }
            std::array<u8, 0x200> head{};
            g_memory.ReadBlock(*cpu, head.data(), bytes);
            id = FastHash(head.data(), bytes, id);
        }
    }
    program_identity_.emplace(program.serial, id);
    return id;
}

bool NvnRenderer::ValidateShaderEntry(const ShaderEntry& entry, GpuAddr code) {
    const auto backing = GpuToCpuBacking(code);
    if (!backing) return false;
    const GuestAddr cpu = backing->cpu;
    std::vector<u8> now;
    for (const ShaderBytesDep& r : entry.ranges) {
        if (!ContainsBackingAccess(*backing, r.offset, r.bytes.size()) || !g_memory.IsMapped(cpu + r.offset, r.bytes.size())) {
            return false;
        }
        now.resize(r.bytes.size());
        g_memory.ReadBlock(cpu + r.offset, now.data(), now.size());
        if (std::memcmp(now.data(), r.bytes.data(), now.size()) != 0) { ++shader_dep_fail_[0]; return false; }
    }
    for (const ShaderCbufDep& c : entry.cbufs) {
        if (c.handle_only) continue;
        if (const u32 now_value = ReadCbuf(entry.nvn_stage, c.index, c.offset); now_value != c.value) {
            ++shader_dep_fail_[1];
            if (shader_dep_debug_ && shader_dep_debug_hist_.size() < 4096) {
                auto& e = shader_dep_debug_hist_[(u64{c.index} << 32) | c.offset];
                ++e.first;
                e.second.insert(now_value);
            }
            return false;
        }
    }
    for (const ShaderTextureDep& t : entry.textures) {
        const u32 handle = t.from_cbuf_offset >= 0
                               ? ReadCbuf(entry.nvn_stage, kTextureBoundBuffer, static_cast<u32>(t.from_cbuf_offset)) & 0xFFFFF
                               : t.handle;
        const auto tic = ReadTic(handle);
        if (t.type & 0x80000000u) {
            const Shader::TextureType type = tic ? ShaderTextureType(*tic) : Shader::TextureType::Color2D;
            if ((static_cast<u32>(type) | 0x80000000u) != t.type) { ++shader_dep_fail_[2]; return false; }
        } else if ((tic && TicFormat(*tic).integer) != t.integer) {
            ++shader_dep_fail_[2];
            return false;
        }
    }
    return true;
}

ShaderEntry* NvnRenderer::LookupShaderEntry(GpuAddr code) {
    auto* list = shader_cache_.Find(code);
    if (!list) return nullptr;
    for (auto& entry : *list) {
        if (ValidateShaderEntry(*entry, code)) return entry.get();
    }
    return nullptr;
}

Pipeline* NvnRenderer::GetPipeline(u32 topology) {
    static Pipeline none{};
    auto prog = RenderModel().programs.find(bound_.program);
    if (prog == RenderModel().programs.end() || prog->second.stages.empty()) {
        none.error = "no-program";
        return &none;
    }
    std::optional<HitchScope> pipe_phase;
    pipe_phase.emplace(kHitchPipeKey);
    std::vector<u64>& key = pipeline_key_;
    key.clear();
    key.insert(key.end(), {program_content_key_ ? ProgramIdentity(prog->second) : bound_.program_serial, topology, bound_.color_word, bound_.channel_mask,
                           bound_.depth_stencil[0], bound_.depth_stencil[1], bound_.polygon});
    for (u32 w : bound_.attribs) key.push_back(w);
    AppendVertexStreamStateToPipelineKey(key, bound_.streams);
    for (u32 i = 0; i < num_color_targets_; ++i) {
        key.push_back(bound_.blend_set[i] ? (u64{bound_.blend_words[i][0]} << 32 | bound_.blend_words[i][1]) : 0);
        const HostImage* img = ImageFor(color_targets_[i], color_levels_[i]);
        key.push_back(img ? img->format : 0);
    }
    const HostImage* ds = depth_target_ ? ImageFor(depth_target_, depth_level_) : nullptr;
    key.push_back(ds ? ds->format : 0);
    pipe_phase.emplace(kHitchPipeFind);
    const u64 hash = FastHash(key.data(), key.size() * sizeof(u64));
    if (auto it = pipelines_.find(hash); it != pipelines_.end()) {
        if (trace_render_memory_) ++pipeline_cache_hits_;
        const WinSampleTraceConfig& win_trace = GetWinSampleTraceConfig();
        const bool trace_c1_hit = win_trace.enabled && win_trace.valid && win_trace.frame &&
                                  present_count_ + 1 == *win_trace.frame && win_trace.program &&
                                  bound_.program == *win_trace.program && num_color_targets_ > 0 &&
                                  ImageKey(color_targets_[0]) == win_trace.target_key;
        if (trace_c1_hit) {
            for (const nvn::ShaderStage& shader : prog->second.stages) {
                bool found = false;
                for (const auto& [shader_key, reads] : c1_specialization_reads_) {
                    if (shader_key.first != shader.code) continue;
                    found = true;
                    std::vector<u32> compiled_words, current_words;
                    compiled_words.reserve(reads.size() * 2);
                    current_words.reserve(reads.size() * 2);
                    size_t changed = 0;
                    std::ostringstream differences;
                    size_t shown = 0;
                    for (const CbufReadProvenance& read : reads) {
                        const u32 current = ReadCbuf(shader_key.second, 1, read.offset);
                        compiled_words.push_back(read.offset);
                        compiled_words.push_back(read.value);
                        current_words.push_back(read.offset);
                        current_words.push_back(current);
                        if (current != read.value) {
                            ++changed;
                            if (shown < 8) {
                                differences << (shown ? "," : "") << std::hex << read.offset << ":" << read.value
                                            << ">" << current;
                                ++shown;
                            }
                        }
                    }
                    const u64 compiled_hash = Hash(compiled_words.data(), compiled_words.size() * sizeof(u32));
                    const u64 current_hash = Hash(current_words.data(), current_words.size() * sizeof(u32));
                    LOG_INFO("trace-c1-cache q=%llu pipeline=%llx program=0x%llx shader=0x%llx stage=%zu hit=1 "
                             "reads=%zu changed=%zu compiled=%llx current=%llx first=%s",
                             static_cast<unsigned long long>(present_count_ + 1),
                             static_cast<unsigned long long>(hash), static_cast<unsigned long long>(bound_.program),
                             static_cast<unsigned long long>(shader.code), shader_key.second, reads.size(), changed,
                             static_cast<unsigned long long>(compiled_hash), static_cast<unsigned long long>(current_hash),
                             differences.str().c_str());
                }
                if (!found) {
                    LOG_INFO("trace-c1-cache q=%llu pipeline=%llx program=0x%llx shader=0x%llx hit=1 "
                             "compile_reads=missing",
                             static_cast<unsigned long long>(present_count_ + 1),
                             static_cast<unsigned long long>(hash), static_cast<unsigned long long>(bound_.program),
                             static_cast<unsigned long long>(shader.code));
                }
            }
        }
        return it->second.get();
    }
    if (trace_render_memory_) ++pipeline_cache_misses_;
    HitchScope hitch_scope(kHitchPipeline);
    // NXRT_TRACE_PIPELINE_COMPILE=1: one line per pipeline built on the render thread (translate + SPIR-V + driver compile).
    static const bool trace_pipeline_compile = std::getenv("NXRT_TRACE_PIPELINE_COMPILE") != nullptr;
    struct CompileTimer {
        bool on;
        std::chrono::steady_clock::time_point start;
        u64 frame;
        u64 program;
        ~CompileTimer() {
            if (!on) return;
            LOG_INFO("pipeline-compile q=%llu program=%llx ms=%.2f", static_cast<unsigned long long>(frame),
                     static_cast<unsigned long long>(program),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
    } compile_timer{trace_pipeline_compile, trace_pipeline_compile ? std::chrono::steady_clock::now()
                                                                     : std::chrono::steady_clock::time_point{},
                    present_count_ + 1, bound_.program};
    auto& p = pipelines_[hash];
    p = std::make_unique<Pipeline>();
    if (!shader_pools_) shader_pools_ = std::make_unique<ShaderPools>();

    // The IR is needed only while emitting SPIR-V. Keep the metadata used by
    // draws in Pipeline, then return the large translation pools after `items`
    // has been destroyed on every exit path.
    struct ReleaseShaderPools {
        ShaderPools& pools;
        ~ReleaseShaderPools() { pools.ReleaseContents(); }
    } release_shader_pools{*shader_pools_};

    struct Item {
        Shader::Stage stage;
        Shader::IR::Program program;
        GpuAddr code = 0;
        ShaderEntry* entry = nullptr;  // translated-shader cache entry (found, or recorded by this translation)
        bool translated = false;       // `program` holds the IR
    };
    const bool use_shader_cache =
        shader_cache_enabled_ && std::getenv("NXRT_TRACE_SHADER_FAILURE") == nullptr && !GetWinSampleTraceConfig().enabled;
    if (use_shader_cache) shader_cache_.Trim();
    // Both calls are needed: the first reads only the header (to find the stage), the second translates at that
    // stage's NVN slot. Translating at slot 0 can consume another stage's driver c1 and derive a bad control-flow
    // target before the correct slot is known.
    const auto translate_item = [&](Item& item, bool record) -> bool {
        if (!Translate(item.code, nvn::kInvalidNvnStage, item.program, item.stage)) {
            p->error = GpuToCpu(item.code) ? "translate" : "code-address";
            return false;
        }
        const size_t nvn = kNvnStageOfPipelineStage[StageIndex(item.stage)];
        std::unique_ptr<ShaderEntry> recorded;
        if (!Translate(item.code, nvn, item.program, item.stage, record ? &recorded : nullptr)) {
            p->error = "translate";
            return false;
        }
        item.translated = true;
        if (recorded) item.entry = shader_cache_.Add(item.code, std::move(recorded));
        return true;
    };
    std::vector<Item> items;
    for (const nvn::ShaderStage& s : prog->second.stages) {
        Item item{};
        item.code = s.code;
        if (use_shader_cache) {
            if (ShaderEntry* hit = LookupShaderEntry(s.code)) {
                ++shader_cache_hits_;
                item.entry = hit;
                item.stage = hit->stage;
                items.push_back(std::move(item));
                continue;
            }
            ++shader_cache_misses_;
        }
        if (!translate_item(item, use_shader_cache)) return p.get();
        items.push_back(std::move(item));
    }
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return StageIndex(a.stage) < StageIndex(b.stage); });

    // Vertex input from the driver's attribute words and its format table.
    std::array<Shader::AttributeType, 32> input_types{};
    input_types.fill(Shader::AttributeType::Disabled);
    std::vector<VkVertexInputAttributeDescription> attributes;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::array<bool, 16> stream_used{};
    for (u32 i = 0; i < bound_.attribs.size() && i < 32; ++i) {
        const u32 w = bound_.attribs[i];
        if (!((w >> 6) & 1)) continue;
        const u32 hex = Format(w >> 21).vertex_attribute | (w & 0x1fff9f);
        const u32 buffer = hex & 0x1f, constant = (hex >> 6) & 1, offset = (hex >> 7) & 0x3fff;
        const u32 size = (hex >> 21) & 0x3f, type = (hex >> 27) & 7;
        if (constant) continue;
        input_types[i] = AttributeType(type);
        attributes.push_back({i, buffer, VertexFormat(size, type), offset});
        if (buffer < stream_used.size()) stream_used[buffer] = true;
    }
    for (u32 s = 0; s < stream_used.size(); ++s) {
        if (!stream_used[s]) continue;
        const u32 stride = 2 * s < bound_.streams.size() ? bound_.streams[2 * s] : 0;
        const u32 divisor = 2 * s + 1 < bound_.streams.size() ? bound_.streams[2 * s + 1] : 0;
        bindings.push_back({s, stride, divisor ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
    }

    // SPIR-V per stage, bindings counted across stages; the set layout follows the same order.
    Shader::Backend::Bindings binding;
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
    u32 next_binding = 0;
    const auto add = [&](VkDescriptorType type, VkShaderStageFlags stage, const auto& descriptors) {
        for (const auto& d : descriptors) {
            layout_bindings.push_back({next_binding++, type, d.count, stage, nullptr});
        }
    };
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkShaderModule> modules;
    Shader::VaryingState previous_stores{};
    std::map<Shader::IR::Attribute, Shader::IR::Attribute> previous_legacy_stores_mapping;
    bool has_previous = false;
    for (Item& item : items) {
        Shader::RuntimeInfo info{};
        if (has_previous) {
            info.previous_stage_stores = previous_stores;
            info.previous_stage_legacy_stores_mapping = previous_legacy_stores_mapping;
        } else {
            info.previous_stage_stores.mask.set();
        }
        // NVN clip space is OpenGL-style (z in [-w, w]); Vulkan's is [0, w]. Without this conversion a vertex
        // with z = -w (the common full-screen quad) is clipped away. NXRT_NO_NDC_CONVERT=1 restores the old behaviour.
        static const bool convert_ndc = std::getenv("NXRT_NO_NDC_CONVERT") == nullptr;
        info.convert_depth_mode = convert_ndc;
        if (item.stage == Shader::Stage::VertexB) info.generic_input_types = input_types;
        if (item.stage == Shader::Stage::Fragment) {
            for (u32 i = 0; i < num_color_targets_; ++i) {
                const auto* target = ImageFor(color_targets_[i], color_levels_[i]);
                info.color_output_types[i] = target && target->format == VK_FORMAT_R32G32B32A32_UINT
                                                ? Shader::AttributeType::UnsignedInt : Shader::AttributeType::Float;
            }
        }
        // Everything the emitter depends on besides the translated program: the RuntimeInfo above and the binding
        // counters on entry. Equal inputs on an entry that validated give the same SPIR-V and shader info.
        std::vector<u64> runtime_key;
        if (item.entry) {
            const auto& m = info.previous_stage_stores.mask;
            for (size_t w = 0; w < 8; ++w) {
                u64 v = 0;
                for (size_t b = 0; b < 64; ++b) v |= u64{m[w * 64 + b]} << b;
                runtime_key.push_back(v);
            }
            for (const auto& [a, b] : info.previous_stage_legacy_stores_mapping) {
                runtime_key.push_back(u64{static_cast<u32>(a)} << 32 | static_cast<u32>(b));
            }
            runtime_key.push_back(info.convert_depth_mode);
            for (size_t i = 0; i < 32; i += 8) {
                u64 v = 0;
                for (size_t b = 0; b < 8; ++b) v |= u64{static_cast<u8>(info.generic_input_types[i + b])} << (8 * b);
                runtime_key.push_back(v);
            }
            u64 colors = 0;
            for (size_t i = 0; i < 8; ++i) colors |= u64{static_cast<u8>(info.color_output_types[i])} << (8 * i);
            runtime_key.push_back(colors);
        }
        const Shader::Backend::Bindings binding_before = binding;
        std::vector<u32> code;
        Shader::Info stage_info;
        const ShaderVariant* variant = item.entry ? item.entry->FindVariant(runtime_key, binding_before) : nullptr;
        if (variant) {
            ++shader_cache_variant_hits_;
            code = variant->spirv;
            stage_info = variant->info;
            binding = variant->bindings_after;
            if (shader_cache_verify_) {
                // Diagnostic: translate and emit from scratch and compare with what the cache returned.
                ++shader_verify_checked_;
                Item fresh{};
                fresh.code = item.code;
                Shader::RuntimeInfo fresh_info = info;
                Shader::Backend::Bindings fresh_binding = binding_before;
                std::vector<u32> fresh_code;
                bool ok = translate_item(fresh, false);
                if (ok) {
                    try {
                        Shader::Maxwell::ConvertLegacyToGeneric(fresh.program, fresh_info);
                        fresh_code = Shader::Backend::SPIRV::EmitSPIRV(profile_, fresh_info, fresh.program, fresh_binding);
                    } catch (const std::exception&) {
                        ok = false;
                    }
                }
                if (!ok || fresh_code != code || fresh_binding.texture != binding.texture ||
                    fresh_binding.uniform_buffer != binding.uniform_buffer ||
                    fresh_binding.storage_buffer != binding.storage_buffer) {
                    ++shader_verify_mismatch_;
                    if (shader_verify_mismatch_ <= 8) {
                        LOG_ERROR("renderer: SHADER CACHE MISMATCH program=%llx code=%llx stage=%u cached=%zu words fresh=%zu words ok=%d",
                                  static_cast<unsigned long long>(bound_.program), static_cast<unsigned long long>(item.code),
                                  static_cast<u32>(item.stage), code.size(), fresh_code.size(), ok ? 1 : 0);
                    }
                }
            }
        } else {
            if (!item.translated && !translate_item(item, false)) return p.get();
            Shader::IR::Program& program = item.program;
            Shader::Maxwell::ConvertLegacyToGeneric(program, info);
            try {
                code = Shader::Backend::SPIRV::EmitSPIRV(profile_, info, program, binding);
            } catch (const std::exception& e) {
                LOG_ERROR("renderer: SPIR-V emission failed: %s", e.what());
                p->error = "spirv";
                return p.get();
            }
            stage_info = std::move(program.info);
            if (item.entry) {
                ShaderVariant v;
                v.runtime_key = runtime_key;
                v.bindings_before = binding_before;
                v.bindings_after = binding;
                v.spirv = code;
                v.info = stage_info;
                item.entry->bytes += v.spirv.size() * sizeof(u32) + sizeof(Shader::Info);
                shader_cache_.AddBytes(v.spirv.size() * sizeof(u32) + sizeof(Shader::Info));
                item.entry->variants.push_back(std::move(v));
            }
        }
        const size_t index = StageIndex(item.stage);
        VkShaderModule module;
        const VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                           code.size() * sizeof(u32), code.data()};
        vkCreateShaderModule(vk_.device, &smi, nullptr, &module);
        modules.push_back(module);
        previous_stores = stage_info.stores;
        previous_legacy_stores_mapping = stage_info.legacy_stores_mapping;
        has_previous = true;
        p->shader_infos[index] = std::move(stage_info);
        const Shader::Info& shader_info = p->shader_infos[index];
        p->infos[index] = &shader_info;
        const VkShaderStageFlags bit = StageBit(item.stage);
        add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, bit, shader_info.constant_buffer_descriptors);
        add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bit, shader_info.storage_buffers_descriptors);
        add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, bit, shader_info.texture_buffer_descriptors);
        add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, bit, shader_info.image_buffer_descriptors);
        add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, bit, shader_info.texture_descriptors);
        add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, bit, shader_info.image_descriptors);
        stages.push_back({VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, StageBit(item.stage), module,
                          "main", nullptr});
    }
    p->uses_push_descriptors =
        push_descriptors_enabled_ &&
        CanUsePushDescriptorLayout(vk_.has_push_descriptors, vk_.max_push_descriptors, layout_bindings.data(),
                                   static_cast<u32>(layout_bindings.size()));
    const auto create_set_layout = [&](bool push) {
        const VkDescriptorSetLayoutCreateInfo dsl{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            nullptr,
            push ? static_cast<VkDescriptorSetLayoutCreateFlags>(VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR)
                 : VkDescriptorSetLayoutCreateFlags{0},
            static_cast<u32>(layout_bindings.size()),
            layout_bindings.data()};
        return vkCreateDescriptorSetLayout(vk_.device, &dsl, nullptr, &p->set_layout);
    };
    VkResult set_layout_result = create_set_layout(p->uses_push_descriptors);
    if (set_layout_result != VK_SUCCESS && p->uses_push_descriptors) {
        p->uses_push_descriptors = false;
        set_layout_result = create_set_layout(false);
    }
    if (set_layout_result != VK_SUCCESS) {
        p->error = "descriptor-layout";
        return p.get();
    }
    const VkPushConstantRange push{VK_SHADER_STAGE_ALL_GRAPHICS, 0,
                                   static_cast<u32>(sizeof(Shader::Backend::SPIRV::RescalingLayout) +
                                                    sizeof(Shader::Backend::SPIRV::RenderAreaLayout))};
    const auto create_pipeline_layout = [&] {
        const VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1,
                                             &p->set_layout, 1, &push};
        return vkCreatePipelineLayout(vk_.device, &pli, nullptr, &p->layout);
    };
    VkResult pipeline_layout_result = create_pipeline_layout();
    if (pipeline_layout_result != VK_SUCCESS && p->uses_push_descriptors) {
        p->uses_push_descriptors = false;
        if (p->layout) vkDestroyPipelineLayout(vk_.device, p->layout, nullptr);
        p->layout = VK_NULL_HANDLE;
        if (p->set_layout) vkDestroyDescriptorSetLayout(vk_.device, p->set_layout, nullptr);
        p->set_layout = VK_NULL_HANDLE;
        set_layout_result = create_set_layout(false);
        if (set_layout_result == VK_SUCCESS) pipeline_layout_result = create_pipeline_layout();
    }
    if (pipeline_layout_result != VK_SUCCESS) {
        p->error = "pipeline-layout";
        return p.get();
    }

    // Fixed function state from the driver's state words.
    const VkPipelineVertexInputStateCreateInfo vertex_input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                            nullptr,
                                                            0,
                                                            static_cast<u32>(bindings.size()),
                                                            bindings.data(),
                                                            static_cast<u32>(attributes.size()),
                                                            attributes.data()};
    const VkPipelineInputAssemblyStateCreateInfo input_assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, nullptr, 0, Topology(topology), VK_FALSE};
    const VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, nullptr, 0,
                                                     1, nullptr, 1, nullptr};
    static const bool force_nocull = std::getenv("NXRT_FORCE_NOCULL") != nullptr;
    // The viewport is y-flipped by default (see the draw path), which already matches the guest winding, so the
    // front-face inversion below is dropped unless NXRT_YFLIP_INVERT_FRONT=1. NXRT_LEGACY_VIEWPORT=1 restores the old
    // positive-height viewport.
    static const bool viewport_yflip = std::getenv("NXRT_LEGACY_VIEWPORT") == nullptr;
    static const bool keep_front_face = std::getenv("NXRT_KEEP_FRONT_FACE") != nullptr ||
                                        (viewport_yflip && std::getenv("NXRT_YFLIP_INVERT_FRONT") == nullptr);
    const u32 poly = bound_.polygon, cull = force_nocull ? 0 : (poly & 3);
    const VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        nullptr,
        0,
        VK_FALSE,
        VK_FALSE,
        ((poly >> 3) & 3) == 0   ? VK_POLYGON_MODE_POINT
        : ((poly >> 3) & 3) == 1 ? VK_POLYGON_MODE_LINE
                                 : VK_POLYGON_MODE_FILL,
        static_cast<VkCullModeFlags>(cull == 0   ? VK_CULL_MODE_NONE
                                     : cull == 1 ? VK_CULL_MODE_FRONT_BIT
                                     : cull == 2 ? VK_CULL_MODE_BACK_BIT
                                                 : VK_CULL_MODE_FRONT_AND_BACK),
        // The viewport is not y-flipped (Vulkan clip y points down, NVN's up), so a guest-CCW triangle reaches the
        // rasterizer clockwise: the front-face sense has to be inverted for culling to match the guest.
        ((poly >> 2) & 1) == (keep_front_face ? 1u : 0u) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        VK_FALSE,
        0,
        0,
        0,
        1.0f};
    const VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                           nullptr, 0, VK_SAMPLE_COUNT_1_BIT, VK_FALSE, 0, nullptr,
                                                           VK_FALSE, VK_FALSE};
    const u32 ds0 = bound_.depth_stencil[0], ds1 = bound_.depth_stencil[1];
    const auto stencil = [&](u32 shift) {
        const u32 bits = ds1 >> shift;
        return VkStencilOpState{StencilOp((bits >> 4) & 0xf), StencilOp((bits >> 8) & 0xf),
                                StencilOp((bits >> 12) & 0xf), CompareOp(bits & 0xf), 0xff, 0xff, 0};
    };
    static const bool probe_equal_as_lequal = std::getenv("NXRT_PROBE_EQUAL_AS_LEQUAL") != nullptr;
    // Diagnostic (opt-in): NXRT_FORCE_NO_DEPTH_TEST=1 disables the guest depth test (A/B only).
    static const bool force_no_depth_test = std::getenv("NXRT_FORCE_NO_DEPTH_TEST") != nullptr;
    const bool depth_test_enabled = ds && (ds0 & 1) && !force_no_depth_test;
    const bool depth_write_enabled = ds && ((ds0 >> 1) & 1);
    const VkCompareOp guest_depth_compare = CompareOp((ds0 >> 4) & 0xf);
    const bool probe_depth_compare = probe_equal_as_lequal && depth_test_enabled && !depth_write_enabled &&
                                     guest_depth_compare == VK_COMPARE_OP_EQUAL;
    const VkCompareOp effective_depth_compare =
        probe_depth_compare ? VK_COMPARE_OP_LESS_OR_EQUAL : guest_depth_compare;
    if (probe_depth_compare) {
        LOG_INFO("probe-depth-equal-as-lequal program=0x%llx serial=%llu dsw=%08x compare=EQUAL->LESS_OR_EQUAL",
                 static_cast<unsigned long long>(bound_.program),
                 static_cast<unsigned long long>(bound_.program_serial), ds0);
    }
    const VkPipelineDepthStencilStateCreateInfo depth_stencil{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                              nullptr,
                                                              0,
                                                              depth_test_enabled,
                                                              depth_write_enabled,
                                                              effective_depth_compare,
                                                              VK_FALSE,
                                                              ds && ((ds0 >> 2) & 1),
                                                              stencil(0),
                                                              stencil(16),
                                                              0,
                                                              1};
    std::vector<VkPipelineColorBlendAttachmentState> blends;
    for (u32 i = 0; i < num_color_targets_; ++i) {
        const u32 w0 = bound_.blend_set[i] ? bound_.blend_words[i][0] : 0x110c0000;
        const u32 w1 = bound_.blend_set[i] ? bound_.blend_words[i][1] : 0x01020102;
        blends.push_back({static_cast<VkBool32>((bound_.color_word >> (8 + i)) & 1), BlendFactor(w1 & 0xff),
                          BlendFactor((w1 >> 8) & 0xff), BlendOp((w0 >> 24) & 7), BlendFactor((w1 >> 16) & 0xff),
                          BlendFactor(w1 >> 24), BlendOp((w0 >> 28) & 7),
                          EffectiveColorWriteMask(bound_.channel_mask, i)});
    }
    const VkPipelineColorBlendStateCreateInfo color_blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                          nullptr,
                                                          0,
                                                          VK_FALSE,
                                                          VK_LOGIC_OP_COPY,
                                                          static_cast<u32>(blends.size()),
                                                          blends.data(),
                                                          {0, 0, 0, 0}};
    static constexpr VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    const VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, nullptr, 0, 3,
                                                   dynamic_states};
    std::vector<u32> rp_key{num_color_targets_};
    for (u32 i = 0; i < num_color_targets_; ++i) {
        const HostImage* img = ImageFor(color_targets_[i], color_levels_[i]);
        rp_key.push_back(img ? img->format : 0);
    }
    rp_key.push_back(ds ? ds->format : 0);
    const VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                           nullptr,
                                           0,
                                           static_cast<u32>(stages.size()),
                                           stages.data(),
                                           &vertex_input,
                                           &input_assembly,
                                           nullptr,
                                           &viewport,
                                           &raster,
                                           &multisample,
                                           &depth_stencil,
                                           &color_blend,
                                           &dynamic,
                                           p->layout,
                                           RenderPassFor(rp_key),
                                           0,
                                           VK_NULL_HANDLE,
                                           0};
    VkResult r;
    {
        HitchScope driver_scope(kHitchDriverPipe);
        r = vkCreateGraphicsPipelines(vk_.device, VK_NULL_HANDLE, 1, &gpi, nullptr, &p->pipeline);
    }
    for (VkShaderModule m : modules) vkDestroyShaderModule(vk_.device, m, nullptr);
    if (r != VK_SUCCESS) {
        p->error = "vkCreateGraphicsPipelines";
        return p.get();
    }
    p->ok = true;
    return p.get();
}

// ------------------------------------------------------------------ textures and samplers

HostImage* NvnRenderer::SampledImage(u32 texture_id, u32 sample_handle, bool trace_win_sample,
                                     u32 trace_input_index, std::optional<Tic>* resolved_tic) {
    HitchScope hitch_scope(kHitchSampled, u64{texture_id} << 32 | sample_handle);
    // This status belongs to one resolution attempt. Do not let a prior failed
    // texture make a later successful cached sample look like the same error.
    texture_error_.clear();
    if (frame_profile_) ++win_resolves_;
    static u32 astc_trace_calls = 0;
    static const bool trace_astc_env = std::getenv("NXRT_TRACE_ASTC") != nullptr;  // env is fixed for the run
    const bool trace_astc_call = trace_astc_env && astc_trace_calls++ < 64;
    GuestAddr sampled_texture = 0;
    u64 sampled_serial = 0;
    // A texture this renderer drew into is sampled as it is (render to texture). The pool entry, its texture object
    // and the host image key only change with the model epoch, so they are memoized per (pool, id, epoch).
    SampledMemo* memo = nullptr;
    u64 memo_epoch = 0;
    bool memo_hit = false, in_pool = false;
    u64 host_key = 0;
    if (resolve_memo_ && !trace_win_sample && !trace_astc_call) {
        memo_epoch = RenderModel().epoch.load(std::memory_order_acquire);
        memo = &sampled_memo_[(bound_.texture_pool >> 4 ^ bound_.texture_pool >> 15 ^ texture_id * 0x9E3779B1u) &
                              (sampled_memo_.size() - 1)];
        if (memo->pool == bound_.texture_pool && memo->id == texture_id && memo->epoch == memo_epoch) {
            memo_hit = true;
            in_pool = memo->in_pool;
            sampled_texture = memo->texture;
            sampled_serial = memo->serial;
            host_key = memo->host_key;
        }
    }
    if (!memo_hit) {
        if (auto pool = RenderModel().texture_pools.find(bound_.texture_pool); pool != RenderModel().texture_pools.end()) {
            if (auto e = pool->second.find(texture_id); e != pool->second.end()) {
                in_pool = true;
                sampled_texture = e->second.first;
                if (auto texture = RenderModel().textures.find(sampled_texture); texture != RenderModel().textures.end()) {
                    sampled_serial = texture->second.serial;
                }
                u32 level = 0;
                if (auto views = RenderModel().texture_pool_views.find(bound_.texture_pool);
                    target_levels_ && views != RenderModel().texture_pool_views.end()) {
                    if (auto v = views->second.find(texture_id); v != views->second.end()) level = v->second.base_level;
                }
                host_key = LevelKey(ImageKey(e->second.first), level);
            }
        }
        if (memo) *memo = {bound_.texture_pool, texture_id, memo_epoch, in_pool, sampled_texture, sampled_serial, host_key, nullptr};
    }
    if (in_pool) {
        HostImage* rt = memo_hit ? memo->image : nullptr;
        if (!rt) {
            if (auto it = images_.find(host_key); it != images_.end()) {
                rt = &it->second;
                if (memo) memo->image = rt;
            }
        }
        if (rt && rt->rendered && rt->block_texel_target) {
            const auto descriptor = ReadTic(texture_id);
            HitchScope block_sync_scope(kHitchBlockSync, texture_id);
            if (!descriptor || !SyncBlockTexelTargets(sampled_texture, *descriptor)) {
                texture_error_ = "block-target-readback";
                return nullptr;
            }
            // The integer attachment stores BC3 blocks, not sampled RGBA texels.
            // Continue through the normal compressed texture upload below.
        } else if (rt && rt->rendered) {
            if (trace_win_sample) {
                const auto texture = RenderModel().textures.find(sampled_texture);
                const u64 target_key = ImageKey(sampled_texture);
                LOG_INFO("trace-win-resolved: handle=%x texture_pool=%llx guest_texture=%llx serial=%llu "
                         "target_key=%llx storage_pool=%llx storage_offset=%llx format=%u size=%ux%u draws=%llu",
                         sample_handle, static_cast<unsigned long long>(bound_.texture_pool),
                         static_cast<unsigned long long>(sampled_texture),
                         static_cast<unsigned long long>(sampled_serial), static_cast<unsigned long long>(target_key),
                         static_cast<unsigned long long>(texture != RenderModel().textures.end()
                                                            ? texture->second.desc.pool
                                                            : 0),
                         static_cast<unsigned long long>(texture != RenderModel().textures.end()
                                                            ? texture->second.desc.offset
                                                            : 0),
                         static_cast<unsigned>(rt->format), rt->width, rt->height,
                         static_cast<unsigned long long>(rt->draws));
            }
            if (trace_astc_call) {
                LOG_INFO("trace: sampled target-cache id=%x key=%llx format=%u size=%ux%u", texture_id,
                         static_cast<unsigned long long>(ImageKey(sampled_texture)),
                         static_cast<unsigned>(rt->format), rt->width, rt->height);
            }
            Transition(*rt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return rt;
        }
    }
    const auto tic = ReadTic(texture_id);
    if (!tic) {
        texture_error_ = "invalid-descriptor";
        return nullptr;
    }
    if (resolved_tic) *resolved_tic = tic;
    const auto source_cpu = GpuToCpu(tic->Address());
    if (InvalidTextureAddress(tic->Address(), source_cpu && g_memory.IsMapped(*source_cpu, 1))) {
        texture_error_ = "invalid-descriptor-address";
        return nullptr;
    }
    u64 descriptor_key = FastHash(&*tic, sizeof(*tic));
    if (sampled_texture) {
        if (const auto version = block_sample_versions_.find(ImageKey(sampled_texture));
            version != block_sample_versions_.end())
            descriptor_key = FastHash(&version->second, sizeof(version->second), descriptor_key);
    }
    const u64 key = TextureCacheKey(descriptor_key, sampled_texture, sampled_serial);
    static const char* const model_texture_dump_dir = std::getenv("NXRT_DUMP_MODEL_TEXTURE_DIR");
    static const u32 probe_texture_id = std::getenv("NXRT_DUMP_TEXTURE_ID")
                                          ? std::strtoul(std::getenv("NXRT_DUMP_TEXTURE_ID"), nullptr, 16) : 0;
    static std::set<u32> dumped_model_textures;
    const bool model_texture_input = trace_input_index == 0 || trace_input_index == 3 ||
                                     trace_input_index == 4 || trace_input_index == 5 || trace_input_index == 6;
    const bool dump_model_texture = model_texture_dump_dir && *model_texture_dump_dir &&
                                    ((trace_win_sample && bound_.program == 0x201b510660ULL && model_texture_input) ||
                                     (probe_texture_id && texture_id == probe_texture_id)) &&
                                    dumped_model_textures.insert(texture_id).second;
    if (resolve_memo_ && !dump_model_texture) {
        // The three per-key tables only gain entries (never erased) and their nodes are stable, so a direct-mapped
        // cache of the node pointers replaces three hash lookups per sampled texture and draw.
        SampledHitMemo& hit = sampled_hit_memo_[(key ^ key >> 17) & (sampled_hit_memo_.size() - 1)];
        if (!hit.image || hit.key != key) {
            if (auto it = sampled_.find(key); it != sampled_.end()) {
                hit.key = key;
                hit.image = &it->second;
                const auto pitch = pitch_sources_.find(key);
                hit.pitch = pitch != pitch_sources_.end() ? &pitch->second : nullptr;
                const auto swizzled = swizzled_sources_.find(key);
                hit.swizzled = swizzled != swizzled_sources_.end() ? &swizzled->second : nullptr;
            } else {
                hit.image = nullptr;
            }
        }
        if (hit.image && hit.key == key) {
            if (hit.pitch) {
                ++hit.pitch->uses;
                if (reupload_) RefreshPitch(key, *hit.image, *hit.pitch);
            }
            if (hit.swizzled && refresh_swizzled_) RefreshSwizzled(key, *hit.image, *hit.swizzled);
            return hit.image;
        }
    }
    if (auto it = sampled_.find(key); it != sampled_.end()) {
        if (!dump_model_texture) {
            if (auto src = pitch_sources_.find(key); src != pitch_sources_.end()) {
                ++src->second.uses;
                if (reupload_) RefreshPitch(key, it->second, src->second);
            }
            if (auto src = swizzled_sources_.find(key); refresh_swizzled_ && src != swizzled_sources_.end()) {
                RefreshSwizzled(key, it->second, src->second);
            }
            return &it->second;
        }
    }
    if (auto it = failed_textures_.find(key); it != failed_textures_.end()) {
        texture_error_ = it->second;
        return nullptr;
    }
    const auto fail = [&](std::string why) -> HostImage* {
        texture_error_ = why;
        failed_textures_.emplace(key, std::move(why));
        return nullptr;
    };
    const bool is_array = tic->Type() == kTex2DArray;
    const bool is_3d = tic->Type() == kTex3D;
    if (tic->Type() != kTex2D && tic->Type() != kTex2DNoMip && !is_array && !is_3d) {
        return fail("type" + std::to_string(tic->Type()));
    }
    // `layers` counts array layers or, for a 3D texture, depth slices (both are stored one after another).
    const u32 layers = (is_array || is_3d) ? std::min<u32>(tic->Depth(), 2048) : 1;
    const TextureFormat tf = TicFormat(*tic);
    if (tf.format == VK_FORMAT_UNDEFINED) return fail("format" + std::to_string(tic->Format()));
    const bool decode_astc = IsAstcFormat(tic->Format()) &&
                             !vk_.FormatSupports(tf.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
    if (trace_astc_call && IsAstcFormat(tic->Format())) {
        LOG_INFO("trace: astc decision texture=%x format=%u vk=%u size=%ux%u block=%ux%u srgb=%d decode=%d",
                 texture_id, tic->Format(), static_cast<unsigned>(tf.format), tic->Width(), tic->Height(),
                 tf.block_width, tf.block_height, tic->Srgb(), decode_astc);
    }
    if (!decode_astc && !vk_.FormatSupports(tf.format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        return fail("unsupported-format" + std::to_string(tic->Format()));
    }
    const u32 width = tic->Width(), height = tic->Height();
    const u32 wb = (width + tf.block_width - 1) / tf.block_width;
    const u32 hb = (height + tf.block_height - 1) / tf.block_height;
    const auto cpu = GpuToCpu(tic->Address());
    if (!cpu) return fail("address");
    // Array layers are stored one after another; each layer is a whole number of block-linear blocks.
    const size_t layer_bytes = static_cast<size_t>(wb) * hb * tf.bytes_per_block;
    const bool gpu_astc = decode_astc && AstcGpuEnabled() && AstcGpuUsable(vk_) && !dump_model_texture && tic->IsBlockLinear() && !is_3d &&
                          !is_array && layers == 1 && tic->TileWidthSpacing() == 0 && tf.bytes_per_block == 16;
    std::vector<u8> linear(gpu_astc ? 0 : layer_bytes * layers);
    if (is_3d && tic->IsBlockLinear()) {
        const size_t size = BlockLinearSize3D(tf.bytes_per_block, wb, hb, layers, tic->BlockHeight(),
                                              tic->BlockDepth(), tic->TileWidthSpacing());
        if (!g_memory.IsMapped(*cpu, size)) return fail("unmapped");
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        g_memory.ObserveRead(*cpu, size);
#endif
        UnswizzleBlockLinear3D(g_memory.Ptr(*cpu, size), size, linear.data(), tf.bytes_per_block, wb, hb, layers,
                               tic->BlockHeight(), tic->BlockDepth(), tic->TileWidthSpacing());
    }
    for (u32 layer = 0; layer < layers && !gpu_astc && !(is_3d && tic->IsBlockLinear()); ++layer) {
        u8* dst = linear.data() + layer_bytes * layer;
        if (tic->IsBlockLinear()) {
            const size_t size =
                BlockLinearSize(tf.bytes_per_block, wb, hb, tic->BlockHeight(), tic->TileWidthSpacing());
            const GuestAddr base = *cpu + size * layer;
            if (!g_memory.IsMapped(base, size)) return fail("unmapped");
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
            g_memory.ObserveRead(base, size);
#endif
            UnswizzleBlockLinear(g_memory.Ptr(base, size), size, dst, tf.bytes_per_block, wb, hb, tic->BlockHeight(),
                                 tic->TileWidthSpacing());
        } else {
            const u32 pitch = tic->Pitch();
            const GuestAddr base = *cpu + u64{pitch} * hb * layer;
            for (u32 y = 0; y < hb; ++y) {
                g_memory.ReadBlock(base + u64{y} * pitch, dst + static_cast<size_t>(y) * wb * tf.bytes_per_block,
                                   static_cast<size_t>(wb) * tf.bytes_per_block);
            }
        }
    }
    if (dump_model_texture) {
        std::ostringstream stem;
        stem << model_texture_dump_dir << "/model-texture-" << std::hex << texture_id;
        std::ofstream raw(stem.str() + ".blocks", std::ios::binary | std::ios::trunc);
        if (raw) raw.write(reinterpret_cast<const char*>(linear.data()), static_cast<std::streamsize>(linear.size()));
        std::ofstream metadata(stem.str() + ".txt", std::ios::out | std::ios::trunc);
        if (metadata) {
            metadata << "texture_id=0x" << std::hex << texture_id << std::dec << "\ninput_index=" << trace_input_index
                     << "\ngpu=0x" << std::hex << tic->Address() << "\ncpu=0x" << *cpu << std::dec
                     << "\nformat=0x" << std::hex << tic->Format() << std::dec << "\nwidth=" << width
                     << "\nheight=" << height << "\nlayers=" << layers
                     << "\nblock_width=" << tf.block_width << "\nblock_height=" << tf.block_height
                     << "\nbytes_per_block=" << tf.bytes_per_block << "\nblock_linear=" << tic->IsBlockLinear()
                     << "\nsrgb=" << tic->Srgb() << "\nlinear_bytes=" << linear.size() << '\n';
        }
        if (IsAstcFormat(tic->Format()) && layers == 1) {
            std::vector<u8> rgba;
            std::string decode_error;
            if (DecodeAstc(linear, width, height, tf.block_width, tf.block_height, tic->Srgb(), rgba, decode_error)) {
                std::ofstream decoded_file(stem.str() + ".rgba", std::ios::binary | std::ios::trunc);
                if (decoded_file) {
                    decoded_file.write(reinterpret_cast<const char*>(rgba.data()),
                                       static_cast<std::streamsize>(rgba.size()));
                }
            } else {
                LOG_ERROR("renderer model texture probe ASTC decode failed: id=%x error=%s", texture_id,
                          decode_error.c_str());
            }
        }
        LOG_INFO("renderer model texture probe: input=%u id=%x format=%u size=%ux%u layers=%u bytes=%zu astc=%u",
                 trace_input_index, texture_id, tic->Format(), width, height, layers, linear.size(),
                 IsAstcFormat(tic->Format()) ? 1u : 0u);
    }
    if (auto it = sampled_.find(key); it != sampled_.end()) {
        if (auto src = pitch_sources_.find(key); src != pitch_sources_.end()) {
            ++src->second.uses;
            if (reupload_) RefreshPitch(key, it->second, src->second);
        }
        if (auto src = swizzled_sources_.find(key); refresh_swizzled_ && src != swizzled_sources_.end()) {
            RefreshSwizzled(key, it->second, src->second);
        }
        return &it->second;
    }
    VkFormat upload_format = tf.format;
    const std::vector<u8>* upload_bytes = &linear;
    std::vector<u8> decoded;
    u64 gpu_base_offset = 0;
    u32 gpu_base_block_height = 0;
    size_t gpu_base_size = 0;
    if (gpu_astc) {
        gpu_base_block_height = tic->BlockHeight();
        gpu_base_size = BlockLinearSize(16, wb, hb, gpu_base_block_height, 0);
        if (!g_memory.IsMapped(*cpu, gpu_base_size)) return fail("unmapped");
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        g_memory.ObserveRead(*cpu, gpu_base_size);
#endif
        upload_format = tic->Srgb() ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        HitchScope upload_scope(kHitchUpload, u64{texture_id} << 32 | gpu_base_size);
        gpu_base_offset = Upload(g_memory.Ptr(*cpu, gpu_base_size), gpu_base_size, 256);
        if (gpu_base_offset == ~u64{0}) {
            texture_error_ = "upload";
            return nullptr;
        }
    } else if (decode_astc) {
        HitchScope astc_scope(kHitchAstc, u64{texture_id} << 32 | width << 16 | height);
        std::string decode_error;
        std::vector<u8> layer_rgba;
        for (u32 layer = 0; layer < layers; ++layer) {
            const auto first = linear.begin() + static_cast<std::ptrdiff_t>(layer_bytes * layer);
            const std::vector<u8> layer_in(first, first + static_cast<std::ptrdiff_t>(layer_bytes));
            if (!DecodeAstc(layer_in, width, height, tf.block_width, tf.block_height, tic->Srgb(), layer_rgba,
                            decode_error)) {
                return fail("astc-decode:" + decode_error);
            }
            decoded.insert(decoded.end(), layer_rgba.begin(), layer_rgba.end());
        }
        if (std::getenv("NXRT_TRACE_ASTC")) {
            std::array<u8, 4> low{255, 255, 255, 255}, high{};
            std::array<u64, 9> alpha_nonzero{};
            for (size_t i = 0; i + 3 < decoded.size(); i += 4) {
                for (size_t c = 0; c < 4; ++c) {
                    low[c] = std::min(low[c], decoded[i + c]);
                    high[c] = std::max(high[c], decoded[i + c]);
                }
            }
            for (u32 y = 0; y < height; ++y) {
                for (u32 x = 0; x < width; ++x) {
                    const u32 tile_x = std::min(2u, (x * 3) / width);
                    const u32 tile_y = std::min(2u, (y * 3) / height);
                    if (decoded[(static_cast<size_t>(y) * width + x) * 4 + 3]) {
                        ++alpha_nonzero[tile_y * 3 + tile_x];
                    }
                }
            }
            LOG_INFO("trace: astc texture=%x blocks=%ux%u block=%ux%u srgb=%d bytes=%zu rgba range=%u..%u,%u..%u,%u..%u,%u..%u first=%u,%u,%u,%u",
                     texture_id, wb, hb, tf.block_width, tf.block_height, tic->Srgb(), linear.size(), low[0], high[0],
                     low[1], high[1], low[2], high[2], low[3], high[3], decoded[0], decoded[1], decoded[2], decoded[3]);
            LOG_INFO("trace: astc alpha-nonzero 3x3=%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu",
                     static_cast<unsigned long long>(alpha_nonzero[0]),
                     static_cast<unsigned long long>(alpha_nonzero[1]),
                     static_cast<unsigned long long>(alpha_nonzero[2]),
                     static_cast<unsigned long long>(alpha_nonzero[3]),
                     static_cast<unsigned long long>(alpha_nonzero[4]),
                     static_cast<unsigned long long>(alpha_nonzero[5]),
                     static_cast<unsigned long long>(alpha_nonzero[6]),
                     static_cast<unsigned long long>(alpha_nonzero[7]),
                     static_cast<unsigned long long>(alpha_nonzero[8]));
        }
        upload_format = tic->Srgb() ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        upload_bytes = &decoded;
    }
    const u64 offset = gpu_astc ? gpu_base_offset : [&] {
        HitchScope upload_scope(kHitchUpload, u64{texture_id} << 32 | upload_bytes->size());
        return Upload(upload_bytes->data(), upload_bytes->size(), 16);
    }();
    if (offset == ~u64{0}) {
        texture_error_ = "upload";
        return nullptr;
    }
    // NVN block-linear mip levels follow the base image in guest memory. Preserve them instead of
    // letting Vulkan clamp every far-surface sample to level 0. Arrays and 3D images use different
    // level layouts and stay on their existing paths until they are traced separately.
    static const bool upload_texture_mips = std::getenv("NXRT_NO_TEX_MIPS") == nullptr;
    struct MipLevel {
        u64 offset;
        u32 width, height;
        u32 blocks_w = 0, blocks_h = 0, block_height_log2 = 0;
        size_t size = 0;
    };
    std::vector<MipLevel> mip_levels;
    if (upload_texture_mips && tic->IsBlockLinear() && !is_3d && !is_array && layers == 1 &&
        tic->MaxMipLevel() > 0) {
        GuestAddr mip_address = *cpu + BlockLinearSize(tf.bytes_per_block, wb, hb, tic->BlockHeight(),
                                                       tic->TileWidthSpacing());
        u32 block_height = tic->BlockHeight();
        for (u32 level = 1; level <= tic->MaxMipLevel(); ++level) {
            const u32 mip_width = std::max(1u, width >> level);
            const u32 mip_height = std::max(1u, height >> level);
            const u32 mip_blocks_w = (mip_width + tf.block_width - 1) / tf.block_width;
            const u32 mip_blocks_h = (mip_height + tf.block_height - 1) / tf.block_height;
            while (block_height > 0 && mip_blocks_h <= (1u << (block_height - 1)) * 8) --block_height;
            const size_t mip_size = BlockLinearSize(tf.bytes_per_block, mip_blocks_w, mip_blocks_h, block_height,
                                                    tic->TileWidthSpacing());
            if (!g_memory.IsMapped(mip_address, mip_size)) break;

#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
            g_memory.ObserveRead(mip_address, mip_size);
#endif
            if (gpu_astc) {
                const u64 raw_offset = Upload(g_memory.Ptr(mip_address, mip_size), mip_size, 256);
                if (raw_offset == ~u64{0}) break;
                mip_levels.push_back({raw_offset, mip_width, mip_height, mip_blocks_w, mip_blocks_h, block_height,
                                      mip_size});
                mip_address += mip_size;
                continue;
            }
            std::vector<u8> mip_data(static_cast<size_t>(mip_blocks_w) * mip_blocks_h * tf.bytes_per_block);
            UnswizzleBlockLinear(g_memory.Ptr(mip_address, mip_size), mip_size, mip_data.data(), tf.bytes_per_block,
                                 mip_blocks_w, mip_blocks_h, block_height, tic->TileWidthSpacing());
            if (decode_astc) {
                std::vector<u8> mip_rgba;
                std::string decode_error;
                if (!DecodeAstc(mip_data, mip_width, mip_height, tf.block_width, tf.block_height, tic->Srgb(),
                                mip_rgba, decode_error)) {
                    break;
                }
                mip_data.swap(mip_rgba);
            }
            const u64 mip_offset = Upload(mip_data.data(), mip_data.size(), 16);
            if (mip_offset == ~u64{0}) break;
            mip_levels.push_back({mip_offset, mip_width, mip_height});
            mip_address += mip_size;
        }
    }
    // Diagnostic (opt-in): NXRT_TRACE_TEX_CREATE=<max edge> logs each texture upload up to that size with its TIC
    // format, component swizzle and first bytes.
    if (const char* edge = std::getenv("NXRT_TRACE_TEX_CREATE"); edge && *edge && width <= std::strtoul(edge, nullptr, 0)) {
        std::ostringstream head;
        for (size_t i = 0; i < std::min<size_t>(16, linear.size()); ++i) head << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(linear[i]);
        LOG_INFO("tex-create: key=%llx format=%u %ux%u frame=%llu swizzle=%u,%u,%u,%u first16=%s",
                 static_cast<unsigned long long>(key), tic->Format(), width, height,
                 static_cast<unsigned long long>(present_count_), tic->Swizzle(0), tic->Swizzle(1), tic->Swizzle(2),
                 tic->Swizzle(3), head.str().c_str());
    }
    if (std::getenv("NXRT_TRACE_GLYPH") && !tic->IsBlockLinear()) {
        const size_t nonzero = linear.size() - static_cast<size_t>(std::count(linear.begin(), linear.end(), u8{0}));
        LOG_INFO("glyph-tex: first upload key=%llx addr=%llx format=%u %ux%u pitch=%u frame=%llu nonzero=%zu/%zu "
                 "swizzle=%u,%u,%u,%u",
                 static_cast<unsigned long long>(key), static_cast<unsigned long long>(*cpu), tic->Format(), width,
                 height, tic->Pitch(), static_cast<unsigned long long>(present_count_), nonzero, linear.size(),
                 tic->Swizzle(0), tic->Swizzle(1), tic->Swizzle(2), tic->Swizzle(3));
    }
    // OPT-IN (NXRT_ASTC_REFRESH=1): decode a software-decoded ASTC texture again when the guest rewrites it. It made
    // the island trees and objects disappear on NVIDIA (the game recycles texture memory, so a refresh can show other
    // data) and did not bring the Mii face paint back, so the first decode is kept for the whole run by default.
    static const bool refresh_astc = std::getenv("NXRT_ASTC_REFRESH") != nullptr;
    if (refresh_swizzled_ && tic->IsBlockLinear() &&
        (!decode_astc || (refresh_astc && layers == 1 && mip_levels.empty()))) {
        SwizzledSource src{*cpu, 0, tf.bytes_per_block, wb, hb, layers, tic->BlockHeight(), tic->BlockDepth(),
                           tic->TileWidthSpacing(), is_3d};
        src.layer_size = is_3d ? BlockLinearSize3D(tf.bytes_per_block, wb, hb, layers, tic->BlockHeight(),
                                                   tic->BlockDepth(), tic->TileWidthSpacing())
                               : BlockLinearSize(tf.bytes_per_block, wb, hb, tic->BlockHeight(),
                                                 tic->TileWidthSpacing());
        src.bytes = src.layer_size * (is_3d ? 1 : layers);
        if (decode_astc) {
            src.astc = true;
            src.astc_srgb = tic->Srgb();
            src.astc_block_width = tf.block_width;
            src.astc_block_height = tf.block_height;
            src.pixel_width = width;
            src.pixel_height = height;
        }
        const u8* guest = g_memory.Ptr(*cpu, src.bytes);
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        g_memory.ObserveRead(*cpu, src.bytes);
#endif
        src.content_hash = SwizzledHash(guest, src.bytes);
        src.checked_frame = present_count_;
        swizzled_sources_[key] = std::move(src);
    }
    if ((reupload_ || std::getenv("NXRT_TRACE_GLYPH")) && !tic->IsBlockLinear() && !decode_astc && !is_3d) {
        pitch_sources_[key] = PitchSource{*cpu, tic->Pitch(), wb * tf.bytes_per_block, hb, layers, tic->Format(),
                                          std::move(linear), present_count_, 0};
    }
    HostImage& img = sampled_[key];
    if (tic->Format() == 0x1d && !tic->IsBlockLinear()) glyph_image_ = &img;
    img.format = upload_format;
    img.width = width;
    img.height = height;
    const VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, nullptr,
                                static_cast<VkImageCreateFlags>(gpu_astc ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0), is_3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                                upload_format, {width, height, is_3d ? layers : 1}, 1 + static_cast<u32>(mip_levels.size()), is_3d ? 1 : layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                    static_cast<VkImageUsageFlags>(gpu_astc ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
                                VK_SHARING_MODE_EXCLUSIVE, 0, nullptr, VK_IMAGE_LAYOUT_UNDEFINED};
    img.image = vk_.CreateImage(ici);
    if (!img.image.image) {
        sampled_.erase(key);  // not remembered as failed: device memory may free up
        texture_error_ = "image-create";
        return nullptr;
    }
    img.layers = layers;
    img.aspect = tf.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    const VkImageViewUsageCreateInfo sampled_usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr,
                                                   VK_IMAGE_USAGE_SAMPLED_BIT};
    const VkImageViewCreateInfo vci{
        VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        gpu_astc ? &sampled_usage : nullptr,
        0,
        img.image.image,
        is_3d ? VK_IMAGE_VIEW_TYPE_3D : is_array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
        upload_format,
        {Swizzle(tic->Swizzle(0)), Swizzle(tic->Swizzle(1)), Swizzle(tic->Swizzle(2)), Swizzle(tic->Swizzle(3))},
        {img.aspect, 0, 1 + static_cast<u32>(mip_levels.size()), 0, is_3d ? 1 : layers}};
    img.view_type = vci.viewType;
    img.view_aspect = vci.subresourceRange.aspectMask;
    img.view_base_layer = vci.subresourceRange.baseArrayLayer;
    img.view_layer_count = vci.subresourceRange.layerCount;
    img.view_create_result = vkCreateImageView(vk_.device, &vci, nullptr, &img.view);
    if (gpu_astc) {
        AstcGpuPipeline& astc_pipe = AstcPipeline(vk_);
        if (astc_pipe.pipeline) {
            HitchScope gpu_scope(kHitchAstc, u64{texture_id} << 32 | width << 16 | height);
            Transition(img, VK_IMAGE_LAYOUT_GENERAL);
            const VkCommandBuffer cmd = Cmd();
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipe.pipeline);
            const auto decode_level = [&](u32 level, u64 raw_offset, size_t raw_size, u32 blocks_w, u32 blocks_h,
                                          u32 block_height_log2) {
                const VkImageViewUsageCreateInfo storage_usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, nullptr,
                                                               VK_IMAGE_USAGE_STORAGE_BIT};
                const VkImageViewCreateInfo svi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                                &storage_usage,
                                                0,
                                                img.image.image,
                                                VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                                VK_FORMAT_R8G8B8A8_UNORM,
                                                {},
                                                {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1}};
                VkImageView storage_view = VK_NULL_HANDLE;
                if (vkCreateImageView(vk_.device, &svi, nullptr, &storage_view) != VK_SUCCESS) return;
                img.layer_views.push_back(storage_view);
                VkDescriptorSet set = VK_NULL_HANDLE;
                const VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr,
                                                      descriptors_, 1, &astc_pipe.set_layout};
                if (vkAllocateDescriptorSets(vk_.device, &dai, &set) != VK_SUCCESS) return;
                ++sets_in_pool_;
                const VkDescriptorBufferInfo buf{stream_.buffer, raw_offset, raw_size};
                const VkDescriptorImageInfo img_info{VK_NULL_HANDLE, storage_view, VK_IMAGE_LAYOUT_GENERAL};
                const std::array<VkWriteDescriptorSet, 2> writes{{
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 0, 0, 1,
                     VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &buf, nullptr},
                    {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                     &img_info, nullptr, nullptr}}};
                vkUpdateDescriptorSets(vk_.device, 2, writes.data(), 0, nullptr);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, astc_pipe.layout, 0, 1, &set, 0,
                                        nullptr);
                const u32 gobs_x = (blocks_w * 16 + 63) / 64;
                const u32 x_shift = 9 + block_height_log2;
                const AstcGpuPush push{{tf.block_width, tf.block_height}, 0, gobs_x << x_shift, x_shift,
                                       block_height_log2, (1u << block_height_log2) - 1};
                vkCmdPushConstants(cmd, astc_pipe.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
                vkCmdDispatch(cmd, (blocks_w + 7) / 8, (blocks_h + 7) / 8, 1);
                (void)raw_size;
            };
            decode_level(0, offset, gpu_base_size, wb, hb, gpu_base_block_height);
            for (size_t level = 0; level < mip_levels.size(); ++level) {
                const MipLevel& m = mip_levels[level];
                decode_level(static_cast<u32>(level + 1), m.offset, m.size, m.blocks_w, m.blocks_h,
                             m.block_height_log2);
            }
            Transition(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            return &img;
        }
        // Pipeline unavailable: fall back is not possible here because the raw blocks were uploaded.
        LOG_ERROR("renderer: ASTC GPU decoder unavailable; texture %x left blank", texture_id);
        Transition(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return &img;
    }
    Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy copy{offset, 0, 0, {img.aspect, 0, 0, is_3d ? 1 : layers}, {0, 0, 0},
                                  {width, height, is_3d ? layers : 1}};
    vkCmdCopyBufferToImage(Cmd(), stream_.buffer, img.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    for (size_t level = 0; level < mip_levels.size(); ++level) {
        const VkBufferImageCopy mip_copy{mip_levels[level].offset, 0, 0,
                                         {img.aspect, static_cast<u32>(level + 1), 0, 1}, {0, 0, 0},
                                         {mip_levels[level].width, mip_levels[level].height, 1}};
        vkCmdCopyBufferToImage(Cmd(), stream_.buffer, img.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &mip_copy);
    }
    Transition(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &img;
}

HostImage* NvnRenderer::MirrorFloorD32Sample(HostImage& source, u64 draw_id) {
    if (source.format != VK_FORMAT_D32_SFLOAT || (source.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) == 0 ||
        source.layers != 1 || !source.width || !source.height) {
        LOG_ERROR("probe floor D32 mirror rejected: format=%u aspect=0x%x layers=%u size=%ux%u",
                  static_cast<unsigned>(source.format), static_cast<unsigned>(source.aspect), source.layers,
                  source.width, source.height);
        return nullptr;
    }

    const VkImageLayout restore_layout = source.layout;
    const VkDeviceSize byte_count = VkDeviceSize{source.width} * source.height * sizeof(float);
    Buffer readback = AcquireReadback(byte_count);
    if (!readback.buffer || !readback.mapped) return nullptr;

    Transition(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy read_copy{0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0},
                                      {source.width, source.height, 1}};
    vkCmdCopyImageToBuffer(Cmd(), source.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1,
                           &read_copy);
    const size_t pending_stream_size = stream_offset_;
    std::vector<u8> pending_stream(stream_.mapped, stream_.mapped + pending_stream_size);
    if (!Flush(FlushReason::DumpImage)) {
        retained_readbacks_.push_back(readback);
        return nullptr;
    }

    if (pending_stream_size) std::memcpy(stream_.mapped, pending_stream.data(), pending_stream_size);
    stream_offset_ = pending_stream_size;
    Transition(source, restore_layout);
    std::vector<u8> pixels(static_cast<size_t>(byte_count));
    std::memcpy(pixels.data(), readback.mapped, pixels.size());
    ReleaseReadback(readback);
    if (!MirrorImageRows(pixels.data(), source.width, source.height, sizeof(float))) return nullptr;

    HostImage& mirrored = floor_d32_mirror_probe_;
    if (mirrored.image.image &&
        (mirrored.width != source.width || mirrored.height != source.height || mirrored.format != source.format)) {
        if (recording_) EndRenderPass();
        DestroyImage(mirrored);
    }
    if (!mirrored.image.image) {
        mirrored.format = VK_FORMAT_D32_SFLOAT;
        mirrored.width = source.width;
        mirrored.height = source.height;
        mirrored.layers = 1;
        mirrored.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        mirrored.view_aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        const VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                           nullptr,
                                           0,
                                           VK_IMAGE_TYPE_2D,
                                           VK_FORMAT_D32_SFLOAT,
                                           {mirrored.width, mirrored.height, 1},
                                           1,
                                           1,
                                           VK_SAMPLE_COUNT_1_BIT,
                                           VK_IMAGE_TILING_OPTIMAL,
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                           VK_SHARING_MODE_EXCLUSIVE,
                                           0,
                                           nullptr,
                                           VK_IMAGE_LAYOUT_UNDEFINED};
        mirrored.image = vk_.CreateImage(image_info);
        if (!mirrored.image.image) {
            DestroyImage(mirrored);
            return nullptr;
        }
        const VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                              nullptr,
                                              0,
                                              mirrored.image.image,
                                              VK_IMAGE_VIEW_TYPE_2D,
                                              VK_FORMAT_D32_SFLOAT,
                                              {},
                                              {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
        mirrored.view_type = view_info.viewType;
        mirrored.view_create_result = vkCreateImageView(vk_.device, &view_info, nullptr, &mirrored.view);
        if (mirrored.view_create_result != VK_SUCCESS) {
            DestroyImage(mirrored);
            return nullptr;
        }
    }

    const u64 offset = Upload(pixels.data(), pixels.size(), alignof(float));
    if (offset == ~u64{0}) {
        LOG_ERROR("probe floor D32 mirror upload failed: draw=%llu bytes=%llu",
                  static_cast<unsigned long long>(draw_id), static_cast<unsigned long long>(byte_count));
        return nullptr;
    }
    Transition(mirrored, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy upload_copy{offset, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0},
                                        {mirrored.width, mirrored.height, 1}};
    vkCmdCopyBufferToImage(Cmd(), stream_.buffer, mirrored.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &upload_copy);
    Transition(mirrored, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    LOG_INFO("probe floor D32 mirror applied: q=%llu draw=%llu source=%p image=%p size=%ux%u bytes=%llu",
             static_cast<unsigned long long>(present_count_), static_cast<unsigned long long>(draw_id),
             static_cast<void*>(&source), static_cast<void*>(&mirrored), mirrored.width, mirrored.height,
             static_cast<unsigned long long>(byte_count));
    return &mirrored;
}

HostImage* NvnRenderer::SnapshotFeedbackTarget(HostImage& source) {
    if ((source.aspect & VK_IMAGE_ASPECT_COLOR_BIT) == 0 || !source.image.image || !source.width || !source.height ||
        !source.layers) {
        return nullptr;
    }

    HostImage* found = nullptr;
    for (auto& candidate : feedback_snapshots_) {
        const HostImage& c = *candidate;
        if (c.width == source.width && c.height == source.height && c.layers == source.layers &&
            c.format == source.format && c.cube == source.cube && c.mutable_format == source.mutable_format) {
            found = candidate.get();
            break;
        }
    }
    if (!found) {
        if (feedback_snapshots_.size() >= kMaxFeedbackSnapshots) {
            HostImage victim = std::move(*feedback_snapshots_.front());
            feedback_snapshots_.pop_front();
            if (recording_ || async_ring_.InFlight() > 0) {
                retired_images_.push_back(std::move(victim));
            } else {
                DestroyImage(victim);
            }
        }
        feedback_snapshots_.push_back(std::make_unique<HostImage>());
        found = feedback_snapshots_.back().get();
    }
    HostImage& snapshot = *found;
    if (!snapshot.image.image) {
        snapshot.format = source.format;
        snapshot.width = source.width;
        snapshot.height = source.height;
        snapshot.layers = source.layers;
        snapshot.cube = source.cube;
        snapshot.aspect = source.aspect;
        snapshot.mutable_format = source.mutable_format;
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT;
        const VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                    nullptr,
                                    (snapshot.cube ? VkImageCreateFlags{VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT} : 0) |
                                    (snapshot.mutable_format ? VkImageCreateFlags{VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT} : 0),
                                    VK_IMAGE_TYPE_2D,
                                    snapshot.format,
                                    {snapshot.width, snapshot.height, 1},
                                    1,
                                    snapshot.layers,
                                    VK_SAMPLE_COUNT_1_BIT,
                                    VK_IMAGE_TILING_OPTIMAL,
                                    usage,
                                    VK_SHARING_MODE_EXCLUSIVE,
                                    0,
                                    nullptr,
                                    VK_IMAGE_LAYOUT_UNDEFINED};
        snapshot.image = vk_.CreateImage(ici);
        if (!snapshot.image.image) {
            snapshot = {};
            return nullptr;
        }
        const VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                        nullptr,
                                        0,
                                        snapshot.image.image,
                                        VK_IMAGE_VIEW_TYPE_2D,
                                        snapshot.format,
                                        {},
                                        {snapshot.aspect, 0, 1, 0, 1}};
        if (vkCreateImageView(vk_.device, &vci, nullptr, &snapshot.view) != VK_SUCCESS) {
            vk_.Destroy(snapshot.image);
            snapshot = {};
            return nullptr;
        }
        snapshot.nvn_format = source.nvn_format;
    }

    EndRenderPass();
    Transition(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(snapshot, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkImageCopy region{{source.aspect, 0, 0, source.layers},
                             {0, 0, 0},
                             {snapshot.aspect, 0, 0, snapshot.layers},
                             {0, 0, 0},
                             {source.width, source.height, 1}};
    vkCmdCopyImage(Cmd(), source.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, snapshot.image.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    snapshot.rendered = true;
    snapshot.srgb_content = source.srgb_content;
    snapshot.draws = source.draws;
    Transition(snapshot, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return &snapshot;
}

void NvnRenderer::ReadPitch(const PitchSource& src, u8* dst) {
    for (u32 layer = 0; layer < src.layers; ++layer) {
        const GuestAddr base = src.cpu + u64{src.pitch} * src.rows * layer;
        for (u32 y = 0; y < src.rows; ++y) {
            g_memory.ReadBlock(base + u64{y} * src.pitch,
                               dst + (static_cast<size_t>(layer) * src.rows + y) * src.row_bytes, src.row_bytes);
        }
    }
}

void NvnRenderer::RefreshPitch(u64 key, HostImage& img, PitchSource& src) {
    HitchScope hitch_scope(kHitchRefresh);
    if (src.checked_frame == present_count_) return;
    src.checked_frame = present_count_;
    bool tracked_pass_complete = true, pitch_claimed_clean = false;
    if (dirty_tracker_) {
        dirty_tracker_->BeginFrame(present_count_);
        if (frame_profile_) ++win_pitch_calls_;
        const u64 span = u64{src.pitch} * src.rows * src.layers;
        if (g_memory.IsMapped(src.cpu, span) &&
            dirty_tracker_->CanSkip(src.dirty_stamp, g_memory.Ptr(src.cpu, span), span, g_memory.LayoutGeneration(),
                                    !g_memory.OverlapsFileMapping(src.cpu, span))) {
            if (frame_profile_) ++win_pitch_skipped_;
            if (!dirty_verify_) return;
            pitch_claimed_clean = true;
            ++dirty_verify_claims_;
        }
    }
    const auto refresh_start = frame_profile_ ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    const size_t row_bytes = src.row_bytes;
    for (u32 layer = 0; layer < src.layers; ++layer) {
        const GuestAddr base = src.cpu + u64{src.pitch} * src.rows * layer;
        if (!g_memory.IsMapped(base, u64{src.pitch} * src.rows)) {
            tracked_pass_complete = false;
            src.dirty_stamp = {};
            return;
        }
        u8* shadow = src.shadow.data() + static_cast<size_t>(layer) * src.rows * row_bytes;
        const auto row = [&](u32 y) {
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
            g_memory.ObserveRead(base + u64{y} * src.pitch, row_bytes);
#endif
            return g_memory.Ptr(base + u64{y} * src.pitch, row_bytes);
        };
        const auto same = [&](u32 y) { return std::memcmp(row(y), shadow + y * row_bytes, row_bytes) == 0; };
        u32 first = 0;
        while (first < src.rows && same(first)) ++first;
        if (first == src.rows) continue;
        if (pitch_claimed_clean) {
            ++dirty_verify_miss_;
            LOG_WARN("texture dirty VERIFY MISS (pitch): key=%llx addr=%llx frame=%llu",
                     static_cast<unsigned long long>(key), static_cast<unsigned long long>(src.cpu),
                     static_cast<unsigned long long>(present_count_));
        }
        u32 last = src.rows - 1;
        while (last > first && same(last)) --last;
        const u32 count = last - first + 1;
        std::vector<u8> band(static_cast<size_t>(count) * row_bytes);
        for (u32 y = first; y <= last; ++y) std::memcpy(band.data() + (y - first) * row_bytes, row(y), row_bytes);
        const u64 offset = Upload(band.data(), band.size(), 16);
        if (offset == ~u64{0}) {  // stream full: the shadow is unchanged, so the next frame retries
            tracked_pass_complete = false;
            break;
        }
        std::memcpy(shadow + first * row_bytes, band.data(), band.size());
        ++src.uploads;
        if (std::getenv("NXRT_TRACE_GLYPH") && src.uploads <= 16) {
            LOG_INFO("glyph-tex: reupload key=%llx addr=%llx format=%u %ux%u layer=%u rows=%u..%u frame=%llu",
                     static_cast<unsigned long long>(key), static_cast<unsigned long long>(src.cpu), src.format,
                     img.width, img.height, layer, first, last, static_cast<unsigned long long>(present_count_));
        }
        Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        // Rows are texel-block rows; for the uncompressed formats taken here a block is one texel.
        const VkBufferImageCopy copy{offset, 0, 0, {img.aspect, 0, layer, 1}, {0, static_cast<s32>(first), 0},
                                     {img.width, count, 1}};
        vkCmdCopyBufferToImage(Cmd(), stream_.buffer, img.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    Transition(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (dirty_tracker_) {
        // Every layer was compared (or re-uploaded and copied into the shadow) in the current epoch.
        const u64 span = u64{src.pitch} * src.rows * src.layers;
        if (tracked_pass_complete) {
            dirty_tracker_->MarkVerified(src.dirty_stamp, g_memory.LayoutGeneration(),
                                         !g_memory.OverlapsFileMapping(src.cpu, span));
        } else {
            src.dirty_stamp = {};
        }
    }
    if (frame_profile_) {
        frame_sampled_pitch_refresh_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - refresh_start)
                                               .count();
        ++frame_sampled_pitch_refresh_calls_;
    }
}

void NvnRenderer::RefreshSwizzled(u64 key, HostImage& img, SwizzledSource& src) {
    HitchScope hitch_scope(kHitchRefresh, key);
    if (src.checked_frame == present_count_) return;
    src.checked_frame = present_count_;
    if (!g_memory.IsMapped(src.cpu, src.bytes)) {
        src.dirty_stamp = {};
        return;
    }
    const u8* guest = g_memory.Ptr(src.cpu, src.bytes);
    if (frame_profile_) {
        ++win_swz_calls_;
        win_swz_bytes_ += src.bytes;
        if (src.bytes >= (size_t{1} << 20)) {
            ++win_swz_big_calls_;
            win_swz_big_bytes_ += src.bytes;
        }
    }
    bool trackable = false, claimed_clean = false;
    if (dirty_tracker_) {
        dirty_tracker_->BeginFrame(present_count_);
        trackable = !g_memory.OverlapsFileMapping(src.cpu, src.bytes);
        if (dirty_tracker_->CanSkip(src.dirty_stamp, guest, src.bytes, g_memory.LayoutGeneration(), trackable)) {
            if (!dirty_verify_) return;
            claimed_clean = true;
            ++dirty_verify_claims_;
        }
    }
    if (frame_profile_) {
        ++win_swz_hashed_;
        win_swz_hashed_bytes_ += src.bytes;
    }
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    g_memory.ObserveRead(src.cpu, src.bytes);
#endif
    const auto hash_start = frame_profile_ ? std::chrono::steady_clock::now()
                                           : std::chrono::steady_clock::time_point{};
    const size_t content_hash = SwizzledHash(guest, src.bytes);
    if (frame_profile_) {
        frame_sampled_swizzled_hash_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - hash_start)
                                               .count();
        frame_sampled_swizzled_hash_bytes_ += src.bytes;
        ++frame_sampled_swizzled_hash_calls_;
    }
    if (content_hash == src.content_hash) {
        if (dirty_tracker_) dirty_tracker_->MarkVerified(src.dirty_stamp, g_memory.LayoutGeneration(), trackable);
        return;
    }
    src.dirty_stamp = {};
    if (claimed_clean) {
        ++dirty_verify_miss_;
        LOG_WARN("texture dirty VERIFY MISS (swizzled): key=%llx addr=%llx bytes=%zu frame=%llu",
                 static_cast<unsigned long long>(key), static_cast<unsigned long long>(src.cpu), src.bytes,
                 static_cast<unsigned long long>(present_count_));
    }
    const size_t linear_layer = static_cast<size_t>(src.width_blocks) * src.height_blocks * src.bytes_per_block;
    std::vector<u8> linear(linear_layer * src.layers);
    if (src.is_3d) {
        UnswizzleBlockLinear3D(guest, src.layer_size, linear.data(), src.bytes_per_block, src.width_blocks,
                               src.height_blocks, src.layers, src.block_height, src.block_depth, src.tile_width);
    } else {
        for (u32 layer = 0; layer < src.layers; ++layer) {
            UnswizzleBlockLinear(guest + src.layer_size * layer, src.layer_size, linear.data() + linear_layer * layer,
                                 src.bytes_per_block, src.width_blocks, src.height_blocks, src.block_height,
                                 src.tile_width);
        }
    }
    std::vector<u8> decoded_rgba;
    if (src.astc) {
        std::string decode_error;
        if (!DecodeAstc(linear, src.pixel_width, src.pixel_height, src.astc_block_width, src.astc_block_height,
                        src.astc_srgb, decoded_rgba, decode_error)) {
            return;  // keep the previous image; the next frame retries
        }
    }
    const std::vector<u8>& upload = src.astc ? decoded_rgba : linear;
    const u64 offset = Upload(upload.data(), upload.size(), 16);
    if (offset == ~u64{0}) return;  // stream full: the shadow is unchanged, so the next frame retries
    src.content_hash = content_hash;
    if (dirty_tracker_) dirty_tracker_->MarkVerified(src.dirty_stamp, g_memory.LayoutGeneration(), trackable);
    ++src.uploads;
    if (std::getenv("NXRT_TRACE_SAMPLED_REFRESH") && src.uploads <= 16) {
        LOG_INFO("sampled-refresh: key=%llx addr=%llx format=%u %ux%ux%u 3d=%d upload=%llu frame=%llu",
                 static_cast<unsigned long long>(key), static_cast<unsigned long long>(src.cpu),
                 static_cast<unsigned>(img.format), img.width, img.height, src.layers, src.is_3d ? 1 : 0,
                 static_cast<unsigned long long>(src.uploads), static_cast<unsigned long long>(present_count_));
    }
    Transition(img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const VkBufferImageCopy copy{offset, 0, 0, {img.aspect, 0, 0, src.is_3d ? 1 : src.layers}, {0, 0, 0},
                                 {img.width, img.height, src.is_3d ? src.layers : 1}};
    vkCmdCopyBufferToImage(Cmd(), stream_.buffer, img.image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Transition(img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

VkSampler NvnRenderer::SamplerFor(u32 sampler_id, std::optional<VkCompareOp> compare_override,
                                  bool shader_depth, bool shader_integer, bool image_depth) {
    const auto tsc = ReadTsc(sampler_id);
    if (!tsc) return VK_NULL_HANDLE;
    const bool white_border = UseWhiteDepthBorder(tsc->w, shader_depth, shader_integer, image_depth);
    const u64 key = FastHash(&*tsc, sizeof(*tsc)) ^
                    (compare_override ? (u64{static_cast<u32>(*compare_override) + 1} << 56) : 0);
    auto& cache = samplers_[white_border ? 1 : 0];
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    const VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                 nullptr,
                                 0,
                                 Filter(tsc->MagFilter()),
                                 Filter(tsc->MinFilter()),
                                 MipmapMode(tsc->MipFilter()),
                                 WrapMode(tsc->WrapU()),
                                 WrapMode(tsc->WrapV()),
                                 WrapMode(tsc->WrapP()),
                                 tsc->LodBias(),
                                 VK_FALSE,
                                 1.0f,
                                 tsc->DepthCompare(),
                                 compare_override.value_or(SamplerCompareOp(tsc->DepthFunc())),
                                 tsc->MinLod(),
                                 tsc->MaxLod(),
                                 white_border ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                              : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
                                 VK_FALSE};
    VkSampler s;
    vkCreateSampler(vk_.device, &ci, nullptr, &s);
    cache[key] = s;
    return s;
}

// ------------------------------------------------------------------ draws

void NvnRenderer::Draw(const Command& c, u64 list_handle, int depth) {
    const WinSampleTraceConfig& win_trace = GetWinSampleTraceConfig();
    const bool win_frame_matches = !win_trace.frame || present_count_ + 1 == *win_trace.frame;
    const bool win_target_matches = win_frame_matches && win_trace.enabled && win_trace.valid &&
                                    num_color_targets_ > 0 &&
                                    (!win_trace.target_key || ImageKey(color_targets_[0]) == win_trace.target_key);
    const bool win_serial_matches = !win_trace.serial || bound_.program_serial == *win_trace.serial;
    const bool win_program_matches = !win_trace.program || bound_.program == *win_trace.program;
    const bool win_index_matches = !win_trace.index_count ||
                                   (c.op == Op::DrawElements && c.a[2] == *win_trace.index_count &&
                                    !selected_trace_signature_seen_);
    bool win_extent_matches = true;
    if (win_trace.target_width || win_trace.target_height) {
        const auto image = num_color_targets_ > 0 ? images_.find(ImageKey(color_targets_[0])) : images_.end();
        win_extent_matches = image != images_.end() &&
                             (!win_trace.target_width || image->second.width == *win_trace.target_width) &&
                             (!win_trace.target_height || image->second.height == *win_trace.target_height);
    }
    const bool trace_win_draw = win_target_matches && win_serial_matches && win_program_matches && win_frame_matches &&
                                win_index_matches && win_extent_matches;
    if (trace_win_draw && win_trace.index_count) selected_trace_signature_seen_ = true;
    if (trace_win_draw && (win_trace.frame || win_trace.index_count) && num_color_targets_ > 0) {
        selected_trace_target_key_ = ImageKey(color_targets_[0]);
        selected_trace_target_pending_ = true;
    }
    const bool trace_win_frame = trace_win_draw && (win_trace.frame.has_value() || win_trace.index_count.has_value());
    static u64 next_win_draw_id = 0;
    static u32 unframed_win_draw_traces = 0;
    const u64 win_draw_id = trace_win_draw ? ++next_win_draw_id : 0;
    u32 win_input_index = 0;
    u32 null_2d_bindings = 0;
    if (census_) {
        census_->GameDraw(present_count_, CensusProgramHash());
        if (const std::string summary = census_->TakeSummary(); !summary.empty()) LOG_INFO("%s", summary.c_str());
    }
    const DrawCensusConfig& census = GetDrawCensusConfig();
    static const u64 census_target = kEnvProbeDrawTarget ? std::strtoull(kEnvProbeDrawTarget, nullptr, 16) : 0;
    bool target_history = census_target && num_color_targets_ && ImageKey(color_targets_[0]) == census_target;
    if (const char* selected = kEnvProbeDrawTarget; selected && num_color_targets_) {
        unsigned width = 0, height = 0;
        if (std::sscanf(selected, "%ux%u", &width, &height) == 2) {
            if (auto* img = ImageFor(color_targets_[0], color_levels_[0]))
                target_history = img->width == width && img->height == height;
        }
    }
    const bool census_draw = (census.frame && present_count_ + 1 >= census.frame && present_count_ + 1 <= census.frame_end) || target_history;
    if (census_draw && census_seq_frame_ != present_count_ + 1) {
        census_seq_frame_ = present_count_ + 1;
        census_seq_ = 0;
    }
    const u64 census_seq = census_draw ? ++census_seq_ : 0;
    census_sampled_.clear();
    const auto census_log = [this, census_draw, census_seq, &c, list_handle](bool queued, const std::string& why) {
        if (!census_draw) return;
        const auto describe = [this](GuestAddr target, u32 level, u32 layer) {
            std::ostringstream text;
            if (!target) return std::string("none");
            const u64 image_key = ImageKey(target);
            const u64 host_key = LevelKey(image_key, level);
            const auto image = images_.find(host_key);
            text << "guest=0x" << std::hex << target << " image_key=0x" << image_key << " host_key=0x" << host_key
                 << std::dec << " level=" << level << " layer=" << layer;
            if (image != images_.end()) {
                const HostImage& host = image->second;
                text << " host=" << static_cast<const void*>(&host) << " format=" << static_cast<int>(host.format)
                     << " size=" << host.width << 'x' << host.height << " layers=" << host.layers
                     << " effective_layer=" << (layer < host.layers ? layer : 0);
            } else {
                text << " host=missing";
            }
            return text.str();
        };
        std::ostringstream event;
        event << "census q=" << present_count_ + 1 << " seq=" << census_seq << " outcome="
              << (queued ? "queued" : "skip");
        if (!why.empty()) event << " reason=" << why;
        event << " program=0x" << std::hex << bound_.program << std::dec << " serial=" << bound_.program_serial
              << " list=0x" << std::hex << list_handle << std::dec << " op=" << static_cast<int>(c.op)
              << " topo=" << c.a[0] << " count=" << c.a[2] << " inst="
              << (c.op == Op::DrawElements ? (c.words.empty() ? 1u : c.words[0]) : static_cast<u32>(c.a[4]))
              << " ncolor=" << num_color_targets_ << " c0="
              << (num_color_targets_ > 0 ? describe(color_targets_[0], color_levels_[0], color_layers_[0])
                                         : std::string("none"));
        for (u32 i = 1; i < num_color_targets_ && i < color_targets_.size(); ++i) {
            event << " c" << i << '{' << describe(color_targets_[i], color_levels_[i], color_layers_[i]) << '}';
        }
        event << " ds{" << describe(depth_target_, depth_level_, depth_layer_) << "} dsw=" << std::hex
              << bound_.depth_stencil[0] << ','
              << bound_.depth_stencil[1] << " poly=" << bound_.polygon << " mask=" << bound_.channel_mask
              << " blend0=" << bound_.blend_words[0][0] << ',' << bound_.blend_words[0][1] << std::dec
              << " vp=" << bound_.viewport[0] << ',' << bound_.viewport[1] << ',' << bound_.viewport[2] << ','
              << bound_.viewport[3] << " sc=" << bound_.scissor[0] << ',' << bound_.scissor[1] << ','
              << bound_.scissor[2] << ',' << bound_.scissor[3] << " dr=" << bound_.depth_range[0] << ','
              << bound_.depth_range[1]
              << " attrs=" << std::hex;
        for (u32 a = 0; a < bound_.attribs.size() && a < 32; ++a) {
            if ((bound_.attribs[a] >> 6) & 1) event << a << ':' << bound_.attribs[a] << ';';
        }
        event << " streams=";
        for (u32 st = 0; st < 4 && 2 * st + 1 < bound_.streams.size(); ++st) {
            event << bound_.streams[2 * st] << '/' << bound_.streams[2 * st + 1] << ';';
        }
        event << std::dec;
        LOG_INFO("%s", event.str().c_str());
        // Diagnostic (opt-in): NXRT_PROBE_CENSUS_VS_CBUF=<lo>-<hi> prints the first words of vertex-stage cbufs 0..4
        // (as hex and float) for census draws with sequence number in [lo, hi].
        if (const char* range = std::getenv("NXRT_PROBE_CENSUS_VS_CBUF"); range && *range) {
            // NXRT_PROBE_CENSUS_CBUF_STAGE: NVN stage to read (0 vertex, 4 fragment); default 0.
            static const size_t census_cbuf_stage =
                std::getenv("NXRT_PROBE_CENSUS_CBUF_STAGE") ? std::strtoul(std::getenv("NXRT_PROBE_CENSUS_CBUF_STAGE"), nullptr, 10) : 0;
            u64 lo = 0, hi = 0;
            if (std::sscanf(range, "%llu-%llu", reinterpret_cast<unsigned long long*>(&lo),
                            reinterpret_cast<unsigned long long*>(&hi)) == 2 &&
                census_seq >= lo && census_seq <= hi && [&] {
                    // NXRT_PROBE_CENSUS_CBUF_TARGET_W=<w>: only draws whose color target 0 is <w> pixels wide.
                    static const u32 want_w = std::getenv("NXRT_PROBE_CENSUS_CBUF_TARGET_W")
                        ? static_cast<u32>(std::strtoul(std::getenv("NXRT_PROBE_CENSUS_CBUF_TARGET_W"), nullptr, 10)) : 0;
                    if (!want_w) return true;
                    const auto desc = num_color_targets_ ? RenderModel().textures.find(color_targets_[0]) : RenderModel().textures.end();
                    return desc != RenderModel().textures.end() && desc->second.desc.width == want_w;
                }()) {
                const bool extended = std::getenv("NXRT_PROBE_CENSUS_VS_CBUF_EXTENDED") != nullptr;
                if (extended) {
                    if (const char* dir = std::getenv("NXRT_DUMP_DIR")) {
                        for (size_t vb = 0; vb < bound_.vertex_buffers.size(); ++vb) {
                            const auto [va, size] = bound_.vertex_buffers[vb];
                            const auto cpu = va ? GpuToCpu(va) : std::nullopt;
                            const auto length = std::min<u64>(size, 0x10000);
                            if (!cpu || !length || !g_memory.IsMapped(*cpu, length)) continue;
                            std::vector<u8> bytes(length);
                            g_memory.ReadBlock(*cpu, bytes.data(), bytes.size());
                            std::ostringstream name;
                            name << dir << "/census-" << present_count_ + 1 << "-" << census_seq << "-vb" << vb << ".bin";
                            std::ofstream out(name.str(), std::ios::binary);
                            out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                            LOG_INFO("census-vb q=%llu seq=%llu vb=%zu gpu=%llx size=%llu op=%u args=%llx,%llx,%llx,%llx,%llx,%llx word0=%x",
                                     static_cast<unsigned long long>(present_count_ + 1), static_cast<unsigned long long>(census_seq), vb,
                                     static_cast<unsigned long long>(va), static_cast<unsigned long long>(size), static_cast<unsigned>(c.op),
                                     static_cast<unsigned long long>(c.a[0]), static_cast<unsigned long long>(c.a[1]),
                                     static_cast<unsigned long long>(c.a[2]), static_cast<unsigned long long>(c.a[3]),
                                     static_cast<unsigned long long>(c.a[4]), static_cast<unsigned long long>(c.a[5]),
                                     c.words.empty() ? 0 : c.words[0]);
                        }
                        for (size_t stage = 0; stage < 2; ++stage) {
                            for (u32 cb = 0; cb < 18; ++cb) {
                                const auto [va, size] = bound_.uniform_buffers[stage][cb];
                                if (!va || !size) continue;
                                const auto cpu = GpuToCpu(va);
                                if (!cpu || !g_memory.IsMapped(*cpu, std::min<u64>(size, 0x10000))) continue;
                                std::vector<u8> bytes(std::min<u64>(size, 0x10000));
                                g_memory.ReadBlock(*cpu, bytes.data(), bytes.size());
                                std::ostringstream name;
                                name << dir << "/census-" << present_count_ + 1 << "-" << census_seq
                                     << "-s" << stage << "-c" << cb << ".bin";
                                std::ofstream out(name.str(), std::ios::binary);
                                out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                                LOG_INFO("census-cbuf-binding q=%llu seq=%llu stage=%zu cbuf=%u gpu=%llx cpu=%llx size=%llu",
                                         static_cast<unsigned long long>(present_count_ + 1),
                                         static_cast<unsigned long long>(census_seq), stage, cb,
                                         static_cast<unsigned long long>(va), static_cast<unsigned long long>(*cpu),
                                         static_cast<unsigned long long>(size));
                            }
                        }
                    }
                }
                for (u32 cb = 0; cb < (extended ? 18u : 5u); ++cb) {
                    std::ostringstream words;
                    for (u32 w = 0; w < (extended ? (cb == 3 ? 160u : 64u) : 16u); ++w) {
                        const u32 v = ReadCbuf(census_cbuf_stage, cb, w * 4);
                        float f;
                        std::memcpy(&f, &v, sizeof f);
                        char buf[40];
                        std::snprintf(buf, sizeof buf, " %08x(%g)", v, static_cast<double>(f));
                        words << buf;
                    }
                    LOG_INFO("census-vscbuf q=%llu seq=%llu stage=%zu cbuf=%u%s",
                             static_cast<unsigned long long>(present_count_ + 1),
                             static_cast<unsigned long long>(census_seq), census_cbuf_stage, cb, words.str().c_str());
                }
            }
        }
    };
    const auto append_win_trace = [this, trace_win_frame](std::string event) {
        if (!trace_win_frame) return;
        if (pending_win_sample_traces_.size() < kWinTraceCap) {
            pending_win_sample_traces_.push_back(std::move(event));
        } else {
            pending_win_sample_trace_capped_ = true;
        }
    };
    const auto emit_win_draw_result = [this, &append_win_trace, trace_win_frame, trace_win_draw, win_draw_id,
                                       list_handle, depth, &c, &win_input_index](bool queued,
                                                                                const std::string& reason) {
        if (!trace_win_draw) return;
        if (trace_win_frame && queued && c.op == Op::DrawElements && c.a[2] <= 8192) {
            const u32 index_type = static_cast<u32>(c.a[1]);
            const size_t index_size = index_type == 0 ? 1 : index_type == 1 ? 2 : 4;
            const size_t index_bytes = static_cast<size_t>(c.a[2]) * index_size;
            std::vector<u32> indices;
            std::vector<u32> vertices;
            if (const auto index_cpu = GpuToCpu(c.a[3]); index_cpu && g_memory.IsMapped(*index_cpu, index_bytes)) {
                std::vector<u8> raw(index_bytes);
                g_memory.ReadBlock(*index_cpu, raw.data(), raw.size());
                indices.reserve(c.a[2]);
                vertices.reserve(c.a[2]);
                const s64 base_vertex = static_cast<s64>(c.a[4]);
                for (u32 i = 0; i < c.a[2]; ++i) {
                    u32 index = 0;
                    std::memcpy(&index, raw.data() + static_cast<size_t>(i) * index_size, index_size);
                    indices.push_back(index);
                    const s64 vertex = static_cast<s64>(index) + base_vertex;
                    if (vertex < 0 || vertex > std::numeric_limits<u32>::max()) continue;
                    const u32 vertex_id = static_cast<u32>(vertex);
                    if (std::find(vertices.begin(), vertices.end(), vertex_id) == vertices.end()) {
                        vertices.push_back(vertex_id);
                    }
                }
            }
            {
                std::ostringstream layout;
                layout << "geometry draw_id=" << win_draw_id << " index_type=" << index_type
                       << " index_count=" << c.a[2] << " index_bytes=" << index_bytes << " base_vertex="
                       << static_cast<s64>(c.a[4]) << " unique_vertices=" << vertices.size() << " attrs=";
                // The ground vertex shader reads Generic0.xy. Generic1/3/7
                // are vertex-stage outputs consumed by later stages, so include
                // attribute 0 when tracing the input mesh.
                for (u32 attribute : {0u, 1u, 3u, 7u}) {
                    if (attribute >= bound_.attribs.size()) continue;
                    const u32 word = bound_.attribs[attribute];
                    if (!((word >> 6) & 1)) {
                        layout << attribute << ":disabled;";
                        continue;
                    }
                    const u32 decoded = Format(word >> 21).vertex_attribute | (word & 0x1fff9f);
                    const u32 stream = decoded & 0x1f;
                    layout << attribute << ":word=0x" << std::hex << word << ",decoded=0x" << decoded << std::dec
                           << ",stream=" << stream << ",offset=" << ((decoded >> 7) & 0x3fff) << ",size="
                           << ((decoded >> 21) & 0x3f) << ",type=" << ((decoded >> 27) & 7) << ",stride="
                           << (2 * stream < bound_.streams.size() ? bound_.streams[2 * stream] : 0) << ';';
                }
                append_win_trace(layout.str());
            }
            for (size_t begin = 0; begin < indices.size(); begin += 128) {
                std::ostringstream chunk;
                chunk << "indices draw_id=" << win_draw_id << " first=" << begin << " values=" << std::hex;
                const size_t end = std::min(indices.size(), begin + 128);
                for (size_t i = begin; i < end; ++i) chunk << (i == begin ? "" : ",") << indices[i];
                append_win_trace(chunk.str());
            }
            for (u32 attribute : {0u, 1u, 3u, 7u}) {
                if (attribute >= bound_.attribs.size()) continue;
                const u32 word = bound_.attribs[attribute];
                if (!((word >> 6) & 1)) continue;
                const u32 decoded = Format(word >> 21).vertex_attribute | (word & 0x1fff9f);
                const u32 stream = decoded & 0x1f;
                if (stream >= bound_.vertex_buffers.size()) continue;
                const u32 offset = (decoded >> 7) & 0x3fff;
                const u64 stride = 2 * stream < bound_.streams.size() ? bound_.streams[2 * stream] : 0;
                const auto [buffer_va, buffer_size] = bound_.vertex_buffers[stream];
                const auto buffer_cpu = buffer_va ? GpuToCpu(buffer_va) : std::nullopt;
                for (size_t begin = 0; begin < vertices.size(); begin += 16) {
                    std::ostringstream values;
                    values << "vertex_data draw_id=" << win_draw_id << " attr=" << attribute << " first=" << begin
                           << " values=";
                    const size_t end = std::min(vertices.size(), begin + 16);
                    for (size_t i = begin; i < end; ++i) {
                        if (i != begin) values << ';';
                        values << vertices[i] << '=';
                        if (!buffer_cpu || !stride) {
                            values << "unavailable";
                            continue;
                        }
                        const u64 byte_offset = static_cast<u64>(offset) + static_cast<u64>(vertices[i]) * stride;
                        if (byte_offset >= buffer_size) {
                            values << "out_of_range";
                            continue;
                        }
                        const size_t read_size = std::min<u64>(16, buffer_size - byte_offset);
                        const GuestAddr address = *buffer_cpu + byte_offset;
                        if (!read_size || !g_memory.IsMapped(address, read_size)) {
                            values << "unmapped";
                            continue;
                        }
                        std::array<u8, 16> data{};
                        g_memory.ReadBlock(address, data.data(), read_size);
                        values << std::hex << std::setfill('0');
                        for (size_t b = 0; b < read_size; ++b) values << std::setw(2) << static_cast<u32>(data[b]);
                        values << std::dec << std::setfill(' ');
                    }
                    append_win_trace(values.str());
                }
            }
        }
        std::ostringstream event;
        event << "draw_result id=" << win_draw_id << " target_key=0x" << std::hex << ImageKey(color_targets_[0])
              << " program=0x" << bound_.program << std::dec << " serial=" << bound_.program_serial << " list=0x"
              << std::hex << list_handle << std::dec << " depth=" << depth << " op=" << static_cast<int>(c.op)
              << " outcome=" << (queued ? "queued" : "skip");
        if (!reason.empty()) event << " reason=" << reason;
        if (queued) event << " inputs=" << win_input_index;
        // Fixed-function state that can hide a queued draw: viewport, scissor, write masks, blend, depth, culling.
        event << " count=" << c.a[2] << " vp=" << bound_.viewport[0] << ',' << bound_.viewport[1] << ','
              << bound_.viewport[2] << ',' << bound_.viewport[3] << " dr=" << bound_.depth_range[0] << ','
              << bound_.depth_range[1] << " sc=" << bound_.scissor[0] << ',' << bound_.scissor[1] << ','
              << bound_.scissor[2] << ',' << bound_.scissor[3] << std::hex << " mask=" << bound_.channel_mask
              << " color=" << bound_.color_word << " blend0=" << bound_.blend_words[0][0] << ','
              << bound_.blend_words[0][1] << " ds=" << bound_.depth_stencil[0] << ',' << bound_.depth_stencil[1]
              << " poly=" << bound_.polygon << std::dec << " ncolor=" << num_color_targets_
              << " depth_target=" << (depth_target_ ? 1 : 0);
        // Bound uniform buffers and their first words: a draw that runs but writes black often reads zeros here.
        for (size_t st = 0; trace_win_frame && st < bound_.uniform_buffers.size(); ++st) {
            for (u32 cb = 0; cb < bound_.uniform_buffers[st].size(); ++cb) {
                const auto [va, size] = bound_.uniform_buffers[st][cb];
                if (!va) continue;
                event << " ubo" << st << '.' << cb << "@0x" << std::hex << va << '+' << size << ':';
                for (u32 w = 0; w < 8; ++w) event << (w ? "," : "") << ReadCbuf(st, cb, 4 * w);
                event << std::dec;
            }
        }
        if (trace_win_frame) {
            append_win_trace(event.str());
        } else if (unframed_win_draw_traces++ < 24) {
            LOG_INFO("trace-win: q=%llu %s", static_cast<unsigned long long>(present_count_), event.str().c_str());
        }
    };

    // Focused trace for the six-index composition draws. The list handle is
    // propagated through CallCommands so the trace shows the state at the
    // actual draw, rather than only the state at list-recording time.
    if (c.op == Op::DrawElements && c.a[2] == 6 && kEnvTraceDraws) {
        bool matches = true;
        if (const char* filter = std::getenv("NXRT_TRACE_CMD")) {
            char* end = nullptr;
            const u64 wanted = std::strtoull(filter, &end, 16);
            matches = end != filter && *end == '\0' && wanted == list_handle;
        }
        static u32 emitted = 0;
        if (matches && emitted++ < 32) {
            std::ostringstream targets;
            targets << std::hex;
            for (u32 i = 0; i < num_color_targets_; ++i) {
                const GuestAddr target = color_targets_[i];
                const u64 key = ImageKey(target);
                bool window_alias = false;
                for (const auto& [window_handle, window] : RenderModel().windows) {
                    (void)window_handle;
                    for (GuestAddr window_texture : window.textures) {
                        if (ImageKey(window_texture) == key) window_alias = true;
                    }
                }
                targets << " [" << std::dec << i << "]=0x" << std::hex << target << "/key=0x" << key
                        << "/window_alias=" << std::dec << window_alias;
                if (auto desc = RenderModel().textures.find(target); desc != RenderModel().textures.end()) {
                    targets << "/" << desc->second.desc.width << "x" << desc->second.desc.height << "/fmt=0x"
                            << std::hex << desc->second.desc.format << "/serial=" << std::dec << desc->second.serial;
                }
            }
            LOG_INFO("trace: draw6 list=%llx depth=%d program=%llx serial=%llu targets=%u%s "
                     "viewport=%.0f,%.0f,%.0f,%.0f scissor=%u,%u,%u,%u index_type=%llu index_addr=%llx "
                     "count=%llu base_vertex=%llu instance=%llu topology=%llu polygon=%08x color=%08x mask=%08x "
                     "depth=%08x/%08x index_data=%s attribs=%s",
                     static_cast<unsigned long long>(list_handle), depth,
                     static_cast<unsigned long long>(bound_.program), static_cast<unsigned long long>(bound_.program_serial),
                     num_color_targets_, targets.str().c_str(), bound_.viewport[0], bound_.viewport[1],
                     bound_.viewport[2], bound_.viewport[3], bound_.scissor[0], bound_.scissor[1], bound_.scissor[2],
                     bound_.scissor[3], static_cast<unsigned long long>(c.a[1]),
                     static_cast<unsigned long long>(c.a[3]), static_cast<unsigned long long>(c.a[2]),
                     static_cast<unsigned long long>(c.a[4]),
                     static_cast<unsigned long long>(c.words.empty() ? 1 : std::max<u32>(1, c.words[0])),
                     static_cast<unsigned long long>(c.a[0]),
                     bound_.polygon, bound_.color_word, bound_.channel_mask, bound_.depth_stencil[0],
                     bound_.depth_stencil[1], [&] {
                         std::ostringstream values;
                         const u32 type = static_cast<u32>(c.a[1]);
                         const size_t width = type == 0 ? 1 : type == 1 ? 2 : 4;
                         if (const auto cpu = GpuToCpu(c.a[3]); cpu && g_memory.IsMapped(*cpu, width * 6)) {
                             std::array<u8, 24> data{};
                             g_memory.ReadBlock(*cpu, data.data(), width * 6);
                             values << std::hex;
                             for (size_t i = 0; i < 6; ++i) {
                                 u32 index = 0;
                                 std::memcpy(&index, data.data() + i * width, width);
                                 if (i) values << ',';
                                 values << index;
                             }
                         } else {
                             values << "unmapped";
                         }
                         return values.str();
                     }().c_str(), [&] {
                         std::ostringstream values;
                         values << std::hex;
                         for (u32 i = 0; i < bound_.attribs.size(); ++i) {
                             const u32 word = bound_.attribs[i];
                             if (!((word >> 6) & 1)) continue;
                             if (values.tellp() > 0) values << ',';
                             const u32 stream = word & 0x1f;
                             const u32 decoded = Format(word >> 21).vertex_attribute | (word & 0x1fff9f);
                             values << i << ":w=" << word << ":vb=" << bound_.vertex_buffers[stream].first << '/'
                                    << bound_.vertex_buffers[stream].second << ":decoded=" << decoded << ":size="
                                    << ((decoded >> 21) & 0x3f) << ":type=" << ((decoded >> 27) & 7) << ":offset="
                                    << ((decoded >> 7) & 0x3fff) << ":stride="
                                    << (2 * stream < bound_.streams.size() ? bound_.streams[2 * stream] : 0);
                             const auto cpu = GpuToCpu(bound_.vertex_buffers[stream].first);
                             const size_t bytes = std::min<u64>(bound_.vertex_buffers[stream].second, 32);
                             if (cpu && bytes && g_memory.IsMapped(*cpu, bytes)) {
                                 std::array<u8, 32> data{};
                                 g_memory.ReadBlock(*cpu, data.data(), bytes);
                                 values << ":data=";
                                 for (size_t j = 0; j < bytes; ++j) {
                                     if (j) values << '.';
                                     values << std::setw(2) << std::setfill('0') << static_cast<u32>(data[j]);
                                 }
                                 values << std::setfill(' ');
                             }
                         }
                         return values.str();
                     }().c_str());
            static u32 program_stage_traces = 0;
            if (program_stage_traces++ < 4) {
                if (auto program = RenderModel().programs.find(bound_.program); program != RenderModel().programs.end()) {
                    std::ostringstream stages;
                    stages << std::hex;
                    for (const auto& shader : program->second.stages) {
                        if (stages.tellp() > 0) stages << ',';
                        stages << "0x" << shader.code << "/0x" << shader.control;
                    }
                    LOG_INFO("trace: draw6-program program=%llx serial=%llu stages=%s",
                             static_cast<unsigned long long>(bound_.program),
                             static_cast<unsigned long long>(program->second.serial), stages.str().c_str());
                    std::array<u32, 8> vertex_uv{};
                    for (u32 component = 0; component < vertex_uv.size(); ++component) {
                        vertex_uv[component] = ReadCbuf(0, 3, 112 + component * 4);
                    }
                    LOG_INFO("trace: draw6-vertex-uv program=%llx cbuf3=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                             static_cast<unsigned long long>(bound_.program), vertex_uv[0], vertex_uv[1],
                             vertex_uv[2], vertex_uv[3], vertex_uv[4], vertex_uv[5], vertex_uv[6], vertex_uv[7]);
                    std::ostringstream vertex_position;
                    vertex_position << std::hex;
                    for (u32 component = 0; component < 36; ++component) {
                        if (component) vertex_position << ',';
                        vertex_position << ReadCbuf(0, 3, component * 4);
                    }
                    LOG_INFO("trace: draw6-vertex-cbuf0-140 program=%llx cbuf3=%s",
                             static_cast<unsigned long long>(bound_.program), vertex_position.str().c_str());
                    std::ostringstream vertex_transform;
                    vertex_transform << std::hex;
                    for (u32 offset = 528; offset <= 540; offset += 4) {
                        if (offset != 528) vertex_transform << ',';
                        vertex_transform << ReadCbuf(0, 3, offset);
                    }
                    LOG_INFO("trace: draw6-vertex-cbuf528-540 program=%llx cbuf3=%s",
                             static_cast<unsigned long long>(bound_.program), vertex_transform.str().c_str());
                }
            }
        }
    }
    const auto skip = [this, &emit_win_draw_result, &census_log](const std::string& why) {
        emit_win_draw_result(false, why);
        census_log(false, why);
        CensusDrop(why);
        ++skipped_;
        ++skip_reasons_[why];
    };
    if (!render_enabled_) return skip("render-disabled");
    if (!ReserveStream()) return skip("stream-flush-failed");
    PollAsync();
    if (num_color_targets_ == 0 && !depth_target_) return skip("no-targets");
    std::vector<HostImage*>& colors = draw_colors_;
    colors.clear();
    for (u32 i = 0; i < num_color_targets_; ++i) {
        // A null NVNtexture in a color slot leaves that slot unbound (the G-buffer pass binds slots 0, 3 and 5 only).
        if (!color_targets_[i] && null_targets_) {
            colors.push_back(nullptr);
            continue;
        }
        HostImage* img = ImageFor(color_targets_[i], color_levels_[i]);
        if (!img) {
            if (std::getenv("NXRT_TRACE_TARGET_FAILURES")) {
                const auto t = RenderModel().textures.find(color_targets_[i]);
                if (t != RenderModel().textures.end()) {
                    const auto& d = t->second.desc;
                    const auto pool = RenderModel().pools.find(d.pool);
                    LOG_INFO("target-failure q=%llu program=%llx tex=%llx nvn=%u view=%u size=%ux%u stride=%u pool=%llx offset=%llx storage=%llx key=%llx viewvk=%u",
                             (unsigned long long)(present_count_+1), (unsigned long long)bound_.program,
                             (unsigned long long)color_targets_[i], d.format, color_view_formats_[i],
                             d.width, d.height, d.stride, (unsigned long long)d.pool, (unsigned long long)d.offset,
                             (unsigned long long)(pool == RenderModel().pools.end() ? 0 : pool->second.storage),
                             (unsigned long long)ImageKey(color_targets_[i]), [&] {
                                 Tic view_tic{}; view_tic.w[0] = Format(color_view_formats_[i]).tic_word0;
                                 return static_cast<u32>(TicFormat(view_tic).format);
                             }());
                }
            }
            return skip("target");
        }
        if (img->block_texel_target) {
            // ImageFor's memo can outlive the binding of a different view.
            // Only the observed single UINT block attachment is supported.
            Tic view_tic{};
            view_tic.w[0] = Format(color_view_formats_[i]).tic_word0;
            if (num_color_targets_ != 1 || !color_view_formats_[i] ||
                TicFormat(view_tic).format != VK_FORMAT_R32G32B32A32_UINT ||
                bound_.viewport[0] != 0 || bound_.viewport[1] != 0 ||
                bound_.viewport[2] != img->width || bound_.viewport[3] != img->height ||
                ((bound_.color_word >> (8+i)) & 1)) return skip("block-target:raster-contract");
            if (std::getenv("NXRT_TRACE_TARGET_FAILURES"))
                LOG_INFO("block-target q=%llu level=%u host=%ux%u vp=%.0fx%.0f",
                         (unsigned long long)(present_count_+1), color_levels_[i], img->width, img->height,
                         bound_.viewport[2], bound_.viewport[3]);
        }
        colors.push_back(img);
    }
    HostImage* ds = depth_target_ ? ImageFor(depth_target_, depth_level_) : nullptr;
    const u32 topology = static_cast<u32>(c.a[0]);
    if (topology == 7 || topology == 8) return skip("quads");
    const auto pipeline_start = frame_profile_ ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    Pipeline* p;
    {
        HitchScope draw_pipe_scope(kHitchDrawPipe);
        p = GetPipeline(topology);
    }
    if (frame_profile_) {
        frame_draw_pipeline_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - pipeline_start)
                                       .count();
    }
    if (!p->ok) return skip("pipeline:" + p->error);

    // Descriptors, in the order the SPIR-V emitter numbered them. Uploads and
    // layout transitions happen before the render pass begins.
    const auto descriptors_start = frame_profile_ ? std::chrono::steady_clock::now()
                                                   : std::chrono::steady_clock::time_point{};
    size_t buffer_info_capacity = 0;
    size_t image_info_capacity = 0;
    for (const Shader::Info* info : p->infos) {
        if (info) AccumulateDescriptorInfoCapacities(*info, buffer_info_capacity, image_info_capacity);
    }
    auto& writes = draw_descriptor_writes_;
    writes.clear();
    writes.reserve(buffer_info_capacity + image_info_capacity);
    auto& buffer_infos = draw_buffer_infos_;
    buffer_infos.Clear();
    buffer_infos.Reserve(buffer_info_capacity);
    auto& image_infos = draw_image_infos_;
    image_infos.Clear();
    image_infos.Reserve(image_info_capacity);
    const size_t ubo_align = vk_.properties.limits.minUniformBufferOffsetAlignment;
    const size_t ssbo_align = vk_.properties.limits.minStorageBufferOffsetAlignment;
    u32 binding = 0;
    auto& scratch = draw_descriptor_scratch_;
    const auto is_guest_range_mapped = [this](GuestAddr address, size_t size) {
        const auto check_start = frame_profile_ ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
        const bool mapped = g_memory.IsMapped(address, size);
        if (frame_profile_) {
            frame_map_check_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - check_start)
                                       .count();
        }
        return mapped;
    };
    static u32 win_ubo_traces = 0;
    static u32 win_traces = 0;
    const bool win_ubo = trace_win_draw && !win_trace.frame && win_ubo_traces < 8;
    if (win_ubo) ++win_ubo_traces;
    for (size_t ps = 0; ps < kStages; ++ps) {
        const Shader::Info* info = p->infos[ps];
        if (!info) continue;
        const size_t s = kNvnStageOfPipelineStage[ps];
        for (const auto& desc : info->constant_buffer_descriptors) {
            for (u32 k = 0; k < desc.count; ++k) {
                const u32 slot = desc.index + k;
                scratch.assign(256, 0);
                GuestAddr ubo_key = 0;
                bool direct_hit = false, direct_tried = false;
                u64 direct_offset = 0;
                size_t direct_size = 0;
                if (slot == kTextureBoundBuffer) {
                    scratch.assign(0x20 + 8 * 32, 0);
                    std::memcpy(scratch.data() + 0x20, bound_.textures[s].data(), 8 * 32);
                } else if (slot < 18) {
                    const auto [va, size] = bound_.uniform_buffers[s][slot];
                    const auto cpu = va ? GpuToCpu(va) : std::nullopt;
                    if (cpu && size && ubo_direct_ && ubo_dedup_ && !win_ubo) {
                        // Direct mode: compare the guest bytes with the stored snapshot in place; only a miss
                        // is copied (the Guard call is the one ReadBlock makes, so pending GPU results still land first).
                        ubo_key = *cpu;
                        const size_t n = std::min<u64>(size, 0x10000);
                        if (is_guest_range_mapped(*cpu, n)) {
                            const auto read_start = frame_profile_ ? std::chrono::steady_clock::now()
                                                                  : std::chrono::steady_clock::time_point{};
                            direct_tried = true;
                            g_memory.Guard(*cpu, n, false);
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
                            g_memory.ObserveRead(*cpu, n);
#endif
                            const u8* guest_bytes = g_memory.RawPtr(*cpu);
                            if (const auto hit = frame_ubo_uploads_.Find(*cpu, std::span<const u8>(guest_bytes, n),
                                                                         stream_offset_)) {
                                direct_hit = true;
                                direct_offset = *hit;
                                direct_size = n;
                            } else {
                                scratch.resize(n);
                                std::memcpy(scratch.data(), guest_bytes, n);
                            }
                            if (frame_profile_) {
                                frame_uniform_read_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                              std::chrono::steady_clock::now() - read_start)
                                                              .count();
                            }
                        } else {
                            scratch.assign(n, 0);
                        }
                    } else if (cpu && size) {
                        ubo_key = *cpu;
                        scratch.resize(std::min<u64>(size, 0x10000));
                        if (is_guest_range_mapped(*cpu, scratch.size())) {
                            const auto read_start = frame_profile_ ? std::chrono::steady_clock::now()
                                                                  : std::chrono::steady_clock::time_point{};
                            g_memory.ReadBlock(*cpu, scratch.data(), scratch.size());
                            if (frame_profile_) {
                                frame_uniform_read_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                              std::chrono::steady_clock::now() - read_start)
                                                              .count();
                            }
                        } else {
                            std::fill(scratch.begin(), scratch.end(), u8{0});
                        }
                    }
                }
                if (win_ubo && slot < 18) {
                    const auto [wva, wsize] = bound_.uniform_buffers[s][slot];
                    const u32* w = reinterpret_cast<const u32*>(scratch.data());
                    LOG_INFO("trace-win: ubo ps=%zu nvn_stage=%zu slot=%u va=%llx size=%llu mapped=%d "
                             "%08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                             ps, s, slot, static_cast<unsigned long long>(wva), static_cast<unsigned long long>(wsize),
                             wva && GpuToCpu(wva) ? 1 : 0, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9],
                             w[10], w[11], w[12], w[13], w[14], w[15]);
                    const u32* x = reinterpret_cast<const u32*>(scratch.data());
                    if (scratch.size() >= 112) {
                        LOG_INFO("trace-win: ubo+64: %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
                                 x[16], x[17], x[18], x[19], x[20], x[21], x[22], x[23], x[24], x[25], x[26], x[27]);
                    }
                }
                u64 offset = ~u64{0};
                if (direct_hit) {
                    offset = direct_offset;
                    ++win_ubo_hits_;
                    win_ubo_hit_bytes_ += direct_size;
                } else if (ubo_dedup_) {
                    // The key only picks the candidate; the byte comparison decides. Bindings without guest memory
                    // (the texture handle table, unbound slots) get a synthetic key per stage and slot.
                    const GuestAddr key_addr = ubo_key ? ubo_key : (u64{0xFFFF} << 48) | (u64{s} << 8) | slot;
                    const std::optional<u64> hit = direct_tried ? std::nullopt
                                                                : frame_ubo_uploads_.Find(key_addr, scratch, stream_offset_);
                    if (hit) {
                        offset = *hit;
                        ++win_ubo_hits_;
                        win_ubo_hit_bytes_ += scratch.size();
                    } else {
                        ++win_ubo_misses_;
                        offset = Upload(scratch.data(), scratch.size(), ubo_align);
                        if (offset != ~u64{0}) frame_ubo_uploads_.Store(key_addr, scratch, offset);
                    }
                } else {
                    offset = Upload(scratch.data(), scratch.size(), ubo_align);
                }
                if (offset == ~u64{0}) return skip("stream-full");
                const auto* buffer_info = buffer_infos.Push({stream_.buffer, offset, direct_hit ? direct_size : scratch.size()});
                writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, binding, k, 1,
                                  VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, buffer_info, nullptr});
            }
            ++binding;
        }
        for (const auto& desc : info->storage_buffers_descriptors) {
            for (u32 k = 0; k < desc.count; ++k) {
                const u32 off = desc.cbuf_offset + k * 16;
                const GpuAddr va = Pack(ReadCbuf(s, desc.cbuf_index, off), ReadCbuf(s, desc.cbuf_index, off + 4));
                const u32 size = ReadCbuf(s, desc.cbuf_index, off + 8);
                const auto cpu = va ? GpuToCpu(va) : std::nullopt;
                scratch.assign(std::max<size_t>(16, std::min<size_t>(size, 8 << 20)), 0);
                if (cpu && is_guest_range_mapped(*cpu, std::min<size_t>(size, scratch.size()))) {
                    const auto read_start = frame_profile_ ? std::chrono::steady_clock::now()
                                                           : std::chrono::steady_clock::time_point{};
                    g_memory.ReadBlock(*cpu, scratch.data(), std::min<size_t>(size, scratch.size()));
                    if (frame_profile_) {
                        frame_storage_read_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                      std::chrono::steady_clock::now() - read_start)
                                                      .count();
                    }
                }
                if (trace_win_draw && (win_trace.frame || win_traces < 24)) {
                    const size_t nonzero =
                        scratch.size() - static_cast<size_t>(std::count(scratch.begin(), scratch.end(), u8{0}));
                    std::ostringstream event;
                    event << "ssbo draw_id=" << win_draw_id << " nvn_stage=" << s << " slot=" << k << " va=0x"
                          << std::hex << va << std::dec << " size=" << size << " mapped=" << (cpu ? 1 : 0)
                          << " written=" << (desc.is_written ? 1 : 0) << " nonzero=" << nonzero << "/"
                          << scratch.size();
                    if (trace_win_frame) append_win_trace(event.str());
                    else LOG_INFO("trace-win: %s", event.str().c_str());
                }
                const u64 offset = Upload(scratch.data(), scratch.size(), ssbo_align);
                if (offset == ~u64{0}) return skip("stream-full");
                const auto* buffer_info = buffer_infos.Push({stream_.buffer, offset, scratch.size()});
                writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, binding, k, 1,
                                  VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, buffer_info, nullptr});
            }
            ++binding;
        }
        if (!info->texture_buffer_descriptors.empty() || !info->image_buffer_descriptors.empty() ||
            !info->image_descriptors.empty()) {
            return skip("buffer-or-image-descriptors");
        }
        for (const auto& desc : info->texture_descriptors) {
            for (u32 k = 0; k < desc.count; ++k) {
                u32 handle = ReadCbuf(s, desc.cbuf_index, desc.cbuf_offset + (k << desc.size_shift));
                handle <<= desc.shift_left;
                if (desc.has_secondary) {
                    handle |= ReadCbuf(s, desc.secondary_cbuf_index, desc.secondary_cbuf_offset)
                              << desc.secondary_shift_left;
                }
                const auto texture_resolve_start = frame_profile_ ? std::chrono::steady_clock::now()
                                                                  : std::chrono::steady_clock::time_point{};
                const u32 texture_id = handle & 0xFFFFF;
                const u32 sampler_id = (handle >> 20) & 0xFFF;
                const bool trace_win_sample = trace_win_draw && (win_trace.frame || win_traces < 24);
                const bool force_shadow_pass = trace_win_draw && win_input_index == 6 && sampler_id == 0x1df &&
                                               std::getenv("NXRT_PROBE_SHADOW_ALWAYS");
                const bool probe_floor_d32_y_mirror =
                    kEnvProbeFloorD32Mirror && trace_win_draw && win_trace.frame == 11000 &&
                    win_trace.target_key == 0xe78413a906da49ccULL && win_trace.program == 0x20a3aae188ULL &&
                    win_trace.index_count == 1350 && c.op == Op::DrawElements && c.a[2] == 1350 &&
                    win_input_index == 1 && sampler_id == 0x1b9;
                std::optional<Tic> sampled_tic;
                HostImage* img = SampledImage(texture_id, handle, trace_win_sample, win_input_index, &sampled_tic);
                if (img) img->last_use_frame = present_count_;
                if (img && trace_win_sample && std::getenv("NXRT_DUMP_DEPTH_SAMPLES") &&
                    (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT)) {
                    const VkImageLayout restore_layout = img->layout;
                    Transition(*img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                    char suffix[40];
                    std::snprintf(suffix, sizeof suffix, "depth-sample-%x", texture_id);
                    DumpImage(*img, present_count_ + 1, suffix);
                    if (broken_) return skip("depth-sample-dump");
                    Transition(*img, restore_layout);
                }
                if (probe_floor_d32_y_mirror) {
                    if (!img || img->width != 1920 || img->height != 1080 ||
                        img->format != VK_FORMAT_D32_SFLOAT) {
                        LOG_ERROR("probe floor D32 mirror did not match source: q=%llu draw=%llu input=%u texture=%x "
                                  "format=%u size=%ux%u",
                                  static_cast<unsigned long long>(present_count_),
                                  static_cast<unsigned long long>(win_draw_id), win_input_index, texture_id,
                                  img ? static_cast<unsigned>(img->format) : 0u, img ? img->width : 0u,
                                  img ? img->height : 0u);
                        return skip("floor-d32-mirror-source");
                    }
                    img = MirrorFloorD32Sample(*img, win_draw_id);
                    if (!img) return skip("floor-d32-mirror");
                }
                const std::optional<Tsc> trace_tsc = trace_win_sample ? ReadTsc(sampler_id) : std::nullopt;
                VkSampler sampler = SamplerFor(
                    sampler_id, force_shadow_pass ? std::optional<VkCompareOp>{VK_COMPARE_OP_ALWAYS} : std::nullopt,
                    desc.is_depth, desc.is_integer, img && (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT));
                if (trace_win_sample) {
                    if (trace_tsc) {
                        LOG_INFO("trace-win-sampler q=%llu draw=%llu input=%u id=%x valid=%u "
                                 "wrap=%u,%u,%u compare_enable=%u compare_op=%u force_shadow_always=%u "
                                 "filter_mag=%u filter_min=%u "
                                 "mipmap=%u lod_bias=%.6f lod=%.6f..%.6f aniso_log2=%u "
                                 "vk_aniso_enable=0 vk_max_aniso=1",
                                 static_cast<unsigned long long>(present_count_),
                                 static_cast<unsigned long long>(win_draw_id), win_input_index, sampler_id,
                                 sampler ? 1u : 0u, static_cast<unsigned>(WrapMode(trace_tsc->WrapU())),
                                 static_cast<unsigned>(WrapMode(trace_tsc->WrapV())),
                                 static_cast<unsigned>(WrapMode(trace_tsc->WrapP())),
                                 trace_tsc->DepthCompare() ? 1u : 0u,
                                 static_cast<unsigned>(SamplerCompareOp(trace_tsc->DepthFunc())),
                                 force_shadow_pass ? 1u : 0u,
                                 static_cast<unsigned>(Filter(trace_tsc->MagFilter())),
                                 static_cast<unsigned>(Filter(trace_tsc->MinFilter())),
                                 static_cast<unsigned>(MipmapMode(trace_tsc->MipFilter())),
                                 trace_tsc->LodBias(), trace_tsc->MinLod(), trace_tsc->MaxLod(),
                                 trace_tsc->MaxAnisotropyLog2());
                    } else {
                        LOG_INFO("trace-win-sampler q=%llu draw=%llu input=%u id=%x tsc=unmapped valid=%u",
                                 static_cast<unsigned long long>(present_count_),
                                 static_cast<unsigned long long>(win_draw_id), win_input_index, sampler_id,
                                 sampler ? 1u : 0u);
                    }
                }
                std::optional<Tic> trace_tic;
                if (trace_win_sample) trace_tic = ReadTic(texture_id);
                // Current guest bytes behind the TIC: an all-zero texture that the renderer never drew into
                // was produced by GPU work the renderer does not replay (compute, copies), not by the CPU.
                std::string trace_guest = "none";
                if (trace_tic) {
                    const TextureFormat tf = TicFormat(*trace_tic);
                    const auto cpu = GpuToCpu(trace_tic->Address());
                    if (tf.format != VK_FORMAT_UNDEFINED && cpu) {
                        const u32 wb = (trace_tic->Width() + tf.block_width - 1) / tf.block_width;
                        const u32 hb = (trace_tic->Height() + tf.block_height - 1) / tf.block_height;
                        const size_t size = trace_tic->IsBlockLinear()
                                                ? BlockLinearSize(tf.bytes_per_block, wb, hb, trace_tic->BlockHeight(),
                                                                  trace_tic->TileWidthSpacing())
                                                : size_t{trace_tic->Pitch()} * hb;
                        if (g_memory.IsMapped(*cpu, size)) {
                            const u8* bytes = g_memory.Ptr(*cpu, size);
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
                            g_memory.ObserveRead(*cpu, size);
#endif
                            const size_t nonzero = size - static_cast<size_t>(std::count(bytes, bytes + size, u8{0}));
                            std::ostringstream guest;
                            guest << std::hex << "0x" << *cpu << std::dec << ":" << nonzero << "/" << size;
                            trace_guest = guest.str();
                        } else {
                            trace_guest = "unmapped";
                        }
                    }
                }
                if (frame_profile_) {
                    frame_draw_texture_resolve_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                          std::chrono::steady_clock::now() - texture_resolve_start)
                                                          .count();
                }
                if (img && img == glyph_image_ && std::getenv("NXRT_TRACE_GLYPH")) {
                    static u64 glyph_draws = 0;
                    if (const u64 n = glyph_draws++; n < 8 || (n < 4096 && n % 256 == 0)) {
                        u32 cb3[16]{};
                        for (u32 w = 0; w < 16; ++w) cb3[w] = ReadCbuf(s, 3, w * 4);
                        LOG_INFO("glyph-draw: #%llu stage=%zu program=%llx targets=%u rt0=%llx color_word=%08x "
                                 "channel_mask=%08x blend0=%08x,%08x set=%d polygon=%08x ds=%08x,%08x depth_rt=%llx "
                                 "op=%d a=%llx,%llx,%llx,%llx",
                                 static_cast<unsigned long long>(n), ps, static_cast<unsigned long long>(bound_.program),
                                 num_color_targets_,
                                 static_cast<unsigned long long>(num_color_targets_ ? ImageKey(color_targets_[0]) : 0),
                                 bound_.color_word, bound_.channel_mask, bound_.blend_words[0][0],
                                 bound_.blend_words[0][1], bound_.blend_set[0] ? 1 : 0, bound_.polygon,
                                 bound_.depth_stencil[0], bound_.depth_stencil[1],
                                 static_cast<unsigned long long>(depth_target_ ? ImageKey(depth_target_) : 0),
                                 static_cast<int>(c.op), static_cast<unsigned long long>(c.a[0]),
                                 static_cast<unsigned long long>(c.a[1]), static_cast<unsigned long long>(c.a[2]),
                                 static_cast<unsigned long long>(c.a[3]));
                        LOG_INFO("glyph-draw: #%llu cb3 %08x %08x %08x %08x | %08x %08x %08x %08x | %08x %08x %08x "
                                 "%08x | %08x %08x %08x %08x",
                                 static_cast<unsigned long long>(n), cb3[0], cb3[1], cb3[2], cb3[3], cb3[4], cb3[5],
                                 cb3[6], cb3[7], cb3[8], cb3[9], cb3[10], cb3[11], cb3[12], cb3[13], cb3[14], cb3[15]);
                        std::string attribs;
                        for (u32 w : bound_.attribs) attribs += " " + std::to_string(w);
                        LOG_INFO("glyph-draw: #%llu attribs%s instances=%u base_vertex=%lld base_instance=%llu "
                                 "ssbo_base=%u vs_c0[272]=%08x,%08x,%08x c2[272]=%08x,%08x,%08x",
                                 static_cast<unsigned long long>(n), attribs.c_str(),
                                 c.words.empty() ? 1u : c.words[0], static_cast<long long>(c.a[4]),
                                 static_cast<unsigned long long>(c.a[5]), StorageBase(0), ReadCbuf(0, 0, 272),
                                 ReadCbuf(0, 0, 276), ReadCbuf(0, 0, 280), ReadCbuf(0, 2, 272), ReadCbuf(0, 2, 276), ReadCbuf(0, 2, 280));
                        for (u32 slot = 0; slot < 16; ++slot) {
                            const auto [sva, ssize] = bound_.storage_buffers[0][slot];
                            if (sva) {
                                LOG_INFO("glyph-draw: #%llu vs ssbo[%u] va=%llx size=%llu",
                                         static_cast<unsigned long long>(n), slot,
                                         static_cast<unsigned long long>(sva), static_cast<unsigned long long>(ssize));
                            }
                        }
                        for (u32 vb = 0; vb < 3; ++vb) {
                            const auto [va, size] = bound_.vertex_buffers[vb];
                            const auto vcpu = va ? GpuToCpu(va) : std::nullopt;
                            u32 w[16]{};
                            if (vcpu && g_memory.IsMapped(*vcpu, 64)) g_memory.ReadBlock(*vcpu, w, 64);
                            LOG_INFO("glyph-draw: #%llu vb%u va=%llx size=%llu %08x %08x %08x %08x | %08x %08x %08x "
                                     "%08x | %08x %08x %08x %08x | %08x %08x %08x %08x",
                                     static_cast<unsigned long long>(n), vb, static_cast<unsigned long long>(va),
                                     static_cast<unsigned long long>(size), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                                     w[7], w[8], w[9], w[10], w[11], w[12], w[13], w[14], w[15]);
                        }
                    }
                }
                // Diagnostic (opt-in): NXRT_TRACE_SAMPLED_FRAME=<frame> logs the sampled textures of every draw in that
                // frame (same numbering as NXRT_PROBE_DRAW_CENSUS), regardless of index count.
                // "<frame>" or "<first>:<last>" (inclusive range).
                static const std::pair<u64, u64> sampled_trace_range = [] {
                    const char* e = std::getenv("NXRT_TRACE_SAMPLED_FRAME");
                    if (!e || !*e) return std::pair<u64, u64>{0, 0};
                    char* end = nullptr;
                    const u64 first = std::strtoull(e, &end, 0);
                    const u64 last = (end && *end == ':') ? std::strtoull(end + 1, nullptr, 0) : first;
                    return std::pair<u64, u64>{first, last};
                }();
                const bool sampled_frame_match = sampled_trace_range.first &&
                                                 present_count_ + 1 >= sampled_trace_range.first &&
                                                 present_count_ + 1 <= sampled_trace_range.second;
                if ((kEnvTraceDraws && c.a[2] == 6) || sampled_frame_match || target_history) {
                    static u32 texture_traces = 0;
                    if (texture_traces++ < ((sampled_frame_match || target_history) ? 65536u : 48u)) {
                        std::array<u32, 8> color_transform{};
                        for (u32 component = 0; component < color_transform.size(); ++component) {
                            color_transform[component] = ReadCbuf(s, 3, component * 4);
                        }
                        std::array<u32, 8> texture_coordinates{};
                        for (u32 component = 0; component < texture_coordinates.size(); ++component) {
                            texture_coordinates[component] = ReadCbuf(s, 3, 112 + component * 4);
                        }
                        if ((sampled_frame_match || target_history) && img) {
                            for (const auto& [image_key, image] : images_) {
                                if (&image == img) {
                                    LOG_INFO("sampled-target-key program=%llx texture=%x key=%llx",
                                             static_cast<unsigned long long>(bound_.program), handle & 0xfffff,
                                             static_cast<unsigned long long>(image_key));
                                    break;
                                }
                            }
                        }
                        LOG_INFO("trace: sampled frame=%llu program=%llx serial=%llu stage=%zu cbuf=%u offset=%x slot=%u "
                                 "handle=%llx texture=%x sampler=%x pool=%llx image=%p format=%u size=%ux%u "
                                 "rendered=%d draws=%llu color_xform=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x "
                                 "uv_xform=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x error=%s tgt=%llx",
                                 static_cast<unsigned long long>(present_count_ + 1),
                                 static_cast<unsigned long long>(bound_.program),
                                 static_cast<unsigned long long>(bound_.program_serial), ps, desc.cbuf_index,
                                 desc.cbuf_offset, k, static_cast<unsigned long long>(handle), handle & 0xFFFFF,
                                 handle >> 20, static_cast<unsigned long long>(bound_.texture_pool),
                                 static_cast<void*>(img), img ? static_cast<unsigned>(img->format) : 0,
                                 img ? img->width : 0, img ? img->height : 0, img ? img->rendered : false,
                                 static_cast<unsigned long long>(img ? img->draws : 0), color_transform[0],
                                 color_transform[1], color_transform[2], color_transform[3], color_transform[4],
                                 color_transform[5], color_transform[6], color_transform[7],
                                 texture_coordinates[0], texture_coordinates[1], texture_coordinates[2],
                                 texture_coordinates[3], texture_coordinates[4], texture_coordinates[5],
                                 texture_coordinates[6], texture_coordinates[7],
                                 img ? "ok" : texture_error_.c_str(),
                                 static_cast<unsigned long long>(ImageKey(color_targets_[0])));
                        if (const auto extra_tic = ReadTic(handle & 0xFFFFF)) {
                            const auto extra_tsc = ReadTsc((handle >> 20) & 0xFFF);
                            LOG_INFO("trace: sampled-extra program=%llx texture=%x tic_format=%u swizzle=%u,%u,%u,%u srgb=%d "
                                     "wrap=%u,%u filt=%u,%u cbuf3[128]=%08x cbuf3[132]=%08x",
                                     static_cast<unsigned long long>(bound_.program), handle & 0xFFFFF,
                                     extra_tic->Format(), extra_tic->Swizzle(0), extra_tic->Swizzle(1),
                                     extra_tic->Swizzle(2), extra_tic->Swizzle(3), extra_tic->Srgb() ? 1 : 0,
                                     extra_tsc ? extra_tsc->WrapU() : 99u, extra_tsc ? extra_tsc->WrapV() : 99u,
                                     extra_tsc ? extra_tsc->MagFilter() : 99u, extra_tsc ? extra_tsc->MinFilter() : 99u,
                                     ReadCbuf(s, 3, 128), ReadCbuf(s, 3, 132));
                        }
                    }
                }
                if (trace_win_draw) {
                    if (trace_win_frame) {
                        if (pending_win_sample_traces_.size() < kWinTraceCap) {
                            GuestAddr resolved_texture = 0;
                            if (auto pool = RenderModel().texture_pools.find(bound_.texture_pool);
                                pool != RenderModel().texture_pools.end()) {
                                if (auto entry = pool->second.find(handle & 0xFFFFF); entry != pool->second.end()) {
                                    resolved_texture = entry->second.first;
                                }
                            }
                            std::optional<u64> image_key;
                            if (img) {
                                for (const auto& [key, candidate] : images_) {
                                    if (&candidate == img) {
                                        image_key = key;
                                        break;
                                    }
                                }
                            }
                            std::ostringstream event;
                            event << "input draw_id=" << win_draw_id << " index=" << win_input_index
                                  << " target_key=0x" << std::hex << ImageKey(color_targets_[0]) << " serial="
                                  << std::dec << bound_.program_serial << " program=0x" << std::hex << bound_.program
                                  << " list=0x" << list_handle << " depth=" << std::dec << depth
                                  << " op=" << static_cast<int>(c.op) << " handle=0x" << std::hex << handle
                                  << " texture_id=0x" << texture_id << " sampler_id=0x" << sampler_id
                                  << " resolved_guest_texture=";
                            if (resolved_texture) event << "0x" << resolved_texture;
                            else event << "none";
                            event << " image_key=";
                            if (image_key) event << "0x" << *image_key;
                            else event << "none";
                            event << " image_valid=" << (img ? 1 : 0) << " image_format=" << std::dec
                                  << (img ? static_cast<unsigned>(img->format) : 0) << " image_size="
                                  << (img ? img->width : 0) << 'x' << (img ? img->height : 0)
                                  << " image_rendered=" << (img && img->rendered) << " image_draws="
                                  << (img ? img->draws : 0) << " sampler_valid=" << (sampler ? 1 : 0)
                                  << " tic_valid=" << (trace_tic ? 1 : 0) << " tic_type=";
                            if (trace_tic) {
                                event << trace_tic->Type() << " tic_format=" << trace_tic->Format()
                                      << " tic_size=" << trace_tic->Width() << 'x' << trace_tic->Height() << 'x'
                                      << trace_tic->Depth();
                            } else {
                                event << "none tic_format=none tic_size=none";
                            }
                            const bool image_valid = img != nullptr;
                            const bool sampler_valid = sampler != VK_NULL_HANDLE;
                            event << " sample_failure="
                                  << (image_valid ? "none" : (texture_error_.empty() ? "unknown" : texture_error_))
                                  << " guest_nonzero=" << trace_guest << " descriptor_status="
                                  << (!image_valid ? "skip-image" : !sampler_valid ? "skip-sampler" : "ready");
                            append_win_trace(event.str());
                        } else {
                            pending_win_sample_trace_capped_ = true;
                        }
                    } else if (win_traces++ < 24) {
                        std::ostringstream event;
                        event << "sample q=" << present_count_ << " draw_id=" << win_draw_id << " index="
                              << win_input_index << " target_key=0x" << std::hex << ImageKey(color_targets_[0])
                              << " program=0x" << bound_.program << " serial=" << std::dec << bound_.program_serial
                              << " list=0x" << std::hex << list_handle << " depth=" << std::dec << depth
                              << " op=" << static_cast<int>(c.op) << " handle=0x" << std::hex << handle
                              << " texture_id=0x" << texture_id << " sampler_id=0x" << sampler_id << std::dec
                              << " image_valid=" << (img ? 1 : 0) << " image_format="
                              << (img ? static_cast<unsigned>(img->format) : 0) << " image_size="
                              << (img ? img->width : 0) << 'x' << (img ? img->height : 0)
                              << " image_rendered=" << (img && img->rendered) << " image_draws="
                              << (img ? img->draws : 0) << " sampler_valid=" << (sampler ? 1 : 0)
                              << " tic_valid=" << (trace_tic ? 1 : 0) << " tic_type=";
                        if (trace_tic) {
                            event << trace_tic->Type() << " tic_format=" << trace_tic->Format() << " tic_size="
                                  << trace_tic->Width() << 'x' << trace_tic->Height() << 'x' << trace_tic->Depth();
                        } else {
                            event << "none tic_format=none tic_size=none";
                        }
                        const bool image_valid = img != nullptr;
                        const bool sampler_valid = sampler != VK_NULL_HANDLE;
                        event << " sample_failure="
                              << (image_valid ? "none" : (texture_error_.empty() ? "unknown" : texture_error_))
                              << " guest_nonzero=" << trace_guest << " descriptor_status="
                              << (!image_valid ? "skip-image" : !sampler_valid ? "skip-sampler" : "ready");
                        LOG_INFO("trace-win: %s", event.str().c_str());
                        LOG_INFO("trace-win: state color_word=%08x channel_mask=%08x blend0=%08x,%08x set=%d ds0=%08x "
                                 "polygon=%08x ncolor=%u program=%llx", bound_.color_word, bound_.channel_mask,
                                 bound_.blend_words[0][0], bound_.blend_words[0][1], bound_.blend_set[0] ? 1 : 0,
                                 bound_.depth_stencil[0], bound_.polygon, num_color_targets_,
                                 static_cast<unsigned long long>(bound_.program));
                        LOG_INFO("trace-win: draw op=%d a0=%llx a1=%llx a2=%llx a3=%llx", static_cast<int>(c.op),
                                 static_cast<unsigned long long>(c.a[0]), static_cast<unsigned long long>(c.a[1]),
                                 static_cast<unsigned long long>(c.a[2]), static_cast<unsigned long long>(c.a[3]));
                        for (u32 a = 0; a < 4; ++a) {
                            LOG_INFO("trace-win: attrib%u=%08x", a, bound_.attribs[a]);
                        }
                        for (u32 vb = 0; vb < 3; ++vb) {
                            const auto [va, size] = bound_.vertex_buffers[vb];
                            const auto cpu = va ? GpuToCpu(va) : std::nullopt;
                            u32 w[16]{};
                            if (cpu && g_memory.IsMapped(*cpu, 64)) g_memory.ReadBlock(*cpu, w, 64);
                            LOG_INFO("trace-win: vb%u va=%llx size=%llu %08x %08x %08x %08x | %08x %08x %08x %08x | "
                                     "%08x %08x %08x %08x | %08x %08x %08x %08x", vb,
                                     static_cast<unsigned long long>(va), static_cast<unsigned long long>(size), w[0],
                                     w[1], w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11], w[12], w[13],
                                     w[14], w[15]);
                        }
                        if (const auto icpu = GpuToCpu(c.a[3]); icpu) {
                            u16 ix[8]{};
                            if (g_memory.IsMapped(*icpu, 16)) g_memory.ReadBlock(*icpu, ix, 16);
                            LOG_INFO("trace-win: idx %04x %04x %04x %04x %04x %04x %04x %04x", ix[0], ix[1], ix[2],
                                     ix[3], ix[4], ix[5], ix[6], ix[7]);
                        }
                        for (size_t st = 0; st < 5; ++st) {
                            for (u32 cb = 0; cb < 6; ++cb) {
                                LOG_INFO("trace-win:   stage%zu cbuf%u: %08x %08x %08x %08x %08x %08x %08x %08x", st, cb,
                                         ReadCbuf(st, cb, 0), ReadCbuf(st, cb, 4), ReadCbuf(st, cb, 8),
                                         ReadCbuf(st, cb, 12), ReadCbuf(st, cb, 16), ReadCbuf(st, cb, 20),
                                         ReadCbuf(st, cb, 24), ReadCbuf(st, cb, 28));
                            }
                        }
                    }
                }
                ++win_input_index;
                const bool invalid_guest_descriptor = texture_error_ == "invalid-descriptor" ||
                                                      texture_error_ == "invalid-descriptor-address";
                if (!img && resolve_invalid_textures_ && desc.type == Shader::TextureType::Color2D &&
                    !desc.is_depth && !desc.is_integer && invalid_guest_descriptor) {
                    if (census_draw && null_2d_bindings < 3) {
                        LOG_INFO("null-texture-2d q=%llu program=0x%llx cbuf=%u offset=%x count=%u element=%u "
                                 "handle=%x original_error=%s",
                                 static_cast<unsigned long long>(present_count_ + 1),
                                 static_cast<unsigned long long>(bound_.program), desc.cbuf_index, desc.cbuf_offset,
                                 desc.count, k, handle, texture_error_.c_str());
                    }
                    ++null_2d_bindings;
                    img = &null_2d_;
                    sampler = null_2d_sampler_;
                    texture_error_.clear();
                }
                if (!img || !sampler) {
                    const std::string reason = !sampler ? "sampler" : "texture:" + texture_error_;
                    return skip(reason);
                }
                if (feedback_snapshot_enabled_ && std::find(colors.begin(), colors.end(), img) != colors.end()) {
                    HostImage* snapshot = SnapshotFeedbackTarget(*img);
                    if (!snapshot) return skip("feedback-snapshot");
                    if (trace_win_sample) {
                        LOG_INFO("trace-win feedback-snapshot draw_id=%llu source=%p snapshot=%p size=%ux%u layers=%u",
                                 static_cast<unsigned long long>(win_draw_id), static_cast<void*>(img),
                                 static_cast<void*>(snapshot), snapshot->width, snapshot->height, snapshot->layers);
                    }
                    img = snapshot;
                }
                const VkImageView sampled_view = SampleView(*img, texture_id, sampled_tic ? &*sampled_tic : nullptr);
                if (trace_win_sample) {
                    const bool uses_array_view = sampled_view && sampled_view == img->array_view;
                    const VkImageViewType view_type = uses_array_view ? img->array_view_type : img->view_type;
                    const VkImageAspectFlags view_aspect = uses_array_view ? img->array_view_aspect : img->view_aspect;
                    const u32 view_base_layer = uses_array_view ? img->array_view_base_layer : img->view_base_layer;
                    const u32 view_layer_count = uses_array_view ? img->array_view_layer_count : img->view_layer_count;
                    const VkResult view_result = uses_array_view ? img->array_view_create_result : img->view_create_result;
                    LOG_INFO("trace-win-vkview q=%llu draw=%llu input=%u texture=%x tic_type=%u "
                             "valid=%u view_type=%u aspect=0x%x base_layer=%u layer_count=%u "
                             "image_format=%u image_layers=%u image_cube=%u create_result=%d",
                             static_cast<unsigned long long>(present_count_),
                             static_cast<unsigned long long>(win_draw_id), win_input_index, texture_id,
                             trace_tic ? trace_tic->Type() : std::numeric_limits<u32>::max(),
                             sampled_view ? 1u : 0u, static_cast<unsigned>(view_type),
                             static_cast<unsigned>(view_aspect), view_base_layer, view_layer_count,
                             static_cast<unsigned>(img->format), img->layers, img->cube ? 1u : 0u,
                             static_cast<int>(view_result));
                }
                if (!sampled_view) return skip("image-view");
                if (census_draw) census_sampled_.push_back(img);
                const auto* image_info =
                    image_infos.Push({sampler, sampled_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, binding, k, 1,
                                  VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, image_info, nullptr, nullptr});
            }
            ++binding;
        }
    }

    if (frame_profile_) {
        frame_draw_descriptors_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                          std::chrono::steady_clock::now() - descriptors_start)
                                          .count();
    }

    // Vertex and index data, copied from guest memory.
    const auto buffers_start = frame_profile_ ? std::chrono::steady_clock::now()
                                               : std::chrono::steady_clock::time_point{};
    std::array<VkBuffer, 16> vbufs{};
    std::array<VkDeviceSize, 16> voffs{};
    u32 max_stream = 0;
    for (u32 i = 0; i < bound_.attribs.size() && i < 32; ++i) {
        const u32 w = bound_.attribs[i];
        if (!((w >> 6) & 1)) continue;
        const u32 stream = w & 0x1f;
        if (stream >= 16 || vbufs[stream]) continue;
        const auto [va, size] = bound_.vertex_buffers[stream];
        const auto cpu = va ? GpuToCpu(va) : std::nullopt;
        if (!cpu || !size) return skip("vertex-buffer");
        const u64 copy_size = std::min<u64>(size, 16 << 20);
        if (!is_guest_range_mapped(*cpu, copy_size)) return skip("vertex-unmapped");
        std::optional<u64> cached_offset;
        if (vertex_direct_) {
            // Compare against the guest bytes in place; only a miss copies them.
            g_memory.Guard(*cpu, copy_size, false);
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
            g_memory.ObserveRead(*cpu, copy_size);
#endif
            cached_offset = frame_vertex_uploads_.Find(
                *cpu, 16, std::span<const u8>(g_memory.RawPtr(*cpu), static_cast<size_t>(copy_size)), stream_offset_);
            if (frame_profile_ && cached_offset) frame_vertex_bytes_ += copy_size;
        }
        if (!cached_offset) {
            scratch.resize(copy_size);
            const auto vertex_read_start = frame_profile_ ? std::chrono::steady_clock::now()
                                                          : std::chrono::steady_clock::time_point{};
            g_memory.ReadBlock(*cpu, scratch.data(), scratch.size());
            if (frame_profile_) {
                frame_vertex_read_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                             std::chrono::steady_clock::now() - vertex_read_start)
                                             .count();
                frame_vertex_bytes_ += scratch.size();
            }
            if (!vertex_direct_) cached_offset = frame_vertex_uploads_.Find(*cpu, 16, scratch, stream_offset_);
            if (!cached_offset) {
                voffs[stream] = Upload(scratch.data(), scratch.size(), 16);
                if (voffs[stream] == ~u64{0}) return skip("stream-full");
                frame_vertex_uploads_.Store(*cpu, 16, scratch, voffs[stream]);
            }
        }
        if (cached_offset) voffs[stream] = *cached_offset;
        if (frame_profile_) {
            ++frame_vertex_range_uses_;
            if (cached_offset) {
                frame_vertex_reused_bytes_ += copy_size;
            } else {
                ++frame_vertex_range_unique_;
            }
        }
        vbufs[stream] = stream_.buffer;
        max_stream = std::max(max_stream, stream + 1);
    }
    u64 index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    const bool indexed = c.op == Op::DrawElements;
    if (indexed) {
        const u32 type = static_cast<u32>(c.a[1]);
        const u32 index_size = type == 0 ? 1 : type == 1 ? 2 : 4;
        index_type = type == 2 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        const auto cpu = GpuToCpu(c.a[3]);
        if (!cpu) return skip("index-buffer");
        scratch.resize(static_cast<size_t>(index_size) * c.a[2]);
        if (!is_guest_range_mapped(*cpu, scratch.size())) return skip("index-unmapped");
        const auto index_read_start = frame_profile_ ? std::chrono::steady_clock::now()
                                                     : std::chrono::steady_clock::time_point{};
        g_memory.ReadBlock(*cpu, scratch.data(), scratch.size());
        if (frame_profile_) {
            frame_index_read_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now() - index_read_start)
                                        .count();
            frame_index_bytes_ += scratch.size();
        }
        if (index_size == 1) { // widen 8-bit indices
            ScratchBytes wide;
            wide.assign(scratch.size() * 2, 0);
            for (size_t i = 0; i < scratch.size(); ++i) wide[2 * i] = scratch[i];
            scratch = std::move(wide);
        }
        index_offset = Upload(scratch.data(), scratch.size(), 4);
        if (index_offset == ~u64{0}) return skip("stream-full");
    }

    if (frame_profile_) {
        frame_draw_buffers_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - buffers_start)
                                     .count();
    }

    // Attachments and render pass.
    const auto commands_start = frame_profile_ ? std::chrono::steady_clock::now()
                                                : std::chrono::steady_clock::time_point{};
    for (HostImage* img : colors) {
        if (img) Transition(*img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    }
    if (ds && pending_depth_clear_.img == ds) {
        if (ShouldDropPendingDepthClear(bound_.depth_stencil[0], ds->aspect == VK_IMAGE_ASPECT_DEPTH_BIT,
                                        ds->depth_write_frame, present_count_ + 1)) {
            ++skip_reasons_["cleards:dropped-equal-after-prepass"];
            if (census_draw) {
                LOG_INFO("census-cleards-dropped q=%llu seq=%llu", static_cast<unsigned long long>(present_count_ + 1),
                         static_cast<unsigned long long>(census_seq));
            }
            pending_depth_clear_ = {};
        }
    }
    if (ds && ((bound_.depth_stencil[0] >> 1) & 1)) ds->depth_write_frame = present_count_ + 1;
    if (ds) Transition(*ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    std::vector<u64>& targets = draw_targets_;
    targets.clear();
    u32 width = ~0u, height = ~0u;
    std::vector<VkImageView>& views = draw_views_;
    views.clear();
    std::vector<u32>& rp_key = draw_rp_key_;
    rp_key.clear();
    rp_key.push_back(num_color_targets_);
    for (size_t i = 0; i < colors.size(); ++i) {
        HostImage* img = colors[i];
        if (!img) { // unused slot: format 0 in the render pass key, no framebuffer view
            targets.push_back(0);
            rp_key.push_back(0);
            continue;
        }
        const VkImageView view = TargetView(*img, color_layers_[i]);
        targets.push_back(reinterpret_cast<u64>(view));
        views.push_back(view);
        rp_key.push_back(img->format);
        width = std::min(width, img->width);
        height = std::min(height, img->height);
    }
    rp_key.push_back(ds ? ds->format : 0);
    if (ds) {
        const VkImageView view = TargetView(*ds, depth_layer_);
        targets.push_back(reinterpret_cast<u64>(view));
        views.push_back(view);
        width = std::min(width, ds->width);
        height = std::min(height, ds->height);
    }
    if (views.empty()) return skip("no-targets");
    const VkRenderPass render_pass = RenderPassFor(rp_key);
    VkCommandBuffer cmd = Cmd();
    constexpr u32 kCensusQueries = 8192;
    u32 census_query = ~0u;
    if (census_draw && census_query_seqs_.size() < kCensusQueries) {
        if (!census_queries_) {
            const VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, nullptr, 0,
                                            VK_QUERY_TYPE_OCCLUSION, kCensusQueries, 0};
            if (vkCreateQueryPool(vk_.device, &qci, nullptr, &census_queries_) != VK_SUCCESS) {
                census_queries_ = VK_NULL_HANDLE;
            }
        }
        if (census_queries_) {
            census_query = static_cast<u32>(census_query_seqs_.size());
            census_query_seqs_.push_back(census_seq);
            EndRenderPass();
            vkCmdResetQueryPool(cmd, census_queries_, census_query, 1);
        }
    }
    if (!in_render_pass_ || active_targets_ != targets) {
        EndRenderPass();
        std::vector<u64>& fb_key = draw_fb_key_;
        fb_key = targets;
        fb_key.push_back(reinterpret_cast<u64>(render_pass));
        VkFramebuffer& fb = framebuffers_[fb_key];
        if (!fb) {
            const VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, nullptr, 0, render_pass,
                                              static_cast<u32>(views.size()), views.data(), width, height, 1};
            vkCreateFramebuffer(vk_.device, &fci, nullptr, &fb);
        }
        const VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, nullptr, render_pass, fb,
                                        {{0, 0}, {width, height}}, 0, nullptr};
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        in_render_pass_ = true;
        active_targets_ = targets;
    }
    if (!graphics_state_cache_enabled_ || graphics_state_cache_.Pipeline(p->pipeline))
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
    else
        ++graphics_state_skipped_[0];
    if (p->uses_push_descriptors) {
        if (!vk_.cmd_push_descriptor_set) return skip("push-descriptor-proc");
        if (frame_profile_) ++frame_push_descriptor_sets_;
        vk_.cmd_push_descriptor_set(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0,
                                    static_cast<u32>(writes.size()), writes.data());
    } else {
        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptors_, 1,
                                              &p->set_layout};
        if (vkAllocateDescriptorSets(vk_.device, &dai, &set) != VK_SUCCESS) return skip("descriptor-pool");
        ++sets_in_pool_;
        if (frame_profile_) ++frame_descriptor_sets_;
        if (trace_cflush_) ++trace_sets_;
        for (auto& w : writes) w.dstSet = set;
        vkUpdateDescriptorSets(vk_.device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &set, 0, nullptr);
    }
    std::array<u8, sizeof(Shader::Backend::SPIRV::RescalingLayout) + sizeof(Shader::Backend::SPIRV::RenderAreaLayout)>
        push{};
    const float one = 1.0f;
    std::memcpy(push.data() + Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET, &one, sizeof(one));
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, static_cast<u32>(push.size()), push.data());
    // Default: the game's screen-space math (e.g. the water pass computes
    // v = 0.5 - 0.5 * y / w to sample the D32) expects clip y = +1 in texel row 0 (upper-left window origin). A
    // positive-height Vulkan viewport puts clip y = +1 in the last row, so every render target is stored upside down
    // and screen-space/UV reads of it come out vertically mirrored. A negative height covers the same rows y..y+h
    // with clip y = +1 at row y.
    static const bool viewport_yflip = std::getenv("NXRT_LEGACY_VIEWPORT") == nullptr;
    const VkViewport vp = viewport_yflip
                              ? VkViewport{bound_.viewport[0], bound_.viewport[1] + bound_.viewport[3],
                                           bound_.viewport[2], -bound_.viewport[3], bound_.depth_range[0],
                                           bound_.depth_range[1]}
                              : VkViewport{bound_.viewport[0], bound_.viewport[1], bound_.viewport[2],
                                           bound_.viewport[3], bound_.depth_range[0], bound_.depth_range[1]};
    if (!graphics_state_cache_enabled_ || graphics_state_cache_.Viewport(vp))
        vkCmdSetViewport(cmd, 0, 1, &vp);
    else
        ++graphics_state_skipped_[1];
    const VkRect2D scissor{{static_cast<s32>(bound_.scissor[0]), static_cast<s32>(bound_.scissor[1])},
                           {std::min(bound_.scissor[2], width), std::min(bound_.scissor[3], height)}};
    if (!graphics_state_cache_enabled_ || graphics_state_cache_.Scissor(scissor))
        vkCmdSetScissor(cmd, 0, 1, &scissor);
    else
        ++graphics_state_skipped_[2];
    if (!graphics_state_cache_enabled_ || graphics_state_cache_.Blend(bound_.blend_color))
        vkCmdSetBlendConstants(cmd, bound_.blend_color.data());
    else
        ++graphics_state_skipped_[3];
    for (u32 s = 0; s < max_stream; ++s) {
        if (vbufs[s]) vkCmdBindVertexBuffers(cmd, s, 1, &vbufs[s], &voffs[s]);
    }
    if (census_query != ~0u) vkCmdBeginQuery(cmd, census_queries_, census_query, 0);
    if (indexed) {
        vkCmdBindIndexBuffer(cmd, stream_.buffer, index_offset, index_type);
        const u32 instances = c.words.empty() ? 1 : std::max<u32>(1, c.words[0]);
        vkCmdDrawIndexed(cmd, static_cast<u32>(c.a[2]), instances, 0, static_cast<s32>(c.a[4]),
                         static_cast<u32>(c.a[5]));
    } else {
        vkCmdDraw(cmd, static_cast<u32>(c.a[2]), std::max<u32>(1, static_cast<u32>(c.a[4])), static_cast<u32>(c.a[1]),
                  static_cast<u32>(c.a[3]));
    }
    if (census_query != ~0u) vkCmdEndQuery(cmd, census_queries_, census_query);
    emit_win_draw_result(true, {});
    if (census_) {
        const bool zero_count = c.a[2] == 0;
        const bool empty_vp_sc = bound_.viewport[2] <= 0 || bound_.viewport[3] <= 0 || bound_.scissor[2] == 0 ||
                                 bound_.scissor[3] == 0;
        census_->Exec(indexed, indexed && !c.words.empty() && c.words[0] > 1, zero_count, empty_vp_sc);
    }
    for (size_t i = 0; i < colors.size(); ++i) {
        HostImage* img = colors[i];
        if (!img) continue;
        // The palette compositor writes encoded colors via a linear NVN view
        // (RGBA8=37, RGBA8_SRGB=56). Only a successful write establishes this.
        img->srgb_content.Draw(bound_.program_serial, img->nvn_format == 56 && color_view_formats_[i] == 37,
                               color_view_formats_[i] == 0,
                               c.a[2] != 0 && (EffectiveColorWriteMask(bound_.channel_mask, i) & 7) &&
                               bound_.viewport[2] > 0 && bound_.viewport[3] > 0 &&
                               bound_.scissor[2] != 0 && bound_.scissor[3] != 0);
        img->rendered = true;
        if (img->block_texel_target && c.a[2] && (EffectiveColorWriteMask(bound_.channel_mask, i) & 0xf))
            ++img->block_write_generation;
        ++img->draws;
    }
    if (ds) ds->rendered = true;
    ++draws_;
    if (render_stats_) {
        char buf[200];
        std::snprintf(buf, sizeof buf, "draw:%s topo=%u count=%llu inst=%u vp=%.0fx%.0f+%.0f+%.0f sc=%ux%u+%u+%u prog=%llu",
                      colors.empty() ? "depth-only" : TargetLabel(color_targets_[0], color_levels_[0]).c_str(), topology,
                      static_cast<unsigned long long>(c.a[2]),
                      c.op == Op::DrawElements && !c.words.empty() ? c.words[0] : 1u, bound_.viewport[2],
                      bound_.viewport[3], bound_.viewport[0], bound_.viewport[1], bound_.scissor[2], bound_.scissor[3],
                      bound_.scissor[0], bound_.scissor[1], static_cast<unsigned long long>(bound_.program_serial));
        ++pass_stats_[buf];
    }
    if (frame_profile_) {
        frame_draw_commands_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - commands_start)
                                      .count();
    }
    if (trace_win_frame && colors[0] &&
        std::find(win_trace.checkpoint_draw_ids.begin(), win_trace.checkpoint_draw_ids.end(), win_draw_id) !=
            win_trace.checkpoint_draw_ids.end()) {
        DumpWinSampleCheckpoint(*colors[0], *win_trace.frame, win_draw_id);
    }
    census_log(true, {});
    if (census_draw && census.Checkpoint(census_seq)) {
        if (!colors.empty() && colors[0]) DumpWinSampleCheckpoint(*colors[0], present_count_ + 1, census_seq, "census-c0");
        if (ds) DumpWinSampleCheckpoint(*ds, present_count_ + 1, census_seq, "census-ds");
        // NXRT_PROBE_CENSUS_SAMPLED=1: also read back every image the draw sampled (census-s<N>).
        if (std::getenv("NXRT_PROBE_CENSUS_SAMPLED")) {
            for (size_t i = 0; i < census_sampled_.size(); ++i) {
                if (census_sampled_[i] && census_sampled_[i] != &null_2d_) {
                    const std::string tag = "census-s" + std::to_string(i);
                    DumpWinSampleCheckpoint(*census_sampled_[i], present_count_ + 1, census_seq, tag.c_str());
                }
            }
        }
    }
}

// ------------------------------------------------------------------ compute

// Writes the {counter, timestamp} pair the GPU would report. Draws are not counted, so every counter grows by a
// large step per report: a begin/end pair of occlusion reports always reads as visible (nothing is culled), and the
// timestamp is in GPU ticks (19.2 MHz) since the renderer started.
void NvnRenderer::CensusDrop(const std::string& reason) {
    if (census_) census_->Drop(reason);
}

// Stable program id for the census: hash of the code/control addresses of every stage (vertex + fragment ...).
u64 NvnRenderer::CensusProgramHash() const {
    const auto prog = RenderModel().programs.find(bound_.program);
    if (prog == RenderModel().programs.end()) return 0;
    std::vector<u64> words;
    for (const auto& stage : prog->second.stages) {
        words.push_back(stage.code);
        words.push_back(stage.control);
    }
    return words.empty() ? 0 : Hash(words.data(), words.size() * sizeof(u64));
}

void NvnRenderer::ReportCounter(const Command& c) {
    static const auto start = std::chrono::steady_clock::now();
    static const bool trace = std::getenv("NXRT_TRACE_COUNTERS") != nullptr;
    const auto cpu = GpuToCpu(c.a[1]);
    if (!cpu || !g_memory.IsMapped(*cpu, 16)) {
        if (census_) census_->Counter(present_count_, c.a[0], c.a[1], 0, false);
        return;
    }
    const u64 ns = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    const u64 ticks_real = ns / 625 * 12; // 19.2 MHz = 12 ticks per 625 ns
    // Opt-in NXRT_TS_MODE=scale:<k> (0.1..1): timestamp (type 0) reports run at k times the host clock, so the
    // differences the game measures between two reports shrink by k. NXRT_TS_LOG=<file>: one row per type-0 report.
    static const double ts_scale = [] {
        const char* e = std::getenv("NXRT_TS_MODE");
        double k = 0.0;
        if (e && std::sscanf(e, "scale:%lf", &k) == 1 && k >= 0.1 && k <= 1.0) {
            LOG_INFO("renderer: NXRT_TS_MODE scale:%.3f", k);
            return k;
        }
        if (e && *e) LOG_WARN("renderer: NXRT_TS_MODE '%s' ignored (scale:<0.1..1>)", e);
        return 0.0;
    }();
    static u64 ts_origin = 0;
    static std::FILE* ts_log = [] {
        const char* p = std::getenv("NXRT_TS_LOG");
        std::FILE* fp = p && *p ? std::fopen(p, "w") : nullptr;
        if (fp) std::fputs("q,gpu,type,real_ticks,value\n", fp);
        return fp;
    }();
    static u32 ts_log_rows = 0;
    u64 ticks = ticks_real;
    if (c.a[0] == 0 && ts_scale > 0.0) {
        if (!ts_origin) ts_origin = ticks_real;
        ticks = ts_origin + static_cast<u64>(static_cast<double>(ticks_real - ts_origin) * ts_scale);
    }
    // NXRT_COUNTER16_MODE (diagnostic): how counters other than the timestamp are reported.
    // default = +0x100000 per report (~54.6 ms at 19.2 MHz), small = +16, zero = 0, real = the real clock in ticks.
    static const int counter_mode = [] {
        const char* e = std::getenv("NXRT_COUNTER16_MODE");
        if (!e) return 0;
        if (std::strcmp(e, "small") == 0) return 1;
        if (std::strcmp(e, "zero") == 0) return 2;
        if (std::strcmp(e, "real") == 0) return 3;
        return 0;
    }();
    report_counter_ += counter_mode == 1 ? 16 : 0x100000;
    u64 value = c.a[0] == 0 ? ticks : report_counter_; // NVN_COUNTER_TYPE_TIMESTAMP = 0
    if (c.a[0] != 0 && counter_mode == 2) value = 0;
    if (c.a[0] != 0 && counter_mode == 3) value = ticks;
    if (const auto& cc = GetDrawCensusConfig(); cc.frame && present_count_ + 1 >= cc.frame && present_count_ + 1 <= cc.frame_end) {
        LOG_INFO("census-report q=%llu after_seq=%llu type=%llu gpu=%llx value=%llx prev0=%llx prev16=%llx",
                 static_cast<unsigned long long>(present_count_ + 1),
                 static_cast<unsigned long long>(census_seq_frame_ == present_count_ + 1 ? census_seq_ : 0),
                 static_cast<unsigned long long>(c.a[0]), static_cast<unsigned long long>(c.a[1]),
                 static_cast<unsigned long long>(value), static_cast<unsigned long long>(g_memory.Read<u64>(*cpu)),
                 static_cast<unsigned long long>(g_memory.IsMapped(*cpu + 16, 8) ? g_memory.Read<u64>(*cpu + 16) : 0));
    }
    if (CounterTrace::Enabled()) CounterTrace::Get().Record(c.a[0], value);
    if (ts_log && c.a[0] == 0) {
        std::fprintf(ts_log, "%llu,%llx,%llu,%llu,%llu\n", static_cast<unsigned long long>(present_count_ + 1),
                     static_cast<unsigned long long>(c.a[1]), static_cast<unsigned long long>(c.a[0]),
                     static_cast<unsigned long long>(ticks_real), static_cast<unsigned long long>(value));
        if (++ts_log_rows % 32 == 0) std::fflush(ts_log);
    }
    if (census_) census_->Counter(present_count_, c.a[0], c.a[1], value, true);
    g_memory.Write<u64>(*cpu, value);
    g_memory.Write<u64>(*cpu + 8, ticks);
    if (c.a[0] != 0) synthetic_reports_[c.a[1]] = value;
    else synthetic_reports_.erase(c.a[1]);
    if (trace && reported_counter_types_.insert(c.a[0]).second) {
        LOG_INFO("trace-counter: type %llu at gpu %llx", static_cast<unsigned long long>(c.a[0]),
                 static_cast<unsigned long long>(c.a[1]));
    }
}

// Workgroup size and memory sizes of a compute program, from its NVN control section (GLSLC output).
// NXRT_COMPUTE_WG=x,y,z overrides the workgroup size (diagnostic).
std::optional<ComputeLaunch> ReadComputeLaunch(GuestAddr control) {
    ComputeLaunch launch;
    if (const char* wg = std::getenv("NXRT_COMPUTE_WG"); wg && *wg) {
        if (std::sscanf(wg, "%u,%u,%u", &launch.workgroup[0], &launch.workgroup[1], &launch.workgroup[2]) == 3) {
            return launch;
        }
    }
    // Control section (magic 0x98761234), measured on the game's three compute programs: +0x700 and +0x704 are
    // memory sizes (0x300 for the 32-wide programs, 0x800 for the single-thread one; used here as local and shared
    // memory sizes, which only bounds what the shader may address), +0x748 the workgroup size.
    if (!control || !g_memory.IsMapped(control, 0x800) || g_memory.Read<u32>(control) != 0x98761234) {
        return std::nullopt;
    }
    for (u32 i = 0; i < 3; ++i) launch.workgroup[i] = g_memory.Read<u32>(control + 0x748 + 4 * i);
    if (launch.workgroup[0] == 0 || launch.workgroup[1] == 0 || launch.workgroup[2] == 0 ||
        launch.workgroup[0] * launch.workgroup[1] * launch.workgroup[2] > 1024) {
        return std::nullopt;
    }
    launch.local_memory = std::min<u32>(g_memory.Read<u32>(control + 0x700), 0x10000);
    launch.shared_memory = std::min<u32>(g_memory.Read<u32>(control + 0x704), 0xc000);
    return launch;
}

// Copy rendered texels back to the guest buffer before subsequent commands consume it.
// Facepaint uses this both for its editing canvas and for the final saved image.
void NvnRenderer::CopyTextureToBuffer(const Command& c) {
    HitchScope hitch_scope(kHitchCopyToBuffer);
    static const bool enabled = [] {
        const char* value = std::getenv("NXRT_TEXTURE_TO_BUFFER");
        return !value || std::strcmp(value, "0") != 0;
    }();
    if (!enabled || c.words.size() != 8) return;
    const auto source = images_.find(LevelKey(ImageKey(c.a[0]), c.words[6]));
    if (source == images_.end() || !source->second.rendered) {
        ++skip_reasons_["texture-to-buffer:source-not-rendered"];
        return;
    }
    auto& image = source->second;
    const auto format = Format(image.nvn_format);
    Tic tic{};
    tic.w[0] = format.tic_word0;
    const auto decoded = TicFormat(tic);
    const u32 x = c.words[0], y = c.words[1], width = c.words[3], height = c.words[4];
    const u64 layer = u64{c.words[7]} + c.words[2];
    if (!format.valid || !format.bytes_per_block || decoded.block_width != 1 || decoded.block_height != 1 ||
        decoded.format != image.format || image.aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
        !width || !height || c.words[5] != 1 || layer >= image.layers ||
        x >= image.width || y >= image.height || width > image.width - x || height > image.height - y) {
        ++skip_reasons_["texture-to-buffer:unsupported-region-or-format"];
        return;
    }
    const auto destination = GpuToCpuBacking(c.a[1]);
    const u64 size = u64{width} * height * format.bytes_per_block;
    if (!destination || !ContainsBackingAccess(*destination, 0, size) ||
        !g_memory.IsMapped(destination->cpu, size)) {
        ++skip_reasons_["texture-to-buffer:destination"];
        return;
    }
    Buffer readback = vk_.CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (!readback.buffer || !readback.mapped) {
        vk_.Destroy(readback);
        return;
    }
    EndRenderPass();
    const auto restore_layout = image.layout;
    Transition(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkBufferImageCopy copy{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<u32>(layer), 1},
                                 {static_cast<s32>(x), static_cast<s32>(y), 0}, {width, height, 1}};
    vkCmdCopyImageToBuffer(Cmd(), image.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &copy);
    const VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
                                         VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                         VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, readback.buffer, 0, size};
    vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                         0, nullptr, 1, &barrier, 0, nullptr);
    Transition(image, restore_layout);
    if (!Flush(FlushReason::MappedReadback)) {
        retained_readbacks_.push_back(readback);
        return;
    }
    g_memory.WriteBlock(destination->cpu, readback.mapped, size);
    if (std::getenv("NXRT_TRACE_FACEPAINT")) {
        LOG_INFO("texture-to-buffer: q=%llu source=%llx cpu=%llx size=%ux%u bytes=%llu hash=%llx",
                 (unsigned long long)(present_count_ + 1), (unsigned long long)c.a[0],
                 (unsigned long long)destination->cpu, width, height, (unsigned long long)size,
                 (unsigned long long)XxHash64(readback.mapped, size));
    }
    vk_.Destroy(readback);
}

// nvnCommandBufferCopyTextureToTexture between textures this renderer holds as host images (render targets). The
// destination becomes a rendered host image, so later samples read the copy. A source the renderer never drew into
// lives only in guest memory: the copy is skipped and counted.
void NvnRenderer::CopyTexture(const Command& c) {
    HitchScope hitch_scope(kHitchCopy);
    ++copies_;
    if (c.words.size() < 16) return;
    const u32* sr = c.words.data();
    const u32* dr = c.words.data() + 6;
    const u32 src_level = c.words[12], dst_level = c.words[14];
    const auto src_it = images_.find(LevelKey(ImageKey(c.a[0]), src_level));
    const auto dst_it = images_.find(LevelKey(ImageKey(c.a[1]), dst_level));
    // Copy regions use logical texels, while these attachments contain BC3
    // blocks. Do not silently clamp them to the smaller integer extent.
    if ((src_it != images_.end() && src_it->second.block_texel_target) ||
        (dst_it != images_.end() && dst_it->second.block_texel_target)) {
        ++skip_reasons_["copy:block-target"];
        return;
    }
    if (src_it == images_.end() || !src_it->second.rendered) {
        if (const auto copy = nvn::PlanGuestTextureCopy(RenderModel(), c);
            copy && g_memory.IsMapped(copy->source, copy->size) && g_memory.IsMapped(copy->destination, copy->size)) {
            // The uploaded source is already in guest memory. Identical driver
            // layouts let a full copy preserve the tiled/compressed bytes.
            if (!Flush(FlushReason::MappedReadback)) return;
            std::vector<u8> bytes(copy->size);
            g_memory.ReadBlock(copy->source, bytes.data(), bytes.size());
            g_memory.WriteBlock(copy->destination, bytes.data(), bytes.size());
            if (auto dst = images_.find(LevelKey(ImageKey(c.a[1]), dst_level)); dst != images_.end()) {
                dst->second.rendered = false;
                dst->second.srgb_content.Clear();
            }
            ++copies_done_;
            if (std::getenv("NXRT_TRACE_FACEPAINT"))
                LOG_INFO("guest-texture-copy: q=%llu src=%llx dst=%llx bytes=%llu",
                         (unsigned long long)(present_count_ + 1), (unsigned long long)copy->source,
                         (unsigned long long)copy->destination, (unsigned long long)copy->size);
            return;
        }
        ++skip_reasons_["copy:source-not-rendered"];
        return;
    }
    HostImage* dst = ImageFor(c.a[1], dst_level);
    // ImageFor may recreate images; look the source up again.
    HostImage* src = &images_[LevelKey(ImageKey(c.a[0]), src_level)];
    if (!dst || dst->block_texel_target || !src->image.image || src == dst) {
        ++skip_reasons_["copy:destination"];
        return;
    }
    const auto clamp = [](const u32* r, const HostImage& img, s32 out[4]) {
        out[0] = std::clamp<s32>(static_cast<s32>(r[0]), 0, static_cast<s32>(img.width));
        out[1] = std::clamp<s32>(static_cast<s32>(r[1]), 0, static_cast<s32>(img.height));
        out[2] = std::clamp<s32>(static_cast<s32>(r[0] + r[3]), out[0], static_cast<s32>(img.width));
        out[3] = std::clamp<s32>(static_cast<s32>(r[1] + r[4]), out[1], static_cast<s32>(img.height));
    };
    s32 s_box[4], d_box[4];
    clamp(sr, *src, s_box);
    clamp(dr, *dst, d_box);
    if (s_box[2] == s_box[0] || s_box[3] == s_box[1] || d_box[2] == d_box[0] || d_box[3] == d_box[1]) return;
    EndRenderPass();
    Transition(*src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(*dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    const u32 src_layer = c.words[13] < src->layers ? c.words[13] : 0;
    const u32 dst_layer = c.words[15] < dst->layers ? c.words[15] : 0;
    const VkImageAspectFlags src_aspect =
        (src->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT} : src->aspect;
    const VkImageAspectFlags dst_aspect =
        (dst->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT} : dst->aspect;
    if (src_aspect != dst_aspect) {
        ++skip_reasons_["copy:aspect"];
        return;
    }
    const bool same_size = s_box[2] - s_box[0] == d_box[2] - d_box[0] && s_box[3] - s_box[1] == d_box[3] - d_box[1];
    if (same_size && src->format == dst->format) {
        const VkImageCopy region{{src_aspect, 0, src_layer, 1},
                                 {s_box[0], s_box[1], 0},
                                 {dst_aspect, 0, dst_layer, 1},
                                 {d_box[0], d_box[1], 0},
                                 {static_cast<u32>(s_box[2] - s_box[0]), static_cast<u32>(s_box[3] - s_box[1]), 1}};
        vkCmdCopyImage(Cmd(), src->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
        // Scaled or format-converting copy. NVN_COPY_FLAGS_LINEAR_FILTER_BIT (1) selects linear filtering.
        const VkImageBlit region{{src_aspect, 0, src_layer, 1},
                                 {{s_box[0], s_box[1], 0}, {s_box[2], s_box[3], 1}},
                                 {dst_aspect, 0, dst_layer, 1},
                                 {{d_box[0], d_box[1], 0}, {d_box[2], d_box[3], 1}}};
        const bool linear = (c.a[2] & 1) && src_aspect == VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdBlitImage(Cmd(), src->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    const bool full_destination = d_box[0] == 0 && d_box[1] == 0 &&
                                  d_box[2] == static_cast<s32>(dst->width) &&
                                  d_box[3] == static_cast<s32>(dst->height) && dst->layers == 1;
    dst->srgb_content.CopyFrom(src->srgb_content, full_destination);
    dst->rendered = true;
    ++copies_done_;
}

ComputePipeline* NvnRenderer::GetComputePipeline() {
    static ComputePipeline none{};
    const auto prog = RenderModel().programs.find(bound_.program);
    if (prog == RenderModel().programs.end() || prog->second.stages.size() != 1) {
        none.error = "no-program";
        return &none;
    }
    auto& p = compute_pipelines_[prog->second.serial];
    if (p) return p.get();
    p = std::make_unique<ComputePipeline>();
    const nvn::ShaderStage& stage = prog->second.stages[0];
    const auto launch = ReadComputeLaunch(stage.control);
    if (!launch) {
        p->error = "control";
        return p.get();
    }
    const auto backing = GpuToCpuBacking(stage.code);
    if (!backing || !ContainsBackingAccess(*backing, 0, sizeof(u32))) {
        p->error = "code-address";
        return p.get();
    }
    if (!shader_pools_) shader_pools_ = std::make_unique<ShaderPools>();
    struct ReleaseShaderPools {
        ShaderPools& pools;
        ~ReleaseShaderPools() { pools.ReleaseContents(); }
    } release_shader_pools{*shader_pools_};
    // Compute shader data: magic 0x12345679, then zeros up to the code (+0x80 holds no instruction in this game):
    // the code starts at the first non-zero 32-byte bundle (a scheduling word leads every bundle).
    const u32 magic = g_memory.Read<u32>(backing->cpu);
    u32 start = magic == 0x12345678 ? 0x30 : 0;
    if (magic == 0x12345679) {
        start = 0;
        for (u32 at = 0x20; at + 8 <= std::min<u64>(backing->size, 0x1000); at += 0x20) {
            if (g_memory.Read<u64>(backing->cpu + at) != 0) {
                start = at;
                break;
            }
        }
        std::string head;
        for (u32 i = 0; i < 24 && 0x80 + 8 * i + 8 <= backing->size; ++i) {
            head += fmt::format(" {:016x}", g_memory.Read<u64>(backing->cpu + 0x80 + 8 * i));
        }
        LOG_INFO("renderer: compute program 0x%llx code starts at +0x%x; +0x80:%s",
                 static_cast<unsigned long long>(bound_.program), start, head.c_str());
        if (!start) {
            p->error = "code-start";
            return p.get();
        }
    }
    NvnEnvironment env{*this, backing->cpu, backing->size, start, kComputeNvnStage, false};
    env.SetCompute(start, *launch);
    Shader::IR::Program program;
    NvnEnvironment::DepRecorder recorder;
    env.recorder = &recorder;
    try {
        Shader::Maxwell::Flow::CFG cfg(env, shader_pools_->flow_block, start);
        program = Shader::Maxwell::TranslateProgram(shader_pools_->inst, shader_pools_->block, env, cfg, host_info_);
        {
            std::vector<u32>& offs = recorder.instruction_offsets;
            std::sort(offs.begin(), offs.end());
            offs.erase(std::unique(offs.begin(), offs.end()), offs.end());
            u64 h = FastHash(&start, sizeof(start), 0xc0de);
            h = FastHash(&*launch, sizeof(*launch), h);
            for (size_t i = 0; i < offs.size();) {
                size_t j = i + 1;
                while (j < offs.size() && offs[j] == offs[j - 1] + 8) ++j;
                const u32 len = offs[j - 1] + 8 - offs[i];
                std::vector<u8> bytes(len);
                g_memory.ReadBlock(backing->cpu + offs[i], bytes.data(), len);
                h = FastHash(&offs[i], sizeof(u32), h);
                h = FastHash(bytes.data(), len, h);
                i = j;
            }
            p->content_hash = h;
        }
    } catch (const std::exception& e) {
        LOG_ERROR("renderer: compute shader at 0x%llx (program 0x%llx) did not translate at +0x%x: %s",
                  static_cast<unsigned long long>(stage.code), static_cast<unsigned long long>(bound_.program),
                  env.last_address, e.what());
        p->error = "translate";
        return p.get();
    }
    Shader::Backend::Bindings binding;
    std::vector<u32> code;
    try {
        code = Shader::Backend::SPIRV::EmitSPIRV(compute_profile_, Shader::RuntimeInfo{}, program, binding);
    } catch (const std::exception& e) {
        LOG_ERROR("renderer: compute SPIR-V emission failed: %s", e.what());
        p->error = "spirv";
        return p.get();
    }
    p->info = std::move(program.info);
    std::vector<VkDescriptorSetLayoutBinding> layout_bindings;
    const auto add = [&](VkDescriptorType type, const auto& descriptors) {
        for (const auto& d : descriptors) {
            layout_bindings.push_back({static_cast<u32>(layout_bindings.size()), type, d.count,
                                       VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        }
    };
    add(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, p->info.constant_buffer_descriptors);
    add(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, p->info.storage_buffers_descriptors);
    add(VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, p->info.texture_buffer_descriptors);
    add(VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, p->info.image_buffer_descriptors);
    add(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, p->info.texture_descriptors);
    add(VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, p->info.image_descriptors);
    const VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, nullptr, 0,
                                              static_cast<u32>(layout_bindings.size()), layout_bindings.data()};
    vkCreateDescriptorSetLayout(vk_.device, &dsl, nullptr, &p->set_layout);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   static_cast<u32>(sizeof(Shader::Backend::SPIRV::RescalingLayout))};
    const VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, nullptr, 0, 1, &p->set_layout,
                                         1, &push};
    vkCreatePipelineLayout(vk_.device, &pli, nullptr, &p->layout);
    if (const char* dump_dir = std::getenv("NXRT_DUMP_COMPUTE_SPIRV"); dump_dir && *dump_dir) {
        const char* filter = std::getenv("NXRT_DUMP_COMPUTE_PROGRAM");
        const u64 wanted = filter ? std::strtoull(filter, nullptr, 0) : 0;
        if (!wanted || wanted == bound_.program) {
            const std::string path = fmt::format("{}/compute-{:x}.spv", dump_dir, bound_.program);
            std::ofstream dump(path, std::ios::binary);
            if (dump) {
                dump.write(reinterpret_cast<const char*>(code.data()),
                           static_cast<std::streamsize>(code.size() * sizeof(u32)));
                LOG_INFO("renderer: compute SPIR-V dump program=%llx words=%zu path=%s ok=%d",
                         static_cast<unsigned long long>(bound_.program), code.size(), path.c_str(), dump.good());
            } else {
                LOG_WARN("renderer: compute SPIR-V dump program=%llx cannot open %s",
                         static_cast<unsigned long long>(bound_.program), path.c_str());
            }
        }
    }
    VkShaderModule module = VK_NULL_HANDLE;
    const VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, nullptr, 0,
                                       code.size() * sizeof(u32), code.data()};
    vkCreateShaderModule(vk_.device, &smi, nullptr, &module);
    const VkComputePipelineCreateInfo cpi{
        VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, nullptr, 0,
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main",
         nullptr},
        p->layout, VK_NULL_HANDLE, 0};
    if (std::getenv("NXRT_DUMP_COMPUTE_SPIRV")) {
        LOG_INFO("renderer: compute pipeline create begin program=%llx spirv_words=%zu",
                 static_cast<unsigned long long>(bound_.program), code.size());
    }
    const VkResult r = vkCreateComputePipelines(vk_.device, VK_NULL_HANDLE, 1, &cpi, nullptr, &p->pipeline);
    vkDestroyShaderModule(vk_.device, module, nullptr);
    if (r != VK_SUCCESS) {
        p->error = "vkCreateComputePipelines";
        return p.get();
    }
    LOG_INFO("renderer: compute pipeline program=%llx workgroup=%ux%ux%u shared=%u local=%u ubos=%zu ssbos=%zu "
             "textures=%zu images=%zu",
             static_cast<unsigned long long>(bound_.program), launch->workgroup[0], launch->workgroup[1],
             launch->workgroup[2], launch->shared_memory, launch->local_memory,
             p->info.constant_buffer_descriptors.size(), p->info.storage_buffers_descriptors.size(),
             p->info.texture_descriptors.size(), p->info.image_descriptors.size());
    p->ok = true;
    return p.get();
}

// Runs a compute dispatch the way the GPU would at this point of the list. Storage buffers are copied from guest
// memory into the stream buffer like a draw's; the ones the shader writes are copied back once the GPU finished, so
// later draws (and the CPU) see the results.
namespace {
// Diagnostic (opt-in): NXRT_DUMP_DISPATCH=<hex guest program> and NXRT_DUMP_DISPATCH_DIR=<dir> write, before the
// dispatch runs, everything the compute shader reads (uniform and storage buffer contents, group counts), so the
// dispatch can be replayed on its own outside the game (tools/dispatch-repro).
struct DispatchDump {
    u64 program = 0;
    std::string dir;
    u32 limit = 3, done = 0;
    bool Enabled(u64 current) const { return program && current == program && done < limit && !dir.empty(); }
};
DispatchDump& DispatchDumpConfig() {
    static DispatchDump config = [] {
        DispatchDump d;
        if (const char* p = std::getenv("NXRT_DUMP_DISPATCH"); p && *p) d.program = std::strtoull(p, nullptr, 16);
        if (const char* dir = std::getenv("NXRT_DUMP_DISPATCH_DIR"); dir && *dir) d.dir = dir;
        return d;
    }();
    return config;
}
void WriteDumpFile(const std::string& path, const void* data, size_t size) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (f) f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
}
} // namespace

void NvnRenderer::Dispatch(const Command& c) {
    HitchScope hitch_scope(kHitchDispatch);
    ++compute_dispatches_;
    if (kHitchMs > 0.0) ++g_hitch.dispatches[bound_.program], ++g_hitch.dispatch_total;
    if (census_) census_->ComputeDispatch(present_count_, bound_.program);
    TraceDispatch(c);
    static const bool diag_return = std::getenv("NXRT_DIAG_DISPATCH_RETURN") != nullptr;  // timing only, wrong output
    if (diag_return) return;
    if (!compute_enabled_ || broken_ || !render_enabled_) return;
    const auto skip = [this](const std::string& why) {
        ++compute_skipped_;
        ++skip_reasons_["compute:" + why];
    };
    // Diagnostics (opt-in): NXRT_COMPUTE_SKIP_PROGRAMS=<hex>[,<hex>...] drops dispatches of those guest programs;
    // NXRT_SYNC_DISPATCH=1 submits and waits after every dispatch, logging which one is running, to find a GPU hang.
    static const std::set<u64> skip_programs = [] {
        std::set<u64> out;
        // Compute program 0x2044e14ad8 (island scene, 1 thread, 1 dispatch per frame) makes palms, bushes and signs
        // vanish while the camera is still; it was found by bisecting the island's four compute programs in play
        // sessions (skipping only it keeps every object stable). Its cause is still open, so it is skipped by default.
        // NXRT_COMPUTE_KEEP_ISLAND_PROGRAM=1 runs it again.
        if (const char* k = std::getenv("NXRT_COMPUTE_KEEP_ISLAND_PROGRAM"); !(k && k[0] == '1')) out.insert(0x2044e14ad8);
        if (const char* v = std::getenv("NXRT_COMPUTE_SKIP_PROGRAMS"); v && *v) {
            const std::string text(v);
            size_t begin = 0;
            while (begin < text.size()) {
                const size_t end = std::min(text.find(',', begin), text.size());
                if (const u64 id = std::strtoull(text.substr(begin, end - begin).c_str(), nullptr, 16)) out.insert(id);
                begin = end + 1;
            }
        }
        return out;
    }();
    static const bool sync_dispatch = std::getenv("NXRT_SYNC_DISPATCH") != nullptr;
    if (!ReserveStream()) return skip("stream-flush-failed");
    PollAsync();
    ComputePipeline* p;
    {
        HitchScope lookup_scope(kHitchPipelineLookup);
        p = GetComputePipeline();
    }
    if (!p->ok) return skip(p->error);
    // The pipeline above is built (and dumped) first; only the dispatch itself is dropped.
    if (skip_programs.count(bound_.program)) return skip("diagnostic-skip-program");
    const Shader::Info& info = p->info;
    if (!info.texture_buffer_descriptors.empty() || !info.image_buffer_descriptors.empty() ||
        !info.image_descriptors.empty()) {
        return skip("image-descriptors");
    }
    if (chain_on_) {
        bool fingerprint_ok = false;
        const auto fp_start = std::chrono::steady_clock::now();
        if (chain_.count == 0 && !chain_.skipping) chain_started_ = fp_start;
        const u64 fp = DispatchFingerprint(info, c, p->content_hash, &fingerprint_ok);
        if (chain_.skipping) chain_fp_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - fp_start).count();
        if (!fingerprint_ok) {
            if (chain_.skipping) ChainAbandonSkip("program unknown");
        } else if (ChainOnDispatch(fp)) {
            return;  // covered by a stored run: its result is written when the last dispatch arrives
        }
    }
    if (batch_compute_writebacks_ && !pending_compute_writebacks_.empty()) {
        constexpr size_t s = kComputeNvnStage;
        const auto overlaps_pending = [this](GuestAddr cpu, size_t size) {
            const GuestMemoryRange range{cpu, size};
            return std::any_of(pending_compute_writebacks_.begin(), pending_compute_writebacks_.end(),
                               [&](const PendingComputeWriteback& writeback) {
                                   return GuestMemoryRangesOverlap(range, {writeback.cpu, writeback.size});
                               });
        };
        const auto can_forward_pending = [this](GuestAddr cpu, size_t size, bool read_only) {
            if (!forward_compute_writebacks_ || DispatchDumpConfig().Enabled(bound_.program)) return false;
            return FindForwardableComputeWriteback(
                       std::span<const PendingComputeWriteback>{pending_compute_writebacks_.data(),
                                                                 pending_compute_writebacks_.size()},
                       {cpu, size}, read_only, vk_.properties.limits.minStorageBufferOffsetAlignment)
                .has_value();
        };
        // Compute texture descriptors can sample the same guest backing memory as a pending SSBO
        // writeback. Resolve those before texture upload/refresh reads from guest RAM. The
        // diagnostic skip is guarded by the read observer and only tests whether this
        // conservative barrier is needed in the current replay.
        bool has_dependency = MustFlushForUntrackedComputeTextures(
            !pending_compute_writebacks_.empty(), !info.texture_descriptors.empty(), skip_compute_texture_flush_);
        if (skip_compute_texture_flush_ && !info.texture_descriptors.empty()) {
            static u64 skipped_texture_flush_traces = 0;
            if (skipped_texture_flush_traces++ < 128) {
                LOG_INFO("renderer: compute-texture-flush-skipped q=%llu program=0x%llx pending=%zu inputs=%zu",
                         static_cast<unsigned long long>(present_count_ + 1),
                         static_cast<unsigned long long>(bound_.program), pending_compute_writebacks_.size(),
                         info.texture_descriptors.size());
            }
        }
        for (const auto& desc : info.constant_buffer_descriptors) {
            for (u32 k = 0; k < desc.count && !has_dependency; ++k) {
                const u32 slot = desc.index + k;
                if (slot == kTextureBoundBuffer || slot >= 18) continue;
                const auto [va, size] = bound_.uniform_buffers[s][slot];
                const auto cpu = va ? GpuToCpu(va) : std::nullopt;
                const size_t used = std::min<u64>(size, 0x10000);
                has_dependency = cpu && used && g_memory.IsMapped(*cpu, used) && overlaps_pending(*cpu, used);
            }
        }
        for (const auto& desc : info.storage_buffers_descriptors) {
            for (u32 k = 0; k < desc.count && !has_dependency; ++k) {
                const u32 off = desc.cbuf_offset + k * 16;
                const GpuAddr va = Pack(ReadCbuf(kComputeNvnStage, desc.cbuf_index, off),
                                        ReadCbuf(kComputeNvnStage, desc.cbuf_index, off + 4));
                const u32 size = ReadCbuf(kComputeNvnStage, desc.cbuf_index, off + 8);
                const auto cpu = va ? GpuToCpu(va) : std::nullopt;
                const size_t used = std::min<size_t>(size, 8 << 20);
                const bool mapped = cpu && used && g_memory.IsMapped(*cpu, used);
                const bool overlaps = mapped && overlaps_pending(*cpu, used);
                has_dependency = overlaps && !can_forward_pending(*cpu, used, !desc.is_written);
            }
        }
        if (has_dependency && !Flush(FlushReason::Compute)) return skip("compute-readback-dependency-flush-failed");
    }
    std::optional<HitchScope> phase_scope;
    phase_scope.emplace(kHitchSetup);
    if (!async_batch_.empty()) {
        // Uniform buffers and textures are read from guest memory: results still in the unsubmitted batch must go
        // through the ledger first.
        bool touches = !info.texture_descriptors.empty();
        for (const auto& desc : info.constant_buffer_descriptors) {
            for (u32 k = 0; k < desc.count && !touches; ++k) {
                const u32 slot = desc.index + k;
                if (slot == kTextureBoundBuffer || slot >= 18) continue;
                const auto [va, size] = bound_.uniform_buffers[kComputeNvnStage][slot];
                const auto cpu = va ? GpuToCpu(va) : std::nullopt;
                touches = cpu && size && AsyncBatchOverlaps(*cpu, std::min<u64>(size, 0x10000));
            }
        }
        if (touches && !SubmitAsyncBatch()) return skip("async-batch-submit-failed");
    }
    size_t buffer_info_capacity = 0;
    size_t image_info_capacity = 0;
    AccumulateDescriptorInfoCapacities(info, buffer_info_capacity, image_info_capacity);
    EndRenderPass();
    constexpr size_t s = kComputeNvnStage;
    std::vector<VkWriteDescriptorSet> writes;
    writes.reserve(buffer_info_capacity + image_info_capacity);
    DescriptorInfoStorage<VkDescriptorBufferInfo> buffer_infos;
    buffer_infos.Reserve(buffer_info_capacity);
    DescriptorInfoStorage<VkDescriptorImageInfo> image_infos;
    image_infos.Reserve(image_info_capacity);
    std::vector<PendingComputeWriteback> write_backs;
    const size_t ubo_align = vk_.properties.limits.minUniformBufferOffsetAlignment;
    const size_t ssbo_align = vk_.properties.limits.minStorageBufferOffsetAlignment;
    std::vector<u8> scratch;
    u32 binding = 0;
    DispatchDump& dump_cfg = DispatchDumpConfig();
    const bool dump_this = dump_cfg.Enabled(bound_.program);
    std::string dump_meta;
    ProgramTrace& ptrace = ProgramTrace::Instance();
    const bool trace_this = ptrace.Enabled() && bound_.program == ptrace.Program();
    ProgramTrace::Dispatch tdisp;
    if (trace_this) {
        tdisp.frame = present_count_;
        tdisp.index = compute_dispatches_;
        tdisp.program = bound_.program;
    }
    phase_scope.emplace(kHitchUbo);
    for (const auto& desc : info.constant_buffer_descriptors) {
        for (u32 k = 0; k < desc.count; ++k) {
            const u32 slot = desc.index + k;
            scratch.assign(256, 0);
            if (slot == kTextureBoundBuffer) {
                scratch.assign(0x20 + 8 * 32, 0);
                std::memcpy(scratch.data() + 0x20, bound_.textures[s].data(), 8 * 32);
            } else if (slot < 18) {
                const auto [va, size] = bound_.uniform_buffers[s][slot];
                const auto cpu = va ? GpuToCpu(va) : std::nullopt;
                if (cpu && size) {
                    scratch.resize(std::min<u64>(size, 0x10000));
                    if (g_memory.IsMapped(*cpu, scratch.size())) g_memory.ReadBlock(*cpu, scratch.data(), scratch.size());
                    g_hitch.ubo_read_bytes += scratch.size();
                }
            }
            if (trace_this && binding == 0 && scratch.size() >= 50 * 16)
                std::memcpy(tdisp.c0_49, scratch.data() + 49 * 16, 16);
            if (trace_this && binding == 1) {
                tdisp.c3_hash = ProgramTraceHash(scratch.data(), scratch.size());
                if (scratch.size() >= 44 * 16) std::memcpy(&tdisp.c3_43z, scratch.data() + 43 * 16 + 8, 4);
            }
            if (dump_this) {
                WriteDumpFile(fmt::format("{}/d{}-ubo-b{}-k{}.bin", dump_cfg.dir, dump_cfg.done, binding, k), scratch.data(),
                              scratch.size());
                dump_meta += fmt::format("ubo {} {} {}\n", binding, k, scratch.size());
            }
            const u64 offset = Upload(scratch.data(), scratch.size(), ubo_align);
            if (offset == ~u64{0}) return skip("stream-full");
            const auto* buffer_info = buffer_infos.Push({stream_.buffer, offset, scratch.size()});
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, binding, k, 1,
                              VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, buffer_info, nullptr});
        }
        ++binding;
    }
    phase_scope.emplace(kHitchSsbo);
    // NXRT_ASYNC_COMPUTE_FLUSH: the guest ranges of every storage buffer of this dispatch, to forward a pending
    // result in place only when no other binding of the dispatch touches the same bytes.
    std::vector<GuestMemoryRange> dispatch_ssbo_ranges;
    size_t dispatch_ssbo_index = 0;
    if (ledger_ && (ledger_->Pending() || !async_batch_.empty())) {
        for (const auto& desc : info.storage_buffers_descriptors) {
            for (u32 k = 0; k < desc.count; ++k) {
                const u32 off = desc.cbuf_offset + k * 16;
                const GpuAddr va = Pack(ReadCbuf(s, desc.cbuf_index, off), ReadCbuf(s, desc.cbuf_index, off + 4));
                const u32 size = ReadCbuf(s, desc.cbuf_index, off + 8);
                const auto cpu = va ? GpuToCpu(va) : std::nullopt;
                dispatch_ssbo_ranges.push_back(cpu ? GuestMemoryRange{*cpu, std::min<size_t>(size, 8 << 20)}
                                                   : GuestMemoryRange{0, 0});
            }
        }
    }
    for (const auto& desc : info.storage_buffers_descriptors) {
        for (u32 k = 0; k < desc.count; ++k) {
            const size_t ssbo_index = dispatch_ssbo_index++;
            const u32 off = desc.cbuf_offset + k * 16;
            const GpuAddr va = Pack(ReadCbuf(s, desc.cbuf_index, off), ReadCbuf(s, desc.cbuf_index, off + 4));
            const u32 size = ReadCbuf(s, desc.cbuf_index, off + 8);
            const auto cpu = va ? GpuToCpu(va) : std::nullopt;
            const size_t used = std::min<size_t>(size, 8 << 20);
            const size_t descriptor_range = std::max<size_t>(16, used);
            const bool mapped = cpu && used && g_memory.IsMapped(*cpu, used);
            if (chain_on_) ChainNoteSsbo(cpu ? *cpu : 0, used, desc.is_written, mapped);
            const auto forwarded_offset = mapped && forward_compute_writebacks_ && !dump_this && !desc.is_written
                                              ? FindForwardableComputeWriteback(
                                                    std::span<const PendingComputeWriteback>{
                                                        pending_compute_writebacks_.data(),
                                                        pending_compute_writebacks_.size()},
                                                    {*cpu, used}, true, ssbo_align)
                                              : std::nullopt;
            std::optional<u64> async_forwarded;
            if (mapped && !dump_this && ledger_ && ledger_->Pending() && ssbo_index < dispatch_ssbo_ranges.size() &&
                !AsyncBatchOverlaps(*cpu, used)) {
                bool alone = true;
                for (size_t other = 0; other < dispatch_ssbo_ranges.size() && alone; ++other) {
                    alone = other == ssbo_index ||
                            !GuestMemoryRangesOverlap(dispatch_ssbo_ranges[other], dispatch_ssbo_ranges[ssbo_index]);
                }
                if (alone) {
                    HitchScope fwd_scope(kHitchSsboFwd);
                    async_forwarded = ledger_->FindForward({*cpu, used}, ssbo_align);
                }
                if (async_forwarded) ++async_forwards_;
                if (async_forwarded) ++g_hitch.ssbo_fwd_n;
            }
            u64 offset = ~u64{0};
            bool ssbo_in_vram = false;  // composed straight into the VRAM mirror
            if (async_forwarded && !forwarded_offset) {
                offset = *async_forwarded;
            } else if (forwarded_offset) {
                offset = *forwarded_offset;
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
                static u64 forwarded_trace_count = 0;
                if (std::getenv("NXRT_TRACE_COMPUTE_READBACKS") && forwarded_trace_count++ < 128) {
                    LOG_INFO("renderer: compute-writeback-forward q=%llu program=0x%llx input=0x%llx/%zu "
                             "stream_offset=%llu",
                             static_cast<unsigned long long>(present_count_ + 1),
                             static_cast<unsigned long long>(bound_.program),
                             static_cast<unsigned long long>(*cpu), used,
                             static_cast<unsigned long long>(*forwarded_offset));
                }
#endif
            } else if (ssbo_direct_read_ && !dump_this) {
                // Read the guest bytes straight into the upload stream: no zero-filled temporary and no second copy.
                const size_t align = std::max<size_t>(ssbo_align, 4);
                const size_t at = (stream_offset_ + align - 1) / align * align;
                if (at + descriptor_range > kStreamSize) return skip("stream-full");
                u8* dst = stream_.mapped + at;
                const size_t have = mapped ? used : 0;
                bool composed = false;
                if (have && ssbo_gpu_compose_ && ledger_ && (ledger_->Pending() || !async_batch_.empty()) &&
                    ssbo_index < dispatch_ssbo_ranges.size()) {
                    bool alone = true;
                    for (size_t other = 0; other < dispatch_ssbo_ranges.size() && alone; ++other) {
                        alone = other == ssbo_index ||
                                !GuestMemoryRangesOverlap(dispatch_ssbo_ranges[other], dispatch_ssbo_ranges[ssbo_index]);
                    }
                    auto pieces = alone ? ledger_->CollectOverlaps({*cpu, have})
                                        : std::vector<AsyncWritebackLedger::PendingPiece>{};
                    if (alone) {
                        // Results of this command buffer's earlier dispatches are newer than anything submitted.
                        for (const PendingComputeWriteback& w : async_batch_)
                            if (GuestMemoryRangesOverlap({*cpu, have}, {w.cpu, w.size}))
                                pieces.push_back({{w.cpu, w.size}, w.stream_offset});
                    }
                    if (!pieces.empty()) {
                        // Bytes outside the pending pieces come from the guest as they are; the pieces' own guest
                        // bytes are stale and get overwritten on the GPU before the dispatch reads them.
                        std::memcpy(dst, g_memory.RawPtr(*cpu), have);
                        VkCommandBuffer cb = Cmd();
                        const VkMemoryBarrier to_copy{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                                                      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                             &to_copy, 0, nullptr, 0, nullptr);
                        const VkBuffer piece_buffer = ssbo_vram_ ? dstream_.buffer : stream_.buffer;
                        if (ssbo_vram_) {
                            // Guest bytes first, then the pending pieces (already in VRAM) over them.
                            const VkBufferCopy whole{at, at, descriptor_range};
                            vkCmdCopyBuffer(cb, stream_.buffer, dstream_.buffer, 1, &whole);
                            const VkMemoryBarrier after_whole{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                              VK_ACCESS_TRANSFER_WRITE_BIT,
                                                              VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
                            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                                                 &after_whole, 0, nullptr, 0, nullptr);
                        }
                        const u64 lo_read = *cpu, hi_read = *cpu + have;
                        for (size_t i = 0; i < pieces.size(); ++i) {
                            const auto& piece = pieces[i];
                            const u64 lo = std::max<u64>(lo_read, piece.range.address);
                            const u64 hi = std::min<u64>(hi_read, piece.range.address + piece.range.size);
                            if (hi <= lo) continue;
                            if (i) {
                                // Newer pieces overwrite older ones where they overlap: order the copies.
                                const VkMemoryBarrier between{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                                              VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
                                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                                     1, &between, 0, nullptr, 0, nullptr);
                            }
                            const VkBufferCopy region{piece.stream_offset + (lo - piece.range.address), at + (lo - lo_read),
                                                      hi - lo};
                            vkCmdCopyBuffer(cb, piece_buffer, piece_buffer, 1, &region);
                        }
                        // The dispatch's own "before" barrier (memory write -> shader read) orders the copies.
                        composed = true;
                        ssbo_in_vram = ssbo_vram_;
                        ++ssbo_composed_;
                    }
                }
                if (have && !composed && AsyncBatchOverlaps(*cpu, have) && !SubmitAsyncBatch())
                    return skip("async-batch-submit-failed");
                if (have && !composed) {
                    if (kHitchMs > 0.0) {
                        std::string cls = "none";
                        if (ledger_ && ledger_->Pending()) {
                            const auto ov = ledger_->DescribeOverlap({*cpu, have});
                            bool alone = true;
                            for (size_t other = 0; ssbo_index < dispatch_ssbo_ranges.size() && other < dispatch_ssbo_ranges.size() && alone; ++other)
                                alone = other == ssbo_index || !GuestMemoryRangesOverlap(dispatch_ssbo_ranges[other],
                                                                                         dispatch_ssbo_ranges[ssbo_index]);
                            if (ov.overlapping) {
                                const u64 a0 = *cpu, a1 = *cpu + have, b0 = ov.newest.address,
                                          b1 = ov.newest.address + ov.newest.size;
                                cls = a0 == b0 && a1 == b1 ? "exact" : (b0 <= a0 && a1 <= b1) ? "inside-pending"
                                    : (a0 <= b0 && b1 <= a1) ? "covers-pending" : "partial";
                                if (a0 == b0 && a1 == b1 && ov.newest_stream_offset % ssbo_align) cls = "exact-misaligned";
                                if (!alone) cls += "+not-alone";
                                cls += desc.is_written ? "+w" : "+r";
                                static u32 shown = 0;
                                if (shown < 24) {
                                    ++shown;
                                    LOG_INFO("hitch-guard q=%llu program=%llx ssbo=%zu read=%llx+%zx pending=%llx+%llx n=%zu class=%s",
                                             static_cast<unsigned long long>(present_count_ + 1),
                                             static_cast<unsigned long long>(bound_.program), ssbo_index,
                                             static_cast<unsigned long long>(*cpu), have,
                                             static_cast<unsigned long long>(ov.newest.address),
                                             static_cast<unsigned long long>(ov.newest.size), ov.overlapping, cls.c_str());
                                }
                            }
                        }
                        const auto t0 = std::chrono::steady_clock::now();
                        {
                            HitchScope guard_scope(kHitchSsboGuard, u64{have});
                            g_memory.Guard(*cpu, have, false);
                        }
                        auto& gc = g_hitch.guard_class[cls];
                        gc.first += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                        ++gc.second;
                    }
                    HitchScope read_scope(kHitchSsboRead, u64{have});
                    g_memory.ReadBlock(*cpu, dst, have);
                }
                if (descriptor_range > have) std::memset(dst + have, 0, descriptor_range - have);
                offset = at;
                stream_offset_ = at + descriptor_range;
                g_hitch.ssbo_read_bytes += used, ++g_hitch.ssbo_read_n;
            } else {
                scratch.assign(descriptor_range, 0);
                if (mapped) g_memory.ReadBlock(*cpu, scratch.data(), used);
                offset = Upload(scratch.data(), scratch.size(), ssbo_align);
                if (offset == ~u64{0}) return skip("stream-full");
            }
            if (trace_this && desc.is_written && mapped) {
                tdisp.ssbo_va = va;
                tdisp.ssbo_cpu = *cpu;
                tdisp.ssbo_size = used;
                ptrace.Begin(tdisp, stream_.mapped + offset, used);
            }
            if (dump_this) {
                WriteDumpFile(fmt::format("{}/d{}-ssbo-b{}-k{}.bin", dump_cfg.dir, dump_cfg.done, binding, k), scratch.data(),
                              scratch.size());
                dump_meta += fmt::format("ssbo {} {} {} guest_va={:x} guest_size={} mapped={} written={}\n", binding, k,
                                         scratch.size(), static_cast<u64>(va), size, mapped ? 1 : 0, desc.is_written ? 1 : 0);
            }
            if (desc.is_written && mapped) {
                write_backs.push_back({bound_.program, present_count_ + 1, compute_dispatches_, offset, *cpu, used});
                if (census_) census_->ComputeWriteback(used);
            }
            if (ssbo_vram_ && !async_forwarded && !ssbo_in_vram) {
                VkCommandBuffer cb = Cmd();
                const VkBufferCopy whole{offset, offset, descriptor_range};
                vkCmdCopyBuffer(cb, stream_.buffer, dstream_.buffer, 1, &whole);
            }
            const auto* buffer_info =
                buffer_infos.Push({ssbo_vram_ ? dstream_.buffer : stream_.buffer, offset, descriptor_range});
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, binding, k, 1,
                              VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, buffer_info, nullptr});
        }
        ++binding;
    }
    for (const auto& desc : info.texture_descriptors) {
        for (u32 k = 0; k < desc.count; ++k) {
            u32 handle = ReadCbuf(s, desc.cbuf_index, desc.cbuf_offset + (k << desc.size_shift));
            handle <<= desc.shift_left;
            if (desc.has_secondary) {
                handle |= ReadCbuf(s, desc.secondary_cbuf_index, desc.secondary_cbuf_offset)
                          << desc.secondary_shift_left;
            }
            std::optional<Tic> sampled_tic;
            HostImage* img = SampledImage(handle & 0xFFFFF, handle, false, ~u32{0}, &sampled_tic);
            if (img) img->last_use_frame = present_count_;
            VkSampler sampler = SamplerFor((handle >> 20) & 0xFFF, std::nullopt, desc.is_depth, desc.is_integer,
                                           img && (img->aspect & VK_IMAGE_ASPECT_DEPTH_BIT));
            const bool invalid_guest_descriptor = texture_error_ == "invalid-descriptor" ||
                                                  texture_error_ == "invalid-descriptor-address";
            if (!img && resolve_invalid_textures_ && desc.type == Shader::TextureType::Color2D &&
                !desc.is_depth && !desc.is_integer && invalid_guest_descriptor) {
                img = &null_2d_;
                sampler = null_2d_sampler_;
                texture_error_.clear();
            }
            if (!img || !sampler) return skip(!sampler ? "sampler" : "texture:" + texture_error_);
            const auto* image_info = image_infos.Push(
                {sampler, SampleView(*img, handle & 0xFFFFF, sampled_tic ? &*sampled_tic : nullptr),
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
            writes.push_back({VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, VK_NULL_HANDLE, binding, k, 1,
                              VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, image_info, nullptr, nullptr});
        }
        ++binding;
    }
    if (chain_on_) {
        chain_.chain_hash = FastHash(&chain_.fp, sizeof(chain_.fp), chain_.chain_hash);
        chain_.fps.push_back(chain_.fp);
        if (chain_.count++ == 0) {
            chain_.first_q = present_count_ + 1;
            if (ledger_) {
                const AsyncWritebackStats st = ledger_->Stats();
                chain_.loads0 = st.intercepted[static_cast<size_t>(AsyncMaterializeCause::GuestLoad)];
                chain_.stores0 = st.intercepted[static_cast<size_t>(AsyncMaterializeCause::GuestStore)];
            }
        }
        if (chain_.count == kChainPrefix) {
            // The key of a run: its first kChainPrefix dispatches and the contents of what they read.
            chain_.prefix_input_hash = chain_.input_hash;
            // The entry is found by the command sequence alone; what the run read is checked against it afterwards (and the
            // leftovers in scratch buffers, which differ from run to run, are learned and ignored: see ChainFinish).
            u64 key = FastHash(chain_.fps.data(), chain_.fps.size() * sizeof(u64), 0x5eed);
            if (key == 0) key = 1;
            chain_.key = key;
            if (chain_cache_enabled_ && !chain_.no_save) {
                auto entry = std::make_unique<ChainEntry>();
                bool inputs_match = false;
                if (LoadChainEntry(key, *entry) && entry->count >= kChainPrefix &&
                    std::equal(chain_.fps.begin(), chain_.fps.end(), entry->fingerprints.begin())) {
                    // Every stretch the stored run read, in the state it had when first touched, must be what this run sees -
                    // except stretches learned to hold leftovers. Those first touched during the prefix have already been
                    // modified by the dispatches executed so far: compare the state recorded when this run first touched them.
                    inputs_match = true;
                    std::vector<u8> bytes;
                    for (const ChainInput& in : entry->inputs) {
                        if (in.flags & kChainInputVolatile) continue;
                        if (in.first_dispatch < kChainPrefix) {
                            bool found = false;
                            for (const ChainInput& live : chain_.inputs) {
                                if (live.address == in.address && live.size == in.size) {
                                    found = live.hash == in.hash;
                                    break;
                                }
                            }
                            if (!found) { inputs_match = false; break; }
                            continue;
                        }
                        if (!g_memory.IsMapped(in.address, in.size)) { inputs_match = false; break; }
                        bytes.resize(in.size);
                        g_memory.ReadBlock(in.address, bytes.data(), in.size);
                        if (FastHash(bytes.data(), bytes.size(), in.address) != in.hash) { inputs_match = false; break; }
                    }
                    if (!inputs_match) {
                        LOG_INFO("chain-cache: stored run key=%016llx not used: its inputs differ (this run will be recorded and the differences learned)",
                                 static_cast<unsigned long long>(key));
                    }
                }
                if (inputs_match) {
                    LOG_INFO("chain-cache: recognised stored run q=%llu key=%016llx dispatches=%llu; skipping the rest",
                             static_cast<unsigned long long>(present_count_ + 1), static_cast<unsigned long long>(key),
                             static_cast<unsigned long long>(entry->count));
                    chain_.skip_index = chain_.count;
                    chain_prefix_ns_ = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - chain_started_).count();
                    chain_.entry = std::move(entry);
                    chain_.skipping = true;
                    if (chain_.skip_index == chain_.entry->count) ChainApplyEntry();
                }
            }
        }
    }
    phase_scope.emplace(kHitchRecord);
    VkCommandBuffer cmd = Cmd();
    VkDescriptorSet set = VK_NULL_HANDLE;
    const VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, nullptr, descriptors_, 1,
                                          &p->set_layout};
    {
        HitchScope desc_scope(kHitchDescriptors);
        if (vkAllocateDescriptorSets(vk_.device, &dai, &set) != VK_SUCCESS) return skip("descriptor-pool");
        ++sets_in_pool_;
        if (trace_cflush_) ++trace_sets_;
        for (auto& w : writes) w.dstSet = set;
        vkUpdateDescriptorSets(vk_.device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
    }
    // Earlier draws and dispatches finish before this one reads what they wrote.
    const VkMemoryBarrier before{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_MEMORY_WRITE_BIT,
                                 VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &before,
                         0, nullptr, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, nullptr);
    std::array<u8, sizeof(Shader::Backend::SPIRV::RescalingLayout)> push{};
    const float one = 1.0f;
    std::memcpy(push.data() + Shader::Backend::SPIRV::RESCALING_LAYOUT_DOWN_FACTOR_OFFSET, &one, sizeof(one));
    vkCmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, static_cast<u32>(push.size()), push.data());
    {
        char buf[160];
        std::snprintf(buf, sizeof buf, "program=0x%llx groups=%llux%llux%llu",
                      static_cast<unsigned long long>(bound_.program), static_cast<unsigned long long>(c.a[0]),
                      static_cast<unsigned long long>(c.a[1]), static_cast<unsigned long long>(c.a[2]));
        if (recent_dispatches_.size() >= 64) recent_dispatches_.erase(recent_dispatches_.begin());
        recent_dispatches_.emplace_back(buf);
    }
    if (dump_this) {
        dump_meta += fmt::format("groups {} {} {}\nprogram {:x}\n", c.a[0], c.a[1], c.a[2], bound_.program);
        dump_meta += fmt::format("storage_base_compute {}\n", StorageBase(kComputeNvnStage));
        for (u32 i = 0; i < 16; ++i) {
            const auto [va, size] = bound_.storage_buffers[kComputeNvnStage][i];
            if (va || size) dump_meta += fmt::format("bound_storage[{}] va={:x} size={}\n", i, va, size);
        }
        for (u32 st = 0; st < 5; ++st)
            dump_meta += fmt::format("storage_base_stage{} {}\n", st, StorageBase(st));
        WriteDumpFile(fmt::format("{}/d{}-meta.txt", dump_cfg.dir, dump_cfg.done), dump_meta.data(), dump_meta.size());
        LOG_INFO("renderer: dispatch dump #%u written to %s", dump_cfg.done, dump_cfg.dir.c_str());
        if (dump_cfg.done == 0) nvn::DumpStats();  // which NVN calls ran so far (to spot unobserved variants)
        ++dump_cfg.done;
    }
    const bool dispatch_ts = gpu_ts_ && ts_recorded_;
    const bool dispatch_ts_slot = dispatch_ts && dispatch_ts_records_.size() < kMaxDispatchTs;
    if (dispatch_ts_slot) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, ts_pool_,
                            2 + 2 * static_cast<u32>(dispatch_ts_records_.size()));
    } else if (dispatch_ts) {
        ++profile_dispatch_ts_skipped_;
    }
    vkCmdDispatch(cmd, static_cast<u32>(c.a[0]), static_cast<u32>(c.a[1]), static_cast<u32>(c.a[2]));
    if (dispatch_ts_slot) {
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, ts_pool_,
                            3 + 2 * static_cast<u32>(dispatch_ts_records_.size()));
        dispatch_ts_records_.push_back({bound_.program, {c.a[0], c.a[1], c.a[2]}});
    }
    if (ssbo_vram_ && !write_backs.empty()) {
        const VkMemoryBarrier to_back{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr, VK_ACCESS_SHADER_WRITE_BIT,
                                      VK_ACCESS_TRANSFER_READ_BIT};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &to_back, 0,
                             nullptr, 0, nullptr);
        for (const PendingComputeWriteback& w : write_backs) {
            const VkBufferCopy back{w.stream_offset, w.stream_offset, w.size};
            vkCmdCopyBuffer(cmd, dstream_.buffer, stream_.buffer, 1, &back);
        }
    }
    const VkMemoryBarrier after{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                                VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &after, 0, nullptr, 0,
                         nullptr);
    phase_scope.emplace(kHitchPost);
    if (sync_dispatch) {
        LOG_INFO("dispatch-sync begin program=0x%llx groups=%llux%llux%llu",
                 static_cast<unsigned long long>(bound_.program), static_cast<unsigned long long>(c.a[0]),
                 static_cast<unsigned long long>(c.a[1]), static_cast<unsigned long long>(c.a[2]));
        std::fflush(stderr);
        if (!Flush(FlushReason::Compute)) return;
        LOG_INFO("dispatch-sync ok");
        if (write_backs.empty()) return;
        for (const PendingComputeWriteback& w : write_backs) {
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
            ComputeReadbackTrace::Instance().PrepareWriteback(w.program, w.frame, w.cpu, w.size);
#endif
            ProgramTraceBeforeWriteback(w.program, w.cpu, w.size);
            g_memory.WriteBlock(w.cpu, stream_.mapped + w.stream_offset, w.size);
            ProgramTraceAfterWriteback(w.program, w.dispatch, w.cpu, stream_.mapped + w.stream_offset, w.size);
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
            ComputeReadbackTrace::Instance().RecordWriteback(w.program, w.frame, w.cpu, w.size);
#endif
        }
        return;
    }
    if (write_backs.empty()) return;
    if (batch_compute_writebacks_) {
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        if (forward_compute_writebacks_) {
            for (const PendingComputeWriteback& w : write_backs) {
                ComputeReadbackTrace::Instance().QueuePendingWriteback(w.program, w.frame, w.cpu, w.size);
            }
        }
#endif
        pending_compute_writebacks_.insert(pending_compute_writebacks_.end(), write_backs.begin(), write_backs.end());
        constexpr size_t kMaximumPendingWritebacks = 4096;
        if (pending_compute_writebacks_.size() >= kMaximumPendingWritebacks && !Flush(FlushReason::Compute)) {
            return skip("compute-writeback-batch-flush-failed");
        }
        return;
    }
    if (async_compute_) {
        // Submit without waiting; the results are copied to guest memory when something needs them, when the fence
        // is seen signalled by PollAsync, or at the latest before Execute returns. Falls back to the code below.
        if (async_batch_max_ > 1) {
            async_batch_.insert(async_batch_.end(), write_backs.begin(), write_backs.end());
            ++async_batch_dispatches_;
            if (async_batch_dispatches_ >= async_batch_max_ || stream_offset_ > kAsyncStreamLimit / 2 ||
                sets_in_pool_ > kAsyncDescriptorSetLimit / 2) {
                SubmitAsyncBatch();
            }
            return;
        }
        if (TryFlushAsyncCompute(write_backs, true)) return;
    }
    static const u64 profile_writeback_from_frame = [] {
        const char* value = std::getenv("NXRT_PROFILE_COMPUTE_WRITEBACK_Q");
        return value && *value ? std::strtoull(value, nullptr, 0) : 0;
    }();
    static u64 profile_writeback_count = 0;
    const bool profile_writeback = frame_profile_ && profile_writeback_from_frame &&
                                   present_count_ + 1 >= profile_writeback_from_frame &&
                                   profile_writeback_count < 64;
    size_t writeback_bytes = 0;
    std::string writeback_ranges;
    if (profile_writeback) {
        for (const PendingComputeWriteback& w : write_backs) {
            writeback_bytes += w.size;
            if (!writeback_ranges.empty()) writeback_ranges += ',';
            writeback_ranges += fmt::format("{:x}/{}", static_cast<u64>(w.cpu), w.size);
        }
    }
    const u64 flush_wait_before = frame_flush_wait_ns_;
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    const u32 trace_idx = trace_flush_frame_ == present_count_ + 1 ? trace_flush_index_ : 0;
    if (trace_cflush_) {
        for (const PendingComputeWriteback& w : write_backs) {
            trace_flush_ranges_.emplace_back(w.cpu, w.size);
            ComputeReadbackTrace::Instance().NoteInFlight(w.program, w.frame, w.cpu, w.size, trace_idx);
        }
    }
#endif
    if (!Flush(FlushReason::Compute)) return;
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    if (trace_cflush_) {
        for (const PendingComputeWriteback& w : write_backs) {
            ComputeReadbackTrace::Instance().NoteDone(w.program, w.frame, w.cpu, w.size, trace_submit_);
        }
    }
#endif
    if (profile_writeback) {
        const u64 flush_wait_ns = frame_flush_wait_ns_ - flush_wait_before;
        LOG_INFO("renderer: compute-writeback q=%llu program=0x%llx groups=%llux%llux%llu buffers=%zu bytes=%zu "
                 "ranges=%s flush_wait=%.3fms",
                 static_cast<unsigned long long>(present_count_ + 1),
                 static_cast<unsigned long long>(bound_.program), static_cast<unsigned long long>(c.a[0]),
                 static_cast<unsigned long long>(c.a[1]), static_cast<unsigned long long>(c.a[2]), write_backs.size(),
                 writeback_bytes, writeback_ranges.c_str(), static_cast<double>(flush_wait_ns) / 1'000'000.0);
        ++profile_writeback_count;
    }
    for (const PendingComputeWriteback& w : write_backs) {
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        ComputeReadbackTrace::Instance().PrepareWriteback(w.program, w.frame, w.cpu, w.size);
#endif
        ProgramTraceBeforeWriteback(w.program, w.cpu, w.size);
        g_memory.WriteBlock(w.cpu, stream_.mapped + w.stream_offset, w.size);
        ProgramTraceAfterWriteback(w.program, w.dispatch, w.cpu, stream_.mapped + w.stream_offset, w.size);
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
        ComputeReadbackTrace::Instance().RecordWriteback(w.program, w.frame, w.cpu, w.size);
#endif
    }
}

void NvnRenderer::TraceDispatch(const Command& c) {
    // NXRT_TRACE_COMPUTE: once per program, the NVN shader data header and the start of its control section, to
    // locate the workgroup size and shared memory the driver would put in the launch descriptor.
    static const bool trace = std::getenv("NXRT_TRACE_COMPUTE") != nullptr;
    static std::set<u64> traced;
    if (!trace || traced.size() >= 8 || !traced.insert(bound_.program).second) return;
    const auto prog = RenderModel().programs.find(bound_.program);
    if (prog == RenderModel().programs.end()) {
        LOG_INFO("trace-compute: dispatch %llux%llux%llu program=%llx unknown", static_cast<unsigned long long>(c.a[0]),
                 static_cast<unsigned long long>(c.a[1]), static_cast<unsigned long long>(c.a[2]),
                 static_cast<unsigned long long>(bound_.program));
        return;
    }
    for (const nvn::ShaderStage& stage : prog->second.stages) {
        const auto code = GpuToCpu(stage.code);
        const auto words = [](GuestAddr at, u32 count) {
            std::string out;
            for (u32 i = 0; i < count; ++i) {
                char buf[12];
                std::snprintf(buf, sizeof buf, " %08x", g_memory.IsMapped(at + 4 * i, 4) ? g_memory.Read<u32>(at + 4 * i) : 0);
                out += buf;
            }
            return out;
        };
        LOG_INFO("trace-compute: dispatch %llux%llux%llu program=%llx serial=%llu stages=%zu code=%llx head:%s",
                 static_cast<unsigned long long>(c.a[0]), static_cast<unsigned long long>(c.a[1]),
                 static_cast<unsigned long long>(c.a[2]), static_cast<unsigned long long>(bound_.program),
                 static_cast<unsigned long long>(prog->second.serial), prog->second.stages.size(),
                 static_cast<unsigned long long>(stage.code), code ? words(*code, 32).c_str() : " unmapped");
        for (u32 row = 0; row < 0x800 / 4 && stage.control; row += 16) {
            LOG_INFO("trace-compute: control +%03x:%s", row * 4, words(stage.control + row * 4, 16).c_str());
        }
    }
}

// ------------------------------------------------------------------ lists

void NvnRenderer::MaybeYieldModelLock() {
    if (execution_model_ || !execute_locks_ || (++yield_tick_ & 15) != 0 || model_.lock.waiters() == 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - model_lock_taken_ < std::chrono::microseconds(400)) return;
    auto& lock = execute_locks_->ModelLock();
    if (!lock.owns_lock()) return;
    lock.unlock();
    const auto until = now + std::chrono::microseconds(250);
    while (model_.lock.waiters() > 0 && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    lock.lock();
    model_lock_taken_ = std::chrono::steady_clock::now();
    ++model_lock_yields_;
}

void NvnRenderer::ExecuteList(const CommandList& list, int depth, u64 list_handle) {
    if (depth == 0) clear_gate_open_ = true;
    const bool has_origins = list.origins.size() == list.commands.size();
    for (size_t command_index = 0; command_index < list.commands.size(); ++command_index) {
        const Command& c = list.commands[command_index];
        if (model_lock_yield_ && depth == 0) MaybeYieldModelLock();  // nested lists may point into RenderModel().finished
        const u64 command_list_handle = has_origins ? list.origins[command_index].list_handle : list_handle;
        const int command_depth = has_origins ? static_cast<int>(list.origins[command_index].depth) : depth;
        if (broken_) return;
        if (chain_on_ && (chain_.count || chain_.skipping)) {
            switch (c.op) {
            case Op::BindBlendState: case Op::BindColorState: case Op::BindChannelMaskState:
            case Op::BindDepthStencilState: case Op::BindPolygonState: case Op::BindMultisampleState:
            case Op::BindVertexAttribState: case Op::BindVertexStreamState: case Op::BindProgram:
            case Op::BindVertexBuffer: case Op::BindUniformBuffer: case Op::BindStorageBuffer: case Op::BindTexture:
            case Op::SetRenderTargets: case Op::SetViewport: case Op::SetScissor: case Op::SetDepthRange:
            case Op::SetBlendColor: case Op::SetTexturePool: case Op::SetSamplerPool: case Op::Barrier:
            case Op::CallCommands: case Op::SetRenderEnable: case Op::DispatchCompute:
                break;
            default:
                ChainFinish();
                break;
            }
        }
        if (!async_batch_.empty()) {
            switch (c.op) {
            case Op::BindBlendState: case Op::BindColorState: case Op::BindChannelMaskState:
            case Op::BindDepthStencilState: case Op::BindPolygonState: case Op::BindMultisampleState:
            case Op::BindVertexAttribState: case Op::BindVertexStreamState: case Op::BindProgram:
            case Op::BindVertexBuffer: case Op::BindUniformBuffer: case Op::BindStorageBuffer: case Op::BindTexture:
            case Op::SetRenderTargets: case Op::SetViewport: case Op::SetScissor: case Op::SetDepthRange:
            case Op::SetBlendColor: case Op::SetTexturePool: case Op::SetSamplerPool: case Op::Barrier:
            case Op::CallCommands: case Op::SetRenderEnable: case Op::DispatchCompute:
                break;
            default:
                if (!SubmitAsyncBatch()) return;
                break;
            }
        }
        if (batch_compute_writebacks_ && !pending_compute_writebacks_.empty()) {
            bool state_only = false;
            switch (c.op) {
            case Op::BindBlendState:
            case Op::BindColorState:
            case Op::BindChannelMaskState:
            case Op::BindDepthStencilState:
            case Op::BindPolygonState:
            case Op::BindMultisampleState:
            case Op::BindVertexAttribState:
            case Op::BindVertexStreamState:
            case Op::BindProgram:
            case Op::BindVertexBuffer:
            case Op::BindUniformBuffer:
            case Op::BindStorageBuffer:
            case Op::BindTexture:
            case Op::SetRenderTargets:
            case Op::SetViewport:
            case Op::SetScissor:
            case Op::SetDepthRange:
            case Op::SetBlendColor:
            case Op::SetTexturePool:
            case Op::SetSamplerPool:
            case Op::Barrier:
            case Op::CallCommands:
            case Op::SetRenderEnable:
            case Op::DispatchCompute:
                state_only = true;
                break;
            default:
                break;
            }
            if (!state_only && !Flush(FlushReason::Compute)) return;
        }
        switch (c.op) {
        case Op::BindBlendState: {
            const u32 target = c.words[0] & 7;
            bound_.blend_words[target] = {c.words[0], c.words[1]};
            bound_.blend_set[target] = true;
            break;
        }
        case Op::BindColorState: bound_.color_word = c.words[0]; break;
        case Op::BindChannelMaskState: bound_.channel_mask = c.words[0]; break;
        case Op::BindDepthStencilState: bound_.depth_stencil = {c.words[0], c.words[1]}; break;
        case Op::BindPolygonState: bound_.polygon = c.words[0]; break;
        case Op::BindVertexAttribState: bound_.attribs = c.words; break;
        case Op::BindVertexStreamState: bound_.streams = c.words; break;
        case Op::BindProgram:
            bound_.program = c.a[0];
            if (auto p = RenderModel().programs.find(c.a[0]); p != RenderModel().programs.end()) {
                bound_.program_serial = p->second.serial;
            }
            break;
        case Op::BindVertexBuffer:
            if (c.a[0] < bound_.vertex_buffers.size()) bound_.vertex_buffers[c.a[0]] = {c.a[1], c.a[2]};
            break;
        case Op::BindUniformBuffer:
            // a[4] != 0: a[1] is already a Maxwell const buffer slot (a driver buffer such as the shader's c1).
            if (const u64 slot = c.a[4] ? c.a[1] : c.a[1] + kUniformSlotBase; c.a[0] < 6 && slot < 18) {
                bound_.uniform_buffers[c.a[0]][slot] = {c.a[2], c.a[3]};
                bound_.uniform_buffer_bind_origin[c.a[0]][slot] =
                    static_cast<nvn::UniformBufferBindOrigin>(c.a[5]);
            }
            break;
        case Op::UpdateBuffer:
            // Inline const-buffer write from the game's own command writer: the GPU would perform it here, in order.
            if (const auto cpu = GpuToCpu(c.a[0]); cpu && g_memory.IsMapped(*cpu, c.words.size() * 4)) {
                for (size_t i = 0; i < c.words.size(); ++i) g_memory.Write<u32>(*cpu + 4 * i, c.words[i]);
            }
            break;
        case Op::BindStorageBuffer:
            if (c.a[0] < 6 && c.a[1] < 16) bound_.storage_buffers[c.a[0]][c.a[1]] = {c.a[2], c.a[3]};
            break;
        case Op::BindTexture:
        case Op::BindTexturesDeferred: {
            const bool applied = nvn::ApplyTextureBinding(c, bound_.textures, [](GuestAddr address) -> std::optional<u64> {
                if (!g_memory.IsMapped(address, sizeof(u64))) return std::nullopt;
                return g_memory.Read<u64>(address);
            });
            const auto& trace = GetWinSampleTraceConfig();
            const u64 frame = present_count_ + 1;
            const bool trace_frame = trace.frame && frame == *trace.frame;
            const bool trace_program = !trace.frame && trace.program && *trace.program == bound_.program;
            if (c.op == Op::BindTexturesDeferred && trace.enabled && trace.valid && (trace_frame || trace_program)) {
                // A bind can precede BindProgram. An explicit frame therefore traces
                // every deferred range, retaining the program observed at that point.
                static u64 traced_frame = ~u64{0};
                static u32 trace_events = 0;
                constexpr u32 kDeferredTraceCap = 4096;
                if (traced_frame != frame) { traced_frame = frame; trace_events = 0; }
                if (trace_events >= kDeferredTraceCap) {
                    if (trace_events == kDeferredTraceCap) {
                        LOG_INFO("trace-deferred-textures: q=%llu truncated after %u events",
                                 static_cast<unsigned long long>(frame), kDeferredTraceCap);
                        ++trace_events;
                    }
                    break;
                }
                ++trace_events;
                std::ostringstream handles;
                if (applied) {
                    for (u64 i = 0; i < c.a[2]; ++i) {
                        handles << (i ? "," : "") << "0x" << std::hex << bound_.textures[c.a[0]][c.a[1] + i];
                    }
                }
                LOG_INFO("trace-deferred-textures: q=%llu program=0x%llx list=0x%llx depth=%d "
                         "stage=%llu first=%llu count=%llu pointer=0x%llx applied=%d handles=%s",
                         static_cast<unsigned long long>(frame),
                         static_cast<unsigned long long>(bound_.program),
                         static_cast<unsigned long long>(command_list_handle), command_depth,
                         static_cast<unsigned long long>(c.a[0]), static_cast<unsigned long long>(c.a[1]),
                         static_cast<unsigned long long>(c.a[2]), static_cast<unsigned long long>(c.a[3]), applied,
                         handles.str().c_str());
            }
            break;
        }
        case Op::SetViewport:
            bound_.viewport = c.a[4] ? c.f : std::array<float, 4>{
                static_cast<float>(static_cast<s32>(c.a[0])), static_cast<float>(static_cast<s32>(c.a[1])),
                static_cast<float>(static_cast<s32>(c.a[2])), static_cast<float>(static_cast<s32>(c.a[3]))};
            break;
        case Op::SetScissor:
            if (const auto& cc = GetDrawCensusConfig(); cc.frame && present_count_ + 1 >= cc.frame && present_count_ + 1 <= cc.frame_end) {
                LOG_INFO("census-setsc q=%llu after_seq=%llu xywh=%llu,%llu,%llu,%llu",
                         static_cast<unsigned long long>(present_count_ + 1),
                         static_cast<unsigned long long>(census_seq_frame_ == present_count_ + 1 ? census_seq_ : 0),
                         static_cast<unsigned long long>(c.a[0]), static_cast<unsigned long long>(c.a[1]),
                         static_cast<unsigned long long>(c.a[2]), static_cast<unsigned long long>(c.a[3]));
            }
            bound_.scissor = {static_cast<u32>(c.a[0]), static_cast<u32>(c.a[1]), static_cast<u32>(c.a[2]),
                              static_cast<u32>(c.a[3])};
            break;
        case Op::SetDepthRange: bound_.depth_range = {c.f[0], c.f[1]}; break;
        case Op::SetBlendColor: bound_.blend_color = c.f; break;
        case Op::SetTexturePool: bound_.texture_pool = c.a[0]; break;
        case Op::SetSamplerPool: bound_.sampler_pool = c.a[0]; break;
        case Op::SetRenderTargets:
            EndRenderPass();
            num_color_targets_ = static_cast<u32>(std::min<u64>(c.a[0], color_targets_.size()));
            for (u32 i = 0; i < num_color_targets_; ++i) {
                color_targets_[i] = Pack(c.words[4 * i], c.words[4 * i + 1]);
                const size_t level_word = 4 * static_cast<size_t>(c.a[0]) + i;
                color_levels_[i] = target_levels_ && level_word < c.words.size() ? c.words[level_word] : 0;
                const size_t layer_word = 5 * static_cast<size_t>(c.a[0]) + i;
                color_layers_[i] = target_layers_ && layer_word < c.words.size() ? c.words[layer_word] : 0;
                const size_t format_word = 6 * static_cast<size_t>(c.a[0]) + i;
                color_view_formats_[i] = format_word < c.words.size() ? c.words[format_word] : 0;
            }
            depth_target_ = c.a[1];
            depth_level_ = target_levels_ ? static_cast<u32>(c.a[3]) : 0;
            depth_layer_ = target_layers_ ? static_cast<u32>(c.a[4]) : 0;
            if (const auto& cc = GetDrawCensusConfig(); cc.frame && present_count_ + 1 >= cc.frame && present_count_ + 1 <= cc.frame_end) {
                LOG_INFO("census-setrt q=%llu after_seq=%llu ncolor=%u c0key=%llx dskey=%llx dsguest=%llx",
                         static_cast<unsigned long long>(present_count_ + 1),
                         static_cast<unsigned long long>(census_seq_frame_ == present_count_ + 1 ? census_seq_ : 0),
                         num_color_targets_,
                         static_cast<unsigned long long>(num_color_targets_ ? ImageKey(color_targets_[0]) : 0),
                         static_cast<unsigned long long>(depth_target_ ? ImageKey(depth_target_) : 0),
                         static_cast<unsigned long long>(depth_target_));
            }
            if (render_stats_) {
                std::string label = "setrt:";
                for (u32 i = 0; i < num_color_targets_; ++i) label += (i ? "+" : "") + TargetLabel(color_targets_[i], color_levels_[i]);
                if (!num_color_targets_) label += "none";
                label += depth_target_ ? " +depth" : "";
                ++pass_stats_[label];
            }
            if (kEnvTraceDraws) {
                static u32 emitted = 0;
                if (emitted++ < 64) {
                    std::ostringstream targets;
                    targets << std::hex;
                    for (u32 i = 0; i < num_color_targets_; ++i) {
                        const GuestAddr target = color_targets_[i];
                        targets << " [" << std::dec << i << "]=0x" << std::hex << target << "/key=0x"
                                << ImageKey(target);
                        if (auto desc = RenderModel().textures.find(target); desc != RenderModel().textures.end()) {
                            targets << "/" << desc->second.desc.width << "x" << desc->second.desc.height << "/fmt=0x"
                                    << std::hex << desc->second.desc.format << "/storage=0x" << desc->second.desc.pool
                                    << "+0x" << desc->second.desc.offset << "/serial=" << std::dec << desc->second.serial;
                        }
                    }
                    LOG_INFO("trace: setrt list=%llx depth=%d count=%u%s",
                             static_cast<unsigned long long>(command_list_handle), command_depth, num_color_targets_,
                             targets.str().c_str());
                }
            }
            break;
        case Op::ClearColor: {
            const u32 index = static_cast<u32>(c.a[0]);
            if (const auto& cc = GetDrawCensusConfig(); cc.frame && present_count_ + 1 >= cc.frame && present_count_ + 1 <= cc.frame_end) {
                LOG_INFO("census-clearc q=%llu after_seq=%llu index=%u key=%llx rgba=%.3f,%.3f,%.3f,%.3f mask=%llx sc=%u,%u,%u,%u vp=%g,%g,%g,%g guest=%llx size=%ux%u",
                         static_cast<unsigned long long>(present_count_ + 1),
                         static_cast<unsigned long long>(census_seq_frame_ == present_count_ + 1 ? census_seq_ : 0),
                         index, static_cast<unsigned long long>(index < num_color_targets_ ? ImageKey(color_targets_[index]) : 0),
                         static_cast<double>(c.f[0]), static_cast<double>(c.f[1]), static_cast<double>(c.f[2]),
                         static_cast<double>(c.f[3]), static_cast<unsigned long long>(c.a[1]), bound_.scissor[0],
                         bound_.scissor[1], bound_.scissor[2], bound_.scissor[3], static_cast<double>(bound_.viewport[0]),
                         static_cast<double>(bound_.viewport[1]), static_cast<double>(bound_.viewport[2]),
                         static_cast<double>(bound_.viewport[3]),
                         static_cast<unsigned long long>(index < num_color_targets_ ? color_targets_[index] : 0),
                         [&] { auto d = RenderModel().textures.find(index < num_color_targets_ ? color_targets_[index] : 0); return d != RenderModel().textures.end() ? d->second.desc.width : 0u; }(),
                         [&] { auto d = RenderModel().textures.find(index < num_color_targets_ ? color_targets_[index] : 0); return d != RenderModel().textures.end() ? d->second.desc.height : 0u; }());
            }
            if (render_stats_) {
                char rgba[64];
                std::snprintf(rgba, sizeof rgba, " rgba=%.3f,%.3f,%.3f,%.3f", c.f[0], c.f[1], c.f[2], c.f[3]);
                ++pass_stats_["clear:" + (index < num_color_targets_ ? TargetLabel(color_targets_[index], color_levels_[index]) : "no-target") + rgba];
            }
            if (index >= num_color_targets_ || !render_enabled_) break;
            if (!clear_gate_open_) {
                ++skip_reasons_["clearc:conditional"];
                break;
            }
            HostImage* img = ImageFor(color_targets_[index], color_levels_[index]);
            if (!img || img->aspect != VK_IMAGE_ASPECT_COLOR_BIT) break;
            Transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue color{};
            for (int i = 0; i < 4; ++i) color.float32[i] = c.f[i];
            const u32 layer = color_layers_[index] < img->layers ? color_layers_[index] : 0;
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
            vkCmdClearColorImage(Cmd(), img->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
            img->rendered = true;
            if (img->block_texel_target) ++img->block_write_generation;
            img->srgb_content.Clear();
            break;
        }
        case Op::ClearDepthStencil: {
            if (const auto& cc = GetDrawCensusConfig(); cc.frame && present_count_ + 1 >= cc.frame && present_count_ + 1 <= cc.frame_end) {
                LOG_INFO("census-cleards q=%llu after_seq=%llu dskey=%llx depth=%.3f stencil=%llu render_enabled=%d dsw=%x masks=%llu,%llu sc=%u,%u,%u,%u vp=%g,%g,%g,%g",
                         static_cast<unsigned long long>(present_count_ + 1),
                         static_cast<unsigned long long>(census_seq_frame_ == present_count_ + 1 ? census_seq_ : 0),
                         static_cast<unsigned long long>(depth_target_ ? ImageKey(depth_target_) : 0),
                         static_cast<double>(c.f[0]), static_cast<unsigned long long>(c.a[1]), render_enabled_,
                         bound_.depth_stencil[0],
                         static_cast<unsigned long long>(c.a[0]), static_cast<unsigned long long>(c.a[2]),
                         bound_.scissor[0], bound_.scissor[1], bound_.scissor[2], bound_.scissor[3],
                         static_cast<double>(bound_.viewport[0]), static_cast<double>(bound_.viewport[1]),
                         static_cast<double>(bound_.viewport[2]), static_cast<double>(bound_.viewport[3]));
            }
            HostImage* img = depth_target_ && render_enabled_ && clear_gate_open_ ? ImageFor(depth_target_, depth_level_) : nullptr;
            if (!img || img->aspect == VK_IMAGE_ASPECT_COLOR_BIT) break;
            // The Mii icon passes clear depth at the start of their second (EQUAL-tested, no depth write) pass right after
            // a depth pre-pass of the same frame wrote it, and expect that depth to survive (with the clear honored the
            // EQUAL pass draws nothing and the icon stays empty). The mechanism on the hardware is unexplained, so the
            // clear is held back until the first use of the image and dropped only if that use is an EQUAL-tested draw
            // without depth writes into a depth-only image that a draw wrote earlier in this same frame.
            // NXRT_LEGACY_DEPTH_CLEAR_ALWAYS=1 clears immediately like before.
            static const bool legacy_depth_clear_always = std::getenv("NXRT_LEGACY_DEPTH_CLEAR_ALWAYS") != nullptr;
            if (!legacy_depth_clear_always && img->aspect == VK_IMAGE_ASPECT_DEPTH_BIT) {
                ApplyPendingDepthClear();
                pending_depth_clear_ = {img, c.f[0], static_cast<u32>(c.a[1]), depth_layer_};
                break;
            }
            Transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            const VkClearDepthStencilValue value{c.f[0], static_cast<u32>(c.a[1])};
            const VkImageSubresourceRange range{img->aspect, 0, 1, depth_layer_ < img->layers ? depth_layer_ : 0, 1};
            vkCmdClearDepthStencilImage(Cmd(), img->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &value, 1,
                                        &range);
            img->rendered = true;
            break;
        }
        case Op::DrawArrays:
        case Op::DrawElements: {
            // Diagnostic (opt-in): NXRT_TRACE_FACE_COLORS=1 logs each NEW set of fragment cbuf3 words (colors) used by
            // six-index quads into 256/384-wide targets (the Mii face compositor), tagged with the present frame.
            static const bool trace_face_colors = std::getenv("NXRT_TRACE_FACE_COLORS") != nullptr;
            if (trace_face_colors && c.a[2] == 6 && num_color_targets_) {
                const auto desc = RenderModel().textures.find(color_targets_[0]);
                if (desc != RenderModel().textures.end() && desc->second.desc.width >= 48 && desc->second.desc.width <= 512 &&
                    desc->second.desc.width != 256) {  // 256 wide = shadow/depth passes, not the compositor
                    std::array<u32, 16> w{};
                    for (u32 i = 0; i < w.size(); ++i) w[i] = ReadCbuf(1, 3, i * 4);
                    static std::set<u64> seen_face_colors;
                    // mode is a small integer and color A has alpha 1.0 (matrices and other passes are excluded)
                    if (w[0] < 64 && w[7] == 0x3f800000u && seen_face_colors.insert(Hash(w.data(), sizeof w)).second) {
                        auto f = [&](u32 i) { float v; std::memcpy(&v, &w[i], 4); return static_cast<double>(v); };
                        LOG_INFO("face-colors q=%llu program=%llx target=%ux%u nvn=%u mode=%08x A=%.4f,%.4f,%.4f,%.4f B=%.4f,%.4f,%.4f,%.4f "
                                 "C=%.4f,%.4f,%.4f,%.4f",
                                 static_cast<unsigned long long>(present_count_ + 1), (unsigned long long)bound_.program,
                                 desc->second.desc.width, desc->second.desc.height, desc->second.desc.format,
                                 w[0], f(4), f(5), f(6), f(7), f(8), f(9), f(10), f(11), f(12),
                                 f(13), f(14), f(15));
                    }
                }
            }
            if (frame_profile_) {
                const auto draw_start = std::chrono::steady_clock::now();
                Draw(c, command_list_handle, command_depth);
                frame_draw_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - draw_start)
                                      .count();
                ++frame_draw_count_;
            } else {
                Draw(c, command_list_handle, command_depth);
            }
            break;
        }
        case Op::DispatchCompute: Dispatch(c); break;
        case Op::ReportCounter: ReportCounter(c); break;
        case Op::CopyTexture: CopyTexture(c); break;
        case Op::CopyTextureToBuffer: CopyTextureToBuffer(c); break;
        case Op::ClearTexture: {
            if (c.words.size() < 8 || !render_enabled_ || !clear_gate_open_) break;
            const u32 level = c.words[6], layer_in = c.words[7];
            HostImage* img = ImageFor(c.a[0], level);
            if (!img || img->aspect != VK_IMAGE_ASPECT_COLOR_BIT) {
                ++skip_reasons_["cleartex:image"];
                break;
            }
            const u32* r = c.words.data();
            const bool full = !c.a[2] || (r[0] == 0 && r[1] == 0 && r[3] >= img->width && r[4] >= img->height);
            if (!full) {
                ++skip_reasons_["cleartex:partial-region"];
                break;
            }
            if (c.a[1] != 0xf) {
                ++skip_reasons_[c.a[1] ? "cleartex:partial-mask" : "cleartex:empty-mask"];
                break;
            }
            Transition(*img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkClearColorValue color{};
            for (int i = 0; i < 4; ++i) color.float32[i] = c.f[i];
            const u32 layer = layer_in < img->layers ? layer_in : 0;
            const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
            vkCmdClearColorImage(Cmd(), img->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
            img->rendered = true;
            if (img->block_texel_target) ++img->block_write_generation;
            img->srgb_content.Clear();
            ++skip_reasons_["cleartex:done"];
            break;
        }
        case Op::SetRenderEnable:
            clear_gate_open_ = true;
            if (const auto& cc = GetDrawCensusConfig(); cc.frame && present_count_ + 1 >= cc.frame && present_count_ + 1 <= cc.frame_end) {
                LOG_INFO("census-renderenable q=%llu after_seq=%llu set=%llu applied=%d",
                         static_cast<unsigned long long>(present_count_ + 1),
                         static_cast<unsigned long long>(census_seq_frame_ == present_count_ + 1 ? census_seq_ : 0),
                         static_cast<unsigned long long>(c.a[0]), conditional_render_ ? 1 : 0);
            }
            if (conditional_render_) render_enabled_ = c.a[0] != 0;
            break;
        case Op::RenderEnableConditional: {
            // The GPU compares the counter values of the two reports at a[1] and a[1] + 16.
            const auto cpu = GpuToCpu(c.a[1]);
            if (!cpu || !g_memory.IsMapped(*cpu, 32)) break;
            const bool equal = g_memory.Read<u64>(*cpu) == g_memory.Read<u64>(*cpu + 16);
            const bool enable = c.a[0] == 0 ? equal : !equal;
            {
                // Reports still holding what ReportCounter synthesized count as 0 (see clear_gate_open_).
                const auto effective = [this](GuestAddr gpu, GuestAddr host) -> u64 {
                    const u64 value = g_memory.Read<u64>(host);
                    const auto synthetic = synthetic_reports_.find(gpu);
                    return synthetic != synthetic_reports_.end() && synthetic->second == value ? 0 : value;
                };
                const bool effective_equal = effective(c.a[1], *cpu) == effective(c.a[1] + 16, *cpu + 16);
                clear_gate_open_ = legacy_conditional_clear_ || (c.a[0] == 0 ? effective_equal : !effective_equal);
            }
            if (const auto& cc = GetDrawCensusConfig(); cc.frame && present_count_ + 1 >= cc.frame && present_count_ + 1 <= cc.frame_end) {
                LOG_INFO("census-renderenable q=%llu after_seq=%llu mode=%llu gpu=%llx v0=%llx v1=%llx enable=%d applied=%d",
                         static_cast<unsigned long long>(present_count_ + 1),
                         static_cast<unsigned long long>(census_seq_frame_ == present_count_ + 1 ? census_seq_ : 0),
                         static_cast<unsigned long long>(c.a[0]), static_cast<unsigned long long>(c.a[1]),
                         static_cast<unsigned long long>(g_memory.Read<u64>(*cpu)),
                         static_cast<unsigned long long>(g_memory.Read<u64>(*cpu + 16)), enable ? 1 : 0,
                         conditional_render_ ? 1 : 0);
            }
            ++conditional_stats_[enable];
            if (conditional_render_) render_enabled_ = enable;
            break;
        }
        case Op::CallCommands:
            if (depth < 4) {
                for (size_t i = 0; i + 1 < c.words.size(); i += 2) {
                    if (auto it = RenderModel().finished.find(Pack(c.words[i], c.words[i + 1])); it != RenderModel().finished.end()) {
                        ExecuteList(*it->second, depth + 1, Pack(c.words[i], c.words[i + 1]));
                    }
                }
            }
            break;
        default:
            break;
        }
    }
}

// Writes the presented frame as a PPM (diagnostics; NXRT_DUMP_EVERY=N, NXRT_DUMP_DIR).
void NvnRenderer::DumpImage(HostImage& img, u64 frame, const char* suffix) {
    const bool packed_hdr = img.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    const bool half_float = img.format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool depth_image = (img.aspect & VK_IMAGE_ASPECT_DEPTH_BIT) != 0 &&
                             (img.format == VK_FORMAT_D16_UNORM || img.format == VK_FORMAT_D32_SFLOAT);
    if (!depth_image && img.format != VK_FORMAT_R8G8B8A8_UNORM && img.format != VK_FORMAT_R8G8B8A8_SRGB &&
        img.format != VK_FORMAT_B8G8R8A8_UNORM && !packed_hdr && !half_float) {
        return;
    }
    const VkDeviceSize bytes_per_pixel = depth_image ? (img.format == VK_FORMAT_D16_UNORM ? 2 : 4)
                                                      : half_float ? 8 : 4;
    const VkDeviceSize size = VkDeviceSize{img.width} * img.height * bytes_per_pixel;
    Buffer buffer = vk_.CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    // NXRT_DUMP_LAYER=<n> reads that array layer of a layered image (default 0).
    static const u32 dump_layer = std::getenv("NXRT_DUMP_LAYER") ? std::atoi(std::getenv("NXRT_DUMP_LAYER")) : 0;
    const u32 layer = dump_layer < img.layers ? dump_layer : 0;
    const VkBufferImageCopy copy{0, 0, 0,
                                 {static_cast<VkImageAspectFlags>(depth_image ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, layer, 1},
                                 {0, 0, 0}, {img.width, img.height, 1}};
    vkCmdCopyImageToBuffer(Cmd(), img.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &copy);
    if (!Flush(FlushReason::DumpImage)) {
        retained_readbacks_.push_back(buffer);
        return;
    }
    if (const char* trace_raw_pixel = std::getenv("NXRT_TRACE_RAW_PIXEL"); trace_raw_pixel && *trace_raw_pixel) {
        const char* raw_x_env = std::getenv("NXRT_TRACE_RAW_PIXEL_X");
        const char* raw_y_env = std::getenv("NXRT_TRACE_RAW_PIXEL_Y");
        const u32 x = raw_x_env ? std::min(static_cast<u32>(std::strtoul(raw_x_env, nullptr, 0)), img.width - 1)
                                : img.width / 2;
        const u32 y = raw_y_env ? std::min(static_cast<u32>(std::strtoul(raw_y_env, nullptr, 0)), img.height - 1)
                                : img.height / 2;
        const size_t sample_size = static_cast<size_t>(bytes_per_pixel);
        const auto log_raw = [&](u32 sample_x, u32 sample_y, const char* label) {
            const u8* sample = buffer.mapped + (static_cast<size_t>(sample_y) * img.width + sample_x) * sample_size;
            u8 raw[8]{};
            std::memcpy(raw, sample, std::min(sample_size, sizeof(raw)));
            LOG_INFO("renderer raw pixel frame=%llu image=%s format=%d size=%ux%u sample=%s xy=%u,%u bytes=%zu "
                     "raw=%02x%02x%02x%02x%02x%02x%02x%02x",
                     static_cast<unsigned long long>(frame), suffix && *suffix ? suffix : "present",
                     static_cast<int>(img.format), img.width, img.height, label, sample_x, sample_y, sample_size,
                     static_cast<unsigned>(raw[0]), static_cast<unsigned>(raw[1]),
                     static_cast<unsigned>(raw[2]), static_cast<unsigned>(raw[3]),
                     static_cast<unsigned>(raw[4]), static_cast<unsigned>(raw[5]),
                     static_cast<unsigned>(raw[6]), static_cast<unsigned>(raw[7]));
        };
        log_raw(x, y, "requested");
        if (depth_image) {
            const u32 mirrored_y = img.height - 1 - y;
            if (mirrored_y != y) log_raw(x, mirrored_y, "vertical-mirror");
        }
    }
    const char* dir = std::getenv("NXRT_DUMP_DIR");
    const std::string path = !dump_path_override_.empty()
                                 ? dump_path_override_
                                 : std::string(dir && *dir ? dir : ".") + "/frame-" + std::to_string(frame) +
                                       (suffix && *suffix ? "-" + std::string(suffix) : "") + ".ppm";
    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P6\n%u %u\n255\n", img.width, img.height);
        const bool bgr = img.format == VK_FORMAT_B8G8R8A8_UNORM;
        std::vector<u8> row(static_cast<size_t>(img.width) * 3);
        u64 nonzero = 0, differs = 0;
        u8 first[3] = {0, 0, 0};
        const auto tone_map = [](float value) {
            const float mapped = value / (1.0f + value);
            return static_cast<u8>(std::lround(std::pow(std::clamp(mapped, 0.0f, 1.0f), 1.0f / 2.2f) * 255.0f));
        };
        const auto unpack_unsigned_float = [](u32 bits, u32 mantissa_bits) {
            const u32 mantissa_mask = (1u << mantissa_bits) - 1;
            const u32 mantissa = bits & mantissa_mask;
            const u32 exponent = bits >> mantissa_bits;
            if (exponent == 0) return std::ldexp(static_cast<float>(mantissa), 1 - 15 - static_cast<int>(mantissa_bits));
            if (exponent == 31) return mantissa ? 0.0f : 65504.0f;
            return std::ldexp(1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mantissa_bits),
                              static_cast<int>(exponent) - 15);
        };
        const auto half_to_float = [](u16 bits) {
            const bool negative = (bits & 0x8000) != 0;
            const u16 exponent = (bits >> 10) & 0x1f;
            const u16 mantissa = bits & 0x03ff;
            float value;
            if (exponent == 0) {
                value = std::ldexp(static_cast<float>(mantissa), -24);
            } else if (exponent == 0x1f) {
                value = mantissa == 0 ? std::numeric_limits<float>::infinity()
                                      : std::numeric_limits<float>::quiet_NaN();
            } else {
                value = std::ldexp(1.0f + static_cast<float>(mantissa) / 1024.0f,
                                   static_cast<int>(exponent) - 15);
            }
            return negative ? -value : value;
        };
        const auto tone_map_half = [&tone_map](float value) {
            if (std::isnan(value) || value <= 0.0f) return u8{0};
            if (!std::isfinite(value)) return u8{255};
            return tone_map(value);
        };
        for (u32 y = 0; y < img.height; ++y) {
            const u8* src = buffer.mapped + static_cast<size_t>(y) * img.width * bytes_per_pixel;
            for (u32 x = 0; x < img.width; ++x) {
                if (depth_image) {
                    float depth = 0.0f;
                    if (img.format == VK_FORMAT_D16_UNORM) {
                        u16 value;
                        std::memcpy(&value, src + static_cast<size_t>(x) * 2, sizeof(value));
                        depth = static_cast<float>(value) / 65535.0f;
                    } else {
                        std::memcpy(&depth, src + static_cast<size_t>(x) * 4, sizeof(depth));
                    }
                    const u8 gray = std::isfinite(depth)
                                        ? static_cast<u8>(std::lround(std::clamp(depth, 0.0f, 1.0f) * 255.0f))
                                        : u8{0};
                    row[x * 3 + 0] = gray;
                    row[x * 3 + 1] = gray;
                    row[x * 3 + 2] = gray;
                } else if (half_float) {
                    u16 rgba[4];
                    std::memcpy(rgba, src + static_cast<size_t>(x) * 8, sizeof(rgba));
                    row[x * 3 + 0] = tone_map_half(half_to_float(rgba[0]));
                    row[x * 3 + 1] = tone_map_half(half_to_float(rgba[1]));
                    row[x * 3 + 2] = tone_map_half(half_to_float(rgba[2]));
                } else if (packed_hdr) {
                    u32 packed;
                    std::memcpy(&packed, src + x * 4, sizeof(packed));
                    row[x * 3 + 0] = tone_map(unpack_unsigned_float(packed & 0x7ff, 6));
                    row[x * 3 + 1] = tone_map(unpack_unsigned_float((packed >> 11) & 0x7ff, 6));
                    row[x * 3 + 2] = tone_map(unpack_unsigned_float((packed >> 22) & 0x3ff, 5));
                } else {
                    row[x * 3 + 0] = src[x * 4 + (bgr ? 2 : 0)];
                    row[x * 3 + 1] = src[x * 4 + 1];
                    row[x * 3 + 2] = src[x * 4 + (bgr ? 0 : 2)];
                }
            }
            if (y == 0) std::memcpy(first, row.data(), 3);
            for (u32 x = 0; x < img.width; ++x) {
                const u8* px = row.data() + x * 3;
                nonzero += (px[0] | px[1] | px[2]) != 0;
                differs += std::memcmp(px, first, 3) != 0;
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        const u64 total = u64{img.width} * img.height;
        LOG_INFO("renderer: frame %llu written to %s", static_cast<unsigned long long>(frame), path.c_str());
        LOG_INFO("renderer: coverage %s: %ux%u, %llu/%llu pixels non-black (%.4f%%), %llu differ from pixel(0,0)=(%u,%u,%u) (%.4f%%)",
                 suffix && *suffix ? suffix : "present", img.width, img.height, static_cast<unsigned long long>(nonzero),
                 static_cast<unsigned long long>(total), 100.0 * nonzero / total, static_cast<unsigned long long>(differs),
                 first[0], first[1], first[2], 100.0 * differs / total);
    }
    vk_.Destroy(buffer);
}

// Diagnostic-only full-target snapshot immediately after a selected draw. The next draw
// transitions the same image back to COLOR_ATTACHMENT_OPTIMAL through the normal path.
void NvnRenderer::DumpWinSampleCheckpoint(HostImage& img, u64 frame, u64 draw_id, const char* tag) {
    const bool half_float = img.format == VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool depth16 = img.format == VK_FORMAT_D16_UNORM;
    const bool depth32 = img.format == VK_FORMAT_D32_SFLOAT || img.format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                         img.format == VK_FORMAT_D24_UNORM_S8_UINT || img.format == VK_FORMAT_X8_D24_UNORM_PACK32;
    const bool four_byte = depth32 || img.format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 ||
                           img.format == VK_FORMAT_R8G8B8A8_UNORM || img.format == VK_FORMAT_R8G8B8A8_SRGB ||
                           img.format == VK_FORMAT_B8G8R8A8_UNORM || img.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
                           img.format == VK_FORMAT_R16G16_SFLOAT || img.format == VK_FORMAT_R32_SFLOAT;
    if (!four_byte && !half_float && !depth16) {
        LOG_ERROR("trace-win checkpoint draw=%llu unsupported format=%d",
                  static_cast<unsigned long long>(draw_id), static_cast<int>(img.format));
        return;
    }
    const VkDeviceSize bytes_per_pixel = half_float ? 8 : depth16 ? 2 : 4;
    const VkDeviceSize size = VkDeviceSize{img.width} * img.height * bytes_per_pixel;
    Buffer buffer = vk_.CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    if (!buffer.buffer || !buffer.mapped) {
        vk_.Destroy(buffer);
        LOG_ERROR("trace-win checkpoint draw=%llu failed to allocate readback (%llu bytes)",
                  static_cast<unsigned long long>(draw_id), static_cast<unsigned long long>(size));
        return;
    }

    Transition(img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    const VkImageAspectFlags copy_aspect = (depth16 || depth32) ? VkImageAspectFlags{VK_IMAGE_ASPECT_DEPTH_BIT}
                                                                 : img.aspect;
    const VkBufferImageCopy copy{0, 0, 0, {copy_aspect, 0, 0, 1}, {0, 0, 0}, {img.width, img.height, 1}};
    vkCmdCopyImageToBuffer(Cmd(), img.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer.buffer, 1, &copy);
    const VkBufferMemoryBarrier host_read{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                          nullptr,
                                          VK_ACCESS_TRANSFER_WRITE_BIT,
                                          VK_ACCESS_HOST_READ_BIT,
                                          VK_QUEUE_FAMILY_IGNORED,
                                          VK_QUEUE_FAMILY_IGNORED,
                                          buffer.buffer,
                                          0,
                                          size};
    vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
                         &host_read, 0, nullptr);
    if (!Flush(FlushReason::DumpTarget)) {
        retained_readbacks_.push_back(buffer);
        return;
    }

    const char* dir = std::getenv("NXRT_DUMP_DIR");
    const std::string path = std::string(dir && *dir ? dir : ".") + "/frame-" + std::to_string(frame) + "-" + tag + "-" +
                             std::to_string(draw_id) + ".raw";
    std::FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) {
        LOG_ERROR("trace-win checkpoint draw=%llu cannot open %s", static_cast<unsigned long long>(draw_id),
                  path.c_str());
        vk_.Destroy(buffer);
        return;
    }
    const size_t written = std::fwrite(buffer.mapped, 1, static_cast<size_t>(size), file);
    std::fclose(file);
    if (written == size) {
        LOG_INFO("trace-win checkpoint q=%llu %s=%llu format=%d size=%ux%u bytes=%llu file=%s",
                 static_cast<unsigned long long>(frame), tag, static_cast<unsigned long long>(draw_id),
                 static_cast<int>(img.format), img.width, img.height, static_cast<unsigned long long>(size),
                 path.c_str());
    } else {
        LOG_ERROR("trace-win checkpoint draw=%llu short write %zu/%llu to %s",
                  static_cast<unsigned long long>(draw_id), written, static_cast<unsigned long long>(size),
                  path.c_str());
    }
    vk_.Destroy(buffer);
}

bool NvnRenderer::InvalidateMappedRange(GuestAddr pool_address, u64 offset, u64 size) {
    // The title renders its picking IDs into a CPU-mapped, pitched texture,
    // then invalidates that pool range before inspecting the pixels. Vulkan
    // images have separate storage: invalidating the guest cache alone cannot
    // expose their writes. The opt-out is retained for regression comparisons.
    static const bool enabled = [] {
        const char* value = std::getenv("NXRT_MAPPED_TEXTURE_READBACK");
        return !value || std::strcmp(value, "0") != 0;
    }();
    if (!enabled || !size) return false;
    RendererScope renderer_scope;
    ExecuteLockScope locks(mutex_, model_.lock);
    if (broken_) return false;
    SelectResolutionModel(RenderModel());
    const auto pool_it = RenderModel().pools.find(pool_address);
    if (pool_it == RenderModel().pools.end()) return false;
    const auto& pool = pool_it->second;
    if (!pool.storage || offset > pool.size || size > pool.size - offset) return false;
    const u64 range_end = offset + size;
    bool changed = false;
    // Texture objects by pool, rebuilt only when the model epoch changed: the game invalidates a mapped range twice
    // per frame and the full scan of every texture object cost ~5% of the render thread.
    {
        const u64 epoch = RenderModel().epoch.load(std::memory_order_acquire);
        if (textures_by_pool_.model != &RenderModel() || textures_by_pool_.epoch != epoch || !resolve_memo_) {
            textures_by_pool_.by_pool.clear();
            for (const auto& [texture, value] : RenderModel().textures) {
                textures_by_pool_.by_pool[value.desc.pool].push_back(texture);
            }
            textures_by_pool_.model = &RenderModel();
            textures_by_pool_.epoch = epoch;
        }
    }
    const auto pool_textures = textures_by_pool_.by_pool.find(pool_address);
    if (pool_textures == textures_by_pool_.by_pool.end()) return false;
    for (const GuestAddr texture : pool_textures->second) {
        const auto texture_it = RenderModel().textures.find(texture);
        if (texture_it == RenderModel().textures.end()) continue;
        const auto& value = texture_it->second;
        const auto& d = value.desc;
        if (d.pool != pool_address || !d.stride || d.levels > 1 || d.depth > 1 || d.samples > 1 ||
            d.offset >= range_end || d.offset >= pool.size) continue;
        const auto image_it = images_.find(ImageKey(texture));
        if (image_it == images_.end()) continue;
        auto& image = image_it->second;
        if (!image.rendered || !image.image.image || image.layers != 1 ||
            image.width != d.width || image.height != d.height || image.nvn_format != d.format ||
            image.aspect != VK_IMAGE_ASPECT_COLOR_BIT || !image.width || !image.height) continue;
        const auto format = Format(d.format);
        Tic tic{};
        tic.w[0] = format.tic_word0;
        const auto decoded = TicFormat(tic);
        if (!format.valid || !format.bytes_per_block || decoded.block_width != 1 || decoded.block_height != 1 ||
            decoded.format != image.format) continue;
        const u64 row_bytes = u64{image.width} * format.bytes_per_block;
        if (row_bytes > d.stride) continue;
        const u64 span = u64{image.height - 1} * d.stride + row_bytes;
        if (span > pool.size - d.offset || offset >= d.offset + span ||
            !g_memory.IsMapped(pool.storage + d.offset, span)) continue;

        const VkDeviceSize packed_size = row_bytes * image.height;
        Buffer readback = AcquireReadback(packed_size);
        if (!readback.buffer || !readback.mapped) {
            ReleaseReadback(readback);
            return changed;
        }
        EndRenderPass();
        const auto restore_layout = image.layout;
        Transition(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        const VkBufferImageCopy copy{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                     {0, 0, 0}, {image.width, image.height, 1}};
        vkCmdCopyImageToBuffer(Cmd(), image.image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               readback.buffer, 1, &copy);
        const VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, nullptr,
                                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
                                             VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
                                             readback.buffer, 0, packed_size};
        vkCmdPipelineBarrier(Cmd(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, nullptr, 1, &barrier, 0, nullptr);
        Transition(image, restore_layout);
        if (!Flush(FlushReason::MappedReadback)) {
            retained_readbacks_.push_back(readback);
            return changed;
        }
        // Copy only invalidated pixel bytes. Preserve row padding and any
        // bytes outside a partial invalidation, even when it starts mid-row.
        for (u32 row = 0; row < image.height; ++row) {
            const u64 begin = d.offset + u64{row} * d.stride;
            const u64 lo = std::max(begin, offset), hi = std::min(begin + row_bytes, range_end);
            if (lo < hi) {
                g_memory.WriteBlock(pool.storage + lo,
                    readback.mapped + u64{row} * row_bytes + lo - begin, hi - lo);
                changed = true;
            }
        }
        static u32 logged = 0;
        const bool trace = std::getenv("NXRT_TRACE_MAPPED_READBACK") && present_count_ >= 5700 && present_count_ < 5730;
        if (logged++ < 8 || trace) {
            const u64 center = (u64{image.height / 2} * image.width + image.width / 2) * format.bytes_per_block;
            u32 pixel = 0;
            std::memcpy(&pixel, readback.mapped + center, std::min<u32>(format.bytes_per_block, sizeof(pixel)));
            LOG_INFO("mapped-texture-readback: q=%llu pool=%llx cpu=%llx size=%ux%u pitch=%u bpp=%u center=%x range=%llx/%llu",
                     static_cast<unsigned long long>(present_count_ + 1), static_cast<unsigned long long>(pool_address),
                     static_cast<unsigned long long>(pool.storage + d.offset), image.width, image.height, d.stride,
                     format.bytes_per_block, pixel, static_cast<unsigned long long>(offset),
                     static_cast<unsigned long long>(size));
        }
        ReleaseReadback(readback);
    }
    return changed;
}

void NvnRenderer::Execute(const CommandList& list) {
    ExecuteImpl(list, nullptr);
}

void NvnRenderer::ExecuteSubmission(const nvn::SubmissionSnapshot& submission) {
    ExecuteImpl(submission.list, submission.execution_model.get());
}

// Textures decoded from guest memory (sampled_) used to stay for the whole run, so a 6 GB card ran out of device memory after
// ~20 minutes of play (every scene adds textures). Past half of the card's memory held by images, or after an allocation that
// failed, the textures not sampled for a few seconds are dropped, oldest first; the next draw that needs one rebuilds it from
// guest memory. They go through retired_images_, which is destroyed only where the GPU is known to be done with them.
void NvnRenderer::EvictSampledTextures() {
    const bool pressure = VulkanContext::TakeAllocationFailure();
    const u64 heap = vk_.DeviceLocalHeapBytes();
    if (!heap) return;
    // The walk over every texture is not free: without an allocation failure look at most once every two seconds.
    static u64 last_check = 0;
    if (!pressure && present_count_ - last_check < 60 && last_check) return;
    last_check = present_count_;
    u64 held = VulkanContext::ImageBytesHeld();
    // NXRT_IMAGE_BUDGET_MIB=<n> (test): evict as if the card's memory were 2n MiB.
    static const u64 forced = [] {
        const char* v = std::getenv("NXRT_IMAGE_BUDGET_MIB");
        return v ? std::strtoull(v, nullptr, 10) << 20 : u64{0};
    }();
    const u64 budget = forced ? forced : heap / 2;
    if (!pressure && held < budget) return;
    const u64 target = pressure ? budget * 2 / 3 : budget * 4 / 5;
    std::vector<std::pair<u64, u64>> candidates;  // (last use, key)
    for (auto& [key, image] : sampled_) {
        if (&image == glyph_image_ || !image.image.image) continue;
        if (present_count_ - image.last_use_frame < 180) continue;  // used in the last ~6 seconds: keep
        candidates.emplace_back(image.last_use_frame, key);
    }
    std::sort(candidates.begin(), candidates.end());
    u64 freed = 0, count = 0;
    for (const auto& [_, key] : candidates) {
        if (held <= target) break;
        auto it = sampled_.find(key);
        if (it == sampled_.end()) continue;
        const u64 bytes = it->second.image.allocation_size;
        retired_images_.push_back(std::move(it->second));
        sampled_.erase(it);
        pitch_sources_.erase(key);
        swizzled_sources_.erase(key);
        held -= std::min(held, bytes);
        freed += bytes;
        ++count;
    }
    if (count) {
        for (auto& memo : sampled_hit_memo_) memo = {};  // they point into the maps just erased from
        LOG_INFO("renderer: device memory %s: dropped %llu unused textures (%llu MiB); %zu remain", pressure ? "ran out" : "is filling",
                 static_cast<unsigned long long>(count), static_cast<unsigned long long>(freed >> 20), sampled_.size());
    }
}

void NvnRenderer::ExecuteImpl(const CommandList& list, nvn::Model* metadata) {
    RendererScope renderer_scope;
    ExecuteLockScope locks(mutex_, model_.lock);
    nvn::ExecutionModelScope model_scope(execution_model_, metadata);
    SelectResolutionModel(RenderModel());
    struct LocksGuard {
        NvnRenderer& r;
        ~LocksGuard() { r.execute_locks_ = nullptr; }
    } locks_guard{*this};
    execute_locks_ = &locks;
    model_lock_taken_ = std::chrono::steady_clock::now();
    if (broken_) return;
    EvictSampledTextures();
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    ComputeReadbackTrace::Instance().SetFrame(present_count_ + 1);
    ProgramTrace::Instance().SetFrame(present_count_);
#endif
    try {
        const auto execute_start = (frame_profile_ || trace_cflush_) ? std::chrono::steady_clock::now()
                                                                     : std::chrono::steady_clock::time_point{};
        trace_execute_start_ = execute_start;
        ExecuteList(list, 0, 0);
        if (frame_profile_) {
            frame_execute_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - execute_start)
                                     .count();
            ++frame_execute_count_;
        }
        const ExecuteModelRelease model_release =
            locks.BeforeFinalFlush(release_model_at_execute_end_,
                                   !pending_compute_writebacks_.empty() || (ledger_ && ledger_->Pending()),
                                   execution_model_ != nullptr);
        if (frame_profile_ && model_release == ExecuteModelRelease::Released) ++profile_model_tail_releases_;
        if (frame_profile_ && model_release == ExecuteModelRelease::PendingWritebacks) ++profile_model_tail_fallbacks_;
        FlushEndOfWork(FlushReason::Execute);
        if (ledger_) {
            // Invariant: nothing is pending when Execute returns, so the guest never outlives a stale range
            // (relaxed with NXRT_ASYNC_GRAPHICS: the ledger's page guard serves the guest's accesses).
            if (ledger_->Pending() && !AsyncGraphicsEnabled()) {
                ++async_end_violations_;
                LOG_ERROR("renderer: async-compute: %zu write-back(s) still pending at the end of Execute", ledger_->Pending());
                if (broken_) ledger_->DropAll();
                else ledger_->MaterializeAll(AsyncMaterializeCause::ExecuteEnd);
            }
            ReportAsync(false);
        }
        if (chain_cache_enabled_ && !chain_.skipping && chain_.count && chain_.count != chain_.snapshot_count) ChainSnapshot();
    } catch (const std::exception& e) {
        LOG_ERROR("renderer disabled by an exception: %s", e.what());
        broken_ = true;
        if (ledger_) ledger_->DropAll();
    }
}

void NvnRenderer::Present(GuestAddr texture, u64 frame) {
    if (chain_on_) ChainFinish();
    if (kHitchMs > 0.0) {
        static auto last_present = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double, std::milli>(now - last_present).count();
        last_present = now;
        if (dt >= kHitchMs) {
            std::string parts;
            char buf[96];
            for (u32 c = 0; c < kHitchCount; ++c) {
                if (g_hitch.n[c] == 0) continue;
                std::snprintf(buf, sizeof buf, " %s=%.1fms/%u", kHitchNames[c], g_hitch.ms[c], g_hitch.n[c]);
                parts += buf;
                if (g_hitch.worst_ms[c] >= 5.0) {
                    std::snprintf(buf, sizeof buf, "[worst %.1fms tag=%llx]", g_hitch.worst_ms[c],
                                  static_cast<unsigned long long>(g_hitch.worst_tag[c]));
                    parts += buf;
                }
            }
            {
                static const char* const kReasonNames[8] = {"image-recreate", "execute", "present", "dump-image", "dump-target", "compute", "stream-full", "mapped-readback"};
                for (u32 r = 0; r < 8; ++r) {
                    if (g_hitch.reason_n[r] < 50) continue;
                    std::snprintf(buf, sizeof buf, " flush[%s]=%.0fms/%u", kReasonNames[r], g_hitch.reason_ms[r], g_hitch.reason_n[r]);
                    parts += buf;
                }
            }
            if (g_hitch.dispatch_total >= 200) {
                for (const auto& [cls, v] : g_hitch.guard_class) {
                    std::snprintf(buf, sizeof buf, " guard[%s]=%.0fms/%u", cls.c_str(), v.first, v.second);
                    parts += buf;
                }
            }
            if (g_hitch.dispatch_total >= 200) {
                std::snprintf(buf, sizeof buf, " ro=%.0fMB/%llu ro-repeat=%.0fMB/%llu", g_hitch.ssbo_ro_bytes / 1048576.0,
                              static_cast<unsigned long long>(g_hitch.ssbo_ro_n), g_hitch.ssbo_ro_hit_bytes / 1048576.0,
                              static_cast<unsigned long long>(g_hitch.ssbo_ro_hit_n));
                parts += buf;
            }
            if (g_hitch.dispatch_total >= 200) {
                std::snprintf(buf, sizeof buf, " ssbo-read=%.1fMB/%llu fwd=%llu ubo-read=%.1fMB", g_hitch.ssbo_read_bytes / 1048576.0,
                              static_cast<unsigned long long>(g_hitch.ssbo_read_n), static_cast<unsigned long long>(g_hitch.ssbo_fwd_n),
                              g_hitch.ubo_read_bytes / 1048576.0);
                parts += buf;
            }
            if (g_hitch.dispatch_total >= 200) {
                std::vector<std::pair<u32, u64>> top;
                for (const auto& [prog, n] : g_hitch.dispatches) top.emplace_back(n, prog);
                std::sort(top.rbegin(), top.rend());
                std::snprintf(buf, sizeof buf, " dispatches=%u[", g_hitch.dispatch_total);
                parts += buf;
                for (size_t i = 0; i < top.size() && i < 4; ++i) {
                    std::snprintf(buf, sizeof buf, "%s%llx x%u", i ? " " : "", static_cast<unsigned long long>(top[i].second), top[i].first);
                    parts += buf;
                }
                parts += "]";
            }
            LOG_INFO("hitch q=%llu dt=%.1fms:%s", static_cast<unsigned long long>(present_count_ + 1), dt,
                     parts.empty() ? " (no tracked region)" : parts.c_str());
        }
        for (u32 c = 0; c < kHitchCount; ++c) g_hitch.ms[c] = 0, g_hitch.n[c] = 0, g_hitch.worst_ms[c] = 0, g_hitch.worst_tag[c] = 0;
        g_hitch.dispatches.clear(), g_hitch.dispatch_total = 0;
        g_hitch.ssbo_read_bytes = g_hitch.ssbo_read_n = g_hitch.ssbo_fwd_n = g_hitch.ubo_read_bytes = 0;
        g_hitch.ssbo_ro_bytes = g_hitch.ssbo_ro_n = g_hitch.ssbo_ro_hit_bytes = g_hitch.ssbo_ro_hit_n = 0;
        g_hitch.ro_seen.clear();
        g_hitch.guard_class.clear();
        for (u32 r = 0; r < 8; ++r) g_hitch.reason_ms[r] = 0, g_hitch.reason_n[r] = 0;
    }
    const auto profile_start = frame_profile_ ? std::chrono::steady_clock::now()
                                             : std::chrono::steady_clock::time_point{};
    if (frame_profile_) {
        if (last_profile_present_ != std::chrono::steady_clock::time_point{}) {
            profile_interval_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        profile_start - last_profile_present_)
                                        .count();
        }
        last_profile_present_ = profile_start;
    }
    RendererScope renderer_scope;
    std::lock_guard lock(mutex_);
    SelectResolutionModel(RenderModel());
#if defined(NXRT_ENABLE_GUEST_MEMORY_READ_OBSERVER)
    ComputeReadbackTrace::Instance().SetFrame(present_count_ + 1);
    ProgramTrace::Instance().SetFrame(present_count_);
#endif
    HostImage* img = nullptr;
    {
        std::lock_guard model_lock(model_.lock);
        if (!broken_) img = ImageFor(texture);
        if (img && img->rendered) {
            Transition(*img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        }
        if (!FlushEndOfWork(FlushReason::Present)) return;
        const WinSampleTraceConfig& win_trace = GetWinSampleTraceConfig();
        const bool trace_frame_match = win_trace.frame && frame == *win_trace.frame;
        const bool trace_signature_match = !win_trace.frame && win_trace.index_count &&
                                           !pending_win_sample_traces_.empty();
        if (trace_frame_match || trace_signature_match) {
            if (trace_frame_match && pending_win_sample_traces_.empty()) {
                    LOG_INFO("trace-win q=%llu target_key=0x%llx serial_filter=%s no matching draws",
                             static_cast<unsigned long long>(frame),
                             static_cast<unsigned long long>(win_trace.target_key),
                             win_trace.serial ? std::to_string(*win_trace.serial).c_str() : "any");
            }
            for (const std::string& event : pending_win_sample_traces_) {
                LOG_INFO("trace-win q=%llu %s", static_cast<unsigned long long>(frame), event.c_str());
            }
            if (pending_win_sample_trace_capped_) {
                LOG_INFO("trace-win q=%llu sample trace capped at %zu entries",
                         static_cast<unsigned long long>(frame), kWinTraceCap);
            }
        }
        if (win_trace.frame || win_trace.index_count) {
            pending_win_sample_traces_.clear();
            pending_win_sample_trace_capped_ = false;
        }
        if (!census_query_seqs_.empty() && GetDrawCensusConfig().frame == present_count_ + 1 &&
            Flush(FlushReason::DumpTarget)) {
            std::vector<u64> samples(census_query_seqs_.size());
            const VkResult result = vkGetQueryPoolResults(
                vk_.device, census_queries_, 0, static_cast<u32>(samples.size()), samples.size() * sizeof(u64),
                samples.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
            for (size_t i = 0; i < samples.size(); ++i) {
                LOG_INFO("census-samples q=%llu seq=%llu samples=%llu result=%d",
                         static_cast<unsigned long long>(present_count_ + 1),
                         static_cast<unsigned long long>(census_query_seqs_[i]),
                         static_cast<unsigned long long>(samples[i]), static_cast<int>(result));
            }
            census_query_seqs_.clear();
        }
        // Diagnostic-only pacing: the recorded input replays assume a slow (~10-15 fps) run; a faster
        // host reaches menus in fewer frames and desynchronizes the replay. Unset = no delay.
        static const u64 probe_frame_sleep_ms =
            std::getenv("NXRT_PROBE_FRAME_SLEEP_MS") ? std::strtoull(std::getenv("NXRT_PROBE_FRAME_SLEEP_MS"), nullptr, 0)
                                                     : 0;
        if (probe_frame_sleep_ms) std::this_thread::sleep_for(std::chrono::milliseconds(probe_frame_sleep_ms));
        ++present_count_;
        if (graphics_state_cache_enabled_ && present_count_ % 600 == 0) {
            LOG_INFO("renderer graphics-state-cache q=%llu skipped cumulative pipeline=%llu viewport=%llu scissor=%llu blend=%llu",
                     static_cast<unsigned long long>(present_count_),
                     static_cast<unsigned long long>(graphics_state_skipped_[0]),
                     static_cast<unsigned long long>(graphics_state_skipped_[1]),
                     static_cast<unsigned long long>(graphics_state_skipped_[2]),
                     static_cast<unsigned long long>(graphics_state_skipped_[3]));
        }
        SampleProfiler::Get().Tick(present_count_);
        if (trace_render_memory_ && frame % 60 == 0) {
            u64 target_image_bytes = 0;
            u64 sampled_image_bytes = 0;
            u64 pitch_shadow_bytes = 0;
            u64 pipeline_host_lower_bound = 0;
            u64 texture_pool_host_lower_bound = 0;
            const auto add_saturated = [](u64& total, u64 amount) {
                total = amount > std::numeric_limits<u64>::max() - total ? std::numeric_limits<u64>::max()
                                                                         : total + amount;
            };
            const auto add_product_saturated = [&](u64& total, size_t count, size_t width) {
                if (width && count > std::numeric_limits<u64>::max() / width) {
                    total = std::numeric_limits<u64>::max();
                    return;
                }
                add_saturated(total, static_cast<u64>(count * width));
            };
            for (const auto& [_, image] : images_) add_saturated(target_image_bytes, image.image.allocation_size);
            for (const auto& [_, image] : sampled_) add_saturated(sampled_image_bytes, image.image.allocation_size);
            for (const auto& [_, source] : pitch_sources_) {
                add_saturated(pitch_shadow_bytes, static_cast<u64>(source.shadow.capacity()));
            }

            size_t texture_pool_entries = 0;
            using TexturePoolMap = decltype(RenderModel().texture_pools);
            using TexturePoolEntries = TexturePoolMap::mapped_type;
            add_product_saturated(texture_pool_host_lower_bound, RenderModel().texture_pools.bucket_count(), sizeof(void*));
            add_product_saturated(texture_pool_host_lower_bound, RenderModel().texture_pools.size(), sizeof(TexturePoolMap::value_type));
            for (const auto& [_, entries] : RenderModel().texture_pools) {
                texture_pool_entries += entries.size();
                add_product_saturated(texture_pool_host_lower_bound, entries.bucket_count(), sizeof(void*));
                add_product_saturated(texture_pool_host_lower_bound, entries.size(), sizeof(TexturePoolEntries::value_type));
            }
            using PipelineMap = decltype(pipelines_);
            add_product_saturated(pipeline_host_lower_bound, pipelines_.size(), sizeof(Pipeline));
            add_product_saturated(pipeline_host_lower_bound, pipelines_.size(), sizeof(PipelineMap::value_type));
            add_product_saturated(pipeline_host_lower_bound, pipelines_.bucket_count(), sizeof(void*));
            const auto add_dynamic_small_vector = [&](u64& total, size_t capacity, size_t inline_capacity,
                                                       size_t element_size) {
                if (capacity > inline_capacity) {
                    add_product_saturated(total, capacity - inline_capacity, element_size);
                }
            };
            for (const auto& [_, pipeline] : pipelines_) {
                if (!pipeline) continue;
                for (const Shader::Info& info : pipeline->shader_infos) {
                    using LegacyStoreMap = decltype(info.legacy_stores_mapping);
                    add_product_saturated(pipeline_host_lower_bound, info.legacy_stores_mapping.size(),
                                          sizeof(LegacyStoreMap::value_type));
                    add_dynamic_small_vector(pipeline_host_lower_bound, info.texture_buffer_descriptors.capacity(), 6,
                                             sizeof(Shader::TextureBufferDescriptor));
                    add_dynamic_small_vector(pipeline_host_lower_bound, info.image_buffer_descriptors.capacity(), 2,
                                             sizeof(Shader::ImageBufferDescriptor));
                    add_dynamic_small_vector(pipeline_host_lower_bound, info.texture_descriptors.capacity(), 12,
                                             sizeof(Shader::TextureDescriptor));
                    add_dynamic_small_vector(pipeline_host_lower_bound, info.image_descriptors.capacity(), 4,
                                             sizeof(Shader::ImageDescriptor));
                }
            }
            const size_t shader_pool_reserved_bytes = shader_pools_ ? shader_pools_->ReservedStorageBytes() : 0;
            ProcessMemoryStatus process_memory{};
            std::ifstream status_file("/proc/self/status");
            std::string status_line;
            while (std::getline(status_file, status_line)) ParseProcessMemoryStatusLine(status_line, process_memory);
            LOG_INFO("renderer memory frame %llu: VmRSS=%lluKiB VmSwap=%lluKiB status=%d/%d target_images=%zu/%lluKiB sampled_images=%zu/%lluKiB pitch_shadows=%zu/%lluKiB guest_textures=%zu texture_pool_entries=%zu texture_pool_host_lb=%lluKiB pool_reg_new=%llu pool_reg_changed=%llu pool_reg_same=%llu framebuffers=%zu render_passes=%zu pipelines=%zu pipeline_host_lb=%lluKiB pipeline_hits=%llu pipeline_misses=%llu shader_pool_reserved=%zuKiB samplers=%zu",
                     static_cast<unsigned long long>(frame), static_cast<unsigned long long>(process_memory.rss_kib),
                     static_cast<unsigned long long>(process_memory.swap_kib), process_memory.has_rss,
                     process_memory.has_swap, images_.size(), static_cast<unsigned long long>(target_image_bytes / 1024),
                     sampled_.size(), static_cast<unsigned long long>(sampled_image_bytes / 1024),
                     pitch_sources_.size(), static_cast<unsigned long long>(pitch_shadow_bytes / 1024),
                     RenderModel().textures.size(), texture_pool_entries,
                     static_cast<unsigned long long>(texture_pool_host_lower_bound / 1024),
                     static_cast<unsigned long long>(RenderModel().texture_pool_registration.new_entries),
                     static_cast<unsigned long long>(RenderModel().texture_pool_registration.changed_entries),
                     static_cast<unsigned long long>(RenderModel().texture_pool_registration.unchanged_entries),
                     framebuffers_.size(), render_passes_.size(), pipelines_.size(),
                     static_cast<unsigned long long>(pipeline_host_lower_bound / 1024),
                     static_cast<unsigned long long>(pipeline_cache_hits_),
                     static_cast<unsigned long long>(pipeline_cache_misses_), shader_pool_reserved_bytes / 1024,
                     samplers_[0].size() + samplers_[1].size());
        }
        if (std::getenv("NXRT_TRACE_GLYPH") && frame % 60 == 0) {
            static const u64 dump_frame =
                std::getenv("NXRT_TRACE_GLYPH_DUMP") ? std::strtoull(std::getenv("NXRT_TRACE_GLYPH_DUMP"), nullptr, 0) : 0;
            for (auto& [key, src] : pitch_sources_) {
                if (src.format != 0x1d) continue;
                std::vector<u8> guest(static_cast<size_t>(src.row_bytes) * src.rows * src.layers);
                ReadPitch(src, guest.data());
                const size_t nonzero =
                    guest.size() - static_cast<size_t>(std::count(guest.begin(), guest.end(), u8{0}));
                LOG_INFO("glyph-tex: frame %llu key=%llx uses=%llu uploads=%llu guest_nonzero=%zu same_as_uploaded=%d",
                         static_cast<unsigned long long>(frame), static_cast<unsigned long long>(key),
                         static_cast<unsigned long long>(src.uses), static_cast<unsigned long long>(src.uploads),
                         nonzero, guest == src.shadow);
                src.uses = 0;
                if (dump_frame && frame >= dump_frame && dump_frame + 60 > frame) {
                    const char* dir = std::getenv("NXRT_DUMP_DIR");
                    const std::string path = std::string(dir && *dir ? dir : ".") + "/glyph-" +
                                             std::to_string(frame) + ".pgm";
                    if (std::FILE* f = std::fopen(path.c_str(), "wb")) {
                        std::fprintf(f, "P5\n%u %u\n255\n", src.row_bytes, src.rows * src.layers);
                        std::fwrite(guest.data(), 1, guest.size(), f);
                        std::fclose(f);
                    }
                }
            }
        }
    }
    if (std::getenv("NXRT_RENDER_STATS") && frame % 120 == 0) {
        std::string reasons;
        for (const auto& [why, n] : skip_reasons_) reasons += " " + why + "=" + std::to_string(n);
        reasons += " stream-flushes=" + std::to_string(stream_flushes_);
        LOG_INFO("renderer: frame %llu, %llu draws, %llu skipped:%s; dispatches %llu (%llu skipped), copies %llu "
                 "(%llu done), conditional render would disable %llu / enable %llu",
                 static_cast<unsigned long long>(frame), static_cast<unsigned long long>(draws_),
                 static_cast<unsigned long long>(skipped_), reasons.c_str(),
                 static_cast<unsigned long long>(compute_dispatches_), static_cast<unsigned long long>(compute_skipped_),
                 static_cast<unsigned long long>(copies_), static_cast<unsigned long long>(copies_done_),
                 static_cast<unsigned long long>(conditional_stats_[0]),
                 static_cast<unsigned long long>(conditional_stats_[1]));
        for (const auto& [what, n] : pass_stats_) {
            LOG_INFO("renderer:   %llu x %s", static_cast<unsigned long long>(n), what.c_str());
        }
    }
    // F9 marker (NXRT_INPUT_RECORD): one-shot dump of the presented frame to a requested path.
    if (hle::input_record::DumpPending() && img && img->rendered) {
        std::string marker_path;
        if (hle::input_record::TakeDump(marker_path)) {
            dump_path_override_ = marker_path;
            DumpImage(*img, frame);
            dump_path_override_.clear();
            LOG_INFO("renderer: marker frame dump written to '%s'", marker_path.c_str());
            if (broken_) return;
        }
    }
    static const u64 dump_every = std::getenv("NXRT_DUMP_EVERY") ? std::strtoull(std::getenv("NXRT_DUMP_EVERY"), nullptr, 0) : 0;
    // NXRT_DUMP_RANGE=<from>:<to> limits NXRT_DUMP_EVERY to that input-frame (VI present count) window.
    static const std::pair<u64, u64> dump_range = [] {
        const char* v = std::getenv("NXRT_DUMP_RANGE");
        if (!v || !*v) return std::pair<u64, u64>{0, ~u64{0}};
        char* end = nullptr;
        const u64 lo = std::strtoull(v, &end, 10);
        const u64 hi = end && *end == ':' ? std::strtoull(end + 1, nullptr, 10) : ~u64{0};
        return std::pair<u64, u64>{lo, hi};
    }();
    const u64 input_frame = hle::input_record::CurrentFrame();
    if (dump_every && frame % dump_every == 0 && input_frame >= dump_range.first && input_frame <= dump_range.second) {
        LOG_INFO("renderer: present frame %llu: %s (format %d, %ux%u, %llu draws into it)",
                 static_cast<unsigned long long>(frame), img ? (img->rendered ? "rendered" : "not rendered") : "unknown texture",
                 img ? static_cast<int>(img->format) : 0, img ? img->width : 0, img ? img->height : 0,
                 img ? static_cast<unsigned long long>(img->draws) : 0ULL);
        if (img && img->rendered) {
            DumpImage(*img, frame);
            if (broken_) return;
        }
        std::lock_guard model_lock(model_.lock);
        LOG_INFO("  window texture %llx key %llx", static_cast<unsigned long long>(texture),
                 static_cast<unsigned long long>(ImageKey(texture)));
        for (auto& [key, other] : images_) {
            if (other.draws) {
                LOG_INFO("  target key %llx: %ux%u format %d, %llu draws", static_cast<unsigned long long>(key),
                         other.width, other.height, static_cast<int>(other.format),
                         static_cast<unsigned long long>(other.draws));
            }
        }
        static const u64 dump_target_key = std::getenv("NXRT_DUMP_TARGET_KEY")
                                               ? std::strtoull(std::getenv("NXRT_DUMP_TARGET_KEY"), nullptr, 0)
                                               : 0;
        static const u64 dump_target_frame = std::getenv("NXRT_DUMP_TARGET_FRAME")
                                                  ? std::strtoull(std::getenv("NXRT_DUMP_TARGET_FRAME"), nullptr, 0)
                                                  : 0;
        if (dump_target_key && (!dump_target_frame || frame == dump_target_frame)) {
            if (auto target = images_.find(dump_target_key); target != images_.end() && target->second.rendered) {
                Transition(target->second, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                if (!Flush(FlushReason::DumpTarget)) return;
                DumpImage(target->second, frame, "target");
                if (broken_) return;
            }
        }
        if (std::getenv("NXRT_DUMP_TARGETS")) {
            for (auto& [key, other] : images_) {
                if (!other.draws || !other.rendered) continue;
                Transition(other, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                if (!Flush(FlushReason::DumpTarget)) return;
                char tag[40];
                std::snprintf(tag, sizeof tag, "t%llx", static_cast<unsigned long long>(key));
                DumpImage(other, frame, tag);
                if (std::getenv("NXRT_PROBE_SMALL_TARGET_RAW") && other.width <= 512 && other.height <= 512 &&
                    other.format == VK_FORMAT_R8G8B8A8_UNORM) {
                    DumpWinSampleCheckpoint(other, frame, key, "target");
                }
                if (broken_) return;
            }
        }
        if (std::getenv("NXRT_NVN_STATS") && frame == dump_every * 5) nvn::DumpStats();
        if (auto t = RenderModel().textures.find(texture); t != RenderModel().textures.end()) {
            LOG_INFO("  window texture storage: pool %llx offset %llx", static_cast<unsigned long long>(t->second.desc.pool),
                     static_cast<unsigned long long>(t->second.desc.offset));
        }
    }
    if (selected_trace_target_pending_) {
        if (std::getenv("NXRT_DUMP_SELECTED_TARGET")) {
            std::lock_guard model_lock(model_.lock);
            if (auto target = images_.find(selected_trace_target_key_);
                target != images_.end() && target->second.rendered) {
                Transition(target->second, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                if (!Flush(FlushReason::DumpTarget)) return;
                DumpImage(target->second, frame, "selected-target");
                if (broken_) return;
            }
            if (img && img->rendered) {
                DumpImage(*img, frame, "selected-present");
                if (broken_) return;
            }
        }
        selected_trace_target_pending_ = false;
    }
    const auto present_start = frame_profile_ ? std::chrono::steady_clock::now()
                                              : std::chrono::steady_clock::time_point{};
    if (img && img->rendered) vk_.PresentImage(img->image.image, img->width, img->height, frame);
    else vk_.PresentImage(VK_NULL_HANDLE, 1, 1, frame);
    if (frame_profile_) {
        const auto profile_end = std::chrono::steady_clock::now();
        profile_render_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(profile_end - profile_start).count();
        profile_present_ns_ += std::chrono::duration_cast<std::chrono::nanoseconds>(profile_end - present_start).count();
        profile_flush_wait_ns_ += frame_flush_wait_ns_;
        profile_flush_count_ += frame_flush_count_;
        if (gpu_ts_) {
            for (size_t i = 0; i < frame_gpu_sites_.count.size(); ++i) {
                profile_gpu_sites_.gpu_ns[i] += frame_gpu_sites_.gpu_ns[i];
                profile_gpu_sites_.wait_ns[i] += frame_gpu_sites_.wait_ns[i];
                profile_gpu_sites_.count[i] += frame_gpu_sites_.count[i];
            }
            frame_gpu_sites_.Reset();
        }
        for (size_t i = 0; i < frame_flush_reason_count_.size(); ++i) {
            profile_flush_reason_count_[i] += frame_flush_reason_count_[i];
            profile_flush_reason_ns_[i] += frame_flush_reason_ns_[i];
        }
        profile_execute_ns_ += frame_execute_ns_;
        profile_execute_count_ += frame_execute_count_;
        profile_draw_ns_ += frame_draw_ns_;
        profile_draw_count_ += frame_draw_count_;
        profile_draw_pipeline_ns_ += frame_draw_pipeline_ns_;
        profile_draw_descriptors_ns_ += frame_draw_descriptors_ns_;
        profile_draw_buffers_ns_ += frame_draw_buffers_ns_;
        profile_draw_commands_ns_ += frame_draw_commands_ns_;
        profile_draw_texture_resolve_ns_ += frame_draw_texture_resolve_ns_;
        profile_sampled_swizzled_hash_ns_ += frame_sampled_swizzled_hash_ns_;
        profile_sampled_swizzled_hash_bytes_ += frame_sampled_swizzled_hash_bytes_;
        profile_sampled_swizzled_hash_calls_ += frame_sampled_swizzled_hash_calls_;
        profile_sampled_pitch_refresh_ns_ += frame_sampled_pitch_refresh_ns_;
        profile_sampled_pitch_refresh_calls_ += frame_sampled_pitch_refresh_calls_;
        profile_vertex_read_ns_ += frame_vertex_read_ns_;
        profile_index_read_ns_ += frame_index_read_ns_;
        profile_vertex_bytes_ += frame_vertex_bytes_;
        profile_vertex_range_uses_ += frame_vertex_range_uses_;
        profile_vertex_range_unique_ += frame_vertex_range_unique_;
        profile_vertex_reused_bytes_ += frame_vertex_reused_bytes_;
        profile_index_bytes_ += frame_index_bytes_;
        profile_upload_ns_ += frame_upload_ns_;
        profile_upload_bytes_ += frame_upload_bytes_;
        profile_upload_calls_ += frame_upload_calls_;
        profile_gpu_to_cpu_ns_ += frame_gpu_to_cpu_ns_;
        profile_gpu_to_cpu_calls_ += frame_gpu_to_cpu_calls_;
        profile_map_check_ns_ += frame_map_check_ns_;
        profile_uniform_read_ns_ += frame_uniform_read_ns_;
        profile_storage_read_ns_ += frame_storage_read_ns_;
        ++profile_samples_;
        frame_flush_wait_ns_ = 0;
        frame_flush_count_ = 0;
        frame_flush_reason_count_.fill(0);
        frame_flush_reason_ns_.fill(0);
        frame_execute_ns_ = 0;
        frame_execute_count_ = 0;
        frame_draw_ns_ = 0;
        frame_draw_count_ = 0;
        frame_draw_pipeline_ns_ = 0;
        frame_draw_descriptors_ns_ = 0;
        frame_draw_buffers_ns_ = 0;
        frame_draw_commands_ns_ = 0;
        frame_draw_texture_resolve_ns_ = 0;
        frame_sampled_swizzled_hash_ns_ = frame_sampled_swizzled_hash_bytes_ = frame_sampled_swizzled_hash_calls_ = 0;
        frame_sampled_pitch_refresh_ns_ = frame_sampled_pitch_refresh_calls_ = 0;
        frame_vertex_read_ns_ = frame_index_read_ns_ = 0;
        frame_vertex_bytes_ = frame_index_bytes_ = 0;
        frame_vertex_range_uses_ = frame_vertex_range_unique_ = frame_vertex_reused_bytes_ = 0;
        frame_vertex_uploads_.Invalidate();
        frame_upload_ns_ = frame_upload_bytes_ = frame_upload_calls_ = 0;
        frame_gpu_to_cpu_ns_ = frame_gpu_to_cpu_calls_ = frame_map_check_ns_ = 0;
        frame_uniform_read_ns_ = frame_storage_read_ns_ = 0;
        if (frame % 60 == 0) {
            const double samples = static_cast<double>(profile_samples_);
            const double intervals = static_cast<double>(std::max<u64>(1, profile_samples_ - 1));
            {
                // Time other threads waited for the NVN model lock (recorder calls block while Execute holds it).
                const u64 wait_ns = model_.lock.wait_ns(), contended = model_.lock.contended();
                LOG_INFO("renderer perf-lock frame %llu: model_lock_wait=%.2fms/frame contended=%.1f/frame",
                         static_cast<unsigned long long>(present_count_),
                         static_cast<double>(wait_ns - lock_wait_ns_seen_) / samples / 1e6,
                         static_cast<double>(contended - lock_contended_seen_) / samples);
                LOG_INFO("renderer perf-lock-yield frame %llu: yields=%llu backing-rebuilds=%llu unresolved-pools=%zu",
                         static_cast<unsigned long long>(present_count_),
                         static_cast<unsigned long long>(model_lock_yields_),
                         static_cast<unsigned long long>(backing_cache_.rebuilds),
                         backing_cache_.unresolved_pools.size());
                lock_wait_ns_seen_ = wait_ns;
                lock_contended_seen_ = contended;
            }
            LOG_INFO("renderer perf frame %llu: interval=%.2fms execute=%.2fms lists/frame=%.2f draw=%.2fms draws/frame=%.1f draw_parts=%.2f/%.2f/%.2f/%.2fms pipeline/descriptors/buffers/commands draw_details texture_resolve=%.2fms sampled_swizzled_hash=%.2fms/%.2fMiB/%.1f calls sampled_pitch_refresh=%.2fms/%.1f calls vertex_read=%.2fms index_read=%.2fms vertex=%.2fMiB ranges=%.0f/%.0f repeated=%.2fMiB index=%.2fMiB upload=%.2fms/%.2fMiB/%.0f calls gpu_to_cpu=%.2fms/%.0f calls mapped_checks=%.2fms ubo_read=%.2fms ssbo_read=%.2fms render=%.2fms present=%.2fms flush_wait=%.2fms flushes/frame=%.2f flush_image_recreate=%.2f/%.2fms flush_execute=%.2f/%.2fms flush_present=%.2f/%.2fms flush_dump=%.2f/%.2fms flush_compute=%.2f/%.2fms flush_stream=%.2f/%.2fms flush_mapped_readback=%.2f/%.2fms list_stream_peak=%.2fMiB descriptor_sets_peak=%llu push_descriptor_sets_peak=%llu",
                     static_cast<unsigned long long>(frame), profile_interval_ns_ / intervals / 1'000'000.0,
                     profile_execute_ns_ / samples / 1'000'000.0, profile_execute_count_ / samples,
                     profile_draw_ns_ / samples / 1'000'000.0, profile_draw_count_ / samples,
                     profile_draw_pipeline_ns_ / samples / 1'000'000.0,
                     profile_draw_descriptors_ns_ / samples / 1'000'000.0,
                     profile_draw_buffers_ns_ / samples / 1'000'000.0,
                     profile_draw_commands_ns_ / samples / 1'000'000.0,
                     profile_draw_texture_resolve_ns_ / samples / 1'000'000.0,
                     profile_sampled_swizzled_hash_ns_ / samples / 1'000'000.0,
                     profile_sampled_swizzled_hash_bytes_ / samples / (1024.0 * 1024.0),
                     profile_sampled_swizzled_hash_calls_ / samples,
                     profile_sampled_pitch_refresh_ns_ / samples / 1'000'000.0,
                     profile_sampled_pitch_refresh_calls_ / samples,
                     profile_vertex_read_ns_ / samples / 1'000'000.0,
                     profile_index_read_ns_ / samples / 1'000'000.0,
                     profile_vertex_bytes_ / samples / (1024.0 * 1024.0),
                     profile_vertex_range_uses_ / samples, profile_vertex_range_unique_ / samples,
                     profile_vertex_reused_bytes_ / samples / (1024.0 * 1024.0),
                     profile_index_bytes_ / samples / (1024.0 * 1024.0),
                     profile_upload_ns_ / samples / 1'000'000.0,
                     profile_upload_bytes_ / samples / (1024.0 * 1024.0), profile_upload_calls_ / samples,
                     profile_gpu_to_cpu_ns_ / samples / 1'000'000.0,
                     profile_gpu_to_cpu_calls_ / samples,
                     profile_map_check_ns_ / samples / 1'000'000.0,
                     profile_uniform_read_ns_ / samples / 1'000'000.0,
                     profile_storage_read_ns_ / samples / 1'000'000.0,
                     profile_render_ns_ / samples / 1'000'000.0, profile_present_ns_ / samples / 1'000'000.0,
                     profile_flush_wait_ns_ / samples / 1'000'000.0, profile_flush_count_ / samples,
                     profile_flush_reason_count_[static_cast<size_t>(FlushReason::ImageRecreate)] / samples,
                     profile_flush_reason_ns_[static_cast<size_t>(FlushReason::ImageRecreate)] / samples / 1'000'000.0,
                     profile_flush_reason_count_[static_cast<size_t>(FlushReason::Execute)] / samples,
                     profile_flush_reason_ns_[static_cast<size_t>(FlushReason::Execute)] / samples / 1'000'000.0,
                     profile_flush_reason_count_[static_cast<size_t>(FlushReason::Present)] / samples,
                     profile_flush_reason_ns_[static_cast<size_t>(FlushReason::Present)] / samples / 1'000'000.0,
                     (profile_flush_reason_count_[static_cast<size_t>(FlushReason::DumpImage)] +
                      profile_flush_reason_count_[static_cast<size_t>(FlushReason::DumpTarget)]) / samples,
                     (profile_flush_reason_ns_[static_cast<size_t>(FlushReason::DumpImage)] +
                      profile_flush_reason_ns_[static_cast<size_t>(FlushReason::DumpTarget)]) / samples / 1'000'000.0,
                     profile_flush_reason_count_[static_cast<size_t>(FlushReason::Compute)] / samples,
                     profile_flush_reason_ns_[static_cast<size_t>(FlushReason::Compute)] / samples / 1'000'000.0,
                     profile_flush_reason_count_[static_cast<size_t>(FlushReason::StreamFull)] / samples,
                     profile_flush_reason_ns_[static_cast<size_t>(FlushReason::StreamFull)] / samples / 1'000'000.0,
                     profile_flush_reason_count_[static_cast<size_t>(FlushReason::MappedReadback)] / samples,
                     profile_flush_reason_ns_[static_cast<size_t>(FlushReason::MappedReadback)] / samples / 1'000'000.0,
                     profile_stream_list_peak_bytes_ / (1024.0 * 1024.0),
                     static_cast<unsigned long long>(profile_descriptor_sets_peak_),
                     static_cast<unsigned long long>(profile_push_descriptor_sets_peak_));
            if (gpu_ts_) {
                // Appended after the existing renderer perf line: T_gpu and per-site gpu/wait/overhead (ms/frame).
                static constexpr const char* kSiteNames[] = {"image_recreate", "execute", "present", "dump_image",
                                                             "dump_target", "compute", "stream", "mapped_readback"};
                static_assert(std::size(kSiteNames) == static_cast<size_t>(FlushReason::Count));
                std::string sites;
                char buf[160];
                for (size_t i = 0; i < profile_gpu_sites_.count.size(); ++i) {
                    std::snprintf(buf, sizeof(buf), " gpu_%s=%.2f/%.2f/%.2fms", kSiteNames[i],
                                  profile_gpu_sites_.GpuMs(i) / samples, profile_gpu_sites_.WaitMs(i) / samples,
                                  profile_gpu_sites_.OverheadMs(i) / samples);
                    sites += buf;
                }
                const double total_gpu = profile_gpu_sites_.TotalGpuMs() / samples;
                const double total_wait = profile_gpu_sites_.TotalWaitMs() / samples;
                LOG_INFO("renderer perf-gpu frame %llu: T_gpu=%.2fms gpu_wait=%.2fms gpu_overhead=%.2fms%s "
                         "(site=gpu/wait/overhead per frame)",
                         static_cast<unsigned long long>(frame), total_gpu, total_wait, total_wait - total_gpu,
                         sites.c_str());
                profile_gpu_sites_.Reset();
                // Top compute programs by GPU time (per frame): program, dispatches, total/mean ms, grid.
                std::vector<std::pair<u64, DispatchTsStat>> top(profile_dispatch_ts_.begin(),
                                                                profile_dispatch_ts_.end());
                std::sort(top.begin(), top.end(),
                          [](const auto& a, const auto& b) { return a.second.gpu_ns > b.second.gpu_ns; });
                double total_dispatch_ms = 0.0;
                u64 total_dispatches = 0;
                for (const auto& [_, st] : top) {
                    total_dispatch_ms += st.gpu_ns / 1e6;
                    total_dispatches += st.count;
                }
                LOG_INFO("renderer perf-gpu-dispatch frame %llu: programs=%zu dispatches/frame=%.1f gpu=%.2fms/frame "
                         "skipped=%llu",
                         static_cast<unsigned long long>(frame), top.size(),
                         static_cast<double>(total_dispatches) / samples, total_dispatch_ms / samples,
                         static_cast<unsigned long long>(profile_dispatch_ts_skipped_));
                for (size_t i = 0; i < top.size() && i < 12; ++i) {
                    const DispatchTsStat& st = top[i].second;
                    LOG_INFO("renderer perf-gpu-dispatch   program=0x%llx gpu=%.3fms/frame disp=%.2f/frame "
                             "mean=%.1fus groups_avg=%.1f last_grid=%llux%llux%llu",
                             static_cast<unsigned long long>(top[i].first), st.gpu_ns / 1e6 / samples,
                             static_cast<double>(st.count) / samples,
                             st.gpu_ns / 1e3 / static_cast<double>(st.count),
                             static_cast<double>(st.threads_groups) / static_cast<double>(st.count),
                             static_cast<unsigned long long>(st.groups[0]),
                             static_cast<unsigned long long>(st.groups[1]),
                             static_cast<unsigned long long>(st.groups[2]));
                }
                profile_dispatch_ts_.clear();
                profile_dispatch_ts_skipped_ = 0;
            }
            if (win_resolves_ != 0 || win_swz_calls_ != 0 || dirty_tracker_) {
                LOG_INFO("renderer texrefresh frame %llu: resolves=%.1f/frame swz_refresh=%.1f calls/%.2fMiB big(>=1MiB)=%.1f calls/%.2fMiB hashed=%.1f calls/%.2fMiB pitch=%.1f calls skipped=%.1f",
                         static_cast<unsigned long long>(frame), win_resolves_ / samples, win_swz_calls_ / samples,
                         win_swz_bytes_ / samples / (1024.0 * 1024.0), win_swz_big_calls_ / samples,
                         win_swz_big_bytes_ / samples / (1024.0 * 1024.0), win_swz_hashed_ / samples,
                         win_swz_hashed_bytes_ / samples / (1024.0 * 1024.0), win_pitch_calls_ / samples,
                         win_pitch_skipped_ / samples);
                if (ubo_dedup_) {
                    LOG_INFO("renderer ubo-dedup frame %llu: hits=%.1f/frame (%.2fMiB saved) misses=%.1f/frame",
                             static_cast<unsigned long long>(frame), win_ubo_hits_ / samples,
                             win_ubo_hit_bytes_ / samples / (1024.0 * 1024.0), win_ubo_misses_ / samples);
                    win_ubo_hits_ = win_ubo_misses_ = win_ubo_hit_bytes_ = 0;
                }
                if (dirty_tracker_) {
                    const DirtyTrackerStats& t = dirty_tracker_->stats();
                    LOG_INFO("renderer texdirty frame %llu: epoch=%llu skipped=%.1f/frame (%.2fMiB) dirty=%.1f (%.2fMiB) unverified=%.1f (%.2fMiB) untrackable=%.1f clears=%llu clear=%.3fms query=%.3fms/frame (%.1f queries/frame)",
                             static_cast<unsigned long long>(frame),
                             static_cast<unsigned long long>(dirty_tracker_->epoch()), t.skipped / samples,
                             t.skipped_bytes / samples / (1024.0 * 1024.0), t.dirty / samples,
                             t.dirty_bytes / samples / (1024.0 * 1024.0), t.unverified / samples,
                             t.unverified_bytes / samples / (1024.0 * 1024.0), t.untrackable / samples,
                             static_cast<unsigned long long>(t.clears),
                             t.clears ? t.clear_ns / 1e6 / t.clears : 0.0, t.query_ns / 1e6 / samples,
                             t.queries / samples);
                    if (dirty_verify_) {
                        LOG_INFO("renderer texdirty-verify frame %llu: claims=%llu misses=%llu (cumulative)",
                                 static_cast<unsigned long long>(frame),
                                 static_cast<unsigned long long>(dirty_verify_claims_),
                                 static_cast<unsigned long long>(dirty_verify_miss_));
                    }
                    dirty_tracker_->ResetStats();
                }
                win_resolves_ = win_swz_calls_ = win_swz_bytes_ = win_swz_big_calls_ = win_swz_big_bytes_ = 0;
                win_swz_hashed_ = win_swz_hashed_bytes_ = win_pitch_calls_ = win_pitch_skipped_ = 0;
            }
            if (release_model_at_execute_end_) {
                LOG_INFO("renderer model-tail per-window: releases=%llu pending-writeback-fallbacks=%llu",
                         static_cast<unsigned long long>(profile_model_tail_releases_),
                         static_cast<unsigned long long>(profile_model_tail_fallbacks_));
            }
            profile_interval_ns_ = profile_render_ns_ = profile_present_ns_ = profile_execute_ns_ =
                profile_draw_ns_ = profile_flush_wait_ns_ = 0;
            profile_draw_pipeline_ns_ = profile_draw_descriptors_ns_ = profile_draw_buffers_ns_ =
                profile_draw_commands_ns_ = 0;
            profile_draw_texture_resolve_ns_ = profile_vertex_read_ns_ = profile_index_read_ns_ = 0;
            profile_sampled_swizzled_hash_ns_ = profile_sampled_swizzled_hash_bytes_ =
                profile_sampled_swizzled_hash_calls_ = profile_sampled_pitch_refresh_ns_ =
                    profile_sampled_pitch_refresh_calls_ = 0;
            profile_vertex_bytes_ = profile_index_bytes_ = 0;
            profile_vertex_range_uses_ = profile_vertex_range_unique_ = profile_vertex_reused_bytes_ = 0;
            profile_upload_ns_ = profile_upload_bytes_ = profile_upload_calls_ = 0;
            profile_gpu_to_cpu_ns_ = profile_gpu_to_cpu_calls_ = profile_map_check_ns_ = 0;
            profile_uniform_read_ns_ = profile_storage_read_ns_ = 0;
            profile_flush_count_ = profile_execute_count_ = profile_draw_count_ = profile_samples_ = 0;
            profile_model_tail_releases_ = profile_model_tail_fallbacks_ = 0;
            profile_flush_reason_count_.fill(0);
            profile_flush_reason_ns_.fill(0);
            profile_stream_list_peak_bytes_ = profile_descriptor_sets_peak_ = profile_push_descriptor_sets_peak_ = 0;
        }
    }
}

} // namespace

std::unique_ptr<nvn::Renderer> CreateNvnRenderer(SDL_Window* window) {
    auto r = std::make_unique<NvnRenderer>();
    if (!r->Init(window)) return nullptr;
    return r;
}

} // namespace nxrt::render
