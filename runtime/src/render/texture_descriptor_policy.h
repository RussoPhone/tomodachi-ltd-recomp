#pragma once

#include <cstdint>
#include <limits>

namespace nxrt::render {

enum class DescriptorPoolStatus { Ready, NoPool, Unmapped, OutOfRange, AddressOverflow };

struct DescriptorPoolAddress {
    DescriptorPoolStatus status = DescriptorPoolStatus::Unmapped;
    std::uint64_t address = 0;
    std::uint32_t count = 0;
};

template <typename Read>
DescriptorPoolAddress ResolveNvnPoolDescriptor(std::uint64_t object, std::uint32_t index, Read&& read) {
    if (!object) return {DescriptorPoolStatus::NoPool};
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (object > maximum - 19) return {DescriptorPoolStatus::AddressOverflow};
    std::uint64_t pool = 0, offset = 0, base = 0;
    std::uint32_t count = 0;
    if (!read(object, &pool, 8) || !read(object + 8, &offset, 8) || !read(object + 16, &count, 4)) return {};
    // NVN GetSize and Initialize both use the descriptor count at object+0x10.
    if (index >= count) return {DescriptorPoolStatus::OutOfRange, 0, count};
    if (!pool) return {DescriptorPoolStatus::NoPool, 0, count};
    if (pool > maximum - 0x37) return {DescriptorPoolStatus::AddressOverflow, 0, count};
    if (!read(pool + 0x30, &base, 8)) return {DescriptorPoolStatus::Unmapped, 0, count};
    if (offset > maximum - base) return {DescriptorPoolStatus::AddressOverflow, 0, count};
    base += offset;
    const auto descriptor_offset = std::uint64_t{index} * 32;
    if (descriptor_offset > maximum - base || base + descriptor_offset > maximum - 31)
        return {DescriptorPoolStatus::AddressOverflow, 0, count};
    return {DescriptorPoolStatus::Ready, base + descriptor_offset, count};
}

inline bool InvalidTextureAddress(std::uint64_t address, bool gpu_mapped) {
    return address == 0 || address >= (std::uint64_t{1} << 40) || !gpu_mapped;
}

}  // namespace nxrt::render
