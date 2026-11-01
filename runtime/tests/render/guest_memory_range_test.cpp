#include <render/guest_memory_range.h>

#include <array>
#include <cstdlib>

using nxrt::render::GuestMemoryRange;
using nxrt::render::GuestMemoryRangesOverlap;
using nxrt::render::MarkGuestMemoryRangeSlowPath;

struct TestComputeWriteback {
    std::uint64_t cpu;
    std::size_t size;
    std::uint64_t stream_offset;
};

int main() {
    const auto require = [](bool condition) {
        if (!condition) std::abort();
    };

    require(GuestMemoryRangesOverlap({0x1000, 0x40}, {0x1020, 0x10}));
    require(GuestMemoryRangesOverlap({0x1020, 0x10}, {0x1000, 0x40}));
    require(GuestMemoryRangesOverlap({0x1000, 0x40}, {0x1000, 0x40}));
    require(!GuestMemoryRangesOverlap({0x1000, 0x40}, {0x1040, 0x10}));
    require(!GuestMemoryRangesOverlap({0x1000, 0}, {0x1000, 0x10}));
    require(GuestMemoryRangesOverlap({UINT64_MAX - 3, 8}, {UINT64_MAX, 1}));
    require(!GuestMemoryRangesOverlap({UINT64_MAX - 3, 8}, {0, 1}));

    alignas(std::uint64_t) std::array<std::uint64_t, 16> page_table{};
    page_table[0] = 0x100000;
    page_table[4] = 0x200000;
    page_table[12] = 1; // already on the callback slow path
    const auto marked = MarkGuestMemoryRangeSlowPath(page_table.data(), {4095, 2}, 4 * 4096);
    require(marked == 2);
    require(page_table[0] == 1 && page_table[4] == 1);
    require(page_table[8] == 0 && page_table[12] == 1);
    require(MarkGuestMemoryRangeSlowPath(page_table.data(), {4095, 2}, 4 * 4096) == 0);
    require(MarkGuestMemoryRangeSlowPath(page_table.data(), {0, 0}, 4 * 4096) == 0);
    require(MarkGuestMemoryRangeSlowPath(page_table.data(), {16383, 2}, 4 * 4096) == 0);

    constexpr std::array exact_then_newer_partial{
        TestComputeWriteback{0x3000, 0x100, 0x8000},
        TestComputeWriteback{0x3040, 0x20, 0x9000},
    };
    require(!nxrt::render::FindForwardableComputeWriteback(std::span{exact_then_newer_partial},
                                                           {0x3000, 0x100}, true));

    constexpr std::array partial_then_exact{
        TestComputeWriteback{0x3000, 0x20, 0x7000},
        TestComputeWriteback{0x3000, 0x100, 0x8000},
    };
    const auto partial_then_exact_span = std::span{partial_then_exact};
    const auto forwarded = nxrt::render::FindForwardableComputeWriteback(partial_then_exact_span,
                                                                         {0x3000, 0x100}, true);
    require(forwarded && *forwarded == 0x8000);
    constexpr std::array misaligned{TestComputeWriteback{0x3000, 0x100, 0x8008}};
    require(!nxrt::render::FindForwardableComputeWriteback(std::span{misaligned},
                                                            {0x3000, 0x100}, true, 0x10));
    require(!nxrt::render::FindForwardableComputeWriteback(partial_then_exact_span, {0x3000, 0x100}, false));
    require(!nxrt::render::FindForwardableComputeWriteback(partial_then_exact_span, {0x3000, 0x80}, true));
    require(!nxrt::render::FindForwardableComputeWriteback(partial_then_exact_span, {0x5000, 0x10}, true));
}
