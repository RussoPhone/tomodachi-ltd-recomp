// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

#include "render/draw_census.h"

using nxrt::render::ClassifyDropReason;
using nxrt::render::DrawCensus;
using nxrt::render::DropReason;

static std::vector<std::string> Lines(const std::string& path) {
    std::ifstream in(path);
    std::vector<std::string> out;
    for (std::string l; std::getline(in, l);) out.push_back(l);
    return out;
}
static std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    for (std::string f; std::getline(ss, f, ',');) out.push_back(f);
    return out;
}

int main() {
    assert(ClassifyDropReason("pipeline:no-program") == DropReason::NoProgram);
    assert(ClassifyDropReason("pipeline:spirv failed") == DropReason::PipelineFailed);
    assert(ClassifyDropReason("vertex-unmapped") == DropReason::VertexUnmapped);
    assert(ClassifyDropReason("something new") == DropReason::Other);

    const std::string path = "/tmp/nxrt_draw_census_test_" + std::to_string(getpid()) + ".csv";
    {
        DrawCensus c(path);
        assert(c.Ok());
        // frame 5: program A 3 draws (2 exec, 1 dropped), program B 1 draw exec
        c.GameDraw(5, 0xA); c.Exec(true, false, false, false);
        c.GameDraw(5, 0xA); c.Exec(false, true, true, false);
        c.GameDraw(5, 0xA); c.Drop("vertex-unmapped");
        c.GameDraw(5, 0xB); c.Exec(false, false, false, true);
        c.Counter(5, 1, 0x1000, 0x100000, true);
        // frame 6 starts: frame 5 rows are finished
        c.GameDraw(6, 0xA); c.Drop("pipeline:no-program");
        c.Flush(); // finishes frame 6 and writes everything
    }
    auto rows = Lines(path);
    assert(rows.size() == 1 + 3); // header + (5,A) (5,B) (6,A)
    auto a = Split(rows[1]);
    assert(a[0] == "5" && a[1] == "000000000000000a");
    assert(a[2] == "3" && a[3] == "2" && a[4] == "1");
    assert(a[5] == "1" && a[6] == "1" && a[7] == "1" && a[8] == "0");
    auto b = Split(rows[2]);
    assert(b[2] == "1" && b[3] == "1" && b[4] == "0" && b[8] == "1");
    auto f6 = Split(rows[3]);
    assert(f6[0] == "6" && f6[2] == "1" && f6[3] == "0" && f6[4] == "1");
    // every row: game == exec + dropped, and the dropped count equals the sum of the reason columns
    for (size_t i = 1; i < rows.size(); ++i) {
        auto f = Split(rows[i]);
        assert(std::stoull(f[2]) == std::stoull(f[3]) + std::stoull(f[4]));
        unsigned long long sum = 0;
        for (size_t k = 9; k < f.size(); ++k) sum += std::stoull(f[k]);
        assert(sum == std::stoull(f[4]));
    }
    auto counters = Lines(path + ".counters.csv");
    assert(counters.size() == 2 && counters[1] == "5,1,1000,100000,1");

    // Periodic flush: 32 finished rows reach the file without Flush()/destructor.
    {
        DrawCensus c(path);
        for (unsigned long long q = 1; q <= 40; ++q) {
            c.GameDraw(q, 7); c.Exec(false, false, false, false);
        }
        // 39 frames finished so far -> 32 written, the rest buffered, header present
        assert(Lines(path).size() == 1 + 32);
        // summary at every 600th frame
        assert(c.TakeSummary().empty());
        c.GameDraw(600, 7); c.Drop("quads");
        c.GameDraw(601, 7);
        std::string s = c.TakeSummary();
        assert(s.find("q=600") != std::string::npos && s.find("dropped=0") != std::string::npos);
        assert(c.TakeSummary().empty());
    }
    // Frames CSV: one line per frame, compute dispatches/programs/bytes, draw totals, input snapshot.
    {
        DrawCensus c(path);
        int step = 0;
        c.SetInputProvider([&] {
            ++step;
            return nxrt::render::CensusInput{1, 2, 3, 4, 0x41};
        });
        c.GameDraw(10, 0xA); c.Exec(false, false, false, false);
        c.GameDraw(10, 0xA); c.Drop("quads");
        c.ComputeDispatch(10, 0xC2); c.ComputeWriteback(64);
        c.ComputeDispatch(10, 0xC1); c.ComputeDispatch(10, 0xC2); c.ComputeWriteback(16);
        c.GameDraw(11, 0xA); c.Exec(false, false, false, false);
        c.Flush();
        c.Flush(); // idempotent: no duplicate frame line
        assert(step == 2);
        auto fr = Lines(path + ".frames.csv");
        assert(fr.size() == 3);
        assert(fr[0].rfind("frame,compute_dispatches,compute_writeback_bytes,compute_programs,", 0) == 0);
        assert(fr[1] == "10,3,80,00000000000000c1;00000000000000c2,2,1,1,2,3,4,41");
        assert(fr[2] == "11,0,0,,1,1,1,2,3,4,41");
    }
    std::remove((path + ".frames.csv").c_str());
    std::remove(path.c_str());
    std::remove((path + ".counters.csv").c_str());
    std::puts("draw_census tests ok");
    return 0;
}
