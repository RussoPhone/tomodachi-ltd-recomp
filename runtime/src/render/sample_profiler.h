// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Diagnostic only (NXRT_PROF_SAMPLER=<file>, NXRT_PROF_FROM/NXRT_PROF_TO = present counts): a SIGPROF flat profile of
// the instruction pointers of the threads that are inside the renderer (Execute/Present). The file holds the load
// base and one hex address per sample; resolve with `addr2line -f -i -C -e nxrt`. Linux x86-64 only; elsewhere a no-op.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__linux__) && defined(__x86_64__)
#include <execinfo.h>
#include <link.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>
#define NXRT_SAMPLE_PROFILER 1
#endif

namespace nxrt::render {

#ifdef NXRT_SAMPLE_PROFILER
inline thread_local int t_sampler_in_renderer = 0;

class SampleProfiler {
public:
    static SampleProfiler& Get() {
        static SampleProfiler p;
        return p;
    }
    bool enabled() const { return path_ != nullptr; }
    void Tick(std::uint64_t frame) {
        if (!path_) return;
        if (!running_ && !done_ && frame >= from_) Start();
        if (running_ && frame >= to_) Stop();
    }

private:
    SampleProfiler() {
        path_ = std::getenv("NXRT_PROF_SAMPLER");
        if (path_ && !*path_) path_ = nullptr;
        if (const char* v = std::getenv("NXRT_PROF_FROM")) from_ = std::strtoull(v, nullptr, 10);
        if (const char* v = std::getenv("NXRT_PROF_TO")) to_ = std::strtoull(v, nullptr, 10);
        if (const char* v = std::getenv("NXRT_PROF_STACK"); v && *v == '1') {
            stacks_ = new std::uint64_t[kMaxStacks * kDepth]();
            void* warm[kDepth];
            backtrace(warm, kDepth);  // load libgcc's unwinder outside the signal handler
        }
    }
    static void Handler(int, siginfo_t*, void* uc) {
        SampleProfiler& p = Get();
        if (!t_sampler_in_renderer) return;
        const std::uint64_t i = p.count_.fetch_add(1, std::memory_order_relaxed);
        if (i < kMax) p.ips_[i] = static_cast<std::uint64_t>(static_cast<ucontext_t*>(uc)->uc_mcontext.gregs[REG_RIP]);
        // NXRT_PROF_STACK=1: also keep the caller chain (glibc backtrace(), diagnostic only, not signal safe).
        if (p.stacks_ && i < kMaxStacks) {
            void* frames[kDepth];
            const int n = backtrace(frames, kDepth);
            for (int k = 0; k < kDepth; ++k) p.stacks_[i * kDepth + k] = k < n ? reinterpret_cast<std::uint64_t>(frames[k]) : 0;
        }
    }
    void Start() {
        struct sigaction sa{};
        sa.sa_sigaction = &Handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&sa.sa_mask);
        sigaction(SIGPROF, &sa, nullptr);
        itimerval tv{{0, 500}, {0, 500}};
        setitimer(ITIMER_PROF, &tv, nullptr);
        running_ = true;
    }
    void Stop() {
        itimerval tv{};
        setitimer(ITIMER_PROF, &tv, nullptr);
        running_ = false;
        done_ = true;
        std::FILE* f = std::fopen(path_, "w");
        if (!f) return;
        std::uint64_t base = 0;
        dl_iterate_phdr([](dl_phdr_info* info, size_t, void* out) {
            *static_cast<std::uint64_t*>(out) = info->dlpi_addr;
            return 1;
        }, &base);
        std::fprintf(f, "base=0x%llx\n", static_cast<unsigned long long>(base));
        const std::uint64_t n = count_.load() < kMax ? count_.load() : kMax;
        for (std::uint64_t i = 0; i < n; ++i) std::fprintf(f, "%llx\n", static_cast<unsigned long long>(ips_[i]));
        if (std::FILE* m = std::fopen("/proc/self/maps", "r")) {
            char line[512];
            while (std::fgets(line, sizeof line, m)) {
                if (std::strstr(line, " r-xp ")) std::fprintf(f, "map %s", line);
            }
            std::fclose(m);
        }
        std::fclose(f);
        if (stacks_) {  // <path>.stk: one line per sample, "ip0 ip1 ..." (leaf first), same load base as the main file
            if (std::FILE* g = std::fopen((std::string(path_) + ".stk").c_str(), "w")) {
                for (std::uint64_t i = 0; i < n && i < kMaxStacks; ++i) {
                    for (int k = 0; k < kDepth && stacks_[i * kDepth + k]; ++k)
                        std::fprintf(g, "%s%llx", k ? " " : "", static_cast<unsigned long long>(stacks_[i * kDepth + k]));
                    std::fprintf(g, "\n");
                }
                std::fclose(g);
            }
        }
    }

    static constexpr std::uint64_t kMaxStacks = 1u << 17;
    static constexpr int kDepth = 14;
    std::uint64_t* stacks_ = nullptr;
    static constexpr std::uint64_t kMax = 1u << 21;
    const char* path_ = nullptr;
    std::uint64_t from_ = 28700, to_ = 30780;
    bool running_ = false, done_ = false;
    std::atomic<std::uint64_t> count_{0};
    std::uint64_t ips_[kMax];
};

struct SamplerScope {
    SamplerScope() { ++t_sampler_in_renderer; }
    ~SamplerScope() { --t_sampler_in_renderer; }
};
#else
class SampleProfiler {
public:
    static SampleProfiler& Get() {
        static SampleProfiler p;
        return p;
    }
    void Tick(std::uint64_t) {}
};
struct SamplerScope {};
#endif

}  // namespace nxrt::render
