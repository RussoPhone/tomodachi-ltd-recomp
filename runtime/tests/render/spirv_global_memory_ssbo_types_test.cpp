// SPDX-License-Identifier: GPL-3.0-or-later
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <iostream>

#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

int main() {
    Shader::Info info{};
    info.used_storage_buffer_types = Shader::IR::Type::F32;

    assert(Shader::Backend::SPIRV::StorageBufferTypesForEmission(info, false) ==
           Shader::IR::Type::U32);
    assert(Shader::Backend::SPIRV::StorageBufferTypesForEmission(info, true) ==
           Shader::IR::Type::F32);

    info.uses_global_memory = true;
    const Shader::IR::Type global_memory_types =
        Shader::IR::Type::U32 | Shader::IR::Type::U32x2 | Shader::IR::Type::U32x4;
    assert(Shader::Backend::SPIRV::StorageBufferTypesForEmission(info, false) ==
           (Shader::IR::Type::U32 | Shader::IR::Type::U32x2 | Shader::IR::Type::U32x4));
    assert(Shader::Backend::SPIRV::StorageBufferTypesForEmission(info, true) ==
           (Shader::IR::Type::F32 | global_memory_types));

    std::cout << "SPIR-V global memory storage-buffer type tests passed\n";
}
