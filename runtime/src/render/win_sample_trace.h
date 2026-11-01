#pragma once

#include <charconv>
#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace nxrt::render {

inline std::optional<std::vector<std::uint64_t>> ParseWinSampleCheckpointDrawIds(std::string_view value) {
    if (value.empty()) return std::nullopt;

    std::vector<std::uint64_t> ids;
    while (!value.empty()) {
        const std::size_t comma = value.find(',');
        const std::string_view token = value.substr(0, comma);
        if (token.empty()) return std::nullopt;

        std::uint64_t id = 0;
        const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), id, 10);
        if (error != std::errc{} || end != token.data() + token.size() || id == 0) return std::nullopt;
        ids.push_back(id);

        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1);
        if (value.empty()) return std::nullopt;
    }
    return ids;
}

} // namespace nxrt::render
