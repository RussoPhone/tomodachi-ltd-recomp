// SPDX-License-Identifier: GPL-3.0-or-later
// acc: one local user, always open and selected. No network account.
#include <cstring>

#include "services/common.h"

namespace nxrt::hle {

namespace {

struct ProfileBase {
    u64 uuid[2];
    u64 timestamp;
    char nickname[0x20];
};
static_assert(sizeof(ProfileBase) == 0x38);

ProfileBase MakeProfile() {
    ProfileBase p{};
    p.uuid[0] = kUserId[0];
    p.uuid[1] = kUserId[1];
    p.timestamp = 1700000000;
    const char* name = std::getenv("NXRT_USER_NAME");
    std::strncpy(p.nickname, name && *name ? name : "Player", sizeof(p.nickname) - 1);
    return p;
}

bool IsOurUser(Ctx& ctx) {
    const u64 lo = ctx.Pop<u64>(), hi = ctx.Pop<u64>();
    return lo == kUserId[0] && hi == kUserId[1];
}

class IProfile final : public ServiceObject {
public:
    IProfile() : ServiceObject("IProfile") {
        Register(0, "Get", [](Ctx& ctx) {
            const ProfileBase p = MakeProfile();
            u8 user_data[0x80] = {};
            ctx.WriteBuffer(user_data, sizeof(user_data));
            ctx.Push(p);
        });
        Register(1, "GetBase", [](Ctx& ctx) { ctx.Push(MakeProfile()); });
        Register(10, "GetImageSize", [](Ctx& ctx) { ctx.Push<u32>(0); });
        Register(11, "LoadImage", [](Ctx& ctx) { ctx.Push<u32>(0); });
    }
};

class IManagerForApplication final : public ServiceObject {
public:
    IManagerForApplication() : ServiceObject("IManagerForApplication") {
        Register(0, "CheckAvailability", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(1, "GetAccountId", [](Ctx& ctx) { ctx.Push<u64>(kUserId[0] ^ kUserId[1]); });
        Register(4, "LoadIdTokenCache", [](Ctx& ctx) { ctx.Push<u32>(0); });
        RegisterStub(160, "StoreOpenContext");
    }
};

class Acc final : public ServiceObject {
public:
    explicit Acc(const char* name) : ServiceObject(name) {
        Register(0, "GetUserCount", [](Ctx& ctx) { ctx.Push<u32>(1); });
        Register(1, "GetUserExistence", [](Ctx& ctx) { ctx.Push<u8>(IsOurUser(ctx)); });
        auto list = [](Ctx& ctx) {
            u64 ids[2] = {kUserId[0], kUserId[1]};
            std::vector<u8> out(ctx.OutBufferSize(), 0);
            std::memcpy(out.data(), ids, std::min(out.size(), sizeof(ids)));
            ctx.WriteBuffer(out.data(), out.size());
        };
        Register(2, "ListAllUsers", list);
        Register(3, "ListOpenUsers", list);
        Register(4, "GetLastOpenedUser", [](Ctx& ctx) {
            ctx.Push<u64>(kUserId[0]);
            ctx.Push<u64>(kUserId[1]);
        });
        Register(5, "GetProfile", [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IProfile>()); });
        Register(50, "IsUserRegistrationRequestPermitted", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(51, "TrySelectUserWithoutInteractionDeprecated", [](Ctx& ctx) {
            ctx.Push<u64>(kUserId[0]);
            ctx.Push<u64>(kUserId[1]);
        });
        Register(52, "TrySelectUserWithoutInteraction", [](Ctx& ctx) {
            ctx.Push<u64>(kUserId[0]);
            ctx.Push<u64>(kUserId[1]);
        });
        Register(60, "ListOpenContextStoredUsers", list);
        RegisterStub(100, "InitializeApplicationInfo");
        Register(101, "GetBaasAccountManagerForApplication",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IManagerForApplication>()); });
        RegisterStub(110, "StoreSaveDataThumbnail");
        Register(131, "ListOpenContextStoredUsers", list);
        RegisterStub(140, "InitializeApplicationInfoRestricted");
        Register(141, "ListQualifiedUsers", list);
        Register(150, "IsUserAccountSwitchLocked", [](Ctx& ctx) { ctx.Push<u8>(0); });
        RegisterStub(160, "InitializeApplicationInfoV2");
    }
};

} // namespace

void RegisterAccServices() {
    for (const char* name : {"acc:u0", "acc:u1", "acc:aa", "acc:su"}) {
        RegisterService(name, [name] { return std::make_shared<Acc>(name); });
    }
}

} // namespace nxrt::hle
