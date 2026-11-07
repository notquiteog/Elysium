// SPDX-FileCopyrightText: Copyright 2026 Elysium Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace VideoCommon {

/// Emitted SPIR-V kept so that pipelines needing the same module do not translate it again.
///
/// Translating and emitting a program is about 87% of what a pipeline costs before the driver
/// compile, and one game's shader cache asks for the same module many times over: of the modules
/// emitted while loading Ocarina of Time's cache, 63% are byte-identical to one already emitted,
/// and a single one of them is emitted 569 times. See docs/shader-translation-cost.md.
///
/// Correctness here is by comparison, not by digest. The bucket is found with a cheap hash of the
/// shader and of what its environment answered, but a stored module is only handed back when its
/// whole Profile, RuntimeInfo and incoming Bindings compare equal to the ones asking. Those
/// comparisons are compiler generated (`= default`), so a field added to any of them is taken
/// into account automatically. A hand-written digest would instead keep matching, and quietly
/// return a module built for different state — which renders wrongly and crashes nothing.
class ShaderReuse {
public:
    /// Everything the emitted SPIR-V is a function of.
    struct Inputs {
        u64 shader_hash{};
        /// FileEnvironment::ContentHash(), or the equivalent for a live environment: the shader
        /// hash alone is not enough, because the environment's answers are folded into the result.
        u64 environment_hash{};
        const Shader::Profile* profile{};
        const Shader::HostTranslateInfo* host_info{};
        const Shader::RuntimeInfo* runtime_info{};
        Shader::Backend::Bindings bindings{};
    };

    struct Hit {
        /// Shared, so it stays alive while the caller uses it even if the cache grows meanwhile.
        std::shared_ptr<const std::vector<u32>> code;
        /// The bindings emission left behind, which the caller must carry to the next stage.
        Shader::Backend::Bindings bindings;
    };

    /// Modules are only kept while pipelines are being built, so this is a ceiling for that
    /// window rather than a steady cost; Clear() gives it all back. EDEN_NX2_SHADER_REUSE_MIB
    /// overrides it, and 0 turns the cache off.
    static constexpr size_t DEFAULT_LIMIT_MIB = 128;

    /// The limit in bytes, from EDEN_NX2_SHADER_REUSE_MIB or the default.
    [[nodiscard]] static size_t ConfiguredLimit();

    explicit ShaderReuse(size_t byte_limit = ConfiguredLimit()) : limit{byte_limit} {}

    [[nodiscard]] std::optional<Hit> Find(const Inputs& inputs);
    void Insert(const Inputs& inputs, std::vector<u32> code, Shader::Backend::Bindings after);

    /// Drops everything. The modules are only worth keeping while pipelines are being built.
    void Clear();

    [[nodiscard]] u64 Hits() const {
        return hits.load(std::memory_order_relaxed);
    }
    [[nodiscard]] u64 Misses() const {
        return misses.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t Bytes() const {
        return bytes.load(std::memory_order_relaxed);
    }

private:
    struct Entry {
        Shader::Profile profile;
        Shader::HostTranslateInfo host_info;
        Shader::RuntimeInfo runtime_info;
        Shader::Backend::Bindings bindings_in;
        Shader::Backend::Bindings bindings_out;
        std::shared_ptr<const std::vector<u32>> code;
    };

    [[nodiscard]] static u64 BucketOf(const Inputs& inputs);

    std::mutex mutex;
    ::Common::unordered_map<u64, std::vector<Entry>> buckets;
    std::atomic<size_t> bytes{};
    std::atomic<u64> hits{};
    std::atomic<u64> misses{};
    size_t limit;
};

} // namespace VideoCommon
