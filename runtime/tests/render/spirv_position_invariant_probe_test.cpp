// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif

#include <cassert>
#include <cstdlib>
#include <iostream>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/object_pool.h"

namespace {

void SetLegacyPositionInvariant(const char* value) {
#ifdef _WIN32
    _putenv_s("NXRT_LEGACY_POSITION_INVARIANT", value);
#else
    if (value[0] == '\0') {
        unsetenv("NXRT_LEGACY_POSITION_INVARIANT");
    } else {
        setenv("NXRT_LEGACY_POSITION_INVARIANT", value, 1);
    }
#endif
}

bool HasInvariantDecoration(const std::vector<u32>& words) {
    constexpr u32 kOpDecorate = 71;
    constexpr u32 kInvariantDecoration = 18;
    for (size_t offset = 5; offset < words.size();) {
        const u32 instruction = words[offset];
        const u32 word_count = instruction >> 16;
        const u32 opcode = instruction & 0xffff;
        if (word_count == 0 || offset + word_count > words.size()) {
            return false;
        }
        if (opcode == kOpDecorate && word_count >= 3 && words[offset + 2] == kInvariantDecoration) {
            return true;
        }
        offset += word_count;
    }
    return false;
}

std::vector<u32> EmitVertexShader() {
    Shader::ObjectPool<Shader::IR::Inst> instruction_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    Shader::IR::Program program{};
    program.stage = Shader::Stage::VertexB;
    auto* block = block_pool.Create(instruction_pool);
    program.blocks.push_back(block);
    program.post_order_blocks.push_back(block);
    program.syntax_list.push_back({.data = {.block = block}, .type = Shader::IR::AbstractSyntaxNode::Type::Block});

    Shader::IR::IREmitter ir(*block);
    ir.Prologue();
    const auto zero = ir.Imm32(0.0f);
    const auto vertex = ir.Imm32(0u);
    for (const auto attribute : {Shader::IR::Attribute::PositionX, Shader::IR::Attribute::PositionY,
                                 Shader::IR::Attribute::PositionZ, Shader::IR::Attribute::PositionW}) {
        ir.SetAttribute(attribute, zero, vertex);
        program.info.stores.Set(attribute);
    }
    ir.Epilogue();

    Shader::Profile profile{};
    profile.supported_spirv = 0x00010500;
    Shader::RuntimeInfo runtime_info{};
    Shader::Backend::Bindings bindings;
    return Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
}

} // namespace

int main() {
    SetLegacyPositionInvariant("");
    assert(HasInvariantDecoration(EmitVertexShader()));
    SetLegacyPositionInvariant("1");
    assert(!HasInvariantDecoration(EmitVertexShader()));
    SetLegacyPositionInvariant("");
    std::cout << "SPIR-V position invariant probe test passed\n";
}
