// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_capture.h"
#include "shader_recompiler/frontend/maxwell/decode.h"

namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;
using Shader::Ampere::ProgramCaptureState;

constexpr Instruction EXIT{{0x0000794d, 0, 0x03800000, 0}};

/// Checks that register SGXT handles out-of-range bit counts before extracting signed or unsigned
/// fields.
TEST_CASE("Ampere register SGXT clamps the full bit count before extraction", "[video_core][ampere]") {
    using Shader::Maxwell::Opcode;
    // SGXT.U32 R16, R21, R16: the destination also holds the bit count.
    for (const u32 predicate : {0U, 7U, 8U}) {
        const Instruction extend{{0x1510021aU | (predicate << 12), 16, 0, 0}};
        const auto lowered = LowerToMaxwell(std::array{extend, EXIT});
        REQUIRE(static_cast<bool>(lowered));
        for (const u32 count : {0U, 1U, 16U, 31U, 32U, 33U, 255U, 256U, 0xffffffffU}) {
            for (const u32 value : {0U, 0x80000001U, 0xffffffffU}) {
                std::array<u32, 256> registers{};
                registers[16] = count;
                registers[21] = value;
                for (const u64 word : lowered.instructions) {
                    const auto opcode = Shader::Maxwell::Decode(word);
                    if (opcode == Opcode::EXIT) break;
                    REQUIRE(((word >> 16) & 15) == predicate);
                    const u32 dest = word & 255;
                    const u32 source = registers[(word >> 8) & 255];
                    switch (opcode) {
                    case Opcode::IMNMX_imm:
                        REQUIRE(((word >> 39) & 15) == 7); // Unsigned minimum, PT.
                        REQUIRE(((word >> 48) & 1) == 0);
                        registers[dest] = std::min(source, static_cast<u32>((word >> 20) & 0x7ffff));
                        break;
                    case Opcode::SHL_imm:
                        registers[dest] = source << ((word >> 20) & 31);
                        break;
                    case Opcode::BFE_reg: {
                        const u32 control = registers[(word >> 20) & 255];
                        const u32 bits = (control >> 8) & 255;
                        REQUIRE((control & 255) == 0);
                        REQUIRE(bits <= 32);
                        REQUIRE(((word >> 48) & 1) == 0);
                        registers[dest] = bits == 32 ? source : source & ((1U << bits) - 1);
                        break;
                    }
                    default:
                        FAIL("Unexpected instruction in register SGXT");
                    }
                }
                const u32 expected = count >= 32 ? value : value & ((1U << count) - 1);
                REQUIRE(registers[16] == expected);
            }
        }
    }
    for (const u32 unsupported : {0x200U, 0x800U}) { // Signed and wrapping forms.
        const Instruction extend{{0x1510721a, 16, unsupported, 0}};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{extend, EXIT})));
    }
}

/// Checks emitted F2F fields for conversion to double, including source selection and modifiers.
TEST_CASE("Ampere float-to-double conversion preserves its source and modifiers", "[video_core][ampere]") {
    for (const u32 source : {2U, 44U, 255U}) {
        for (const u32 modifiers : {0U, 0x40000000U, 0x80000000U, 0xc0000000U}) {
            const Instruction convert{{0x002c0310, source | modifiers, 0x00201800, 0}};
            const auto lowered = LowerToMaxwell(std::array{convert, EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::F2F_reg);
            REQUIRE((word & 255) == 44);
            REQUIRE(((word >> 16) & 15) == 0); // @P0
            REQUIRE(((word >> 8) & 3) == 3); // F64 destination pair.
            REQUIRE(((word >> 10) & 3) == 2); // F32 source.
            REQUIRE(((word >> 20) & 255) == source);
            REQUIRE(((word >> 49) & 1) == ((modifiers >> 30) & 1));
            REQUIRE(((word >> 45) & 1) == (modifiers >> 31));
        }
    }
    for (const u32 dest : {43U, 254U, 255U}) {
        const Instruction convert{{0x00007310U | (dest << 16), 2, 0x00201800, 0}};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{convert, EXIT})));
    }
    const Instruction unsupported{{0x002c7310, 2, 0x00201801, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unsupported, EXIT})));
}

