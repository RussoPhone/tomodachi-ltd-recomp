// SPDX-License-Identifier: GPL-3.0-or-later
#include "guest_sampler.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "cpu.h"
#include "kernel/kernel.h"

namespace nxrt::guest_sampler {

namespace {
void Write(const std::string& path, const std::map<std::string, unsigned long>& incl,
           const std::map<std::string, unsigned long>& top, const std::map<std::string, unsigned long>& stacks, unsigned long samples) {
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "samples %lu\n", samples);
    auto dump = [&](const char* title, const std::map<std::string, unsigned long>& m) {
        std::vector<std::pair<std::string, unsigned long>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        std::fprintf(f, "== %s\n", title);
        for (size_t i = 0; i < v.size() && i < 400; ++i)
            std::fprintf(f, "%8lu %s\n", v[i].second, v[i].first.c_str());
    };
    dump("inclusive (thread:frame)", incl);
    dump("leaf (thread:pc)", top);
    dump("stacks (thread:leaf pc <- ...)", stacks);
    std::fclose(f);
}
} // namespace

void Start() {
    const char* path = std::getenv("NXRT_GUEST_SAMPLE");
    if (!path || !*path) return;
    std::set<u64> tids;
    if (const char* e = std::getenv("NXRT_GUEST_SAMPLE_TIDS")) {
        std::stringstream ss(e);
        for (std::string tok; std::getline(ss, tok, ',');) tids.insert(std::strtoull(tok.c_str(), nullptr, 10));
    }
    const long us = std::getenv("NXRT_GUEST_SAMPLE_US") ? std::strtol(std::getenv("NXRT_GUEST_SAMPLE_US"), nullptr, 10) : 2000;
    const double from = std::getenv("NXRT_GUEST_SAMPLE_FROM_S") ? std::atof(std::getenv("NXRT_GUEST_SAMPLE_FROM_S")) : 0.0;
    const double to = std::getenv("NXRT_GUEST_SAMPLE_TO_S") ? std::atof(std::getenv("NXRT_GUEST_SAMPLE_TO_S")) : 1e9;
    const std::string out = path;
    std::thread([=] {
        std::map<std::string, unsigned long> incl, top, stacks;
        unsigned long samples = 0;
        const auto t0 = std::chrono::steady_clock::now();
        auto last = t0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::microseconds(us > 0 ? us : 2000));
            const auto now = std::chrono::steady_clock::now();
            const double s = std::chrono::duration<double>(now - t0).count();
            if (s >= from && s <= to) {
                for (const auto& t : g_kernel.threads) {
                    if (!t->started || t->exited) continue;
                    if (!tids.empty() && !tids.count(t->id)) continue;
                    if (!t->wait_what.empty()) continue; // blocked: not running guest code
                    const GuestContext c = t->ctx;
                    std::string bt = Backtrace(c, 24);
                    const std::string id = std::to_string(t->id) + ":";
                    top[id + Symbolize(c.pc)]++;
                    stacks[id + Symbolize(c.pc) + " <- " + bt]++;
                    std::set<std::string> seen;
                    std::stringstream ss(bt);
                    std::string tok;
                    while (ss >> tok) {
                        if (tok == "<-") continue;
                        if (seen.insert(tok).second) incl[id + tok]++;
                    }
                    ++samples;
                }
            }
            if (std::chrono::duration<double>(now - last).count() > 5.0) {
                last = now;
                Write(out, incl, top, stacks, samples);
            }
        }
    }).detach();
}

} // namespace nxrt::guest_sampler
