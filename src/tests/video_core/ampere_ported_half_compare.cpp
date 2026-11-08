// SPDX-License-Identifier: GPL-3.0-or-later
#include <array>
#include <catch2/catch_test_macros.hpp>
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

TEST_CASE("Ampere packed half comparisons preserve all register-form controls",
          "[video_core][ampere][fire_emblem]") {
    for (u32 cmp = 0; cmp < 16; ++cmp) {
        for (u32 combine = 0; combine < 3; ++combine) {
            for (u32 swizzle = 0; swizzle < 4; ++swizzle) {
                for (u32 flags = 0; flags < 64; ++flags) {
                    const u32 a_neg = flags & 1, a_abs = (flags >> 1) & 1;
                    const u32 b_neg = (flags >> 2) & 1, b_abs = (flags >> 3) & 1;
                    const u32 bf = (flags >> 4) & 1, ftz = (flags >> 5) & 1;
                    const std::array<Instruction, 2> code{{
                        {{0x02017233, 3U | (swizzle << 28) | (b_abs << 30) | (b_neg << 31),
                          (combine << 5) | (bf << 7) | (a_neg << 8) | (a_abs << 9) |
                          (swizzle << 10) | (cmp << 12) | (ftz << 16) | (10U << 23), 0}},
                        {{0x794d, 0, 0x03800000, 0}},
                    }};
                    const auto lowered = LowerToMaxwell(code);
                    REQUIRE(lowered);
                    const u64 expected = 0x5d18000000070001ULL | (2ULL << 8) | (3ULL << 20) |
                        (u64{swizzle} << 28) | (u64{b_abs} << 30) | (u64{b_neg} << 31) |
                        (u64{cmp} << 35) | (10ULL << 39) | (u64{a_neg} << 43) |
                        (u64{a_abs} << 44) | (u64{combine} << 45) | (u64{swizzle} << 47) |
                        (u64{bf} << 49) | (u64{ftz} << 50);
                    REQUIRE(lowered.instructions.front() == expected);
                }
            }
        }
    }
    std::array<Instruction, 2> invalid{{{{0x02017233, 3, 3U << 5, 0}}, {{0x794d, 0, 0x03800000, 0}}}};
    REQUIRE_FALSE(LowerToMaxwell(invalid));
}

// Mario Kart World emits HSETP2.GT.AND P1, PT, RZ.H0_H0, c[0x7][0xfc].H1_H1, !P1 (opcode 0x634):
// the constant buffer form of the packed half compare. It was rejected outright, which dropped the
// shader and left roads invisible. The lowering loads the word into the scratch register and emits
// the register form, so the B swizzle, abs and neg bits have to survive untouched.
TEST_CASE("Ampere packed half comparisons accept a constant buffer source",
          "[video_core][ampere][mario_kart_world]") {
    constexpr u32 Bank = 7, Offset = 0xfc;      // c[0x7][0xfc]
    constexpr u32 ARegister = 255;              // RZ
    constexpr u32 DestinationP = 1, DestinationQ = 7, InputPredicate = 1, InputNegate = 1;
    constexpr u32 Scratch = 250;
    for (u32 cmp = 0; cmp < 16; ++cmp) {
        for (u32 combine = 0; combine < 3; ++combine) {
            for (u32 swizzle = 0; swizzle < 4; ++swizzle) {
                for (u32 flags = 0; flags < 64; ++flags) {
                    const u32 a_neg = flags & 1, a_abs = (flags >> 1) & 1;
                    const u32 b_neg = (flags >> 2) & 1, b_abs = (flags >> 3) & 1;
                    const u32 h_and = (flags >> 4) & 1, ftz = (flags >> 5) & 1;
                    const std::array<Instruction, 2> code{{
                        {{0x634 | (7U << 12) | (ARegister << 24),
                          (Offset << 6) | (Bank << 22) | (swizzle << 28) | (b_abs << 30) |
                              (b_neg << 31),
                          (combine << 5) | (h_and << 7) | (a_neg << 8) | (a_abs << 9) |
                              (swizzle << 10) | (cmp << 12) | (ftz << 16) | (DestinationP << 17) |
                              (DestinationQ << 20) | (InputPredicate << 23) | (InputNegate << 26),
                          0}},
                        {{0x794d, 0, 0x03800000, 0}},
                    }};
                    const auto lowered = LowerToMaxwell(code);
                    REQUIRE(lowered);
                    REQUIRE(lowered.instructions.size() >= 2);
                    const u64 load = 0x4c98000000000000ULL | (7ULL << 16) | Scratch |
                        (u64{Offset / 4} << 20) | (u64{Bank} << 34) | (15ULL << 39);
                    REQUIRE(lowered.instructions[0] == load);
                    const u64 compare = 0x5d20000000000000ULL | (7ULL << 16) | DestinationQ |
                        (u64{DestinationP} << 3) | (u64{ftz} << 6) | (u64{ARegister} << 8) |
                        (u64{Scratch} << 20) | (u64{swizzle} << 28) | (u64{b_abs} << 30) |
                        (u64{b_neg} << 31) | (u64{cmp} << 35) | (u64{InputPredicate} << 39) |
                        (u64{InputNegate} << 42) | (u64{a_neg} << 43) | (u64{a_abs} << 44) |
                        (u64{combine} << 45) | (u64{swizzle} << 47) | (u64{h_and} << 49);
                    REQUIRE(lowered.instructions[1] == compare);
                }
            }
        }
    }
    const auto lower_one = [](u32 word1, u32 word2) {
        const std::array<Instruction, 2> code{{
            {{0x634 | (7U << 12) | (ARegister << 24), word1, word2, 0}},
            {{0x794d, 0, 0x03800000, 0}},
        }};
        return static_cast<bool>(LowerToMaxwell(code));
    };
    // An address the lowering cannot fold into an LDC, a bank out of range, the bit 91 form and a
    // mode field it does not decode all have to stay refusals rather than lower to the wrong word.
    REQUIRE(lower_one((Offset << 6) | (Bank << 22), 0));
    REQUIRE_FALSE(lower_one(((Offset + 2) << 6) | (Bank << 22), 0));
    REQUIRE_FALSE(lower_one((Offset << 6) | (20U << 22), 0));
    REQUIRE_FALSE(lower_one((Offset << 6) | (Bank << 22), 1U << 27));
    REQUIRE_FALSE(lower_one((Offset << 6) | (Bank << 22), 1U));
}
