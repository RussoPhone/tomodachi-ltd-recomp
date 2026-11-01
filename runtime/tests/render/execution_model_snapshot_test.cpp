// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include "nvn/execution_model_snapshot.h"

int main() {
    using namespace nxrt::nvn;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        if (!ok) { std::printf("FAIL: %s\n", label); ++failures; }
    };
    Model source;
    source.pools[0x1000] = {.storage=0x2000, .size=4096, .generation=7};
    source.pools[0x1100] = {.storage=0x3000, .size=4096, .gpu=0x9000, .gpu2=0xa000};
    source.programs[5].stages.push_back({0x4444, 0x5555});
    source.programs[5].serial = 3;
    source.textures[6].desc.width = 128;
    source.textures[6].serial = 4;
    source.texture_pools[7][1] = {6, 8};
    source.texture_pool_views[7][1].min_layer = 2;
    source.texture_views[8].base_level = 1;
    source.sampler_pools[9][2] = 10;
    source.samplers[10].lod_bias = 0.5f;
    source.windows[11].textures.push_back(6);
    source.virtual_mappings[0xb000] = {.size=4096, .cpu=0xc000, .physical_generation=7};
    source.pool_generation = 7;
    source.epoch.store(42);
    source.finished[12] = std::make_shared<CommandList>();
    int reads = 0;
    auto first = SnapshotExecutionModel(source, [&](u64 address) {
        ++reads;
        return address == 0x1018 ? u64{0x7000} : u64{0x8000};
    });
    check(reads == 2 && first->pools.at(0x1000).gpu == 0x7000 &&
          first->pools.at(0x1000).gpu2 == 0x8000, "missing pool addresses captured at submit");
    check(source.pools.at(0x1000).gpu == 0 && first->pools.at(0x1100).gpu == 0x9000,
          "normalization leaves source and known addresses intact");
    source.programs[5].stages[0].code = 0;
    source.programs[5].stages.push_back({});
    source.textures[6].desc.width = 256;
    source.texture_pools[7][1] = {99, 99};
    source.texture_pool_views[7][1].min_layer = 8;
    source.texture_views[8].base_level = 8;
    source.sampler_pools[9][2] = 99;
    source.samplers[10].lod_bias = 2;
    source.windows[11].textures.clear();
    source.virtual_mappings.clear();
    source.epoch.store(44);
    check(first->programs.at(5).stages.size() == 1 && first->programs.at(5).stages[0].code == 0x4444 &&
          first->programs.at(5).serial == 3, "program stage storage belongs to the submission");
    check(first->textures.at(6).desc.width == 128 && first->textures.at(6).serial == 4 &&
          first->texture_pools.at(7).at(1).first == 6 && first->texture_pool_views.at(7).at(1).min_layer == 2 &&
          first->texture_views.at(8).base_level == 1, "textures and nested views survive object reuse");
    check(first->sampler_pools.at(9).at(2) == 10 && first->samplers.at(10).lod_bias == 0.5f &&
          first->windows.at(11).textures.size() == 1 && first->virtual_mappings.at(0xb000).cpu == 0xc000,
          "samplers, windows and virtual mappings own independent values");
    check(first->pool_generation == 7 && first->epoch.load() == 42 && first->finished.empty() &&
          first->recording.empty(), "capture keeps cache identity and excludes recorder-only lists");
    auto second = SnapshotExecutionModel(source, [](u64) { return u64{0xd000}; });
    check(second->epoch.load() == 44 && second->textures.at(6).desc.width == 256 &&
          first->textures.at(6).desc.width == 128, "next submission sees replacement objects");
    first->pools.at(0x1000).gpu = 0xffff;
    check(second->pools.at(0x1000).gpu == 0xd000, "renderer normalization cannot mutate another submission");
    Model* selected = nullptr;
    auto early_return = [&] {
        ExecutionModelScope scope(selected, first.get());
        check(selected == first.get(), "execution selects the submitted metadata");
        return;
    };
    early_return();
    check(selected == nullptr, "early return restores live model selection");
    try {
        ExecutionModelScope scope(selected, first.get());
        throw 1;
    } catch (int) {}
    check(selected == nullptr, "exception restores live model selection");
    return failures ? 1 : 0;
}
