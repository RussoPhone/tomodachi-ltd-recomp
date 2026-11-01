#pragma once

// Linux soft-dirty backend for TextureDirtyTracker (see texture_dirty_tracker.h). Uses
// /proc/self/clear_refs ("4" clears the soft-dirty bit of every page of the process) and bit 55 of
// /proc/self/pagemap. Init() proves on a scratch mapping that the kernel really reports user stores, stores to
// pages never touched before, and kernel writes (read(2)); any failure leaves the backend unusable.

#include "render/texture_dirty_tracker.h"

#if defined(__linux__)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#include <cstdint>

namespace nxrt::render {

class SoftDirtyBackend final : public DirtyBackend {
public:
    SoftDirtyBackend() = default;
    ~SoftDirtyBackend() override {
#if defined(__linux__)
        if (pagemap_ >= 0) ::close(pagemap_);
        if (clear_refs_ >= 0) ::close(clear_refs_);
#endif
    }
    SoftDirtyBackend(const SoftDirtyBackend&) = delete;
    SoftDirtyBackend& operator=(const SoftDirtyBackend&) = delete;

    bool Init() {
#if defined(__linux__)
        page_size_ = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
        pagemap_ = ::open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        clear_refs_ = ::open("/proc/self/clear_refs", O_WRONLY | O_CLOEXEC);
        if (pagemap_ < 0 || clear_refs_ < 0 || page_size_ == 0) return false;
        return SelfTest();
#else
        return false;
#endif
    }

    bool ClearAll() override {
#if defined(__linux__)
        return clear_refs_ >= 0 && ::write(clear_refs_, "4", 1) == 1;
#else
        return false;
#endif
    }

    bool RangeDirty(const void* host, size_t bytes, bool* dirty) override {
#if defined(__linux__)
        if (pagemap_ < 0 || bytes == 0) return false;
        const uintptr_t first = reinterpret_cast<uintptr_t>(host) / page_size_;
        const uintptr_t last = (reinterpret_cast<uintptr_t>(host) + bytes - 1) / page_size_;
        uint64_t entries[512];
        for (uintptr_t page = first; page <= last;) {
            const size_t count = static_cast<size_t>(last - page + 1) < 512 ? static_cast<size_t>(last - page + 1) : 512;
            const ssize_t want = static_cast<ssize_t>(count * sizeof(uint64_t));
            if (::pread(pagemap_, entries, static_cast<size_t>(want), static_cast<off_t>(page * sizeof(uint64_t))) != want) {
                return false;
            }
            for (size_t i = 0; i < count; ++i) {
                if (entries[i] & (uint64_t{1} << 55)) {  // soft-dirty
                    *dirty = true;
                    return true;
                }
            }
            page += count;
        }
        *dirty = false;
        return true;
#else
        (void)host; (void)bytes; (void)dirty;
        return false;
#endif
    }

private:
#if defined(__linux__)
    bool SelfTest() {
        const size_t ps = page_size_;
        void* map = ::mmap(nullptr, ps * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (map == MAP_FAILED) return false;
        volatile char* b = static_cast<volatile char*>(map);
        bool ok = true;
        const auto dirty = [&](size_t page) {
            bool d = true;
            if (!RangeDirty(const_cast<char*>(reinterpret_cast<volatile char*>(b + page * ps)), 1, &d)) ok = false;
            return d;
        };
        b[0] = 1;            // page 0 populated, page 1..3 never touched
        b[ps * 3] = 1;
        ok = ok && ClearAll();
        ok = ok && !dirty(0) && !dirty(1) && !dirty(2) && !dirty(3);  // a clear must clean populated and empty pages
        b[0] = 2;            // user store to a populated page
        b[ps] = 2;           // user store to a page never touched
        ok = ok && dirty(0) && dirty(1) && !dirty(2) && !dirty(3);
        ok = ok && ClearAll() && !dirty(0) && !dirty(1);
        const int zero = ::open("/dev/zero", O_RDONLY | O_CLOEXEC);  // kernel write on our behalf
        if (zero < 0 || ::read(zero, const_cast<char*>(reinterpret_cast<volatile char*>(b + ps * 3)), 16) != 16) ok = false;
        if (zero >= 0) ::close(zero);
        ok = ok && dirty(3) && !dirty(0) && !dirty(2);
        ::munmap(map, ps * 4);
        return ok;
    }

    int pagemap_ = -1;
    int clear_refs_ = -1;
    size_t page_size_ = 0;
#endif
};

}  // namespace nxrt::render