/// Checks double-to-float rounding and rejects invalid register pairs or conversion modifiers.
TEST_CASE("Ampere double-to-float conversion keeps rounding and aligned source pairs", "[video_core][ampere]") {
    for (const u32 rounding : {0U, 1U, 2U, 3U}) {
        const Instruction convert{{0x00337310, 60, 0x00301000U | (rounding << 14), 0}};
        const auto lowered = LowerToMaxwell(std::array{convert, EXIT});
        REQUIRE(static_cast<bool>(lowered));
        const u64 word = lowered.instructions.front();
        REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::F2F_reg);
        REQUIRE((word & 255) == 51); // Single-register destinations may be odd.
        REQUIRE(((word >> 20) & 255) == 60);
        REQUIRE(((word >> 8) & 3) == 2);
        REQUIRE(((word >> 10) & 3) == 3);
        REQUIRE(((word >> 39) & 3) == rounding);
    }
    for (const u32 source : {1U, 254U, 255U}) {
        const Instruction convert{{0x00337310, source, 0x00301000, 0}};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{convert, EXIT})));
    }
}

/// Checks double addition operand order and rejects unsupported register or constant-buffer forms.
TEST_CASE("Ampere double arithmetic retains constant-buffer and register operand order", "[video_core][ampere]") {
    for (const u32 rounding : {0U, 1U, 2U, 3U}) {
        const std::array<Instruction, 4> operations{{
            {{0x2a267a2b, 0x00400200, 44U | (rounding << 14), 0}}, // DFMA R38, R42, c[1][8], R44
            {{0x1014762b, 0x00400200, 20U | (rounding << 14), 0}}, // DFMA R20, R16, R20, c[1][8]
            {{0x2c2e7229, 0, 46U | (rounding << 14), 0}}, // DADD R46, R44, R46
            {{0x26347629, 0x00400600, rounding << 14, 0}}, // DADD R52, R38, c[1][24]
        }};
        for (size_t index = 0; index < operations.size(); ++index) {
            const auto lowered = LowerToMaxwell(std::array{operations[index], EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            const std::array opcodes{Shader::Maxwell::Opcode::DFMA_cr, Shader::Maxwell::Opcode::DFMA_rc,
                                     Shader::Maxwell::Opcode::DADD_reg, Shader::Maxwell::Opcode::DADD_cbuf};
            REQUIRE(Shader::Maxwell::Decode(word) == opcodes[index]);
            REQUIRE((word & 255) == operations[index].Bits(16, 8));
            REQUIRE(((word >> 8) & 255) == operations[index].Bits(24, 8));
            REQUIRE(((word >> (index < 2 ? 50 : 39)) & 3) == rounding);
            if (index == 2) {
                REQUIRE(((word >> 20) & 255) == 46);
            } else {
                REQUIRE(((word >> 34) & 31) == 1);
                REQUIRE(((word >> 20) & 0x3fff) == (index == 3 ? 6U : 2U));
                if (index < 2) REQUIRE(((word >> 39) & 255) == operations[index].Bits(64, 8));
            }
        }
    }
}

/// Checks that double multiplication preserves its constant-buffer operand and validates modifiers.
TEST_CASE("Ampere double multiply retains the full constant-buffer operand", "[video_core][ampere]") {
    // DMUL R44, R44, c[1][0], reached after TotK's F2F.F64.F32.
    for (const u32 rounding : {0U, 1U, 2U, 3U}) {
        for (const u32 negate : {0U, 1U, 2U, 3U}) {
            const Instruction multiply{{0x2c2c7a28, 0x00400000U | ((negate & 1) << 31),
                                        (rounding << 14) | ((negate >> 1) << 8), 0}};
            const auto lowered = LowerToMaxwell(std::array{multiply, EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::DMUL_cbuf);
            REQUIRE((word & 255) == 44);
            REQUIRE(((word >> 8) & 255) == 44);
            REQUIRE(((word >> 34) & 31) == 1);
            REQUIRE(((word >> 20) & 0x3fff) == 0);
            REQUIRE(((word >> 39) & 3) == rounding);
            REQUIRE(((word >> 48) & 1) == ((negate ^ (negate >> 1)) & 1));
        }
    }
    const Instruction unaligned{{0x2c2c7a28, 0x00400100, 0, 0}}; // c[1][4]
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{unaligned, EXIT})));
    const Instruction absolute{{0x2c2c7a28, 0x40400000, 0, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{absolute, EXIT})));
}

