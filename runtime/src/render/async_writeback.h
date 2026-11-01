// SPDX-License-Identifier: GPL-3.0-or-later
// NXRT_ASYNC_COMPUTE_FLUSH: bookkeeping for compute results that the GPU still owns.
//
// A compute dispatch writes its output SSBO into the upload stream. In the synchronous path the renderer waits for
// the fence and copies the bytes into guest memory right away. In the asynchronous path the command buffer is only
// submitted; the copy ("materialization") is deferred, and the guest range is *pending*: its newest contents live
// in the stream. The ledger keeps those ranges in dispatch order and guarantees that nothing observes or overwrites
// a pending range before the copy:
//   * it implements GuestAccessGuard, so every GuestMemory accessor (renderer reads, HLE, AOT slow path, atomics,
//     raw Ptr users) materializes first and only then executes the access;
//   * pages of pending ranges are diverted to the page-table slow path so AOT loads/stores reach the guard;
//   * Poll() copies the ranges whose fence already signalled;
//   * MaterializeAll() is the barrier the renderer calls before Execute returns.
// Materialization is always a prefix of the queue (dispatch order), so overlapping writers land in order.
#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <utility>
#include <span>
#include <unordered_map>
#include <vector>

#include "guest_memory_range.h"
#include "memory.h"

namespace nxrt::render {

struct AsyncWriteback {
    std::uint64_t program = 0;
    std::uint64_t frame = 0;
    std::uint64_t dispatch = 0;
    std::uint64_t stream_offset = 0;
    GuestMemoryRange range{};
    std::uint32_t slot = 0;          // ring slot whose fence covers the GPU write
    std::uint64_t baseline_hash = 0; // VERIFY: hash of the guest range at submit (or after the previous overlapping copy)
};

enum class AsyncMaterializeCause : std::size_t { GuestLoad, GuestStore, Renderer, Poll, ExecuteEnd, Drain, Count };

// Cumulative elapsed wall time; snapshots are protected by the ledger mutex.
// Guest-access mutex time below counts calls with overlap; non-overlaps have separate totals.
struct AsyncMaterializeProfile {
    std::uint64_t lock_calls = 0, lock_wait_ns = 0;
    std::uint64_t wait_calls = 0, wait_ns = 0;
    std::uint64_t apply_calls = 0, apply_ns = 0, applied_bytes = 0;
};

struct AsyncNonOverlapProfile {
    std::uint64_t lock_calls = 0, lock_wait_ns = 0;
    std::uint64_t pending_page_calls = 0;
};

struct AsyncWritebackStats {
    std::array<AsyncMaterializeProfile, static_cast<std::size_t>(AsyncMaterializeCause::Count)> profile{};
    AsyncNonOverlapProfile non_overlap{};
    std::array<std::uint64_t, static_cast<std::size_t>(AsyncMaterializeCause::Count)> intercepted{};
    std::uint64_t entries_added = 0;
    std::uint64_t entries_materialized = 0;
    std::uint64_t entries_superseded = 0;
    std::uint64_t entries_dropped = 0;       // remapped/unmapped or failed wait
    std::uint64_t page_hits_without_overlap = 0;
    std::uint64_t verify_checked = 0;
    std::uint64_t verify_divergences = 0;
    std::uint64_t wait_failures = 0;
    std::uint64_t apply_skipped = 0;
};

struct AsyncWritebackHooks {
    std::function<bool(std::uint32_t slot)> wait_slot;      // block until the slot's fence signalled; false on failure
    std::function<bool(std::uint32_t slot)> slot_done;      // non-blocking
    std::function<bool(const AsyncWriteback&)> apply;       // copy stream -> guest memory (unguarded); false = skipped
    std::function<std::uint64_t(GuestMemoryRange)> hash_guest; // VERIFY only; unguarded read of the guest bytes
    std::function<bool(std::uint64_t page)> divert_page;    // true when this call flipped a fast page to slow
    std::function<void(std::uint64_t page)> restore_page;   // undo a diversion this ledger made
    std::function<bool()> in_renderer;                      // classifies the calling thread (renderer vs guest)
    std::function<void(const AsyncWriteback&, std::uint64_t now_hash)> on_divergence; // VERIFY report
};

class AsyncWritebackLedger final : public GuestAccessGuard {
public:
    AsyncWritebackLedger(AsyncWritebackHooks hooks, bool verify, bool profile = false)
        : hooks_(std::move(hooks)), verify_(verify), profile_(profile) {}

