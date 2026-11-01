// SPDX-License-Identifier: GPL-3.0-or-later
// time:u/time:a/time:s. The SDK reads the clocks out of the time shared memory
// on its own; IPC is only used for contexts, snapshots and time zones. Time
// zones come from the host: the device location is the host's zone, and
// calendar conversions use the C library.
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>

#include "memory.h"
#include "services/common.h"

namespace nxrt::hle {

namespace {

struct ClockSourceId {
    u64 lo, hi;
};
struct SteadyClockTimePoint {
    s64 time_point; // seconds
    ClockSourceId id;
};
struct SystemClockContext {
    s64 offset;
    SteadyClockTimePoint steady;
};
struct CalendarTime {
    s16 year;
    s8 month, day, hour, minute, second;
    s8 pad;
};
struct CalendarAdditionalInfo {
    s32 day_of_week;
    s32 day_of_year;
    char name[8];
    s32 is_dst;
    s32 ut_offset;
};
struct ClockSnapshot {
    SystemClockContext user_context;
    SystemClockContext network_context;
    s64 user_time;
    s64 network_time;
    CalendarTime user_calendar;
    CalendarTime network_calendar;
    CalendarAdditionalInfo user_info;
    CalendarAdditionalInfo network_info;
    SteadyClockTimePoint steady;
    char location[0x24];
    u8 is_automatic_correction_enabled;
    u8 type;
    u16 unk;
};
static_assert(sizeof(SteadyClockTimePoint) == 0x18);
static_assert(sizeof(SystemClockContext) == 0x20);
static_assert(sizeof(CalendarAdditionalInfo) == 0x18);
static_assert(sizeof(ClockSnapshot) == 0xD0);

template <typename T>
struct LockFree {
    u32 counter;
    u32 pad;
    T value[2];
};
struct ContinuousAdjustment {
    s64 rtc_offset, diff_scale, shift_amount, lower, upper;
    ClockSourceId id;
};

constexpr ClockSourceId kClockId{0x6e78727453746561ULL, 0x6479436c6f636b31ULL};
// Our rule buffers start with this tag; the rest of the 0x4000 bytes is unused.
constexpr u32 kRuleTag = 0x5A54584E; // "NXTZ"

// Seconds since boot on the guest's steady clock (the 19.2 MHz counter).
s64 SteadyNs() {
    const u64 ticks = ReadCntpct();
    return static_cast<s64>((ticks / 192) * 10000 + (ticks % 192) * 10000 / 192);
}
s64 SteadySeconds() {
    return SteadyNs() / 1'000'000'000;
}

#ifdef _WIN32
// Windows zone key -> IANA name (CLDR windowsZones.xml, territory 001). The key
// names are ASCII; zones missing here fall back to UTC.
struct WinZone {
    const char* windows;
    const char* iana;
};
constexpr WinZone kWinZones[] = {
    {"UTC", "UTC"},
    {"GMT Standard Time", "Europe/London"},
    {"Greenwich Standard Time", "Atlantic/Reykjavik"},
    {"W. Europe Standard Time", "Europe/Berlin"},
    {"Central Europe Standard Time", "Europe/Budapest"},
    {"Romance Standard Time", "Europe/Paris"},
    {"Central European Standard Time", "Europe/Warsaw"},
    {"W. Central Africa Standard Time", "Africa/Lagos"},
    {"GTB Standard Time", "Europe/Bucharest"},
    {"E. Europe Standard Time", "Europe/Chisinau"},
    {"FLE Standard Time", "Europe/Kiev"},
    {"Russian Standard Time", "Europe/Moscow"},
    {"Turkey Standard Time", "Europe/Istanbul"},
    {"Israel Standard Time", "Asia/Jerusalem"},
    {"Egypt Standard Time", "Africa/Cairo"},
    {"South Africa Standard Time", "Africa/Johannesburg"},
    {"E. Africa Standard Time", "Africa/Nairobi"},
    {"Arab Standard Time", "Asia/Riyadh"},
    {"Arabic Standard Time", "Asia/Baghdad"},
    {"Arabian Standard Time", "Asia/Dubai"},
    {"Iran Standard Time", "Asia/Tehran"},
    {"Pakistan Standard Time", "Asia/Karachi"},
    {"India Standard Time", "Asia/Calcutta"},
    {"Bangladesh Standard Time", "Asia/Dhaka"},
    {"SE Asia Standard Time", "Asia/Bangkok"},
    {"China Standard Time", "Asia/Shanghai"},
    {"Taipei Standard Time", "Asia/Taipei"},
    {"Singapore Standard Time", "Asia/Singapore"},
    {"W. Australia Standard Time", "Australia/Perth"},
    {"Tokyo Standard Time", "Asia/Tokyo"},
    {"Korea Standard Time", "Asia/Seoul"},
    {"Cen. Australia Standard Time", "Australia/Adelaide"},
    {"AUS Central Standard Time", "Australia/Darwin"},
    {"E. Australia Standard Time", "Australia/Brisbane"},
    {"AUS Eastern Standard Time", "Australia/Sydney"},
    {"Tasmania Standard Time", "Australia/Hobart"},
    {"New Zealand Standard Time", "Pacific/Auckland"},
    {"Hawaiian Standard Time", "Pacific/Honolulu"},
    {"Alaskan Standard Time", "America/Anchorage"},
    {"Pacific Standard Time", "America/Los_Angeles"},
    {"US Mountain Standard Time", "America/Phoenix"},
    {"Mountain Standard Time", "America/Denver"},
    {"Central Standard Time", "America/Chicago"},
    {"Canada Central Standard Time", "America/Regina"},
    {"Central America Standard Time", "America/Guatemala"},
    {"Central Standard Time (Mexico)", "America/Mexico_City"},
    {"Eastern Standard Time", "America/New_York"},
    {"US Eastern Standard Time", "America/Indianapolis"},
    {"SA Pacific Standard Time", "America/Bogota"},
    {"Atlantic Standard Time", "America/Halifax"},
    {"Newfoundland Standard Time", "America/St_Johns"},
    {"Venezuela Standard Time", "America/Caracas"},
    {"SA Western Standard Time", "America/La_Paz"},
    {"Pacific SA Standard Time", "America/Santiago"},
    {"Paraguay Standard Time", "America/Asuncion"},
    {"E. South America Standard Time", "America/Sao_Paulo"},
    {"SA Eastern Standard Time", "America/Cayenne"},
    {"Argentina Standard Time", "America/Buenos_Aires"},
    {"Montevideo Standard Time", "America/Montevideo"},
    {"Azores Standard Time", "Atlantic/Azores"},
    {"Cape Verde Standard Time", "Atlantic/Cape_Verde"},
};

static std::string HostLocationWindows() {
    DYNAMIC_TIME_ZONE_INFORMATION info{};
    if (GetDynamicTimeZoneInformation(&info) == TIME_ZONE_ID_INVALID) return {};
    std::string key;
    for (const wchar_t* c = info.TimeZoneKeyName; *c && key.size() < 128; ++c) {
        key.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
    }
    for (const auto& z : kWinZones) {
        if (key == z.windows) return z.iana;
    }
    LOG_WARN("Windows time zone '%s' has no IANA mapping; calendar conversions will use UTC",
             key.empty() ? "<unknown>" : key.c_str());
    return {};
}
#endif

struct TimeState {
    std::shared_ptr<KSharedMemory> shm;
    SystemClockContext user_context{};
    std::string location;
    std::shared_ptr<KEvent> user_event = MakeEvent("time:user-clock");
    std::shared_ptr<KEvent> network_event = MakeEvent("time:network-clock");
    std::shared_ptr<KEvent> location_event = MakeEvent("time:location");

