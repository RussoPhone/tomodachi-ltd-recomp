// SPDX-License-Identifier: GPL-3.0-or-later
// vi: displays, layers, vsync, and the Android-style buffer queue the game
// presents through (IHOSBinderDriver). Buffers are released the moment they are
// queued; each queued frame is handed to the presenter.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <thread>

#include "frontend/window.h"
#include "render/counter_trace.h"
#include "services/common.h"
#include "services/vi.h"
#include "services/vsync_pacing.h"

namespace nxrt::hle {

namespace {

// ------------------------------------------------------------ parcels

class InParcel {
public:
    explicit InParcel(std::vector<u8> data) : data_(std::move(data)) {
        u32 data_offset = 0x10;
        if (data_.size() >= 16) std::memcpy(&data_offset, data_.data() + 4, 4);
        pos_ = data_offset;
        // Interface token: strict-mode word, length, UTF-16 name with NUL.
        Read<u32>();
        const u32 length = Read<u32>();
        pos_ += (static_cast<size_t>(length) + 1) * 2;
        Align();
    }
    template <typename T>
    T Read() {
        T v{};
        if (pos_ + sizeof(T) <= data_.size()) std::memcpy(&v, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        Align();
        return v;
    }
    // A flattened object: is_valid word, then its size, then the bytes.
    std::vector<u8> ReadObject() {
        if (!Read<u32>()) return {};
        const s64 size = Read<s64>();
        std::vector<u8> v(static_cast<size_t>(size));
        if (pos_ + v.size() <= data_.size()) std::memcpy(v.data(), data_.data() + pos_, v.size());
        pos_ += v.size();
        Align();
        return v;
    }
    std::vector<u8> ReadFlattened() {
        const s64 size = Read<s64>();
        std::vector<u8> v(static_cast<size_t>(size));
        if (pos_ + v.size() <= data_.size()) std::memcpy(v.data(), data_.data() + pos_, v.size());
        pos_ += v.size();
        Align();
        return v;
    }

private:
    void Align() { pos_ = (pos_ + 3) & ~size_t{3}; }
    std::vector<u8> data_;
    size_t pos_ = 0;
};

class OutParcel {
public:
    template <typename T>
    void Write(const T& v) {
        const size_t at = data_.size();
        data_.resize(at + ((sizeof(T) + 3) & ~size_t{3}));
        std::memcpy(data_.data() + at, &v, sizeof(T));
    }
    void WriteBytes(const void* p, size_t n) {
        const size_t at = data_.size();
        data_.resize(at + ((n + 3) & ~size_t{3}));
        std::memcpy(data_.data() + at, p, n);
    }
    void WriteFlattenedObject(const void* p, size_t n) {
        Write<u32>(1);
        Write<s64>(static_cast<s64>(n));
        WriteBytes(p, n);
    }
    void WriteInterface(const void* p, size_t n) {
        WriteBytes(p, n);
        objects_ += 4;
    }
    std::vector<u8> Serialize() const {
        std::vector<u8> out(16 + data_.size() + objects_);
        const u32 header[4] = {static_cast<u32>(data_.size()), 16, objects_, static_cast<u32>(16 + data_.size())};
        std::memcpy(out.data(), header, 16);
        std::memcpy(out.data() + 16, data_.data(), data_.size());
        return out;
    }

private:
    std::vector<u8> data_;
    u32 objects_ = 0;
};

struct NvFence {
    s32 id;
    u32 value;
};
struct MultiFence {
    u32 num_fences;
    NvFence fences[4];
};
static_assert(sizeof(MultiFence) == 0x24);

MultiFence NoFence() {
    MultiFence f{};
    for (auto& x : f.fences) x.id = -1;
    return f;
}

// ------------------------------------------------------------ buffer queue

constexpr int kMaxSlots = 64;

void FlushPresentCsvAtExit();

class BufferQueue {
public:
    void Transact(u32 code, InParcel& in, OutParcel& out) {
        std::lock_guard lock(mutex_);
        s32 status = 0;
        switch (code) {
        case 1: { // RequestBuffer
            const s32 slot = in.Read<s32>();
            if (slot < 0 || slot >= kMaxSlots || slots_[slot].empty()) {
                out.Write<u32>(0);
            } else {
                out.WriteFlattenedObject(slots_[slot].data(), slots_[slot].size());
            }
            break;
        }
        case 2: // SetBufferCount
            in.Read<s32>();
            break;
        case 3: { // DequeueBuffer
            in.Read<u32>(); // async
            const u32 w = in.Read<u32>(), h = in.Read<u32>();
            (void)w;
            (void)h;
            s32 slot = -1;
            for (int i = 0; i < kMaxSlots; ++i) {
                const int s = (next_ + i) % kMaxSlots;
                if (!slots_[s].empty() && !dequeued_[s]) {
                    slot = s;
                    break;
                }
            }
            if (slot < 0) slot = 0;
            dequeued_[slot] = true;
            next_ = (slot + 1) % kMaxSlots;
            out.Write<s32>(slot);
            const MultiFence f = NoFence();
            out.WriteFlattenedObject(&f, sizeof(f));
            break;
        }
        case 7: { // QueueBuffer
            const s32 slot = in.Read<s32>();
            const std::vector<u8> qbi = in.ReadFlattened(); // QueueBufferInput
            NoteSwapInterval(qbi);
            if (slot >= 0 && slot < kMaxSlots) {
                dequeued_[slot] = false;
                if (!slots_[slot].empty()) Present(slots_[slot]);
            }
            PaceToVsync();
            WriteQueueOutput(out);
            break;
        }
        case 8: { // CancelBuffer
            const s32 slot = in.Read<s32>();
            if (slot >= 0 && slot < kMaxSlots) dequeued_[slot] = false;
            break;
        }
        case 9: { // Query
            const s32 what = in.Read<s32>();
            s32 value = 0;
            switch (what) {
            case 0: case 6: value = 1280; break; // width
            case 1: case 7: value = 720; break;  // height
            case 2: value = 1; break;            // RGBA8888
            case 3: value = 0; break;            // min undequeued
            default: value = 0; break;
            }
            out.Write<s32>(value);
            break;
        }
        case 10: // Connect
            WriteQueueOutput(out);
            break;
        case 11: // Disconnect
            break;
        case 14: { // SetPreallocatedBuffer
            const s32 slot = in.Read<s32>();
            std::vector<u8> buffer = in.ReadObject();
            if (slot >= 0 && slot < kMaxSlots) {
                slots_[slot] = std::move(buffer);
                LOG_INFO("vi: buffer slot %d preallocated (%zu bytes)", slot, slots_[slot].size());
            }
            break;
        }
        default:
            LOG_WARN("vi: binder transaction %u not implemented", code);
            break;
        }
        out.Write<s32>(status);
    }

private:
    // A queued frame is shown after swap_interval_ 60 Hz refreshes, so the
    // producer cannot run ahead of that rate.
    static bool LegacyVsync60() {
        static const bool legacy = [] {
            const char* e = std::getenv("NXRT_LEGACY_VSYNC_60");
            return e && e[0] == '1';
        }();
        return legacy;
    }
    void NoteSwapInterval(const std::vector<u8>& qbi) {
        if (LegacyVsync60()) return;
        const s32 v = ParseSwapInterval(qbi.data(), qbi.size());
        if (v == 0) {
            if (!warned_size_) {
                warned_size_ = true;
                LOG_WARN("vi: QueueBufferInput size %zu != 84, keeping swap_interval %d",
                         qbi.size(), swap_interval_);
            }
            return;
        }
        // Keep the last vsync as the baseline: the new period counts from it, and
        // PlanVsyncPacing already rebases when a frame is late, so no debt builds up.
        if (v != swap_interval_ && CounterTrace::Enabled()) {
            LOG_INFO("vi: swap decision frame=%llu %d->%d prev_frame_ms=%.2f (raw before sleep %.2f ms, slept=%d) prev2_frame_ms=%.2f%s",
                     static_cast<unsigned long long>(trace_frame_), swap_interval_, v, trace_total_ms_[0],
                     trace_raw_ms_[0], trace_slept_[0] ? 1 : 0, trace_total_ms_[1], CounterTrace::Get().Describe().c_str());
        }
        swap_interval_ = v;
    }
    void PaceToVsync() {
        using clock = std::chrono::steady_clock;
        // NXRT_SWAP_INTERVAL_MAX=<n> (diagnostic): caps the interval used for pacing only; the game still asks for its own.
        static const s32 interval_max = [] {
            const char* e = std::getenv("NXRT_SWAP_INTERVAL_MAX");
            const long n = e ? std::strtol(e, nullptr, 10) : 0;
            return n >= 1 && n <= 3 ? static_cast<s32>(n) : 3;
        }();
        const s32 interval = LegacyVsync60() ? 1 : std::min(swap_interval_, interval_max);
        const auto period = std::chrono::nanoseconds(kVsyncPeriodNs * interval);
        const auto pace_start = clock::now();
        const auto plan = PlanVsyncPacing(pace_start, next_vsync_, period);
        next_vsync_ = plan.next_vsync;
        if (plan.should_sleep) std::this_thread::sleep_until(next_vsync_);
        if (CounterTrace::Enabled()) {
            const auto end = clock::now();
            const auto ms = [](auto d) { return std::chrono::duration<double, std::milli>(d).count(); };
            trace_total_ms_[1] = trace_total_ms_[0];
            trace_total_ms_[0] = trace_last_end_.time_since_epoch().count() ? ms(end - trace_last_end_) : 0.0;
            trace_raw_ms_[0] = trace_last_end_.time_since_epoch().count() ? ms(pace_start - trace_last_end_) : 0.0;
            trace_slept_[0] = plan.should_sleep;
            trace_last_end_ = end;
            ++trace_frame_;
            CounterTrace::Get().Rotate();
        }
        if (PresentCsv()) RecordPresent(interval, plan.should_sleep);
        if (interval >= 1 && interval <= 3) hist_[interval - 1]++; else hist_[3]++;
        if (++hist_total_ >= 600) {
            LOG_INFO("vi: swap_interval hist 1=%u 2=%u 3=%u other=%u", hist_[0], hist_[1],
                     hist_[2], hist_[3]);
            hist_[0] = hist_[1] = hist_[2] = hist_[3] = 0;
            hist_total_ = 0;
        }
    }
    // Opt-in NXRT_PRESENT_CSV=<file>: one row per presentation, buffered and written in blocks.
    static std::FILE* PresentCsv() {
        static std::FILE* f = [] () -> std::FILE* {
            const char* path = std::getenv("NXRT_PRESENT_CSV");
            if (!path || !path[0]) return nullptr;
            std::FILE* fp = std::fopen(path, "w");
            if (!fp) {
                LOG_WARN("vi: cannot open NXRT_PRESENT_CSV %s", path);
                return nullptr;
            }
            std::fputs("present_index,t_ns_steady,interval_ns,swap_interval,slept\n", fp);
            std::atexit(&FlushPresentCsvAtExit);
            return fp;
        }();
        return f;
    }
    void RecordPresent(s32 interval, bool slept) {
        const long long t = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch()).count();
        char line[96];
        const int n = std::snprintf(line, sizeof(line), "%llu,%lld,%lld,%d,%d\n",
                                    static_cast<unsigned long long>(present_index_++), t,
                                    last_present_ns_ ? t - last_present_ns_ : 0LL, interval,
                                    slept ? 1 : 0);
        last_present_ns_ = t;
        if (n > 0) csv_buf_.append(line, static_cast<size_t>(n));
        if (++csv_rows_ >= 32) FlushPresentCsvLocked();
    }
    void FlushPresentCsvLocked() {
        if (std::FILE* f = PresentCsv(); f && !csv_buf_.empty()) {
            std::fwrite(csv_buf_.data(), 1, csv_buf_.size(), f);
            std::fflush(f);
        }
        csv_buf_.clear();
        csv_rows_ = 0;
    }

public:
    void FlushPresentCsv() {
        std::lock_guard lock(mutex_);
        FlushPresentCsvLocked();
    }

private:
    std::string csv_buf_;
    u32 csv_rows_ = 0;
    u64 present_index_ = 0;
    long long last_present_ns_ = 0;
    s32 swap_interval_ = 1;
    std::chrono::steady_clock::time_point trace_last_end_{};
    double trace_total_ms_[2]{}, trace_raw_ms_[1]{};
    bool trace_slept_[1]{};
    u64 trace_frame_ = 0;
    bool warned_size_ = false;
    u32 hist_[4]{};
    u32 hist_total_ = 0;
    std::chrono::steady_clock::time_point next_vsync_{};