    std::size_t Pending() const noexcept { return count_.load(std::memory_order_acquire); }
    std::uint64_t BloomChecked() const noexcept { return bloom_checked_.load(); }
    std::uint64_t BloomViolations() const noexcept { return bloom_violations_.load(); }

    // Registers the write-backs of a command buffer that has just been submitted. When `supersede_exact` is set
    // (the dispatch updated a forwarded stream range in place), older entries with the very same range and stream
    // offset are removed without a copy: the new entry's bytes are the final contents of that memory.
    void Add(std::span<const AsyncWriteback> entries, bool supersede_exact) {
        std::lock_guard lock(mutex_);
        for (AsyncWriteback entry : entries) {
            if (!entry.range.size) continue;
            if (supersede_exact) {
                for (auto it = queue_.begin(); it != queue_.end();) {
                    if (it->range.address == entry.range.address && it->range.size == entry.range.size &&
                        it->stream_offset == entry.stream_offset) {
                        ReleasePages(it->range);
                        it = queue_.erase(it);
                        ++stats_.entries_superseded;
                    } else {
                        ++it;
                    }
                }
            }
            entry.baseline_hash = verify_ && hooks_.hash_guest ? hooks_.hash_guest(entry.range) : 0;
            DivertPages(entry.range);
            queue_.push_back(entry);
            ++stats_.entries_added;
        }
        count_.store(queue_.size(), std::memory_order_release);
    }

    // The newest pending write of exactly `range` can be reused as an in-place stream buffer by the next dispatch.
    // Mirrors FindForwardableComputeWriteback but for read/write use: the newest overlapping entry must have the
    // identical range, otherwise nothing is forwarded.
    std::optional<std::uint64_t> FindForward(GuestMemoryRange range, std::uint64_t stream_alignment) const {
        if (!range.size || !stream_alignment) return std::nullopt;
        std::lock_guard lock(mutex_);
        for (auto it = queue_.rbegin(); it != queue_.rend(); ++it) {
            if (!GuestMemoryRangesOverlap(it->range, range)) continue;
            if (it->range.address != range.address || it->range.size != range.size) return std::nullopt;
            if (it->stream_offset % stream_alignment) return std::nullopt;
            return it->stream_offset;
        }
        return std::nullopt;
    }

    // Every pending entry overlapping `range`, oldest first, with the stream offset holding its GPU result. While an
    // entry is pending its stream bytes stay valid (the stream is only reset after everything is materialized).
    struct PendingPiece {
        GuestMemoryRange range{};
        std::uint64_t stream_offset = 0;
    };
    std::vector<PendingPiece> CollectOverlaps(GuestMemoryRange range) const {
        std::vector<PendingPiece> out;
        if (!range.size) return out;
        std::lock_guard lock(mutex_);
        for (const AsyncWriteback& entry : queue_) {
            if (GuestMemoryRangesOverlap(entry.range, range)) out.push_back({entry.range, entry.stream_offset});
        }
        return out;
    }

    // Diagnostic (NXRT_TRACE_HITCH_MS): how `range` relates to the pending queue. newest = newest overlapping entry.
    struct OverlapInfo {
        std::size_t overlapping = 0;
        GuestMemoryRange newest{};
        std::uint64_t newest_stream_offset = 0;
    };
    OverlapInfo DescribeOverlap(GuestMemoryRange range) const {
        OverlapInfo info;
        if (!range.size) return info;
        std::lock_guard lock(mutex_);
        for (auto it = queue_.rbegin(); it != queue_.rend(); ++it) {
            if (!GuestMemoryRangesOverlap(it->range, range)) continue;
            if (!info.overlapping) info.newest = it->range, info.newest_stream_offset = it->stream_offset;
            ++info.overlapping;
        }
        return info;
    }

