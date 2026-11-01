#include "render/gpu_code_range.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void TestReadBounds() {
    const nxrt::render::CpuBackingSpan span{0x8000, 0x40};
    Check(nxrt::render::ContainsBackingAccess(span, 0, 8), "read at start should fit");
    Check(nxrt::render::ContainsBackingAccess(span, 0x38, 8), "read ending at boundary should fit");
    Check(!nxrt::render::ContainsBackingAccess(span, 0x3c, 8), "read crossing boundary should fail");

    const nxrt::render::CpuBackingSpan with_nvn_header{0x8000, 0x80};
    Check(nxrt::render::ContainsBackingAccess(with_nvn_header, 0, 4), "NVN magic read should fit");
    Check(nxrt::render::ContainsBackingAccess(with_nvn_header, 0x30, 0x50),
          "program header after NVN header should fit");
    Check(!nxrt::render::ContainsBackingAccess(span, 0x30, 0x50),
          "program header must be rejected when mapped backing is too short");
}

void TestDirectPoolRange() {
    const std::array ranges{nxrt::render::GpuCpuRange{0x1000, 0x8000, 0x100}};
    const auto resolved = nxrt::render::ResolveGpuCpuRange(0x1010, ranges);
    Check(resolved.has_value(), "direct pool address should resolve");
    Check(resolved->cpu == 0x8010, "direct pool CPU address should preserve offset");
    Check(resolved->size == 0xf0, "direct pool span should end at pool boundary");
}

void TestAdjacentDirectPoolsCoalesce() {
    const std::array pools{
        nxrt::render::GpuCpuRange{0x1020, 0x8020, 0x30},
        nxrt::render::GpuCpuRange{0x1000, 0x8000, 0x20},
    };
    const auto resolved = nxrt::render::ResolveGpuBacking(0x1018, {}, pools);
    Check(resolved.has_value(), "direct pool address should resolve");
    Check(resolved->cpu == 0x8018, "direct pool CPU address should preserve offset");
    Check(resolved->size == 0x38, "GPU and CPU contiguous direct pools should coalesce");
}

void TestOverlappingPoolsKeepOriginalFirstMatch() {
    const std::array pools{
        nxrt::render::GpuCpuRange{0x1000, 0x8000, 0x100},
        nxrt::render::GpuCpuRange{0x1000, 0x9000, 0x100},
    };
    const auto resolved = nxrt::render::ResolveGpuBacking(0x1010, {}, pools);
    Check(resolved.has_value(), "overlapping direct pools should resolve");
    Check(resolved->cpu == 0x8010, "direct pools should preserve original first-match priority");
}

void TestVirtualMappingKeepsPriorityOverPool() {
    const std::array virtual_ranges{nxrt::render::GpuCpuRange{0x1000, 0x5000, 0x20}};
    const std::array pool_ranges{nxrt::render::GpuCpuRange{0x1000, 0x8000, 0x100}};
    const auto resolved = nxrt::render::ResolveGpuBacking(0x1010, virtual_ranges, pool_ranges);
    Check(resolved.has_value(), "overlapping virtual mapping should resolve");
    Check(resolved->cpu == 0x5010, "virtual mapping should retain precedence over pool alias");
    Check(resolved->size == 0x10, "virtual mapping span should be selected before pool span");
}

void TestAdjacentVirtualMappings() {
    const std::array ranges{
        nxrt::render::GpuCpuRange{0x1000, 0x5000, 0x20},
        nxrt::render::GpuCpuRange{0x1020, 0x5020, 0x30},
    };
    const auto resolved = nxrt::render::ResolveGpuCpuRange(0x1018, ranges);
    Check(resolved.has_value(), "virtual mapping address should resolve");
    Check(resolved->cpu == 0x5018, "virtual mapping CPU address should preserve offset");
    Check(resolved->size == 0x38, "GPU and CPU contiguous mappings should coalesce");
}

void TestNoncontiguousVirtualMappingDoesNotCoalesce() {
    const std::array ranges{
        nxrt::render::GpuCpuRange{0x1000, 0x5000, 0x20},
        nxrt::render::GpuCpuRange{0x1020, 0x6000, 0x30},
    };
    const auto resolved = nxrt::render::ResolveGpuCpuRange(0x1018, ranges);
    Check(resolved.has_value(), "first virtual mapping should resolve");
    Check(resolved->size == 8, "CPU-discontiguous mappings must not coalesce");
    Check(!nxrt::render::ContainsBackingAccess(*resolved, 0, 16),
          "texture readback must not cross into a GPU-adjacent but CPU-discontiguous mapping");
}

void TestRangeArithmeticHandlesZeroAndUint64Max() {
    const nxrt::render::CpuBackingSpan span{0x8000, 0x40};
    constexpr auto max = std::numeric_limits<std::uint64_t>::max();
    Check(nxrt::render::ContainsBackingAccess(span, 0, 0), "empty access at start should fit");
    Check(!nxrt::render::ContainsBackingAccess(span, max, 0), "empty access at UINT64_MAX should fail");
    Check(!nxrt::render::ContainsBackingAccess(span, 0, max), "UINT64_MAX size should fail without overflow");

    const std::array zero_size{nxrt::render::GpuCpuRange{0x1000, 0x8000, 0}};
    Check(!nxrt::render::ResolveGpuCpuRange(0x1000, zero_size), "zero-size range should not resolve");
    const std::array wrapped{nxrt::render::GpuCpuRange{max - 3, 0x8000, 16}};
    Check(!nxrt::render::ResolveGpuCpuRange(max - 2, wrapped), "GPU range wrapping UINT64_MAX should fail");
    const std::array cpu_wrapped{nxrt::render::GpuCpuRange{0x1000, max - 3, 16}};
    Check(!nxrt::render::ResolveGpuCpuRange(0x1000, cpu_wrapped), "CPU range wrapping UINT64_MAX should fail");
}

} // namespace

int main() {
    TestReadBounds();
    TestDirectPoolRange();
    TestAdjacentDirectPoolsCoalesce();
    TestOverlappingPoolsKeepOriginalFirstMatch();
    TestVirtualMappingKeepsPriorityOverPool();
    TestAdjacentVirtualMappings();
    TestNoncontiguousVirtualMappingDoesNotCoalesce();
    TestRangeArithmeticHandlesZeroAndUint64Max();
}
