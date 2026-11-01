// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "nvn/model.h"

namespace nxrt::nvn {

// The renderer mutex must be held throughout this scope, including destruction.
class ExecutionModelScope {
public:
    ExecutionModelScope(Model*& selected, Model* snapshot) : selected_(selected), previous_(selected) {
        selected_ = snapshot;
    }
    ~ExecutionModelScope() { selected_ = previous_; }
    ExecutionModelScope(const ExecutionModelScope&) = delete;
    ExecutionModelScope& operator=(const ExecutionModelScope&) = delete;
private:
    Model*& selected_;
    Model* previous_;
};

// Caller holds source.lock. Only render-visible metadata is copied; builders,
// recording and finished lists are excluded (SubmissionSnapshot already owns
// the flattened command closure). This does not pin guest memory or permit
// releasing the live Model lock, deferring execution, or acknowledging fences.
template<class ReadPoolAddress>
std::shared_ptr<Model> SnapshotExecutionModel(const Model& source, ReadPoolAddress&& read_pool_address) {
    auto snapshot = std::make_shared<Model>();
    snapshot->pools = source.pools;
    snapshot->virtual_mappings = source.virtual_mappings;
    snapshot->textures = source.textures;
    snapshot->programs = source.programs;
    snapshot->samplers = source.samplers;
    snapshot->texture_pools = source.texture_pools;
    snapshot->texture_views = source.texture_views;
    snapshot->texture_pool_views = source.texture_pool_views;
    snapshot->sampler_pools = source.sampler_pools;
    snapshot->windows = source.windows;
    snapshot->texture_pool_registration = source.texture_pool_registration;
    snapshot->pool_generation = source.pool_generation;
    snapshot->pool_address_revision = source.pool_address_revision;
    snapshot->epoch.store(source.epoch.load(std::memory_order_acquire), std::memory_order_relaxed);
    snapshot->serial = source.serial;
    snapshot->frames = source.frames;
    for (auto& [address, pool] : snapshot->pools) {
        if (pool.storage && !pool.gpu) {
            pool.gpu = read_pool_address(address + 0x18);
            pool.gpu2 = read_pool_address(address + 0x20);
        }
    }
    return snapshot;
}

} // namespace nxrt::nvn
