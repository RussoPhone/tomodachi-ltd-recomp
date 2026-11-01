// SPDX-License-Identifier: GPL-3.0-or-later
// NXRT_TRACE_TTS=1: opt-in, log-only diagnostics for the game's TTS (VoiceText) wait. It groups
// three traces: per-file romfs/save read counters, per-service/command IPC rates, and the state
// of every guest thread, all summarised every 5 s on stderr (lines start with "tts-").
// Nothing here changes guest-visible behaviour. The data structures below are plain C++ so they
// can be unit tested without the runtime.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace nxrt {
class KThread;
}

namespace nxrt::tts_trace {

// ---------------------------------------------------------------- pure helpers

// string -> count within a window; TakeTop returns the n biggest and clears the window.
class Counter {
public:
    void Add(const std::string& key, std::uint64_t n = 1) { counts_[key] += n; }
    std::vector<std::pair<std::string, std::uint64_t>> TakeTop(std::size_t n) {
        std::vector<std::pair<std::string, std::uint64_t>> all(counts_.begin(), counts_.end());
        counts_.clear();
        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) {
            return a.second != b.second ? a.second > b.second : a.first < b.first;
        });
        if (all.size() > n) all.resize(n);
        return all;
    }

private:
    std::map<std::string, std::uint64_t> counts_;
};

constexpr int kHistBuckets = 8; // <=512, <=4K, <=32K, <=256K, <=1M, <=4M, <=16M, >16M
inline int SizeBucket(std::uint64_t bytes) {
    static constexpr std::uint64_t limits[kHistBuckets - 1] = {512, 4096, 32768, 262144, 1u << 20, 4u << 20, 16u << 20};
    for (int i = 0; i < kHistBuckets - 1; ++i)
        if (bytes <= limits[i]) return i;
    return kHistBuckets - 1;
}

struct FileWindow {
    std::uint64_t reads = 0, bytes = 0, ns_total = 0, ns_max = 0;
    std::int64_t off_min = -1, off_max = -1; // first byte read / one past the last byte read
    std::uint64_t hist[kHistBuckets] = {};
    std::uint64_t cum_reads = 0, cum_bytes = 0, cum_ns = 0; // lifetime totals (not reset per window)
};

class FsStats {
public:
    // Returns true the first time `key` is seen (so the caller can print fs-open).
    bool Record(const std::string& key, std::int64_t offset, std::uint64_t bytes, std::uint64_t ns) {
        const bool first = files_.find(key) == files_.end();
        FileWindow& w = files_[key];
        ++w.reads;
        w.bytes += bytes;
        w.ns_total += ns;
        w.ns_max = std::max(w.ns_max, ns);
        const std::int64_t end = offset + static_cast<std::int64_t>(bytes);
        if (w.off_min < 0 || offset < w.off_min) w.off_min = offset;
        if (end > w.off_max) w.off_max = end;
        ++w.hist[SizeBucket(bytes)];
        ++w.cum_reads;
        w.cum_bytes += bytes;
        w.cum_ns += ns;
        return first;
    }
    // Active files of the window (sorted by time spent, then name); resets the window counters.
    std::vector<std::pair<std::string, FileWindow>> TakeWindow() {
        std::vector<std::pair<std::string, FileWindow>> out;
        for (auto& [k, w] : files_) {
            if (!w.reads) continue;
            out.emplace_back(k, w);
            const FileWindow cum = w;
            w = FileWindow{};
            w.cum_reads = cum.cum_reads;
            w.cum_bytes = cum.cum_bytes;
            w.cum_ns = cum.cum_ns;
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return a.second.ns_total != b.second.ns_total ? a.second.ns_total > b.second.ns_total : a.first < b.first;
        });
        return out;
    }

private:
    std::map<std::string, FileWindow> files_;
};

// Maps an absolute byte offset of a RomFS image to "dir/dir/file" and the offset inside it.
// `read(offset, buf, n)` reads the image; it returns false on failure.
class RomfsIndex {
public:
    using Reader = std::function<bool(std::uint64_t, void*, std::uint64_t)>;

