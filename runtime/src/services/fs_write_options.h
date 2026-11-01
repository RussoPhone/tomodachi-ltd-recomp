// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <limits>

#include "common.h"

namespace nxrt::hle::detail {

constexpr u32 kWriteOptionFlush = 1;

// IFile::Write returns a Result, not a byte count. Complete short host writes
// before reporting success, and honor FsWriteOption_Flush (bit 0).
template <typename Pwrite, typename Flush>
bool WriteFileWithOptions(u32 options, const u8* data, u64 size, s64 offset,
                          Pwrite&& pwrite, Flush&& flush) {
    if (offset < 0) return false;

    u64 written = 0;
    while (written < size) {
        if (written > static_cast<u64>(std::numeric_limits<s64>::max() - offset)) return false;
        const u64 remaining = size - written;
        const s64 count = pwrite(data + written, remaining, offset + static_cast<s64>(written));
        if (count <= 0 || static_cast<u64>(count) > remaining) return false;
        written += static_cast<u64>(count);
    }

    return !(options & kWriteOptionFlush) || flush();
}

} // namespace nxrt::hle::detail
