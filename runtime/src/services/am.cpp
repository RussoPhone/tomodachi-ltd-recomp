// SPDX-License-Identifier: GPL-3.0-or-later
// am (applet manager) as an application sees it: appletOE and the interfaces of
// IApplicationProxy. One application, always in focus, docked.
#include <array>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#include "memory.h"
#include "runtime.h"
#include "services/common.h"

namespace nxrt::hle {

std::shared_ptr<ServiceObject> MakeLibraryAppletAccessor(u32 applet_id, u32 mode);

namespace {

constexpr u32 kResultNoDataInChannel = MakeResult(128, 2);
constexpr u32 kResultNoMessages = MakeResult(128, 3);
constexpr u64 kAppletResourceUserId = 0x51;

enum AppletMessage : u32 {
    kFocusStateChanged = 15,
    kOperationModeChanged = 30,
    kPerformanceModeChanged = 31,
};

// Messages the system sends the application, and the event that announces them.
struct AppletState {
    std::mutex mutex;
    std::deque<u32> messages{kFocusStateChanged};
    std::shared_ptr<KEvent> message_event = MakeEvent("am:message", true);
    std::shared_ptr<KEvent> launchable_event = MakeEvent("am:launchable", true);
    std::shared_ptr<KEvent> gpu_error_event = MakeEvent("am:gpu-error");
    std::shared_ptr<KEvent> suspended_tick_event = MakeEvent("am:suspended-tick");
    std::shared_ptr<KEvent> friend_invitation_event = MakeEvent("am:friend-invitation");
    std::shared_ptr<KEvent> notification_event = MakeEvent("am:notification");
    std::shared_ptr<KEvent> health_warning_event = MakeEvent("am:health-warning");
    std::shared_ptr<KEvent> resolution_event = MakeEvent("am:resolution");
    std::shared_ptr<KEvent> operation_mode_event = MakeEvent("am:operation-mode");
    bool preselected_user_popped = false;
    u64 next_layer_id = 1;
};

AppletState& State() {
    static AppletState state;
    return state;
}

// ---------------------------------------------------------------- storage

class IStorageAccessor final : public ServiceObject {
public:
    explicit IStorageAccessor(std::shared_ptr<IStorage> storage)
        : ServiceObject("IStorageAccessor"), storage_(std::move(storage)) {
        Register(0, "GetSize", [this](Ctx& ctx) { ctx.Push<s64>(storage_->data.size()); });
        Register(10, "Write", [this](Ctx& ctx) {
            const s64 offset = ctx.Pop<s64>();
            const std::vector<u8> in = ctx.ReadBuffer();
            if (offset < 0 || offset + in.size() > storage_->data.size()) {
                ctx.result = MakeResult(128, 503); // out of bounds
                return;
            }
            std::memcpy(storage_->data.data() + offset, in.data(), in.size());
        });
        Register(11, "Read", [this](Ctx& ctx) {
            const s64 offset = ctx.Pop<s64>();
            const size_t size = ctx.OutBufferSize();
            if (offset < 0 || offset + size > storage_->data.size()) {
                ctx.result = MakeResult(128, 503);
                return;
            }
            ctx.WriteBuffer(storage_->data.data() + offset, size);
        });
    }

private:
    std::shared_ptr<IStorage> storage_;
};

} // namespace

IStorage::IStorage(std::vector<u8> d) : ServiceObject("IStorage"), data(std::move(d)) {
    Register(0, "Open", [this](Ctx& ctx) {
        ctx.PushInterface(std::make_shared<IStorageAccessor>(
            std::static_pointer_cast<IStorage>(shared_from_this())));
    });
}

namespace {

// ---------------------------------------------------------------- application proxy

class ICommonStateGetter final : public ServiceObject {
public:
    ICommonStateGetter() : ServiceObject("ICommonStateGetter") {
        Register(0, "GetEventHandle", [](Ctx& ctx) { ctx.PushCopyObject(State().message_event); });
        Register(1, "ReceiveMessage", [](Ctx& ctx) {
            AppletState& s = State();
            std::lock_guard lock(s.mutex);
            if (s.messages.empty()) {
                s.message_event->Clear();
                ctx.result = kResultNoMessages;
                return;
            }
            ctx.Push<u32>(s.messages.front());
            s.messages.pop_front();
            if (s.messages.empty()) s.message_event->Clear();
        });
        Register(5, "GetOperationMode", [](Ctx& ctx) { ctx.Push<u8>(1); });   // console (docked)
        Register(6, "GetPerformanceMode", [](Ctx& ctx) { ctx.Push<u32>(1); }); // boost (docked)
        Register(8, "GetBootMode", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(9, "GetCurrentFocusState", [](Ctx& ctx) { ctx.Push<u8>(1); }); // in focus
        Register(50, "IsVrModeEnabled", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(60, "GetDefaultDisplayResolution", [](Ctx& ctx) {
            ctx.Push<u32>(1920);
            ctx.Push<u32>(1080);
        });
        Register(61, "GetDefaultDisplayResolutionChangeEvent",
                 [](Ctx& ctx) { ctx.PushCopyObject(State().resolution_event); });
        RegisterStub(66, "SetCpuBoostMode");
        Register(300, "GetSettingsPlatformRegion", [](Ctx& ctx) { ctx.Push<u8>(1); });
        RegisterStub(900, "SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled");
    }
};

class ISelfController final : public ServiceObject {
public:
    ISelfController() : ServiceObject("ISelfController") {
        Register(0, "Exit", [](Ctx&) {
            LOG_INFO("the game asked to exit");
            std::fflush(nullptr);
            std::exit(0);
        });
        RegisterStub(1, "LockExit");
        RegisterStub(2, "UnlockExit");
        RegisterStub(3, "EnterFatalSection");
        RegisterStub(4, "LeaveFatalSection");
        Register(9, "GetLibraryAppletLaunchableEvent",
                 [](Ctx& ctx) { ctx.PushCopyObject(State().launchable_event); });
        RegisterStub(10, "SetScreenShotPermission");
        RegisterStub(11, "SetOperationModeChangedNotification");
        RegisterStub(12, "SetPerformanceModeChangedNotification");
        RegisterStub(13, "SetFocusHandlingMode");
        RegisterStub(14, "SetRestartMessageEnabled");
        RegisterStub(15, "SetScreenShotAppletIdentityInfo");
        RegisterStub(16, "SetOutOfFocusSuspendingEnabled");
        RegisterStub(19, "SetAlbumImageOrientation");
        Register(40, "CreateManagedDisplayLayer", [](Ctx& ctx) { ctx.Push<u64>(State().next_layer_id++); });
        Register(41, "IsSystemBufferSharingEnabled", [](Ctx& ctx) { ctx.result = MakeResult(128, 1); });
        Register(44, "CreateManagedDisplaySeparableLayer", [](Ctx& ctx) {
            ctx.Push<u64>(State().next_layer_id++);
            ctx.Push<u64>(State().next_layer_id++);
        });
        RegisterStub(50, "SetHandlesRequestToDisplay");
        RegisterStub(51, "ApproveToDisplay");
        RegisterStub(61, "SetMediaPlaybackState");
        RegisterStub(62, "SetIdleTimeDetectionExtension");
        Register(63, "GetIdleTimeDetectionExtension", [](Ctx& ctx) { ctx.Push<u32>(0); });
        RegisterStub(65, "ReportUserIsActive");
        RegisterStub(68, "SetAutoSleepDisabled");
        Register(69, "IsAutoSleepDisabled", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(90, "GetAccumulatedSuspendedTickValue", [](Ctx& ctx) { ctx.Push<u64>(0); });
        Register(91, "GetAccumulatedSuspendedTickChangedEvent",
                 [](Ctx& ctx) { ctx.PushCopyObject(State().suspended_tick_event); });
        RegisterStub(100, "SetAlbumImageTakenNotificationEnabled");
        RegisterStub(130, "SetRecordVolumeMuted");
    }
};

class IWindowController final : public ServiceObject {
public:
    IWindowController() : ServiceObject("IWindowController") {
        Register(1, "GetAppletResourceUserId", [](Ctx& ctx) { ctx.Push<u64>(kAppletResourceUserId); });
        Register(2, "GetAppletResourceUserIdOfCallerApplet", [](Ctx& ctx) { ctx.Push<u64>(0); });
        RegisterStub(10, "AcquireForegroundRights");
        RegisterStub(11, "ReleaseForegroundRights");
        RegisterStub(12, "RejectToChangeIntoBackground");
        RegisterStub(20, "SetAppletWindowVisibility");
        RegisterStub(21, "SetAppletGpuTimeSlice");
    }
};

class IAudioController final : public ServiceObject {
public:
    IAudioController() : ServiceObject("IAudioController") {
        RegisterStub(0, "SetExpectedMasterVolume");
        Register(1, "GetMainAppletExpectedMasterVolume", [](Ctx& ctx) { ctx.Push<float>(1.0f); });
        Register(2, "GetLibraryAppletExpectedMasterVolume", [](Ctx& ctx) { ctx.Push<float>(1.0f); });
        RegisterStub(3, "ChangeMainAppletMasterVolume");
        RegisterStub(4, "SetTransparentVolumeRate");
    }
};

class IDisplayController final : public ServiceObject {
public:
    IDisplayController() : ServiceObject("IDisplayController") {}
};

class IDebugFunctions final : public ServiceObject {
public:
    IDebugFunctions() : ServiceObject("IDebugFunctions") {}
};

class IProcessWindingController final : public ServiceObject {
public:
    IProcessWindingController() : ServiceObject("IProcessWindingController") {
        Register(0, "GetLaunchReason", [](Ctx& ctx) { ctx.Push<u32>(0); });
    }
};

std::string ReadDisplayVersion() {
    // control.nacp: display version is a 16-byte string at 0x3060.
    std::ifstream in(g_config.game_dir / "control" / "control.nacp", std::ios::binary);
    char version[0x10] = {};
    if (in) {
        in.seekg(0x3060);
        in.read(version, sizeof(version));
    }
    version[0xF] = 0;
    return version[0] ? version : "1.0.0";
}

class IApplicationFunctions final : public ServiceObject {
public:
    IApplicationFunctions() : ServiceObject("IApplicationFunctions") {
        Register(1, "PopLaunchParameter", [](Ctx& ctx) {
            const u32 kind = ctx.Pop<u32>();
            AppletState& s = State();
            std::lock_guard lock(s.mutex);
            if (kind != 2 || s.preselected_user_popped) { // 2: preselected user
                ctx.result = kResultNoDataInChannel;
                return;
            }
            s.preselected_user_popped = true;
            std::vector<u8> data(0x88);
            const u32 magic = 0xC79497CA, selected = 1;
            std::memcpy(data.data(), &magic, 4);
            std::memcpy(data.data() + 4, &selected, 4);
            std::memcpy(data.data() + 8, kUserId.data(), 16);
            ctx.PushInterface(std::make_shared<IStorage>(std::move(data)));
        });
        Register(20, "EnsureSaveData", [](Ctx& ctx) { ctx.Push<u64>(0); });
        Register(21, "GetDesiredLanguage", [](Ctx& ctx) {
            // NXRT_LANGUAGE=<Switch language code> (default en-US): ja, en-US, fr, de, it, es, zh-CN, ko, nl, pt, ru,
            // zh-TW, en-GB, fr-CA, es-419, zh-Hans, zh-Hant, pt-BR.
            static const std::string language = [] {
                static constexpr std::array<std::string_view, 18> kCodes = {
                    "ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru", "zh-TW", "en-GB",
                    "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
                const char* configured = std::getenv("NXRT_LANGUAGE");
                if (configured && *configured) {
                    for (std::string_view known : kCodes)
                        if (known == configured) return std::string(known);
                    LOG_WARN("am: ignoring unknown NXRT_LANGUAGE=%s", configured);
                }
                return std::string("en-US");
            }();
            static bool logged = false;
            if (!logged) {
                logged = true;
                LOG_INFO("am: GetDesiredLanguage -> %s", language.c_str());
            }
            u64 code = 0;
            std::memcpy(&code, language.data(), language.size());
            ctx.Push<u64>(code);
        });
        RegisterStub(22, "SetTerminateResult");
        Register(23, "GetDisplayVersion", [](Ctx& ctx) {
            char version[0x10] = {};
            const std::string v = ReadDisplayVersion();
            std::memcpy(version, v.data(), std::min<size_t>(v.size(), 0xF));
            ctx.PushRaw(version, sizeof(version));
        });
        Register(25, "ExtendSaveData", [](Ctx& ctx) { ctx.Push<u64>(0); });
        Register(26, "GetSaveDataSize", [](Ctx& ctx) {
            ctx.Push<u64>(0x10000000);
            ctx.Push<u64>(0x10000000);
        });
        Register(28, "GetSaveDataSizeMax", [](Ctx& ctx) {
            ctx.Push<u64>(0xFFFFFFF);
            ctx.Push<u64>(0xFFFFFFF);
        });
        RegisterStub(30, "BeginBlockingHomeButtonShortAndLongPressed");
        RegisterStub(31, "EndBlockingHomeButtonShortAndLongPressed");
        RegisterStub(32, "BeginBlockingHomeButton");
        RegisterStub(33, "EndBlockingHomeButton");
        Register(40, "NotifyRunning", [](Ctx& ctx) { ctx.Push<u8>(1); });
        Register(50, "GetPseudoDeviceId", [](Ctx& ctx) {
            ctx.Push<u64>(0);
            ctx.Push<u64>(0);
        });
        RegisterStub(60, "SetMediaPlaybackStateForApplication");
        Register(65, "IsGamePlayRecordingSupported", [](Ctx& ctx) { ctx.Push<u8>(0); });
        RegisterStub(66, "InitializeGamePlayRecording");
        RegisterStub(67, "SetGamePlayRecordingState");
        RegisterStub(90, "EnableApplicationCrashReport");
        RegisterStub(100, "InitializeApplicationCopyrightFrameBuffer");
        RegisterStub(101, "SetApplicationCopyrightImage");
        RegisterStub(102, "SetApplicationCopyrightVisibility");
        Register(123, "GetPreviousProgramIndex", [](Ctx& ctx) { ctx.Push<s32>(-1); });
        Register(130, "GetGpuErrorDetectedSystemEvent",
                 [](Ctx& ctx) { ctx.PushCopyObject(State().gpu_error_event); });
        Register(140, "GetFriendInvitationStorageChannelEvent",
                 [](Ctx& ctx) { ctx.PushCopyObject(State().friend_invitation_event); });
        Register(141, "TryPopFromFriendInvitationStorageChannel",
                 [](Ctx& ctx) { ctx.result = kResultNoDataInChannel; });
        Register(150, "GetNotificationStorageChannelEvent",
                 [](Ctx& ctx) { ctx.PushCopyObject(State().notification_event); });
        Register(160, "GetHealthWarningDisappearedSystemEvent",
                 [](Ctx& ctx) { ctx.PushCopyObject(State().health_warning_event); });
        // 210 (newer firmware): the SDK keeps the copy handle it returns as the
        // event nn::oe::GetOperationMode polls for operation-mode changes.
        Register(210, "GetOperationModeChangedEvent(210)",
                 [](Ctx& ctx) { ctx.PushCopyObject(State().operation_mode_event); });
        RegisterStub(1001, "PrepareForJit");
    }
};

class ILibraryAppletCreator final : public ServiceObject {
public:
    ILibraryAppletCreator();
};

ILibraryAppletCreator::ILibraryAppletCreator() : ServiceObject("ILibraryAppletCreator") {
    Register(0, "CreateLibraryApplet", [](Ctx& ctx) {
        const u32 id = ctx.Pop<u32>();
        const u32 mode = ctx.Pop<u32>();
        ctx.PushInterface(MakeLibraryAppletAccessor(id, mode));
    });
    Register(3, "CreateLibraryAppletEx", [](Ctx& ctx) {
        const u32 id = ctx.Pop<u32>();
        const u32 mode = ctx.Pop<u32>();
        ctx.PushInterface(MakeLibraryAppletAccessor(id, mode));
    });
    Register(10, "CreateStorage", [](Ctx& ctx) {
        const s64 size = ctx.Pop<s64>();
        ctx.PushInterface(std::make_shared<IStorage>(std::vector<u8>(static_cast<size_t>(size))));
    });
    Register(11, "CreateTransferMemoryStorage", [](Ctx& ctx) {
        ctx.Pop<u8>(); // writable
        const s64 size = ctx.Pop<s64>();
        std::shared_ptr<KTransferMemory> tm;
        {
            std::lock_guard lock(g_kernel.lock);
            tm = g_kernel.handles.GetAs<KTransferMemory>(ctx.copy_handles.empty() ? 0 : ctx.copy_handles[0]);
        }
        std::vector<u8> data(static_cast<size_t>(size));
        if (tm) g_memory.ReadBlock(tm->address, data.data(), std::min<u64>(tm->size, data.size()));
        ctx.PushInterface(std::make_shared<IStorage>(std::move(data)));
    });
}

class IApplicationProxy final : public ServiceObject {
public:
    IApplicationProxy() : ServiceObject("IApplicationProxy") {
        Register(0, "GetCommonStateGetter",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<ICommonStateGetter>()); });
        Register(1, "GetSelfController",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<ISelfController>()); });
        Register(2, "GetWindowController",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IWindowController>()); });
        Register(3, "GetAudioController",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IAudioController>()); });
        Register(4, "GetDisplayController",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IDisplayController>()); });
        Register(10, "GetProcessWindingController",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IProcessWindingController>()); });
        Register(11, "GetLibraryAppletCreator",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<ILibraryAppletCreator>()); });
        Register(20, "GetApplicationFunctions",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IApplicationFunctions>()); });
        Register(1000, "GetDebugFunctions",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IDebugFunctions>()); });
    }
};

class AppletOE final : public ServiceObject {
public:
    AppletOE() : ServiceObject("appletOE") {
        Register(0, "OpenApplicationProxy",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IApplicationProxy>()); });
    }
};

} // namespace

void RegisterAmServices() {
    RegisterService("appletOE", [] { return std::make_shared<AppletOE>(); });
}

} // namespace nxrt::hle
