// SPDX-License-Identifier: GPL-3.0-or-later

#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/indirect_branch_table_track.h"

#include <cstdlib>
#include <iostream>
#include <unordered_map>

namespace {

constexpr u64 Nop() { return u64{0x50b0} << 48; }

void Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

class FixtureEnvironment final : public Shader::Environment {
public:
    u64 ReadInstruction(u32 address) override {
        if (const auto instruction = instructions.find(address); instruction != instructions.end()) {
            return instruction->second;
        }
        return Nop();
    }

    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return static_cast<Shader::TextureType>(0); }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return static_cast<Shader::TexturePixelFormat>(0);
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 0; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override { return std::nullopt; }
    void Dump(u64, u64) override {}

    std::unordered_map<u32, u64> instructions;
};

u64 Imnmx(u32 dest, u32 src, u32 immediate) {
    return (u64{0x3820} << 48) | dest | (u64{src} << 8) | (u64{immediate} << 20);
}

u64 Shl(u32 dest, u32 src, u32 immediate) {
    return (u64{0x3848} << 48) | dest | (u64{src} << 8) | (u64{immediate} << 20);
}

u64 MovReg(u32 dest, u32 src) {
    return (u64{0x5c98} << 48) | dest | (u64{src} << 20) | (u64{0xf} << 39);
}

u64 Ldc(u32 dest, u32 src, u32 cbuf, u32 offset) {
    return (u64{0xef90} << 48) | (u64{4} << 48) | dest | (u64{src} << 8) |
           (u64{offset} << 20) | (u64{cbuf} << 36);
}

u64 Brx(u32 src, s32 offset) {
    return (u64{0xe25} << 52) | ((static_cast<u64>(offset) & 0xffffff) << 20) | (u64{src} << 8);
}

FixtureEnvironment MakeFixture(bool clobber_shl_result) {
    using namespace Shader::Maxwell;
    FixtureEnvironment env;
    env.instructions.emplace(0x5c8, Brx(14, -0x550));
    env.instructions.emplace(0x5b8, Ldc(14, 9, 1, 0x30));
    env.instructions.emplace(0x5b0, Nop());
    if (clobber_shl_result) {
        env.instructions.emplace(0x5a8, MovReg(9, 0));
        env.instructions.emplace(0x598, Shl(9, 11, 2));
        env.instructions.emplace(0x590, Imnmx(11, 0, 5));
    } else {
        env.instructions.emplace(0x5a8, Shl(9, 11, 2));
        env.instructions.emplace(0x598, Imnmx(11, 0, 5));
    }
    Check(Decode(env.instructions.at(0x5c8)) == Opcode::BRX, "fixture BRX encoding must decode");
    Check(Decode(env.instructions.at(0x5b8)) == Opcode::LDC, "fixture LDC encoding must decode");
    Check(Decode(env.instructions.at(clobber_shl_result ? 0x598 : 0x5a8)) == Opcode::SHL_imm,
          "fixture SHL encoding must decode");
    Check(Decode(env.instructions.at(clobber_shl_result ? 0x590 : 0x598)) == Opcode::IMNMX_imm,
          "fixture IMNMX encoding must decode");
    if (clobber_shl_result) {
        Check(Decode(env.instructions.at(0x5a8)) == Opcode::MOV_reg, "fixture clobber encoding must decode");
    }
    return env;
}

void TestValidChainIsTracked() {
    auto env = MakeFixture(false);
    const auto table = Shader::Maxwell::TrackIndirectBranchTable(
        env, Shader::Maxwell::Location{0x5c8}, Shader::Maxwell::Location{0x500});
    Check(table.has_value(), "valid IMNMX/SHL/LDC/BRX chain should be tracked");
    Check(table->cbuf_index == 1 && table->cbuf_offset == 0x30 && table->num_entries == 6,
          "valid chain should preserve its cbuf table and entry count");
}

void TestClobberedShiftValueIsRejected() {
    auto env = MakeFixture(true);
    const auto table = Shader::Maxwell::TrackIndirectBranchTable(
        env, Shader::Maxwell::Location{0x5c8}, Shader::Maxwell::Location{0x500});
    Check(!table, "tracker must reject an older SHL after an intervening MOV clobbers its destination");
}

} // namespace

int main() {
    TestValidChainIsTracked();
    TestClobberedShiftValueIsRejected();
}
