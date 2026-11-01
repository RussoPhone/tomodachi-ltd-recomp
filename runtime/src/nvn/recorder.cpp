// SPDX-License-Identifier: GPL-3.0-or-later
// Follows the game's NVN calls into the Model: objects through their builders,
// command buffers into command lists keyed by the exact handle EndRecording
// returned, submissions and presents handed to the renderer.
#include "nvn/recorder.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <set>
#include <functional>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <fmt/format.h>

#include "memory.h"
#include "nvn/nvn_layer.h"
#include "nvn/submission_snapshot.h"
#include "nvn/execution_model_snapshot.h"
#include "nvn/texture_copy.h"

namespace nxrt::nvn {

namespace {

Model g_model;
// Driver-reported sizes, captured into copy commands; stale builder results are
// accepted only when the complete storage layout still matches.
struct TextureStorageSize { TextureDesc layout; u64 bytes; };
std::map<GuestAddr, TextureStorageSize> g_builder_storage_sizes, g_texture_storage_sizes;
std::map<GuestAddr, u64> g_builder_layouts, g_texture_layouts;
// Diagnostics (NXRT_NVN_FRAMES): commands in lists as they were ended, against those reached by submissions.
u64 g_ended_lists = 0, g_ended_draws = 0, g_ended_instanced = 0, g_ended_clears = 0, g_reached_draws = 0, g_reached_clears = 0;
Renderer* g_renderer = nullptr;

// Synchronous B3: the private render model is changed only between submits.
// All recording functions run under g_model.lock. Each submit owns its
// captured changes and is applied in sequence through its Execute return.
struct ExecutionModelJournal {
    std::shared_ptr<Model> model;
    std::vector<std::function<void(Model&)>> pending;
    u64 next_capture = 0;  // protected by g_model.lock
    u64 next_execute = 0;  // protected by g_journal_submit_mutex
    bool poisoned = false; // a partially applied batch cannot be reused safely
};
std::unique_ptr<ExecutionModelJournal> g_execution_journal;
std::mutex g_journal_submit_mutex;
std::condition_variable g_journal_submit_cv;

struct JournalSubmission {
    struct PoolAddresses { GuestAddr handle; GpuAddr gpu, gpu2; };
    std::vector<std::function<void(Model&)>> changes;
    std::vector<PoolAddresses> pools;
    u64 sequence = 0, pool_generation = 0, epoch = 0, serial = 0, frames = 0;
    TexturePoolRegistrationStats texture_pool_registration;
};

template<class Map, class Key>
void JournalSet(Map Model::*member, Key key, const typename Map::mapped_type& value) {
    if (!g_execution_journal || !g_execution_journal->model) return;
    g_execution_journal->pending.emplace_back([member, key, value](Model& target) {
        (target.*member)[key] = value;
    });
}

template<class Map, class Key>
void JournalErase(Map Model::*member, Key key) {
    if (!g_execution_journal || !g_execution_journal->model) return;
    g_execution_journal->pending.emplace_back([member, key](Model& target) {
        (target.*member).erase(key);
    });
}

void JournalTextureRegistration(GuestAddr pool, u32 id, std::pair<GuestAddr, GuestAddr> entry, TextureView view) {
    if (!g_execution_journal || !g_execution_journal->model) return;
    g_execution_journal->pending.emplace_back([pool, id, entry, view](Model& target) {
        target.texture_pools[pool][id] = entry;
        target.texture_pool_views[pool][id] = view;
    });
}

void JournalSamplerRegistration(GuestAddr pool, u32 id, GuestAddr sampler) {
    if (!g_execution_journal || !g_execution_journal->model) return;
    g_execution_journal->pending.emplace_back([pool, id, sampler](Model& target) {
        target.sampler_pools[pool][id] = sampler;
    });
}


u32 R32(u64 a) {
    return g_memory.Read<u32>(a);
}
u64 R64(u64 a) {
    return g_memory.Read<u64>(a);
}
float AsFloat(u32 v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

std::shared_ptr<CommandList> ListFor(GuestAddr cmd) {
    auto& slot = g_model.recording[cmd];
    if (!slot) slot = std::make_shared<CommandList>();
    return slot;
}

// What the title last set on the NVNtextureView at `view`; no view (or an untouched one) is the whole texture.
TextureView ViewOf(const Model& m, GuestAddr view) {
    if (!view) return {};
    const auto it = m.texture_views.find(view);
    return it == m.texture_views.end() ? TextureView{} : it->second;
}

void Record(GuestAddr cmd, Command c) {
    std::lock_guard lock(g_model.lock);
    ListFor(cmd)->commands.push_back(std::move(c));
}

void On(const char* name, Observer on_call, Observer on_return = {}) {
    Observe(name, std::move(on_call), std::move(on_return));
}

// Walks a submitted list (and the lists it calls) for statistics. Lock held.
void Count(u64 handle, int depth) {
    auto it = g_model.finished.find(handle);
    if (it == g_model.finished.end()) {
        ++g_model.frame.unknown_handles;
        return;
    }
    for (const Command& c : it->second->commands) {
        ++g_model.frame.commands;
        switch (c.op) {
        case Op::DrawArrays:
        case Op::DrawElements: ++g_model.frame.draws; ++g_reached_draws; break;
        case Op::ClearColor:
        case Op::ClearDepthStencil: ++g_model.frame.clears; ++g_reached_clears; break;
        case Op::SetRenderTargets: ++g_model.frame.render_target_sets; break;
        case Op::CallCommands:
            if (depth < 4) {
                for (size_t i = 0; i + 1 < c.words.size(); i += 2) {
                    Count(c.words[i] | (u64{c.words[i + 1]} << 32), depth + 1);
                }
            }
            break;
        default: break;
        }
    }
}


// ---- inline command words
// The game's own writer (main+0x2b70f0) stores Maxwell method words straight into command memory while an nnSdk
// flag is set, bypassing the NVN calls the recorder observes. The command buffer object keeps the write cursor at
// +0 and the end of the chunk at +8. The words between one call's return and the next call's entry are the game's;
// they are decoded into the same commands the API calls would have produced, in order.
struct InlineState {
    GuestAddr pos = 0, end = 0;
    std::array<u64, 16> vb_start{}, vb_limit_hi{};
    u32 cb_size = 0, cb_hi = 0, cb_lo = 0, cb_pos = 0; // const buffer being written (methods 0x8e0..0x8e3)
    GuestAddr call_pos = 0; // cursor at the entry of the NVN call in progress (0: none)
    GuestAddr call_end = 0;
    std::string call_name;
};
std::unordered_map<GuestAddr, InlineState> g_inline;
u64 g_driver_binds = 0;
u64 g_inline_updates = 0, g_inline_binds = 0, g_inline_words = 0, g_inline_draws = 0, g_inline_vbs = 0, g_inline_skipped = 0;
std::set<u32> g_inline_unknown;

// Decodes [from, to) and appends the commands to the list. Lock held.
void DecodeInline(GuestAddr cmd, InlineState& st, GuestAddr from, GuestAddr to) {
    static const bool trace = std::getenv("NXRT_TRACE_INLINE") != nullptr;
    auto list = ListFor(cmd);
    GuestAddr at = from;
    while (at < to) {
        const u32 h = R32(at);
        const u32 type = h >> 29, count = (h >> 16) & 0x1fff, method = h & 0x1fff;
        at += 4;
        if (type == 4) { // immediate
            if (method >= 0x904 && method < 0x904 + 8 * 5 && ((method - 0x904) & 7) == 0 && (count & 1)) {
                // Bind const buffer (index = count >> 4) to a stage; indices below kUniformSlotBase belong to the driver.
                const u32 index = count >> 4;
                if (index >= 3) {
                    Command c{Op::BindUniformBuffer};
                    c.a = {(method - 0x904) >> 3, index - 3, (u64{st.cb_hi} << 32) | st.cb_lo, st.cb_size, 0,
                           static_cast<u64>(UniformBufferBindOrigin::Guest)};
                    list->commands.push_back(std::move(c));
                    ++g_inline_binds;
                }
                continue;
            }
            if (trace && g_inline_unknown.insert(method << 2).second)
                LOG_INFO("inline: immediate method %x data %x", method << 2, count);
            continue;
        }
        if (type != 1 && type != 3 && type != 5) {
            ++g_inline_skipped;
            continue;
        }
        const u32 n = std::min<u32>(count, static_cast<u32>((to - at) / 4));
        // Each data word goes to a method: incrementing (1), the same (3) or the first then the next (5).
        for (u32 i = 0; i < n; ++i) {
            const u32 value = R32(at + 4 * i);
            const u32 m = type == 1 ? method + i : type == 3 ? method : method + (i ? 1 : 0);
            ++g_inline_words;
            if (m >= 0x700 + 1 && m < 0x700 + 64 && (m & 3) == 1) { // vertex array i: start high (low follows)
                const u32 idx = (m - 0x700) >> 2;
                st.vb_start[idx] = u64{value} << 32;
            } else if (m >= 0x700 + 2 && m < 0x700 + 64 && (m & 3) == 2) {
                st.vb_start[(m - 0x700) >> 2] |= value;
            } else if (m >= 0x7c0 && m < 0x7c0 + 32 && (m & 1) == 0) {
                st.vb_limit_hi[(m - 0x7c0) >> 1] = value;
            } else if (m >= 0x7c0 && m < 0x7c0 + 32) { // limit low completes the pair
                const u32 idx = (m - 0x7c0) >> 1;
                const u64 limit = (st.vb_limit_hi[idx] << 32) | value; // inclusive end
                Command c{Op::BindVertexBuffer};
                c.a[0] = idx;
                c.a[1] = st.vb_start[idx];
                c.a[2] = limit >= st.vb_start[idx] ? limit - st.vb_start[idx] + 1 : 0;
                list->commands.push_back(std::move(c));
                ++g_inline_vbs;
            } else if (m == 0x8e0) {
                st.cb_size = value;
            } else if (m == 0x8e1) {
                st.cb_hi = value;
            } else if (m == 0x8e2) {
                st.cb_lo = value;
            } else if (m == 0x8e3) {
                st.cb_pos = value;
            } else if (m >= 0x8e4 && m < 0x8e4 + 16) { // CB_DATA: the position advances per word
                const GpuAddr dst = ((u64{st.cb_hi} << 32) | st.cb_lo) + st.cb_pos;
                st.cb_pos += 4;
                if (!list->commands.empty() && list->commands.back().op == Op::UpdateBuffer &&
                    list->commands.back().a[0] + 4 * list->commands.back().words.size() == dst) {
                    list->commands.back().words.push_back(value);
                } else {
                    Command c{Op::UpdateBuffer};
                    c.a[0] = dst;
                    c.words.push_back(value);
                    list->commands.push_back(std::move(c));
                }
                ++g_inline_updates;
            } else if (m == 0xe38 && i == 0 && n == 5) { // DrawElements macro: mode, index address hi/lo, type, count
                Command c{Op::DrawElements};
                const u32 w[5] = {R32(at), R32(at + 4), R32(at + 8), R32(at + 12), R32(at + 16)};
                c.a = {w[0], w[3], w[4], (u64{w[1]} << 32) | w[2], 0, 0};
                list->commands.push_back(std::move(c));
                ++g_inline_draws;
            } else if (trace && g_inline_unknown.insert(m << 2).second) {
                LOG_INFO("inline: method %x (%u words, type %u) first word %x", m << 2, count, type, value);
            }
        }
        at += 4 * n;
    }
}

// Words the NVN driver itself wrote during one call. The call is already recorded from its arguments, so only the
// driver's own const buffer binds (slots below kUniformSlotBase) are taken. BindProgram does not bind c1 with
// CB_BIND: it calls macro 0xe18 once per stage with [Maxwell stage (1 = vertex B .. 5 = fragment), ?, program
// offset, ?, c1 address >> 8]; c1 is the program's literal constant section, placed after its code. Shaders index it
// (the nn::font text shader reads its quad corners from c1), so it is bound like a uniform buffer.
constexpr u32 kDriverConstSize = 0x1000; // c1 size is not among the macro arguments; literal tables are small
void ScanDriverBinds(GuestAddr cmd, InlineState& st, GuestAddr from, GuestAddr to) {
    static const bool trace = std::getenv("NXRT_TRACE_DRIVER_BINDS") != nullptr;
    static const bool trace_origin = std::getenv("NXRT_TRACE_DRIVER_BIND_ORIGIN") != nullptr;
    static const u64 trace_program = std::getenv("NXRT_TRACE_DRIVER_BIND_PROGRAM")
                                         ? std::strtoull(std::getenv("NXRT_TRACE_DRIVER_BIND_PROGRAM"), nullptr, 16)
                                         : 0;
    auto list = ListFor(cmd);
    const auto bind = [&](u32 stage, u32 index, GpuAddr addr, u32 size, UniformBufferBindOrigin origin) {
        if (index >= 3 || stage >= 5) return;
        Command c{Op::BindUniformBuffer};
        c.a = {stage, index, addr, size, 1, static_cast<u64>(origin)};
        if (trace && g_driver_binds < 64) {
            LOG_INFO("driver-bind: cmd=%llx stage=%u c%u addr=%llx size=%x", static_cast<unsigned long long>(cmd), stage,
                     index, static_cast<unsigned long long>(addr), size);
        }
        list->commands.push_back(std::move(c));
        ++g_driver_binds;
    };
    const auto cb_bind = [&](u32 stage, u32 method, u32 data) {
        const UniformBufferBindOrigin origin = DriverBindOriginForMethod(method, data);
        if (origin != UniformBufferBindOrigin::Guest) {
            if (trace_origin && trace_program && (data >> 4) == 1) {
                u64 program_handle = 0;
                for (auto command = list->commands.rbegin(); command != list->commands.rend(); ++command) {
                    if (command->op == Op::BindProgram) {
                        program_handle = command->a[0];
                        break;
                    }
                }
                if (program_handle == trace_program) {
                    LOG_INFO("driver-bind-origin: origin=cb-bind stage=%u method=0x%x data=0x%x "
                             "cb_gpu_va=0x%llx size=0x%x program=0x%llx",
                             stage, method, data,
                             static_cast<unsigned long long>((u64{st.cb_hi} << 32) | st.cb_lo), st.cb_size,
                             static_cast<unsigned long long>(program_handle));
                }
            }
            bind(stage, data >> 4, (u64{st.cb_hi} << 32) | st.cb_lo, st.cb_size, origin);
        }
    };
    GuestAddr at = from;
    while (at < to) {
        const u32 h = R32(at);
        const u32 type = h >> 29, count = (h >> 16) & 0x1fff, method = h & 0x1fff;
        at += 4;
        if (type == 4) {
            if (method >= 0x904 && method < 0x904 + 8 * 5 && ((method - 0x904) & 7) == 0) {
                cb_bind((method - 0x904) >> 3, method, count);
            }
            continue;
        }
        if (type != 1 && type != 3 && type != 5) return; // unknown encoding: stop rather than misread
        const u32 n = std::min<u32>(count, static_cast<u32>((to - at) / 4));
        if (method == 0xe18 && n == 5) { // BindProgram macro
            const u32 args[5] = {R32(at), R32(at + 4), R32(at + 8), R32(at + 12), R32(at + 16)};
            const u32 stage = args[0], nvn_stage = NvnStageForMaxwellStage(stage), c1 = args[4];
            if (c1) {
                u64 program_handle = 0;
                for (auto command = list->commands.rbegin(); command != list->commands.rend(); ++command) {
                    if (command->op == Op::BindProgram) {
                        program_handle = command->a[0];
                        break;
                    }
                }
                if (trace_origin && trace_program && program_handle == trace_program) {
                    std::string shader_codes;
                    if (const auto program = g_model.programs.find(program_handle); program != g_model.programs.end()) {
                        for (const ShaderStage& shader : program->second.stages) {
                            if (!shader_codes.empty()) shader_codes += ',';
                            shader_codes += fmt::format("0x{:x}", shader.code);
                        }
                    }
                    LOG_INFO("driver-bind-origin: origin=bind-program-mme stage=%u nvn_stage=%u "
                             "args=%08x,%08x,%08x,%08x,%08x "
                             "program_offset=0x%x c1_gpu_va=0x%llx program=0x%llx shader_codes=%s",
                             stage, nvn_stage, args[0], args[1], args[2], args[3], args[4], args[2],
                             static_cast<unsigned long long>(u64{c1} << 8),
                             static_cast<unsigned long long>(program_handle), shader_codes.c_str());
                }
            }
            if (nvn_stage != kInvalidNvnStage && c1) {
                bind(nvn_stage, 1, u64{c1} << 8, kDriverConstSize,
                     DriverBindOriginForMethod(method));
            }
            at += 4 * n;
            continue;
        }
        for (u32 i = 0; i < n; ++i) {
            const u32 value = R32(at + 4 * i);
            const u32 m = type == 1 ? method + i : type == 3 ? method : method + (i ? 1 : 0);
            if (m == 0x8e0) st.cb_size = value;
            else if (m == 0x8e1) st.cb_hi = value;
            else if (m == 0x8e2) st.cb_lo = value;
            else if (m >= 0x904 && m < 0x904 + 8 * 5 && ((m - 0x904) & 7) == 0)
                cb_bind((m - 0x904) >> 3, m, value);
        }
        at += 4 * n;
    }
}

void InlineBefore(GuestAddr cmd, const std::string& name) {
    std::lock_guard lock(g_model.lock);
    auto it = g_inline.find(cmd);
    if (it == g_inline.end()) return;
    InlineState& st = it->second;
    const GuestAddr cursor = R64(cmd), end = R64(cmd + 8);
    if (end == st.end && cursor > st.pos && cursor - st.pos <= 0x40000) DecodeInline(cmd, st, st.pos, cursor);
    else if (cursor != st.pos) ++g_inline_skipped;
    st.call_pos = end == st.end ? cursor : 0;
    st.call_end = end;
    st.call_name = name;
    if (name == "nvnCommandBufferEndRecording" || name == "nvnCommandBufferBeginRecording" ||
        name == "nvnCommandBufferFinalize" || name == "nvnCommandBufferInitialize") {
        g_inline.erase(cmd);
    }
}

void InlineAfter(GuestAddr cmd) {
    std::lock_guard lock(g_model.lock);
    // Commands and cursors of a recording are only trusted after the game began one on this buffer.
    if (!g_model.recording.count(cmd)) {
        g_inline.erase(cmd);
        return;
    }
    InlineState& st = g_inline[cmd];
    const GuestAddr cursor = R64(cmd), end = R64(cmd + 8);
    static const bool scan_driver = !std::getenv("NXRT_NVN_NO_DRIVER_BINDS");
    if (scan_driver && st.call_pos && end == st.end && cursor > st.call_pos && cursor - st.call_pos <= 0x10000) {
        ScanDriverBinds(cmd, st, st.call_pos, cursor);
    }
    if (static const char* want = std::getenv("NXRT_TRACE_CALL_WORDS"); want && st.call_name == want) {
        static u32 traced = 0;
        if (traced++ < 6) {
            std::string words;
            const GuestAddr from = st.call_pos ? st.call_pos : cursor;
            for (GuestAddr a = from; a < cursor && a < from + 4 * 48; a += 4) {
                char buf[12];
                std::snprintf(buf, sizeof(buf), " %08x", R32(a));
                words += buf;
            }
            LOG_INFO("call-words: %s cmd=%llx entry=%llx/%llx exit=%llx/%llx:%s", want,
                     static_cast<unsigned long long>(cmd), static_cast<unsigned long long>(st.call_pos),
                     static_cast<unsigned long long>(st.call_end), static_cast<unsigned long long>(cursor),
                     static_cast<unsigned long long>(end), words.c_str());
        }
    }
    st.call_pos = 0;
    st.pos = cursor;
    st.end = end;
}

void RegisterObjects() {
    auto& m = g_model;
    const bool trace_render_memory = std::getenv("NXRT_TRACE_RENDER_MEMORY") != nullptr;
    // ---- memory pools
    On("nvnMemoryPoolBuilderSetDefaults", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.pool_builders[a.X(0)] = {};
    });
    On("nvnMemoryPoolBuilderSetStorage", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        auto& b = m.pool_builders[a.X(0)];
        b.storage = a.X(1);
        b.size = a.X(2);
    });
    On("nvnMemoryPoolBuilderSetFlags", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.pool_builders[a.X(0)].flags = static_cast<u32>(a.X(1));
    });
    On("nvnMemoryPoolInitialize", {}, [&m](Call& a) {
        // NVNmemoryPool keeps its GPU addresses at +0x18 (buffer view) and +0x20 (texture view).
        std::lock_guard lock(m.lock);
        const EpochScope epoch_scope(m);
        const GuestAddr handle = a.X(0);
        // A pool handle can be reinitialized over the same storage. Drop mappings captured from
        // its previous lifetime, whether it owned the virtual range or supplied physical backing.
        for (auto it = m.virtual_mappings.begin(); it != m.virtual_mappings.end();) {
            if (it->second.virtual_pool == handle || it->second.physical_pool == handle) {
                JournalErase(&Model::virtual_mappings, it->first);
                it = m.virtual_mappings.erase(it);
            } else {
                ++it;
            }
        }
        MemoryPool p = m.pool_builders[a.X(1)];
        p.gpu = R64(a.X(0) + 0x18);
        p.gpu2 = R64(a.X(0) + 0x20);
        if (++m.pool_generation == 0) ++m.pool_generation;
        p.generation = m.pool_generation;
        m.pools[handle] = p;
        JournalSet(&Model::pools, handle, p);
    });
    On("nvnMemoryPoolMapVirtual", [&m](Call& a) {
        // (virtualPool, count, const NVNmappingRequest*): 40-byte requests: physical pool (0 = unmap),
        // physical offset, virtual offset, size, storage class.
        std::lock_guard lock(m.lock);
        const EpochScope epoch_scope(m);
        m.pools[a.X(0)].is_virtual = true;
        JournalSet(&Model::pools, a.X(0), m.pools[a.X(0)]);
        const GpuAddr base = R64(a.X(0) + 0x18);
        for (u64 i = 0; i < a.X(1); ++i) {
            const GuestAddr req = a.X(2) + 40 * i;
            const GuestAddr phys = R64(req);
            const u64 poff = R64(req + 8), voff = R64(req + 16), size = R64(req + 24);
            const GpuAddr gpu = base + voff;
            for (auto it = m.virtual_mappings.lower_bound(gpu); it != m.virtual_mappings.end() && it->first < gpu + size;) {
                JournalErase(&Model::virtual_mappings, it->first);
                it = m.virtual_mappings.erase(it);
            }
            if (phys) {
                if (auto p = m.pools.find(phys); p != m.pools.end() && p->second.storage) {
                    m.virtual_mappings[gpu] = VirtualMapping{
                        size, p->second.storage + poff, a.X(0), voff, phys, p->second.generation, poff
                    };
                    JournalSet(&Model::virtual_mappings, gpu, m.virtual_mappings[gpu]);
                }
            }
        }
    });
    On("nvnMemoryPoolFinalize", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        const EpochScope epoch_scope(m);
        const GuestAddr handle = a.X(0);
        m.pools.erase(handle);
        JournalErase(&Model::pools, handle);
        for (auto it = m.virtual_mappings.begin(); it != m.virtual_mappings.end();) {
            if (it->second.virtual_pool == handle || it->second.physical_pool == handle) {
                JournalErase(&Model::virtual_mappings, it->first);
                it = m.virtual_mappings.erase(it);
            } else {
                ++it;
            }
        }
    });
    // ---- buffers
    On("nvnBufferBuilderSetDefaults", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.buffer_builders[a.X(0)] = {};
    });
    On("nvnBufferBuilderSetStorage", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.buffer_builders[a.X(0)] = Buffer{a.X(1), a.X(2), a.X(3)};
    });
    On("nvnBufferInitialize", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.buffers[a.X(0)] = m.buffer_builders[a.X(1)];
    });
    On("nvnBufferFinalize", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.buffers.erase(a.X(0));
    });
    // ---- textures
    auto tb = [&m](Call& a) -> TextureDesc& { return m.texture_builders[a.X(0)]; };
    On("nvnTextureBuilderSetDefaults", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        g_builder_storage_sizes.erase(a.X(0));
        g_builder_layouts.erase(a.X(0));
        m.texture_builders[a.X(0)] = {};
    });
    auto tex_field = [&](const char* name, u32 TextureDesc::*field) {
        On(name, [&m, tb, field](Call& a) {
            std::lock_guard lock(m.lock);
            tb(a).*field = static_cast<u32>(a.X(1));
        });
    };
    tex_field("nvnTextureBuilderSetFlags", &TextureDesc::flags);
    tex_field("nvnTextureBuilderSetTarget", &TextureDesc::target);
    tex_field("nvnTextureBuilderSetWidth", &TextureDesc::width);
    tex_field("nvnTextureBuilderSetHeight", &TextureDesc::height);
    tex_field("nvnTextureBuilderSetDepth", &TextureDesc::depth);
    tex_field("nvnTextureBuilderSetLevels", &TextureDesc::levels);
    tex_field("nvnTextureBuilderSetFormat", &TextureDesc::format);
    tex_field("nvnTextureBuilderSetSamples", &TextureDesc::samples);
    tex_field("nvnTextureBuilderSetDepthStencilMode", &TextureDesc::depth_stencil_mode);
    tex_field("nvnTextureBuilderSetStride", &TextureDesc::stride);
    On("nvnTextureBuilderSetSize2D", [&m, tb](Call& a) {
        std::lock_guard lock(m.lock);
        tb(a).width = static_cast<u32>(a.X(1));
        tb(a).height = static_cast<u32>(a.X(2));
    });
    On("nvnTextureBuilderSetSize3D", [&m, tb](Call& a) {
        std::lock_guard lock(m.lock);
        tb(a).width = static_cast<u32>(a.X(1));
        tb(a).height = static_cast<u32>(a.X(2));
        tb(a).depth = static_cast<u32>(a.X(3));
    });
    On("nvnTextureBuilderSetSwizzle", [&m, tb](Call& a) {
        std::lock_guard lock(m.lock);
        for (int i = 0; i < 4; ++i) tb(a).swizzle[i] = static_cast<u32>(a.X(1 + i));
    });
    On("nvnTextureBuilderSetStorage", [&m, tb](Call& a) {
        std::lock_guard lock(m.lock);
        tb(a).pool = a.X(1);
        tb(a).offset = a.X(2);
    });
    On("nvnTextureBuilderSetPackagedTextureData", [&m, tb](Call& a) {
        std::lock_guard lock(m.lock);
        tb(a).packaged_data = a.X(1);
    });
    On("nvnTextureInitialize", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        const EpochScope epoch_scope(m);
        const TextureDesc desc = m.texture_builders[a.X(1)];
        g_texture_storage_sizes.erase(a.X(0));
        g_texture_layouts.erase(a.X(0));
        if (auto layout = g_builder_layouts.find(a.X(1)); layout != g_builder_layouts.end())
            g_texture_layouts[a.X(0)] = layout->second;
        if (auto size = g_builder_storage_sizes.find(a.X(1)); size != g_builder_storage_sizes.end() &&
            SameTextureStorageLayout(size->second.layout, desc)) {
            g_texture_storage_sizes[a.X(0)] = size->second;
        }
        const u64 serial = ++m.serial;
        m.textures[a.X(0)] = Texture{desc, serial};
        JournalSet(&Model::textures, a.X(0), m.textures[a.X(0)]);
        if (std::getenv("NXRT_TRACE_DRAWS") && desc.format == 0x25 &&
            (desc.offset == 0 || desc.offset == 0x7fb0000)) {
            static u32 shown = 0;
            if (shown++ < 64) {
                LOG_INFO("trace: texture-init object=%llx builder=%llx storage=%llx+%llx size=%ux%u format=%x "
                         "target=%x levels=%u serial=%llu",
                         static_cast<unsigned long long>(a.X(0)), static_cast<unsigned long long>(a.X(1)),
                         static_cast<unsigned long long>(desc.pool), static_cast<unsigned long long>(desc.offset),
                         desc.width, desc.height, desc.format, desc.target, desc.levels,
                         static_cast<unsigned long long>(serial));
            }
        }
    });
    On("nvnTextureFinalize", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        g_texture_storage_sizes.erase(a.X(0));
        g_texture_layouts.erase(a.X(0));
        const EpochScope epoch_scope(m);
        m.textures.erase(a.X(0));
        JournalErase(&Model::textures, a.X(0));
    });
    // ---- texture views: only what the renderer needs to pick a mip level or layer
    On("nvnTextureViewSetFormat", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.texture_views[a.X(0)].format = static_cast<u32>(a.X(1));
        JournalSet(&Model::texture_views, a.X(0), m.texture_views[a.X(0)]);
    });
    On("nvnTextureViewSetDefaults", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.texture_views[a.X(0)] = {};
        JournalSet(&Model::texture_views, a.X(0), m.texture_views[a.X(0)]);
    });
    On("nvnTextureViewSetLevels", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        TextureView& v = m.texture_views[a.X(0)];
        v.base_level = static_cast<u32>(a.X(1));
        v.num_levels = static_cast<u32>(a.X(2));
        JournalSet(&Model::texture_views, a.X(0), v);
    });
    On("nvnTextureViewSetLayers", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        TextureView& v = m.texture_views[a.X(0)];
        v.min_layer = static_cast<u32>(a.X(1));
        v.num_layers = static_cast<u32>(a.X(2));
        JournalSet(&Model::texture_views, a.X(0), v);
    });
    On("nvnTexturePoolRegisterTexture", [&m, trace_render_memory](Call& a) {
        std::lock_guard lock(m.lock);
        const u32 id = static_cast<u32>(a.X(1));
        auto& registered_view = m.texture_pool_views[a.X(0)][id];
        const TextureView new_view = ViewOf(m, a.X(3));
        auto& entries = m.texture_pools[a.X(0)];
        const std::pair<GuestAddr, GuestAddr> registered{a.X(2), a.X(3)};
        auto entry = entries.find(id);
        const bool changed = entry == entries.end() || entry->second != registered ||
                             std::memcmp(&registered_view, &new_view, sizeof new_view) != 0;
        if (changed) m.Touch();
        registered_view = new_view;
        if (entry == entries.end()) {
            if (trace_render_memory) ++m.texture_pool_registration.new_entries;
            entries.emplace(id, registered);
        } else if (entry->second == registered) {
            if (trace_render_memory) ++m.texture_pool_registration.unchanged_entries;
        } else {
            if (trace_render_memory) ++m.texture_pool_registration.changed_entries;
            entry->second = registered;
        }
        if (changed) {
            m.Touch();
            JournalTextureRegistration(a.X(0), id, registered, new_view);
        }
        if (std::getenv("NXRT_TRACE_DRAWS") && (id == 0x2a8 || id == 0x2b7)) {
            if (auto texture = m.textures.find(a.X(2)); texture != m.textures.end()) {
                const auto& d = texture->second.desc;
                LOG_INFO("trace: register texture-pool=%llx id=%x texture=%llx view=%llx storage=%llx+%llx "
                         "size=%ux%u format=%x serial=%llu",
                         static_cast<unsigned long long>(a.X(0)), id, static_cast<unsigned long long>(a.X(2)),
                         static_cast<unsigned long long>(a.X(3)), static_cast<unsigned long long>(d.pool),
                         static_cast<unsigned long long>(d.offset), d.width, d.height, d.format,
                         static_cast<unsigned long long>(texture->second.serial));
            } else {
                LOG_INFO("trace: register texture-pool=%llx id=%x texture=%llx view=%llx (no texture record)",
                         static_cast<unsigned long long>(a.X(0)), id, static_cast<unsigned long long>(a.X(2)),
                         static_cast<unsigned long long>(a.X(3)));
            }
        }
    });
    // ---- samplers
    auto sb = [&m](Call& a) -> SamplerDesc& { return m.sampler_builders[a.X(0)]; };
    On("nvnSamplerBuilderSetDefaults", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.sampler_builders[a.X(0)] = {};
    });
    On("nvnSamplerBuilderSetMinMagFilter", [&m, sb](Call& a) {
        std::lock_guard lock(m.lock);
        sb(a).min_filter = static_cast<u32>(a.X(1));
        sb(a).mag_filter = static_cast<u32>(a.X(2));
    });
    On("nvnSamplerBuilderSetWrapMode", [&m, sb](Call& a) {
        std::lock_guard lock(m.lock);
        sb(a).wrap = {static_cast<u32>(a.X(1)), static_cast<u32>(a.X(2)), static_cast<u32>(a.X(3))};
    });
    On("nvnSamplerBuilderSetLodBias", [&m, sb](Call& a) {
        std::lock_guard lock(m.lock);
        sb(a).lod_bias = a.F(0);
    });
    On("nvnSamplerBuilderSetLodClamp", [&m, sb](Call& a) {
        std::lock_guard lock(m.lock);
        sb(a).lod_min = a.F(0);
        sb(a).lod_max = a.F(1);
    });
    On("nvnSamplerBuilderSetMaxAnisotropy", [&m, sb](Call& a) {
        std::lock_guard lock(m.lock);
        sb(a).max_anisotropy = a.F(0);
    });
    On("nvnSamplerBuilderSetCompare", [&m, sb](Call& a) {
        std::lock_guard lock(m.lock);
        sb(a).compare_mode = static_cast<u32>(a.X(1));
        sb(a).compare_func = static_cast<u32>(a.X(2));
    });
    On("nvnSamplerBuilderSetReductionFilter", [&m, sb](Call& a) {
        std::lock_guard lock(m.lock);
        sb(a).reduction = static_cast<u32>(a.X(1));
    });
    On("nvnSamplerBuilderSetBorderColor", [&m, sb](Call& a) {
        std::lock_guard lock(m.lock);
        for (int i = 0; i < 4; ++i) sb(a).border[i] = R32(a.X(1) + 4 * i);
    });
    On("nvnSamplerInitialize", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.samplers[a.X(0)] = m.sampler_builders[a.X(1)];
        JournalSet(&Model::samplers, a.X(0), m.samplers[a.X(0)]);
    });
    On("nvnSamplerPoolRegisterSampler", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        auto& entries = m.sampler_pools[a.X(0)];
        const u32 id = static_cast<u32>(a.X(1));
        const auto prior = entries.find(id);
        const bool changed = prior == entries.end() || prior->second != a.X(2);
        entries[id] = a.X(2);
        if (changed) JournalSamplerRegistration(a.X(0), id, a.X(2));
    });
    // ---- programs
    On("nvnProgramInitialize", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.programs[a.X(0)] = Program{{}, ++m.serial};
        JournalSet(&Model::programs, a.X(0), m.programs[a.X(0)]);
    });
    auto set_shaders = [&m](Call& a, u64 stride) {
        std::lock_guard lock(m.lock);
        Program& p = m.programs[a.X(0)];
        p.stages.clear();
        p.serial = ++m.serial;
        for (u64 i = 0; i < a.X(1); ++i) {
            const GuestAddr e = a.X(2) + i * stride;
            p.stages.push_back(ShaderStage{R64(e), R64(e + 8)});
        }
        JournalSet(&Model::programs, a.X(0), p);
    };
    On("nvnProgramSetShaders", [set_shaders](Call& a) { set_shaders(a, 16); });
    On("nvnProgramSetShadersExt", [set_shaders](Call& a) { set_shaders(a, 24); });
    // ---- windows
    On("nvnWindowBuilderSetTextures", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        auto& w = m.window_builders[a.X(0)];
        w.textures.clear();
        for (u64 i = 0; i < a.X(1); ++i) w.textures.push_back(R64(a.X(2) + 8 * i));
    });
    On("nvnWindowInitialize", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.windows[a.X(0)] = m.window_builders[a.X(1)];
        JournalSet(&Model::windows, a.X(0), m.windows[a.X(0)]);
    });
    On("nvnWindowAcquireTexture", {}, [&m](Call& a) {
        if (!std::getenv("NXRT_TRACE_WINDOW")) return;
        const s32 index = g_memory.IsMapped(a.X(2), sizeof(s32)) ? g_memory.Read<s32>(a.X(2)) : -1;
        std::lock_guard lock(m.lock);
        if (m.frames >= 16 && (m.frames + 1) % 120 != 0) return;
        const auto window = m.windows.find(a.X(0));
        const GuestAddr texture = window != m.windows.end() && index >= 0 &&
                                          static_cast<size_t>(index) < window->second.textures.size()
                                      ? window->second.textures[static_cast<size_t>(index)]
                                      : 0;
        LOG_INFO("trace: acquire window=%llx sync=%llx result=%llx index=%d texture=%llx",
                 static_cast<unsigned long long>(a.X(0)), static_cast<unsigned long long>(a.X(1)),
                 static_cast<unsigned long long>(a.ret), index, static_cast<unsigned long long>(texture));
    });
}

