// SPDX-License-Identifier: GPL-3.0-or-later
// hid: input through the 0x40000-byte shared memory the SDK reads directly.
// Player 1 is a connected Pro Controller whose state is refreshed at 60 Hz from
// the host (see SetHostInput); everything else stays disconnected.
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <sstream>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "services/common.h"
#include "services/hid.h"
#include "services/hid_gesture.h"
#include "services/hid_touch.h"
#include "services/input_record.h"
#include "services/input_script.h"
#include "cpu.h"

namespace nxrt::hle {

namespace {

constexpr int kLifoEntries = 17;

template <typename T>
struct Lifo {
    s64 timestamp;
    s64 total_buffer_count;
    s64 buffer_tail;
    s64 buffer_count;
    struct Entry {
        s64 sampling_number;
        T state;
    } entries[kLifoEntries];

    void Push(const T& state, s64 sampling) {
        total_buffer_count = kLifoEntries;
        if (buffer_count < kLifoEntries - 1) ++buffer_count;
        const s64 next = (buffer_tail + 1) % kLifoEntries;
        entries[next].sampling_number = sampling << 1;
        entries[next].state = state;
        __atomic_store_n(&buffer_tail, next, __ATOMIC_RELEASE);
        timestamp = sampling;
    }
};

struct NpadGenericState {
    s64 sampling_number;
    u64 buttons;
    s32 lx, ly, rx, ry;
    u32 attributes; // bit0 connected, bit1 wired
    u32 reserved;
};
static_assert(sizeof(NpadGenericState) == 0x28);

struct SixAxisSensorState {
    s64 delta_time;
    s64 sampling_number;
    float accel[3], gyro[3], rotation[3], orientation[9];
    u32 attribute;
    u32 reserved;
};
static_assert(sizeof(SixAxisSensorState) == 0x60);

struct GcTriggerState {
    s64 sampling_number;
    s32 l, r;
};

struct NpadInternalState {
    u32 style_tag;
    u32 assignment_mode;
    u32 fullkey_color[3];  // attribute, body, buttons
    u32 joycon_color[5];   // attribute, left body/buttons, right body/buttons
    Lifo<NpadGenericState> fullkey, handheld, joy_dual, joy_left, joy_right, palma, system_ext;
    Lifo<SixAxisSensorState> sixaxis[6];
    u32 device_type;
    u32 reserved;
    u64 system_properties;
    u32 button_properties;
    u32 battery_dual, battery_left, battery_right;
    u32 applet_footer_attributes;
    u8 applet_footer_type;
    u8 reserved2[0x5B + 0x20];
    Lifo<GcTriggerState> gc_trigger;
    u32 lark_l, lark_r, lucia, lager;
    u8 sixaxis_properties[6];
};
static_assert(offsetof(NpadInternalState, fullkey) == 0x28);
static_assert(offsetof(NpadInternalState, device_type) == 0x4188);
static_assert(sizeof(NpadInternalState) == 0x43F8);

constexpr size_t kSharedSize = 0x40000;
constexpr size_t kNpadOffset = 0x9A00;
constexpr size_t kNpadEntrySize = 0x5000;
constexpr size_t kTouchScreenOffset = 0x400;
constexpr size_t kGestureOffset = 0x3BA00;
static_assert(kTouchScreenOffset + sizeof(TouchScreenLifo) <= kSharedSize);
static_assert(kGestureOffset + sizeof(GestureLifo) <= kSharedSize);

struct HidState {
    std::shared_ptr<KSharedMemory> shm = std::make_shared<KSharedMemory>("hid", kSharedSize);
    std::shared_ptr<KEvent> style_event = MakeEvent("hid:style-set", true);
    std::atomic<u64> buttons{0};
    std::atomic<u64> script_buttons{0}; // NXRT_INPUT_SCRIPT, ORed into the sampled state
    std::atomic<s32> lx{0}, ly{0}, rx{0}, ry{0};
    std::atomic<bool> touch_pressed{false};
    std::atomic<u32> touch_x{0}, touch_y{0};
    // Touch injected through NXRT_INPUT_FIFO. Kept apart from the window's mouse
    // state, which is rewritten on every event-loop pass and would otherwise
    // clear a scripted touch before the HID sampler sees it.
    std::atomic<bool> script_touch_pressed{false};
    std::atomic<u32> script_touch_x{0}, script_touch_y{0};
    std::atomic<s32> script_lx{0}, script_ly{0}, script_rx{0}, script_ry{0};
    std::atomic<bool> script_axes_active{false};
    TouchInputSampler touch_sampler;
    GestureSampler gesture_sampler;
    std::once_flag started;

