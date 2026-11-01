#include <array>
#include <cstdio>
#include <span>
#include <vector>

#include "gpu/null_gpu.h"
#include "memory.h"
#include "services/nvdrv.h"

namespace {
constexpr u64 kGpuBase = 0x10'0000'0000;
constexpr u64 kGuestBase = 0x100000;
constexpr u64 kMappedSize = 0x10000;
}

// Only NVDRV address translation is replaced. The command decoder, effect
// application and mapped GuestMemory are the same code used by SubmitGpfifo.
namespace nxrt::hle {
bool GpuVaToGuest(u64 gpu_va, u64* guest) {
    if (gpu_va < kGpuBase || gpu_va >= kGpuBase + kMappedSize) return false;
    *guest = kGuestBase + gpu_va - kGpuBase;
    return true;
}
}

namespace {
using namespace nxrt;

bool Require(bool condition, const char* what, int line) {
    if (!condition) std::fprintf(stderr, "FAILED line %d: %s\n", line, what);
    return condition;
}
#define REQUIRE(x) do { if (!Require((x), #x, __LINE__)) return false; } while (false)

// Hand-built Maxwell packet fixtures, in word-addressed method units.
u32 Header(u32 method, u32 count, u32 mode = 1) {
    return (mode << 29) | (count << 16) | method;
}
u32 High(u64 address) { return static_cast<u32>(address >> 32); }
u32 Low(u64 address) { return static_cast<u32>(address); }
u64 Entry(u64 address, size_t count) { return address | (static_cast<u64>(count) << 42); }

void WriteWords(u64 gpu_va, std::span<const u32> words) {
    g_memory.WriteBlock(kGuestBase + gpu_va - kGpuBase, words.data(), words.size_bytes());
}
u32 ReadWord(u64 gpu_va) { return g_memory.Read<u32>(kGuestBase + gpu_va - kGpuBase); }
void WriteWord(u64 gpu_va, u32 word) { g_memory.Write<u32>(kGuestBase + gpu_va - kGpuBase, word); }

void SubmitWords(u32 channel, std::span<const u32> words) {
    constexpr u64 commands = kGpuBase;
    WriteWords(commands, words);
    const u64 entry = Entry(commands, words.size());
    gpu::SubmitGpfifo(channel, &entry, 1);
}

bool CapturedWordsSurviveGuestMutation() {
    constexpr u64 commands = kGpuBase;
    constexpr u64 original = kGpuBase + 0x4000, rewritten = kGpuBase + 0x4010;
    const std::array words{
        Header(4, 3), High(original), Low(original), 0x11223344u,
        Header(0x1B, 1), 0u,
        Header(4, 3), High(original + 4), Low(original + 4), 0x55667788u,
        Header(0x1B, 1), 0u,
    };
    WriteWords(commands, words);
    const u64 entry = Entry(commands, words.size());
    const auto captured = gpu::CaptureGpfifo(0, &entry, 1);
    const std::array replacement{
        Header(4, 3), High(rewritten), Low(rewritten), 0xDEADBEEFu,
        Header(0x1B, 1), 0u,
    };
    WriteWords(commands, replacement);
    gpu::ExecuteGpfifo(captured);
    REQUIRE(ReadWord(original) == 0x11223344);
    REQUIRE(ReadWord(original + 4) == 0x55667788);
    REQUIRE(ReadWord(rewritten) == 0);
    return true;
}

bool ChannelRegistersPersistAcrossSubmits() {
    constexpr u64 source = kGpuBase + 0x4100, destination = kGpuBase + 0x4120;
    WriteWord(source, 0xA1B2C3D4);
    const std::array configure{
        Header(0, 1), 0xB0B5u,
        Header(0x100, 8), High(source), Low(source), High(destination), Low(destination),
        4u, 4u, 4u, 1u,
    };
    SubmitWords(1, configure);
    REQUIRE(ReadWord(destination) == 0);
    const std::array launch{Header(0xC0, 1), 0x181u}; // pitch copy, no semaphore
    SubmitWords(2, launch); // An unbound channel must not inherit channel 1's registers.
    REQUIRE(ReadWord(destination) == 0);
    SubmitWords(1, launch);
    REQUIRE(ReadWord(destination) == 0xA1B2C3D4);
    return true;
}

bool DmaCopyPrecedesSemaphoreRelease() {
    constexpr u64 source = kGpuBase + 0x4200, destination = kGpuBase + 0x4220;
    WriteWord(source, 0x12345678);
    WriteWord(source + 4, 0xCAFEBABE);
    // Semaphore aliases the first copied word: release-before-copy leaves the
    // source value there, while copy-before-release leaves the release payload.
    const std::array words{
        Header(0, 1), 0xB0B5u,
        Header(0x100, 8), High(source), Low(source), High(destination), Low(destination),
        8u, 8u, 8u, 1u,
        Header(0x90, 3), High(destination), Low(destination), 0x87654321u,
        Header(0xC0, 1), 0x189u, // pitch copy + short semaphore
    };
    SubmitWords(3, words);
    REQUIRE(ReadWord(destination) == 0x87654321);
    REQUIRE(ReadWord(destination + 4) == 0xCAFEBABE);
    return true;
}

bool DmaLaunchOwnsRegisterOperands() {
    constexpr u64 source = kGpuBase + 0x4300, destination = kGpuBase + 0x4320;
    WriteWord(source, 0x11221122);
    WriteWord(source + 4, 0x33443344);
    const std::array words{
        Header(0, 1), 0xB0B5u,
        Header(0x100, 8), High(source), Low(source), High(destination), Low(destination),
        4u, 4u, 4u, 1u,
        Header(0xC0, 1), 0x181u,
        Header(0x101, 1), Low(source + 4),
        Header(0x103, 1), Low(destination + 4),
        Header(0xC0, 1), 0x181u,
    };
    SubmitWords(4, words);
    REQUIRE(ReadWord(destination) == 0x11221122);
    REQUIRE(ReadWord(destination + 4) == 0x33443344);
    return true;
}

bool InlineUploadOwnsAddressAndWordOperands() {
    constexpr u64 destination = kGpuBase + 0x4400;
    const std::array words{
        Header(0, 1), 0xA140u,
        Header(0x60, 4), 8u, 1u, High(destination), Low(destination),
        Header(0x6C, 1), 1u,
        Header(0x6D, 2, 3), 0x10203040u, 0x50607080u,
    };
    SubmitWords(5, words);
    REQUIRE(ReadWord(destination) == 0x10203040);
    REQUIRE(ReadWord(destination + 4) == 0x50607080);
    return true;
}

bool EarlierEffectUpdatesLaterArgumentInSameList() {
    constexpr u64 commands = kGpuBase, destination = kGpuBase + 0x4600;
    // The first release overwrites word 9: the later release's payload argument.
    constexpr u64 operand = commands + 9 * sizeof(u32);
    const std::array words{
        Header(4, 3), High(operand), Low(operand), 0xAABBCCDDu,
        Header(0x1B, 1), 0u,
        Header(4, 3), High(destination), Low(destination), 0x11111111u,
        Header(0x1B, 1), 0u,
    };
    SubmitWords(9, words);
    REQUIRE(ReadWord(operand) == 0xAABBCCDD);
    REQUIRE(ReadWord(destination) == 0xAABBCCDD);
    return true;
}

bool CapturedListKeepsOwnedArgumentsDuringEffects() {
    constexpr u64 commands = kGpuBase, destination = kGpuBase + 0x4620;
    constexpr u64 operand = commands + 9 * sizeof(u32);
    const std::array words{
        Header(4, 3), High(operand), Low(operand), 0xAABBCCDDu,
        Header(0x1B, 1), 0u,
        Header(4, 3), High(destination), Low(destination), 0x11111111u,
        Header(0x1B, 1), 0u,
    };
    WriteWords(commands, words);
    const u64 entry = Entry(commands, words.size());
    const auto captured = gpu::CaptureGpfifo(10, &entry, 1);
    gpu::ExecuteGpfifo(captured);
    REQUIRE(ReadWord(operand) == 0xAABBCCDD);
    REQUIRE(ReadWord(destination) == 0x11111111);
    return true;
}

enum class Writer { Inline, Dma, Semaphore };

bool EarlierEffectUpdatesLaterList(Writer writer, u32 channel) {
    constexpr u64 first = kGpuBase, later = kGpuBase + 0x1000;
    constexpr u64 destination = kGpuBase + 0x4500, source = kGpuBase + 0x4520;
    constexpr u32 updated = 0xAABBCCDD;
    WriteWord(destination, 0);
    const std::array later_words{
        Header(4, 3), High(destination), Low(destination), 0x11111111u,
        Header(0x1B, 1), 0u,
    };
    WriteWords(later, later_words);
    const u64 operand = later + 3 * sizeof(u32);
    std::vector<u32> first_words;
    switch (writer) {
    case Writer::Inline:
        first_words = {
            Header(0, 1), 0xA140u,
            Header(0x60, 4), 4u, 1u, High(operand), Low(operand),
            Header(0x6C, 1), 1u,
            Header(0x6D, 1), updated,
        };
        break;
    case Writer::Dma:
        WriteWord(source, updated);
        first_words = {
            Header(0, 1), 0xB0B5u,
            Header(0x100, 8), High(source), Low(source), High(operand), Low(operand),
            4u, 4u, 4u, 1u,
            Header(0xC0, 1), 0x181u,
        };
        break;
    case Writer::Semaphore:
        first_words = {
            Header(4, 3), High(operand), Low(operand), updated,
            Header(0x1B, 1), 0u,
        };
        break;
    }
    WriteWords(first, first_words);
    const std::array entries{Entry(first, first_words.size()), Entry(later, later_words.size())};
    gpu::SubmitGpfifo(channel, entries.data(), entries.size());
    REQUIRE(ReadWord(operand) == updated);
    REQUIRE(ReadWord(destination) == updated);
    return true;
}
}

