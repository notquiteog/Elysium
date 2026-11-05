// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <optional>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

// Compute encodings Yoshi and the Mysterious Book rejected in 0.7.0 (words captured from its log).
namespace {
using Shader::Ampere::Instruction;
using Shader::Ampere::LowerToMaxwell;

constexpr Instruction exit_instruction{{0x0000794d, 0, 0x03800000, 0}};

// The lowered instruction whose top 12 bits match, if the program lowered.
std::optional<u64> Lowered(const Instruction& instruction, u64 top) {
    const std::array code{instruction, exit_instruction};
    const auto lowered = LowerToMaxwell(code);
    if (!lowered) return std::nullopt;
    for (const u64 maxwell : lowered.instructions) {
        if ((maxwell >> 52) == top) return maxwell;
    }
    FAIL("lowered program has no instruction with the expected opcode");
    return std::nullopt;
}

u32 Field(u64 value, u32 offset, u32 count) {
    return static_cast<u32>((value >> offset) & ((u64{1} << count) - 1));
}
} // Anonymous namespace

TEST_CASE("Ampere ATOMG register address keeps its operation", "[video_core][ampere][yoshi]") {
    // ATOMG.E.EXCH.STRONG.GPU R4, [R4.64], R7
    const auto atom = Lowered({{0x040473a8, 0x00000007, 0x041ee100, 0x00016800}}, 0xed8);
    REQUIRE(atom);
    REQUIRE(Field(*atom, 0, 8) == 4);
    REQUIRE(Field(*atom, 8, 8) == 4);
    REQUIRE(Field(*atom, 20, 8) == 7);
    REQUIRE(Field(*atom, 48, 1) == 1);
    // Operation 9 (compare and swap) has no Maxwell ATOM form here.
    REQUIRE_FALSE(LowerToMaxwell(std::array<Instruction, 2>{
        {{{0x040473a8, 0x00000007, 0x049ee100, 0x00016800}}, exit_instruction}}));
}

TEST_CASE("Ampere SULD lowers buffer and 3D surfaces", "[video_core][ampere][yoshi]") {
    const auto volume = Lowered({{0x00057b97, 0xa0c00400, 0x001e0100, 0x000f6200}}, 0xeb0);
    REQUIRE(volume);
    REQUIRE(Field(*volume, 33, 3) == 5);
    REQUIRE(Field(*volume, 20, 4) == 1);
    REQUIRE(Field(*volume, 36, 13) == 4);
    const auto buffer = Lowered({{0x0f007b97, 0x20c00000, 0x001e0100, 0x000f6200}}, 0xeb0);
    REQUIRE(buffer);
    REQUIRE(Field(*buffer, 33, 3) == 1);
    // 1D_ARRAY (2) stays rejected.
    REQUIRE_FALSE(LowerToMaxwell(std::array<Instruction, 2>{
        {{{0x0f007b97, 0x40c00000, 0x001e0100, 0x000f6200}}, exit_instruction}}));
}

TEST_CASE("Ampere SUATOM lowers to a bound Maxwell surface atomic", "[video_core][ampere][yoshi]") {
    const auto image = Lowered({{0x020c7593, 0x60c0080c, 0x001ee000, 0x00052200}}, 0xea0);
    REQUIRE(image);
    REQUIRE(Field(*image, 0, 8) == 0x0c);  // dest
    REQUIRE(Field(*image, 8, 8) == 0x02);  // coordinates
    REQUIRE(Field(*image, 20, 8) == 0x0c); // operand
    REQUIRE(Field(*image, 29, 4) == 0);    // ADD
    REQUIRE(Field(*image, 33, 3) == 3);    // 2D
    REQUIRE(Field(*image, 36, 13) == 8);
    REQUIRE(Field(*image, 54, 1) == 0); // bound, not bindless
    const auto buffer = Lowered({{0x09060593, 0x20c00006, 0x001ee000, 0x004ea200}}, 0xea0);
    REQUIRE(buffer);
    REQUIRE(Field(*buffer, 33, 3) == 1);
    for (const u32 word1 : {0x40c0080cU, 0xc0c0080cU}) { // 1D_ARRAY, dimension 6
        REQUIRE_FALSE(LowerToMaxwell(std::array<Instruction, 2>{
            {{{0x020c7593, word1, 0x001ee000, 0x00052200}}, exit_instruction}}));
    }
}

TEST_CASE("Ampere SURED lowers a buffer reduction", "[video_core][ampere][yoshi]") {
    const auto reduction = Lowered({{0x0000759f, 0x20c00019, 0x0010e000, 0x0011e200}}, 0xeb4);
    REQUIRE(reduction);
    REQUIRE(Field(*reduction, 33, 3) == 1);
    REQUIRE(Field(*reduction, 0, 8) == 0x19);
}
