// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "render/gpu_code_range.h"
#include "nvn/model.h"
#include "shader_recompiler/frontend/maxwell/instruction.h"
#include "shader_recompiler/frontend/maxwell/opcodes.h"

namespace nxrt::render {

struct ShaderTracePcStatus {
    bool pc_inside;
    bool instruction_fits;
};

inline ShaderTracePcStatus GetShaderTracePcStatus(const CpuBackingSpan& span, std::uint32_t offset) {
    return {
        .pc_inside = ContainsBackingAccess(span, offset, 1),
        .instruction_fits = ContainsBackingAccess(span, offset, sizeof(std::uint64_t)),
    };
}

// Maxwell's Location steps over the bundle control word at bundle boundaries.
inline std::optional<std::uint32_t> PreviousMaxwellInstructionOffset(std::uint32_t offset) {
    const std::uint32_t step = 8 + (offset % 32 == 8 ? 8 : 0);
    if (offset < step) return std::nullopt;
    return offset - step;
}

inline std::optional<std::uint32_t> RelativeMaxwellBranchTarget(std::uint32_t pc, std::int32_t branch_offset) {
    const auto target = static_cast<std::int64_t>(pc) + branch_offset + 8;
    if (target < 0 || target > std::numeric_limits<std::uint32_t>::max()) return std::nullopt;
    return static_cast<std::uint32_t>(target);
}

enum class ShaderTraceFlow {
    OutsideBacking,
    ReadFailed,
    DecodeInvalid,
    NonBranch,
    Bra,
    Jmp,
    Cal,
    Jcal,
    Indirect,
    OtherControlFlow,
};

enum class ShaderTraceTargetStatus { None, Direct, Indirect, OutOfRange };

struct ShaderTraceInstructionMetadata {
    ShaderTraceFlow flow = ShaderTraceFlow::DecodeInvalid;
    ShaderTraceTargetStatus target_status = ShaderTraceTargetStatus::None;
    std::uint32_t target = 0;
};

struct CbufReadProvenance {
    std::uint32_t cbuf_index = 0;
    std::uint32_t offset = 0;
    std::uint64_t bound_gpu_va = 0;
    std::uint64_t bound_size = 0;
    std::uint64_t read_gpu_va = 0;
    bool has_uniform_binding = false;
    nvn::UniformBufferBindOrigin bind_origin = nvn::UniformBufferBindOrigin::Guest;
    bool cpu_mapping_found = false;
    std::uint64_t cpu_address = 0;
    bool guest_memory_mapped = false;
    std::uint32_t value = 0;
};

inline const CbufReadProvenance* FindCbufReadProvenance(const std::vector<CbufReadProvenance>& reads,
                                                        std::uint32_t cbuf_index,
                                                        std::uint32_t offset) noexcept {
    for (auto it = reads.rbegin(); it != reads.rend(); ++it) {
        if (it->cbuf_index == cbuf_index && it->offset == offset) return &*it;
    }
    return nullptr;
}

inline std::string FormatCbufReadProvenance(const CbufReadProvenance& trace) {
    return fmt::format("cbuf={} offset=0x{:x} uniform_binding={} bind_origin={} "
                       "bound_gpu_va=0x{:x} bound_size=0x{:x} "
                       "read_gpu_va=0x{:x} cpu_mapping={} cpu_address=0x{:x} "
                       "guest_mapped={} value=0x{:08x}",
                       trace.cbuf_index, trace.offset, trace.has_uniform_binding,
                       nvn::UniformBufferBindOriginName(trace.bind_origin),
                       static_cast<unsigned long long>(trace.bound_gpu_va),
                       static_cast<unsigned long long>(trace.bound_size),
                       static_cast<unsigned long long>(trace.read_gpu_va), trace.cpu_mapping_found,
                       static_cast<unsigned long long>(trace.cpu_address), trace.guest_memory_mapped,
                       trace.value);
}


inline std::string_view ShaderTraceFlowName(ShaderTraceFlow flow) {
    switch (flow) {
    case ShaderTraceFlow::OutsideBacking:
        return "outside-backing";
    case ShaderTraceFlow::ReadFailed:
        return "read-failed";
    case ShaderTraceFlow::DecodeInvalid:
        return "decode-invalid";
    case ShaderTraceFlow::NonBranch:
        return "non-branch";
    case ShaderTraceFlow::Bra:
        return "BRA";
    case ShaderTraceFlow::Jmp:
        return "JMP";
    case ShaderTraceFlow::Cal:
        return "CAL";
    case ShaderTraceFlow::Jcal:
        return "JCAL";
    case ShaderTraceFlow::Indirect:
        return "indirect-branch";
    case ShaderTraceFlow::OtherControlFlow:
        return "other-control-flow";
    }
    return "unknown";
}

inline std::string_view ShaderTraceTargetStatusName(ShaderTraceTargetStatus status) {
    switch (status) {
    case ShaderTraceTargetStatus::None:
        return "none";
    case ShaderTraceTargetStatus::Direct:
        return "direct";
    case ShaderTraceTargetStatus::Indirect:
        return "indirect-or-indeterminate";
    case ShaderTraceTargetStatus::OutOfRange:
        return "overflow-or-underflow";
    }
    return "unknown";
}

// The caller injects the vendor decoder so the helper test can exercise safe
// failure behavior without constructing a shader or linking the full backend.
template <typename ReadInstruction, typename DecodeInstruction>
inline ShaderTraceInstructionMetadata InspectMaxwellTraceInstruction(const CpuBackingSpan& span, std::uint32_t offset,
                                                                     ReadInstruction&& read_instruction,
                                                                     DecodeInstruction&& decode_instruction) noexcept {
    if (!ContainsBackingAccess(span, offset, sizeof(std::uint64_t))) {
        return {.flow = ShaderTraceFlow::OutsideBacking};
    }

    std::uint64_t raw = 0;
    try {
        raw = read_instruction();
    } catch (...) {
        return {.flow = ShaderTraceFlow::ReadFailed};
    }

    const Shader::Maxwell::Instruction instruction{raw};
    try {
        using Shader::Maxwell::Opcode;
        switch (decode_instruction(raw)) {
        case Opcode::BRA: {
            const auto target = RelativeMaxwellBranchTarget(offset, instruction.branch.Offset());
            if (!target)
                return {.flow = ShaderTraceFlow::Bra, .target_status = ShaderTraceTargetStatus::OutOfRange};
            return {.flow = ShaderTraceFlow::Bra, .target_status = ShaderTraceTargetStatus::Direct, .target = *target};
        }
        case Opcode::JMP:
            return {.flow = ShaderTraceFlow::Jmp,
                    .target_status = ShaderTraceTargetStatus::Direct,
                    .target = instruction.branch.Absolute()};
        case Opcode::CAL: {
            const auto target = RelativeMaxwellBranchTarget(offset, instruction.branch.Offset());
            if (!target)
                return {.flow = ShaderTraceFlow::Cal, .target_status = ShaderTraceTargetStatus::OutOfRange};
            return {.flow = ShaderTraceFlow::Cal, .target_status = ShaderTraceTargetStatus::Direct, .target = *target};
        }
        case Opcode::JCAL:
            return {.flow = ShaderTraceFlow::Jcal,
                    .target_status = ShaderTraceTargetStatus::Direct,
                    .target = instruction.branch.Absolute()};
        case Opcode::BRX:
        case Opcode::JMX:
            return {.flow = ShaderTraceFlow::Indirect, .target_status = ShaderTraceTargetStatus::Indirect};
        case Opcode::PBK:
        case Opcode::PCNT:
        case Opcode::PEXIT:
        case Opcode::PLONGJMP:
        case Opcode::SSY: {
            const auto target = RelativeMaxwellBranchTarget(offset, instruction.branch.Offset());
            if (!target)
                return {.flow = ShaderTraceFlow::OtherControlFlow, .target_status = ShaderTraceTargetStatus::OutOfRange};
            return {.flow = ShaderTraceFlow::OtherControlFlow,
                    .target_status = ShaderTraceTargetStatus::Direct,
                    .target = *target};
        }
        case Opcode::BRK:
        case Opcode::CONT:
        case Opcode::EXIT:
        case Opcode::LONGJMP:
        case Opcode::PRET:
        case Opcode::RET:
        case Opcode::SYNC:
            return {.flow = ShaderTraceFlow::OtherControlFlow};
        default:
            return {.flow = ShaderTraceFlow::NonBranch};
        }
    } catch (...) {
        return {.flow = ShaderTraceFlow::DecodeInvalid};
    }
}

} // namespace nxrt::render
