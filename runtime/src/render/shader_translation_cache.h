// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Cache of translated shaders (Maxwell -> IR -> SPIR-V). Translation is a pure function of the values it reads from
// guest state: instruction words, constant-buffer words, texture descriptor properties and the program header. An
// entry records that read set; a later request reuses the entry only if every recorded read still returns the same
// value, so a hit is exact. The SPIR-V of a stage also depends on the RuntimeInfo handed to the emitter, so each
// entry holds variants keyed by it.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <vector>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/shader_info.h"
#include "shader_recompiler/stage.h"

namespace nxrt::render {

struct ShaderCbufDep {
    std::uint32_t index = 0, offset = 0, value = 0;
    // A word of the driver's texture-handle buffer that the translator used only to look up a texture descriptor: the
    // value itself need not match, the descriptor answers re-derived from the new value must (ShaderTextureDep).
    bool handle_only = false;
};

struct ShaderTextureDep {
    std::int64_t from_cbuf_offset = -1;  // >= 0: the handle is (value of that handle-buffer word) & 0xFFFFF
    std::uint32_t handle = 0;
    std::uint32_t type = 0;      // ShaderTextureType of the descriptor
    bool integer = false;        // TicFormat(...).integer
};

struct ShaderBytesDep {
    std::uint32_t offset = 0;    // from the start of the shader data
    std::vector<std::uint8_t> bytes;
};

struct ShaderVariant {
    std::vector<std::uint64_t> runtime_key;
    Shader::Backend::Bindings bindings_before{};
    Shader::Backend::Bindings bindings_after{};
    std::vector<std::uint32_t> spirv;
    Shader::Info info;
};

struct ShaderEntry {
    Shader::Stage stage{};
    std::size_t nvn_stage = 0;
    std::vector<ShaderBytesDep> ranges;       // header and every instruction word the translator read
    std::vector<ShaderCbufDep> cbufs;
    std::vector<ShaderTextureDep> textures;
    std::vector<ShaderVariant> variants;
    std::size_t bytes = 0;

    const ShaderVariant* FindVariant(const std::vector<std::uint64_t>& key,
                                     const Shader::Backend::Bindings& before) const {
        for (const ShaderVariant& v : variants) {
            if (v.runtime_key == key && v.bindings_before.unified == before.unified &&
                v.bindings_before.uniform_buffer == before.uniform_buffer &&
                v.bindings_before.storage_buffer == before.storage_buffer &&
                v.bindings_before.texture == before.texture && v.bindings_before.image == before.image &&
                v.bindings_before.texture_scaling_index == before.texture_scaling_index &&
                v.bindings_before.image_scaling_index == before.image_scaling_index) {
                return &v;
            }
        }
        return nullptr;
    }
};

class ShaderTranslationCache {
public:
    static inline const std::size_t kMaxEntriesPerShader = [] {
        const char* v = std::getenv("NXRT_SHADER_CACHE_ENTRIES");
        return v && *v ? static_cast<std::size_t>(std::strtoull(v, nullptr, 10)) : std::size_t{8};
    }();
    static constexpr std::size_t kMaxBytes = std::size_t{192} << 20;

    // Entries stored under the GPU address of the shader's code.
    std::vector<std::unique_ptr<ShaderEntry>>* Find(std::uint64_t gpu_code) {
        const auto it = map_.find(gpu_code);
        return it == map_.end() ? nullptr : &it->second;
    }

    // Returns nullptr when the shader already has the maximum number of entries (the caller then runs uncached).
    // Never evicts: pointers handed out earlier stay valid until Trim().
    ShaderEntry* Add(std::uint64_t gpu_code, std::unique_ptr<ShaderEntry> entry) {
        auto& list = map_[gpu_code];
        if (list.size() >= kMaxEntriesPerShader) return nullptr;
        bytes_ += entry->bytes;
        list.push_back(std::move(entry));
        return list.back().get();
    }

    // Called before a batch of lookups: drops everything when over the budget.
    void Trim() {
        if (bytes_ > kMaxBytes) Clear();
    }

    void AddBytes(std::size_t n) { bytes_ += n; }
    void Clear() {
        map_.clear();
        bytes_ = 0;
        ++clears_;
    }
    std::size_t bytes() const { return bytes_; }
    std::uint64_t clears() const { return clears_; }
    std::size_t shaders() const { return map_.size(); }

private:
    std::unordered_map<std::uint64_t, std::vector<std::unique_ptr<ShaderEntry>>> map_;
    std::size_t bytes_ = 0;
    std::uint64_t clears_ = 0;
};

}  // namespace nxrt::render
