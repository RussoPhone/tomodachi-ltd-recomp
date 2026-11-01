// SPDX-License-Identifier: GPL-3.0-or-later
// Offline oracle: compares nxrt::mii against the previous suyu-based implementation.
// Not part of the build (the suyu Mii sources left it). Build by hand from the commit
// before the switch:
//   g++ -std=c++20 -I src -I third_party/suyu tests/mii/mii_oracle_compare.cpp
//       src/services/mii_data/mii_data.cpp third_party/suyu/core/hle/service/mii/types/*.cpp
#include <cstdio>
#include <cstring>

#include "core/hle/service/mii/types/char_info.h"
#include "core/hle/service/mii/types/store_data.h"
#include "services/mii_data/mii_data.h"

namespace old = Service::Mii;
namespace nw = nxrt::mii;

static int failures = 0;

int main() {
    std::mt19937 rng(7);
    // Default Miis: identical CharInfo apart from the random create id.
    for (std::uint32_t i = 0; i < 6; ++i) {
        old::StoreData os; os.BuildDefault(i);
        old::CharInfo oc; oc.SetFromStoreData(os);
        const nw::StoreData ns = nw::BuildDefault(i, rng);
        const nw::CharInfo nc = ns.ToCharInfo();
        const bool same = std::memcmp(reinterpret_cast<const char*>(&oc) + 0x10,
                                      reinterpret_cast<const char*>(&nc) + 0x10, 0x48) == 0;
        // Raw StoreData layout and checksums, given the same create id.
        old::StoreData os2 = os;
        nw::StoreData forced = ns;
        std::memcpy(forced.create_id.bytes.data(), reinterpret_cast<const char*>(&os) + 0x30, 16);
        forced.UpdateChecksums();
        const bool store_same = std::memcmp(&forced, &os2, sizeof(os2)) == 0;
        if (!store_same) {
            for (std::size_t k = 0; k < sizeof(os2); ++k)
                if (reinterpret_cast<const unsigned char*>(&forced)[k] != reinterpret_cast<const unsigned char*>(&os2)[k])
                    std::printf("  byte 0x%02zx new %02x old %02x\n", k,
                                reinterpret_cast<const unsigned char*>(&forced)[k],
                                reinterpret_cast<const unsigned char*>(&os2)[k]);
        }
        std::printf("default[%u] charinfo %s, storedata+crc %s, valid(old %d new %d)\n", i,
                    same ? "OK" : "DIFFER", store_same ? "OK" : "DIFFER",
                    os.IsValid() == old::ValidationResult::NoErrors, ns.IsValid());
        failures += !same + !store_same;
    }
    // Random Miis from the new generator must satisfy the old validator and survive an old
    // CharInfo round trip.
    int bad = 0, total = 0;
    for (std::uint32_t a = 0; a <= 3; ++a)
        for (std::uint32_t g = 0; g <= 2; ++g)
            for (std::uint32_t r = 0; r <= 3; ++r)
                for (int n = 0; n < 300; ++n, ++total) {
                    const nw::StoreData ns = nw::BuildRandom(static_cast<nw::Age>(a), static_cast<nw::Gender>(g),
                                                             static_cast<nw::Race>(r), rng);
                    old::StoreData os;
                    std::memcpy(&os, &ns, sizeof(os));
                    old::CharInfo oc; oc.SetFromStoreData(os);
                    const nw::CharInfo nc = ns.ToCharInfo();
                    if (os.IsValid() != old::ValidationResult::NoErrors ||
                        oc.Verify() != old::ValidationResult::NoErrors ||
                        std::memcmp(&oc, &nc, sizeof(nc)) != 0) {
                        if (bad++ < 3)
                            std::printf("  bad: store %u char %u eq %d\n", (unsigned)os.IsValid(), (unsigned)oc.Verify(),
                                        std::memcmp(&oc, &nc, sizeof(nc)) == 0);
                    }
                }
    std::printf("random: %d/%d accepted and identical in old validator/converter\n", total - bad, total);
    failures += bad != 0;
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures != 0;
}
