// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"

namespace Shader::Ampere {
class ProgramCaptureState {
public:
    enum class Result { Continue, Complete, InvalidTarget };
    static constexpr u32 MaximumInstructions = 65536;

    Result Observe(const Instruction& instruction, u32 index) {
        const u32 opcode = instruction.Bits(0, 12);
        if (index < exit_setup.size()) {
            exit_setup[index] = instruction;
            if (index + 1 == exit_setup.size() &&
                std::all_of(exit_setup.begin(), exit_setup.end(), [](const Instruction& setup) {
                    return (setup.words[3] & 0xc00001ffU) == 0;
                }) &&
                exit_setup[0].words[0] == 0x0000734e &&
                exit_setup[0].words[1] == 0 && exit_setup[0].words[2] == 0 &&
                exit_setup[1].words[0] == 0x00037802 && exit_setup[1].words[2] == 0xf00 &&
                exit_setup[2].words[0] == 0x00007210 && exit_setup[2].words[1] == 3 &&
                exit_setup[2].words[2] == 0x07f1e0ff &&
                exit_setup[3].words[1] == 0xff &&
                ((exit_setup[3].words[0] == 0x01017210 &&
                  exit_setup[3].words[2] == 0x007fe4ff) ||
                 (exit_setup[3].words[0] == 0xff017224 &&
                  exit_setup[3].words[2] == 0x000e0401)) &&
                instruction.words[0] == 0x00007357 && instruction.words[1] == 0 &&
                instruction.words[2] == 0) {
                const u32 target = exit_setup[1].words[1];
                if (target < exit_setup.size() * 16 || target % 16 ||
                    target >= MaximumInstructions * 16)
                    return Result::InvalidTarget;
                last_forward_target = std::max(last_forward_target, target / 16);
            }
        }
        if (opcode == 0x944 || opcode == 0x947 || opcode == 0x945) {
            // BRA keeps its .DIV/.CONV mode in the offset's two low bits.
            const u32 low = instruction.words[1] & (opcode == 0x947 ? ~3U : ~0U);
            const u64 raw = low | (u64{instruction.words[2] & 0x3ffff} << 32);
            const s64 relative = opcode == 0x945 ? static_cast<s32>(instruction.words[1])
                                                : static_cast<s64>(raw << 14) >> 14;
            const s64 target = s64{index} * 16 + 16 + relative;
            if (target < 0 || target % 16 || target >= MaximumInstructions * 16)
                return Result::InvalidTarget;
            // CALL.REL.NOINC (bit 86) pushes no return address, but its callee still returns
            // with RET.REL.NODEC through the link register set ahead of the call.
            if (opcode == 0x944) {
                last_call_target = std::max(last_call_target, static_cast<u32>(target / 16));
            } else {
                last_forward_target = std::max(last_forward_target, static_cast<u32>(target / 16));
            }
            // Every CALL but the lowering's predicated NOINC forward transfer (@P CALL.REL.NOINC
            // over the next BRA) is a linked call, whose callee ends at a RET.
            if (opcode == 0x944 && !(instruction.Bits(12, 4) != 7 && instruction.words[1] == 16 &&
                                     instruction.words[2] == 0x03c00000))
                has_calls = true;
            // The compiler ends every program with an unconditional branch to itself; with a jump
            // table (whose targets live in a constant buffer) that is the only reliable end. It
            // is also the end of a program without calls when no branch seen so far reaches past
            // it: programs whose only exits are predicated (TotK's cutscene video network loops
            // back with BRA and leaves through @P0 EXIT) have no unconditional EXIT to stop at.
            // As in the lowering, a BRA.DIV (mode 2) never branches and bits 87-90 add a condition.
            const bool always_branches =
                instruction.Bits(32, 2) != 2 && instruction.Bits(87, 4) == 7;
            if (opcode == 0x947 && instruction.Bits(12, 4) == 7 && target == s64{index} * 16 &&
                (computed_branch || (always_branches && !has_calls &&
                                     index >= last_forward_target && index >= last_call_target)))
                return Result::Complete;
        }
        if (opcode == 0x949 || opcode == 0x958) computed_branch = true;
        if (computed_branch) return Result::Continue;
        if (opcode == 0x94d && instruction.Bits(12, 4) == 7 && index >= last_forward_target) {
            if (index >= last_call_target) return Result::Complete;
            main_exited = true;
        }
        if (opcode == 0x950 && instruction.Bits(12, 4) == 7 && main_exited &&
            index >= last_call_target && index >= last_forward_target)
            return Result::Complete;
        return Result::Continue;
    }

private:
    std::array<Instruction, 5> exit_setup{};
    u32 last_call_target{};
    u32 last_forward_target{};
    bool main_exited{};
    bool computed_branch{};
    bool has_calls{};
};
} // namespace Shader::Ampere
