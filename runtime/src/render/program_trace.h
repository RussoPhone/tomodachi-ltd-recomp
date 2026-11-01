// Opt-in per-dispatch trace of one compute program (NXRT_TRACE_PROGRAM=<hex program>[,<file.csv|dir>]).
// One "D" row per dispatch (inputs, SSBO range before and after the write-back), plus "W" rows when the guest changed
// the SSBO range between two dispatches and "R" rows when the guest read it (needs the guest-memory read observer).
// Without the variable nothing is allocated and every hook is a single branch. Pure logic: no renderer/guest-memory
// dependency, so it is unit tested on its own.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace nxrt {

inline uint64_t ProgramTraceHash(const void* data, size_t size) {
    uint64_t h = 1469598103934665603ULL;
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 1099511628211ULL;
    return h;
}

class ProgramTrace {
public:
    struct Dispatch {
        uint64_t frame = 0, index = 0, program = 0;
        uint64_t c3_hash = 0;
        uint32_t c3_43z = 0;
        uint32_t c0_49[4] = {};
        uint64_t ssbo_va = 0, ssbo_cpu = 0, ssbo_size = 0;
    };

    static constexpr size_t kOffset14C0 = 0x14C0;

    ProgramTrace() = default;
    ProgramTrace(uint64_t program, const std::string& path) { Open(program, path); }
    ~ProgramTrace() { Close(); }

    static ProgramTrace& Instance() {
        static ProgramTrace trace;
        static const bool opened = [] {
            uint64_t program = 0;
            std::string path;
            if (!ParseSpec(std::getenv("NXRT_TRACE_PROGRAM"), std::getenv("NXRT_DRAW_CENSUS"), program, path)) return false;
            return trace.Open(program, path);
        }();
        (void)opened;
        return trace;
    }
    bool Enabled() const { return file_ != nullptr; }
    uint64_t Program() const { return program_; }

    // "2044e14ad8" or "2044e14ad8,/dir/or/file.csv". census = NXRT_DRAW_CENSUS value (may be null).
    static bool ParseSpec(const char* spec, const char* census, uint64_t& program, std::string& path) {
        if (!spec || !*spec) return false;
        const std::string text(spec);
        const size_t comma = text.find(',');
        char* end = nullptr;
        const std::string hex = text.substr(0, comma);
        program = std::strtoull(hex.c_str(), &end, 16);
        if (!program || end == hex.c_str() || *end) return false;
        std::string dest = comma == std::string::npos ? "" : text.substr(comma + 1);
        if (dest.empty()) dest = census && *census ? std::string(census) + ".program.csv" : "/tmp/trace-program.csv";
        else if (dest.size() < 4 || dest.compare(dest.size() - 4, 4, ".csv") != 0)
            dest += (dest.back() == '/' ? "" : "/") + std::string("program.csv");
        path = dest;
        return true;
    }

    bool Open(uint64_t program, const std::string& path) {
        program_ = program;
        file_ = std::fopen(path.c_str(), "w");
        if (!file_) return false;
        std::fputs("tipo,frame,dispatch_idx,c3_hash,c3_43z,c0_49,ssbo_va,ssbo_size,first16_antes,u32_14C0_antes,"
                   "u32_14C0_depois,first16_depois\n"
                   "# R/W: tipo,frame,evento,offset,tamanho,valor\n",
                   file_);
        std::fflush(file_);
        return true;
    }

    void SetFrame(uint64_t frame) { frame_.store(frame, std::memory_order_relaxed); }

    // Called with the guest bytes of the SSBO range the dispatch is about to read (the "before" image).
    void Begin(const Dispatch& d, const uint8_t* before, size_t size) {
        if (!file_) return;
        std::lock_guard lock(mutex_);
        SetFrame(d.frame);
        if (have_after_ && after_cpu_ == d.ssbo_cpu && after_.size() == size) {
            for (size_t i = 0; i < size; ++i) {
                if (before[i] == after_[i]) continue;
                size_t last = i;
                for (size_t j = i; j < size; ++j) if (before[j] != after_[j]) last = j;
                EmitEvent(d.frame, "guest_write", i, last - i + 1, U32At(before, size, i & ~size_t{3}));
                break;
            }
        }
        watch_cpu_.store(d.ssbo_cpu, std::memory_order_relaxed);
        watch_size_.store(d.ssbo_size, std::memory_order_relaxed);
        Pending p;
        p.d = d;
        p.first16 = Hex16(before, size);
        p.v14c0 = U32Text(before, size, kOffset14C0);
        if (pending_.size() >= 8) FlushOldestLocked();
        pending_[d.index] = std::move(p);
    }

