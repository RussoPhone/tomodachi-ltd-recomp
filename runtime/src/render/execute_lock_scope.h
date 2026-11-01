// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <mutex>

namespace nxrt::render {

enum class ExecuteModelRelease { Disabled, PendingWritebacks, Released };

// Keeps Execute serialized through its final flush. The model lock may only be
// released after ExecuteList has returned and when no guest writeback remains.
template <class ModelMutex = std::mutex>
class ExecuteLockScope {
public:
    ExecuteLockScope(std::mutex& renderer_mutex, ModelMutex& model_mutex)
        : renderer_lock_(renderer_mutex), model_lock_(model_mutex) {}

    ExecuteModelRelease BeforeFinalFlush(bool enabled, bool has_pending_writebacks,
                                        bool has_execution_snapshot = false) {
        if (!enabled || has_execution_snapshot) return ExecuteModelRelease::Disabled;
        if (has_pending_writebacks) return ExecuteModelRelease::PendingWritebacks;
        model_lock_.unlock();
        return ExecuteModelRelease::Released;
    }

    // The model lock while Execute owns it; a yield point may unlock and re-lock it between two commands.
    std::unique_lock<ModelMutex>& ModelLock() { return model_lock_; }

private:
    // Reverse destruction order releases Model before the renderer, including
    // when Execute returns early or the final flush throws.
    std::lock_guard<std::mutex> renderer_lock_;
    std::unique_lock<ModelMutex> model_lock_;
};

} // namespace nxrt::render
