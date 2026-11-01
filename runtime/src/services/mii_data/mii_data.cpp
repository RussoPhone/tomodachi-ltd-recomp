// SPDX-License-Identifier: GPL-3.0-or-later
#include "services/mii_data/mii_data.h"

#include <cstring>

namespace nxrt::mii {

namespace {

struct FieldInfo {
    std::uint8_t word, shift, width, max, bias; // bias: CharInfo value minus stored value
};

constexpr FieldInfo kInfo[kFieldCount] = {
    {2, 22, 2, 3, 0},   // FontRegion
    {5, 8, 4, 11, 0},   // FavoriteColor
    {1, 7, 1, 1, 0},    // Gender
    {0, 8, 7, 127, 0},  // Height
    {0, 16, 7, 127, 0}, // Build
    {0, 31, 1, 1, 0},   // Type
    {2, 14, 2, 3, 0},   // RegionMove
    {5, 12, 4, 11, 0},  // FacelineType
    {5, 16, 4, 9, 0},   // FacelineColor
    {5, 20, 4, 11, 0},  // FacelineWrinkle
    {5, 24, 4, 11, 0},  // FacelineMake
    {0, 0, 8, 131, 0},  // HairType
    {0, 24, 7, 99, 0},  // HairColor
    {0, 23, 1, 1, 0},   // HairFlip
    {2, 8, 6, 59, 0},   // EyeType
    {1, 0, 7, 99, 0},   // EyeColor
    {4, 21, 3, 7, 0},   // EyeScale
    {4, 13, 3, 6, 0},   // EyeAspect
    {4, 5, 3, 7, 0},    // EyeRotate
    {5, 28, 4, 12, 0},  // EyeX
    {2, 24, 5, 18, 0},  // EyeY
    {3, 0, 5, 23, 0},   // EyebrowType
    {1, 8, 7, 99, 0},   // EyebrowColor
    {6, 0, 4, 8, 0},    // EyebrowScale
    {3, 29, 3, 6, 0},   // EyebrowAspect
    {6, 4, 4, 11, 0},   // EyebrowRotate
    {6, 8, 4, 12, 0},   // EyebrowX
    {6, 12, 4, 15, 3},  // EyebrowY (stored 3 lower than CharInfo)
    {3, 8, 5, 17, 0},   // NoseType
    {6, 16, 4, 8, 0},   // NoseScale
    {3, 16, 5, 18, 0},  // NoseY
    {2, 16, 6, 35, 0},  // MouthType
    {1, 16, 7, 99, 0},  // MouthColor
    {6, 20, 4, 8, 0},   // MouthScale
    {3, 21, 3, 6, 0},   // MouthAspect
    {3, 24, 5, 18, 0},  // MouthY
    {1, 24, 7, 99, 0},  // BeardColor
    {3, 13, 3, 5, 0},   // BeardType
    {3, 5, 3, 5, 0},    // MustacheType
    {6, 24, 4, 8, 0},   // MustacheScale
    {4, 0, 5, 16, 0},   // MustacheY
    {5, 0, 5, 19, 0},   // GlassType
    {2, 0, 7, 99, 0},   // GlassColor
    {2, 29, 3, 7, 0},   // GlassScale
    {4, 8, 5, 20, 0},   // GlassY
    {0, 15, 1, 1, 0},   // MoleType
    {6, 28, 4, 8, 0},   // MoleScale
    {4, 16, 5, 16, 0},  // MoleX
    {4, 24, 5, 30, 0},  // MoleY
};

constexpr const FieldInfo& Info(Field f) { return kInfo[static_cast<std::size_t>(f)]; }

// "no name": the nickname every built-in Mii carries.
constexpr Nickname kDefaultNickname = {u'n', u'o', u' ', u'n', u'a', u'm', u'e'};

bool NicknameValid(const Nickname& n) {
    if (n[0] == 0) {
        return false;
    }
    std::size_t i = 1;
    while (i < n.size() && n[i] != 0) {
        ++i;
    }
    while (i < n.size() && n[i] == 0) {
        ++i;
    }
    return i == n.size();
}

std::uint16_t Swap16(std::uint16_t v) { return static_cast<std::uint16_t>((v << 8) | (v >> 8)); }

// Packed CoreData of the six built-in Miis: 0..2 male, 3..5 female (the console's own presets,
// converted from their version-3 palettes to the current colour indices).
constexpr std::uint8_t kDefaults[kDefaultMiiCount][0x30] = {
    {0x44,0x40,0x40,0x08,0x08,0x08,0x13,0x08,0x08,0x02,0x17,0x8c,0x06,0x01,0x69,0x6d,0x8a,0x6a,0x82,0x14,0x00,0x04,0x04,0x20,0x64,0x72,0x44,0x44,0x6e,0x00,0x6f,0x00,0x20,0x00,0x6e,0x00,0x61,0x00,0x6d,0x00,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x37,0x40,0x40,0x06,0x0c,0x06,0x13,0x08,0x08,0x02,0x17,0x8c,0x06,0x01,0x69,0x6d,0x8a,0x6a,0x82,0x14,0x00,0x05,0x00,0x20,0x64,0x72,0x44,0x44,0x6e,0x00,0x6f,0x00,0x20,0x00,0x6e,0x00,0x61,0x00,0x6d,0x00,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x21,0x40,0x40,0x01,0x08,0x01,0x13,0x08,0x08,0x02,0x17,0x8c,0x06,0x01,0x69,0x6d,0x8a,0x6a,0x82,0x14,0x00,0x00,0x01,0x20,0x64,0x72,0x44,0x44,0x6e,0x00,0x6f,0x00,0x20,0x00,0x6e,0x00,0x61,0x00,0x6d,0x00,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x18,0x40,0x40,0x08,0x88,0x08,0x13,0x08,0x08,0x04,0x17,0x8c,0x00,0x01,0x69,0x6d,0x6a,0x6a,0x82,0x14,0x00,0x02,0x02,0x20,0x64,0x72,0x44,0x44,0x6e,0x00,0x6f,0x00,0x20,0x00,0x6e,0x00,0x61,0x00,0x6d,0x00,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x0e,0x40,0x40,0x07,0x8d,0x07,0x13,0x08,0x08,0x04,0x17,0x8c,0x00,0x01,0x69,0x6d,0x6a,0x6a,0x82,0x14,0x00,0x06,0x00,0x20,0x64,0x72,0x44,0x44,0x6e,0x00,0x6f,0x00,0x20,0x00,0x6e,0x00,0x61,0x00,0x6d,0x00,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x0c,0x40,0x40,0x01,0x88,0x01,0x13,0x08,0x08,0x04,0x17,0x8c,0x00,0x01,0x69,0x6d,0x6a,0x6a,0x82,0x14,0x00,0x07,0x00,0x20,0x64,0x72,0x44,0x44,0x6e,0x00,0x6f,0x00,0x20,0x00,0x6e,0x00,0x61,0x00,0x6d,0x00,0x65,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
};

// Palette indices of the original five-to-eight colour choices per feature.
constexpr std::uint8_t kHairPalette[8] = {8, 1, 2, 3, 4, 5, 6, 7};
constexpr std::uint8_t kEyePalette[6] = {8, 9, 10, 11, 12, 13};
constexpr std::uint8_t kMouthPalette[5] = {19, 20, 21, 22, 23};
constexpr std::uint8_t kGlassPalette[6] = {8, 14, 15, 16, 17, 18};

std::uint32_t Pick(std::mt19937& rng, std::uint32_t lo, std::uint32_t hi) {
    return std::uniform_int_distribution<std::uint32_t>(lo, hi)(rng);
}
bool Chance(std::mt19937& rng, std::uint32_t one_in) { return Pick(rng, 0, one_in - 1) == 0; }

} // namespace

std::uint8_t FieldMax(Field f) {
    return static_cast<std::uint8_t>(Info(f).max + Info(f).bias);
}

std::uint32_t CoreData::Get(Field f) const {
    const FieldInfo& i = Info(f);
    return (words[i.word] >> i.shift) & ((1u << i.width) - 1);
}

void CoreData::Set(Field f, std::uint32_t v) {
    const FieldInfo& i = Info(f);
    const std::uint32_t mask = ((1u << i.width) - 1) << i.shift;
    words[i.word] = (words[i.word] & ~mask) | ((v << i.shift) & mask);
}

bool CoreData::IsValid() const {
    if (!NicknameValid(name)) {
        return false;
    }
    for (std::size_t f = 0; f < kFieldCount; ++f) {
        if (Get(static_cast<Field>(f)) > kInfo[f].max) {
            return false;
        }
    }
    return true;
}

bool CharInfo::IsValid() const {
    if (!NicknameValid(name)) {
        return false;
    }
    for (std::size_t f = 0; f < kFieldCount; ++f) {
        const std::uint32_t v = attr[f];
        if (v < kInfo[f].bias || v - kInfo[f].bias > kInfo[f].max) {
            return false;
        }
    }
    return true;
}

std::uint16_t DataCrc(const CoreData& core, const Uuid& id) {
    std::uint8_t buf[sizeof(CoreData) + sizeof(Uuid)];
    std::memcpy(buf, &core, sizeof(CoreData));
    std::memcpy(buf + sizeof(CoreData), id.bytes.data(), id.bytes.size());
    std::uint32_t crc = 0;
    for (const std::uint8_t b : buf) {
        crc ^= static_cast<std::uint32_t>(b) << 8;
        for (int k = 0; k < 8; ++k) {
            crc <<= 1;
            if (crc & 0x10000) {
                crc = (crc ^ 0x1021) & 0xFFFF;
            }
        }
    }
    return Swap16(static_cast<std::uint16_t>(crc));
}

std::uint16_t DeviceCrc(std::size_t record_size) {
    // Author id of this runtime's console. The register is shifted before each id byte is mixed
    // in, then shifted once per bit of the record size.
    static constexpr std::uint8_t kAuthorId[16] = {0x5d, 0x2b, 0x71, 0x2a, 0x31, 0xc8, 0x4c, 0x38,
                                                   0xa0, 0xd5, 0x1d, 0x8e, 0x4f, 0x3a, 0x61, 0x07};
    std::uint32_t crc = 0;
    auto shift = [&crc] {
        crc <<= 1;
        if (crc & 0x10000) {
            crc ^= 0x1021;
        }
    };
    for (std::size_t i = 0; i < 16; ++i) {
        for (int k = 0; k < 8; ++k) {
            shift();
        }
        crc ^= kAuthorId[i];
    }
    for (std::size_t i = 0; i < record_size * 8; ++i) {
        shift();
    }
    return Swap16(static_cast<std::uint16_t>(crc));
}

void StoreData::UpdateChecksums() {
    data_crc = DataCrc(core, create_id);
    device_crc = DeviceCrc(sizeof(StoreData));
}

bool StoreData::IsValid() const {
    return core.IsValid() && data_crc == DataCrc(core, create_id) &&
           device_crc == DeviceCrc(sizeof(StoreData));
}

CharInfo StoreData::ToCharInfo() const {
    CharInfo c{};
    c.create_id = create_id;
    c.name = core.name;
    c.null_terminator = 0;
    for (std::size_t f = 0; f < kFieldCount; ++f) {
        c.attr[f] = static_cast<std::uint8_t>(core.Get(static_cast<Field>(f)) + kInfo[f].bias);
    }
    return c;
}

std::mt19937& SystemRng() {
    static std::mt19937 rng{std::random_device{}()};
    return rng;
}

Uuid MakeCreateId(std::mt19937& rng) {
    Uuid id;
    for (std::size_t i = 0; i < id.bytes.size(); i += 4) {
        const std::uint32_t r = rng();
        std::memcpy(&id.bytes[i], &r, 4);
    }
    id.bytes[6] = static_cast<std::uint8_t>((id.bytes[6] & 0x0F) | 0x40); // version 4
    id.bytes[8] = static_cast<std::uint8_t>((id.bytes[8] & 0x3F) | 0x80); // RFC 4122 variant
    return id;
}

StoreData BuildDefault(std::uint32_t index, std::mt19937& rng) {
    StoreData s{};
    std::memcpy(&s.core, kDefaults[index % kDefaultMiiCount], sizeof(CoreData));
    s.create_id = MakeCreateId(rng);
    s.UpdateChecksums();
    return s;
}

StoreData BuildRandom(Age age, Gender gender, Race race, std::mt19937& rng) {
    if (gender == Gender::All) {
        gender = static_cast<Gender>(Pick(rng, 0, 1));
    }
    if (age == Age::All) {
        const std::uint32_t r = Pick(rng, 0, 9);
        age = r >= 8 ? Age::Old : (r >= 4 ? Age::Normal : Age::Young);
    }
    if (race == Race::All) {
        const std::uint32_t r = Pick(rng, 0, 9);
        race = r >= 8 ? Race::Black : (r >= 4 ? Race::White : Race::Asian);
    }
    const bool male = gender == Gender::Male;

    StoreData s{};
    CoreData& c = s.core;
    c.name = kDefaultNickname;
    c.Set(Field::Gender, male ? 0 : 1);
    c.Set(Field::FavoriteColor, Pick(rng, 0, 11));
    c.Set(Field::Height, 64);
    c.Set(Field::Build, 64);

    c.Set(Field::FacelineType, Pick(rng, 0, 11));
    // Skin tones 0..5, lightest to darkest.
    const std::uint32_t tone = race == Race::White ? Pick(rng, 0, 1)
                               : race == Race::Asian ? Pick(rng, 2, 3)
                                                     : Pick(rng, 4, 5);
    c.Set(Field::FacelineColor, tone);
    if (age == Age::Old ? Chance(rng, 2) : age == Age::Normal && Chance(rng, 8)) {
        c.Set(Field::FacelineWrinkle, Pick(rng, 1, 11));
    }
    if (!male && age != Age::Old && Chance(rng, 2)) {
        c.Set(Field::FacelineMake, Pick(rng, 1, 11));
    }

    const std::uint32_t hair_pal = age == Age::Old ? Pick(rng, 6, 7) : Pick(rng, 0, 5);
    const std::uint32_t hair_color = kHairPalette[hair_pal];
    c.Set(Field::HairType, Pick(rng, 0, 131));
    c.Set(Field::HairColor, hair_color);
    c.Set(Field::HairFlip, Pick(rng, 0, 1));

    c.Set(Field::EyeType, Pick(rng, 0, 59));
    c.Set(Field::EyeColor, kEyePalette[race == Race::Black ? Pick(rng, 0, 1) : Pick(rng, 0, 5)]);
    c.Set(Field::EyeScale, 4);
    c.Set(Field::EyeAspect, 3);
    c.Set(Field::EyeRotate, 4);
    c.Set(Field::EyeX, 2);
    c.Set(Field::EyeY, 12);

    c.Set(Field::EyebrowType, Pick(rng, 0, 22));
    c.Set(Field::EyebrowColor, hair_color);
    c.Set(Field::EyebrowScale, 4);
    c.Set(Field::EyebrowAspect, 3);
    c.Set(Field::EyebrowRotate, 6);
    c.Set(Field::EyebrowX, 2);
    c.Set(Field::EyebrowY, 7);

    c.Set(Field::NoseType, Pick(rng, 0, 17));
    c.Set(Field::NoseScale, 4);
    c.Set(Field::NoseY, 9);

    c.Set(Field::MouthType, Pick(rng, 0, 35));
    c.Set(Field::MouthColor, kMouthPalette[Pick(rng, 0, 4)]);
    c.Set(Field::MouthScale, 4);
    c.Set(Field::MouthAspect, 3);
    c.Set(Field::MouthY, 13);

    c.Set(Field::BeardColor, hair_color);
    c.Set(Field::MustacheScale, 4);
    c.Set(Field::MustacheY, 10);
    if (male && age != Age::Young) {
        const std::uint32_t odds = age == Age::Old ? 3 : 5;
        if (Chance(rng, odds)) {
            c.Set(Field::MustacheType, Pick(rng, 1, 5));
        }
        if (Chance(rng, odds)) {
            c.Set(Field::BeardType, Pick(rng, 1, 5));
        }
    }

    c.Set(Field::GlassColor, kGlassPalette[0]);
    c.Set(Field::GlassScale, 4);
    c.Set(Field::GlassY, 10);
    if (Chance(rng, age == Age::Old ? 3 : 8)) {
        c.Set(Field::GlassType, Pick(rng, 1, 19));
        c.Set(Field::GlassColor, kGlassPalette[Pick(rng, 0, 5)]);
    }

    c.Set(Field::MoleScale, 4);
    c.Set(Field::MoleX, 2);
    c.Set(Field::MoleY, 20);
    if (Chance(rng, 10)) {
        c.Set(Field::MoleType, 1);
        c.Set(Field::MoleX, Pick(rng, 2, 14));
        c.Set(Field::MoleY, Pick(rng, 10, 28));
    }

    s.create_id = MakeCreateId(rng);
    s.UpdateChecksums();
    return s;
}

} // namespace nxrt::mii
