// SPDX-License-Identifier: GPL-3.0-or-later
#include "loader.h"

#include <cstring>
#include <fstream>

#include "memory.h"

namespace nxrt {

namespace {

std::vector<u8> ReadFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return std::vector<u8>(std::istreambuf_iterator<char>(in), {});
}

// LZ4 block format (no frame). Returns false on malformed input.
bool Lz4Decompress(const u8* src, size_t src_size, u8* dst, size_t dst_size) {
    size_t ip = 0, op = 0;
    while (ip < src_size) {
        const u8 token = src[ip++];
        size_t literals = token >> 4;
        if (literals == 15) {
            u8 b;
            do {
                if (ip >= src_size) return false;
                b = src[ip++];
                literals += b;
            } while (b == 255);
        }
        if (ip + literals > src_size || op + literals > dst_size) return false;
        std::memcpy(dst + op, src + ip, literals);
        ip += literals;
        op += literals;
        if (ip >= src_size) break; // the last sequence has no match
        if (ip + 2 > src_size) return false;
        const size_t offset = src[ip] | (src[ip + 1] << 8);
        ip += 2;
        if (offset == 0 || offset > op) return false;
        size_t match = token & 15;
        if (match == 15) {
            u8 b;
            do {
                if (ip >= src_size) return false;
                b = src[ip++];
                match += b;
            } while (b == 255);
        }
        match += 4;
        if (op + match > dst_size) return false;
        for (size_t i = 0; i < match; ++i, ++op) dst[op] = dst[op - offset]; // may overlap
    }
    return op == dst_size;
}

struct NsoSegment {
    u32 file_offset;
    u32 memory_offset;
    u32 size;
};

struct NsoHeader {
    u32 magic;
    u32 version;
    u32 reserved;
    u32 flags;
    NsoSegment text;
    u32 module_name_offset;
    NsoSegment rodata;
    u32 module_name_size;
    NsoSegment data;
    u32 bss_size;
    u8 module_id[0x20];
    u32 compressed_size[3];
    u8 reserved2[0x1C];
    u8 rest[0x100 - 0x88];
};
static_assert(sizeof(NsoHeader) == 0x100);

constexpr u64 PageAlign(u64 v) {
    return (v + kPageSize - 1) & ~(kPageSize - 1);
}

bool LoadNso(const std::filesystem::path& file, u64 base, LoadedModule& out) {
    const std::vector<u8> raw = ReadFile(file);
    if (raw.size() < sizeof(NsoHeader)) return false;
    NsoHeader h;
    std::memcpy(&h, raw.data(), sizeof(h));
    if (h.magic != 0x304F534E) { // "NSO0"
        LOG_ERROR("%s is not an NSO", file.string().c_str());
        return false;
    }
    const NsoSegment segs[3] = {h.text, h.rodata, h.data};
    const u64 image_end = static_cast<u64>(h.data.memory_offset) + h.data.size;
    const u64 image_size = PageAlign(image_end + h.bss_size);

    if (!g_memory.Map(base, image_size, MemState::CodeData, PermRW)) {
        LOG_ERROR("could not map %s at 0x%llx", file.string().c_str(), static_cast<unsigned long long>(base));
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        const NsoSegment& s = segs[i];
        const bool compressed = h.flags & (1u << i);
        const u32 stored = compressed ? h.compressed_size[i] : s.size;
        if (static_cast<u64>(s.file_offset) + stored > raw.size()) return false;
        u8* dst = g_memory.Ptr(base + s.memory_offset, s.size);
        if (compressed) {
            if (!Lz4Decompress(raw.data() + s.file_offset, stored, dst, s.size)) {
                LOG_ERROR("%s: segment %d failed to decompress", file.string().c_str(), i);
                return false;
            }
        } else {
            std::memcpy(dst, raw.data() + s.file_offset, s.size);
        }
    }
    // Same bookkeeping as the kernel: .text R-X, .rodata R--, .data/.bss RW-.
    const u64 ro_start = h.rodata.memory_offset;
    const u64 data_start = h.data.memory_offset;
    g_memory.SetState(base, PageAlign(ro_start), MemState::Code, PermRX);
    g_memory.SetState(base + ro_start, PageAlign(data_start) - ro_start, MemState::Code, PermR);
    g_memory.SetState(base + data_start, image_size - data_start, MemState::CodeData, PermRW);

    out.file = file.filename().string();
    out.base = base;
    out.size = image_size;
    out.text_size = PageAlign(h.text.size);
    return true;
}

} // namespace

