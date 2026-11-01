// SPDX-License-Identifier: GPL-3.0-or-later
#include "services/common.h"
#include "services/system_region.h"

#include <array>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace nxrt::hle {

namespace {

SystemRegionCode HostRegionCode() {
    const char* configured = std::getenv("NXRT_SYSTEM_REGION");
    std::vector<std::string_view> locales;
    for (const char* name : std::array{"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(name);
        if (value && *value) locales.emplace_back(value);
    }
    const SystemRegionCode region = ResolveSystemRegion(configured ? configured : "", locales);
    if (configured && *configured && !SystemRegionFromName(configured))
        LOG_WARN("set: ignoring unknown NXRT_SYSTEM_REGION value");
    return region;
}

const char* RegionName(SystemRegionCode region) {
    switch (region) {
    case SystemRegionCode::Japan: return "Japan";
    case SystemRegionCode::Usa: return "USA";
    case SystemRegionCode::Europe: return "Europe";
    case SystemRegionCode::Australia: return "Australia";
    case SystemRegionCode::HongKongTaiwanKorea: return "Hong Kong/Taiwan/Korea";
    case SystemRegionCode::China: return "China";
    }
    return "USA";
}

class Settings final : public ServiceObject {
public:
    Settings() : ServiceObject("set") {
        Register(4, "GetRegionCode", [](Ctx& ctx) {
            const SystemRegionCode region = HostRegionCode();
            ctx.Push<u32>(static_cast<u32>(region));
            LOG_INFO("set: GetRegionCode -> %s (%u)", RegionName(region), static_cast<u32>(region));
        });
    }
};

} // namespace

void RegisterSettingsService() {
    RegisterService("set", [] { return std::make_shared<Settings>(); });
}

} // namespace nxrt::hle
