// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt shim for suyu's Common::Random.
#pragma once

#include <random>

#include "common/common_types.h"

namespace Common::Random {
inline std::mt19937& GetMT() {
    static std::mt19937 mt{std::random_device{}()};
    return mt;
}
inline std::mt19937& GetMT19937() {
    return GetMT();
}
inline u32 Random32(u32 seed) {
    return std::mt19937{seed}();
}
inline u64 Random64(u64 seed) {
    return std::mt19937_64{seed}();
}
} // namespace Common::Random
