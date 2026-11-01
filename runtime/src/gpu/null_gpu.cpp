// SPDX-License-Identifier: GPL-3.0-or-later
// A GPU that draws nothing but keeps every effect the CPU can observe:
// semaphore releases, DMA copies/fills and inline-to-memory writes. Synchronous
// submits read words as they execute; explicit snapshots instead own their words.
// Effects retain by-value operands and execute before reading the next method.
#include "gpu/null_gpu.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <mutex>
#include <span>

#include "memory.h"
#include "services/nvdrv.h"

namespace nxrt::gpu {

namespace {

using Effects = std::vector<std::function<void()>>;

constexpr u32 kClass3D = 0xB197;
constexpr u32 kClassCompute = 0xB1C0;
constexpr u32 kClassI2M = 0xA140;
constexpr u32 kClassDma = 0xB0B5;

u64 GpuTimestamp() {
    // GPU ticks are nanoseconds scaled by 384/625 on hardware; report ns * 384 / 625.
    const u64 ns = ReadCntpct() * 625 / 12;
    return ns * 384 / 625;
}

u8* Host(u64 gpu_va, u64 size) {
    u64 guest;
    if (!hle::GpuVaToGuest(gpu_va, &guest) || !g_memory.IsMapped(guest, size)) return nullptr;
    return g_memory.Ptr(guest, size);
}

void Write32(u64 gpu_va, u32 v) {
    if (u8* p = Host(gpu_va, 4)) __atomic_store_n(reinterpret_cast<u32*>(p), v, __ATOMIC_RELEASE);
}

void WriteLong(u64 gpu_va, u64 value) {
    if (u8* p = Host(gpu_va, 16)) {
        const u64 ts = GpuTimestamp();
        std::memcpy(p + 8, &ts, 8);
        __atomic_store_n(reinterpret_cast<u64*>(p), value, __ATOMIC_RELEASE);
    }
}

struct Upload { // inline-to-memory state, shared by 3D, compute and I2M classes
    u32 regs[12] = {};
    u64 dest = 0;
    u32 remaining = 0;
    bool active = false;

