// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

constexpr u32 OP_ENTRY_POINT = 15;
constexpr u32 OP_FUNCTION = 54;
constexpr u32 OP_BRANCH_CONDITIONAL = 250;
constexpr u32 OP_SWITCH = 251;
constexpr u32 FunctionControlDontInline = 0x2;

// EDEN_NX2_LEAN_SSBO scoped to one test case.
struct LeanSsbo {
    std::string previous;
    bool existed{};

    explicit LeanSsbo(const char* mode) {
        if (const char* value = std::getenv("EDEN_NX2_LEAN_SSBO")) {
            previous = value;
            existed = true;
        }
        Set(mode);
    }

    static void Set(const char* value) {
#ifdef _WIN32
        _putenv_s("EDEN_NX2_LEAN_SSBO", value ? value : "");
#else
        if (value) {
            setenv("EDEN_NX2_LEAN_SSBO", value, 1);
        } else {
            unsetenv("EDEN_NX2_LEAN_SSBO");
        }
#endif
    }

    ~LeanSsbo() {
        Set(existed ? previous.c_str() : nullptr);
    }
};

std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, u32 opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        if ((code[offset] & 0xffffU) == opcode) {
            result.emplace_back(code.begin() + offset + 1, code.begin() + offset + size);
        }
        offset += size;
    }
    return result;
}

// A compute program whose global accesses cannot be bound to one buffer: every access goes
// through the helpers DefineGlobalMemoryFunctions emits, which route over `buffers` candidates.
struct GlobalProgram {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    Profile profile;

    explicit GlobalProgram(u32 buffers) {
        program.stage = Stage::Compute;
        program.workgroup_size = {64, 1, 1};
        program.blocks = {&block};
        program.post_order_blocks = {&block};
        program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                               {{}, IR::AbstractSyntaxNode::Type::Return}};
        profile.supported_spirv = 0x00010500;
        profile.support_int64 = true;
        profile.support_descriptor_aliasing = true;
        profile.min_ssbo_alignment = 16;
        auto& info = program.info;
        info.uses_global_memory = true;
        info.uses_int64 = true; // the helpers take a 64-bit address
        info.native_global_bounds = true;
        info.used_storage_buffer_types = IR::Type::U32 | IR::Type::U32x2 | IR::Type::U32x4;
        info.constant_buffer_descriptors = {{0, 1}};
        info.constant_buffer_mask = 1;
        info.used_constant_buffer_types = IR::Type::U32 | IR::Type::U32x2;
        for (u32 index = 0; index < buffers; ++index) {
            info.storage_buffers_descriptors.push_back({0, index * 16, 1, true});
        }
    }

    // EDEN_SPIRV_TEST_OUTPUT=<dir> writes the module out, for spirv-val.
    std::vector<u32> Emit(std::string_view name) {
        const auto code = Backend::SPIRV::EmitSPIRV(profile, program);
        if (const char* directory = std::getenv("EDEN_SPIRV_TEST_OUTPUT")) {
            std::filesystem::create_directories(directory);
            std::ofstream file{std::filesystem::path{directory} / (std::string{name} + ".spv"),
                               std::ios::binary};
            file.write(reinterpret_cast<const char*>(code.data()),
                       static_cast<std::streamsize>(code.size() * sizeof(u32)));
            REQUIRE(file.good());
        }
        return code;
    }
};

// The helpers are every function but the entry point, which OpEntryPoint names.
std::vector<std::vector<u32>> Helpers(const std::vector<u32>& code) {
    const auto entry_points = Instructions(code, OP_ENTRY_POINT);
    REQUIRE(entry_points.size() == 1);
    const u32 main_id = entry_points[0][1];
    auto functions = Instructions(code, OP_FUNCTION);
    const auto main = std::ranges::find_if(
        functions, [main_id](const auto& words) { return words[1] == main_id; });
    REQUIRE(main != functions.end());
    functions.erase(main);
    return functions;
}

} // namespace

// From Duck912's 9a362357a2 (Eglantine). EDEN_NX2_LEAN_SSBO=0 restores the old code, but it is
// read into a function-local static once per process, so there is no second case here for it.
// Ocarina of Time's foliage compute programs have ~30
// candidate buffers and 40-odd native global accesses; NVIDIA's compiler inlined the routing
// helper at each one and took about 2.4 s per program.
TEST_CASE("Native global memory helpers stay calls and route with one switch",
          "[video_core][spirv][ocarina_of_time]") {
    constexpr u32 Buffers = 12;
    LeanSsbo lean{"1"};
    GlobalProgram fixture{Buffers};
    const auto code = fixture.Emit("native-global-lean");
    // Six helpers (load and write for u32, u32x2 and u32x4), each kept as a call.
    const auto helpers = Helpers(code);
    REQUIRE(helpers.size() == 6);
    for (const auto& words : helpers) {
        REQUIRE((words[2] & FunctionControlDontInline) != 0);
    }
    // One switch per helper over all the candidates, and no branch per candidate left.
    const auto switches = Instructions(code, OP_SWITCH);
    REQUIRE(switches.size() == 6);
    for (const auto& words : switches) {
        REQUIRE(words.size() == 2 + 2 * Buffers); // selector, default, then (literal, label)
        for (u32 index = 0; index < Buffers; ++index) {
            REQUIRE(words[2 + index * 2] == index);
        }
    }
    REQUIRE(Instructions(code, OP_BRANCH_CONDITIONAL).empty());
}