    void WriteQueueOutput(OutParcel& out) {
        const u32 output[4] = {1280, 720, 0, 0};
        out.WriteBytes(output, sizeof(output));
    }
    void Present(const std::vector<u8>& buffer) {
        PresentedBuffer pb{};
        if (buffer.size() >= 0x16C) {
            std::memcpy(&pb.width, buffer.data() + 4, 4);
            std::memcpy(&pb.height, buffer.data() + 8, 4);
            std::memcpy(&pb.stride, buffer.data() + 12, 4);
            std::memcpy(&pb.format, buffer.data() + 16, 4);
            std::memcpy(&pb.nvmap_handle, buffer.data() + 0x74, 4);
            std::memcpy(&pb.offset, buffer.data() + 0x78, 4);
        }
        OnPresent(pb);
    }

    std::mutex mutex_;
    std::vector<u8> slots_[kMaxSlots];
    bool dequeued_[kMaxSlots] = {};
    int next_ = 0;
};

BufferQueue& Queue() {
    static BufferQueue q;
    return q;
}

void FlushPresentCsvAtExit() { Queue().FlushPresentCsv(); }

std::shared_ptr<KEvent>& BufferEvent() {
    static auto e = MakeEvent("vi:buffer-available", true);
    return e;
}

// Signals vsync at 60 Hz for as long as the process lives.
std::shared_ptr<KEvent> VsyncEvent() {
    static std::shared_ptr<KEvent> event = [] {
        auto e = MakeEvent("vi:vsync");
        std::thread([e] {
            auto next = std::chrono::steady_clock::now();
            for (;;) {
                next += std::chrono::microseconds(16667);
                std::this_thread::sleep_until(next);
                e->Signal();
            }
        }).detach();
        return e;
    }();
    return event;
}

class IHOSBinderDriver final : public ServiceObject {
public:
    IHOSBinderDriver() : ServiceObject("IHOSBinderDriver") {
        auto transact = [](Ctx& ctx) {
            ctx.Pop<s32>(); // binder id
            const u32 code = ctx.Pop<u32>();
            InParcel in(ctx.ReadBuffer(0));
            OutParcel out;
            Queue().Transact(code, in, out);
            const std::vector<u8> reply = out.Serialize();
            ctx.WriteBuffer(reply.data(), reply.size(), 0);
        };
        Register(0, "TransactParcel", transact);
        RegisterStub(1, "AdjustRefcount");
        Register(2, "GetNativeHandle", [](Ctx& ctx) { ctx.PushCopyObject(BufferEvent()); });
        Register(3, "TransactParcelAuto", transact);
    }
};

class ISystemDisplayService final : public ServiceObject {
public:
    ISystemDisplayService() : ServiceObject("ISystemDisplayService") {
        RegisterStub(2205, "SetLayerZ");
        RegisterStub(2207, "SetLayerVisibility");
    }
};

class IManagerDisplayService final : public ServiceObject {
public:
    IManagerDisplayService() : ServiceObject("IManagerDisplayService") {
        Register(2010, "CreateManagedLayer", [](Ctx& ctx) { ctx.Push<u64>(1); });
        RegisterStub(2011, "DestroyManagedLayer");
        RegisterStub(6000, "AddToLayerStack");
        RegisterStub(6002, "SetLayerVisibility");
    }
};

struct NativeWindow {
    u32 magic = 2;
    u32 process_id = 1;
    u64 id = 1;
    u32 pad[2] = {};
    char dispdrv[8] = {'d', 'i', 's', 'p', 'd', 'r', 'v', 0};
    u32 pad2[2] = {};
};
static_assert(sizeof(NativeWindow) == 0x28);

class IApplicationDisplayService final : public ServiceObject {
public:
    IApplicationDisplayService() : ServiceObject("IApplicationDisplayService") {
        Register(100, "GetRelayService", [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IHOSBinderDriver>()); });
        Register(101, "GetSystemDisplayService",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<ISystemDisplayService>()); });
        Register(102, "GetManagerDisplayService",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IManagerDisplayService>()); });
        Register(103, "GetIndirectDisplayTransactionService",
                 [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IHOSBinderDriver>()); });
        Register(1000, "ListDisplays", [](Ctx& ctx) {
            u8 info[0x60] = {};
            std::strcpy(reinterpret_cast<char*>(info), "Default");
            info[0x40] = 1;               // has limited layers
            const u64 max_layers = 1;
            const u64 w = 1920, h = 1080;
            std::memcpy(info + 0x48, &max_layers, 8);
            std::memcpy(info + 0x50, &w, 8);
            std::memcpy(info + 0x58, &h, 8);
            ctx.WriteBuffer(info, sizeof(info));
            ctx.Push<u64>(1);
        });
        Register(1010, "OpenDisplay", [](Ctx& ctx) { ctx.Push<u64>(1); });
        Register(1011, "OpenDefaultDisplay", [](Ctx& ctx) { ctx.Push<u64>(1); });
        RegisterStub(1020, "CloseDisplay");
        RegisterStub(1101, "SetDisplayEnabled");
        Register(1102, "GetDisplayResolution", [](Ctx& ctx) {
            ctx.Push<s64>(1920);
            ctx.Push<s64>(1080);
        });
        Register(2020, "OpenLayer", [](Ctx& ctx) {
            OutParcel p;
            const NativeWindow w;
            p.WriteInterface(&w, sizeof(w));
            const std::vector<u8> parcel = p.Serialize();
            ctx.WriteBuffer(parcel.data(), parcel.size());
            ctx.Push<u64>(parcel.size());
        });
        RegisterStub(2021, "CloseLayer");
        Register(2030, "CreateStrayLayer", [](Ctx& ctx) {
            OutParcel p;
            const NativeWindow w;
            p.WriteInterface(&w, sizeof(w));
            const std::vector<u8> parcel = p.Serialize();
            ctx.WriteBuffer(parcel.data(), parcel.size());
            ctx.Push<u64>(2);
            ctx.Push<u64>(parcel.size());
        });
        RegisterStub(2031, "DestroyStrayLayer");
        RegisterStub(2101, "SetLayerScalingMode");
        Register(2102, "ConvertScalingMode", [](Ctx& ctx) {
            const u32 mode = ctx.Pop<u32>();
            static const u64 map[] = {0, 1, 2, 3, 4};
            ctx.Push<u64>(mode < 5 ? map[mode] : 2);
        });
        Register(2450, "GetIndirectLayerImageMap", [](Ctx& ctx) {
            ctx.Push<u64>(0);
            ctx.Push<u64>(0);
        });
        Register(2460, "GetIndirectLayerImageRequiredMemoryInfo", [](Ctx& ctx) {
            const s64 w = ctx.Pop<s64>(), h = ctx.Pop<s64>();
            const s64 size = (w * h * 4 + 0x1FFFF) / 0x20000 * 0x20000;
            ctx.Push<s64>(size);
            ctx.Push<s64>(0x1000);
        });
        Register(5202, "GetDisplayVsyncEvent", [](Ctx& ctx) { ctx.PushCopyObject(VsyncEvent()); });
    }
};

