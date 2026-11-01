#include "services/system_region.h"

#include <cassert>

int main() {
    using nxrt::hle::SystemRegionCode;
    using nxrt::hle::SystemRegionFromLocale;
    using nxrt::hle::SystemRegionFromLocales;
    using nxrt::hle::SystemRegionFromName;
    using nxrt::hle::ResolveSystemRegion;

    assert(SystemRegionFromLocale("ja_JP.UTF-8") == SystemRegionCode::Japan);
    assert(SystemRegionFromLocale("pt_BR.UTF-8") == SystemRegionCode::Usa);
    assert(SystemRegionFromLocale("fr_FR.UTF-8") == SystemRegionCode::Europe);
    assert(SystemRegionFromLocale("en_AU.UTF-8") == SystemRegionCode::Australia);
    assert(SystemRegionFromLocale("zh_CN.UTF-8") == SystemRegionCode::China);
    assert(SystemRegionFromLocale("zh_TW.UTF-8") == SystemRegionCode::HongKongTaiwanKorea);
    assert(SystemRegionFromLocale("ko_KR.UTF-8") == SystemRegionCode::HongKongTaiwanKorea);
    assert(!SystemRegionFromLocale("C.UTF-8"));
    assert(!SystemRegionFromLocale("invalid_LOCALE"));
    assert(SystemRegionFromLocales({"C.UTF-8", "ja_JP.UTF-8"}) == SystemRegionCode::Japan);
    assert(SystemRegionFromLocales({"C.UTF-8", "pt_BR.UTF-8"}) == SystemRegionCode::Usa);
    assert(SystemRegionFromLocales({"unknown", "invalid_LOCALE"}) == SystemRegionCode::Usa);
    assert(SystemRegionFromName("Japan") == SystemRegionCode::Japan);
    assert(SystemRegionFromName("hong-kong_taiwan_korea") ==
           SystemRegionCode::HongKongTaiwanKorea);
    assert(SystemRegionFromName("invalid") == std::nullopt);
    assert(ResolveSystemRegion("China", {"ja_JP"}) == SystemRegionCode::China);
    assert(ResolveSystemRegion("invalid", {"ja_JP"}) == SystemRegionCode::Japan);
    assert(ResolveSystemRegion("", {"C.UTF-8", "fr_FR"}) == SystemRegionCode::Europe);
}
