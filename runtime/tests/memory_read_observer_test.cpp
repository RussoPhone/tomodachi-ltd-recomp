#include "memory.h"
#include "render/guest_memory_range.h"

#include <array>
#include <cstdlib>

namespace {
struct Observation {
    u64 address = 0;
    u64 size = 0;
    const char* file = nullptr;
    std::uint_least32_t line = 0;
    int calls = 0;
};

void Observe(void* context, u64 address, u64 size, const char* file, std::uint_least32_t line) noexcept {
    auto& observation = *static_cast<Observation*>(context);
    observation.address = address;
    observation.size = size;
    observation.file = file;
    observation.line = line;
    ++observation.calls;
}
}

int main() {
    nxrt::GuestMemory memory;
    if (!memory.Init() || !memory.Map(0x100000, 0x1000, nxrt::MemState::Normal, nxrt::PermRW)) return EXIT_FAILURE;

    Observation observation;
    memory.SetReadObserver(Observe, &observation);
    memory.Write<u32>(0x100000, 0x12345678);
    if (memory.Read<u32>(0x100000) != 0x12345678 || observation.calls != 1 ||
        observation.address != 0x100000 || observation.size != sizeof(u32) || !observation.file ||
        observation.line == 0) {
        return EXIT_FAILURE;
    }

    std::array<u8, 4> bytes{};
    memory.ReadBlock(0x100000, bytes.data(), bytes.size());
    if (observation.calls != 2 || observation.address != 0x100000 || observation.size != bytes.size() ||
        bytes[0] != 0x78 || bytes[3] != 0x12) {
        return EXIT_FAILURE;
    }

    const std::array<u8, 3> string_bytes{'h', 'i', 0};
    memory.WriteBlock(0x100100, string_bytes.data(), string_bytes.size());
    if (memory.ReadString(0x100100, 8) != "hi" || observation.calls != 3 ||
        observation.address != 0x100100 || observation.size != string_bytes.size()) {
        return EXIT_FAILURE;
    }

    if (nxrt::render::MarkGuestMemoryRangeSlowPath(memory.PageTable(), {0x100000, sizeof(u32)},
                                                   nxrt::AddressSpaceLayout::kEnd) != 1 ||
        !memory.IsMapped(0x100000, sizeof(u32))) {
        return EXIT_FAILURE;
    }
    const auto& host_mem = memory.HostMem();
    if (host_mem.excl_load(host_mem.user, 0x100000, sizeof(u32)) != 0x12345678 || observation.calls != 4 ||
        observation.address != 0x100000 || observation.size != sizeof(u32)) {
        return EXIT_FAILURE;
    }
    u64 lo = 0;
    u64 hi = 0;
    host_mem.excl_load_pair(host_mem.user, 0x100010, sizeof(u32), &lo, &hi);
    if (observation.calls != 5 || observation.address != 0x100010 || observation.size != 2 * sizeof(u32)) {
        return EXIT_FAILURE;
    }
    if (host_mem.load(host_mem.user, 0x100000, sizeof(u32)) != 0x12345678 || observation.calls != 6 ||
        observation.address != 0x100000 || observation.size != sizeof(u32)) {
        return EXIT_FAILURE;
    }

    memory.SetReadObserver(nullptr, nullptr);
    (void)memory.Read<u32>(0x100000);
    if (observation.calls != 6) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
