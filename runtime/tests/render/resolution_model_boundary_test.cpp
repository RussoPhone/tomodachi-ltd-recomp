// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <new>
#include "render/resolution_model_boundary.h"
int main() {
    nxrt::nvn::Model live, submitted;
    nxrt::render::ResolutionModelBoundary boundary;
    int resets = 0;
    auto select = [&](const auto& model) { if (boundary.Changed(model)) ++resets; };
    select(live); select(live);
    if (resets != 1) { std::puts("FAIL: repeated live selection invalidated"); return 1; }
    select(submitted); select(submitted);
    if (resets != 2) { std::puts("FAIL: submitted model identity not tracked"); return 1; }
    submitted.pool_address_revision++;
    select(submitted); select(submitted);
    if (resets != 3) { std::puts("FAIL: changed GPU pool address did not invalidate"); return 1; }
    select(live); select(submitted);
    if (resets != 5) { std::puts("FAIL: live/submitted model switches not tracked"); return 1; }
    alignas(nxrt::nvn::Model) unsigned char storage[sizeof(nxrt::nvn::Model)];
    nxrt::render::ResolutionModelBoundary reused;
    auto* first = new (storage) nxrt::nvn::Model;
    if (!reused.Changed(*first)) return 1;
    first->~Model();
    auto* second = new (storage) nxrt::nvn::Model;
    const bool detected = reused.Changed(*second);
    second->~Model();
    if (!detected) { std::puts("FAIL: a replacement model reused the same address"); return 1; }
    return 0;
}
