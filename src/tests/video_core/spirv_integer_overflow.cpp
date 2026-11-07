// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdint>
#include <limits>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
namespace IR = Shader::IR;

/// Signed 32-bit addition overflow, by definition rather than by the emitters' formulation.
constexpr bool AddOverflows(s32 a, s32 b) {
    const s64 wide = static_cast<s64>(a) + static_cast<s64>(b);
    return wide > static_cast<s64>((std::numeric_limits<s32>::max)()) ||
           wide < static_cast<s64>((std::numeric_limits<s32>::min)());
}

/// What the emitters compute: for a >= 0, b > INT32_MAX - a; for a < 0, b < INT32_MIN - a. The
/// bounds are built in unsigned arithmetic and read back as signed, exactly as the shader does.
constexpr bool AddOverflowsAsEmitted(s32 a, s32 b) {
    constexpr u32 s32_max{static_cast<u32>((std::numeric_limits<s32>::max)())};
    constexpr u32 s32_min{static_cast<u32>((std::numeric_limits<s32>::min)())};
    const u32 unsigned_a = static_cast<u32>(a);
    const auto as_signed = [](u32 value) { return static_cast<s32>(value); };
    return a >= 0 ? b > as_signed(s32_max - unsigned_a) : b < as_signed(s32_min - unsigned_a);
}

class TestEnvironment final : public Shader::Environment {
public:
    TestEnvironment() {
        stage = Shader::Stage::Compute;
    }
    u64 ReadInstruction(u32) override {
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override {
        return 0;
    }
    Shader::TextureType ReadTextureType(u32) override {
        return Shader::TextureType::Color2D;
    }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return false;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {1, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

/// The 32-bit values of every OpConstant in the module.
std::vector<u32> Constants(const std::vector<u32>& code) {
    std::vector<u32> values;
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        REQUIRE(count > 0);
        REQUIRE(offset + count <= code.size());
        if ((code[offset] & 0xffff) == static_cast<u32>(spv::Op::OpConstant) && count == 4) {
            values.push_back(code[offset + 3]);
        }
        offset += count;
    }
    return values;
}

bool Has(const std::vector<u32>& values, u32 wanted) {
    for (const u32 value : values) {
        if (value == wanted) {
            return true;
        }
    }
    return false;
}

/// A compute shader that adds two values and stores the overflow flag.
std::vector<u32> EmitOverflowingAdd() {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Shader::Stage::Compute;
    program.workgroup_size = {1, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.storage_buffers_descriptors = {{0, 0, 1, true}};

    IR::IREmitter ir{block};
    const IR::U32 a{ir.LocalInvocationIdX()};
    const IR::U32 b{ir.LocalInvocationIdY()};
    const IR::U32 sum{ir.IAdd(a, b)};
    const IR::U1 overflow{ir.GetOverflowFromOp(sum)};
    block.AppendNewInst(IR::Opcode::WriteStorage32,
                        {ir.Imm32(0), ir.Imm32(0), ir.Select(overflow, sum, ir.Imm32(0))});

    TestEnvironment env;
    Shader::Optimization::CollectShaderInfoPass(env, program);
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    return Shader::Backend::SPIRV::EmitSPIRV(profile, program);
}
} // namespace

TEST_CASE("Signed add overflow is tested against both boundaries", "[video_core][shader][iadd]") {
    // The case the single INT32_MAX bound missed: INT32_MAX - a is (INT32_MIN - a) - 1 in 32-bit
    // wraparound, so a negative side built from it tested one too low.
    REQUIRE(AddOverflows((std::numeric_limits<s32>::min)(), -1));
    REQUIRE(AddOverflowsAsEmitted((std::numeric_limits<s32>::min)(), -1));

    constexpr std::array<s32, 14> interesting{
        (std::numeric_limits<s32>::min)(),
        (std::numeric_limits<s32>::min)() + 1,
        (std::numeric_limits<s32>::min)() + 2,
        -2147482649,
        -1000,
        -2,
        -1,
        0,
        1,
        2,
        1000,
        (std::numeric_limits<s32>::max)() - 2,
        (std::numeric_limits<s32>::max)() - 1,
        (std::numeric_limits<s32>::max)(),
    };
    for (const s32 a : interesting) {
        for (const s32 b : interesting) {
            CAPTURE(a, b);
            REQUIRE(AddOverflowsAsEmitted(a, b) == AddOverflows(a, b));
        }
    }
    // And over every boundary neighbourhood, where an off-by-one hides.
    for (const s32 a : interesting) {
        const s64 lower = static_cast<s64>((std::numeric_limits<s32>::min)()) - a;
        const s64 upper = static_cast<s64>((std::numeric_limits<s32>::max)()) - a;
        for (const s64 bound : {lower, upper}) {
            for (const s64 delta : {-2, -1, 0, 1, 2}) {
                const s64 candidate = bound + delta;
                if (candidate < static_cast<s64>((std::numeric_limits<s32>::min)()) ||
                    candidate > static_cast<s64>((std::numeric_limits<s32>::max)())) {
                    continue;
                }
                const s32 b = static_cast<s32>(candidate);
                CAPTURE(a, b);
                REQUIRE(AddOverflowsAsEmitted(a, b) == AddOverflows(a, b));
            }
        }
    }
}

TEST_CASE("SPIR-V builds both signed add overflow bounds", "[video_core][spirv][iadd]") {
    const std::vector<u32> code = EmitOverflowingAdd();
    const std::vector<u32> constants = Constants(code);
    constexpr u32 s32_max{static_cast<u32>((std::numeric_limits<s32>::max)())};
    constexpr u32 s32_min{static_cast<u32>((std::numeric_limits<s32>::min)())};
    // The positive bound was always there; the negative one only exists once both are built.
    CHECK(Has(constants, s32_max));
    CHECK(Has(constants, s32_min));
}
