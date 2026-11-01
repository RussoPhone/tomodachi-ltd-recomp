// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

int main() {
    Shader::Info info{};
    info.used_constant_buffer_types = Shader::IR::Type::F32;
    info.used_indirect_cbuf_types = Shader::IR::Type::U16;

    assert(Shader::Backend::SPIRV::ConstantBufferTypesForEmission(info) ==
           (Shader::IR::Type::F32 | Shader::IR::Type::U16));

    info.uses_global_memory = true;
    assert(Shader::Backend::SPIRV::ConstantBufferTypesForEmission(info) ==
           (Shader::IR::Type::F32 | Shader::IR::Type::U16 | Shader::IR::Type::U32));

    std::cout << "SPIR-V global memory constant-buffer type tests passed\n";
}
