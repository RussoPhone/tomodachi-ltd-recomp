// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace nxrt::render {
// Compatibility provenance for palette compositors that write encoded bytes
// through a linear view. This is deliberately narrower than global TIC decoding:
// unrelated writes and explicit contradictory formats invalidate the convention.
struct RenderedSrgbContent {
    bool encoded = false;
    std::uint64_t writer = 0;

    void Draw(std::uint64_t program, bool linear_srgb_override, bool inherited_view, bool writes_rgb = true) {
        if (!writes_rgb) return;
        if (linear_srgb_override) {
            encoded = true;
            writer = program;
        } else if (!inherited_view || writer != program) {
            Clear();
        }
    }
    void Clear() { encoded = false; writer = 0; }
    void CopyFrom(const RenderedSrgbContent& source, bool full) {
        if (full) *this = source;
        else if (encoded != source.encoded) Clear();
        else if (writer != source.writer) writer = 0;
    }
    bool Decode(bool tic_srgb, bool legacy_ignore) const {
        return encoded && tic_srgb && !legacy_ignore;
    }
};
} // namespace nxrt::render
