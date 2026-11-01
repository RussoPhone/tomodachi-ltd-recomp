#pragma once

// Opt-in (NXRT_TEXTURE_DIRTY_TRACKING=1) shortcut for the per-frame re-hash of guest texture memory.
//
// The renderer re-reads the swizzled bytes of every sampled block-linear texture each frame to notice that the
// guest rewrote them. This tracker lets it skip that read when the host kernel can prove nothing wrote to the
// pages since the content was last verified. The proof is the kernel's soft-dirty bit of the host page table
// entry, which is set by *any* write through the guest mapping: recompiled guest stores (including the direct
// fastmem ones), memcpy from the HLE services and the renderer, and writes the kernel does on our behalf
// (read(2) into guest memory). It does not rely on the runtime reporting its writes.
//
// Protocol (single renderer thread):
//   BeginFrame()   once per frame; every `clear_interval` frames it clears all soft-dirty bits and starts a new
//                  epoch. A clear erases the evidence of every texture, so a stamp is only valid for the epoch it
//                  was taken in: after a clear each texture is hashed once, exactly as without tracking.
//   CanSkip()      true only if the stamp belongs to the current epoch and layout and no page of the range has the
//                  soft-dirty bit set. Anything unknown (backend error, stale stamp, unmapped, shared object)
//                  answers false, and false means "hash like before".
//   MarkVerified() call after the content of the range was compared or re-uploaded *in the current epoch*.
//
// Writes that land between a verification and the next CanSkip set the bit after the clear, so they are seen.
// Writes that raced a hash are covered the same way: the bit is set no later than the first byte changes.

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace nxrt::render {

// Reports whether the host pages backing a range were written since the last ClearAll().
class DirtyBackend {
public:
    virtual ~DirtyBackend() = default;
    // Forget all writes so far. Returns false on failure; the tracker then disables itself.
    virtual bool ClearAll() = 0;
    // Sets *dirty to true if any page of [host, host+bytes) was written since ClearAll(). Returns false if the
    // answer is unknown.
    virtual bool RangeDirty(const void* host, size_t bytes, bool* dirty) = 0;
};

struct DirtyStamp {
    uint64_t epoch = 0;   // 0: never verified
    uint64_t layout = 0;  // GuestMemory::LayoutGeneration() at verification
};

struct DirtyTrackerStats {
    uint64_t skipped = 0;          // CanSkip() == true
    uint64_t skipped_bytes = 0;
    uint64_t dirty = 0;            // pages written since the clear
    uint64_t dirty_bytes = 0;
    uint64_t unverified = 0;       // no stamp for this epoch/layout (first frame after a clear, new texture)
    uint64_t unverified_bytes = 0;
    uint64_t untrackable = 0;      // shared object mapping or backend error
    uint64_t untrackable_bytes = 0;
    uint64_t clears = 0;
    uint64_t clear_ns = 0;
    uint64_t query_ns = 0;
    uint64_t queries = 0;
};

class TextureDirtyTracker {
public:
    TextureDirtyTracker(DirtyBackend* backend, uint32_t clear_interval_frames)
        : backend_(backend), interval_(clear_interval_frames ? clear_interval_frames : 1) {}

    bool enabled() const { return backend_ && !disabled_; }
    uint64_t epoch() const { return epoch_; }

    void BeginFrame(uint64_t frame) {
        if (!enabled()) return;
        if (epoch_ != 0 && frame >= clear_frame_ && frame - clear_frame_ < interval_) return;
        const auto start = std::chrono::steady_clock::now();
        if (!backend_->ClearAll()) {
            disabled_ = true;
            return;
        }
        stats_.clear_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        ++stats_.clears;
        ++epoch_;
        clear_frame_ = frame;
    }

    // `trackable` is false for ranges the soft-dirty bits cannot vouch for (shared object mappings).
    bool CanSkip(const DirtyStamp& stamp, const void* host, size_t bytes, uint64_t layout, bool trackable) {
        if (!enabled() || bytes == 0) return false;
        if (!trackable) {
            ++stats_.untrackable;
            stats_.untrackable_bytes += bytes;
            return false;
        }
        if (stamp.epoch != epoch_ || stamp.layout != layout) {
            ++stats_.unverified;
            stats_.unverified_bytes += bytes;
            return false;
        }
        const auto start = std::chrono::steady_clock::now();
        bool dirty = true;
        const bool known = backend_->RangeDirty(host, bytes, &dirty);
        stats_.query_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
        ++stats_.queries;
        if (!known) {
            ++stats_.untrackable;
            stats_.untrackable_bytes += bytes;
            return false;
        }
        if (dirty) {
            ++stats_.dirty;
            stats_.dirty_bytes += bytes;
            return false;
        }
        ++stats_.skipped;
        stats_.skipped_bytes += bytes;
        return true;
    }

    // The content of the range was just compared with (or uploaded into) the host copy.
    void MarkVerified(DirtyStamp& stamp, uint64_t layout, bool trackable) const {
        if (!enabled() || !trackable) {
            stamp = {};
            return;
        }
        stamp.epoch = epoch_;
        stamp.layout = layout;
    }

    const DirtyTrackerStats& stats() const { return stats_; }
    void ResetStats() { stats_ = {}; }

private:
    DirtyBackend* backend_;
    uint32_t interval_;
    bool disabled_ = false;
    uint64_t epoch_ = 0;
    uint64_t clear_frame_ = 0;
    DirtyTrackerStats stats_;
};

}  // namespace nxrt::render
