#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <vector>

#include "platform/win32_memory.h"

namespace {

bool fail_next_allocation = false;

struct FakeSection {
    bool live = true;
};

struct FakeView {
    void* base;
    SIZE_T size;
    HANDLE section;
};

struct FakePlaceholder {
    std::uintptr_t begin;
    SIZE_T size;
};

alignas(65536) std::array<unsigned char, 65536> reservation{};
std::array<FakeSection, 2> sections{};
std::vector<FakeView> views;
std::vector<FakePlaceholder> placeholders;
int map_calls = 0;
int duplicate_calls = 0;
int close_calls = 0;
int virtual_free_calls = 0;
int fail_map_call = -1;
int fail_virtual_free_call = -1;
bool fail_duplicate = false;
bool fail_map = false;
bool fail_unmap = false;

void* WINAPI FakeVirtualAlloc2(HANDLE, void* base, SIZE_T size, ULONG, ULONG, void*, ULONG) {
    if (base || size > reservation.size()) return nullptr;
    placeholders = {{reinterpret_cast<std::uintptr_t>(reservation.data()), size}};
    return reservation.data();
}

void* WINAPI FakeMapViewOfFile3(HANDLE section, HANDLE, void* base, ULONG64 offset, SIZE_T size,
                                ULONG allocation_type, ULONG, void*, ULONG) {
    ++map_calls;
    if (fail_map) {
        fail_map = false;
        return nullptr;
    }
    if (map_calls == fail_map_call) return nullptr;
    const auto* fake = static_cast<FakeSection*>(section);
    if (!fake || !fake->live) return nullptr;
    constexpr std::uintptr_t page_size = 0x1000;
    const auto base_value = reinterpret_cast<std::uintptr_t>(base);
    if (!(allocation_type & MEM_REPLACE_PLACEHOLDER) || (base_value & (page_size - 1)) ||
        (offset & (page_size - 1)) || (size & (page_size - 1))) {
        return nullptr;
    }
    for (const FakeView& view : views) {
        const auto view_begin = reinterpret_cast<std::uintptr_t>(view.base);
        const auto view_end = view_begin + view.size;
        const auto new_begin = base_value;
        const auto new_end = new_begin + size;
        if (new_begin < view_end && view_begin < new_end) return nullptr;
    }
    auto placeholder = std::find_if(placeholders.begin(), placeholders.end(), [&](const auto& range) {
        return range.begin == base_value && range.size == size;
    });
    if (placeholder == placeholders.end()) return nullptr;
    placeholders.erase(placeholder);
    views.push_back({base, size, section});
    return base;
}

BOOL WINAPI FakeUnmapViewOfFile2(HANDLE, void* base, ULONG) {
    if (fail_unmap) {
        fail_unmap = false;
        return 0;
    }
    for (auto it = views.begin(); it != views.end(); ++it) {
        if (it->base == base) {
            placeholders.push_back({reinterpret_cast<std::uintptr_t>(it->base), it->size});
            views.erase(it);
            return 1;
        }
    }
    return 0;
}

void* FakeProcAddress(const char* name) {
    if (std::string_view(name) == "VirtualAlloc2") {
        return reinterpret_cast<void*>(&FakeVirtualAlloc2);
    }
    if (std::string_view(name) == "MapViewOfFile3") {
        return reinterpret_cast<void*>(&FakeMapViewOfFile3);
    }
    if (std::string_view(name) == "UnmapViewOfFile2") {
        return reinterpret_cast<void*>(&FakeUnmapViewOfFile2);
    }
    return nullptr;
}

bool Check(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
    return condition;
}

bool ReservationIsCovered() {
    std::vector<FakePlaceholder> ranges = placeholders;
    for (const FakeView& view : views) {
        ranges.push_back({reinterpret_cast<std::uintptr_t>(view.base), view.size});
    }
    std::sort(ranges.begin(), ranges.end(), [](const auto& left, const auto& right) {
        return left.begin < right.begin;
    });
    auto next = reinterpret_cast<std::uintptr_t>(reservation.data());
    for (const auto& range : ranges) {
        if (range.begin != next) return false;
        next += range.size;
    }
    return next == reinterpret_cast<std::uintptr_t>(reservation.data()) + reservation.size();
}

} // namespace

