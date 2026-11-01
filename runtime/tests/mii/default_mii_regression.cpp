// SPDX-License-Identifier: GPL-3.0-or-later
// Regression: BuildDefault(0..5) and BuildBase(0..1) must reproduce, byte for byte,
// the CoreData that CoreData::BuildRandom generates from the seeds documented in
// third_party/suyu/core/hle/service/mii/types/raw_data.cpp, and must be valid.
#include <cstdio>
#include <cstring>

#include "core/hle/service/mii/mii_util.h"
#include "core/hle/service/mii/types/core_data.h"
#include "core/hle/service/mii/types/store_data.h"

using namespace Service::Mii;

static int failures = 0;

static void Check(const char* what, u32 index, const CoreData& expected, const StoreData& built) {
    // core_data is the first member of StoreData (0x30 bytes); create_id/crc vary.
    static_assert(sizeof(CoreData) == 0x30);
    u8 got[sizeof(CoreData)];
    std::memcpy(got, &built, sizeof(got));
    const bool same = std::memcmp(got, &expected, sizeof(got)) == 0;
    const auto valid = built.IsValid();
    std::printf("%s[%u]: bytes %s, IsValid %s (0x%x)\n", what, index, same ? "OK" : "DIFFER",
                valid == ValidationResult::NoErrors ? "OK" : "FAIL", static_cast<u32>(valid));
    if (!same) {
        const u8* e = reinterpret_cast<const u8*>(&expected);
        for (std::size_t i = 0; i < sizeof(got); ++i) {
            if (got[i] != e[i]) {
                std::printf("  byte 0x%02zx: got %02x expected %02x\n", i, got[i], e[i]);
            }
        }
    }
    if (!same || valid != ValidationResult::NoErrors) {
        ++failures;
    }
}

static CoreData Generate(u32 seed, Age age, Gender gender) {
    Common::Random::GetMT().seed(seed);
    CoreData core;
    core.BuildRandom(age, gender, Race::White);
    return core;
}

int main() {
    constexpr Age ages[3] = {Age::Young, Age::Normal, Age::Old};
    for (u32 i = 0; i < 6; ++i) {
        const auto expected = Generate(20261010 + i, ages[i % 3], i / 3 ? Gender::Female : Gender::Male);
        StoreData built;
        built.BuildDefault(i);
        Check("BuildDefault", i, expected, built);
    }
    for (u32 g = 0; g < 2; ++g) {
        const auto expected = Generate(20261100 + g, Age::Normal, g ? Gender::Female : Gender::Male);
        StoreData built;
        built.BuildBase(g ? Gender::Female : Gender::Male);
        Check("BuildBase", g, expected, built);
    }
    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
