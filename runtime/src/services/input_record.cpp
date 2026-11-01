// SPDX-License-Identifier: GPL-3.0-or-later
#include "services/input_record.h"

#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <fstream>
#include <istream>
#include <sstream>
#include <string_view>

#include "services/hid.h"
#include "services/input_script.h"

namespace nxrt::hle::input_record {
namespace {

std::atomic<bool> g_dump_pending{false};
std::mutex g_dump_mutex;
std::string g_dump_path;
std::atomic<std::uint64_t> g_current_frame{0};

template <typename T>
bool ParseInt(std::string_view token, T& value) {
    if (token.empty()) return false;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    return error == std::errc{} && end == token.data() + token.size();
}

bool ParseAxis(const std::string& token, std::int32_t& value) {
    return ParseInt(token, value) && value >= -32767 && value <= 32767;
}

std::vector<std::string> Split(const std::string& s) {
    std::istringstream in(s);
    std::vector<std::string> out;
    for (std::string t; in >> t;) out.push_back(std::move(t));
    return out;
}

bool ParseKeyed(const std::string& token, const char* key, std::uint64_t& value) {
    const std::string prefix = std::string(key) + "=";
    return token.rfind(prefix, 0) == 0 && ParseInt(std::string_view(token).substr(prefix.size()), value);
}

} // namespace

bool Event::SameState(const Event& o) const {
    if (buttons != o.buttons || touch != o.touch) return false;
    if (touch && (touch_x != o.touch_x || touch_y != o.touch_y)) return false;
    const bool a = has_axes, b = o.has_axes;
    const std::int32_t al[4] = {a ? lx : 0, a ? ly : 0, a ? rx : 0, a ? ry : 0};
    const std::int32_t bl[4] = {b ? o.lx : 0, b ? o.ly : 0, b ? o.rx : 0, b ? o.ry : 0};
    for (int i = 0; i < 4; ++i)
        if (al[i] != bl[i]) return false;
    return true;
}

std::string FormatButtons(std::uint64_t buttons) {
    static constexpr std::array names{
        std::pair{"A", kButtonA}, std::pair{"B", kButtonB}, std::pair{"X", kButtonX},
        std::pair{"Y", kButtonY}, std::pair{"StickL", kButtonStickL}, std::pair{"StickR", kButtonStickR},
        std::pair{"L", kButtonL}, std::pair{"R", kButtonR}, std::pair{"ZL", kButtonZL},
        std::pair{"ZR", kButtonZR}, std::pair{"Plus", kButtonPlus}, std::pair{"Minus", kButtonMinus},
        std::pair{"Left", kButtonLeft}, std::pair{"Up", kButtonUp}, std::pair{"Right", kButtonRight},
        std::pair{"Down", kButtonDown},
    };
    std::string out;
    for (const auto& [name, bit] : names) {
        if (!(buttons & bit)) continue;
        if (!out.empty()) out += ',';
        out += name;
    }
    return out.empty() ? "-" : out;
}

std::string FormatEvent(const Event& e) {
    std::string s = std::to_string(e.frame) + " " + FormatButtons(e.buttons);
    if (e.has_axes) {
        s += " " + std::to_string(e.lx) + " " + std::to_string(e.ly) + " " + std::to_string(e.rx) + " " +
             std::to_string(e.ry);
    }
    if (e.touch) s += " touch " + std::to_string(e.touch_x) + " " + std::to_string(e.touch_y);
    if (e.has_ms) s += " # t=" + std::to_string(e.ms);
    return s;
}

std::string FormatMarker(const Marker& m) {
    return "# MARCO " + std::to_string(m.n) + " quadro=" + std::to_string(m.frame) + " t=" + std::to_string(m.ms);
}

bool ParseEventLine(const std::string& source, Event& out) {
    std::string line = source;
    std::string comment;
    if (const auto hash = line.find('#'); hash != std::string::npos) {
        comment = line.substr(hash + 1);
        line.erase(hash);
    }
    auto f = Split(line);
    if (f.size() < 2) return false;
    Event e{};
    // Optional trailing `touch x y`.
    if (f.size() >= 5 && f[f.size() - 3] == "touch") {
        std::uint32_t x = 0, y = 0;
        if (!ParseInt(f[f.size() - 2], x) || !ParseInt(f[f.size() - 1], y)) return false;
        e.touch = true;
        e.touch_x = std::min<std::uint32_t>(x, 1279);
        e.touch_y = std::min<std::uint32_t>(y, 719);
        f.resize(f.size() - 3);
    }
    if (f.size() != 2 && f.size() != 6) return false;
    input_script::Step step;
    if (!input_script::ParseLine(f[0] + " " + f[1], step)) return false;
    e.frame = step.frame;
    e.buttons = step.buttons;
    if (f.size() == 6) {
        if (!ParseAxis(f[2], e.lx) || !ParseAxis(f[3], e.ly) || !ParseAxis(f[4], e.rx) || !ParseAxis(f[5], e.ry))
            return false;
        e.has_axes = true;
    }
    for (const auto& t : Split(comment)) {
        std::uint64_t ms = 0;
        if (ParseKeyed(t, "t", ms)) {
            e.ms = ms;
            e.has_ms = true;
        }
    }
    out = e;
    return true;
}

bool ParseMarkerLine(const std::string& line, Marker& out) {
    const auto f = Split(line);
    if (f.size() != 5 || f[0] != "#" || f[1] != "MARCO") return false;
    Marker m{};
    if (!ParseInt(f[2], m.n) || !ParseKeyed(f[3], "quadro", m.frame) || !ParseKeyed(f[4], "t", m.ms)) return false;
    out = m;
    return true;
}

bool Parse(std::istream& in, Recording& out) {
    std::string line;
    while (std::getline(in, line)) {
        Marker m;
        Event e;
        if (ParseMarkerLine(line, m)) {
            out.markers.push_back(m);
            continue;
        }
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos || line[first] == '#') continue;
        if (ParseEventLine(line, e))
            out.events.push_back(e);
        else
            ++out.bad_lines;
    }
    return true;
}