    // Copies the ready prefix (entries whose fence already signalled). Returns how many were copied.
    std::size_t Poll() {
        if (!Pending()) return 0;
        const auto start = profile_ ? Clock::now() : Clock::time_point{};
        std::lock_guard lock(mutex_);
        if (profile_) RecordLock(AsyncMaterializeCause::Poll, start);
        std::size_t ready = 0;
        while (ready < queue_.size() && hooks_.slot_done(queue_[ready].slot)) ++ready;
        if (!ready) return 0;
        Materialize(ready, AsyncMaterializeCause::Poll);
        return ready;
    }

    // Copies everything. The renderer calls it with every fence already waited (Drain/ExecuteEnd), or lets it wait.
    void MaterializeAll(AsyncMaterializeCause cause) {
        if (!Pending()) return;
        const auto start = profile_ ? Clock::now() : Clock::time_point{};
        std::lock_guard lock(mutex_);
        if (profile_) RecordLock(cause, start);
        Materialize(queue_.size(), cause);
    }

    // Forgets every pending entry (renderer disabled): the guest keeps whatever it had.
    void DropAll() {
        std::lock_guard lock(mutex_);
        for (const AsyncWriteback& entry : queue_) ReleasePages(entry.range);
        stats_.entries_dropped += queue_.size();
        queue_.clear();
        count_.store(0, std::memory_order_release);
    }

    void BeforeAccess(std::uint64_t va, std::uint64_t size, bool write) noexcept override {
        if (!Pending() || !size) return;
        if (!BloomMayTouch(va, size)) {
            // No pending result lives on any page of this range. NXRT_LEDGER_BLOOM_VERIFY=1 re-checks that under the lock.
            static const bool verify = std::getenv("NXRT_LEDGER_BLOOM_VERIFY") != nullptr;
            if (verify) {
                std::lock_guard lock(mutex_);
                ++bloom_checked_;
                if (TouchesPendingPage({va, size})) ++bloom_violations_;
            } else {
                return;
            }
            return;
        }
        try {
            const auto start = profile_ ? Clock::now() : Clock::time_point{};
            std::lock_guard lock(mutex_);
            // Capture after acquisition; attribution is delayed until overlap and thread class are known.
            const auto acquired = profile_ ? Clock::now() : Clock::time_point{};
            const GuestMemoryRange range{va, size};
            std::size_t last = queue_.size();
            for (std::size_t i = queue_.size(); i-- > 0;) {
                if (GuestMemoryRangesOverlap(queue_[i].range, range)) {
                    last = i;
                    break;
                }
            }
            if (last == queue_.size()) {
                const bool touches_pending_page = TouchesPendingPage(range);
                if (touches_pending_page) ++stats_.page_hits_without_overlap;
                if (profile_) {
                    ++stats_.non_overlap.lock_calls;
                    stats_.non_overlap.lock_wait_ns += Ns(acquired - start);
                    if (touches_pending_page) ++stats_.non_overlap.pending_page_calls;
                }
                return;
            }
            const bool renderer = hooks_.in_renderer && hooks_.in_renderer();
            const auto cause = renderer ? AsyncMaterializeCause::Renderer
                                        : (write ? AsyncMaterializeCause::GuestStore : AsyncMaterializeCause::GuestLoad);
            if (profile_) RecordLock(cause, start, acquired);
            Materialize(last + 1, cause);
        } catch (...) {
            // Accessors are noexcept; a failure here cannot be reported further.
        }
    }

    void OnRemap(std::uint64_t va, std::uint64_t size) noexcept override {
        if (!Pending() || !size) return;
        try {
            std::lock_guard lock(mutex_);
            const GuestMemoryRange range{va, size};
            for (auto it = queue_.begin(); it != queue_.end();) {
                if (GuestMemoryRangesOverlap(it->range, range)) {
                    ReleasePages(it->range);
                    it = queue_.erase(it);
                    ++stats_.entries_dropped;
                } else {
                    ++it;
                }
            }
            count_.store(queue_.size(), std::memory_order_release);
        } catch (...) {
        }
    }