int main() {
    if (!nxrt::g_memory.Init() ||
        !nxrt::g_memory.Map(kGuestBase, kMappedSize, nxrt::MemState::Normal, nxrt::PermRW)) return 1;
    struct Test { const char* name; bool (*run)(); };
    const std::array tests{
        Test{"captured words survive guest mutation", CapturedWordsSurviveGuestMutation},
        Test{"channel registers persist across submits", ChannelRegistersPersistAcrossSubmits},
        Test{"DMA copy precedes semaphore release", DmaCopyPrecedesSemaphoreRelease},
        Test{"DMA launch owns register operands", DmaLaunchOwnsRegisterOperands},
        Test{"inline upload owns address and word operands", InlineUploadOwnsAddressAndWordOperands},
        Test{"intra-list semaphore write updates later argument", EarlierEffectUpdatesLaterArgumentInSameList},
        Test{"captured list keeps owned arguments during effects", CapturedListKeepsOwnedArgumentsDuringEffects},
        Test{"cross-list I2M write", [] { return EarlierEffectUpdatesLaterList(Writer::Inline, 6); }},
        Test{"cross-list DMA write", [] { return EarlierEffectUpdatesLaterList(Writer::Dma, 7); }},
        Test{"cross-list semaphore write", [] { return EarlierEffectUpdatesLaterList(Writer::Semaphore, 8); }},
    };
    bool passed = true;
    for (const auto& test : tests) {
        const bool result = test.run();
        std::fprintf(stderr, "%s: %s\n", result ? "PASS" : "FAIL", test.name);
        passed &= result;
    }
    return passed ? 0 : 1;
}
