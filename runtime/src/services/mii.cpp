// SPDX-License-Identifier: GPL-3.0-or-later
// mii:e / mii:u: the console's Mii database is empty; the six default Miis and
// random Miis come from nxrt's own Mii data model (services/mii_data).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "services/common.h"
#include "services/mii_data/mii_data.h"

namespace nxrt::hle {

namespace {

namespace mii = nxrt::mii;
constexpr u32 kDefaultMiiCount = nxrt::mii::kDefaultMiiCount;
constexpr u32 kResultInvalidArgument = MakeResult(126, 1);
constexpr u32 kResultNotUpdated = MakeResult(126, 3);
constexpr u32 kResultNotFound = MakeResult(126, 4);

// IPC threads share one generator for create ids and random Miis.
std::mutex g_rng_mutex;

// NXRT_MII_LOG=1: one stderr line per default-Mii request (which index and which entry point).
bool LogEnabled() {
    static const bool on = std::getenv("NXRT_MII_LOG") != nullptr;
    return on;
}

mii::CharInfo BuildDefault(u32 index) {
    if (LogEnabled()) {
        std::fprintf(stderr, "[mii] default index %u\n", index);
    }
    std::lock_guard lock(g_rng_mutex);
    return mii::BuildDefault(index, mii::SystemRng()).ToCharInfo();
}

u32 CountFor(u32 flag) {
    return (flag & 2) ? kDefaultMiiCount : 0; // the database itself is empty
}

class IDatabaseService final : public ServiceObject {
public:
    IDatabaseService() : ServiceObject("IDatabaseService") {
        Register(0, "IsUpdated", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(1, "IsFullDatabase", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(2, "GetCount", [](Ctx& ctx) { ctx.Push<u32>(CountFor(ctx.Pop<u32>())); });
        Register(3, "Get", [](Ctx& ctx) { // CharInfoElement[]
            const u32 flag = ctx.Pop<u32>();
            std::vector<u8> out;
            const u32 n = std::min<u32>(CountFor(flag), static_cast<u32>(ctx.OutBufferSize() / sizeof(mii::CharInfoElement)));
            for (u32 i = 0; i < n; ++i) {
                mii::CharInfoElement e{};
                e.char_info = BuildDefault(i);
                e.source = mii::kSourceDefault;
                const u8* p = reinterpret_cast<const u8*>(&e);
                out.insert(out.end(), p, p + sizeof(e));
            }
            ctx.WriteBuffer(out.data(), out.size());
            ctx.Push<u32>(n);
        });
        Register(4, "Get1", [](Ctx& ctx) { // CharInfo[]
            const u32 flag = ctx.Pop<u32>();
            std::vector<u8> out;
            const u32 n = std::min<u32>(CountFor(flag), static_cast<u32>(ctx.OutBufferSize() / sizeof(mii::CharInfo)));
            for (u32 i = 0; i < n; ++i) {
                const mii::CharInfo info = BuildDefault(i);
                const u8* p = reinterpret_cast<const u8*>(&info);
                out.insert(out.end(), p, p + sizeof(info));
            }
            ctx.WriteBuffer(out.data(), out.size());
            ctx.Push<u32>(n);
        });
        Register(5, "UpdateLatest", [](Ctx& ctx) { ctx.result = kResultNotUpdated; });
        Register(6, "BuildRandom", [](Ctx& ctx) {
            const u32 age = ctx.Pop<u32>(), gender = ctx.Pop<u32>(), race = ctx.Pop<u32>();
            if (age > 3 || gender > 2 || race > 3) {
                ctx.result = kResultInvalidArgument;
                return;
            }
            std::lock_guard lock(g_rng_mutex);
            ctx.Push(mii::BuildRandom(static_cast<mii::Age>(age), static_cast<mii::Gender>(gender),
                                      static_cast<mii::Race>(race), mii::SystemRng())
                         .ToCharInfo());
        });
        Register(7, "BuildDefault", [](Ctx& ctx) {
            const s32 index = ctx.Pop<s32>();
            if (index < 0 || index >= static_cast<s32>(kDefaultMiiCount)) {
                ctx.result = kResultInvalidArgument;
                return;
            }
            ctx.Push(BuildDefault(static_cast<u32>(index)));
        });
        Register(8, "Get2", [](Ctx& ctx) { ctx.Push<u32>(0); });
        Register(9, "Get3", [](Ctx& ctx) { ctx.Push<u32>(0); });
        Register(10, "UpdateLatest1", [](Ctx& ctx) { ctx.result = kResultNotUpdated; });
        Register(11, "FindIndex", [](Ctx& ctx) { ctx.result = kResultNotFound; });
        Register(20, "IsBrokenDatabaseWithClearFlag", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(21, "GetIndex", [](Ctx& ctx) { ctx.result = kResultNotFound; });
        RegisterStub(22, "SetInterfaceVersion");
    }
};

static_assert(sizeof(mii::CharInfo) == 0x58);

class MiiStatic final : public ServiceObject {
public:
    explicit MiiStatic(const char* name) : ServiceObject(name) {
        Register(0, "GetDatabaseService", [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IDatabaseService>()); });
    }
};

} // namespace

void RegisterMiiServices() {
    for (const char* name : {"mii:e", "mii:u"}) {
        RegisterService(name, [name] { return std::make_shared<MiiStatic>(name); });
    }
}

} // namespace nxrt::hle
