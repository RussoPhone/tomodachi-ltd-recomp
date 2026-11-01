// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common.h"

// Host-side model of the title's NVN objects.
//
// Builders and textures/samplers/pools are followed through their API calls (shadow copies keyed by
// the guest address of the object). Small state objects (blend, depth-stencil, polygon...) are read
// straight from guest memory when bound: their layout is the driver's, decoded from its setters
// (docs/mission-5-native.md), which stays correct even when the title copies them around.
namespace nxrt::nvn {

using GuestAddr = u64;  // CPU virtual address in the title's address space
using GpuAddr = u64;    // GPU virtual address (what nvnBufferGetAddress / pools hand out)

enum class UniformBufferBindOrigin : u8 { Guest, DriverCbBind, DriverBindProgram };

inline constexpr u32 kInvalidNvnStage = 0xff;

// BindProgram's E18 macro uses Maxwell stage IDs (1=vertex, 2=tess-control,
// 3=tess-eval, 4=geometry, 5=fragment), while NVN object bindings use API
// stage slots (0=vertex, 1=fragment, 2=geometry, 3=tess-control, 4=tess-eval).
constexpr u32 NvnStageForMaxwellStage(u32 stage) noexcept {
    constexpr std::array<u32, 5> stage_map{0, 3, 4, 2, 1};
    return stage >= 1 && stage <= stage_map.size() ? stage_map[stage - 1] : kInvalidNvnStage;
}

constexpr UniformBufferBindOrigin DriverBindOriginForMethod(u32 method, u32 data = 1) noexcept {
    if (method == 0xe18) return UniformBufferBindOrigin::DriverBindProgram;
    if (method >= 0x904 && method < 0x904 + 8 * 5 && ((method - 0x904) & 7) == 0 && (data & 1) &&
        (data >> 4) < 3) {
        return UniformBufferBindOrigin::DriverCbBind;
    }
    return UniformBufferBindOrigin::Guest;
}

constexpr std::string_view UniformBufferBindOriginName(UniformBufferBindOrigin origin) noexcept {
    switch (origin) {
    case UniformBufferBindOrigin::Guest: return "guest";
    case UniformBufferBindOrigin::DriverCbBind: return "cb-bind";
    case UniformBufferBindOrigin::DriverBindProgram: return "bind-program-mme";
    }
    return "unknown";
}

struct MemoryPool {
    GuestAddr storage = 0;  // CPU address of the memory the title gave the pool
    u64 size = 0;
    u32 flags = 0;
    GpuAddr gpu = 0;        // NVNmemoryPool +0x18: GPU address of the pitch (buffer) mapping
    GpuAddr gpu2 = 0;       // NVNmemoryPool +0x20: GPU address of the second (texture) mapping
    u64 generation = 0;     // advances whenever this pool object is initialized
    bool is_virtual = false;
};

/// A range of a virtual memory pool backed by a physical pool (nvnMemoryPoolMapVirtual).
struct VirtualMapping {
    u64 size = 0;
    GuestAddr cpu = 0;
    GuestAddr virtual_pool = 0;
    u64 virtual_offset = 0;
    GuestAddr physical_pool = 0;
    u64 physical_generation = 0;
    u64 physical_offset = 0;
};

struct Buffer {
    GuestAddr pool = 0;
    u64 offset = 0;
    u64 size = 0;
};

struct TextureDesc {
    u32 flags = 0, target = 0, width = 0, height = 1, depth = 1, levels = 1, format = 0, samples = 0;
    std::array<u32, 4> swizzle{2, 3, 4, 5};
    u32 depth_stencil_mode = 0, stride = 0;
    GuestAddr pool = 0;
    u64 offset = 0;
    GuestAddr packaged_data = 0;
};

struct Texture {
    TextureDesc desc;
    u64 serial = 0;  // bumps when re-initialised at the same address
};

struct TextureView {  // what the title set on an NVNtextureView through nvnTextureViewSet*, by view address
    u32 base_level = 0, num_levels = 0, min_layer = 0, num_layers = 0;
    u32 format = 0;  // zero inherits the texture format
};

struct SamplerDesc {
    u32 min_filter = 0, mag_filter = 0;
    std::array<u32, 3> wrap{};
    float lod_bias = 0, lod_min = 0, lod_max = 1000, max_anisotropy = 1;
    u32 compare_mode = 0, compare_func = 0, reduction = 0;
    std::array<u32, 4> border{};
};

struct ShaderStage {
    GpuAddr code = 0;      // NVNshaderData.data
    GuestAddr control = 0; // NVNshaderData.control
};

struct Program {
    std::vector<ShaderStage> stages;
    u64 serial = 0;
};

enum class Op : u8 {
    BindBlendState, BindColorState, BindChannelMaskState, BindDepthStencilState, BindPolygonState,
    BindMultisampleState, BindVertexAttribState, BindVertexStreamState, BindProgram, BindVertexBuffer,
    BindUniformBuffer, BindStorageBuffer, BindTexture, SetRenderTargets, SetViewport, SetScissor,
    SetDepthRange, SetBlendColor, SetTexturePool, SetSamplerPool, ClearColor, ClearDepthStencil,
    DrawArrays, DrawElements, Barrier, CallCommands, SetRenderEnable, Other,
    UpdateBuffer, // inline const-buffer write: a[0] = GPU address, words = data (only the game's own inline writer)
    DispatchCompute, // a[0..2] = workgroup counts x, y, z
    ReportCounter,   // a[0] = NVNcounterType, a[1] = GPU address of the 16-byte {counter, timestamp} result
    RenderEnableConditional, // a[0] = NVNconditionalRenderMode (0 render if equal, 1 if not), a[1] = GPU address
                             // of two 16-byte counter reports
    CopyTexture,     // a[0] = source NVNtexture, a[1] = destination; words = src region[6], dst region[6],
                     // src level, src layer, dst level, dst layer; a[2] = NVNcopyFlags; a[3..4] = known storage sizes.
                     // New recordings append {present, layout low32, layout high32} for each packaged layout.
    BindTexturesDeferred, // a[0..3] = NVN stage, first slot, count, guest CPU pointer to u64 handles;
                          // the driver reads the array when submitting the list, including every replay
    ClearTexture,    // a[0] = NVNtexture, a[1] = channel mask (1 R, 2 G, 4 B, 8 A), a[2] = 1 if a region was given;
                     // f[0..3] = clear color; words = region[6] {x,y,z,w,h,d}, view base level, view min layer
    CopyTextureToBuffer, // a[0] = source texture, a[1] = destination GPU address, a[2] = flags;
                         // words = region[6], view base level, view min layer
};

struct Command {
    // SetViewport: a[4]==0 uses signed integer a[0..3]; a[4]==1 uses float f[0..3].
    Op op;
    std::array<u64, 6> a{};            // decoded integer arguments
    std::array<float, 4> f{};          // decoded float arguments
    std::vector<u32> words;            // copied state words / handle lists
};

using TextureBindings = std::array<std::array<u64, 32>, 6>;

constexpr bool ValidTextureBindingRange(u64 stage, u64 first, u64 count) noexcept {
    return stage < 6 && first <= 32 && count <= 32 - first;
}

// Shared by the renderer's command execution and the recorder contract test.
// Deferred handles are CPU addresses, not GPU buffer addresses. Validate/read the
// whole range before replacing state so an unavailable tail cannot leave a partial bind.
template<class ReadHandle>
bool ApplyTextureBinding(const Command& command, TextureBindings& bindings, ReadHandle&& read_handle) {
    const auto& a = command.a;
    if (command.op == Op::BindTexture) {
        if (!ValidTextureBindingRange(a[0], a[1], 1)) return false;
        bindings[a[0]][a[1]] = a[2];
        return true;
    }
    if (command.op != Op::BindTexturesDeferred || !ValidTextureBindingRange(a[0], a[1], a[2])) return false;
    if (!a[2]) return true;
    std::array<u64, 32> handles{};
    if (a[4]) {
        if (a[5] != 1 || command.words.size() != static_cast<size_t>(a[2] * 2)) return false;
        for (u64 i = 0; i < a[2]; ++i) {
            handles[i] = command.words[2 * i] | (u64{command.words[2 * i + 1]} << 32);
        }
    } else {
        if (!a[3] || a[3] > ~u64{0} - 8 * (a[2] - 1)) return false;
        for (u64 i = 0; i < a[2]; ++i) {
            const std::optional<u64> handle = read_handle(a[3] + 8 * i);
            if (!handle) return false;
            handles[i] = *handle;
        }
    }
    for (u64 i = 0; i < a[2]; ++i) bindings[a[0]][a[1] + i] = handles[i];
    return true;
}

struct CommandOrigin {
    u64 list_handle = 0;
    u32 depth = 0;
    friend bool operator==(const CommandOrigin&, const CommandOrigin&) = default;
};

struct CommandList {
    std::vector<Command> commands;
    // Diagnostic-only source locations for commands expanded from CallCommands at submit time.
    // Capture serialization intentionally stores the ordered commands, not these trace annotations.
    std::vector<CommandOrigin> origins;
};

struct Model;

struct SubmissionSnapshot {
    CommandList list;
    // Optional submit-time metadata. Guest buffers/backing are not owned by this snapshot.
    std::shared_ptr<Model> execution_model;
};

struct Window {
    std::vector<GuestAddr> textures;
    u32 acquired = 0;
};

struct FrameStats {
    u64 submits = 0, commands = 0, draws = 0, clears = 0, render_target_sets = 0;
    u64 unknown_handles = 0, unknown_textures = 0;
};

struct TexturePoolRegistrationStats {
    u64 new_entries = 0, changed_entries = 0, unchanged_entries = 0;
};

// std::mutex that counts how long other threads waited for it (diagnostic: NXRT_FRAME_PROFILE prints it).
class TimedMutex {
public:
    void lock() {
        if (mutex_.try_lock()) return;
        waiters_.fetch_add(1, std::memory_order_relaxed);
        const auto start = std::chrono::steady_clock::now();
        mutex_.lock();
        wait_ns_.fetch_add(static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                std::chrono::steady_clock::now() - start).count()),
                           std::memory_order_relaxed);
        contended_.fetch_add(1, std::memory_order_relaxed);
        waiters_.fetch_sub(1, std::memory_order_relaxed);
    }
    bool try_lock() { return mutex_.try_lock(); }
    void unlock() { mutex_.unlock(); }
    int waiters() const { return waiters_.load(std::memory_order_relaxed); }
    u64 wait_ns() const { return wait_ns_.load(std::memory_order_relaxed); }
    u64 contended() const { return contended_.load(std::memory_order_relaxed); }

