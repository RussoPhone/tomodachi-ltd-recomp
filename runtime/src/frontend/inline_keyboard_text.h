// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nxrt::frontend::inline_keyboard_text {

inline bool IsHighSurrogate(char16_t value) { return value >= 0xD800 && value <= 0xDBFF; }
inline bool IsLowSurrogate(char16_t value) { return value >= 0xDC00 && value <= 0xDFFF; }

inline void AppendUtf16(std::u16string& out, std::uint32_t cp) {
    if (cp <= 0xFFFF) {
        out.push_back(static_cast<char16_t>(cp));
    } else {
        cp -= 0x10000;
        out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
        out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
    }
}

inline void AppendUtf8(std::string& out, std::uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

inline std::u16string Utf8ToUtf16(std::string_view input) {
    std::u16string out;
    for (std::size_t i = 0; i < input.size();) {
        const auto lead = static_cast<unsigned char>(input[i]);
        if (lead < 0x80) {
            out.push_back(static_cast<char16_t>(lead));
            ++i;
            continue;
        }

        std::size_t count = 0;
        std::uint32_t cp = 0;
        if (lead >= 0xC2 && lead <= 0xDF) { count = 2; cp = lead & 0x1F; }
        else if (lead >= 0xE0 && lead <= 0xEF) { count = 3; cp = lead & 0x0F; }
        else if (lead >= 0xF0 && lead <= 0xF4) { count = 4; cp = lead & 0x07; }
        if (!count) {
            AppendUtf16(out, 0xFFFD);
            ++i;
            continue;
        }

        std::size_t consumed = 1;
        bool valid = i + count <= input.size();
        for (std::size_t j = 1; valid && j < count; ++j) {
            const auto byte = static_cast<unsigned char>(input[i + j]);
            if ((byte & 0xC0) != 0x80) {
                valid = false;
                break;
            }
            if (j == 1 && ((lead == 0xE0 && byte < 0xA0) || (lead == 0xED && byte > 0x9F) ||
                           (lead == 0xF0 && byte < 0x90) || (lead == 0xF4 && byte > 0x8F))) {
                valid = false;
                break;
            }
            cp = (cp << 6) | (byte & 0x3F);
            ++consumed;
        }
        if (!valid) {
            // Consume the malformed prefix as one replacement, including a truncated tail.
            while (i + consumed < input.size() &&
                   (static_cast<unsigned char>(input[i + consumed]) & 0xC0) == 0x80)
                ++consumed;
            AppendUtf16(out, 0xFFFD);
            i += consumed;
            continue;
        }
        AppendUtf16(out, cp);
        i += count;
    }
    return out;
}

inline std::string Utf16ToUtf8(std::u16string_view input) {
    std::string out;
    for (std::size_t i = 0; i < input.size(); ++i) {
        std::uint32_t cp = input[i];
        if (IsHighSurrogate(input[i])) {
            if (i + 1 < input.size() && IsLowSurrogate(input[i + 1])) {
                const auto high = input[i];
                const auto low = input[i + 1];
                cp = 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            } else {
                cp = 0xFFFD;
            }
        } else if (IsLowSurrogate(input[i])) {
            cp = 0xFFFD;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

inline std::u16string Truncate(std::u16string_view input, std::size_t max_units) {
    std::size_t length = std::min(input.size(), max_units);
    if (length > 0 && length < input.size() && IsHighSurrogate(input[length - 1]) &&
        IsLowSurrogate(input[length]))
        --length;
    return std::u16string(input.substr(0, length));
}

inline std::int32_t NormalizeCursor(std::u16string_view text, std::int32_t cursor) {
    auto bounded = static_cast<std::size_t>(std::clamp<std::int64_t>(cursor, 0, text.size()));
    if (bounded > 0 && bounded < text.size() && IsHighSurrogate(text[bounded - 1]) &&
        IsLowSurrogate(text[bounded]))
        --bounded;
    return static_cast<std::int32_t>(bounded);
}

inline void Insert(std::u16string& text, std::int32_t& cursor, std::u16string_view input,
                   std::size_t max_units) {
    cursor = NormalizeCursor(text, cursor);
    std::u16string addition;
    for (std::size_t i = 0; i < input.size();) {
        const std::size_t width = IsHighSurrogate(input[i]) && i + 1 < input.size() &&
                                          IsLowSurrogate(input[i + 1])
                                      ? 2
                                      : 1;
        if (IsHighSurrogate(input[i]) && width == 1) { ++i; continue; }
        if (IsLowSurrogate(input[i])) { ++i; continue; }
        if (text.size() + addition.size() + width > max_units) break;
        addition.append(input.substr(i, width));
        i += width;
    }
    text.insert(static_cast<std::size_t>(cursor), addition);
    cursor += static_cast<std::int32_t>(addition.size());
}

inline void Backspace(std::u16string& text, std::int32_t& cursor) {
    cursor = NormalizeCursor(text, cursor);
    if (cursor == 0) return;
    std::size_t begin = static_cast<std::size_t>(cursor - 1);
    if (begin > 0 && IsLowSurrogate(text[begin]) && IsHighSurrogate(text[begin - 1])) --begin;
    text.erase(begin, static_cast<std::size_t>(cursor) - begin);
    cursor = static_cast<std::int32_t>(begin);
}

} // namespace nxrt::frontend::inline_keyboard_text
