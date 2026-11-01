// SPDX-License-Identifier: GPL-3.0-or-later
// Small services that only need fixed answers: logging, performance modes.
#include "services/common.h"

namespace nxrt::hle {

namespace {

class ILogger final : public ServiceObject {
public:
    ILogger() : ServiceObject("ILogger") {
        RegisterStub(0, "Log");
        RegisterStub(1, "SetDestination");
    }
};

class LM final : public ServiceObject {
public:
    LM() : ServiceObject("lm") {
        Register(0, "OpenLogger", [](Ctx& ctx) { ctx.PushInterface(std::make_shared<ILogger>()); });
    }
};

class ApmSession final : public ServiceObject {
public:
    ApmSession() : ServiceObject("ISession") {
        Register(0, "SetPerformanceConfiguration", [this](Ctx& ctx) {
            const u32 mode = ctx.Pop<u32>();
            config_[mode & 1] = ctx.Pop<u32>();
        });
        Register(1, "GetPerformanceConfiguration", [this](Ctx& ctx) {
            ctx.Push<u32>(config_[ctx.Pop<u32>() & 1]);
        });
        RegisterStub(2, "SetCpuOverclockEnabled");
    }

private:
    u32 config_[2] = {0x00010000, 0x00020003};
};

class APM final : public ServiceObject {
public:
    APM() : ServiceObject("apm") {
        Register(0, "OpenSession", [](Ctx& ctx) { ctx.PushInterface(std::make_shared<ApmSession>()); });
        Register(1, "GetPerformanceMode", [](Ctx& ctx) { ctx.Push<u32>(1); }); // docked
        Register(6, "IsCpuOverclockEnabled", [](Ctx& ctx) { ctx.Push<u8>(0); });
    }
};

class IPurchaseEventManager final : public ServiceObject {
public:
    IPurchaseEventManager() : ServiceObject("IPurchaseEventManager") {
        RegisterStub(0, "SetDefaultDeliveryTarget");
        RegisterStub(1, "SetDeliveryTarget");
        Register(2, "GetPurchasedEvent", [this](Ctx& ctx) { ctx.PushCopyObject(event_); });
        Register(3, "PopPurchasedProductInfo", [](Ctx& ctx) { ctx.result = MakeResult(166, 503); });
        Register(4, "PopPurchasedProductInfoWithUid", [](Ctx& ctx) { ctx.result = MakeResult(166, 503); });
    }

private:
    std::shared_ptr<KEvent> event_ = MakeEvent("aoc:purchased");
};

// No add-on content is installed.
class AocU final : public ServiceObject {
public:
    AocU() : ServiceObject("aoc:u") {
        Register(2, "CountAddOnContent", [](Ctx& ctx) { ctx.Push<u32>(0); });
        Register(3, "ListAddOnContent", [](Ctx& ctx) { ctx.Push<u32>(0); });
        Register(5, "GetAddOnContentBaseId", [](Ctx& ctx) { ctx.Push<u64>(0x010051F0207B3000ULL); });
        RegisterStub(7, "PrepareAddOnContent");
        Register(8, "GetAddOnContentListChangedEvent", [this](Ctx& ctx) { ctx.PushCopyObject(changed_); });
        Register(10, "GetAddOnContentListChangedEventWithProcessId",
                 [this](Ctx& ctx) { ctx.PushCopyObject(changed_); });
        RegisterStub(11, "NotifyMountAddOnContent");
        RegisterStub(12, "NotifyUnmountAddOnContent");
        RegisterStub(50, "CheckAddOnContentMountStatus");
        Register(100, "CreateEcPurchasedEventManager",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IPurchaseEventManager>()); });
        Register(101, "CreatePermanentEcPurchasedEventManager",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IPurchaseEventManager>()); });
    }

private:
    std::shared_ptr<KEvent> changed_ = MakeEvent("aoc:changed");
};