private:
    std::mutex mutex_;
    std::atomic<int> waiters_{0};
    std::atomic<u64> wait_ns_{0}, contended_{0};
};

inline std::atomic<u64> next_model_resolution_identity{1};

struct Model {
    TimedMutex lock;
    // Distinguishes distinct Model lifetimes, even if an allocator reuses an address.
    const u64 resolution_identity = next_model_resolution_identity.fetch_add(1, std::memory_order_relaxed);
    std::unordered_map<GuestAddr, MemoryPool> pool_builders, pools;
    std::map<GpuAddr, VirtualMapping> virtual_mappings;  // by GPU address
    std::unordered_map<GuestAddr, Buffer> buffer_builders, buffers;
    std::unordered_map<GuestAddr, TextureDesc> texture_builders;
    std::unordered_map<GuestAddr, Texture> textures;
    std::unordered_map<GuestAddr, SamplerDesc> sampler_builders, samplers;
    std::unordered_map<GuestAddr, Program> programs;
    std::unordered_map<GuestAddr, Window> window_builders, windows;
    // texture/sampler pools: descriptor index -> object
    std::unordered_map<GuestAddr, std::unordered_map<u32, std::pair<GuestAddr, GuestAddr>>> texture_pools;
    TexturePoolRegistrationStats texture_pool_registration;
    std::unordered_map<GuestAddr, TextureView> texture_views;
    // the view each texture-pool entry was registered with, resolved at registration (views are often reused)
    std::unordered_map<GuestAddr, std::unordered_map<u32, TextureView>> texture_pool_views;
    std::unordered_map<GuestAddr, std::unordered_map<u32, GuestAddr>> sampler_pools;
    // command buffers being recorded, and finished lists by the exact handle EndRecording returned
    std::unordered_map<GuestAddr, std::shared_ptr<CommandList>> recording;
    std::unordered_map<u64, std::shared_ptr<CommandList>> finished;
    u64 serial = 0;
    u64 pool_generation = 0;
    // Changes when a submitted private model captures different GPU pool addresses.
    u64 pool_address_revision = 0;
    // Bumped (under `lock`) whenever a pool, a virtual mapping, a texture object or a texture-pool entry changes. The
    // renderer memoizes address and image resolution per epoch (NXRT_NO_RESOLVE_MEMO=1 disables that).
    std::atomic<u64> epoch{1};
    void Touch() { epoch.fetch_add(1, std::memory_order_release); }
    FrameStats frame;
    u64 frames = 0;
};

// Brackets a mutation of the objects the renderer memoizes (see Model::epoch): bumps before and after, so a reader
// cannot pair the new epoch with half-updated data.
struct EpochScope {
    explicit EpochScope(Model& m) : model(m) { model.Touch(); }
    ~EpochScope() { model.Touch(); }
    Model& model;
};

} // namespace nxrt::nvn
