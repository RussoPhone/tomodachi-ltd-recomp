// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nxrt::render {

// Compact XXH64 implementation used for per-frame guest texture change checks.
// The input is only compared against a later hash in the same process, so a
// fixed seed is sufficient and no hash value is serialized.
inline std::uint64_t XxHash64(const void* input, std::size_t size) {
    constexpr std::uint64_t p1 = 11400714785074694791ULL;
    constexpr std::uint64_t p2 = 14029467366897019727ULL;
    constexpr std::uint64_t p3 = 1609587929392839161ULL;
    constexpr std::uint64_t p4 = 9650029242287828579ULL;
    constexpr std::uint64_t p5 = 2870177450012600261ULL;

    const auto* p = input ? static_cast<const std::uint8_t*>(input) : reinterpret_cast<const std::uint8_t*>("");
    const auto* const end = p + size;
    const auto read64 = [](const std::uint8_t* at) {
        std::uint64_t value;
        std::memcpy(&value, at, sizeof(value));
        return value;
    };
    const auto read32 = [](const std::uint8_t* at) {
        std::uint32_t value;
        std::memcpy(&value, at, sizeof(value));
        return value;
    };
    const auto round = [=](std::uint64_t acc, std::uint64_t value) {
        acc += value * p2;
        acc = std::rotl(acc, 31);
        return acc * p1;
    };

    std::uint64_t hash;
    if (size >= 32) {
        std::uint64_t v1 = p1 + p2;
        std::uint64_t v2 = p2;
        std::uint64_t v3 = 0;
        std::uint64_t v4 = 0 - p1;
        const auto* const limit = end - 32;
        do {
            v1 = round(v1, read64(p)); p += 8;
            v2 = round(v2, read64(p)); p += 8;
            v3 = round(v3, read64(p)); p += 8;
            v4 = round(v4, read64(p)); p += 8;
        } while (p <= limit);

        hash = std::rotl(v1, 1) + std::rotl(v2, 7) + std::rotl(v3, 12) + std::rotl(v4, 18);
        const auto merge_round = [&](std::uint64_t acc, std::uint64_t value) {
            acc ^= round(0, value);
            return acc * p1 + p4;
        };
        hash = merge_round(hash, v1);
        hash = merge_round(hash, v2);
        hash = merge_round(hash, v3);
        hash = merge_round(hash, v4);
    } else {
        hash = p5;
    }

    hash += size;
    while (static_cast<std::size_t>(end - p) >= 8) {
        hash ^= round(0, read64(p));
        hash = std::rotl(hash, 27) * p1 + p4;
        p += 8;
    }
    if (static_cast<std::size_t>(end - p) >= 4) {
        hash ^= static_cast<std::uint64_t>(read32(p)) * p1;
        hash = std::rotl(hash, 23) * p2 + p3;
        p += 4;
    }
    while (p < end) {
        hash ^= static_cast<std::uint64_t>(*p++) * p5;
        hash = std::rotl(hash, 11) * p1;
    }

    hash ^= hash >> 33;
    hash *= p2;
    hash ^= hash >> 29;
    hash *= p3;
    hash ^= hash >> 32;
    return hash;
}

}  // namespace nxrt::render