void* operator new(std::size_t size) {
    if (fail_next_allocation) {
        fail_next_allocation = false;
        throw std::bad_alloc();
    }
    if (void* p = std::malloc(size)) return p;
    throw std::bad_alloc();
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

extern "C" HMODULE GetModuleHandleW(const wchar_t*) { return reinterpret_cast<HMODULE>(1); }
extern "C" void* GetProcAddress(HMODULE, const char* name) { return FakeProcAddress(name); }
extern "C" HANDLE GetCurrentProcess() { return reinterpret_cast<HANDLE>(2); }
extern "C" BOOL DuplicateHandle(HANDLE, HANDLE source, HANDLE, LPHANDLE target, DWORD, BOOL,
                                 DWORD options) {
    ++duplicate_calls;
    if (fail_duplicate) {
        fail_duplicate = false;
        return 0;
    }
    if (options != DUPLICATE_SAME_ACCESS || source != &sections[0] || !sections[0].live) return 0;
    sections[1].live = true;
    *target = &sections[1];
    return 1;
}
extern "C" BOOL CloseHandle(HANDLE handle) {
    ++close_calls;
    auto* section = static_cast<FakeSection*>(handle);
    if (!section) return 0;
    section->live = false;
    return 1;
}
extern "C" BOOL VirtualFree(void* address, SIZE_T size, DWORD type) {
    ++virtual_free_calls;
    if (virtual_free_calls == fail_virtual_free_call) return 0;
    const auto begin = reinterpret_cast<std::uintptr_t>(address);
    if (type & MEM_PRESERVE_PLACEHOLDER) {
        for (auto it = placeholders.begin(); it != placeholders.end(); ++it) {
            if (it->begin != begin || size >= it->size) continue;
            const SIZE_T remainder = it->size - size;
            it->size = size;
            placeholders.push_back({begin + size, remainder});
            return 1;
        }
        return 0;
    }
    if (type & MEM_COALESCE_PLACEHOLDERS) {
        const auto end = begin + size;
        std::vector<FakePlaceholder> merged;
        for (const auto& placeholder : placeholders) {
            const auto placeholder_end = placeholder.begin + placeholder.size;
            if (placeholder.begin < end && begin < placeholder_end) {
                if (placeholder.begin < begin || placeholder_end > end) return 0;
                merged.push_back(placeholder);
            }
        }
        if (merged.empty()) return 0;
        std::size_t covered = 0;
        for (const auto& placeholder : merged) covered += placeholder.size;
        if (covered != size) return 0;
        placeholders.erase(std::remove_if(placeholders.begin(), placeholders.end(), [&](const auto& p) {
                              return p.begin >= begin && p.begin + p.size <= end;
                          }),
                          placeholders.end());
        placeholders.push_back({begin, size});
        return 1;
    }
    return 0;
}
extern "C" void* VirtualAlloc(void*, SIZE_T, DWORD, DWORD) { return nullptr; }
extern "C" void* AddVectoredExceptionHandler(ULONG, LONG(CALLBACK*)(EXCEPTION_POINTERS*)) {
    return reinterpret_cast<void*>(1);
}
extern "C" DWORD GetLastError() { return 0; }

namespace nxrt {
void Log(LogLevel, const char*, ...) {}
bool LogEnabled(LogLevel) { return false; }
[[noreturn]] void Fatal(const char*, ...) { std::abort(); }
} // namespace nxrt

int main() {
    nxrt::win32::HostSpace space;
    if (!Check(space.Init(0x10000), "reserve fake guest range")) return 1;

    const int closes_before_duplicate_failure = close_calls;
    fail_duplicate = true;
    if (!Check(!space.MapView(0x1000, 0x1000, &sections[0], 0),
               "reject a failed DuplicateHandle")) {
        return 1;
    }
    if (!Check(close_calls == closes_before_duplicate_failure,
               "do not close a handle when duplication itself failed")) {
        return 1;
    }

    const int closes_before_allocation_failure = close_calls;
    fail_next_allocation = true;
    bool allocation_failed = false;
    try {
        (void)space.MapView(0x1000, 0x1000, &sections[0], 0);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    }
    if (!Check(allocation_failed, "inject shared-handle allocation failure")) return 1;
    if (!Check(close_calls == closes_before_allocation_failure + 1,
               "duplicated section handle closes if shared ownership allocation fails")) {
        return 1;
    }
    if (!Check(views.empty(), "allocation failure does not create a section view")) return 1;

    const int closes_before_map_failure = close_calls;
    fail_map = true;
    if (!Check(!space.MapView(0x1000, 0x1000, &sections[0], 0),
               "report a failed MapViewOfFile3")) {
        return 1;
    }
    if (!Check(close_calls == closes_before_map_failure + 1,
               "close duplicated section handle if mapping fails")) {
        return 1;
    }
    if (!Check(views.empty(), "failed map leaves no section view")) return 1;

    // A guest range aligned to 4 KiB but not 64 KiB can be mapped directly
    // when MEM_REPLACE_PLACEHOLDER is used.
    const HANDLE original = &sections[0];
    if (!Check(space.MapView(0x1000, 0x4000, original, 0),
               "map a 4 KiB-aligned replacement view")) {
        return 1;
    }
    if (!Check(views.size() == 1, "initial section view was created")) return 1;

    // The caller may close its mapping-object handle after MapView. A partial
    // replacement still has to recreate the portions outside the changed range.
    if (!Check(CloseHandle(original), "close caller's section handle")) return 1;
    fail_unmap = true;
    space.Release(0x2000, 0x1000);
    if (!Check(views.size() == 1, "unmap failure keeps the original view mapped")) return 1;
    if (!Check(ReservationIsCovered(), "failed unmap leaves fake guest reservation covered")) return 1;
    if (!Check(sections[1].live, "unmap failure retains the section handle")) return 1;

    fail_virtual_free_call = virtual_free_calls + 1;
    space.Release(0x2000, 0x1000);
    if (!Check(views.empty(), "split failure leaves the original view unmapped")) return 1;
    if (!Check(ReservationIsCovered(), "split failure leaves placeholders covering the guest range")) {
        return 1;
    }
    if (!Check(sections[1].live, "split failure retains the section for recovery")) return 1;
    fail_map_call = map_calls + 1;
    space.Release(0x0000, 0x1000);
    if (!Check(views.empty(), "failed retry leaves pending fragments unmapped")) return 1;
    if (!Check(ReservationIsCovered(), "failed remap leaves placeholders covering the guest range")) {
        return 1;
    }
    if (!Check(sections[1].live, "failed retry retains the section handle")) return 1;
    space.Release(0x0000, 0x1000);
    if (!Check(views.size() == 2, "retry restores both surviving view fragments")) return 1;
    if (!Check(ReservationIsCovered(), "successful retry restores a fully covered guest range")) {
        return 1;
    }
    if (!Check(map_calls == 5, "retry attempted and restored both view fragments")) return 1;
    if (!Check(duplicate_calls == 4, "mapping retained an owned section handle")) return 1;
    if (!Check(sections[1].live, "owned section handle survives caller close")) return 1;

    fail_virtual_free_call = virtual_free_calls + 2;
    space.Release(0x1000, 0x3000);
    if (!Check(views.empty(), "coalesce failure defers the surviving view fragment")) return 1;
    if (!Check(ReservationIsCovered(), "coalesce failure leaves a covered guest reservation")) {
        return 1;
    }
    if (!Check(sections[1].live, "coalesce failure retains the section for recovery")) return 1;

    space.Release(0x0000, 0x1000);
    if (!Check(views.size() == 1, "retry restores the fragment after coalesce failure")) return 1;
    if (!Check(reinterpret_cast<std::uintptr_t>(views[0].base) ==
                   reinterpret_cast<std::uintptr_t>(reservation.data()) + 0x4000,
               "coalesce retry restores the original surviving address")) {
        return 1;
    }
    if (!Check(map_calls == 6, "coalesce retry performs the deferred mapping")) return 1;
    if (!Check(ReservationIsCovered(), "coalesce retry keeps the guest reservation covered")) {
        return 1;
    }

    space.Release(0x4000, 0x1000);
    if (!Check(views.empty(), "full release removed all views")) return 1;
    if (!Check(!sections[1].live, "owned section handle closes after last view")) return 1;
    return 0;
}