bool Load(const std::string& path, Recording& out) {
    std::ifstream f(path);
    if (!f) return false;
    return Parse(f, out);
}

Writer::~Writer() {
    if (file_) std::fclose(file_);
}

std::uint64_t Writer::NowMs() const {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    const auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return (ns - start_ns_) / 1000000;
}

bool Writer::Open(const std::string& path) {
    std::lock_guard lock(mutex_);
    file_ = std::fopen(path.c_str(), "w");
    if (!file_) return false;
    path_ = path;
    start_ns_ = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now().time_since_epoch())
                                               .count());
    std::fputs("# nxrt input record v1: <frame> <buttons> [lx ly rx ry] [touch x y]  # t=<ms>\n", file_);
    std::fflush(file_);
    return true;
}

void Writer::WriteEvent(Event e) {
    std::lock_guard lock(mutex_);
    if (!file_) return;
    e.ms = NowMs();
    e.has_ms = true;
    std::fprintf(file_, "%s\n", FormatEvent(e).c_str());
    std::fflush(file_);
}

Marker Writer::WriteMarker(std::uint64_t frame) {
    std::lock_guard lock(mutex_);
    Marker m{next_marker_++, frame, NowMs()};
    if (file_) {
        std::fprintf(file_, "%s\n", FormatMarker(m).c_str());
        std::fflush(file_);
    }
    return m;
}

void RequestDump(const std::string& path) {
    std::lock_guard lock(g_dump_mutex);
    g_dump_path = path;
    g_dump_pending.store(true, std::memory_order_release);
}

bool DumpPending() {
    return g_dump_pending.load(std::memory_order_acquire);
}

bool TakeDump(std::string& path) {
    std::lock_guard lock(g_dump_mutex);
    if (!g_dump_pending.load(std::memory_order_relaxed)) return false;
    path = std::move(g_dump_path);
    g_dump_pending.store(false, std::memory_order_release);
    return true;
}

void SetCurrentFrame(std::uint64_t frame) {
    g_current_frame.store(frame, std::memory_order_relaxed);
}

std::uint64_t CurrentFrame() {
    return g_current_frame.load(std::memory_order_relaxed);
}

} // namespace nxrt::hle::input_record
