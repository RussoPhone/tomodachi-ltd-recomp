// SPDX-License-Identifier: GPL-3.0-or-later
// nxrt's own Mii data model: layout, the six built-in Miis (field by field against the reference preset table), validation, checksums and
// BuildRandom. tests/mii/mii_oracle_compare.cpp compares against the old code directly.
#include <cstdio>
#include <cstring>

#include "services/mii_data/mii_data.h"

using namespace nxrt::mii;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// The console's six preset Miis, one row each, attributes in Field order (CharInfo numbering,
// colours already on the current palette: black hair 8, browns 1..7, and so on). The numbers are
// the preset table of the reference implementation (suyu raw_data DefaultMii) after its version-3
// palette conversion; 0..2 are male, 3..5 female.
static const unsigned char kRef[kDefaultMiiCount][kFieldCount] = {
    {0,4,0,64,64,0,0,0,4,0,0,68,8,0,2,8,4,3,4,2,12,6,8,4,3,6,2,10,1,4,9,23,19,4,3,13,8,0,0,4,10,0,8,4,10,0,4,2,20},
    {0,5,0,64,64,0,0,0,0,0,0,55,6,0,2,12,4,3,4,2,12,6,6,4,3,6,2,10,1,4,9,23,19,4,3,13,8,0,0,4,10,0,8,4,10,0,4,2,20},
    {0,0,0,64,64,0,0,0,1,0,0,33,1,0,2,8,4,3,4,2,12,6,1,4,3,6,2,10,1,4,9,23,19,4,3,13,8,0,0,4,10,0,8,4,10,0,4,2,20},
    {0,2,1,64,64,0,0,0,2,0,0,24,8,0,4,8,4,3,3,2,12,0,8,4,3,6,2,10,1,4,9,23,19,4,3,13,8,0,0,4,10,0,8,4,10,0,4,2,20},
    {0,6,1,64,64,0,0,0,0,0,0,14,7,0,4,13,4,3,3,2,12,0,7,4,3,6,2,10,1,4,9,23,19,4,3,13,8,0,0,4,10,0,8,4,10,0,4,2,20},
    {0,7,1,64,64,0,0,0,0,0,0,12,1,0,4,8,4,3,3,2,12,0,1,4,3,6,2,10,1,4,9,23,19,4,3,13,8,0,0,4,10,0,8,4,10,0,4,2,20},
};

int main() {
    std::mt19937 rng(12345);
    CHECK(sizeof(CharInfo) == 0x58 && sizeof(StoreData) == 0x44 && sizeof(CoreData) == 0x30);
    CHECK(offsetof(CharInfo, attr) == 0x26 && offsetof(CharInfo, padding) == 0x57);

    for (std::uint32_t i = 0; i < kDefaultMiiCount; ++i) {
        const StoreData s = BuildDefault(i, rng);
        const CharInfo c = s.ToCharInfo();
        CHECK(s.IsValid());
        CHECK(c.IsValid());
        for (std::size_t f = 0; f < kFieldCount; ++f) {
            if (c.attr[f] != kRef[i][f]) {
                std::printf("FAIL default %u field %zu: got %u want %u\n", i, f, c.attr[f], kRef[i][f]);
                ++failures;
            }
        }
        CHECK(c.Get(Field::Gender) == (i < 3 ? 0 : 1));
        CHECK(c.Get(Field::GlassType) == 0 && c.Get(Field::FacelineWrinkle) == 0);
        CHECK(std::memcmp(c.name.data(), u"no name", 14) == 0 && c.name[7] == 0);
        CHECK(!(BuildDefault(i, rng).create_id == s.create_id));
        CHECK((s.create_id.bytes[6] >> 4) == 4 && (s.create_id.bytes[8] >> 6) == 2);
    }

    // Checksums catch a flipped attribute and a changed id.
    StoreData s = BuildDefault(0, rng);
    StoreData t = s;
    t.core.Set(Field::HairType, t.core.Get(Field::HairType) ^ 1);
    CHECK(!t.IsValid());
    t = s;
    t.create_id.bytes[0] ^= 1;
    CHECK(!t.IsValid());

    // Field round trip and range limits.
    CoreData cd{};
    cd.name = BuildDefault(0, rng).core.name;
    CHECK(cd.IsValid());
    cd.Set(Field::EyeY, 19);
    CHECK(!cd.IsValid());
    cd.Set(Field::EyeY, 18);
    CHECK(cd.IsValid());
    CHECK(FieldMax(Field::EyebrowY) == 18);
    cd.name[0] = 0;
    CHECK(!cd.IsValid());

    // Random Miis for every request combination are valid.
    for (std::uint32_t age = 0; age <= 3; ++age) {
        for (std::uint32_t gender = 0; gender <= 2; ++gender) {
            for (std::uint32_t race = 0; race <= 3; ++race) {
                for (int n = 0; n < 200; ++n) {
                    const StoreData r = BuildRandom(static_cast<Age>(age), static_cast<Gender>(gender),
                                                    static_cast<Race>(race), rng);
                    CHECK(r.IsValid());
                    CHECK(r.ToCharInfo().IsValid());
                    if (gender < 2) {
                        CHECK(r.core.Get(Field::Gender) == gender);
                    }
                    if (gender == 1) {
                        CHECK(r.core.Get(Field::BeardType) == 0 && r.core.Get(Field::MustacheType) == 0);
                    }
                }
            }
        }
    }
    std::printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
