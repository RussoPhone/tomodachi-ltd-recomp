// SPDX-License-Identifier: GPL-3.0-or-later
// Mii data model for mii:e / mii:u: CharInfo (0x58), CoreData (0x30) and StoreData (0x44),
// written for nxrt from the documented Switch Mii formats; no emulator code.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <random>

namespace nxrt::mii {

constexpr std::size_t kNicknameChars = 10;
constexpr std::uint32_t kDefaultMiiCount = 6;

// Attribute fields, in the order they appear in CharInfo from offset 0x26.
enum class Field : std::uint8_t {
    FontRegion, FavoriteColor, Gender, Height, Build, Type, RegionMove,
    FacelineType, FacelineColor, FacelineWrinkle, FacelineMake,
    HairType, HairColor, HairFlip,
    EyeType, EyeColor, EyeScale, EyeAspect, EyeRotate, EyeX, EyeY,
    EyebrowType, EyebrowColor, EyebrowScale, EyebrowAspect, EyebrowRotate, EyebrowX, EyebrowY,
    NoseType, NoseScale, NoseY,
    MouthType, MouthColor, MouthScale, MouthAspect, MouthY,
    BeardColor, BeardType,
    MustacheType, MustacheScale, MustacheY,
    GlassType, GlassColor, GlassScale, GlassY,
    MoleType, MoleScale, MoleX, MoleY,
    Count,
};
constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);

// Request values for BuildRandom; the "All" values mean "pick one".
enum class Age : std::uint32_t { Young, Normal, Old, All };
enum class Gender : std::uint32_t { Male, Female, All };
enum class Race : std::uint32_t { Black, White, Asian, All };

struct Uuid {
    std::array<std::uint8_t, 16> bytes{};
    bool operator==(const Uuid&) const = default;
};

using Nickname = std::array<char16_t, kNicknameChars>;

// Valid values of a field in the 0..max range (CharInfo-side numbering).
std::uint8_t FieldMax(Field f);

// 0x58 bytes, as the game reads it from mii:e.
struct CharInfo {
    Uuid create_id;
    Nickname name;
    char16_t null_terminator;
    std::array<std::uint8_t, kFieldCount> attr; // indexed by Field
    std::uint8_t padding;

    std::uint8_t Get(Field f) const { return attr[static_cast<std::size_t>(f)]; }
    void Set(Field f, std::uint8_t v) { attr[static_cast<std::size_t>(f)] = v; }
    // Every field in range and the nickname non-empty with nothing after its first terminator.
    bool IsValid() const;
};
static_assert(sizeof(CharInfo) == 0x58);
static_assert(std::is_trivially_copyable_v<CharInfo>);

struct CharInfoElement {
    CharInfo char_info;
    std::uint32_t source; // 0 database, 1 default, 2 account, 3 friend
};
static_assert(sizeof(CharInfoElement) == 0x5c);
constexpr std::uint32_t kSourceDefault = 1;

// Attributes packed in seven 32-bit words, followed by the nickname.
struct CoreData {
    std::array<std::uint32_t, 7> words{};
    Nickname name{};

    std::uint32_t Get(Field f) const;
    void Set(Field f, std::uint32_t v);
    bool IsValid() const;
};
static_assert(sizeof(CoreData) == 0x30);

struct StoreData {
    CoreData core;
    Uuid create_id;
    std::uint16_t data_crc = 0;
    std::uint16_t device_crc = 0;

    void UpdateChecksums();
    // Core fields in range and both checksums matching.
    bool IsValid() const;
    CharInfo ToCharInfo() const;
};
static_assert(sizeof(StoreData) == 0x44);

// CRC-16 (poly 0x1021) of the core data and create id, and of the device id plus the record size,
// stored byte-swapped as the console keeps them.
std::uint16_t DataCrc(const CoreData& core, const Uuid& id);
std::uint16_t DeviceCrc(std::size_t record_size);

Uuid MakeCreateId(std::mt19937& rng);

// Built-in Miis, index 0..kDefaultMiiCount-1 (the console presets: 0..2 male, 3..5 female).
// create_id is fresh on every call.
StoreData BuildDefault(std::uint32_t index, std::mt19937& rng);
StoreData BuildRandom(Age age, Gender gender, Race race, std::mt19937& rng);

// Process-wide generator seeded from the OS.
std::mt19937& SystemRng();

} // namespace nxrt::mii
