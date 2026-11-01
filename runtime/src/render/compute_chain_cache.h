// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Persistent record of a long run of consecutive compute dispatches (the Mii face-paint editor opens one: ~142k tiny
// dispatches in a single frame). The run is a deterministic function of what the guest recorded: the same dispatch
// sequence on the same input gives a result that is interchangeable with a fresh GPU result (the GPU's own output of
// those buffers is not bit-reproducible, and the picture and the save are identical either way). An entry stores the
// fingerprint of every dispatch and the final contents of every buffer the run wrote. A later run whose first
// dispatches match the entry executes only that prefix, checks the fingerprint of each following dispatch without
// executing it, and writes the stored buffers when the last one arrives.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace nxrt::render {

struct ChainRange {
    std::uint64_t address = 0;
    std::vector<std::uint8_t> bytes;
};

// A stretch of guest memory the run read (or may have read) in the state it was in when the run first touched it.
struct ChainInput {
    std::uint64_t address = 0, size = 0, hash = 0;
    std::uint64_t first_dispatch = 0;  // index of the dispatch that first touched it
    // bit 0: first touched by a binding the dispatch writes (it may hold leftovers, not input)
    // bit 1: volatile - observed to differ between two recordings of the same command sequence while first touched by a
    //        written binding; its content is ignored when matching. Read-only stretches are never volatile.
    std::uint64_t flags = 0;
};
constexpr std::uint64_t kChainInputWritten = 1, kChainInputVolatile = 2;

struct ChainEntry {
    std::uint64_t key = 0;
    std::uint64_t count = 0;
    std::vector<std::uint64_t> fingerprints;
    std::vector<ChainRange> ranges;
    std::vector<ChainInput> inputs;
};

inline std::string ChainCacheDir() {
    const char* xdg = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    std::string dir = xdg && *xdg ? std::string(xdg) : (home && *home ? std::string(home) + "/.cache" : "/tmp");
    return dir + "/nxrt/chain";
}

inline std::string ChainCachePath(std::uint64_t key) {
    char name[48];
    std::snprintf(name, sizeof name, "/%016llx.bin", static_cast<unsigned long long>(key));
    return ChainCacheDir() + name;
}

constexpr std::uint32_t kChainCacheMagic = 0x4343584e;  // "NXCC"
constexpr std::uint32_t kChainCacheVersion = 4;

inline bool LoadChainEntry(std::uint64_t key, ChainEntry& out) {
    std::ifstream in(ChainCachePath(key), std::ios::binary);
    if (!in) return false;
    std::uint32_t magic = 0, version = 0;
    std::uint64_t count = 0, nranges = 0, stored_key = 0;
    in.read(reinterpret_cast<char*>(&magic), 4);
    in.read(reinterpret_cast<char*>(&version), 4);
    in.read(reinterpret_cast<char*>(&stored_key), 8);
    in.read(reinterpret_cast<char*>(&count), 8);
    if (!in || magic != kChainCacheMagic || version != kChainCacheVersion || stored_key != key || count == 0 ||
        count > (std::uint64_t{1} << 24)) {
        return false;
    }
    out = ChainEntry{};
    out.key = key;
    out.count = count;
    out.fingerprints.resize(count);
    in.read(reinterpret_cast<char*>(out.fingerprints.data()), static_cast<std::streamsize>(count * 8));
    std::uint64_t ninputs = 0;
    in.read(reinterpret_cast<char*>(&ninputs), 8);
    if (!in || ninputs > (std::uint64_t{1} << 20)) return false;
    out.inputs.resize(ninputs);
    in.read(reinterpret_cast<char*>(out.inputs.data()), static_cast<std::streamsize>(ninputs * sizeof(ChainInput)));
    in.read(reinterpret_cast<char*>(&nranges), 8);
    if (!in || nranges > 4096) return false;
    out.ranges.resize(nranges);
    for (ChainRange& r : out.ranges) {
        std::uint64_t size = 0;
        in.read(reinterpret_cast<char*>(&r.address), 8);
        in.read(reinterpret_cast<char*>(&size), 8);
        if (!in || size > (std::uint64_t{64} << 20)) return false;
        r.bytes.resize(size);
        in.read(reinterpret_cast<char*>(r.bytes.data()), static_cast<std::streamsize>(size));
    }
    return static_cast<bool>(in);
}

inline bool SaveChainEntry(const ChainEntry& e) {
    std::error_code ec;
    std::filesystem::create_directories(ChainCacheDir(), ec);
    const std::string path = ChainCachePath(e.key);
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        const std::uint64_t count = e.fingerprints.size(), nranges = e.ranges.size();
        out.write(reinterpret_cast<const char*>(&kChainCacheMagic), 4);
        out.write(reinterpret_cast<const char*>(&kChainCacheVersion), 4);
        out.write(reinterpret_cast<const char*>(&e.key), 8);
        out.write(reinterpret_cast<const char*>(&count), 8);
        out.write(reinterpret_cast<const char*>(e.fingerprints.data()), static_cast<std::streamsize>(count * 8));
        const std::uint64_t ninputs = e.inputs.size();
        out.write(reinterpret_cast<const char*>(&ninputs), 8);
        out.write(reinterpret_cast<const char*>(e.inputs.data()), static_cast<std::streamsize>(ninputs * sizeof(ChainInput)));
        out.write(reinterpret_cast<const char*>(&nranges), 8);
        for (const ChainRange& r : e.ranges) {
            const std::uint64_t size = r.bytes.size();
            out.write(reinterpret_cast<const char*>(&r.address), 8);
            out.write(reinterpret_cast<const char*>(&size), 8);
            out.write(reinterpret_cast<const char*>(r.bytes.data()), static_cast<std::streamsize>(size));
        }
        if (!out) return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) return false;
    // Keep the newest few entries.
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (const auto& f : std::filesystem::directory_iterator(ChainCacheDir(), ec)) {
        if (f.path().extension() == ".bin") files.emplace_back(f.last_write_time(ec), f.path());
    }
    std::sort(files.begin(), files.end());
    while (files.size() > 8) {
        std::filesystem::remove(files.front().second, ec);
        files.erase(files.begin());
    }
    return true;
}

inline void RemoveChainEntry(std::uint64_t key) {
    std::error_code ec;
    std::filesystem::remove(ChainCachePath(key), ec);
}

}  // namespace nxrt::render
