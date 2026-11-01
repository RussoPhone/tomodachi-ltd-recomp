// SPDX-License-Identifier: GPL-3.0-or-later
// NXRT_INPUT_RECORD / NXRT_INPUT_REPLAY file format and helpers.
//
// One line per state change, in the NXRT_INPUT_SCRIPT dialect plus an optional touch field:
//   <frame> <buttons> [lx ly rx ry] [touch x y]   # t=<ms since start>
// Omitted axes mean a neutral stick, omitted touch means released. Markers are comments:
//   # MARCO <n> quadro=<frame> t=<ms>
#pragma once

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace nxrt::hle::input_record {

struct Event {
    std::uint64_t frame{};
    std::uint64_t buttons{};
    bool has_axes{};
    std::int32_t lx{}, ly{}, rx{}, ry{};
    bool touch{};
    std::uint32_t touch_x{}, touch_y{};
    std::uint64_t ms{};
    bool has_ms{};

    // Same input state (frame and clock time ignored).
    bool SameState(const Event& o) const;
};

struct Marker {
    std::uint64_t n{};
    std::uint64_t frame{};
    std::uint64_t ms{};
};

struct Recording {
    std::vector<Event> events;
    std::vector<Marker> markers;
    std::size_t bad_lines{};
};

std::string FormatButtons(std::uint64_t buttons);
std::string FormatEvent(const Event& e);
std::string FormatMarker(const Marker& m);
// False for comments, blank and invalid lines (use ParseMarkerLine for markers).
bool ParseEventLine(const std::string& line, Event& out);
bool ParseMarkerLine(const std::string& line, Marker& out);
bool Parse(std::istream& in, Recording& out);
bool Load(const std::string& path, Recording& out);

// Append-only writer, flushed after every line so closing the window loses nothing.
class Writer {
public:
    ~Writer();
    bool Open(const std::string& path);
    void WriteEvent(Event e); // stamps the clock time
    Marker WriteMarker(std::uint64_t frame);
    const std::string& path() const { return path_; }

private:
    std::mutex mutex_;
    std::FILE* file_ = nullptr;
    std::string path_;
    std::uint64_t start_ns_ = 0;
    std::uint64_t next_marker_ = 1;
    std::uint64_t NowMs() const;
};

// One-shot frame dump request (marker): the renderer takes it at its next present.
void RequestDump(const std::string& path);
bool DumpPending();
bool TakeDump(std::string& path);

// Last frame announced by OnInputFrame (frame counting used by the input script).
void SetCurrentFrame(std::uint64_t frame);
std::uint64_t CurrentFrame();

} // namespace nxrt::hle::input_record
