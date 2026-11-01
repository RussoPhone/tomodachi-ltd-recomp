// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt shim: the subset of suyu's Common::UUID the vendored Mii code uses.
#pragma once

#include <array>
#include <cstring>
#include <random>

#include "common/common_types.h"

namespace Common {

struct UUID {
    std::array<u8, 0x10> uuid{};

    bool IsInvalid() const { return uuid == std::array<u8, 0x10>{}; }
    bool IsValid() const { return !IsInvalid(); }
    bool operator==(const UUID&) const = default;

    static UUID MakeDefault() {
        UUID u;
        u.uuid = {0x5d, 0x2b, 0x71, 0x2a, 0x31, 0xc8, 0x4c, 0x38, 0xa0, 0xd5, 0x1d, 0x8e, 0x4f, 0x3a, 0x61, 0x07};
        return u;
    }
    static UUID MakeRandomRFC4122V4() { return MakeRandomRFC(); }
    static UUID MakeRandomRFC() {
        static std::mt19937_64 rng{std::random_device{}()};
        UUID u;
        for (auto& b : u.uuid) b = static_cast<u8>(rng());
        u.uuid[6] = static_cast<u8>((u.uuid[6] & 0x0F) | 0x40); // version 4
        u.uuid[8] = static_cast<u8>((u.uuid[8] & 0x3F) | 0x80); // RFC variant
        return u;
    }
};
static_assert(sizeof(UUID) == 0x10);

} // namespace Common
