// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace nxrt::hle {

// Tracks values promised to clients separately from work that has actually
// completed. Wait never needs NvState::mutex, which GPU address translation
// takes while executing a submission.
class SyncpointTimeline {
public:
    static constexpr uint32_t kCount = 192;

    struct Fence {
        uint32_t id = 0;
        uint32_t value = 0;
    };

    enum class WaitResult { Complete, Timeout };

    Fence Submit(uint32_t id, uint32_t increment) {
        std::lock_guard lock(mutex_);
        submitted_[id] += increment;
        return {id, submitted_[id]};
    }

    void Complete(Fence fence) {
        {
            std::lock_guard lock(mutex_);
            if (Reached(fence.value, completed_[fence.id])) completed_[fence.id] = fence.value;
        }
        changed_.notify_all();
    }

    WaitResult Wait(uint32_t id, uint32_t value, int32_t timeout_ms) {
        std::unique_lock lock(mutex_);
        const auto reached = [&] { return Reached(completed_[id], value); };
        if (timeout_ms < 0) {
            changed_.wait(lock, reached);
            return WaitResult::Complete;
        }
        return changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms), reached)
                   ? WaitResult::Complete
                   : WaitResult::Timeout;
    }

    uint32_t Submitted(uint32_t id) const {
        std::lock_guard lock(mutex_);
        return submitted_[id];
    }

    uint32_t Completed(uint32_t id) const {
        std::lock_guard lock(mutex_);
        return completed_[id];
    }

private:
    static bool Reached(uint32_t current, uint32_t target) {
        return static_cast<int32_t>(current - target) >= 0;
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::array<uint32_t, kCount> submitted_{};
    std::array<uint32_t, kCount> completed_{};
};

} // namespace nxrt::hle