    AsyncWritebackStats Stats() const {
        std::lock_guard lock(mutex_);
        return stats_;
    }

    std::vector<AsyncWriteback> Snapshot() const {
        std::lock_guard lock(mutex_);
        return {queue_.begin(), queue_.end()};
    }

private:
    using Clock = std::chrono::steady_clock;
    static std::uint64_t Ns(Clock::duration duration) noexcept {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
    }
    // Caller holds mutex_.
    void RecordLock(AsyncMaterializeCause cause, Clock::time_point start, Clock::time_point acquired) {
        auto& p = stats_.profile[static_cast<std::size_t>(cause)];
        ++p.lock_calls;
        p.lock_wait_ns += Ns(acquired - start);
    }
    void RecordLock(AsyncMaterializeCause cause, Clock::time_point start) {
        RecordLock(cause, start, Clock::now());
    }
    static constexpr std::uint64_t kPageBits = nxrt::kPageBits;

    struct PageRef {
        std::uint32_t refs = 0;
        bool flipped = false; // this ledger moved the page from fast to slow
    };

    void DivertPages(GuestMemoryRange range) {
        const std::uint64_t first = range.address >> kPageBits, last = (range.address + range.size - 1) >> kPageBits;
        for (std::uint64_t page = first; page <= last; ++page) {
            PageRef& ref = pages_[page];
            if (ref.refs++ == 0) ref.flipped = hooks_.divert_page ? hooks_.divert_page(page) : false;
        }
        RebuildBloom();
    }

    void ReleasePages(GuestMemoryRange range) {
        const std::uint64_t first = range.address >> kPageBits, last = (range.address + range.size - 1) >> kPageBits;
        for (std::uint64_t page = first; page <= last; ++page) {
            auto it = pages_.find(page);
            if (it == pages_.end()) continue;
            if (--it->second.refs == 0) {
                if (it->second.flipped && hooks_.restore_page) hooks_.restore_page(page);
                pages_.erase(it);
            }
        }
        RebuildBloom();
    }

    bool TouchesPendingPage(GuestMemoryRange range) const {
        const std::uint64_t first = range.address >> kPageBits, last = (range.address + range.size - 1) >> kPageBits;
        for (std::uint64_t page = first; page <= last; ++page) {
            if (pages_.count(page)) return true;
        }
        return false;
    }

    // Copies queue_[0, count) in order. Caller holds mutex_.
    void Materialize(std::size_t count, AsyncMaterializeCause cause) {
        ++stats_.intercepted[static_cast<std::size_t>(cause)];
        std::size_t done = 0;
        for (; done < count; ++done) {
            AsyncWriteback& entry = queue_[done];
            const auto wait_start = profile_ ? Clock::now() : Clock::time_point{};
            const bool waited = hooks_.wait_slot(entry.slot);
            if (profile_) {
                auto& p = stats_.profile[static_cast<std::size_t>(cause)];
                ++p.wait_calls;
                p.wait_ns += Ns(Clock::now() - wait_start);
            }
            if (!waited) {
                ++stats_.wait_failures;
                break;
            }
            if (verify_ && hooks_.hash_guest) {
                ++stats_.verify_checked;
                const std::uint64_t now = hooks_.hash_guest(entry.range);
                if (now != entry.baseline_hash) {
                    ++stats_.verify_divergences;
                    if (hooks_.on_divergence) hooks_.on_divergence(entry, now);
                }
            }
            const auto apply_start = profile_ ? Clock::now() : Clock::time_point{};
            const bool applied = hooks_.apply(entry);
            if (profile_) {
                auto& p = stats_.profile[static_cast<std::size_t>(cause)];
                ++p.apply_calls;
                p.apply_ns += Ns(Clock::now() - apply_start);
                if (applied) p.applied_bytes += entry.range.size;
            }
            if (applied) ++stats_.entries_materialized;
            else ++stats_.apply_skipped;
            if (verify_ && hooks_.hash_guest) {
                // The copy legitimately changed these bytes for every later entry that overlaps them.
                for (std::size_t later = done + 1; later < queue_.size(); ++later) {
                    if (GuestMemoryRangesOverlap(queue_[later].range, entry.range)) {
                        queue_[later].baseline_hash = hooks_.hash_guest(queue_[later].range);
                    }
                }
            }
            ReleasePages(entry.range);
        }
        if (done < count) {
            // A fence failed: the GPU is gone. Drop the rest so no thread waits on it again.
            for (std::size_t i = done; i < count; ++i) {
                ReleasePages(queue_[i].range);
                ++stats_.entries_dropped;
            }
            done = count;
        }
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(done));
        count_.store(queue_.size(), std::memory_order_release);
    }

