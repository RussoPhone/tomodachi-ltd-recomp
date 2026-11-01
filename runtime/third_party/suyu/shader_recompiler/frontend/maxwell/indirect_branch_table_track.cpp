// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <optional>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/indirect_branch_table_track.h"
#include "shader_recompiler/frontend/maxwell/opcodes.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/load_constant.h"

namespace Shader::Maxwell {
namespace {
constexpr size_t MaxScannedIndirectBranchInstructions = 256;

union Encoding {
    u64 raw;
    BitField<0, 8, IR::Reg> dest_reg;
    BitField<8, 8, IR::Reg> src_reg;
    BitField<20, 19, u64> immediate;
    BitField<56, 1, u64> is_negative;
    BitField<20, 24, s64> brx_offset;
};

struct TrackedInstruction {
    u64 raw;
    u32 offset;
};

bool IsControlFlow(Opcode opcode) {
    switch (opcode) {
    case Opcode::BRA:
    case Opcode::BRX:
    case Opcode::JMP:
    case Opcode::JMX:
    case Opcode::CAL:
    case Opcode::JCAL:
    case Opcode::RET:
    case Opcode::BRK:
    case Opcode::CONT:
    case Opcode::EXIT:
    case Opcode::LONGJMP:
    case Opcode::PLONGJMP:
    case Opcode::PRET:
    case Opcode::PBK:
    case Opcode::PCNT:
    case Opcode::PEXIT:
    case Opcode::SSY:
    case Opcode::SYNC: return true;
    default: return false;
    }
}

// The table pattern is a data-flow chain, not just a set of nearby opcodes.
// Maxwell's common GPR destination field occupies the low byte, so reject a
// candidate if an intervening instruction may overwrite the value we track.
// Unknown writes are treated conservatively: rejecting a candidate falls back
// to normal translation, while accepting a stale definition produces a bad BRX.
bool HasInterveningClobberOrControlFlow(Environment& env, Location begin, Location end, IR::Reg reg) {
    for (Location pos{begin}; pos < end; ++pos) {
        const u64 insn{env.ReadInstruction(pos.Offset())};
        const Opcode opcode{Decode(insn)};
        if (IsControlFlow(opcode) || (insn & 0xff) == static_cast<u32>(reg)) {
            return true;
        }
    }
    return false;
}

template <typename Callable>
std::optional<TrackedInstruction> Track(Environment& env, Location block_begin, Location& pos,
                                        std::vector<IndirectBranchScannedInstruction>* scanned,
                                        u32& scanned_omitted,
                                        Callable&& func) {
    while (pos >= block_begin) {
        const u32 offset = pos.Offset();
        const u64 insn{env.ReadInstruction(offset)};
        --pos;
        const Opcode opcode = Decode(insn);
        if (scanned) {
            if (scanned->size() < MaxScannedIndirectBranchInstructions) {
                scanned->push_back({.offset = offset,
                                    .opcode = NameOf(opcode),
                                    .raw_low_byte = static_cast<u32>(insn & 0xff),
                                    .candidate_low_byte_match_mask = 0,
                                    .control_flow = IsControlFlow(opcode)});
            } else {
                ++scanned_omitted;
            }
        }
        if (func(insn, opcode)) {
            return TrackedInstruction{insn, offset};
        }
    }
    return std::nullopt;
}

std::optional<TrackedInstruction> TrackLDC(Environment& env, Location block_begin, Location& pos,
                                           IR::Reg brx_reg,
                                           std::vector<IndirectBranchScannedInstruction>* scanned,
                                           u32& scanned_omitted) {
    return Track(env, block_begin, pos, scanned, scanned_omitted, [brx_reg](u64 insn, Opcode opcode) {
        const LDC::Encoding ldc{insn};
        return opcode == Opcode::LDC && ldc.dest_reg == brx_reg && ldc.size == LDC::Size::B32 &&
               ldc.mode == LDC::Mode::Default;
    });
}

std::optional<TrackedInstruction> TrackSHL(Environment& env, Location block_begin, Location& pos,
                                           IR::Reg ldc_reg,
                                           std::vector<IndirectBranchScannedInstruction>* scanned,
                                           u32& scanned_omitted) {
    return Track(env, block_begin, pos, scanned, scanned_omitted, [ldc_reg](u64 insn, Opcode opcode) {
        const Encoding shl{insn};
        return opcode == Opcode::SHL_imm && shl.dest_reg == ldc_reg;
    });
}

std::optional<TrackedInstruction> TrackIMNMX(Environment& env, Location block_begin, Location& pos,
                                             IR::Reg shl_reg,
                                             std::vector<IndirectBranchScannedInstruction>* scanned,
                                             u32& scanned_omitted) {
    return Track(env, block_begin, pos, scanned, scanned_omitted, [shl_reg](u64 insn, Opcode opcode) {
        const Encoding imnmx{insn};
        return opcode == Opcode::IMNMX_imm && imnmx.dest_reg == shl_reg;
    });
}
} // Anonymous namespace

std::optional<IndirectBranchTableInfo> TrackIndirectBranchTable(Environment& env, Location brx_pos,
                                                                Location block_begin) {
    const u64 brx_insn{env.ReadInstruction(brx_pos.Offset())};
    const Opcode brx_opcode{Decode(brx_insn)};
    if (brx_opcode != Opcode::BRX && brx_opcode != Opcode::JMX) {
        throw LogicError("Tracked instruction is not BRX or JMX");
    }
    const IR::Reg brx_reg{Encoding{brx_insn}.src_reg};
    const s32 brx_offset{static_cast<s32>(Encoding{brx_insn}.brx_offset)};

    std::vector<IndirectBranchScannedInstruction> scanned;
    auto* const scan_evidence = env.TraceShaderFailure() ? &scanned : nullptr;
    u32 scanned_omitted = 0;
    Location pos{brx_pos};
    const auto ldc_match = TrackLDC(env, block_begin, pos, brx_reg, scan_evidence, scanned_omitted);
    if (!ldc_match) {
        return std::nullopt;
    }
    const LDC::Encoding ldc{ldc_match->raw};
    const u32 cbuf_index{static_cast<u32>(ldc.index)};
    const u32 cbuf_offset{static_cast<u32>(static_cast<s32>(ldc.offset.Value()))};
    const IR::Reg ldc_reg{ldc.src_reg};
    if (HasInterveningClobberOrControlFlow(env, Location{ldc_match->offset} + 1, brx_pos, brx_reg)) {
        return std::nullopt;
    }

    const auto shl_match = TrackSHL(env, block_begin, pos, ldc_reg, scan_evidence, scanned_omitted);
    if (!shl_match) {
        return std::nullopt;
    }
    const Encoding shl{shl_match->raw};
    const IR::Reg shl_reg{shl.src_reg};
    if (HasInterveningClobberOrControlFlow(env, Location{shl_match->offset} + 1,
                                           Location{ldc_match->offset}, ldc_reg)) {
        return std::nullopt;
    }

    const auto imnmx_match = TrackIMNMX(env, block_begin, pos, shl_reg, scan_evidence, scanned_omitted);
    if (!imnmx_match) {
        return std::nullopt;
    }
    const Encoding imnmx{imnmx_match->raw};
    if (imnmx.is_negative != 0 ||
        HasInterveningClobberOrControlFlow(env, Location{imnmx_match->offset} + 1,
                                           Location{shl_match->offset}, shl_reg)) {
        return std::nullopt;
    }
    const u32 imnmx_immediate{static_cast<u32>(imnmx.immediate.Value())};
    IndirectBranchCandidateTrace candidate_trace{
        .brx_offset = brx_pos.Offset(),
        .brx_source_register = brx_reg,
        .ldc_offset = ldc_match->offset,
        .ldc_destination_register = ldc.dest_reg,
        .ldc_source_register = ldc.src_reg,
        .shl_offset = shl_match->offset,
        .shl_destination_register = shl.dest_reg,
        .shl_source_register = shl.src_reg,
        .imnmx_offset = imnmx_match->offset,
        .imnmx_destination_register = imnmx.dest_reg,
        .imnmx_source_register = Encoding{imnmx_match->raw}.src_reg,
        .scanned_omitted = scanned_omitted,
        .intervening_omitted = 0,
        .intervening = {},
    };
    for (const auto& instruction : scanned) {
        if (instruction.offset <= imnmx_match->offset || instruction.offset >= brx_pos.Offset() ||
            instruction.offset == shl_match->offset || instruction.offset == ldc_match->offset) {
            continue;
        }
        u32 match_mask = 0;
        const std::array<IR::Reg, 7> candidate_registers{
            brx_reg, IR::Reg{ldc.dest_reg}, IR::Reg{ldc.src_reg}, IR::Reg{shl.dest_reg}, IR::Reg{shl.src_reg},
            IR::Reg{imnmx.dest_reg}, IR::Reg{Encoding{imnmx_match->raw}.src_reg}};
        for (size_t index = 0; index < candidate_registers.size(); ++index) {
            if (instruction.raw_low_byte == static_cast<u32>(candidate_registers[index])) {
                match_mask |= 1u << index;
            }
        }
        if (candidate_trace.intervening.size() < 16) {
            auto evidence = instruction;
            evidence.candidate_low_byte_match_mask = match_mask;
            candidate_trace.intervening.push_back(std::move(evidence));
        } else {
            ++candidate_trace.intervening_omitted;
        }
    }
    return IndirectBranchTableInfo{
        .cbuf_index = cbuf_index,
        .cbuf_offset = cbuf_offset,
        .num_entries = imnmx_immediate + 1,
        .branch_offset = brx_offset,
        .branch_reg = brx_reg,
        .ldc_offset = ldc_match->offset,
        .shl_offset = shl_match->offset,
        .imnmx_offset = imnmx_match->offset,
        .candidate_trace = std::move(candidate_trace),
    };
}

} // namespace Shader::Maxwell