    // Called when the write-back of dispatch `index` was copied to guest memory.
    void Writeback(uint64_t index, uint64_t cpu, const uint8_t* data, size_t size) {
        if (!file_) return;
        std::lock_guard lock(mutex_);
        after_.assign(data, data + size);
        after_cpu_ = cpu;
        have_after_ = true;
        auto it = pending_.find(index);
        if (it == pending_.end()) return;
        EmitDispatch(it->second, U32Text(data, size, kOffset14C0), Hex16(data, size));
        pending_.erase(it);
    }

    // Guest read observer: any read overlapping the SSBO range of the traced program.
    void OnGuestRead(uint64_t address, uint64_t size) {
        if (!file_ || !size) return;
        const uint64_t base = watch_cpu_.load(std::memory_order_relaxed);
        const uint64_t span = watch_size_.load(std::memory_order_relaxed);
        if (!span || address + size <= base || address >= base + span) return;
        std::lock_guard lock(mutex_);
        const uint64_t frame = frame_.load(std::memory_order_relaxed);
        const uint64_t from = std::max(address, base) - base;
        const uint64_t len = std::min(address + size, base + span) - std::max(address, base);
        if (frame != read_frame_) { read_frame_ = frame; read_seen_.clear(); }
        if (read_seen_.size() >= 32 || !read_seen_.insert({from, len}).second) return;
        EmitEvent(frame, "guest_read", from, len, have_after_ ? U32At(after_.data(), after_.size(), from & ~uint64_t{3}) : 0);
    }

    void Close() {
        if (!file_) return;
        std::lock_guard lock(mutex_);
        while (!pending_.empty()) FlushOldestLocked();
        std::fclose(file_);
        file_ = nullptr;
    }

private:
    struct Pending {
        Dispatch d;
        std::string first16, v14c0;
    };

    static uint32_t U32At(const uint8_t* p, size_t size, size_t off) {
        uint32_t v = 0;
        if (off + 4 <= size) std::memcpy(&v, p + off, 4);
        return v;
    }
    static std::string U32Text(const uint8_t* p, size_t size, size_t off) {
        if (off + 4 > size) return "-";
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08x", U32At(p, size, off));
        return buf;
    }
    static std::string Hex16(const uint8_t* p, size_t size) {
        std::string out;
        char buf[4];
        for (size_t i = 0; i < std::min<size_t>(16, size); ++i) {
            std::snprintf(buf, sizeof buf, "%02x", p[i]);
            out += buf;
        }
        return out.empty() ? "-" : out;
    }
    void FlushOldestLocked() {
        auto it = pending_.begin();
        EmitDispatch(it->second, "-", "-");
        pending_.erase(it);
    }
    void EmitDispatch(const Pending& p, const std::string& v14c0_after, const std::string& first16_after) {
        const Dispatch& d = p.d;
        std::fprintf(file_, "D,%llu,%llu,%016llx,%08x,%08x:%08x:%08x:%08x,%llx,%llu,%s,%s,%s,%s\n",
                     (unsigned long long)d.frame, (unsigned long long)d.index, (unsigned long long)d.c3_hash, d.c3_43z,
                     d.c0_49[0], d.c0_49[1], d.c0_49[2], d.c0_49[3], (unsigned long long)d.ssbo_va,
                     (unsigned long long)d.ssbo_size, p.first16.c_str(), p.v14c0.c_str(), v14c0_after.c_str(),
                     first16_after.c_str());
        Tick();
    }
    void EmitEvent(uint64_t frame, const char* name, uint64_t offset, uint64_t size, uint32_t value) {
        std::fprintf(file_, "E,%llu,%s,%llx,%llu,%08x\n", (unsigned long long)frame, name, (unsigned long long)offset,
                     (unsigned long long)size, value);
        Tick();
    }
    void Tick() {
        if (++lines_ % 16 == 0) std::fflush(file_);
    }

    FILE* file_ = nullptr;
    uint64_t program_ = 0;
    std::mutex mutex_;
    std::map<uint64_t, Pending> pending_;
    std::vector<uint8_t> after_;
    uint64_t after_cpu_ = 0, read_frame_ = ~uint64_t{0}, lines_ = 0;
    bool have_after_ = false;
    std::set<std::pair<uint64_t, uint64_t>> read_seen_;
    std::atomic<uint64_t> frame_{0}, watch_cpu_{0}, watch_size_{0};
};

} // namespace nxrt
