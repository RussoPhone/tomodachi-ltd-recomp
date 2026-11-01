#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <tuple>
#include <vector>

namespace nxrt::render {

class GuestBufferUploadCache {
public:
    void Store(std::uint64_t address, std::uint64_t alignment, std::span<const std::uint8_t> current,
               std::uint64_t offset) {
        const auto key = std::tuple{address, static_cast<std::uint64_t>(current.size()), alignment};
        auto it = offsets_.find(key);
        if (it == offsets_.end()) {
            const auto snapshot_offset = snapshots_.size();
            snapshots_.insert(snapshots_.end(), current.begin(), current.end());
            offsets_.emplace(key, Entry{offset, snapshot_offset});
            return;
        }

        Entry& entry = it->second;
        if (entry.snapshot_offset <= snapshots_.size() && current.size() <= snapshots_.size() - entry.snapshot_offset) {
            std::copy(current.begin(), current.end(), snapshots_.begin() + entry.snapshot_offset);
        } else {
            entry.snapshot_offset = snapshots_.size();
            snapshots_.insert(snapshots_.end(), current.begin(), current.end());
        }
        entry.offset = offset;
    }

    std::optional<std::uint64_t> Find(std::uint64_t address, std::uint64_t alignment,
                                      std::span<const std::uint8_t> current, std::uint64_t uploaded_size) const {
        const auto it = offsets_.find({address, current.size(), alignment});
        if (it == offsets_.end()) return std::nullopt;
        const Entry& entry = it->second;
        if (entry.offset > uploaded_size || current.size() > uploaded_size - entry.offset) return std::nullopt;
        if (entry.snapshot_offset > snapshots_.size() || current.size() > snapshots_.size() - entry.snapshot_offset)
            return std::nullopt;
        // Compare against a cached host snapshot. Reading the mapped Vulkan upload buffer can be very slow on
        // write-combined host-visible memory, while guest memory may still be rewritten at the same GPU address.
        if (!std::equal(current.begin(), current.end(), snapshots_.begin() + entry.snapshot_offset))
            return std::nullopt;
        return entry.offset;
    }

    void Invalidate() {
        offsets_.clear();
        snapshots_.clear();
    }

private:
    struct Entry {
        std::uint64_t offset;
        std::size_t snapshot_offset;
    };

    std::map<std::tuple<std::uint64_t, std::uint64_t, std::uint64_t>, Entry> offsets_;
    std::vector<std::uint8_t> snapshots_;
};

// Direct-mapped variant for the uniform buffers of a draw: thousands of lookups per frame, so no allocation and no
// ordered map. A slot holds one (address, size) pair; a collision or a changed size simply evicts the older entry,
// which only costs a re-upload. The bytes are always compared with the stored snapshot, so a hit is exact.
class DirectMappedUploadCache {
public:
    explicit DirectMappedUploadCache(std::size_t slots = 8192) : slots_(slots) {}

    std::optional<std::uint64_t> Find(std::uint64_t address, std::span<const std::uint8_t> current,
                                      std::uint64_t uploaded_size) const {
        const Slot& s = slots_[Index(address, current.size())];
        if (!s.valid || s.address != address || s.size != current.size()) return std::nullopt;
        if (s.offset > uploaded_size || current.size() > uploaded_size - s.offset) return std::nullopt;
        if (!std::equal(current.begin(), current.end(), snapshots_.begin() + s.snapshot_offset)) return std::nullopt;
        return s.offset;
    }

    void Store(std::uint64_t address, std::span<const std::uint8_t> current, std::uint64_t offset) {
        Slot& s = slots_[Index(address, current.size())];
        if (s.valid && s.size >= current.size() && s.address == address) {
            std::copy(current.begin(), current.end(), snapshots_.begin() + s.snapshot_offset);
        } else {
            s.snapshot_offset = snapshots_.size();
            snapshots_.insert(snapshots_.end(), current.begin(), current.end());
        }
        s.address = address;
        s.size = current.size();
        s.offset = offset;
        s.valid = true;
    }

    void Invalidate() {
        for (Slot& s : slots_) s.valid = false;
        snapshots_.clear();
    }

private:
    struct Slot {
        std::uint64_t address = 0;
        std::uint64_t size = 0;
        std::uint64_t offset = 0;
        std::size_t snapshot_offset = 0;
        bool valid = false;
    };
    std::size_t Index(std::uint64_t address, std::size_t size) const {
        std::uint64_t h = (address ^ (static_cast<std::uint64_t>(size) << 17)) * 0x9E3779B97F4A7C15ULL;
        return static_cast<std::size_t>(h >> 40) % slots_.size();
    }
    std::vector<Slot> slots_;
    std::vector<std::uint8_t> snapshots_;
};

}  // namespace nxrt::render
