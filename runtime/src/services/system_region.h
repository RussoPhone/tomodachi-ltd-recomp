// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"

namespace nxrt::hle {

enum class SystemRegionCode : u32 {
    Japan = 0,
    Usa = 1,
    Europe = 2,
    Australia = 3,
    HongKongTaiwanKorea = 4,
    China = 5,
};

inline std::optional<SystemRegionCode> SystemRegionFromLocale(std::string_view locale) {
    const size_t suffix = locale.find_first_of(".@");
    if (suffix != std::string_view::npos) locale = locale.substr(0, suffix);
    const size_t separator = locale.find_last_of("_-");
    if (separator == std::string_view::npos || separator + 1 >= locale.size()) return std::nullopt;

    std::string territory(locale.substr(separator + 1));
    for (char& c : territory) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (territory == "JP") return SystemRegionCode::Japan;
    if (territory == "AU" || territory == "NZ") return SystemRegionCode::Australia;
    if (territory == "CN") return SystemRegionCode::China;
    if (territory == "HK" || territory == "TW" || territory == "KR")
        return SystemRegionCode::HongKongTaiwanKorea;

    constexpr std::array<std::string_view, 34> europe = {
        "AL", "AT", "BA", "BE", "BG", "BY", "CH", "CY", "CZ", "DE", "DK", "EE", "ES", "FI", "FR",
        "GB", "GR", "HR", "HU", "IE", "IS", "IT", "LT", "LU", "LV", "ME", "MK", "MT", "NL", "NO",
        "PL", "PT", "RO", "RS"};
    for (const std::string_view country : europe) {
        if (territory == country) return SystemRegionCode::Europe;
    }
    return std::nullopt;
}

inline SystemRegionCode SystemRegionFromLocales(const std::vector<std::string_view>& locales) {
    for (const std::string_view locale : locales) {
        if (const auto region = SystemRegionFromLocale(locale)) return *region;
    }
    return SystemRegionCode::Usa;
}

inline std::optional<SystemRegionCode> SystemRegionFromName(std::string_view name) {
    std::string normalized;
    for (char c : name) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (c >= 'A' && c <= 'Z') normalized.push_back(c);
    }
    if (normalized == "JAPAN") return SystemRegionCode::Japan;
    if (normalized == "USA" || normalized == "US" || normalized == "NORTHAMERICA")
        return SystemRegionCode::Usa;
    if (normalized == "EUROPE") return SystemRegionCode::Europe;
    if (normalized == "AUSTRALIA") return SystemRegionCode::Australia;
    if (normalized == "HONGKONGTAIWANKOREA" || normalized == "HKTWK")
        return SystemRegionCode::HongKongTaiwanKorea;
    if (normalized == "CHINA") return SystemRegionCode::China;
    return std::nullopt;
}

inline SystemRegionCode ResolveSystemRegion(std::string_view configured,
                                            const std::vector<std::string_view>& locales) {
    if (const auto region = SystemRegionFromName(configured)) return *region;
    return SystemRegionFromLocales(locales);
}

} // namespace nxrt::hle