void RegisterCommands() {
    auto& m = g_model;
    On("nvnCommandBufferBeginRecording", [&m](Call& a) {
        std::lock_guard lock(m.lock);
        m.recording[a.X(0)] = std::make_shared<CommandList>();
    });
    // The handle EndRecording returns is exactly what the game later passes to
    // CallCommands and SubmitCommands: it is the list's key.
    On("nvnCommandBufferEndRecording", {}, [&m](Call& a) {
        std::lock_guard lock(m.lock);
        auto rec = m.recording.find(a.X(0));
        auto list = rec != m.recording.end() ? rec->second : std::make_shared<CommandList>();
        if (rec != m.recording.end()) m.recording.erase(rec);
        if (std::getenv("NXRT_TRACE_WINDOW")) {
            // Does this list draw into a window texture?
            for (const Command& c : list->commands) {
                if (c.op != Op::SetRenderTargets) continue;
                for (size_t i = 0; i + 3 < c.words.size(); i += 4) {
                    const u64 tex = c.words[i] | (u64{c.words[i + 1]} << 32);
                    for (const auto& [w, win] : m.windows) {
                        for (GuestAddr wt : win.textures) {
                            if (wt == tex) {
                                static int shown = 0;
                                if (shown++ < 2) {
                                    std::string ops;
                                    for (const Command& k : list->commands) ops += " " + std::to_string(static_cast<int>(k.op));
                                    LOG_INFO("trace: ops:%s", ops.c_str());
                                    LOG_INFO("trace: list %llx (cmd buffer %llx, %zu commands) renders into window texture %llx",
                                             (unsigned long long)a.ret, (unsigned long long)a.X(0), list->commands.size(),
                                             (unsigned long long)tex);
                                }
                            }
                        }
                    }
                }
            }
        }
        ++g_ended_lists;
        for (const Command& c : list->commands) {
            if (c.op == Op::DrawArrays || c.op == Op::DrawElements) {
                ++g_ended_draws;
                g_ended_instanced += !c.words.empty();
            }
            g_ended_clears += c.op == Op::ClearColor || c.op == Op::ClearDepthStencil;
        }
        if (a.ret) m.finished[a.ret] = std::move(list);
    });
    // Diagnostic: decode the raw command memory of one command buffer (NXRT_DUMP_CMDMEM=<cmd hex>). The game's
    // inline writer (main+0x2b70f0, guard value 1) puts Maxwell method words straight into command memory, which no
    // API-level hook sees. Prints the method/count sequence of the first few recordings.
    static const u64 dump_cmd = std::getenv("NXRT_DUMP_CMDMEM") ? std::strtoull(std::getenv("NXRT_DUMP_CMDMEM"), nullptr, 16) : 0;
    if (dump_cmd) {
        static GuestAddr s_base = 0;
        On("nvnCommandBufferAddCommandMemory", [&m](Call& a) {
            if (a.X(0) != dump_cmd) return;
            std::lock_guard lock(m.lock);
            auto it = m.pools.find(a.X(1));
            s_base = it == m.pools.end() ? 0 : it->second.storage + a.X(2);
            static int once = 0;
            if (once++ < 2)
                LOG_INFO("cmdmem: AddCommandMemory cmd %llx pool %llx off %llx size %llx -> cpu %llx%s",
                         (unsigned long long)a.X(0), (unsigned long long)a.X(1), (unsigned long long)a.X(2),
                         (unsigned long long)a.X(3), (unsigned long long)s_base, it == m.pools.end() ? " (pool unknown)" : "");
        });
        On("nvnCommandBufferGetCommandMemoryUsed", {}, [](Call& a) {
            static int shown = 0;
            static std::set<u64> sizes;
            if (a.X(0) != dump_cmd || !s_base) return;
            const u64 used = a.ret; // bytes
            if (sizes.insert(used).second && sizes.size() <= 40)
                LOG_INFO("cmdmem: new used size %llu bytes", (unsigned long long)used);
            if (used <= 64 || used > 0x100000 || shown >= 3) return;
            ++shown;
            if (const char* dir = std::getenv("NXRT_DUMP_CMDMEM_DIR")) {
                char path[512];
                std::snprintf(path, sizeof path, "%s/cmdmem-%d.bin", dir, shown);
                if (FILE* f = std::fopen(path, "wb")) {
                    for (u64 w = 0; w < used / 4; ++w) {
                        const u32 v = R32(s_base + 4 * w);
                        std::fwrite(&v, 4, 1, f);
                    }
                    std::fclose(f);
                }
            }
            std::string out;
            u64 words = used / 4, i = 0;
            int n = 0, draws = 0;
            while (i < words && n < 400) {
                const u32 h = R32(s_base + 4 * i);
                const u32 type = h >> 29, count = (h >> 16) & 0x1fff, method = (h & 0x1fff) << 2;
                char tmp[48];
                if (type == 1 || type == 3 || type == 5) {
                    std::snprintf(tmp, sizeof tmp, " %x/%u", method, count);
                    out += tmp;
                    if (method == 0x1614 || method == 0x1618 || method == 0x1610 || method == 0x1c04 + 0x7fff) ++draws;
                    i += 1 + (type == 5 ? 0 : count);
                } else {
                    std::snprintf(tmp, sizeof tmp, " ?%08x", h);
                    out += tmp;
                    ++i;
                }
                ++n;
            }
            LOG_INFO("cmdmem: list %llx used %llu bytes, %llu words; methods(method/count):%s", (unsigned long long)a.ret,
                     (unsigned long long)used, (unsigned long long)words, out.c_str());
        });
    }
    auto state = [](const char* name, Op op, u32 words) {
        On(name, [op, words](Call& a) {
            Command c{op};
            for (u32 i = 0; i < words; ++i) c.words.push_back(R32(a.X(1) + 4 * i));
            Record(a.X(0), std::move(c));
        });
    };
    state("nvnCommandBufferBindBlendState", Op::BindBlendState, 2);
    state("nvnCommandBufferBindColorState", Op::BindColorState, 1);
    state("nvnCommandBufferBindChannelMaskState", Op::BindChannelMaskState, 1);
    state("nvnCommandBufferBindDepthStencilState", Op::BindDepthStencilState, 2);
    state("nvnCommandBufferBindPolygonState", Op::BindPolygonState, 1);
    state("nvnCommandBufferBindMultisampleState", Op::BindMultisampleState, 6);
    auto array = [](const char* name, Op op, u32 words_each) {
        On(name, [op, words_each](Call& a) {
            Command c{op};
            c.a[0] = a.X(1);
            for (u64 i = 0; i < a.X(1) * words_each; ++i) c.words.push_back(R32(a.X(2) + 4 * i));
            Record(a.X(0), std::move(c));
        });
    };
    array("nvnCommandBufferBindVertexAttribState", Op::BindVertexAttribState, 1);
    array("nvnCommandBufferBindVertexStreamState", Op::BindVertexStreamState, 2);
    auto ints = [](const char* name, Op op, size_t count) {
        On(name, [op, count](Call& a) {
            Command c{op};
            for (size_t i = 0; i < count; ++i) c.a[i] = a.X(1 + static_cast<int>(i));
            Record(a.X(0), std::move(c));
        });
    };
    ints("nvnCommandBufferBindProgram", Op::BindProgram, 2);
    ints("nvnCommandBufferBindVertexBuffer", Op::BindVertexBuffer, 3);
    ints("nvnCommandBufferBindUniformBuffer", Op::BindUniformBuffer, 4);
    ints("nvnCommandBufferBindStorageBuffer", Op::BindStorageBuffer, 4);
    ints("nvnCommandBufferBindTexture", Op::BindTexture, 3);
    // Array forms: (cmd, [stage,] first, count, const NVNbufferRange* {address, size}). The recorder only knew the
    // single-buffer calls, so a compute pass that binds its storage buffers through the array API (the Mii face
    // paint) ran with an empty descriptor table. Ranges are copied by value, one command per range.
    auto bind_ranges = [](const char* name, Op op, bool has_stage) {
        On(name, [op, has_stage](Call& a) {
            const u64 stage = has_stage ? a.X(1) : 0;
            const int base = has_stage ? 2 : 1;
            const u64 first = a.X(base), count = a.X(base + 1), address = a.X(base + 2);
            if (!count || count > 16 || first + count > 16 || !g_memory.IsMapped(address, count * 16)) return;
            for (u64 i = 0; i < count; ++i) {
                Command c{op};
                if (has_stage) c.a = {stage, first + i, R64(address + 16 * i), R64(address + 16 * i + 8), 0, 0};
                else c.a = {first + i, R64(address + 16 * i), R64(address + 16 * i + 8), 0, 0, 0};
                Record(a.X(0), std::move(c));
            }
        });
    };
    bind_ranges("nvnCommandBufferBindStorageBuffers", Op::BindStorageBuffer, true);
    bind_ranges("nvnCommandBufferBindUniformBuffers", Op::BindUniformBuffer, true);
    bind_ranges("nvnCommandBufferBindVertexBuffers", Op::BindVertexBuffer, false);
    On("nvnCommandBufferBindTextures", [](Call& a) {
        const u64 stage = a.X(1), first = a.X(2), count = a.X(3), handles = a.X(4);
        if (!ValidTextureBindingRange(stage, first, count) || !count ||
            !g_memory.IsMapped(handles, count * sizeof(u64))) return;
        std::lock_guard lock(g_model.lock);
        auto list = ListFor(a.X(0));
        for (u64 i = 0; i < count; ++i) {
            Command c{Op::BindTexture, {}, {}, {}};
            c.a = {stage, first + i, R64(handles + 8 * i), 0, 0, 0};
            list->commands.push_back(std::move(c));
        }
    });
    // Driver control record type 0x17 retains this pointer, then reads the array
    // while submitting. A recording-time snapshot would miss later guest writes.
    ints("nvnCommandBufferBindTexturesDeferred", Op::BindTexturesDeferred, 4);
    ints("nvnCommandBufferSetViewport", Op::SetViewport, 4);
    On("nvnCommandBufferSetViewports", [](Call& a) {
        // NVN viewport ranges are {x,y,width,height} floats, recorded by value.
        // Only viewport zero is consumed by the current single-viewport renderer.
        const u64 first = a.X(1), count = a.X(2), address = a.X(3);
        if (first != 0 || !count || count > 16 || !g_memory.IsMapped(address, sizeof(float) * 4)) return;
        Command c{Op::SetViewport};
        c.a[4] = 1; // floating range API; integer SetViewport keeps this flag zero
        g_memory.ReadBlock(address, c.f.data(), sizeof(float) * 4);
        if (std::getenv("NXRT_TRACE_VIEWPORT_RANGES")) {
            LOG_INFO("viewport-ranges cmd=%llx first=%llu count=%llu xywh=%g,%g,%g,%g legacy=%d",
                     static_cast<unsigned long long>(a.X(0)), static_cast<unsigned long long>(first),
                     static_cast<unsigned long long>(count), c.f[0], c.f[1], c.f[2], c.f[3],
                     std::getenv("NXRT_LEGACY_IGNORE_VIEWPORT_RANGES") != nullptr);
        }
        if (!std::getenv("NXRT_LEGACY_IGNORE_VIEWPORT_RANGES")) Record(a.X(0), std::move(c));
    });
    ints("nvnCommandBufferSetScissor", Op::SetScissor, 4);
    On("nvnCommandBufferSetScissors", [](Call& a) {
        // (cmd, first, count, const int* rects): rects are {x,y,width,height} ints. Only scissor zero is consumed by
        // the single-viewport renderer. Without this the scissor stays at whatever the integer API set last.
        const u64 first = a.X(1), count = a.X(2), address = a.X(3);
        if (first != 0 || !count || count > 16 || !g_memory.IsMapped(address, sizeof(u32) * 4)) return;
        Command c{Op::SetScissor};
        for (int i = 0; i < 4; ++i) c.a[i] = R32(address + 4 * i);
        if (std::getenv("NXRT_TRACE_SCISSOR_RANGES")) {
            LOG_INFO("scissor-ranges cmd=%llx count=%llu xywh=%llu,%llu,%llu,%llu", static_cast<unsigned long long>(a.X(0)),
                     static_cast<unsigned long long>(count), static_cast<unsigned long long>(c.a[0]),
                     static_cast<unsigned long long>(c.a[1]), static_cast<unsigned long long>(c.a[2]),
                     static_cast<unsigned long long>(c.a[3]));
        }
        if (!std::getenv("NXRT_LEGACY_IGNORE_SCISSOR_RANGES")) Record(a.X(0), std::move(c));
    });
    On("nvnCommandBufferClearTexture", [](Call& a) {
        // (cmd, const NVNtexture*, const NVNtextureView*, const NVNcopyRegion* region, const float* color, int mask)
        Command c{Op::ClearTexture};
        c.a[0] = a.X(1);
        c.a[1] = a.X(5) & 0xf;
        c.a[2] = a.X(3) ? 1 : 0;
        for (int i = 0; i < 4; ++i) c.f[i] = a.X(4) ? AsFloat(R32(a.X(4) + 4 * i)) : 0.f;
        for (u32 i = 0; i < 6; ++i) c.words.push_back(a.X(3) ? R32(a.X(3) + 4 * i) : 0);
        {
            std::lock_guard lock(g_model.lock);
            const TextureView view = ViewOf(g_model, a.X(2));
            c.words.push_back(view.base_level);
            c.words.push_back(view.min_layer);
        }
        if (std::getenv("NXRT_TRACE_CLEAR_TEXTURE")) {
            LOG_INFO("clear-texture cmd=%llx tex=%llx level=%u layer=%u mask=%llx region=%d xyz=%d,%d,%d whd=%u,%u,%u "
                     "color=%g,%g,%g,%g",
                     static_cast<unsigned long long>(a.X(0)), static_cast<unsigned long long>(c.a[0]), c.words[6],
                     c.words[7], static_cast<unsigned long long>(c.a[1]), static_cast<int>(c.a[2]),
                     static_cast<int>(c.words[0]), static_cast<int>(c.words[1]), static_cast<int>(c.words[2]), c.words[3],
                     c.words[4], c.words[5], c.f[0], c.f[1], c.f[2], c.f[3]);
        }
        if (!std::getenv("NXRT_LEGACY_IGNORE_CLEAR_TEXTURE")) Record(a.X(0), std::move(c));
    });
    ints("nvnCommandBufferSetTexturePool", Op::SetTexturePool, 1);
    ints("nvnCommandBufferSetSamplerPool", Op::SetSamplerPool, 1);
    ints("nvnCommandBufferDrawArraysInstanced", Op::DrawArrays, 5);
    ints("nvnCommandBufferDrawElementsBaseVertex", Op::DrawElements, 5);
    ints("nvnCommandBufferBarrier", Op::Barrier, 1);
    ints("nvnCommandBufferSetRenderEnable", Op::SetRenderEnable, 1);
    ints("nvnCommandBufferDispatchCompute", Op::DispatchCompute, 3);
    ints("nvnCommandBufferSetRenderEnableConditional", Op::RenderEnableConditional, 2);
    On("nvnCommandBufferDrawArrays", [](Call& a) {
        Command c{Op::DrawArrays};
        c.a = {a.X(1), a.X(2), a.X(3), 0, 1, 0};
        Record(a.X(0), std::move(c));
    });
    On("nvnCommandBufferDrawElements", [](Call& a) {
        Command c{Op::DrawElements};
        c.a = {a.X(1), a.X(2), a.X(3), a.X(4), 0, 0};
        Record(a.X(0), std::move(c));
    });
    On("nvnCommandBufferDrawElementsInstanced", [](Call& a) {
        // (cmd, mode, type, count, indexAddress, baseVertex, baseInstance, instanceCount)
        Command c{Op::DrawElements};
        for (int i = 0; i < 6; ++i) c.a[i] = a.X(1 + i);
        c.words = {static_cast<u32>(a.X(7))};
        Record(a.X(0), std::move(c));
    });
    On("nvnCommandBufferSetDepthRange", [](Call& a) {
        Command c{Op::SetDepthRange};
        c.f = {a.F(0), a.F(1), 0, 0};
        Record(a.X(0), std::move(c));
    });
    // (cmd, NVNbufferAddress buffer, size_t bufferSize, ptrdiff_t updateOffset, size_t updateSize, const void* data):
    // the driver turns it into CB_DATA words the inline decoder never sees, and the GPU applies it in order with
    // the draws around it. NXRT_NO_UPDATE_UNIFORM=1 drops it (A/B).
    static const bool update_uniform = std::getenv("NXRT_NO_UPDATE_UNIFORM") == nullptr;
    if (update_uniform) {
        On("nvnCommandBufferUpdateUniformBuffer", [](Call& a) {
            const u64 offset = a.X(3), size = a.X(4) & ~u64{3}, data = a.X(5);
            if (!a.X(1) || !data || !size || size > 0x10000 || offset + size > a.X(2)) return;
            Command c{Op::UpdateBuffer, {}, {}, {}};
            c.a[0] = a.X(1) + offset;
            for (u64 i = 0; i < size; i += 4) c.words.push_back(R32(data + i));
            Record(a.X(0), std::move(c));
        });
    }
    // (cmd, NVNcounterType counter, NVNbufferAddress buffer): the GPU writes {u64 counter, u64 timestamp} there when
    // it reaches the report. The game reads these (occlusion samples) to decide what to draw; nobody wrote them, so
    // they stayed zero. NXRT_NO_REPORT_COUNTER=1 drops the report (A/B).
    if (!std::getenv("NXRT_NO_REPORT_COUNTER")) {
        On("nvnCommandBufferReportCounter", [](Call& a) {
            Command c{Op::ReportCounter, {}, {}, {}};
            c.a[0] = a.X(1);
            c.a[1] = a.X(2);
            Record(a.X(0), std::move(c));
        });
    }
    // (cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags); a region is {x, y, z, width, height, depth}.
    // NXRT_NO_COPY_TEXTURE=1 drops it (A/B).
    if (!std::getenv("NXRT_NO_COPY_TEXTURE")) {
        On("nvnCommandBufferCopyTextureToTexture", [](Call& a) {
            Command c{Op::CopyTexture, {}, {}, {}};
            c.a[0] = a.X(1);
            c.a[1] = a.X(4);
            c.a[2] = a.X(7);
            for (int r : {3, 6}) {
                for (u32 i = 0; i < 6; ++i) c.words.push_back(a.X(r) ? R32(a.X(r) + 4 * i) : 0);
            }
            {
                std::lock_guard lock(g_model.lock);
                for (int v : {2, 5}) {
                    const TextureView view = ViewOf(g_model, a.X(v));
                    c.words.push_back(view.base_level);
                    c.words.push_back(view.min_layer);
                }
                for (int i : {0, 1}) {
                    if (auto size = g_texture_storage_sizes.find(c.a[i]); size != g_texture_storage_sizes.end())
                        c.a[3 + i] = size->second.bytes;
                    const auto layout = g_texture_layouts.find(c.a[i]);
                    c.words.push_back(layout != g_texture_layouts.end());
                    const u64 packed = layout == g_texture_layouts.end() ? 0 : layout->second;
                    c.words.push_back(static_cast<u32>(packed));
                    c.words.push_back(static_cast<u32>(packed >> 32));

                }
            }
            Record(a.X(0), std::move(c));
        });
    }
    On("nvnCommandBufferSetBlendColor", [](Call& a) {
        Command c{Op::SetBlendColor};
        for (int i = 0; i < 4; ++i) c.f[i] = AsFloat(R32(a.X(1) + 4 * i));
        Record(a.X(0), std::move(c));
    });
    On("nvnCommandBufferClearColor", [](Call& a) {
        Command c{Op::ClearColor};
        c.a[0] = a.X(1);
        c.a[1] = a.X(3);
        for (int i = 0; i < 4; ++i) c.f[i] = AsFloat(R32(a.X(2) + 4 * i));
        Record(a.X(0), std::move(c));
    });
    On("nvnCommandBufferClearDepthStencil", [](Call& a) {
        Command c{Op::ClearDepthStencil};
        c.f[0] = a.F(0);
        c.a = {a.X(1), a.X(2), a.X(3)};
        Record(a.X(0), std::move(c));
    });
    On("nvnCommandBufferSetRenderTargets", [](Call& a) {
        // (cmd, numColors, const NVNtexture** colors, const NVNtextureView** colorViews,
        //  const NVNtexture* depthStencil, const NVNtextureView* depthStencilView)
        Command c{Op::SetRenderTargets};
        c.a[0] = a.X(1);
        c.a[1] = a.X(4);
        c.a[2] = a.X(5);
        std::vector<u64> views;
        for (u64 i = 0; i < a.X(1); ++i) {
            const u64 tex = a.X(2) ? R64(a.X(2) + 8 * i) : 0;
            const u64 view = a.X(3) ? R64(a.X(3) + 8 * i) : 0;
            c.words.push_back(static_cast<u32>(tex));
            c.words.push_back(static_cast<u32>(tex >> 32));
            c.words.push_back(static_cast<u32>(view));
            c.words.push_back(static_cast<u32>(view >> 32));
            views.push_back(view);
        }
        {
            // Mip level each target renders to, after the 4 words per target: the view may be reused right away.
            std::lock_guard lock(g_model.lock);
            for (u64 view : views) c.words.push_back(ViewOf(g_model, view).base_level);
            c.a[3] = ViewOf(g_model, a.X(5)).base_level;
            // Then the array layer (cube face) of each target.
            for (u64 view : views) c.words.push_back(ViewOf(g_model, view).min_layer);
            c.a[4] = ViewOf(g_model, a.X(5)).min_layer;
            // Preserve format overrides before this reusable view is changed again.
            for (u64 view : views) c.words.push_back(ViewOf(g_model, view).format);
        }
        Record(a.X(0), std::move(c));
    });
    On("nvnCommandBufferCallCommands", [](Call& a) {
        if (std::getenv("NXRT_TRACE_WINDOW")) {
            static int shown = 0;
            if (shown++ < 12) LOG_INFO("trace: cmd %llx calls %llu lists, first %llx", (unsigned long long)a.X(0),
                                       (unsigned long long)a.X(1), (unsigned long long)R64(a.X(2)));
        }
        Command c{Op::CallCommands};
        c.a[0] = a.X(1);
        for (u64 i = 0; i < a.X(1); ++i) {
            const u64 h = R64(a.X(2) + 8 * i);
            c.words.push_back(static_cast<u32>(h));
            c.words.push_back(static_cast<u32>(h >> 32));
        }
        Record(a.X(0), std::move(c));
    });
    On("nvnQueueSubmitCommands", [&m](Call& a) {
        std::vector<SubmissionSnapshot> lists;
        std::optional<JournalSubmission> journal_submission;
        {
            std::lock_guard lock(m.lock);
            static u32 trace_submits = 0;
            for (u64 i = 0; i < a.X(1); ++i) {
                const u64 handle = R64(a.X(2) + 8 * i);
                const bool sample_submit = m.frames == 0 ? trace_submits < 2 : m.frames <= 8 || m.frames % 120 == 0;
                if (std::getenv("NXRT_TRACE_WINDOW") && sample_submit && trace_submits < 64) {
                    ++trace_submits;
                    std::string ops;
                    if (auto it = m.finished.find(handle); it != m.finished.end()) {
                        for (const Command& command : it->second->commands) {
                            ops += " " + std::to_string(static_cast<int>(command.op));
                            if (command.op == Op::DrawElements) ops += "(" + std::to_string(command.a[2]) + ")";
                        }
                    }
                    LOG_INFO("trace: submit frame=%llu queue=%llx list=%llx ops:%s",
                             static_cast<unsigned long long>(m.frames), static_cast<unsigned long long>(a.X(0)),
                             static_cast<unsigned long long>(handle), ops.substr(0, 320).c_str());
                }
                if (std::getenv("NXRT_TRACE_WINDOW") && m.frames >= 300 && m.frames < 302) {
                    auto it = m.finished.find(handle);
                    std::string ops;
                    if (it != m.finished.end()) {
                        for (const Command& k : it->second->commands) ops += " " + std::to_string(static_cast<int>(k.op));
                    }
                    LOG_INFO("trace: frame %llu submit %llx:%s", (unsigned long long)m.frames, (unsigned long long)handle,
                             ops.substr(0, 400).c_str());
                    if (it != m.finished.end()) {
                        for (const Command& k : it->second->commands) {
                            if (k.op != Op::CallCommands) continue;
                            for (size_t w = 0; w + 1 < k.words.size(); w += 2) {
                                const u64 h = k.words[w] | (u64{k.words[w + 1]} << 32);
                                auto cl = m.finished.find(h);
                                std::string cops;
                                if (cl != m.finished.end()) {
                                    for (const Command& q : cl->second->commands) {
                                        cops += " " + std::to_string(static_cast<int>(q.op));
                                        if (q.op == Op::DrawElements) cops += "(" + std::to_string(q.a[2]) + ")";
                                    }
                                }
                                LOG_INFO("trace:   called %llx:%s", (unsigned long long)h, cops.substr(0, 300).c_str());
                            }
                        }
                    }
                }
                Count(handle, 0);
                if (g_renderer) {
                    if (auto it = m.finished.find(handle); it != m.finished.end()) {
                        lists.push_back(SnapshotSubmission(
                            handle, *it->second,
                            [](u64 child_handle) -> const CommandList* {
                                const auto child = g_model.finished.find(child_handle);
                                return child == g_model.finished.end() ? nullptr : child->second.get();
                            },
                            [](GuestAddr address) -> std::optional<u64> {
                                if (!g_memory.IsMapped(address, sizeof(u64))) return std::nullopt;
                                return g_memory.Read<u64>(address);
                            }));
                    }
                }
            }
            const char* snapshot_mode = std::getenv("NXRT_EXECUTION_MODEL_SNAPSHOT");
            if (g_execution_journal && !lists.empty()) {
                if (!g_execution_journal->model) {
                    g_execution_journal->model = SnapshotExecutionModel(m, [](GuestAddr address) { return R64(address); });
                    LOG_INFO("nvn: synchronous execution model journal enabled (guest memory is not snapshotted)");
                }
                journal_submission.emplace();
                auto& captured = *journal_submission;
                captured.pool_generation = m.pool_generation;
                captured.epoch = m.epoch.load(std::memory_order_acquire);
                captured.serial = m.serial;
                captured.frames = m.frames;
                captured.texture_pool_registration = m.texture_pool_registration;
                captured.pools.reserve(m.pools.size());
                for (const auto& [address, pool] : m.pools) {
                    if (!pool.storage) continue;
                    const GpuAddr gpu = pool.gpu ? pool.gpu : R64(address + 0x18);
                    const GpuAddr gpu2 = pool.gpu ? pool.gpu2 : R64(address + 0x20);
                    captured.pools.push_back({address, gpu, gpu2});
                }
                // Reserve/capture can allocate; do not consume a sequence or
                // pending entries until all potentially throwing reads finish.
                captured.changes = std::move(g_execution_journal->pending);
                g_execution_journal->pending.clear();
                captured.sequence = g_execution_journal->next_capture++;
                for (auto& list : lists) list.execution_model = g_execution_journal->model;
            } else if (snapshot_mode && std::strcmp(snapshot_mode, "1") == 0 && !lists.empty()) {
                auto metadata = SnapshotExecutionModel(m, [](GuestAddr address) { return R64(address); });
                for (auto& list : lists) list.execution_model = metadata;
                static bool snapshot_logged = false;
                if (!snapshot_logged) {
                    LOG_INFO("nvn: submit metadata snapshot enabled (guest memory is not snapshotted)");
                    snapshot_logged = true;
                }
            }
            ++m.frame.submits;
        }
        if (journal_submission) {
            auto& captured = *journal_submission;
            std::unique_lock order_lock(g_journal_submit_mutex);
            g_journal_submit_cv.wait(order_lock, [&] { return captured.sequence == g_execution_journal->next_execute; });
            // Always retire the ticket, even if a renderer/consumer throws.
            struct Retire {
                ~Retire() { ++g_execution_journal->next_execute; g_journal_submit_cv.notify_all(); }
            } retire;
            auto& target = *g_execution_journal->model;
            if (g_execution_journal->poisoned) {
                throw std::runtime_error("execution model journal is unusable after a failed batch");
            }
            bool addresses_changed = false;
            for (const auto& addresses : captured.pools) {
                const auto old = target.pools.find(addresses.handle);
                if (old == target.pools.end() || old->second.gpu != addresses.gpu || old->second.gpu2 != addresses.gpu2) {
                    addresses_changed = true;
                }
            }
            try {
                for (auto& change : captured.changes) change(target);
            } catch (...) {
                // No later submission may consume a partially applied model.
                g_execution_journal->poisoned = true;
                throw;
            }
            if (addresses_changed) ++target.pool_address_revision;
            target.pool_generation = captured.pool_generation;
            target.epoch.store(captured.epoch, std::memory_order_relaxed);
            target.serial = captured.serial;
            target.frames = captured.frames;
            target.texture_pool_registration = captured.texture_pool_registration;
            for (const auto& addresses : captured.pools) {
                if (auto pool = target.pools.find(addresses.handle); pool != target.pools.end()) {
                    pool->second.gpu = addresses.gpu;
                    pool->second.gpu2 = addresses.gpu2;
                }
            }
            if (g_renderer) {
                for (const SubmissionSnapshot& list : lists) g_renderer->ExecuteSubmission(list);
            }
        } else if (g_renderer) {
            for (const SubmissionSnapshot& list : lists) g_renderer->ExecuteSubmission(list);
        }
    });
    On("nvnQueuePresentTexture", [&m](Call& a) {
        GuestAddr texture = 0;
        FrameStats stats;
        {
            std::lock_guard lock(m.lock);
            ++m.frames;
            stats = m.frame;
            m.frame = {};
            if (auto w = m.windows.find(a.X(1)); w != m.windows.end() && a.X(2) < w->second.textures.size()) {
                texture = w->second.textures[a.X(2)];
            }
            if (std::getenv("NXRT_TRACE_WINDOW") && (m.frames <= 16 || m.frames % 120 == 0)) {
                LOG_INFO("trace: present frame=%llu window=%llx index=%llu texture=%llx",
                         static_cast<unsigned long long>(m.frames), static_cast<unsigned long long>(a.X(1)),
                         static_cast<unsigned long long>(a.X(2)), static_cast<unsigned long long>(texture));
            }
            if (std::getenv("NXRT_NVN_FRAMES") && m.frames % 120 == 0) {
                LOG_INFO("nvn frame %llu: %llu submits, %llu commands, %llu draws, %llu clears, %llu unknown handles; "
                         "%zu textures, %zu programs, %zu finished lists",
                         (unsigned long long)m.frames, (unsigned long long)stats.submits,
                         (unsigned long long)stats.commands, (unsigned long long)stats.draws,
                         (unsigned long long)stats.clears, (unsigned long long)stats.unknown_handles,
                         m.textures.size(), m.programs.size(), m.finished.size());
                LOG_INFO("nvn cumulative: %llu lists ended with %llu draws (%llu instanced) and %llu clears; "
                         "submissions reached %llu draws and %llu clears",
                         (unsigned long long)g_ended_lists, (unsigned long long)g_ended_draws,
                         (unsigned long long)g_ended_instanced, (unsigned long long)g_ended_clears,
                         (unsigned long long)g_reached_draws, (unsigned long long)g_reached_clears);
                LOG_INFO("nvn inline: %llu words decoded, %llu draws, %llu vertex buffers, %llu uniform words, "
                         "%llu uniform binds, %llu regions skipped, %llu driver const buffer binds",
                         (unsigned long long)g_inline_words, (unsigned long long)g_inline_draws,
                         (unsigned long long)g_inline_vbs, (unsigned long long)g_inline_updates,
                         (unsigned long long)g_inline_binds, (unsigned long long)g_inline_skipped,
                         (unsigned long long)g_driver_binds);
            }
        }
        if (g_renderer && texture) g_renderer->Present(texture, m.frames);
    });
}

} // namespace