u64 FindExport(const LoadedModule& m, const std::string& name) {
    // MOD0 header: text+4 holds its offset; it points at .dynamic.
    const u32 mod0_off = g_memory.Read<u32>(m.base + 4);
    const u64 mod0 = m.base + mod0_off;
    if (g_memory.Read<u32>(mod0) != 0x30444F4D) return 0; // "MOD0"
    const u64 dynamic = mod0 + static_cast<s32>(g_memory.Read<u32>(mod0 + 4));
    u64 symtab = 0, strtab = 0, strsz = 0, hash = 0, gnu_hash = 0;
    for (u64 d = dynamic;; d += 16) {
        const s64 tag = g_memory.Read<s64>(d);
        const u64 val = g_memory.Read<u64>(d + 8);
        if (tag == 0) break;
        switch (tag) {
        case 4: hash = val; break;        // DT_HASH
        case 5: strtab = val; break;      // DT_STRTAB
        case 6: symtab = val; break;      // DT_SYMTAB
        case 10: strsz = val; break;      // DT_STRSZ
        case 0x6ffffef5: gnu_hash = val; break;
        default: break;
        }
    }
    if (!symtab || !strtab) return 0;
    // Symbol count from DT_HASH's nchain; otherwise symtab runs up to strtab.
    u64 count = hash ? g_memory.Read<u32>(m.base + hash + 4) : (strtab - symtab) / 24;
    (void)gnu_hash;
    (void)strsz;
    for (u64 i = 0; i < count; ++i) {
        const u64 sym = m.base + symtab + i * 24;
        const u32 name_off = g_memory.Read<u32>(sym);
        const u16 shndx = g_memory.Read<u16>(sym + 6);
        const u64 value = g_memory.Read<u64>(sym + 8);
        if (!shndx || !value) continue; // undefined (an import)
        if (g_memory.ReadString(m.base + strtab + name_off, 256) == name) return m.base + value;
    }
    return 0;
}

bool LoadNpdm(const std::filesystem::path& file, ProgramMetadata& out) {
    const std::vector<u8> raw = ReadFile(file);
    if (raw.size() < 0x80 || std::memcmp(raw.data(), "META", 4) != 0) return false;
    out.is_64bit = raw[0xC] & 1;
    out.main_thread_priority = raw[0xE];
    out.main_thread_core = raw[0xF];
    std::memcpy(&out.main_thread_stack_size, raw.data() + 0x1C, 4);
    u32 aci_offset;
    std::memcpy(&aci_offset, raw.data() + 0x70, 4);
    if (aci_offset + 0x18 <= raw.size() && std::memcmp(raw.data() + aci_offset, "ACI0", 4) == 0) {
        std::memcpy(&out.program_id, raw.data() + aci_offset + 0x10, 8);
    }
    return true;
}

bool LoadExeFs(const std::filesystem::path& exefs, u64 base, std::vector<LoadedModule>& out) {
    static const char* const kOrder[] = {"rtld",    "main",    "subsdk0", "subsdk1", "subsdk2",
                                         "subsdk3", "subsdk4", "subsdk5", "subsdk6", "subsdk7",
                                         "subsdk8", "subsdk9", "sdk"};
    u64 next = base;
    for (const char* name : kOrder) {
        const auto file = exefs / name;
        if (!std::filesystem::exists(file)) continue;
        LoadedModule m;
        if (!LoadNso(file, next, m)) return false;
        LOG_INFO("loaded %-8s at 0x%llx (0x%llx bytes)", name, static_cast<unsigned long long>(m.base),
                 static_cast<unsigned long long>(m.size));
        next += m.size;
        out.push_back(m);
    }
    return !out.empty();
}

} // namespace nxrt