/// Checks register-form double multiplication and rejects unsupported modifiers or register pairs.
TEST_CASE("Ampere double multiply lowers register operands", "[video_core][ampere]") {
    // DMUL R12, R36, R0, from a TotK graphics program.
    for (const u32 rounding : {0U, 1U, 2U, 3U}) {
        for (const u32 negate : {0U, 1U, 2U, 3U}) {
            const Instruction multiply{{0x240c7228, (negate & 1) << 31,
                                        (rounding << 14) | ((negate >> 1) << 8), 0}};
            const auto lowered = LowerToMaxwell(std::array{multiply, EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::DMUL_reg);
            REQUIRE((word & 255) == 12);
            REQUIRE(((word >> 8) & 255) == 36);
            REQUIRE(((word >> 20) & 255) == 0);
            REQUIRE(((word >> 39) & 3) == rounding);
            REQUIRE(((word >> 48) & 1) == ((negate ^ (negate >> 1)) & 1));
        }
    }
    for (const u32 second : {1U, 254U, 255U}) { // Odd, reserved or RZ second pairs.
        const Instruction multiply{{0x240c7228, second, 0, 0}};
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{multiply, EXIT})));
    }
    const Instruction absolute_b{{0x240c7228, 0x40000000, 0, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{absolute_b, EXIT})));
    const Instruction absolute_a{{0x240c7228, 0, 0x200, 0}};
    REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{absolute_a, EXIT})));
}

/// BRA (unconditional) from `index` to `target`, both instruction indices.
Instruction Branch(s64 index, s64 target) {
    const u64 relative = static_cast<u64>((target - index - 1) * 16);
    return Instruction{{0x00007947, static_cast<u32>(relative),
                        0x03800000U | static_cast<u32>((relative >> 32) & 0x3ffff), 0}};
}

/// Checks capture termination at closing self-branches and preserves paths with reachable code or
/// calls.
TEST_CASE("Ampere capture ends at the closing self-branch of a program without an unconditional EXIT", "[video_core][ampere]") {
    // TotK's cutscene video network: a loop left only through @P0 EXIT, then BRA to itself.
    constexpr Instruction PREDICATED_EXIT{{0x0000094d, 0, 0x03800000, 0}};
    const std::array<Instruction, 3> looping{{PREDICATED_EXIT, Branch(1, 0), Branch(2, 2)}};
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(looping[0], 0) == ProgramCaptureState::Result::Continue);
    REQUIRE(capture.Observe(looping[1], 1) == ProgramCaptureState::Result::Continue);
    REQUIRE(capture.Observe(looping[2], 2) == ProgramCaptureState::Result::Complete);
    REQUIRE(static_cast<bool>(LowerToMaxwell(looping)));

    // A self-branch that an earlier branch jumps past is not the end.
    ProgramCaptureState skipped;
    REQUIRE(skipped.Observe(Branch(0, 3), 0) == ProgramCaptureState::Result::Continue);
    REQUIRE(skipped.Observe(Branch(1, 1), 1) == ProgramCaptureState::Result::Continue);

    // BRA.DIV never branches in the lowering, and a condition in bits 87-90 makes the branch
    // conditional: neither ends the program.
    Instruction divergent = Branch(2, 2);
    divergent.words[1] |= 2;
    Instruction conditional = Branch(2, 2);
    conditional.words[2] &= ~0x07800000U; // P0 as the second condition
    for (const Instruction& branch : {divergent, conditional}) {
        ProgramCaptureState not_terminal;
        REQUIRE(not_terminal.Observe(looping[0], 0) == ProgramCaptureState::Result::Continue);
        REQUIRE(not_terminal.Observe(looping[1], 1) == ProgramCaptureState::Result::Continue);
        REQUIRE(not_terminal.Observe(branch, 2) == ProgramCaptureState::Result::Continue);
    }

    // A linked call (CALL.REL.NOINC with the link set up) has a callee that ends at RET, so a
    // self-branch inside it does not end the capture.
    const Instruction call{{0x00007944, 16, 0x03c00000, 0}}; // to instruction 2
    ProgramCaptureState linked;
    REQUIRE(linked.Observe(call, 0) == ProgramCaptureState::Result::Continue);
    REQUIRE(linked.Observe(EXIT, 1) == ProgramCaptureState::Result::Continue);
    REQUIRE(linked.Observe(Branch(2, 2), 2) == ProgramCaptureState::Result::Continue);
}