    TimeState() {
        location = HostLocation();
        user_context.offset = static_cast<s64>(::time(nullptr)) - SteadySeconds();
        user_context.steady = {SteadySeconds(), kClockId};

        shm = std::make_shared<KSharedMemory>("time", 0x1000);
        u8* p = shm->HostPointer();
        std::memset(p, 0, 0x1000);
        // Steady clock: guest steady time (ns) = uptime + this base.
        auto* steady = reinterpret_cast<LockFree<SteadyClockTimePoint>*>(p + 0x00);
        steady->value[0] = {0, kClockId};
        auto* local = reinterpret_cast<LockFree<SystemClockContext>*>(p + 0x38);
        local->value[0] = user_context;
        auto* network = reinterpret_cast<LockFree<SystemClockContext>*>(p + 0x80);
        network->value[0] = user_context;
        // automatic_corrections at 0xC8 stays false
        auto* adjust = reinterpret_cast<LockFree<ContinuousAdjustment>*>(p + 0xD0);
        adjust->value[0] = {SteadyNs(), 0, 0, 0, 0, kClockId};
    }

    static std::string HostLocation() {
        if (const char* tz = std::getenv("TZ"); tz && *tz && *tz != ':') return tz;
#ifdef _WIN32
        return HostLocationWindows();
#else
        std::error_code ec;
        const auto target = std::filesystem::read_symlink("/etc/localtime", ec).string();
        const auto at = target.find("zoneinfo/");
        if (!ec && at != std::string::npos) return target.substr(at + 9);
        return "UTC";
#endif
    }
};

TimeState& State() {
    static TimeState state;
    return state;
}

s64 CurrentUserTime() {
    return State().user_context.offset + SteadySeconds();
}

// Calendar conversion with either the host's zone or UTC.
#ifdef _WIN32
// Treat the local broken-down fields as UTC to recover the exact offset for
// this timestamp. This also follows the UCRT's TZ override and avoids using
// the current offset for historical/future dates.
s32 WindowsUtcOffset(const struct tm& local, time_t utc) {
    using namespace std::chrono;
    const year_month_day date{year{local.tm_year + 1900}, month{static_cast<unsigned>(local.tm_mon + 1)},
                              day{static_cast<unsigned>(local.tm_mday)}};
    const auto since_epoch = sys_days{date}.time_since_epoch() + hours{local.tm_hour} + minutes{local.tm_min} +
                             seconds{local.tm_sec};
    const s64 local_as_utc = duration_cast<seconds>(since_epoch).count();
    const s64 offset = local_as_utc - static_cast<s64>(utc);
    // Time-zone offsets are well within one day; reject invalid calendar data.
    if (offset < -24 * 60 * 60 || offset > 24 * 60 * 60) return 0;
    return static_cast<s32>(offset);
}

// The guest field is only eight bytes, so retain the actual offset instead of
// guessing a localized zone abbreviation. Include seconds for historical
// offsets that are not a whole number of minutes.
void SetWindowsOffsetName(char (&name)[8], s32 offset) {
    if (offset == 0) {
        std::memcpy(name, "UTC", sizeof("UTC"));
        return;
    }
    const s32 magnitude = offset < 0 ? -offset : offset;
    const char sign = offset < 0 ? '-' : '+';
    const s32 hours = magnitude / 3600;
    const s32 minutes = (magnitude / 60) % 60;
    const s32 seconds = magnitude % 60;
    if (seconds == 0) std::snprintf(name, sizeof(name), "%c%02d%02d", sign, hours, minutes);
    else std::snprintf(name, sizeof(name), "%c%02d%02d%02d", sign, hours, minutes, seconds);
}

// Equivalent to timegm for the bounded guest calendar fields: normalize the
// month/day/time arithmetically, avoiding the UCRT's 1970 lower time bound.
s64 WindowsUtcCalendarEpoch(const struct tm& value) {
    s64 year = static_cast<s64>(value.tm_year) + 1900;
    s64 month = value.tm_mon;
    s64 year_adjustment = month / 12;
    if (month < 0 && month % 12 != 0) --year_adjustment;
    year += year_adjustment;
    month -= year_adjustment * 12;

    // Gregorian days from civil date, with day 1 as the month base; tm_mday
    // and the time fields are then added to preserve timegm normalization.
    year -= month < 2;
    const s64 era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned adjusted_month = static_cast<unsigned>(month + (month < 2 ? 10 : -2));
    const unsigned day_of_year = (153 * adjusted_month + 2) / 5;
    const unsigned day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    s64 days = era * 146097 + static_cast<s64>(day_of_era) - 719468;
    days += static_cast<s64>(value.tm_mday) - 1;

    constexpr s64 seconds_per_day = 24 * 60 * 60;
    const s64 seconds_within_day = static_cast<s64>(value.tm_hour) * 3600 +
                                   static_cast<s64>(value.tm_min) * 60 + value.tm_sec;
    constexpr s64 max = std::numeric_limits<s64>::max();
    constexpr s64 min = std::numeric_limits<s64>::min();
    if (days > max / seconds_per_day || days < min / seconds_per_day) return -1;
    const s64 day_seconds = days * seconds_per_day;
    if ((seconds_within_day > 0 && day_seconds > max - seconds_within_day) ||
        (seconds_within_day < 0 && day_seconds < min - seconds_within_day)) {
        return -1;
    }
    return day_seconds + seconds_within_day;
}
#endif

bool ToCalendar(s64 posix, bool local, CalendarTime& out, CalendarAdditionalInfo& info) {
    const time_t t = static_cast<time_t>(posix);
    struct tm tm {};
#ifdef _WIN32
    const auto conversion_result = local ? localtime_s(&tm, &t) : gmtime_s(&tm, &t);
    if (conversion_result != 0) return false;
#else
    const auto* conversion_result = local ? localtime_r(&t, &tm) : gmtime_r(&t, &tm);
    if (!conversion_result) return false;
#endif
    out = {static_cast<s16>(tm.tm_year + 1900), static_cast<s8>(tm.tm_mon + 1), static_cast<s8>(tm.tm_mday),
           static_cast<s8>(tm.tm_hour), static_cast<s8>(tm.tm_min), static_cast<s8>(tm.tm_sec), 0};
    info = {};
    info.day_of_week = tm.tm_wday;
    info.day_of_year = tm.tm_yday;
#ifdef _WIN32
    info.ut_offset = local ? WindowsUtcOffset(tm, t) : 0;
    SetWindowsOffsetName(info.name, info.ut_offset);
#else
    std::strncpy(info.name, local && tm.tm_zone ? tm.tm_zone : "UTC", sizeof(info.name) - 1);
    info.ut_offset = static_cast<s32>(tm.tm_gmtoff);
#endif
    info.is_dst = tm.tm_isdst > 0;
    return true;
}

s64 FromCalendar(const CalendarTime& c, bool local) {
    struct tm tm {};
    tm.tm_year = c.year - 1900;
    tm.tm_mon = c.month - 1;
    tm.tm_mday = c.day;
    tm.tm_hour = c.hour;
    tm.tm_min = c.minute;
    tm.tm_sec = c.second;
    tm.tm_isdst = -1;
#ifdef _WIN32
    return local ? static_cast<s64>(mktime(&tm)) : WindowsUtcCalendarEpoch(tm);
#else
    return local ? static_cast<s64>(mktime(&tm)) : static_cast<s64>(timegm(&tm));
#endif
}

bool RuleIsLocal(const std::vector<u8>& rule) {
    u32 tag = 0;
    if (rule.size() >= 8) std::memcpy(&tag, rule.data(), 4);
    return tag == kRuleTag && rule[4] == 1 && !State().location.empty();
}

bool FillSnapshot(ClockSnapshot& s, const SystemClockContext& user, const SystemClockContext& network, u8 type) {
    std::memset(&s, 0, sizeof(s));
    s.user_context = user;
    s.network_context = network;
    s.steady = {SteadySeconds(), kClockId};
    s.user_time = user.offset + s.steady.time_point;
    s.network_time = network.offset + s.steady.time_point;
    const bool local = !State().location.empty();
    if (!ToCalendar(s.user_time, local, s.user_calendar, s.user_info) ||
        !ToCalendar(s.network_time, local, s.network_calendar, s.network_info)) {
        return false;
    }
    std::strncpy(s.location, State().location.c_str(), sizeof(s.location) - 1);
    s.type = type;
    return true;
}

class ISystemClock final : public ServiceObject {
public:
    ISystemClock(const char* name, bool network) : ServiceObject(name), network_(network) {
        Register(0, "GetCurrentTime", [](Ctx& ctx) { ctx.Push<s64>(CurrentUserTime()); });
        RegisterStub(1, "SetCurrentTime");
        Register(2, "GetSystemClockContext", [](Ctx& ctx) { ctx.Push(State().user_context); });
        RegisterStub(3, "SetSystemClockContext");
        Register(4, "GetOperationEventReadableHandle", [this](Ctx& ctx) {
            ctx.PushCopyObject(network_ ? State().network_event : State().user_event);
        });
    }

private:
    bool network_;
};

class ISteadyClock final : public ServiceObject {
public:
    ISteadyClock() : ServiceObject("ISteadyClock") {
        Register(0, "GetCurrentTimePoint",
                 [](Ctx& ctx) { ctx.Push(SteadyClockTimePoint{SteadySeconds(), kClockId}); });
        Register(2, "GetTestOffset", [](Ctx& ctx) { ctx.Push<s64>(0); });
        RegisterStub(3, "SetTestOffset");
        Register(100, "GetRtcValue", [](Ctx& ctx) { ctx.Push<s64>(::time(nullptr)); });
        Register(101, "IsRtcResetDetected", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(102, "GetSetupResultValue", [](Ctx& ctx) { ctx.Push<u32>(0); });
        Register(200, "GetInternalOffset", [](Ctx& ctx) { ctx.Push<s64>(0); });
    }
};

class ITimeZoneService final : public ServiceObject {
public:
    ITimeZoneService() : ServiceObject("ITimeZoneService") {
        Register(0, "GetDeviceLocationName", [](Ctx& ctx) {
            char name[0x24] = {};
            std::strncpy(name, State().location.c_str(), sizeof(name) - 1);
            ctx.PushRaw(name, sizeof(name));
        });
        RegisterStub(1, "SetDeviceLocationName");
        Register(2, "GetTotalLocationNameCount", [](Ctx& ctx) { ctx.Push<s32>(1); });
        Register(3, "LoadLocationNameList", [](Ctx& ctx) {
            char name[0x24] = {};
            std::strncpy(name, State().location.c_str(), sizeof(name) - 1);
            ctx.WriteBuffer(name, sizeof(name));
            ctx.Push<s32>(1);
        });
        Register(4, "LoadTimeZoneRule", [](Ctx& ctx) {
            char name[0x25] = {};
            std::memcpy(name, ctx.RawInput().data(), std::min<size_t>(0x24, ctx.RawInput().size()));
            std::vector<u8> rule(8);
            std::memcpy(rule.data(), &kRuleTag, 4);
            rule[4] = State().location == name ? 1 : 0; // other zones are treated as UTC
            if (!rule[4]) LOG_WARN("time zone '%s' is not the host's; using UTC", name);
            ctx.WriteBuffer(rule.data(), rule.size());
        });
        Register(5, "GetTimeZoneRuleVersion", [](Ctx& ctx) {
            char version[0x10] = "2024a";
            ctx.PushRaw(version, sizeof(version));
        });
        Register(6, "GetDeviceLocationNameAndUpdatedTime", [](Ctx& ctx) {
            char name[0x24] = {};
            std::strncpy(name, State().location.c_str(), sizeof(name) - 1);
            ctx.PushRaw(name, sizeof(name));
            ctx.Push(SteadyClockTimePoint{0, kClockId});
        });
        Register(20, "GetDeviceLocationNameOperationEventReadableHandle",
                 [](Ctx& ctx) { ctx.PushCopyObject(State().location_event); });
        Register(100, "ToCalendarTime", [](Ctx& ctx) {
            const s64 t = ctx.Pop<s64>();
            CalendarTime c;
            CalendarAdditionalInfo info;
            if (!ToCalendar(t, RuleIsLocal(ctx.ReadBuffer()), c, info)) {
                ctx.result = rc::InvalidArgument;
                return;
            }
            ctx.Push(c);
            ctx.Push(info);
        });
        Register(101, "ToCalendarTimeWithMyRule", [](Ctx& ctx) {
            const s64 t = ctx.Pop<s64>();
            CalendarTime c;
            CalendarAdditionalInfo info;
            if (!ToCalendar(t, !State().location.empty(), c, info)) {
                ctx.result = rc::InvalidArgument;
                return;
            }
            ctx.Push(c);
            ctx.Push(info);
        });
        Register(201, "ToPosixTime", [](Ctx& ctx) {
            const CalendarTime c = ctx.Pop<CalendarTime>();
            const s64 t = FromCalendar(c, RuleIsLocal(ctx.ReadBuffer()));
            ctx.WriteBuffer(&t, sizeof(t));
            ctx.Push<s32>(1);
        });
        Register(202, "ToPosixTimeWithMyRule", [](Ctx& ctx) {
            const CalendarTime c = ctx.Pop<CalendarTime>();
            const s64 t = FromCalendar(c, !State().location.empty());
            ctx.WriteBuffer(&t, sizeof(t));
            ctx.Push<s32>(1);
        });
    }
};

class IStaticService final : public ServiceObject {
public:
    explicit IStaticService(const char* name) : ServiceObject(name) {
        Register(0, "GetStandardUserSystemClock",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<ISystemClock>("ISystemClock(user)", false)); });
        Register(1, "GetStandardNetworkSystemClock", [](Ctx& ctx) {
            ctx.PushInterface(std::make_shared<ISystemClock>("ISystemClock(network)", true));
        });
        Register(2, "GetStandardSteadyClock", [](Ctx& ctx) { ctx.PushInterface(std::make_shared<ISteadyClock>()); });
        Register(3, "GetTimeZoneService", [](Ctx& ctx) { ctx.PushInterface(std::make_shared<ITimeZoneService>()); });
        Register(4, "GetStandardLocalSystemClock",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<ISystemClock>("ISystemClock(local)", false)); });
        Register(5, "GetEphemeralNetworkSystemClock", [](Ctx& ctx) {
            ctx.PushInterface(std::make_shared<ISystemClock>("ISystemClock(ephemeral)", true));
        });
        Register(20, "GetSharedMemoryNativeHandle", [](Ctx& ctx) { ctx.PushCopyObject(State().shm); });
        Register(100, "IsStandardUserSystemClockAutomaticCorrectionEnabled", [](Ctx& ctx) { ctx.Push<u8>(0); });
        RegisterStub(101, "SetStandardUserSystemClockAutomaticCorrectionEnabled");
        Register(102, "GetStandardUserSystemClockInitialYear", [](Ctx& ctx) { ctx.Push<s32>(2019); });
        Register(200, "IsStandardNetworkSystemClockAccuracySufficient", [](Ctx& ctx) { ctx.Push<u8>(1); });
        Register(201, "GetStandardUserSystemClockAutomaticCorrectionUpdatedTime",
                 [](Ctx& ctx) { ctx.Push(SteadyClockTimePoint{0, kClockId}); });
        Register(300, "CalculateMonotonicSystemClockBaseTimePoint", [](Ctx& ctx) {
            const SystemClockContext c = ctx.Pop<SystemClockContext>();
            ctx.Push<s64>(c.offset);
        });
        Register(400, "GetClockSnapshot", [](Ctx& ctx) {
            const u8 type = ctx.Pop<u8>();
            ClockSnapshot s;
            if (!FillSnapshot(s, State().user_context, State().user_context, type)) {
                ctx.result = rc::InvalidArgument;
                return;
            }
            ctx.WriteBuffer(&s, sizeof(s));
        });
        Register(401, "GetClockSnapshotFromSystemClockContext", [](Ctx& ctx) {
            const u8 type = ctx.Pop<u8>();
            const SystemClockContext user = ctx.Pop<SystemClockContext>();
            const SystemClockContext network = ctx.Pop<SystemClockContext>();
            ClockSnapshot s;
            if (!FillSnapshot(s, user, network, type)) {
                ctx.result = rc::InvalidArgument;
                return;
            }
            ctx.WriteBuffer(&s, sizeof(s));
        });
        Register(500, "CalculateStandardUserSystemClockDifferenceByUser", [](Ctx& ctx) {
            ClockSnapshot a{}, b{};
            const auto ba = ctx.ReadBuffer(0), bb = ctx.ReadBuffer(1);
            std::memcpy(&a, ba.data(), std::min(ba.size(), sizeof(a)));
            std::memcpy(&b, bb.data(), std::min(bb.size(), sizeof(b)));
            ctx.Push<s64>((b.user_context.offset - a.user_context.offset) * 1'000'000'000);
        });
        Register(501, "CalculateSpanBetween", [](Ctx& ctx) {
            ClockSnapshot a{}, b{};
            const auto ba = ctx.ReadBuffer(0), bb = ctx.ReadBuffer(1);
            std::memcpy(&a, ba.data(), std::min(ba.size(), sizeof(a)));
            std::memcpy(&b, bb.data(), std::min(bb.size(), sizeof(b)));
            s64 seconds;
            if (a.steady.id.lo == b.steady.id.lo && a.steady.id.hi == b.steady.id.hi) {
                seconds = b.steady.time_point - a.steady.time_point;
            } else {
                seconds = b.user_time - a.user_time;
            }
            ctx.Push<s64>(seconds * 1'000'000'000);
        });
    }
};

} // namespace

void RegisterTimeServices() {
    for (const char* name : {"time:u", "time:a", "time:s"}) {
        RegisterService(name, [name] { return std::make_shared<IStaticService>(name); });
    }
}

} // namespace nxrt::hle
