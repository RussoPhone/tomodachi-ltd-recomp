// SPDX-License-Identifier: GPL-3.0-or-later
// Windows backing for the guest address space (included only when _WIN32 is
// defined; see memory.cpp). Windows has no MAP_FIXED over an existing mapping,
// so the 512 GiB reservation is one *placeholder* (Windows 10 1803+), and every
// later change splits placeholders, replaces them with a private allocation or a
// section view, and turns them back into placeholders on unmap.
//
// UNTESTED: this was written without a Windows machine; see
// docs/reports/AGENTE-F.md for the list of what has to be checked.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "common.h"

namespace nxrt::win32 {

// Defined here because older mingw-w64 headers lack them.
#ifndef MEM_RESERVE_PLACEHOLDER
#define MEM_RESERVE_PLACEHOLDER 0x00040000
#endif
#ifndef MEM_REPLACE_PLACEHOLDER
#define MEM_REPLACE_PLACEHOLDER 0x00004000
#endif
#ifndef MEM_PRESERVE_PLACEHOLDER
#define MEM_PRESERVE_PLACEHOLDER 0x00000002
#endif
#ifndef MEM_COALESCE_PLACEHOLDERS
#define MEM_COALESCE_PLACEHOLDERS 0x00000001
#endif

class HostSpace {
public:
    // Reserve [base, base+size) as a single placeholder.
    bool Init(u64 size) {
        HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
        if (!kb) return false;
        virtual_alloc2_ = reinterpret_cast<VirtualAlloc2Fn>(
            reinterpret_cast<void*>(GetProcAddress(kb, "VirtualAlloc2")));
        map_view3_ = reinterpret_cast<MapViewOfFile3Fn>(
            reinterpret_cast<void*>(GetProcAddress(kb, "MapViewOfFile3")));
        unmap_view2_ = reinterpret_cast<UnmapViewOfFile2Fn>(
            reinterpret_cast<void*>(GetProcAddress(kb, "UnmapViewOfFile2")));
        if (!virtual_alloc2_ || !map_view3_ || !unmap_view2_) return false;
        void* p = virtual_alloc2_(GetCurrentProcess(), nullptr, size,
                                  MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0);
        if (!p) return false;
        base_ = static_cast<u8*>(p);
        pieces_[0] = Piece{size, Kind::Placeholder, nullptr, 0};
        return true;
    }

    u8* Base() const { return base_; }

    // Make [off, off+size) readable/writable zeroed memory. Parts that are
    // already private memory keep their contents (like mprotect on Linux).
    bool Commit(u64 off, u64 size) {
        std::lock_guard lock(mutex_);
        const u64 end = off + size;
        // Collect the sub-ranges that are not private yet before changing anything.
        std::map<u64, u64> todo;
        for (auto it = Containing(off); it != pieces_.end() && it->first < end; ++it) {
            const u64 s = std::max(it->first, off);
            const u64 e = std::min(it->first + it->second.size, end);
            if (it->second.kind == Kind::Private) {
                continue;
            }
            if (!todo.empty() && std::prev(todo.end())->first + std::prev(todo.end())->second == s) {
                std::prev(todo.end())->second += e - s;
            } else {
                todo[s] = e - s;
            }
        }
        for (auto& [s, n] : todo) {
            if (!PrepareLocked(s, n)) return false;
            void* p = virtual_alloc2_(GetCurrentProcess(), base_ + s, n,
                                      MEM_RESERVE | MEM_COMMIT | MEM_REPLACE_PLACEHOLDER,
                                      PAGE_READWRITE, nullptr, 0);
            if (!p) {
                LOG_ERROR("VirtualAlloc2 commit failed at +0x%llx size 0x%llx (error %lu)",
                          (unsigned long long)s, (unsigned long long)n, GetLastError());
                return false;
            }
            pieces_[s] = Piece{n, Kind::Private, nullptr, 0};
        }
        return true;
    }

    // Map `section` (a file-mapping HANDLE) at [off, off+size). MapViewOfFile3
    // with MEM_REPLACE_PLACEHOLDER accepts page-aligned base addresses and
    // offsets. Keep an owned handle because a partial replacement can later
    // unmap and recreate the surviving view fragments after the caller closes
    // its handle.
    bool MapView(u64 off, u64 size, HANDLE section, u64 file_off) {
        std::lock_guard lock(mutex_);
        HANDLE owned = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), section, GetCurrentProcess(), &owned, 0, FALSE,
                             DUPLICATE_SAME_ACCESS)) {
            LOG_ERROR("DuplicateHandle for shared memory failed (error %lu)", GetLastError());
            return false;
        }
        SectionHandle duplicate_guard(owned);
        auto section_ref = std::make_shared<SectionHandle>(std::move(duplicate_guard));
        if (!PrepareLocked(off, size)) return false;
        if (!MapExact(off, size, section_ref, file_off)) return false;
        return true;
    }

    // Turn [off, off+size) back into inaccessible placeholders.
    void Release(u64 off, u64 size) {
        std::lock_guard lock(mutex_);
        PrepareLocked(off, size);
    }

