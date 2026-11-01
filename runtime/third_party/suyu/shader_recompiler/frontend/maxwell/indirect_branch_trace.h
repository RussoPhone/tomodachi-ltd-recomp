// SPDX-FileCopyrightText: Copyright 2026 nxrt contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string>
#include <vector>

#include <fmt/format.h>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/reg.h"
#include "shader_recompiler/frontend/maxwell/opcodes.h"

namespace Shader::Maxwell {

struct IndirectBranchScannedInstruction {
    u32 offset{};
    std::string opcode;
    // Raw low byte is deliberately not decoded as a destination register:
    // Maxwell's operand fields vary by opcode, so matches are diagnostic hints only.
    u32 raw_low_byte{};
    u32 candidate_low_byte_match_mask{};
    bool control_flow{};
};

struct IndirectBranchCandidateTrace {
    u32 brx_offset{};
    IR::Reg brx_source_register{};
    u32 ldc_offset{};
    IR::Reg ldc_destination_register{};
    IR::Reg ldc_source_register{};
    u32 shl_offset{};
    IR::Reg shl_destination_register{};
    IR::Reg shl_source_register{};
    u32 imnmx_offset{};
    IR::Reg imnmx_destination_register{};
    IR::Reg imnmx_source_register{};
    u32 scanned_omitted{};
    u32 intervening_omitted{};
    std::vector<IndirectBranchScannedInstruction> intervening;
};

inline std::string FormatIndirectBranchCandidateTrace(const IndirectBranchCandidateTrace& trace) {
    std::string formatted = fmt::format(
        "brx=+0x{:x} src=r{} ldc=+0x{:x} dest=r{} src=r{} shl=+0x{:x} dest=r{} src=r{} "
        "imnmx=+0x{:x} dest=r{} src=r{} scanned_omitted={} intervening_omitted={} intervening=[",
        trace.brx_offset, static_cast<u32>(trace.brx_source_register), trace.ldc_offset,
        static_cast<u32>(trace.ldc_destination_register), static_cast<u32>(trace.ldc_source_register),
        trace.shl_offset, static_cast<u32>(trace.shl_destination_register),
        static_cast<u32>(trace.shl_source_register), trace.imnmx_offset,
        static_cast<u32>(trace.imnmx_destination_register), static_cast<u32>(trace.imnmx_source_register),
        trace.scanned_omitted, trace.intervening_omitted);
    bool first = true;
    for (const auto& instruction : trace.intervening) {
        if (!first) formatted += ',';
        first = false;
        formatted += fmt::format("+0x{:x}:{}:candidate_match_heuristic_mask=0x{:x}:control_flow={}",
                                 instruction.offset, instruction.opcode,
                                 instruction.candidate_low_byte_match_mask,
                                 static_cast<unsigned>(instruction.control_flow));
    }
    formatted += ']';
    return formatted;
}

} // namespace Shader::Maxwell