// Parental controls: nothing is restricted.
class IParentalControlService final : public ServiceObject {
public:
    IParentalControlService() : ServiceObject("IParentalControlService") {
        RegisterStub(1, "Initialize");
        RegisterStub(1001, "CheckFreeCommunicationPermission");
        RegisterStub(1002, "ConfirmLaunchApplicationPermission");
        RegisterStub(1003, "ConfirmResumeApplicationPermission");
        RegisterStub(1004, "ConfirmSnsPostPermission");
        RegisterStub(1005, "ConfirmSystemSettingsPermission");
        Register(1006, "IsRestrictionTemporaryUnlocked", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(1010, "IsRestrictedSystemSettingsEntered", [](Ctx& ctx) { ctx.Push<u8>(0); });
        RegisterStub(1013, "ConfirmStereoVisionPermission");
        RegisterStub(1017, "EndFreeCommunication");
        RegisterStub(1018, "IsFreeCommunicationAvailable");
        Register(1031, "IsRestrictionEnabled", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(1032, "GetSafetyLevel", [](Ctx& ctx) { ctx.Push<u32>(0); });
        Register(1039, "GetFreeCommunicationApplicationListCount", [](Ctx& ctx) { ctx.Push<s32>(0); });
        Register(1065, "IsStereoVisionPermitted", [](Ctx& ctx) { ctx.Push<u8>(1); });
        Register(1403, "IsPairingActive", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(1432, "GetSynchronizationEvent", [this](Ctx& ctx) { ctx.PushCopyObject(sync_); });
        RegisterStub(1451, "StartPlayTimer");
        RegisterStub(1452, "StopPlayTimer");
        Register(1453, "IsPlayTimerEnabled", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(1455, "IsRestrictedByPlayTimer", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(1457, "GetPlayTimerEventToRequestSuspension", [this](Ctx& ctx) { ctx.PushCopyObject(timer_); });
        Register(1458, "IsPlayTimerAlarmDisabled", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(1473, "GetUnlinkedEvent", [this](Ctx& ctx) { ctx.PushCopyObject(unlinked_); });
    }

private:
    std::shared_ptr<KEvent> sync_ = MakeEvent("pctl:sync");
    std::shared_ptr<KEvent> timer_ = MakeEvent("pctl:timer");
    std::shared_ptr<KEvent> unlinked_ = MakeEvent("pctl:unlinked");
};

class Pctl final : public ServiceObject {
public:
    explicit Pctl(const char* name) : ServiceObject(name) {
        Register(0, "CreateService",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IParentalControlService>()); });
        Register(1, "CreateServiceWithoutInitialize",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IParentalControlService>()); });
    }
};

class IContextRegistrar final : public ServiceObject {
public:
    IContextRegistrar() : ServiceObject("IContextRegistrar") {
        Register(0, "Complete", [](Ctx& ctx) { ctx.Push<u32>(0); });
    }
};

class Ectx final : public ServiceObject {
public:
    Ectx() : ServiceObject("ectx:aw") {
        Register(0, "CreateContextRegistrar",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IContextRegistrar>()); });
    }
};

// Profanity filter: nothing is filtered.
class Ngc final : public ServiceObject {
public:
    Ngc() : ServiceObject("ngc:u") {
        Register(0, "GetContentVersion", [](Ctx& ctx) { ctx.Push<u32>(1); });
        auto check = [](Ctx& ctx) { ctx.Push<u32>(0); };
        auto mask = [](Ctx& ctx) {
            const std::vector<u8> text = ctx.ReadBuffer();
            ctx.WriteBuffer(text.data(), text.size());
            ctx.Push<u32>(0);
        };
        Register(1, "Check", check);
        Register(2, "Mask", mask);
        RegisterStub(3, "Reload");
        Register(4, "Check2", check);
        Register(5, "Mask2", mask);
    }
};

// Play reports (telemetry): accepted and dropped.
class Prepo final : public ServiceObject {
public:
    explicit Prepo(const char* name) : ServiceObject(name) {
        for (u32 id : {10100u, 10101u, 10102u, 10103u, 10104u, 10105u, 10106u, 10107u, 10200u, 20100u, 20101u,
                       20102u, 20103u}) {
            RegisterStub(id, "SaveReport");
        }
        Register(10300, "GetTransmissionStatus", [](Ctx& ctx) { ctx.Push<u32>(0); });
        Register(10400, "GetSystemSessionId", [](Ctx& ctx) { ctx.Push<u64>(0x1234); });
    }
};

} // namespace

void RegisterMiscServices() {
    RegisterService("ngc:u", [] { return std::make_shared<Ngc>(); });
    for (const char* name : {"prepo:u", "prepo:a", "prepo:a2", "prepo:s"}) {
        RegisterService(name, [name] { return std::make_shared<Prepo>(name); });
    }
    RegisterService("aoc:u", [] { return std::make_shared<AocU>(); });
    RegisterService("ectx:aw", [] { return std::make_shared<Ectx>(); });
    for (const char* name : {"pctl", "pctl:s", "pctl:a", "pctl:r"}) {
        RegisterService(name, [name] { return std::make_shared<Pctl>(name); });
    }
    RegisterService("lm", [] { return std::make_shared<LM>(); });
    RegisterService("apm", [] { return std::make_shared<APM>(); });
}

} // namespace nxrt::hle