    AsyncWritebackHooks hooks_;
    const bool verify_;
    const bool profile_;
    mutable std::mutex mutex_;
    std::vector<AsyncWriteback> queue_;
    std::unordered_map<std::uint64_t, PageRef> pages_;
    std::atomic<std::size_t> count_{0};
    // Lock-free filter over the pages that hold pending results (a superset: false positives only cost the slow path).
    // Rebuilt under mutex_ whenever pages_ changes. A page that no bit says is pending cannot overlap a queued entry.
    static constexpr std::size_t kBloomBits = 1 << 16;
    std::array<std::atomic<std::uint64_t>, kBloomBits / 64> bloom_{};
    static std::size_t BloomBit(std::uint64_t page) noexcept {
        return static_cast<std::size_t>((page * 0x9E3779B97F4A7C15ULL) >> 48) & (kBloomBits - 1);
    }
    void RebuildBloom() {
        for (auto& w : bloom_) w.store(0, std::memory_order_relaxed);
        for (const auto& [page, ref] : pages_) {
            const std::size_t bit = BloomBit(page);
            bloom_[bit >> 6].fetch_or(std::uint64_t{1} << (bit & 63), std::memory_order_release);
        }
    }
    bool BloomMayTouch(std::uint64_t address, std::uint64_t size) const noexcept {
        const std::uint64_t first = address >> kPageBits, last = (address + size - 1) >> kPageBits;
        if (last - first > 256) return true;  // a big range: take the exact path
        for (std::uint64_t page = first; page <= last; ++page) {
            const std::size_t bit = BloomBit(page);
            if (bloom_[bit >> 6].load(std::memory_order_acquire) & (std::uint64_t{1} << (bit & 63))) return true;
        }
        return false;
    }
    std::atomic<std::uint64_t> bloom_checked_{0}, bloom_violations_{0};
    AsyncWritebackStats stats_{};
};

// Ring of command-buffer slots for the asynchronous flush. Pure bookkeeping (the renderer owns the Vulkan objects):
// slots are used in order and are only recycled together, once every fence has been waited and the stream,
// descriptor pool and retired images are reset. When the ring (or another budget) is exhausted the caller falls
// back to the synchronous flush, which drains and resets everything.
class AsyncSlotRing {
public:
    explicit AsyncSlotRing(std::size_t slots) : slots_(slots) {}

    std::size_t Slots() const noexcept { return slots_; }
    std::size_t InFlight() const noexcept { return in_flight_; }
    std::size_t Current() const noexcept { return in_flight_; } // the slot being recorded

    // The recording slot must stay free for the synchronous flush that follows an async submit.
    bool CanSubmitAsync() const noexcept { return in_flight_ + 2 <= slots_; }

    // The recording slot was submitted without waiting. Returns the slot that is now recording.
    std::size_t MarkSubmitted() noexcept { return ++in_flight_; }

    // Everything was waited and reset.
    void RecycleAll() noexcept { in_flight_ = 0; }

private:
    std::size_t slots_;
    std::size_t in_flight_ = 0;
};

} // namespace nxrt::render