    void Launch(u32 arg) {
        const u32 line_length = regs[0], line_count = regs[1];
        dest = (static_cast<u64>(regs[2]) << 32) | regs[3];
        remaining = line_length * std::max<u32>(line_count, 1);
        // Block-linear destinations are textures: invisible to the CPU, skip.
        active = (arg & 1) != 0 && remaining;
    }
    void Data(u32 word, Effects& effects) {
        if (!active) return;
        const u32 n = std::min<u32>(4, remaining);
        const u64 address = dest;
        effects.emplace_back([address, word, n] {
            if (u8* p = Host(address, n)) std::memcpy(p, &word, n);
        });
        dest += n;
        remaining -= n;
        if (!remaining) active = false;
    }
};

struct Channel {
    u32 subchannel_class[8] = {};
    u32 puller[0x40] = {};
    Upload upload[8];
    u32 regs3d_report[4] = {};
    u32 dma[0x200] = {};
};

void PullerMethod(Channel& ch, u32 method, u32 arg, Effects& effects) {
    ch.puller[method] = arg;
    const u64 addr = (static_cast<u64>(ch.puller[4] & 0xFF) << 32) | ch.puller[5];
    switch (method) {
    case 0x7: { // SemaphoreOperation
        const u32 op = arg & 0xF;
        if (op == 2) {
            const u64 value = ch.puller[6];
            effects.emplace_back([addr, value] { WriteLong(addr, value); });
        }
        break;
    }
    case 0x1B: // SemaphoreRelease
        {
            const u32 value = ch.puller[6];
            effects.emplace_back([addr, value] { Write32(addr, value); });
        }
        break;
    default:
        break;
    }
}

void Report3D(Channel& ch, u32 query, Effects& effects) {
    const u64 addr = (static_cast<u64>(ch.regs3d_report[0] & 0xFF) << 32) | ch.regs3d_report[1];
    const u32 payload = ch.regs3d_report[2];
    const u32 operation = query & 3;
    const bool short_query = (query >> 28) & 1;
    const u32 report = (query >> 23) & 0x1F;
    if (operation == 1 || operation == 3) return; // acquire / trap
    const u64 value = (operation == 0 || report == 0) ? payload : 0; // counters read as zero
    if (short_query) {
        const u32 short_value = static_cast<u32>(value);
        effects.emplace_back([addr, short_value] { Write32(addr, short_value); });
    } else {
        effects.emplace_back([addr, value] { WriteLong(addr, value); });
    }
}

void ApplyDma(const u32* r, u32 arg) {
    const u64 src = (static_cast<u64>(r[0x100] & 0xFF) << 32) | r[0x101];
    const u64 dst = (static_cast<u64>(r[0x102] & 0xFF) << 32) | r[0x103];
    const u32 pitch_in = r[0x104], pitch_out = r[0x105];
    const u32 line_length = r[0x106], line_count = r[0x107];
    const bool src_pitch = (arg >> 7) & 1, dst_pitch = (arg >> 8) & 1;
    const bool multi_line = (arg >> 9) & 1, remap = (arg >> 10) & 1;
    const u32 lines = multi_line ? line_count : 1;
    if ((arg & 3) && dst_pitch) {
        if (remap) {
            // Fill: the remap unit writes the constant (const_a) into each component.
            const u32 const_a = r[0x1C0];
            const u32 comp_size = ((r[0x1C2] >> 16) & 3) + 1;
            const u32 num_comp = ((r[0x1C2] >> 24) & 3) + 1; // destination components
            const u64 bytes = static_cast<u64>(line_length) * comp_size * num_comp;
            for (u32 y = 0; y < lines; ++y) {
                if (u8* p = Host(dst + static_cast<u64>(y) * pitch_out, bytes)) {
                    for (u64 i = 0; i + 4 <= bytes; i += 4) std::memcpy(p + i, &const_a, 4);
                }
            }
        } else if (src_pitch) {
            for (u32 y = 0; y < lines; ++y) {
                u8* s = Host(src + static_cast<u64>(y) * pitch_in, line_length);
                u8* d = Host(dst + static_cast<u64>(y) * pitch_out, line_length);
                if (s && d) std::memmove(d, s, line_length);
            }
        }
    }
    const u32 sem_type = (arg >> 3) & 3;
    if (sem_type) {
        const u64 addr = (static_cast<u64>(r[0x90] & 0xFF) << 32) | r[0x91];
        if (sem_type == 1) Write32(addr, r[0x92]);
        else WriteLong(addr, r[0x92]);
    }
}

void DmaLaunch(Channel& ch, u32 arg, Effects& effects) {
    const std::array<u32, 0x200> regs = [&] {
        std::array<u32, 0x200> copy{};
        std::copy(ch.dma, ch.dma + 0x200, copy.begin());
        return copy;
    }();
    effects.emplace_back([regs, arg] { ApplyDma(regs.data(), arg); });
}

void EngineMethod(Channel& ch, u32 subchannel, u32 method, u32 arg, Effects& effects) {
    const u32 cls = ch.subchannel_class[subchannel];
    Upload& up = ch.upload[subchannel];
    if ((cls == kClass3D || cls == kClassCompute || cls == kClassI2M) && method >= 0x60 && method <= 0x6D) {
        if (method < 0x6C) up.regs[method - 0x60] = arg;
        else if (method == 0x6C) up.Launch(arg);
        else up.Data(arg, effects);
        return;
    }
    if (cls == kClass3D && method >= 0x6C0 && method <= 0x6C3) {
        ch.regs3d_report[method - 0x6C0] = arg;
        if (method == 0x6C3) Report3D(ch, arg, effects);
        return;
    }
    if (cls == kClassDma && method < 0x200) {
        ch.dma[method] = arg;
        if (method == 0xC0) DmaLaunch(ch, arg, effects);
        return;
    }
}

void CallMethod(Channel& ch, u32 subchannel, u32 method, u32 arg, Effects& effects) {
    if (method == 0) {
        ch.subchannel_class[subchannel & 7] = arg & 0xFFFF;
        return;
    }
    if (method < 0x40) {
        PullerMethod(ch, method, arg, effects);
        return;
    }
    EngineMethod(ch, subchannel & 7, method, arg, effects);
}

template <typename WordReader>
void ProcessCommandList(Channel& ch, size_t word_count, WordReader read_word) {
    Effects effects;
    const auto execute_method = [&](u32 subchannel, u32 method, u32 arg) {
        CallMethod(ch, subchannel, method, arg, effects);
        // A method's effects may rewrite an argument or header we have not read
        // yet. Apply them before the next word, as the original inline path did.
        for (const auto& effect : effects) effect();
        effects.clear();
    };
    u32 i = 0;
    while (i < word_count) {
        const u32 header = read_word(i++);
        const u32 method = header & 0x1FFF;
        const u32 subchannel = (header >> 13) & 7;
        const u32 count = (header >> 16) & 0x1FFF;
        const u32 mode = header >> 29;
        switch (mode) {
        case 1:   // increasing
        case 0: { // increasing (old)
            for (u32 k = 0; k < count && i < word_count; ++k, ++i) {
                execute_method(subchannel, method + k, read_word(i));
            }
            break;
        }
        case 3:   // non-increasing
        case 2: { // non-increasing (old)
            for (u32 k = 0; k < count && i < word_count; ++k, ++i) {
                execute_method(subchannel, method, read_word(i));
            }
            break;
        }
        case 4: // inline: the argument is the count field
            execute_method(subchannel, method, count);
            break;
        case 5: { // increase once
            for (u32 k = 0; k < count && i < word_count; ++k, ++i) {
                execute_method(subchannel, method + (k ? 1 : 0), read_word(i));
            }
            break;
        }
        default:
            break;
        }
    }
}

void ExecuteCommandList(Channel& ch, std::span<const u32> words) {
    ProcessCommandList(ch, words.size(), [words](u32 i) { return words[i]; });
}

std::mutex g_mutex;
Channel g_channels[64];

} // namespace

GpfifoSnapshot CaptureGpfifo(u32 channel, const u64* entries, size_t count) {
    GpfifoSnapshot snapshot;
    snapshot.channel = channel;
    snapshot.command_lists.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const u64 e = entries[i];
        const u64 va = e & ((u64{1} << 40) - 1);
        const u32 words = static_cast<u32>((e >> 42) & 0x1FFFFF);
        if (!va || !words) continue;
        const u64 bytes = static_cast<u64>(words) * sizeof(u32);
        const u8* base = Host(va, bytes);
        if (!base) continue;
        std::vector<u32> copied(words);
        std::memcpy(copied.data(), base, static_cast<size_t>(bytes));
        snapshot.command_lists.push_back(OwnCommandWords(copied));
    }
    return snapshot;
}

void ExecuteGpfifo(const GpfifoSnapshot& snapshot) {
    std::lock_guard lock(g_mutex);
    Channel& ch = g_channels[snapshot.channel & 63];
    for (const auto& command_list : snapshot.command_lists) ExecuteCommandList(ch, command_list);
}

void SubmitGpfifo(u32 channel, const u64* entries, size_t count) {
    std::lock_guard lock(g_mutex);
    Channel& ch = g_channels[channel & 63];
    for (size_t i = 0; i < count; ++i) {
        const u64 e = entries[i];
        const u64 va = e & ((u64{1} << 40) - 1);
        const u32 words = static_cast<u32>((e >> 42) & 0x1FFFFF);
        if (!va || !words) continue;
        const u8* base = Host(va, static_cast<u64>(words) * sizeof(u32));
        if (!base) continue;
        // Read each word only after prior effects, including writes to this list.
        ProcessCommandList(ch, words, [base](u32 word_index) {
            u32 word;
            std::memcpy(&word, base + static_cast<size_t>(word_index) * sizeof(u32), sizeof(word));
            return word;
        });
    }
}

} // namespace nxrt::gpu
