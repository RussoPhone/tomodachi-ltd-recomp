#pragma once
// Diagnostic only (NXRT_TRACE_SWAP_DECISION=1): remembers what the renderer wrote for each NVN counter type in the last
// two presented frames, so the VI service can log them when the game changes its swap interval.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#include "common.h"

namespace nxrt {

class CounterTrace {
public:
    static bool Enabled() {
        static const bool on = [] {
            const char* e = std::getenv("NXRT_TRACE_SWAP_DECISION");
            return e && e[0] == '1';
        }();
        return on;
    }
    static CounterTrace& Get() {
        static CounterTrace t;
        return t;
    }
    void Record(u64 type, u64 value) {
        if (type >= kTypes) type = kTypes - 1;
        std::lock_guard lock(mutex_);
        Slot& s = frames_[0][type];
        if (s.n++ == 0) s.first = value;
        s.last = value;
    }
    // Called once per presented frame by the VI pacing: current frame becomes "previous".
    void Rotate() {
        std::lock_guard lock(mutex_);
        frames_[2] = frames_[1];
        frames_[1] = frames_[0];
        frames_[0] = {};
    }
    std::string Describe() {
        std::lock_guard lock(mutex_);
        std::string out;
        char buf[128];
        for (int f = 1; f <= 2; ++f) {
            for (size_t t = 0; t < kTypes; ++t) {
                const Slot& s = frames_[f][t];
                if (!s.n) continue;
                std::snprintf(buf, sizeof(buf), " [prev%d type%zu n=%u first=%llu last=%llu delta=%lld]", f, t, s.n,
                              static_cast<unsigned long long>(s.first), static_cast<unsigned long long>(s.last),
                              static_cast<long long>(s.last - s.first));
                out += buf;
            }
        }
        return out;
    }

private:
    static constexpr size_t kTypes = 32;
    struct Slot {
        u32 n = 0;
        u64 first = 0, last = 0;
    };
    std::mutex mutex_;
    std::array<std::array<Slot, kTypes>, 3> frames_{};
};

} // namespace nxrt