private:
    enum class Kind { Placeholder, Private, View };
    struct SectionHandle {
        explicit SectionHandle(HANDLE value) : handle(value) {}
        SectionHandle(const SectionHandle&) = delete;
        SectionHandle& operator=(const SectionHandle&) = delete;
        SectionHandle(SectionHandle&& other) noexcept : handle(std::exchange(other.handle, nullptr)) {}
        ~SectionHandle() {
            if (handle) CloseHandle(handle);
        }
        HANDLE handle;
    };
    struct Piece {
        u64 size;
        Kind kind;
        std::shared_ptr<SectionHandle> section; // View only; keeps remaps valid
        u64 file_off;   // View only: section offset of the piece's first byte
    };
    struct Remap {
        u64 off, size;
        std::shared_ptr<SectionHandle> section;
        u64 file_off;
    };

    using VirtualAlloc2Fn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, ULONG, void*, ULONG);
    using MapViewOfFile3Fn = PVOID(WINAPI*)(HANDLE, HANDLE, PVOID, ULONG64, SIZE_T, ULONG, ULONG,
                                            void*, ULONG);
    using UnmapViewOfFile2Fn = BOOL(WINAPI*)(HANDLE, PVOID, ULONG);

    // First piece that overlaps `off`.
    std::map<u64, Piece>::iterator Containing(u64 off) {
        auto it = pieces_.upper_bound(off);
        if (it != pieces_.begin()) --it;
        return it;
    }

    bool MapExact(u64 off, u64 size, const std::shared_ptr<SectionHandle>& section, u64 file_off) {
        void* p = map_view3_(section->handle, GetCurrentProcess(), base_ + off, file_off, size,
                             MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
        if (!p) {
            LOG_ERROR("MapViewOfFile3 failed at +0x%llx size 0x%llx (error %lu)",
                      (unsigned long long)off, (unsigned long long)size, GetLastError());
            return false;
        }
        pieces_[off] = Piece{size, Kind::View, section, file_off};
        return true;
    }

    // Split the placeholder that contains `at` so a piece starts exactly there.
    bool SplitPlaceholder(u64 at) {
        auto it = Containing(at);
        if (it == pieces_.end() || it->first == at || it->first + it->second.size <= at) return true;
        if (it->second.kind != Kind::Placeholder) return false;
        const u64 start = it->first;
        const Piece whole = it->second;
        if (!VirtualFree(base_ + start, at - start, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
            LOG_ERROR("placeholder split failed at +0x%llx (error %lu)", (unsigned long long)at,
                      GetLastError());
            return false;
        }
        it->second.size = at - start;
        pieces_[at] = Piece{whole.size - (at - start), Kind::Placeholder, nullptr, 0};
        return true;
    }

    // Retry view fragments left as placeholders by a previous failed
    // replacement. Keep their section references until each remap succeeds.
    bool RetryPendingLocked() {
        std::size_t completed = 0;
        for (; completed < pending_remaps_.size(); ++completed) {
            const Remap& r = pending_remaps_[completed];
            if (!SplitPlaceholder(r.off) || !SplitPlaceholder(r.off + r.size) ||
                !MapExact(r.off, r.size, r.section, r.file_off)) {
                pending_remaps_.erase(pending_remaps_.begin(),
                                      pending_remaps_.begin() + completed);
                return false;
            }
        }
        pending_remaps_.clear();
        return true;
    }

    // After this, [off, off+size) is exactly one placeholder and every part of an
    // existing view or private allocation that lies outside it is still mapped.
    bool PrepareLocked(u64 off, u64 size) {
        if (!RetryPendingLocked()) return false;

        const u64 end = off + size;
        std::vector<Remap> remaps;
        const auto defer_remaps = [this, &remaps] { pending_remaps_ = std::move(remaps); };

        // 1. Everything overlapping the range becomes a placeholder.
        std::vector<u64> keys;
        for (auto it = Containing(off); it != pieces_.end() && it->first < end; ++it) {
            if (it->first + it->second.size > off) keys.push_back(it->first);
        }
        remaps.reserve(keys.size() * 2);
        for (u64 key : keys) {
            const Piece piece = pieces_[key];
            const u64 pend = key + piece.size;
            if (piece.kind == Kind::Placeholder) continue;
            if (piece.kind == Kind::Private) {
                // A part of a private allocation can be freed in place.
                const u64 s = std::max(key, off);
                const u64 e = std::min(pend, end);
                if (!VirtualFree(base_ + s, e - s, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_ERROR("VirtualFree(preserve) failed at +0x%llx (error %lu)",
                              (unsigned long long)s, GetLastError());
                    defer_remaps();
                    return false;
                }
                pieces_.erase(key);
                if (s > key) pieces_[key] = Piece{s - key, Kind::Private, nullptr, 0};
                pieces_[s] = Piece{e - s, Kind::Placeholder, nullptr, 0};
                if (e < pend) pieces_[e] = Piece{pend - e, Kind::Private, nullptr, 0};
            } else {
                // A view can only be unmapped whole; whatever sticks out of the
                // range is mapped again afterwards.
                if (!unmap_view2_(GetCurrentProcess(), base_ + key, MEM_PRESERVE_PLACEHOLDER)) {
                    LOG_ERROR("UnmapViewOfFile2 failed at +0x%llx (error %lu)",
                              (unsigned long long)key, GetLastError());
                    defer_remaps();
                    return false;
                }
                pieces_[key] = Piece{piece.size, Kind::Placeholder, nullptr, 0};
                if (key < off) remaps.push_back({key, off - key, piece.section, piece.file_off});
                if (pend > end) {
                    remaps.push_back({end, pend - end, piece.section, piece.file_off + (end - key)});
                }
            }
        }

        // 2. Cut the placeholders at both ends, then merge them into one.
        if (!SplitPlaceholder(off) || !SplitPlaceholder(end)) {
            defer_remaps();
            return false;
        }
        auto first = pieces_.find(off);
        if (first == pieces_.end()) {
            defer_remaps();
            return false;
        }
        auto last = pieces_.lower_bound(end);
        if (std::next(first) != last) {
            if (!VirtualFree(base_ + off, size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS)) {
                LOG_ERROR("placeholder coalesce failed at +0x%llx (error %lu)",
                          (unsigned long long)off, GetLastError());
                defer_remaps();
                return false;
            }
            pieces_.erase(std::next(first), last);
            first->second.size = size;
        }

        // 3. Put back the parts of split views that stay mapped.
        std::size_t completed = 0;
        for (; completed < remaps.size(); ++completed) {
            const Remap& r = remaps[completed];
            if (!SplitPlaceholder(r.off) || !SplitPlaceholder(r.off + r.size) ||
                !MapExact(r.off, r.size, r.section, r.file_off)) {
                remaps.erase(remaps.begin(), remaps.begin() + completed);
                defer_remaps();
                return false;
            }
        }
        return true;
    }

    u8* base_ = nullptr;
    std::map<u64, Piece> pieces_; // covers the whole reservation, non-overlapping
    std::vector<Remap> pending_remaps_;
    std::mutex mutex_;
    VirtualAlloc2Fn virtual_alloc2_ = nullptr;
    MapViewOfFile3Fn map_view3_ = nullptr;
    UnmapViewOfFile2Fn unmap_view2_ = nullptr;
};

// The guest page table is ~4 GiB of mostly zero entries. Committing it up front
// would burn commit charge, so it is reserved and committed in 1 MiB pieces on
// first touch, from SetPageEntries and, for reads of an untouched range, from
// a vectored exception handler (the generated code reads entries directly).
class LazyTable {
public:
    static constexpr u64 kChunk = u64{1} << 20;

    bool Init(u64 size) {
        base_ = static_cast<u8*>(VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_READWRITE));
        if (!base_) return false;
        size_ = size;
        instance_ = this;
        handler_ = AddVectoredExceptionHandler(1, &LazyTable::OnException);
        return handler_ != nullptr;
    }

    u8* Base() const { return base_; }

    // Commit what covers [off, off+len). Committing twice does not zero.
    bool Touch(u64 off, u64 len) {
        const u64 first = off & ~(kChunk - 1);
        const u64 last = (off + len + kChunk - 1) & ~(kChunk - 1);
        return VirtualAlloc(base_ + first, last - first, MEM_COMMIT, PAGE_READWRITE) != nullptr;
    }

private:
    static LONG CALLBACK OnException(EXCEPTION_POINTERS* info) {
        if (!instance_ || info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        const auto addr = static_cast<u64>(info->ExceptionRecord->ExceptionInformation[1]);
        const auto lo = reinterpret_cast<u64>(instance_->base_);
        if (addr < lo || addr >= lo + instance_->size_) return EXCEPTION_CONTINUE_SEARCH;
        return instance_->Touch(addr - lo, 1) ? EXCEPTION_CONTINUE_EXECUTION
                                              : EXCEPTION_CONTINUE_SEARCH;
    }

    u8* base_ = nullptr;
    u64 size_ = 0;
    void* handler_ = nullptr;
    static inline LazyTable* instance_ = nullptr;
};

} // namespace nxrt::win32
