#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "guest_abi.h"

namespace nxrt::render {

struct GuestMemoryRange {
    std::uint64_t address;
    std::uint64_t size;
};

inline bool GuestMemoryRangesOverlap(GuestMemoryRange a, GuestMemoryRange b) noexcept {
    if (!a.size || !b.size) return false;
    if (a.address <= b.address) return b.address - a.address < a.size;
    return a.address - b.address < b.size;
}

// Page-table entry value 1 keeps GuestMemory::IsMapped true while clearing the
// backing-pointer bits. The generated AOT load helpers then use HostLoad, where
// the opt-in read observer can see accesses to compute writeback pages.
inline std::size_t MarkGuestMemoryRangeSlowPath(const void* page_table, GuestMemoryRange range,
                                                std::uint64_t address_space_max) noexcept {
    if (!page_table || !range.size || range.address >= address_space_max ||
        range.size > address_space_max - range.address) {
        return 0;
    }

    const std::uint64_t first_page = range.address >> nxrt::kPageBits;
    const std::uint64_t last_page = (range.address + range.size - 1) >> nxrt::kPageBits;
    const auto* const bytes = static_cast<const std::byte*>(page_table);
    std::size_t marked = 0;
    for (std::uint64_t page = first_page; page <= last_page; ++page) {
        const std::size_t offset = static_cast<std::size_t>(page << nxrt::kPageEntryStrideLog2);
        auto* const entry = reinterpret_cast<std::uint64_t*>(const_cast<std::byte*>(bytes + offset));
        std::atomic_ref<std::uint64_t> atomic_entry(*entry);
        const std::uint64_t current = atomic_entry.load(std::memory_order_acquire);
        if ((current & nxrt::kPagePointerMask) == 0) continue;
        atomic_entry.store(1, std::memory_order_release);
        ++marked;
    }
    return marked;
}

struct GuestWritebackRecord {
    std::uint64_t program;
    std::uint64_t frame;
    GuestMemoryRange range;
};

// A pending GPU result may back a later read-only SSBO without a guest-memory
// round trip only when the newest overlapping write covers the exact range.
// Newer partial writes would make an older full-range alias stale.
template <typename Writeback, std::size_t Extent>
inline std::optional<std::uint64_t> FindForwardableComputeWriteback(
    std::span<const Writeback, Extent> writebacks, GuestMemoryRange read_range, bool read_only,
    std::uint64_t stream_alignment = 1) noexcept {
    if (!read_only || !read_range.size || !stream_alignment) return std::nullopt;
    for (auto it = writebacks.rbegin(); it != writebacks.rend(); ++it) {
        const GuestMemoryRange write_range{it->cpu, it->size};
        if (!GuestMemoryRangesOverlap(write_range, read_range)) continue;
        if (write_range.address != read_range.address || write_range.size != read_range.size) {
            return std::nullopt;
        }
        return it->stream_offset % stream_alignment == 0 ? std::optional<std::uint64_t>{it->stream_offset}
                                                         : std::nullopt;
    }
    return std::nullopt;
}

class GuestMemoryReadbackTracker {
public:
    void Record(GuestWritebackRecord writeback) {
        if (!writeback.range.size) return;
        records_.erase(std::remove_if(records_.begin(), records_.end(), [&](const GuestWritebackRecord& record) {
                           const bool stale = writeback.frame >= record.frame &&
                                              writeback.frame - record.frame > kRecentFrames;
                           const bool superseded = record.range.address == writeback.range.address &&
                                                   record.range.size == writeback.range.size &&
                                                   record.frame <= writeback.frame;
                           return stale || superseded;
                       }),
                       records_.end());
        if (records_.size() == kMaximumRecords) records_.erase(records_.begin());
        records_.push_back(writeback);
    }

    std::vector<GuestWritebackRecord> FindOverlappingWrites(std::uint64_t frame,
                                                             GuestMemoryRange read_range) const {
        std::vector<GuestWritebackRecord> matches;
        if (!read_range.size) return matches;
        for (const GuestWritebackRecord& record : records_) {
            if (frame >= record.frame && frame - record.frame <= kRecentFrames &&
                GuestMemoryRangesOverlap(record.range, read_range)) {
                matches.push_back(record);
            }
        }
        return matches;
    }

private:
    static constexpr std::uint64_t kRecentFrames = 1;
    static constexpr std::size_t kMaximumRecords = 128;
    std::vector<GuestWritebackRecord> records_;
};

} // namespace nxrt::render
