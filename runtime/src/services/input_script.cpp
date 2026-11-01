// SPDX-License-Identifier: GPL-3.0-or-later
#include "services/input_script.h"

#include <array>
#include <charconv>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

#include "services/hid.h"

namespace nxrt::hle::input_script {
namespace {

bool ParseButtons(const std::string& list, std::uint64_t& out) {
    static constexpr std::array names{
        std::pair{"A", kButtonA}, std::pair{"B", kButtonB}, std::pair{"X", kButtonX},
        std::pair{"Y", kButtonY}, std::pair{"StickL", kButtonStickL},
        std::pair{"StickR", kButtonStickR}, std::pair{"L", kButtonL}, std::pair{"R", kButtonR},
        std::pair{"ZL", kButtonZL}, std::pair{"ZR", kButtonZR}, std::pair{"Plus", kButtonPlus},
        std::pair{"Minus", kButtonMinus}, std::pair{"Left", kButtonLeft}, std::pair{"Up", kButtonUp},
        std::pair{"Right", kButtonRight}, std::pair{"Down", kButtonDown},
    };
    out = 0;
    if (list == "-" || list == "none") return true;

    std::stringstream ss(list);
    std::string token;
    while (std::getline(ss, token, ',')) {
        bool found = false;
        for (const auto& [name, bit] : names) {
            if (token == name || token == std::string("kButton") + name) {
                out |= bit;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

template <typename T>
bool ParseInteger(std::string_view token, T& value) {
    if (token.empty()) return false;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    return error == std::errc{} && end == token.data() + token.size();
}

bool ParseAxis(const std::string& token, std::int32_t& value) {
    if (!ParseInteger(token, value)) return false;
    return value >= -32767 && value <= 32767;
}

} // namespace

bool ParseLine(const std::string& source, Step& out) {
    std::string line = source;
    if (const auto comment = line.find('#'); comment != std::string::npos) line.erase(comment);
    std::istringstream stream(line);
    std::vector<std::string> fields;
    for (std::string field; stream >> field;) fields.push_back(std::move(field));
    if (fields.empty()) return false;
    if (fields.size() != 2 && fields.size() != 6) return false;

    Step parsed{};
    if (!ParseInteger(fields[0], parsed.frame) || !ParseButtons(fields[1], parsed.buttons)) return false;
    parsed.text = fields[1];
    if (fields.size() == 6) {
        if (!ParseAxis(fields[2], parsed.lx) || !ParseAxis(fields[3], parsed.ly) ||
            !ParseAxis(fields[4], parsed.rx) || !ParseAxis(fields[5], parsed.ry))
            return false;
        parsed.has_axes = true;
    }
    out = std::move(parsed);
    return true;
}

void Apply(const Step& step, State& state) {
    state.buttons = step.buttons;
    if (step.has_axes) {
        state.sticks = {step.lx, step.ly, step.rx, step.ry};
        state.axes_active = true;
    }
}

} // namespace nxrt::hle::input_script