class ViRoot final : public ServiceObject {
public:
    explicit ViRoot(const char* name) : ServiceObject(name) {
        auto get = [](Ctx& ctx) { ctx.PushInterface(std::make_shared<IApplicationDisplayService>()); };
        Register(0, "GetDisplayService", get);
        Register(1, "GetDisplayServiceWithProxyNameExchange", get);
        Register(2, "GetDisplayService", get);
        Register(3, "GetDisplayService", get);
    }
};

std::atomic<u64> g_frames{0};

} // namespace

void OnPresent(const PresentedBuffer& buffer) {
    frontend::NotifyFrame();
    const u64 n = ++g_frames;
    static auto last = std::chrono::steady_clock::now();
    static u64 last_n = 0;
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(5)) {
        const double fps = (n - last_n) / std::chrono::duration<double>(now - last).count();
        LOG_INFO("vi: %llu frames presented (%.1f fps), %ux%u format %u", static_cast<unsigned long long>(n), fps,
                 buffer.width, buffer.height, buffer.format);
        last = now;
        last_n = n;
    }
}

void RegisterViServices() {
    for (const char* name : {"vi:m", "vi:s", "vi:u"}) {
        RegisterService(name, [name] { return std::make_shared<ViRoot>(name); });
    }
}

} // namespace nxrt::hle