Model& GetModel() {
    return g_model;
}

void SetRenderer(Renderer* renderer) {
    g_renderer = renderer;
}

void InstallRecorder() {
    On("nvnTextureBuilderSetPackagedTextureLayout", [](Call& a) {
        std::lock_guard lock(g_model.lock);
        g_builder_storage_sizes.erase(a.X(0));
        if (a.X(1) && g_memory.IsMapped(a.X(1), 8)) g_builder_layouts[a.X(0)] = R64(a.X(1));
        else g_builder_layouts.erase(a.X(0));
    });
    Observe("nvnTextureBuilderGetStorageSize", {}, [](Call& a) {
        std::lock_guard lock(g_model.lock);
        g_builder_storage_sizes[a.X(0)] = {g_model.texture_builders[a.X(0)], a.ret};
    });
    On("nvnCommandBufferCopyTextureToBuffer", [](Call& a) {
        if (!a.X(3) || !g_memory.IsMapped(a.X(3), 24)) return;
        Command c{Op::CopyTextureToBuffer};
        c.a[0] = a.X(1);
        c.a[1] = a.X(4);
        c.a[2] = a.X(5);
        for (u32 i = 0; i < 6; ++i) c.words.push_back(R32(a.X(3) + 4 * i));
        {
            std::lock_guard lock(g_model.lock);
            const auto view = ViewOf(g_model, a.X(2));
            c.words.push_back(view.base_level);
            c.words.push_back(view.min_layer);
        }
        Record(a.X(0), std::move(c));
    });
    On("nvnMemoryPoolInvalidateMappedRange", [](Call& a) {
        if (g_renderer) g_renderer->InvalidateMappedRange(a.X(0), a.X(1), a.X(2));
    });
    const char* journal_mode = std::getenv("NXRT_EXECUTION_MODEL_JOURNAL");
    if (journal_mode && std::strcmp(journal_mode, "1") == 0) {
        g_execution_journal = std::make_unique<ExecutionModelJournal>();
    }
    if (!std::getenv("NXRT_NVN_NO_DECODE")) SetCommandBufferHooks(InlineBefore, InlineAfter);
    RegisterObjects();
    RegisterCommands();
}

} // namespace nxrt::nvn
