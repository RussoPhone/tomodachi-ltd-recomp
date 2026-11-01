// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <cstring>

#include "tts_trace.h"

using namespace nxrt::tts_trace;

namespace {
void Put32(std::vector<std::uint8_t>& img, std::size_t at, std::uint32_t v) { std::memcpy(img.data() + at, &v, 4); }
void Put64(std::vector<std::uint8_t>& img, std::size_t at, std::uint64_t v) { std::memcpy(img.data() + at, &v, 8); }
} // namespace

int main() {
    // Counter: top-N ordering, window reset.
    Counter c;
    c.Add("a"); c.Add("b", 5); c.Add("c", 5); c.Add("a", 2);
    auto top = c.TakeTop(2);
    assert(top.size() == 2 && top[0].first == "b" && top[0].second == 5 && top[1].first == "c");
    assert(c.TakeTop(10).empty());

    // Size buckets.
    assert(SizeBucket(0) == 0 && SizeBucket(512) == 0 && SizeBucket(513) == 1 && SizeBucket(1u << 20) == 4 &&
           SizeBucket(100u << 20) == kHistBuckets - 1);

    // FsStats: window counters reset, lifetime totals kept, inactive files omitted.
    FsStats fs;
    assert(fs.Record("x", 100, 50, 1000));
    assert(!fs.Record("x", 0, 10, 3000));
    assert(fs.Record("y", 0, 4096, 10));
    auto w = fs.TakeWindow();
    assert(w.size() == 2 && w[0].first == "x");
    assert(w[0].second.reads == 2 && w[0].second.bytes == 60 && w[0].second.ns_total == 4000 && w[0].second.ns_max == 3000);
    assert(w[0].second.off_min == 0 && w[0].second.off_max == 150);
    assert(w[0].second.hist[0] == 2 && w[1].second.hist[1] == 1);
    assert(fs.Record("z", 0, 1, 1) && fs.TakeWindow().size() == 1);
    auto w2 = fs.TakeWindow();
    assert(w2.empty());
    fs.Record("x", 5, 5, 5);
    w2 = fs.TakeWindow();
    assert(w2.size() == 1 && w2[0].second.reads == 1 && w2[0].second.cum_reads == 3);

    // RomfsIndex on a synthetic image: root/, root/VoiceText/, files a.vtdb2 (in VoiceText) and b (in root).
    std::vector<std::uint8_t> img(0x500, 0);
    Put64(img, 0x00, 0x50);
    Put64(img, 0x18, 0x100); Put64(img, 0x20, 0x18 + 0x18 + 12); // dir meta
    Put64(img, 0x38, 0x200); Put64(img, 0x40, 0x20 + 8 + 0x20 + 4); // file meta
    Put64(img, 0x48, 0x400);                                      // data offset
    // root dir at 0 (empty name), VoiceText dir at 0x18 (parent 0, name length 9 -> 12 bytes).
    Put32(img, 0x100 + 0x18 + 0x00, 0);
    Put32(img, 0x100 + 0x18 + 0x14, 9);
    std::memcpy(img.data() + 0x100 + 0x18 + 0x18, "VoiceText", 9);
    // file a.vtdb2 (len 7 -> 8) parent 0x18 at rel 0 size 100
    Put32(img, 0x200, 0x18); Put64(img, 0x208, 0); Put64(img, 0x210, 100); Put32(img, 0x21C, 7);
    std::memcpy(img.data() + 0x220, "a.vtdb2", 7);
    // file b (len 1 -> 4) parent 0 at rel 100 size 50, entry at 0x200 + 0x28
    Put32(img, 0x228, 0); Put64(img, 0x230, 100); Put64(img, 0x238, 50); Put32(img, 0x244, 1);
    std::memcpy(img.data() + 0x248, "b", 1);
    RomfsIndex idx;
    const bool built = idx.Build([&](std::uint64_t off, void* buf, std::uint64_t n) {
        if (off + n > img.size()) return false;
        std::memcpy(buf, img.data() + off, n);
        return true;
    });
    assert(built && idx.size() == 2);
    std::uint64_t in = 99;
    assert(idx.Lookup(0x400, &in) == "VoiceText/a.vtdb2" && in == 0);
    assert(idx.Lookup(0x400 + 99, &in) == "VoiceText/a.vtdb2" && in == 99);
    assert(idx.Lookup(0x400 + 100, &in) == "b" && in == 0);
    assert(idx.Lookup(0x400 + 150).empty()); // past the last file
    assert(idx.Lookup(0x10).empty());        // header
    std::puts("tts_trace tests ok");
    return 0;
}