/// Checks register-form double fused multiply-add and rejects unsupported modifiers or register
/// pairs.
TEST_CASE("Ampere double fused multiply-add lowers register operands", "[video_core][ampere]") {
    // DFMA R26, R34, R26, R12, from TotK's cutscene video network.
    for (const u32 rounding : {0U, 1U, 2U, 3U}) {
        for (const u32 negate : {0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U}) { // -b, -a, -c
            const Instruction fma{{0x221a722b, 0x1aU | ((negate & 1) << 31),
                                   12U | (rounding << 14) | (((negate >> 1) & 1) << 8) |
                                       ((negate >> 2) << 11),
                                   0}};
            const auto lowered = LowerToMaxwell(std::array{fma, EXIT});
            REQUIRE(static_cast<bool>(lowered));
            const u64 word = lowered.instructions.front();
            REQUIRE(Shader::Maxwell::Decode(word) == Shader::Maxwell::Opcode::DFMA_reg);
            REQUIRE((word & 255) == 26);
            REQUIRE(((word >> 8) & 255) == 34);
            REQUIRE(((word >> 20) & 255) == 26);
            REQUIRE(((word >> 39) & 255) == 12);
            REQUIRE(((word >> 48) & 1) == ((negate ^ (negate >> 1)) & 1));
            REQUIRE(((word >> 49) & 1) == (negate >> 2));
            REQUIRE(((word >> 50) & 3) == rounding);
        }
    }
    // |b|, |a| and |c| have no Maxwell DFMA form; odd or reserved pairs are rejected.
    for (const Instruction rejected : {Instruction{{0x221a722b, 0x4000001a, 12, 0}},
                                       Instruction{{0x221a722b, 0x1a, 12 | 0x200, 0}},
                                       Instruction{{0x221a722b, 0x1a, 12 | 0x400, 0}},
                                       Instruction{{0x221a722b, 0x1b, 12, 0}},
                                       Instruction{{0x221a722b, 0x1a, 13, 0}},
                                       Instruction{{0x221a722b, 0x1a, 254, 0}}}) {
        REQUIRE_FALSE(static_cast<bool>(LowerToMaxwell(std::array{rejected, EXIT})));
    }
}

// Lowered Maxwell words whose top bits (above `shift`) are `opcode`.
template <typename Program>
size_t CountMaxwell(const Program& lowered, u64 opcode, u32 shift) {
    return static_cast<size_t>(std::count_if(lowered.instructions.begin(),
                                             lowered.instructions.end(),
                                             [&](u64 word) { return (word >> shift) == opcode; }));
}

template <typename Program>
u64 FindMaxwell(const Program& lowered, u64 opcode, u32 shift) {
    const auto it = std::find_if(lowered.instructions.begin(), lowered.instructions.end(),
                                 [&](u64 word) { return (word >> shift) == opcode; });
    return it != lowered.instructions.end() ? *it : 0;
}