    bool Build(const Reader& read) {
        std::uint8_t h[0x50];
        if (!read(0, h, sizeof(h))) return false;
        const auto u64at = [&](std::size_t o) { std::uint64_t v; std::memcpy(&v, h + o, 8); return v; };
        if (u64at(0) != 0x50) return false;
        const std::uint64_t dir_off = u64at(0x18), dir_size = u64at(0x20);
        const std::uint64_t file_off = u64at(0x38), file_size = u64at(0x40), data_off = u64at(0x48);
        constexpr std::uint64_t kMaxTable = 256u << 20;
        if (!dir_size || !file_size || dir_size > kMaxTable || file_size > kMaxTable) return false;
        std::vector<std::uint8_t> dirs(dir_size), files(file_size);
        if (!read(dir_off, dirs.data(), dir_size) || !read(file_off, files.data(), file_size)) return false;
        const auto u32at = [](const std::vector<std::uint8_t>& v, std::size_t o) {
            std::uint32_t x = 0;
            if (o + 4 <= v.size()) std::memcpy(&x, v.data() + o, 4);
            return x;
        };
        // Directory entries: parent, sibling, child dir, child file, hash sibling, name length, name.
        std::map<std::uint32_t, std::pair<std::uint32_t, std::string>> dir_info; // offset -> parent, name
        for (std::size_t o = 0; o + 0x18 <= dirs.size();) {
            const std::uint32_t len = u32at(dirs, o + 0x14);
            if (o + 0x18 + len > dirs.size()) break;
            dir_info[static_cast<std::uint32_t>(o)] = {
                u32at(dirs, o), std::string(reinterpret_cast<const char*>(dirs.data() + o + 0x18), len)};
            o += 0x18 + ((len + 3) & ~3u);
        }
        const auto dir_path = [&](std::uint32_t d) {
            std::vector<std::string> parts;
            for (int guard = 0; guard < 64; ++guard) {
                const auto it = dir_info.find(d);
                if (it == dir_info.end()) break;
                if (!it->second.second.empty()) parts.push_back(it->second.second);
                if (it->second.first == d) break;
                d = it->second.first;
            }
            std::string p;
            for (auto i = parts.rbegin(); i != parts.rend(); ++i) p += *i + "/";
            return p;
        };
        // File entries: parent, sibling, data offset (8), data size (8), hash sibling, name length, name.
        for (std::size_t o = 0; o + 0x20 <= files.size();) {
            const std::uint32_t len = u32at(files, o + 0x1C);
            if (o + 0x20 + len > files.size()) break;
            Entry e;
            std::uint64_t rel = 0, size = 0;
            std::memcpy(&rel, files.data() + o + 8, 8);
            std::memcpy(&size, files.data() + o + 16, 8);
            e.start = data_off + rel;
            e.size = size;
            e.path = dir_path(u32at(files, o)) + std::string(reinterpret_cast<const char*>(files.data() + o + 0x20), len);
            entries_.push_back(std::move(e));
            o += 0x20 + ((len + 3) & ~3u);
        }
        std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.start < b.start; });
        return !entries_.empty();
    }

    // Name of the file containing `offset` ("" when it is in the header/tables); `in_file` gets the offset inside it.
    const std::string& Lookup(std::uint64_t offset, std::uint64_t* in_file = nullptr) const {
        static const std::string none;
        auto it = std::upper_bound(entries_.begin(), entries_.end(), offset,
                                   [](std::uint64_t v, const Entry& e) { return v < e.start; });
        if (it == entries_.begin()) return none;
        --it;
        if (offset >= it->start + it->size && it->size) return none;
        if (in_file) *in_file = offset - it->start;
        return it->path;
    }
    std::size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::uint64_t start = 0, size = 0;
        std::string path;
    };
    std::vector<Entry> entries_;
};

// ---------------------------------------------------------------- runtime (tts_trace.cpp)

inline bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("NXRT_TRACE_TTS");
        return v && *v && !(v[0] == '0' && !v[1]);
    }();
    return on;
}

std::uint64_t NowNs(); // steady clock, ns
void Start();          // starts the 5 s reporter thread (idempotent)

// fs: `key` is "romfs/dir/file", "romfs/<meta>" or "save/<relative path>".
void FsOpen(const std::string& description);
void FsRead(const std::string& key, std::int64_t offset, std::uint64_t bytes, std::uint64_t ns);
// IPC: one handled command.
void IpcCount(const std::string& service, const char* command);
// Called around every SVC of a guest thread (only when Enabled()).
void SvcEnter(KThread* thread, std::uint32_t imm, const std::uint64_t* x);
void SvcLeave(KThread* thread);
// F10 in the window: timestamp stamp plus an immediate report.
void Mark(std::uint64_t frame);

} // namespace nxrt::tts_trace
