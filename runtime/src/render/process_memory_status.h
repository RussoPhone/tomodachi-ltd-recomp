// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <cstdint>
#include <string_view>

namespace nxrt::render {

struct ProcessMemoryStatus {
    std::uint64_t rss_kib = 0;
    std::uint64_t swap_kib = 0;
    bool has_rss = false;
    bool has_swap = false;
};

inline bool ParseProcessMemoryStatusLine(std::string_view line, ProcessMemoryStatus& status) {
    enum class Field { None, Rss, Swap } field = Field::None;
    if (line.starts_with("VmRSS:")) {
        field = Field::Rss;
        line.remove_prefix(6);
    } else if (line.starts_with("VmSwap:")) {
        field = Field::Swap;
        line.remove_prefix(7);
    } else {
        return false;
    }

    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(line.data(), line.data() + line.size(), value);
    if (error != std::errc{} || end == line.data()) return false;
    line.remove_prefix(static_cast<std::size_t>(end - line.data()));
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line != "kB") return false;

    if (field == Field::Rss) {
        status.rss_kib = value;
        status.has_rss = true;
    } else {
        status.swap_kib = value;
        status.has_swap = true;
    }
    return true;
}

} // namespace nxrt::render