TEST_CASE("Ampere BRA.CONV always branches and BRA.DIV never does", "[video_core][ampere]") {
    // BRA.CONV UR6 / BRA.DIV ~URZ to the second EXIT: the mode is in the offset's low two bits.
    const std::array<Instruction, 3> convergent{{
        {{0x06007947, 0x10 | 3, 0x0b800000, 0}},
        EXIT,
        EXIT,
    }};
    ProgramCaptureState capture;
    REQUIRE(capture.Observe(convergent[0], 0) == ProgramCaptureState::Result::Continue);
    const auto taken = LowerToMaxwell(convergent);
    REQUIRE(static_cast<bool>(taken));
    REQUIRE(CountMaxwell(taken, 0xe24, 52) == 1);

    const std::array<Instruction, 3> divergent{{
        {{0xff007947, 0x10 | 2, 0x0b800000, 0}},
        EXIT,
        EXIT,
    }};
    const auto dropped = LowerToMaxwell(divergent);
    REQUIRE(static_cast<bool>(dropped));
    REQUIRE(CountMaxwell(dropped, 0xe24, 52) == 0);
}

TEST_CASE("Bananza captured BRA.U preserves its predicate and relative target",
          "[video_core][ampere]") {
    // 040074A01BF12000 fragment abb4ff47..., code offset 0x5b0.
    constexpr Instruction Nop{{0x00007918, 0, 0, 0x000fc000}};
    std::array<Instruction, 133> code;
    code.fill(Nop);
    code[91] = Instruction{{0x00008947, 0x00000281, 0x03800000, 0x000fea00}};
    code.back() = EXIT;
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == code.size());
    CHECK(lowered.instructions[91] == 0xe24000001b08000fULL);

    const auto backward = LowerToMaxwell(std::array{
        Nop, Instruction{{0x00008947, 0xffffffe1, 0x0383ffff, 0x000fea00}}, EXIT});
    REQUIRE(backward);
    CHECK(backward.instructions[1] == 0xe2400fffff08000fULL);

    const auto guarded = LowerToMaxwell(std::array{
        Instruction{{0x00008947, 0x11, 0x04800000, 0x000fea00}}, Nop, EXIT});
    REQUIRE(guarded);
    REQUIRE(guarded.instructions.size() == 4);
    CHECK(guarded.instructions[0] == 0xe24000000100000fULL);
    CHECK(guarded.instructions[1] == 0xe24000000109000fULL);
}

TEST_CASE("Ampere BRA.U rejects unsupported fields and invalid targets", "[video_core][ampere]") {
    const Instruction branch{{0x00008947, 0x1, 0x03800000, 0x000fea00}};
    for (const u32 bit : {82U, 83U, 84U, 85U, 86U, 91U, 92U, 93U, 94U, 95U,
                          96U, 97U, 98U, 99U, 100U, 101U, 102U, 103U, 104U}) {
        auto unsupported = branch;
        unsupported.words[bit / 32] |= 1U << (bit % 32);
        const auto lowered = LowerToMaxwell(std::array{unsupported, EXIT});
        INFO(bit);
        REQUIRE_FALSE(lowered);
        CHECK(lowered.error == "native branch convergence mode");
    }
    for (const u32 offset : {5U, 0x11U, 0xffffffe1U}) {
        auto invalid = branch;
        invalid.words[1] = offset;
        if (offset == 0xffffffe1U) invalid.words[2] |= 0x3ffff;
        REQUIRE_FALSE(LowerToMaxwell(std::array{invalid, EXIT}));
    }
}

