// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "nvn/model.h"

namespace nxrt::render {
// Address-resolution caches are shared by Execute and Present. A selected
// model or a captured pool address may change independently of Model::epoch.
class ResolutionModelBoundary {
public:
    bool Changed(const nvn::Model& model) {
        if (selected_ == &model && identity_ == model.resolution_identity && revision_ == model.pool_address_revision) return false;
        selected_ = &model;
        identity_ = model.resolution_identity;
        revision_ = model.pool_address_revision;
        return true;
    }
private:
    const nvn::Model* selected_ = nullptr;
    u64 identity_ = 0;
    u64 revision_ = 0;
};
} // namespace nxrt::render
