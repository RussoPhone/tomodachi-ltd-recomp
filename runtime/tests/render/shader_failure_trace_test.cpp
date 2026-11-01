#include "render/shader_failure_trace.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/indirect_branch_trace.h"

#include <cstdlib>
#include <iostream>
#include <limits>

namespace {

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void TestPcAndInstructionSpanStatus() {
    const nxrt::render::CpuBackingSpan span{0x8000, 0x20};
    const auto inside = nxrt::render::GetShaderTracePcStatus(span, 0x1c);
    Check(inside.pc_inside, "PC at the final backing byte should be inside");
    Check(!inside.instruction_fits, "instruction crossing backing end should not fit");

    const auto outside = nxrt::render::GetShaderTracePcStatus(span, 0x20);
    Check(!outside.pc_inside, "PC at the backing end should be outside");
    Check(!outside.instruction_fits, "instruction at backing end should not fit");
}

void TestPreviousMaxwellInstructionOffset() {
    Check(nxrt::render::PreviousMaxwellInstructionOffset(0x123456f8) == 0x123456f0,
          "previous instruction should honor Maxwell bundle stepping");
    Check(nxrt::render::PreviousMaxwellInstructionOffset(0x123456e8) == 0x123456d8,
          "previous instruction should skip the bundle control word where needed");
}

void TestCbufReadProvenanceFormatting() {
    using namespace nxrt::render;
    Check(nxrt::nvn::NvnStageForMaxwellStage(1) == 0 && nxrt::nvn::NvnStageForMaxwellStage(2) == 3 &&
              nxrt::nvn::NvnStageForMaxwellStage(3) == 4 && nxrt::nvn::NvnStageForMaxwellStage(4) == 2 &&
              nxrt::nvn::NvnStageForMaxwellStage(5) == 1,
          "BindProgram MME stage IDs must map to NVN stage slots, not Maxwell stage minus one");
    Check(nxrt::nvn::NvnStageForMaxwellStage(0) == nxrt::nvn::kInvalidNvnStage,
          "invalid Maxwell stage should not map to an NVN stage slot");
    Check(nxrt::nvn::DriverBindOriginForMethod(0x904 + 2 * 8) ==
              nxrt::nvn::UniformBufferBindOrigin::DriverCbBind,
          "CB_BIND method should preserve its distinct driver-bind origin");
    Check(nxrt::nvn::DriverBindOriginForMethod(0xe18) ==
              nxrt::nvn::UniformBufferBindOrigin::DriverBindProgram,
          "BindProgram MME method should preserve its distinct driver-bind origin");
    Check(nxrt::nvn::DriverBindOriginForMethod(0x904 + 2 * 8, 0x31) ==
              nxrt::nvn::UniformBufferBindOrigin::Guest,
          "CB_BIND slots owned by the guest should not be classified as driver binds");
    const CbufReadProvenance trace{
        .cbuf_index = 1,
        .offset = 0x18,
        .bound_gpu_va = 0x04065000,
        .bound_size = 0x100,
        .read_gpu_va = 0x04065018,
        .has_uniform_binding = true,
        .bind_origin = nxrt::nvn::UniformBufferBindOrigin::DriverBindProgram,
        .cpu_mapping_found = true,
        .cpu_address = 0x20001018,
        .guest_memory_mapped = true,
        .value = 0x04065462,
    };
    Check(FormatCbufReadProvenance(trace) ==
              "cbuf=1 offset=0x18 uniform_binding=true bind_origin=bind-program-mme "
              "bound_gpu_va=0x4065000 bound_size=0x100 "
              "read_gpu_va=0x4065018 cpu_mapping=true cpu_address=0x20001018 guest_mapped=true "
              "value=0x04065462",
          "failure trace should expose the bound UBO and exact mapped guest read without shader bytes");
    auto cb_bind = trace;
    cb_bind.bind_origin = nxrt::nvn::UniformBufferBindOrigin::DriverCbBind;
    Check(FormatCbufReadProvenance(cb_bind).find("bind_origin=cb-bind") != std::string::npos,
          "failure trace should distinguish CB_BIND from BindProgram MME binds");
    auto user_binding = trace;
    user_binding.bind_origin = nxrt::nvn::UniformBufferBindOrigin::Guest;
    Check(FormatCbufReadProvenance(user_binding).find("bind_origin=guest") != std::string::npos,
          "failure trace should distinguish guest uniform-buffer binds");
}

void TestCbufReadProvenanceSelection() {
    using namespace nxrt::render;
    const std::vector<CbufReadProvenance> reads{
        {.cbuf_index = 1, .offset = 0x18, .value = 0x11111111},
        {.cbuf_index = 2, .offset = 0x30, .value = 0x22222222},
        {.cbuf_index = 1, .offset = 0x30, .value = 0x04065462},
        {.cbuf_index = 1, .offset = 0x3c, .value = 0},
    };
    const CbufReadProvenance* const selected = FindCbufReadProvenance(reads, 1, 0x30);
    Check(selected && selected->value == 0x04065462,
          "failure trace should select the read matching cbuf index and table entry offset");
}

void TestIndirectCandidateEvidenceFormatting() {
    using namespace Shader::Maxwell;
    const IndirectBranchCandidateTrace trace{
        .brx_offset = 0x5c8,
        .brx_source_register = Shader::IR::Reg::R7,
        .ldc_offset = 0x5b8,
        .ldc_destination_register = Shader::IR::Reg::R7,
        .ldc_source_register = Shader::IR::Reg::R9,
        .shl_offset = 0x5b0,
        .shl_destination_register = Shader::IR::Reg::R9,
        .shl_source_register = Shader::IR::Reg::R11,
        .imnmx_offset = 0x5a8,
        .imnmx_destination_register = Shader::IR::Reg::R11,
        .imnmx_source_register = Shader::IR::Reg::R0,
        .scanned_omitted = 4,
        .intervening = {{.offset = 0x5c0,
                         .opcode = "IADD (reg)",
                         .candidate_low_byte_match_mask = 1,
                         .control_flow = false}},
    };
    const std::string formatted = FormatIndirectBranchCandidateTrace(trace);
    Check(formatted.find("brx=+0x5c8 src=r7") != std::string::npos &&
              formatted.find("ldc=+0x5b8 dest=r7 src=r9") != std::string::npos &&
              formatted.find("shl=+0x5b0 dest=r9 src=r11") != std::string::npos &&
              formatted.find("imnmx=+0x5a8 dest=r11 src=r0") != std::string::npos,
          "candidate trace should show matched register IDs and offsets");
    Check(formatted.find("scanned_omitted=4 intervening_omitted=0") != std::string::npos,
          "candidate trace should report scan and output truncation counts");
    Check(formatted.find("+0x5c0:IADD (reg):candidate_match_heuristic_mask=0x1:control_flow=0") !=
              std::string::npos,
          "candidate trace should expose heuristic register-field matches and control flow without shader bytes");
}

void TestRelativeBranchTarget() {
    Check(nxrt::render::RelativeMaxwellBranchTarget(0x100, 0x20) == 0x128,
          "relative branch target should be based on PC plus instruction size");
    Check(nxrt::render::RelativeMaxwellBranchTarget(0x100, -0x20) == 0xe8,
          "negative relative branch target should be preserved");
    Check(!nxrt::render::RelativeMaxwellBranchTarget(0, -9),
          "relative branch target underflow should be rejected");
    Check(!nxrt::render::RelativeMaxwellBranchTarget(std::numeric_limits<std::uint32_t>::max(), 0),
          "relative branch target overflow should be rejected");
}

std::uint64_t Bra(std::int32_t offset) {
    return (0xE24ull << 52) | ((static_cast<std::uint64_t>(offset) & 0xFFFFFF) << 20);
}

std::uint64_t Jmp(std::uint32_t target) { return (0xE21ull << 52) | (static_cast<std::uint64_t>(target) << 20); }

void TestExactInstructionFlowAndTargets() {
    using namespace nxrt::render;
    const CpuBackingSpan span{0x8000, 0x100};
    const auto read = [](std::uint64_t raw) { return [raw] { return raw; }; };

    const auto bra = InspectMaxwellTraceInstruction(span, 0x10, read(Bra(0x20)), Shader::Maxwell::Decode);
    Check(bra.flow == ShaderTraceFlow::Bra, "exact instruction should classify BRA");
    Check(bra.target_status == ShaderTraceTargetStatus::Direct && bra.target == 0x38,
          "BRA target should use the current instruction PC and signed offset");

    const auto jmp = InspectMaxwellTraceInstruction(span, 0x20, read(Jmp(0x2345)), Shader::Maxwell::Decode);
    Check(jmp.flow == ShaderTraceFlow::Jmp, "exact instruction should classify JMP");
    Check(jmp.target_status == ShaderTraceTargetStatus::Direct && jmp.target == 0x2345,
          "JMP target should preserve the decoded absolute address");

    const auto indirect = InspectMaxwellTraceInstruction(span, 0x28, read(0xE25ull << 52), Shader::Maxwell::Decode);
    Check(indirect.flow == ShaderTraceFlow::Indirect &&
              indirect.target_status == ShaderTraceTargetStatus::Indirect,
          "BRX target should be reported as indirect and indeterminate");

    const auto underflow = InspectMaxwellTraceInstruction(span, 0, read(Bra(-9)), Shader::Maxwell::Decode);
    Check(underflow.flow == ShaderTraceFlow::Bra && underflow.target_status == ShaderTraceTargetStatus::OutOfRange,
          "BRA target underflow should be reported without wrapping");

    const auto overflow = InspectMaxwellTraceInstruction(CpuBackingSpan{0, std::numeric_limits<std::uint32_t>::max()},
                                                         std::numeric_limits<std::uint32_t>::max() - 8, read(Bra(1)),
                                                         Shader::Maxwell::Decode);
    Check(overflow.flow == ShaderTraceFlow::Bra && overflow.target_status == ShaderTraceTargetStatus::OutOfRange,
          "BRA target overflow should be reported without wrapping");

    const auto non_branch = InspectMaxwellTraceInstruction(span, 0x30, read(0x50Bull << 52), Shader::Maxwell::Decode);
    Check(non_branch.flow == ShaderTraceFlow::NonBranch,
          "a successfully decoded arithmetic/control-neutral instruction should "
          "be non-branch");

    const auto invalid = InspectMaxwellTraceInstruction(span, 0x40, read(0), Shader::Maxwell::Decode);
    Check(invalid.flow == ShaderTraceFlow::DecodeInvalid,
          "decoder rejection should be recorded as metadata instead of escaping");

    bool read_attempted = false;
    const auto failed_read = InspectMaxwellTraceInstruction(
        span, 0x50,
        [&]() -> std::uint64_t {
            read_attempted = true;
            throw 1;
        },
        Shader::Maxwell::Decode);
    Check(read_attempted && failed_read.flow == ShaderTraceFlow::ReadFailed,
          "instruction read failure should become metadata instead of escaping");

    bool out_of_span_read_attempted = false;
    const auto outside = InspectMaxwellTraceInstruction(
        span, 0x100,
        [&]() -> std::uint64_t {
            out_of_span_read_attempted = true;
            return 0;
        },
        Shader::Maxwell::Decode);
    Check(!out_of_span_read_attempted && outside.flow == ShaderTraceFlow::OutsideBacking,
          "out-of-backing instruction should not be read");
}

} // namespace

int main() {
    TestPcAndInstructionSpanStatus();
    TestPreviousMaxwellInstructionOffset();
    TestCbufReadProvenanceFormatting();
    TestCbufReadProvenanceSelection();
    TestIndirectCandidateEvidenceFormatting();
    TestRelativeBranchTarget();
    TestExactInstructionFlowAndTargets();
}
