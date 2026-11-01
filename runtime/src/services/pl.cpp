// SPDX-License-Identifier: GPL-3.0-or-later
// pl:u (shared fonts). The fonts are suyu's open replacements for the system
// fonts (third_party/suyu/fonts, GPL-2.0+), never Nintendo's; they are laid out
// in shared memory exactly as the system's are, "BFTTF"-wrapped.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <mutex>

#include "runtime.h"
#include "services/common.h"

namespace nxrt::hle {

namespace {

constexpr u32 kExpectedResult = 0x7f9a0218;
constexpr u32 kExpectedMagic = 0x36f81a1e;
constexpr u64 kSharedFontSize = 0x1100000;

u32 Swap32(u32 v) {
    return __builtin_bswap32(v);
}

struct FontRegion {
    u32 offset, size;
};

struct PlState {
    std::shared_ptr<KSharedMemory> shm;
    std::vector<FontRegion> regions;
    std::once_flag loaded;

    void Load() {
        std::call_once(loaded, [this] {
            shm = std::make_shared<KSharedMemory>("font", kSharedFontSize);
            static const char* const files[] = {"font_standard.ttf",           "font_chinese_simplified.ttf",
                                                "font_extended_chinese_simplified.ttf",
                                                "font_chinese_traditional.ttf", "font_korean.ttf",
                                                "font_nintendo_extended.ttf",  "font_nintendo_extended.ttf"};
            size_t offset = 0;
            for (const char* name : files) {
                std::ifstream in(g_config.font_dir / name, std::ios::binary);
                std::vector<u8> ttf(std::istreambuf_iterator<char>(in), {});
                if (ttf.empty()) {
                    LOG_ERROR("pl:u: missing %s in %s", name, g_config.font_dir.c_str());
                    regions.push_back({0, 0});
                    continue;
                }
                ttf.resize((ttf.size() + 3) & ~size_t{3});
                const size_t words = ttf.size() / 4;
                // Same transformation as the system: the stored words are the
                // big-endian font data, the first two words a magic and the size.
                const u32 key = Swap32(kExpectedResult ^ kExpectedMagic);
                std::vector<u32> bfttf(words + 2);
                bfttf[0] = Swap32(kExpectedMagic);
                bfttf[1] = Swap32(static_cast<u32>(words * 4)) ^ key;
                for (size_t i = 0; i < words; ++i) {
                    u32 w;
                    std::memcpy(&w, ttf.data() + i * 4, 4);
                    bfttf[i + 2] = w ^ key;
                }
                for (u32& w : bfttf) w = Swap32(w);
                const u32 dkey = bfttf[0] ^ kExpectedResult;
                std::vector<u32> out(bfttf.size());
                for (size_t i = 0; i < bfttf.size(); ++i) out[i] = Swap32(bfttf[i] ^ dkey);
                out[1] = Swap32(out[1]) ^ dkey;
                if (offset + out.size() * 4 > kSharedFontSize) break;
                std::memcpy(shm->HostPointer() + offset, out.data(), out.size() * 4);
                regions.push_back({static_cast<u32>(offset + 8), static_cast<u32>(out.size() * 4 - 8)});
                offset += out.size() * 4;
            }
        });
    }
};

PlState& State() {
    static PlState s;
    return s;
}

class PlU final : public ServiceObject {
public:
    PlU() : ServiceObject("pl:u") {
        RegisterStub(0, "RequestLoad");
        Register(1, "GetLoadState", [](Ctx& ctx) { ctx.Push<u32>(1); }); // loaded
        Register(2, "GetSize", [](Ctx& ctx) {
            State().Load();
            const u32 type = ctx.Pop<u32>();
            ctx.Push<u32>(type < State().regions.size() ? State().regions[type].size : 0);
        });
        Register(3, "GetSharedMemoryAddressOffset", [](Ctx& ctx) {
            State().Load();
            const u32 type = ctx.Pop<u32>();
            ctx.Push<u32>(type < State().regions.size() ? State().regions[type].offset : 0);
        });
        Register(4, "GetSharedMemoryNativeHandle", [](Ctx& ctx) {
            State().Load();
            ctx.PushCopyObject(State().shm);
        });
        auto priority = [](Ctx& ctx) {
            State().Load();
            const auto& r = State().regions;
            const size_t n = std::min<size_t>({6, r.size(), ctx.OutBufferSize(0) / 4, ctx.OutBufferSize(1) / 4,
                                               ctx.OutBufferSize(2) / 4});
            std::vector<u32> codes(n), offsets(n), sizes(n);
            for (size_t i = 0; i < n; ++i) {
                codes[i] = static_cast<u32>(i);
                offsets[i] = r[i].offset;
                sizes[i] = r[i].size;
            }
            ctx.WriteBuffer(codes, 0);
            ctx.WriteBuffer(offsets, 1);
            ctx.WriteBuffer(sizes, 2);
            ctx.Push<u8>(1);
            ctx.Push<u32>(static_cast<u32>(n));
        };
        Register(5, "GetSharedFontInOrderOfPriority", priority);
        Register(6, "GetSharedFontInOrderOfPriorityForSystem", priority);
    }
};

} // namespace

void RegisterPlServices() {
    RegisterService("pl:u", [] { return std::make_shared<PlU>(); });
    RegisterService("pl:s", [] { return std::make_shared<PlU>(); });
}

} // namespace nxrt::hle
