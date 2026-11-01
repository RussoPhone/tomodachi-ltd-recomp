#include "render/texture_descriptor_policy.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

struct GuestBytes {
    std::array<unsigned char, 1024> data{};

    template <typename T>
    void Put(std::uint64_t address, T value) {
        std::memcpy(data.data() + address, &value, sizeof(value));
    }

    bool Read(std::uint64_t address, void* out, std::size_t size) const {
        if (address > data.size() || size > data.size() - address) return false;
        std::memcpy(out, data.data() + address, size);
        return true;
    }
};

int main() {
    using namespace nxrt::render;
    GuestBytes bytes;
    // NVN pool object at64, CPU base256, descriptor offset32 and capacity2.
    bytes.Put<std::uint64_t>(64, 128);
    bytes.Put<std::uint64_t>(72, 32);
    bytes.Put<std::uint32_t>(80, 2);
    bytes.Put<std::uint64_t>(176, 256);
    const auto read = [&](std::uint64_t address, void* out, std::size_t size) {
        return bytes.Read(address, out, size);
    };

    const auto first = ResolveNvnPoolDescriptor(64, 0, read);
    const auto last = ResolveNvnPoolDescriptor(64, 1, read);
    if (first.status != DescriptorPoolStatus::Ready || first.address != 288 ||
        last.status != DescriptorPoolStatus::Ready || last.address != 320) return 1;
    if (ResolveNvnPoolDescriptor(64, 2, read).status != DescriptorPoolStatus::OutOfRange) {
        std::fprintf(stderr, "descriptor at capacity escaped NVN pool bounds\n");
        return 1;
    }
    bytes.Put<std::uint32_t>(80, 0);
    if (ResolveNvnPoolDescriptor(64, 0, read).status != DescriptorPoolStatus::OutOfRange) return 1;
    if (ResolveNvnPoolDescriptor(0, 0, read).status != DescriptorPoolStatus::NoPool ||
        ResolveNvnPoolDescriptor(1008, 0, read).status != DescriptorPoolStatus::Unmapped) return 1;
    bytes.Put<std::uint32_t>(80, 2);
    bytes.Put<std::uint64_t>(176, std::numeric_limits<std::uint64_t>::max() - 16);
    if (ResolveNvnPoolDescriptor(64, 1, read).status != DescriptorPoolStatus::AddressOverflow) return 1;

    if (!InvalidTextureAddress(0, true) || !InvalidTextureAddress(1ull << 40, true) ||
        !InvalidTextureAddress(0x1000, false) || InvalidTextureAddress(0x1000, true)) {
        std::fprintf(stderr, "invalid guest image address was not classified as null\n");
        return 1;
    }
    return 0;
}