    NpadInternalState* Npad(int index) {
        return reinterpret_cast<NpadInternalState*>(shm->HostPointer() + kNpadOffset + index * kNpadEntrySize);
    }

    void Start() {
        std::call_once(started, [this] {
            NpadInternalState* p = Npad(0); // Player 1
            p->style_tag = 1;               // FullKey (Pro Controller)
            p->assignment_mode = 0;         // dual
            p->fullkey_color[0] = 0;        // colors present
            p->fullkey_color[1] = 0xFF323232;
            p->fullkey_color[2] = 0xFFE6E6E6;
            p->device_type = 1;             // FullKey
            p->system_properties = (1u << 3) | (1u << 13) | (1u << 14) | (1u << 15); // powered, +, -, d-pad
            p->battery_dual = 4;
            p->applet_footer_type = 3;      // Pro Controller
            std::thread([this] { Run(); }).detach();
        });
    }

    void Run() {
        s64 sampling = 0;
        static const bool trace_host_input = [] {
            const char* value = std::getenv("NXRT_TRACE_HOST_INPUT");
            return value && value[0] == '1' && value[1] == '\0';
        }();
        u64 previous_buttons = ~u64{0};
        s32 previous_lx = 0, previous_ly = 0, previous_rx = 0, previous_ry = 0;
        bool previous_touch_pressed = false;
        u32 previous_touch_x = 0, previous_touch_y = 0;
        auto next = std::chrono::steady_clock::now();
        for (;;) {
            next += std::chrono::microseconds(16667);
            std::this_thread::sleep_until(next);
            ++sampling;
            NpadGenericState s{};
            s.sampling_number = sampling;
            s.buttons = buttons.load(std::memory_order_relaxed) | script_buttons.load(std::memory_order_relaxed);
            if (script_axes_active.load(std::memory_order_relaxed)) {
                s.lx = script_lx.load(std::memory_order_relaxed);
                s.ly = script_ly.load(std::memory_order_relaxed);
                s.rx = script_rx.load(std::memory_order_relaxed);
                s.ry = script_ry.load(std::memory_order_relaxed);
            } else {
                s.lx = lx;
                s.ly = ly;
                s.rx = rx;
                s.ry = ry;
            }
            s.attributes = 1; // connected
            if (trace_host_input &&
                (s.buttons != previous_buttons || s.lx != previous_lx || s.ly != previous_ly ||
                 s.rx != previous_rx || s.ry != previous_ry)) {
                LOG_INFO("hid: player 1 sample %lld buttons=0x%llx sticks=(%d,%d,%d,%d)",
                         static_cast<long long>(sampling), static_cast<unsigned long long>(s.buttons),
                         s.lx, s.ly, s.rx, s.ry);
                previous_buttons = s.buttons;
                previous_lx = s.lx;
                previous_ly = s.ly;
                previous_rx = s.rx;
                previous_ry = s.ry;
            }
            Npad(0)->fullkey.Push(s, sampling);
            auto* touch_lifo =
                reinterpret_cast<TouchScreenLifo*>(shm->HostPointer() + kTouchScreenOffset);
            const bool script_touch = this->script_touch_pressed.load(std::memory_order_relaxed);
            const bool touch_pressed =
                script_touch || this->touch_pressed.load(std::memory_order_relaxed);
            // The End sample keeps the position the finger left from.
            const u32 touch_x = !touch_pressed ? previous_touch_x
                                : script_touch ? this->script_touch_x.load(std::memory_order_relaxed)
                                               : this->touch_x.load(std::memory_order_relaxed);
            const u32 touch_y = !touch_pressed ? previous_touch_y
                                : script_touch ? this->script_touch_y.load(std::memory_order_relaxed)
                                               : this->touch_y.load(std::memory_order_relaxed);
            touch_sampler.Write(*touch_lifo, sampling, touch_pressed, touch_x, touch_y);
            gesture_sampler.Write(*reinterpret_cast<GestureLifo*>(shm->HostPointer() + kGestureOffset),
                                  sampling, touch_pressed, touch_x, touch_y);
            if (touch_pressed != previous_touch_pressed) {
                ResetHidReadTraceOnTouchTransition();
            }
            if (trace_host_input &&
                (touch_pressed != previous_touch_pressed || touch_x != previous_touch_x ||
                 touch_y != previous_touch_y)) {
                LOG_INFO("hid: touch sample %lld pressed=%u position=(%u,%u)",
                         static_cast<long long>(sampling), touch_pressed ? 1u : 0u, touch_x, touch_y);
            }
            previous_touch_pressed = touch_pressed;
            previous_touch_x = touch_x;
            previous_touch_y = touch_y;
            NpadGenericState none{};
            none.sampling_number = sampling;
            for (int i = 1; i < 10; ++i) {
                if (Npad(i)->style_tag == 0) Npad(i)->fullkey.Push(none, sampling);
            }
        }
    }
};

HidState& State() {
    static HidState s;
    return s;
}

std::vector<input_script::Step> LoadScript() {
    std::vector<input_script::Step> steps;
    const char* path = std::getenv("NXRT_INPUT_SCRIPT");
    if (!path || !*path) return steps;
    std::ifstream f(path);
    if (!f) {
        LOG_WARN("hid: input script '%s' cannot be opened", path);
        return steps;
    }
    std::string line;
    size_t line_number = 0;
    while (std::getline(f, line)) {
        ++line_number;
        if (const auto comment = line.find('#'); comment != std::string::npos) line.erase(comment);
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        input_script::Step step;
        if (!input_script::ParseLine(line, step)) {
            LOG_WARN("hid: input script '%s' line %zu is invalid; expected `frame buttons [lx ly rx ry]` "
                     "with axes in -32767..32767",
                     path, line_number);
            continue;
        }
        steps.push_back(std::move(step));
    }
    LOG_INFO("hid: input script '%s' loaded, %zu steps", path, steps.size());
    return steps;
}

class IAppletResource final : public ServiceObject {
public:
    IAppletResource() : ServiceObject("IAppletResource") {
        Register(0, "GetSharedMemoryHandle", [](Ctx& ctx) {
            State().Start();
            ctx.PushCopyObject(State().shm);
        });
    }
};

class IActiveVibrationDeviceList final : public ServiceObject {
public:
    IActiveVibrationDeviceList() : ServiceObject("IActiveVibrationDeviceList") {
        RegisterStub(0, "ActivateVibrationDevice");
    }
};

class Hid final : public ServiceObject {
public:
    Hid() : ServiceObject("hid") {
        Register(0, "CreateAppletResource", [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IAppletResource>()); });
        for (u32 id : {1u, 11u, 21u, 26u, 31u, 51u, 56u, 60u, 61u, 63u, 64u, 66u, 67u, 69u, 70u, 72u, 79u, 81u,
                       84u, 87u, 89u, 91u, 92u, 100u, 102u, 103u, 104u, 107u, 109u, 120u, 122u, 123u, 124u,
                       125u, 126u, 127u, 128u, 130u, 132u, 133u, 134u, 135u, 136u, 201u, 204u, 206u, 207u,
                       209u, 210u, 212u, 300u, 301u, 302u, 303u, 304u, 305u, 306u, 307u, 310u, 1000u, 1002u}) {
            RegisterStub(id, "(accepted)");
        }
        Register(1004, "SetTouchScreenResolution", [](Ctx& ctx) {
            static const bool trace = [] {
                const char* value = std::getenv("NXRT_TRACE_HID_CONFIG");
                return value && value[0] == '1' && value[1] == '\0';
            }();
            const u32 width = ctx.Pop<u32>();
            const u32 height = ctx.Pop<u32>();
            const u64 applet_resource_user_id = ctx.Pop<u64>();
            if (trace) {
                LOG_INFO("hid: SetTouchScreenResolution width=%u height=%u aruid=%llu", width, height,
                         static_cast<unsigned long long>(applet_resource_user_id));
            }
        });
        Register(68, "IsSixAxisSensorFusionEnabled", [](Ctx& ctx) { ctx.Push<u8>(1); });
        Register(82, "IsSixAxisSensorAtRest", [](Ctx& ctx) { ctx.Push<u8>(1); });
        Register(83, "IsFirmwareUpdateAvailableForSixAxisSensor", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(85, "IsSixAxisSensorUnalteredPassthroughEnabled", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(101, "GetSupportedNpadStyleSet", [](Ctx& ctx) { ctx.Push<u32>(0x1F); });
        Register(106, "AcquireNpadStyleSetUpdateEventHandle",
                 [](Ctx& ctx) { ctx.PushCopyObject(State().style_event); });
        Register(108, "GetPlayerLedPattern", [](Ctx& ctx) { ctx.Push<u64>(1); });
        Register(121, "GetNpadJoyHoldType", [](Ctx& ctx) { ctx.Push<u64>(1); });
        Register(129, "GetNpadHandheldActivationMode", [](Ctx& ctx) { ctx.Push<u64>(0); });
        Register(131, "IsUnintendedHomeButtonInputProtectionEnabled", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(200, "GetVibrationDeviceInfo", [](Ctx& ctx) {
            ctx.Push<u32>(1); // linear resonant actuator
            ctx.Push<u32>(0); // position: none
        });
        Register(202, "GetActualVibrationValue", [](Ctx& ctx) {
            const float v[4] = {0.0f, 160.0f, 0.0f, 320.0f};
            ctx.PushRaw(v, sizeof(v));
        });
        Register(203, "CreateActiveVibrationDeviceList",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IActiveVibrationDeviceList>()); });
        Register(205, "IsVibrationPermitted", [](Ctx& ctx) { ctx.Push<u8>(1); });
        Register(211, "IsVibrationDeviceMounted", [](Ctx& ctx) { ctx.Push<u8>(1); });
        Register(400, "IsUsbFullKeyControllerEnabled", [](Ctx& ctx) { ctx.Push<u8>(0); });
        Register(1001, "GetNpadCommunicationMode", [](Ctx& ctx) { ctx.Push<u64>(0); });
        Register(1003, "IsFirmwareUpdateNeededForNotification", [](Ctx& ctx) { ctx.Push<u8>(0); });
    }
};


// NXRT_INPUT_FIFO=<path>: opt-in input driven from outside the process (tools/nav).
// One command per line, applied on the next presented frame; the last command
// stays in effect until replaced unless it carries `hold=N` (released after N
// presented frames, so the press length does not depend on the frame rate):
//   <buttons> [lx ly rx ry] [hold=N]   buttons as in NXRT_INPUT_SCRIPT (A, A,Right, -)
//   touch <x> <y> [hold=N]             touch down at 1280x720 coordinates
//   touch up                           touch release
// Every applied command is logged as `hid: fifo #n frame=F: <line>`.
struct FifoState {
    int fd = -1;
    bool tried = false;
    std::string pending;
    u64 count = 0;
    u64 release_buttons_at = 0; // 0 = none
    u64 release_touch_at = 0;
};

void ApplyFifoLine(FifoState& fs, const std::string& raw, u64 frame) {
    std::istringstream in(raw);
    std::vector<std::string> f;
    for (std::string t; in >> t;) f.push_back(std::move(t));
    if (f.empty()) return;
    u64 hold = 0;
    if (f.back().rfind("hold=", 0) == 0) {
        char* end = nullptr;
        hold = std::strtoull(f.back().c_str() + 5, &end, 10);
        if (!end || *end) {
            LOG_WARN("hid: fifo invalid hold in '%s'", raw.c_str());
            return;
        }
        f.pop_back();
    }
    HidState& hid = State();
    if (!f.empty() && f[0] == "touch") {
        if (f.size() == 2 && f[1] == "up") {
            hid.script_touch_pressed.store(false, std::memory_order_relaxed);
            fs.release_touch_at = 0;
        } else if (f.size() == 3) {
            hid.script_touch_x.store(
                std::min<u32>(static_cast<u32>(std::strtoul(f[1].c_str(), nullptr, 10)), 1279),
                std::memory_order_relaxed);
            hid.script_touch_y.store(
                std::min<u32>(static_cast<u32>(std::strtoul(f[2].c_str(), nullptr, 10)), 719),
                std::memory_order_relaxed);
            hid.script_touch_pressed.store(true, std::memory_order_relaxed);
            fs.release_touch_at = hold ? frame + hold : 0;
        } else {
            LOG_WARN("hid: fifo invalid touch command '%s'", raw.c_str());
            return;
        }
    } else {
        std::string joined = "0";
        for (const auto& t : f) joined += " " + t;
        input_script::Step step;
        if (!input_script::ParseLine(joined, step)) {
            LOG_WARN("hid: fifo invalid command '%s'", raw.c_str());
            return;
        }
        static input_script::State fifo_state{};
        input_script::Apply(step, fifo_state);
        hid.script_buttons.store(fifo_state.buttons, std::memory_order_relaxed);
        if (fifo_state.axes_active) {
            hid.script_lx.store(fifo_state.sticks.lx, std::memory_order_relaxed);
            hid.script_ly.store(fifo_state.sticks.ly, std::memory_order_relaxed);
            hid.script_rx.store(fifo_state.sticks.rx, std::memory_order_relaxed);
            hid.script_ry.store(fifo_state.sticks.ry, std::memory_order_relaxed);
            hid.script_axes_active.store(true, std::memory_order_relaxed);
        }
        fs.release_buttons_at = hold ? frame + hold : 0;
    }
    LOG_INFO("hid: fifo #%llu frame=%llu: %s", static_cast<unsigned long long>(++fs.count),
             static_cast<unsigned long long>(frame), raw.c_str());
}

void PollInputFifo(u64 frame) {
#ifndef _WIN32
    static FifoState fs;
    if (!fs.tried) {
        fs.tried = true;
        if (const char* path = std::getenv("NXRT_INPUT_FIFO"); path && *path) {
            // O_RDWR keeps the FIFO open (no EOF) while writers come and go.
            fs.fd = open(path, O_RDWR | O_NONBLOCK);
            if (fs.fd < 0)
                LOG_WARN("hid: NXRT_INPUT_FIFO '%s' cannot be opened", path);
            else
                LOG_INFO("hid: input fifo '%s' open", path);
        }
    }
    if (fs.fd < 0) return;
    HidState& hid = State();
    if (fs.release_buttons_at && frame >= fs.release_buttons_at) {
        hid.script_buttons.store(0, std::memory_order_relaxed);
        fs.release_buttons_at = 0;
    }
    if (fs.release_touch_at && frame >= fs.release_touch_at) {
        hid.script_touch_pressed.store(false, std::memory_order_relaxed);
        fs.release_touch_at = 0;
    }
    char buf[512];
    ssize_t n;
    while ((n = read(fs.fd, buf, sizeof(buf))) > 0) fs.pending.append(buf, static_cast<size_t>(n));
    size_t nl;
    while ((nl = fs.pending.find('\n')) != std::string::npos) {
        std::string line = fs.pending.substr(0, nl);
        fs.pending.erase(0, nl + 1);
        ApplyFifoLine(fs, line, frame);
    }
    if (fs.pending.size() > 4096) fs.pending.clear();
#else
    (void)frame;
#endif
}

// NXRT_INPUT_RECORD=<file>: records the effective input per presented frame (only on change).
// Taps that start and end between two frames are kept for one frame via the "seen" accumulators.
const bool g_record_enabled = [] {
    const char* v = std::getenv("NXRT_INPUT_RECORD");
    return v && *v;
}();
std::atomic<u64> g_seen_buttons{0};
std::atomic<bool> g_seen_touch{false};

input_record::Writer* RecordWriter() {
    static input_record::Writer* writer = [] () -> input_record::Writer* {
        auto* w = new input_record::Writer;
        const char* path = std::getenv("NXRT_INPUT_RECORD");
        if (!w->Open(path)) {
            LOG_WARN("hid: NXRT_INPUT_RECORD '%s' cannot be opened", path);
            delete w;
            return nullptr;
        }
        LOG_INFO("hid: recording input to '%s' (F9 = marker)", path);
        return w;
    }();
    return writer;
}

void RecordFrame(u64 frame) {
    input_record::Writer* writer = RecordWriter();
    if (!writer) return;
    HidState& hid = State();
    input_record::Event e;
    e.frame = frame;
    e.buttons = hid.buttons.load(std::memory_order_relaxed) | hid.script_buttons.load(std::memory_order_relaxed) |
                g_seen_buttons.exchange(0, std::memory_order_relaxed);
    if (hid.script_axes_active.load(std::memory_order_relaxed)) {
        e.lx = hid.script_lx.load(std::memory_order_relaxed);
        e.ly = hid.script_ly.load(std::memory_order_relaxed);
        e.rx = hid.script_rx.load(std::memory_order_relaxed);
        e.ry = hid.script_ry.load(std::memory_order_relaxed);
    } else {
        e.lx = hid.lx;
        e.ly = hid.ly;
        e.rx = hid.rx;
        e.ry = hid.ry;
    }
    e.has_axes = e.lx || e.ly || e.rx || e.ry;
    const bool seen_touch = g_seen_touch.exchange(false, std::memory_order_relaxed);
    const bool script_touch = hid.script_touch_pressed.load(std::memory_order_relaxed);
    e.touch = script_touch || hid.touch_pressed.load(std::memory_order_relaxed) || seen_touch;
    if (e.touch) {
        e.touch_x = script_touch ? hid.script_touch_x.load(std::memory_order_relaxed)
                                 : hid.touch_x.load(std::memory_order_relaxed);
        e.touch_y = script_touch ? hid.script_touch_y.load(std::memory_order_relaxed)
                                 : hid.touch_y.load(std::memory_order_relaxed);
    }
    static input_record::Event previous{};
    if (e.SameState(previous)) return;
    previous = e;
    writer->WriteEvent(e);
}

// NXRT_INPUT_REPLAY=<file>: applies a recorded file; NXRT_INPUT_REPLAY_STOP_AT_MARCO=<n> raises SIGINT
// NXRT_INPUT_REPLAY_STOP_AFTER (default 120) frames after marker n.
void ReplayFrame(u64 frame) {
    static input_record::Recording rec;
    static size_t next = 0;
    static u64 stop_frame = 0;
    static bool stopped = false;
    static const bool loaded = [] {
        const char* path = std::getenv("NXRT_INPUT_REPLAY");
        if (!path || !*path) return false;
        if (!input_record::Load(path, rec)) {
            LOG_WARN("hid: NXRT_INPUT_REPLAY '%s' cannot be opened", path);
            return false;
        }
        LOG_INFO("hid: replay '%s' loaded, %zu events, %zu markers, %zu bad lines", path, rec.events.size(),
                 rec.markers.size(), rec.bad_lines);
        if (const char* n = std::getenv("NXRT_INPUT_REPLAY_STOP_AT_MARCO"); n && *n) {
            const u64 want = std::strtoull(n, nullptr, 10);
            const char* after = std::getenv("NXRT_INPUT_REPLAY_STOP_AFTER");
            const u64 extra = after && *after ? std::strtoull(after, nullptr, 10) : 120;
            bool found = false;
            for (const auto& m : rec.markers) {
                if (m.n == want) {
                    stop_frame = m.frame + extra;
                    found = true;
                }
            }
            if (found)
                LOG_INFO("hid: replay stops (SIGINT) at frame %llu", static_cast<unsigned long long>(stop_frame));
            else
                LOG_WARN("hid: replay marker %llu not found; not stopping", static_cast<unsigned long long>(want));
        }
        return true;
    }();
    if (!loaded) return;
    HidState& hid = State();
    while (next < rec.events.size() && rec.events[next].frame <= frame) {
        const input_record::Event& e = rec.events[next++];
        hid.script_buttons.store(e.buttons, std::memory_order_relaxed);
        hid.script_lx.store(e.has_axes ? e.lx : 0, std::memory_order_relaxed);
        hid.script_ly.store(e.has_axes ? e.ly : 0, std::memory_order_relaxed);
        hid.script_rx.store(e.has_axes ? e.rx : 0, std::memory_order_relaxed);
        hid.script_ry.store(e.has_axes ? e.ry : 0, std::memory_order_relaxed);
        hid.script_axes_active.store(true, std::memory_order_relaxed);
        if (e.touch) {
            hid.script_touch_x.store(e.touch_x, std::memory_order_relaxed);
            hid.script_touch_y.store(e.touch_y, std::memory_order_relaxed);
        }
        hid.script_touch_pressed.store(e.touch, std::memory_order_relaxed);
        LOG_INFO("hid: replay frame %llu: %s", static_cast<unsigned long long>(frame),
                 input_record::FormatEvent(e).c_str());
    }
    if (stop_frame && !stopped && frame >= stop_frame) {
        stopped = true;
        LOG_INFO("hid: replay reached frame %llu; raising SIGINT", static_cast<unsigned long long>(frame));
        std::fflush(nullptr);
        std::raise(SIGINT);
    }
}

} // namespace

void InputRecordMarker(u64 frame) {
    if (!g_record_enabled) return;
    input_record::Writer* writer = RecordWriter();
    if (!writer) return;
    const input_record::Marker m = writer->WriteMarker(frame);
    input_record::RequestDump(writer->path() + ".marco" + std::to_string(m.n) + ".ppm");
    std::fprintf(stderr, "marco %llu gravado no quadro %llu\n", static_cast<unsigned long long>(m.n),
                 static_cast<unsigned long long>(m.frame));
}

bool InputRecordEnabled() {
    return g_record_enabled;
}

void SetHostInput(u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry) {
    HidState& s = State();
    s.buttons = buttons;
    if (g_record_enabled && buttons) g_seen_buttons.fetch_or(buttons, std::memory_order_relaxed);
    s.lx = lx;
    s.ly = ly;
    s.rx = rx;
    s.ry = ry;
}

HostInputState GetHostInput() {
    const HidState& s = State();
    return {s.buttons, s.lx, s.ly, s.rx, s.ry};
}

void SetHostTouch(bool pressed, u32 x, u32 y) {
    HidState& s = State();
    // A release keeps the last position: the End sample of real hardware still
    // carries the coordinates the finger left from, and UI buttons that act on
    // release hit-test against them.
    if (pressed) {
        s.touch_x.store(std::min<u32>(x, 1279), std::memory_order_relaxed);
        s.touch_y.store(std::min<u32>(y, 719), std::memory_order_relaxed);
    }
    s.touch_pressed.store(pressed, std::memory_order_relaxed);
    if (g_record_enabled && pressed) g_seen_touch.store(true, std::memory_order_relaxed);
}

void OnInputFrame(u64 frame) {
    input_record::SetCurrentFrame(frame);
    PollInputFifo(frame);
    ReplayFrame(frame);
    static const std::vector<input_script::Step> steps = LoadScript();
    static size_t next = 0;
    static input_script::State script_state{};
    // Called only from the presenting thread, so `next` needs no lock.
    while (next < steps.size() && steps[next].frame <= frame) {
        const input_script::Step& st = steps[next++];
        input_script::Apply(st, script_state);
        HidState& hid = State();
        hid.script_buttons.store(script_state.buttons, std::memory_order_relaxed);
        if (script_state.axes_active) {
            hid.script_lx.store(script_state.sticks.lx, std::memory_order_relaxed);
            hid.script_ly.store(script_state.sticks.ly, std::memory_order_relaxed);
            hid.script_rx.store(script_state.sticks.rx, std::memory_order_relaxed);
            hid.script_ry.store(script_state.sticks.ry, std::memory_order_relaxed);
            hid.script_axes_active.store(true, std::memory_order_relaxed);
            LOG_INFO("hid: input script frame %llu: buttons=%s (0x%llx), sticks=script(%d,%d,%d,%d)",
                     static_cast<unsigned long long>(frame), st.text.c_str(),
                     static_cast<unsigned long long>(st.buttons), script_state.sticks.lx, script_state.sticks.ly,
                     script_state.sticks.rx, script_state.sticks.ry);
        } else {
            LOG_INFO("hid: input script frame %llu: buttons=%s (0x%llx), sticks=host(%d,%d,%d,%d)",
                     static_cast<unsigned long long>(frame), st.text.c_str(),
                     static_cast<unsigned long long>(st.buttons), hid.lx.load(std::memory_order_relaxed),
                     hid.ly.load(std::memory_order_relaxed), hid.rx.load(std::memory_order_relaxed),
                     hid.ry.load(std::memory_order_relaxed));
        }
    }
    if (g_record_enabled) RecordFrame(frame);
}

void RegisterHidServices() {
    RegisterService("hid", [] { return std::make_shared<Hid>(); });
}

} // namespace nxrt::hle