TEST_CASE("Ampere TLD.SCR.LZ takes its handle from a uniform register", "[video_core][ampere]") {
    // TLD.SCR.LZ R5, R8, R4, R5, UR4, 0x0, 2D, 0x7 -> Maxwell TLD.B.
    const std::array<Instruction, 2> code{{
        {{0x04087f66, 0x30000405, 0x089e0705, 0x001f4200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 tld = FindMaxwell(lowered, 0xdd, 56);
    REQUIRE(tld != 0);
    REQUIRE(((tld >> 28) & 7) == 2); // 2D
    REQUIRE(((tld >> 31) & 0xf) == 7);
}

TEST_CASE("Ampere I2F converts signed 16-bit sources", "[video_core][ampere]") {
    // I2F.S16 R8, R8
    const std::array<Instruction, 2> code{{
        {{0x00087306, 0x00000008, 0x00101400, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    const u64 i2f = FindMaxwell(lowered, 0x5cb, 52);
    REQUIRE(((i2f >> 10) & 3) == 1); // 16-bit source
    REQUIRE(((i2f >> 13) & 1) == 1); // signed
}

TEST_CASE("Ampere integer-to-float conversions take immediates", "[video_core][ampere]") {
    const std::array<Instruction, 3> code{{
        {{0x00077906, 0x00000020, 0x00209000, 0x000e2200}},
        {{0x00047845, 0x00000100, 0x0020d000, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(lowered);
    REQUIRE(lowered.instructions.size() == 3);
    CHECK(lowered.instructions[0] == 0x010420000007f007ULL);
    CHECK(lowered.instructions[1] == 0x010438000007f004ULL);
    const Instruction warp_sync{{0x00007348, 0x00000007, 0x03800000, 0x000fe800}};
    const auto rejected = LowerToMaxwell(std::array{code[0], code[1], warp_sync, EXIT});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error == "unsupported native opcode");
    CHECK(rejected.error_offset == 0x20);
}

TEST_CASE("Ampere SUST.P keeps its channel mask", "[video_core][ampere]") {
    // SUST.P.2D.STRONG.SM.R.IGN [R6], R4, 0x0, 0x59
    const std::array<Instruction, 2> code{{
        {{0x06007b9b, 0x60005904, 0x0010a100, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((FindMaxwell(lowered, 0xeb2, 52) >> 20) & 0xf) == 1);
}

TEST_CASE("Ampere SHF.R.U64 immediate shifts keep their amount", "[video_core][ampere]") {
    // SHF.R.U64 R14, R14, n, R17 (Ocarina vertex shader 1a55fa57a335b963). The Maxwell
    // translator reads the immediate from bits 20-38, so no max-shift bits may be set there.
    for (const u32 shift : {0U, 1U, 21U, 31U, 32U}) {
        const std::array<Instruction, 2> code{{
            {{0x0e0e7819, shift, 0x00001211, 0x020fe200}},
            EXIT,
        }};
        const auto lowered = LowerToMaxwell(code);
        REQUIRE(static_cast<bool>(lowered));
        const u64 shf = FindMaxwell(lowered, 0x38f8, 48);
        REQUIRE(shf != 0);
        CHECK(((shf >> 20) & 0x7ffff) == shift);
        CHECK(((shf >> 39) & 0xff) == 17);
    }
}

TEST_CASE("Ampere SHFL with a register lane keeps its mode", "[video_core][ampere]") {
    // SHFL.DOWN PT, R7, R0, R5, 0x1f
    const std::array<Instruction, 2> code{{
        {{0x00077589, 0x08001f05, 0x000e0000, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(((FindMaxwell(lowered, 0xef1, 52) >> 30) & 3) == 2);
}

TEST_CASE("Ampere calls find a return address set ahead of them", "[video_core][ampere]") {
    // MOV R9, 0x30; MOV R4, 0x5; CALL.REL.NOINC; EXIT; RET.REL.NODEC R9
    std::array<Instruction, 5> code{{
        {{0x00097802, 0x30, 0x00000f00, 0}},
        {{0x00047802, 0x5, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x09007950, 0xffffffb0, 0x03c3ffff, 0}},
    }};
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));

    // The link register is written again between its MOV and the call.
    code[1] = {{0x00097802, 0x5, 0x00000f00, 0}};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(!static_cast<bool>(lowered));
    REQUIRE(lowered.error == "native call linkage mode");
}

TEST_CASE("Ampere programs go on past an EXIT that a branch jumps over", "[video_core][ampere]") {
    // A call, then a branch over an EXIT to more of the program, with another call there.
    const std::array<Instruction, 8> code{{
        {{0x00047802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x50, 0x03c00000, 0}},
        {{0x00007947, 0x10, 0x03800000, 0}},
        EXIT,
        {{0x00047802, 0x60, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x04007950, 0xffffff80, 0x03c3ffff, 0}},
    }};
    const auto lowered = LowerToMaxwell(code);
    REQUIRE(static_cast<bool>(lowered));
    REQUIRE(lowered.source_size == sizeof(code));
}

TEST_CASE("Ampere capture ends at the return of a helper called past the EXIT",
          "[video_core][ampere]") {
    // Ocarina 69a40579971fb867: MOV R0, 0x20; CALL.REL.NOINC; EXIT; RET.REL.NODEC R0, then the
    // next program in memory.
    const std::array<Instruction, 4> code{{
        {{0x00007802, 0x20, 0x00000f00, 0}},
        {{0x00007944, 0x10, 0x03c00000, 0}},
        EXIT,
        {{0x00007950, 0xffffffc0, 0x03c3ffff, 0}},
    }};
    ProgramCaptureState capture;
    for (u32 index = 0; index + 1 < code.size(); ++index)
        CHECK(capture.Observe(code[index], index) == ProgramCaptureState::Result::Continue);
    CHECK(capture.Observe(code.back(), 3) == ProgramCaptureState::Result::Complete);
    REQUIRE(static_cast<bool>(LowerToMaxwell(code)));

    ProgramCaptureState jump;
    CHECK(jump.Observe({{0x00007944, 0x10, 0x03c00000, 0}}, 0) == ProgramCaptureState::Result::Continue);
    CHECK(jump.Observe(EXIT, 1) == ProgramCaptureState::Result::Continue);
    CHECK(jump.Observe(EXIT, 2) == ProgramCaptureState::Result::Complete);
}

TEST_CASE("Ampere uniform select and bit operations reach every uniform register",
          "[video_core][ampere]") {
    // Ocarina e8decc3e5c0c184e: @UP0 USEL UR4, UR4, 0x20, UP1. UP0 and UP1 come from R239 into
    // P6 and P5; the SEL runs under P6 and the guest predicates are restored.
    const std::array<Instruction, 2> select{{
        {{0x04040887, 0x20, 0x08800000, 0x000fe200}},
        EXIT,
    }};
    const auto selected = LowerToMaxwell(select);
    INFO(selected.error);
    REQUIRE(static_cast<bool>(selected));
    const std::array<u64, 5> expected_select{
        0x010000000207f0fcULL, 0x38e800000607fffdULL, 0x38f000000607ef00ULL,
        0x5ca002800fc6dcdcULL, 0x38f000000607fd00ULL};
    CHECK(std::search(selected.instructions.begin(), selected.instructions.end(),
                      expected_select.begin(), expected_select.end()) != selected.instructions.end());

    // Ocarina 2079791ecb288b83: UBREV UR4, UR17.
    const std::array<Instruction, 2> reverse{{
        {{0x000472be, 0x11, 0x08000000, 0x000fc800}},
        EXIT,
    }};
    const auto reversed = LowerToMaxwell(reverse);
    INFO(reversed.error);
    REQUIRE(static_cast<bool>(reversed));
    CHECK(reversed.instructions.front() == 0x382801800007e9dcULL);

    // Ocarina 2079791ecb288b83: UFLO.U32.SH UR21, UR21.
    const std::array<Instruction, 2> leading{{
        {{0x001572bd, 0x15, 0x080e0400, 0x000fe400}},
        EXIT,
    }};
    const auto found = LowerToMaxwell(leading);
    INFO(found.error);
    REQUIRE(static_cast<bool>(found));
    CHECK(found.instructions.front() == 0x5c3002000ed700edULL);

    // Ocarina 2079791ecb288b83: UPOPC UR10, UR17.
    const std::array<Instruction, 2> count{{
        {{0x000a72bf, 0x11, 0x08000000, 0x000fe200}},
        EXIT,
    }};
    const auto counted = LowerToMaxwell(count);
    INFO(counted.error);
    REQUIRE(static_cast<bool>(counted));
    CHECK(counted.instructions.front() == 0x5c0800000e9700e2ULL);
}

TEST_CASE("Ampere guarded uniform comparisons keep the output when the guard is false",
          "[video_core][ampere]") {
    // Ocarina 2079791ecb288b83: @UP4 UISETP.GT.U32.AND UP0, UPT, UR21, 0x1f, UPT. UP4 and the old
    // UP0 load into P2 and P6, so a false guard merges UP0 back unchanged.
    const std::array<Instruction, 2> code{{
        {{0x1500488c, 0x1f, 0x0bf04070, 0x000fe200}},
        EXIT,
    }};
    const auto lowered = LowerToMaxwell(code);
    INFO(lowered.error);
    REQUIRE(static_cast<bool>(lowered));
    const std::array<u64, 6> expected{
        0x0100000001f7f0fcULL, 0x38e800000447fffdULL, 0x38f000000447ef00ULL,
        0x5b6803800fc2ed37ULL, 0x38e800000407efefULL, 0x38f000000447fd00ULL};
    CHECK(std::equal(expected.begin(), expected.end(), lowered.instructions.begin()));

    // SEL R3, R4, UR7, P0
    const std::array<Instruction, 2> select{{
        {{0x04037c07, 0x7, 0x08000000, 0x000fe400}},
        EXIT,
    }};
    const auto selected = LowerToMaxwell(select);
    INFO(selected.error);
    REQUIRE(static_cast<bool>(selected));
    CHECK(selected.instructions.front() == 0x5ca000000df70403ULL);
}

TEST_CASE("Ampere Ocarina byte constant loads, ULEA carries to UP3 and two-source F2IP",
          "[video_core][ampere]") {
    const auto lower = [](const Instruction& instruction) {
        const std::array<Instruction, 2> code{{instruction, EXIT}};
        return LowerToMaxwell(code);
    };
    const auto field = [](u64 word, u32 offset, u32 count) {
        return (word >> offset) & ((u64{1} << count) - 1);
    };
    // ULDC.U8 UR8, c[0x4][UR6+0x82]: LDC.U8 (size 0) indexed by UR6's register, odd offset kept.
    const auto byte_load = lower({{0x06087abb, 0x01002080, 0x08000000, 0x000fe400}});
    INFO(byte_load.error);
    REQUIRE(static_cast<bool>(byte_load));
    const u64 ldc = FindMaxwell(byte_load, 0xef9, 52);
    REQUIRE(ldc != 0);
    CHECK(field(ldc, 48, 3) == 0);
    CHECK(field(ldc, 20, 16) == 0x82);
    CHECK(field(ldc, 36, 5) == 4);

    // ULEA UR10, UP3, UR7, UR10, 0x2: the carry is computed in P3 (6 - 3) and merged into R239.
    const auto lea = lower({{0x070a7291, 0x0000000a, 0x0f86103f, 0x000fe200}});
    INFO(lea.error);
    REQUIRE(static_cast<bool>(lea));
    const u64 compare = FindMaxwell(lea, 0x5b6, 52);
    REQUIRE(compare != 0);
    CHECK(field(compare, 3, 3) == 3);
    CHECK(std::any_of(lea.instructions.begin(), lea.instructions.end(), [&](u64 word) {
        return (word >> 52) == 0x38e && field(word, 0, 8) == 239 && field(word, 20, 8) == 8;
    }));

    // F2IP.U8.F32.NTZ R45, R44, R21, RZ: two byte conversions, R44's scaled into byte 1.
    const auto pack = lower({{0x2c2d7243, 0x00000015, 0x000004ff, 0x000fc400}});
    INFO(pack.error);
    REQUIRE(static_cast<bool>(pack));
    CHECK(CountMaxwell(pack, 0x5cb, 52) == 2);
    CHECK(FindMaxwell(pack, 0x5c1, 52) != 0);

    // ULDC UR7, c[0x4][UR15+0x29]: only the indexed sum has to be aligned.
    const auto indexed = lower({{0x0f077abb, 0x01000a40, 0x08000800, 0x000fc600}});
    INFO(indexed.error);
    REQUIRE(static_cast<bool>(indexed));
    CHECK(field(FindMaxwell(indexed, 0xef9, 52), 20, 16) == 0x29);

    // I2F.U8 R0, UR17.B3: Maxwell I2F with byte selector 3 (bits 41-42).
    const auto convert = lower({{0x00097d06, 0x30000011, 0x08001000, 0x000fe200}});
    INFO(convert.error);
    REQUIRE(static_cast<bool>(convert));
    CHECK(field(FindMaxwell(convert, 0x5cb, 52), 41, 2) == 3);
}
} // namespace
