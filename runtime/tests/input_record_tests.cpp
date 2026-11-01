// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "services/hid.h"
#include "services/input_record.h"

using namespace nxrt::hle::input_record;

int main() {
    Event e;
    assert(ParseEventLine("10819 A # t=1234", e));
    assert(e.frame == 10819 && e.buttons == nxrt::hle::kButtonA && !e.has_axes && !e.touch && e.has_ms && e.ms == 1234);
    assert(ParseEventLine("5 A,Right 100 -200 0 32767", e) && e.has_axes && e.ly == -200 && e.ry == 32767);
    assert(ParseEventLine("7 - touch 640 360", e) && e.touch && e.touch_x == 640 && e.touch_y == 360 && e.buttons == 0);
    assert(ParseEventLine("8 B 1 2 3 4 touch 99999 99999", e) && e.touch_x == 1279 && e.touch_y == 719);
    for (const char* bad : {"", "# only comment", "9", "9 Q", "9 A 1 2 3", "9 A touch 1", "9 A touch x y",
                            "9 A 40000 0 0 0"}) {
        assert(!ParseEventLine(bad, e));
    }

    // Synthetic recording -> file on disk -> same events.
    Event a{3, nxrt::hle::kButtonA | nxrt::hle::kButtonUp}, b{4}, c{9};
    b.has_axes = true; b.lx = -32767; b.ry = 5;
    c.touch = true; c.touch_x = 100; c.touch_y = 200; c.buttons = nxrt::hle::kButtonZR;
    const std::string path = "input_record_test.tmp";
    {
        Writer w;
        assert(w.Open(path));
        w.WriteEvent(a);
        const Marker m1 = w.WriteMarker(3);
        w.WriteEvent(b);
        w.WriteEvent(c);
        const Marker m2 = w.WriteMarker(9);
        assert(m1.n == 1 && m2.n == 2 && m2.frame == 9);
    }
    Recording rec;
    assert(Load(path, rec));
    std::remove(path.c_str());
    assert(rec.bad_lines == 0 && rec.events.size() == 3 && rec.markers.size() == 2);
    assert(rec.events[0].SameState(a) && rec.events[1].SameState(b) && rec.events[2].SameState(c));
    assert(rec.events[0].frame == 3 && rec.events[2].frame == 9 && rec.events[2].has_ms);
    assert(rec.markers[1].n == 2 && rec.markers[1].frame == 9);
    assert(FormatEvent(rec.events[1]).find("-32767 0 0 5") != std::string::npos);

    // Neutral axes are equivalent whether or not they are spelled out.
    Event z{1}, zz{1};
    zz.has_axes = true;
    assert(z.SameState(zz));

    std::istringstream in("# MARCO 3 quadro=77 t=900\n1 A\nbogus line\n2 - touch 1 2\n");
    Recording r2;
    Parse(in, r2);
    assert(r2.events.size() == 2 && r2.bad_lines == 1 && r2.markers.size() == 1 && r2.markers[0].frame == 77);

    std::string p;
    assert(!DumpPending());
    RequestDump("x.ppm");
    assert(DumpPending() && TakeDump(p) && p == "x.ppm" && !DumpPending());
    std::cout << "input record tests passed\n";
}
