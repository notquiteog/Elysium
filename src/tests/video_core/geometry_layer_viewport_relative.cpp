// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"

namespace {
// Pass-through geometry program setting viewports 0 and 2 in its mask, the shape of Mario Tennis
// Fever's shadow cascade shader: MOV32I R0, 5; AST a[0x3a0] (ViewportMask), R0; EXIT.
constexpr std::array<u64, 3> code{0x010000000057f000ULL, 0xeff07f803a07ff00ULL,
                                  0xe30000000007000fULL};

class PassthroughEnvironment final : public Shader::Environment {
public:
    PassthroughEnvironment() {
        stage = Shader::Stage::Geometry;
        sph.common0.geometry_passthrough.Assign(1);
        sph.common2.threads_per_input_primitive.Assign(1);
        sph.common3.output_topology.Assign(Shader::OutputTopology::TriangleStrip);
        // Clear bits pass position and generic 0 through (bit set = not passed)
        gp_passthrough_mask.fill(~0U);
        gp_passthrough_mask[static_cast<size_t>(Shader::IR::Attribute::PositionX) / 32] = 0;
    }

    u64 ReadInstruction(u32 address) override {
        for (u32 i = 0; i < code.size(); ++i) {
            if (address == 0x50 + i * 8 + ((i + 1) / 3) * 8) return code[i];
        }
        FAIL("Read outside the pass-through program");
        return 0;
    }
    u32 ReadCbufValue(u32, u32) override { return 0; }
    Shader::TextureType ReadTextureType(u32) override { return Shader::TextureType::Color2D; }
    Shader::TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
    }
    bool IsTexturePixelFormatInteger(u32) override { return false; }
    u32 ReadViewportTransformState() override { return 1; }
    u32 TextureBoundBuffer() const override { return 0; }
    u32 LocalMemorySize() const override { return 0; }
    u32 SharedMemorySize() const override { return 0; }
    std::array<u32, 3> WorkgroupSize() const override { return {1, 1, 1}; }
    bool HasHLEMacroState() const override { return false; }
    std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

struct Lowered {
    u32 invocations;
    size_t layer_stores;
    size_t invocation_id_reads;
};

Lowered Lower(const Shader::HostTranslateInfo& host_info) {
    PassthroughEnvironment env;
    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_pool;
    Shader::Maxwell::Flow::CFG cfg{env, flow_pool, 0x50};
    const Shader::IR::Program program{
        Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host_info)};
    Lowered out{program.invocations, 0, 0};
    for (const Shader::IR::Block* const block : program.blocks) {
        for (const Shader::IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == Shader::IR::Opcode::SetAttribute &&
                inst.Arg(0).Attribute() == Shader::IR::Attribute::Layer) {
                ++out.layer_stores;
            }
            if (inst.GetOpcode() == Shader::IR::Opcode::InvocationId) {
                ++out.invocation_id_reads;
            }
        }
    }
    return out;
}
} // Anonymous namespace

TEST_CASE("Layer relative to the viewport draws a pass-through primitive once per viewport",
          "[video_core][geometry][passthrough]") {
    // Hosts without VK_NV_viewport_array2 and every host for such a program (StageHostInfo)
    Shader::HostTranslateInfo host_info;
    host_info.layer_viewport_relative = true;
    const Lowered relative{Lower(host_info)};
    REQUIRE(relative.invocations == 16);
    REQUIRE(relative.invocation_id_reads > 0);
    REQUIRE(relative.layer_stores == 3); // one per triangle vertex

    host_info.layer_viewport_relative = false;
    const Lowered absolute{Lower(host_info)};
    REQUIRE(absolute.invocations == 1);
    REQUIRE(absolute.invocation_id_reads == 0);
    REQUIRE(absolute.layer_stores == 0);
}
