// SPDX-FileCopyrightText: Copyright 2026 Elysium Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>

#include "video_core/shader_reuse.h"

namespace VideoCommon {
namespace {
constexpr u64 Mix(u64 value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdULL;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ULL;
    value ^= value >> 33;
    return value;
}
} // Anonymous namespace

size_t ShaderReuse::ConfiguredLimit() {
    if (const char* const value = std::getenv("EDEN_NX2_SHADER_REUSE_MIB")) {
        const long long mib = std::atoll(value);
        if (mib >= 0) {
            return static_cast<size_t>(mib) * 1024 * 1024;
        }
    }
    return DEFAULT_LIMIT_MIB * 1024 * 1024;
}

u64 ShaderReuse::BucketOf(const Inputs& inputs) {
    return Mix(inputs.shader_hash ^ Mix(inputs.environment_hash));
}

std::optional<ShaderReuse::Hit> ShaderReuse::Find(const Inputs& inputs) {
    if (!inputs.profile || !inputs.runtime_info || !inputs.host_info) {
        return std::nullopt;
    }
    std::scoped_lock lock{mutex};
    const auto bucket = buckets.find(BucketOf(inputs));
    if (bucket != buckets.end()) {
        for (const Entry& entry : bucket->second) {
            // Everything emission reads, compared rather than digested.
            if (entry.bindings_in == inputs.bindings && entry.runtime_info == *inputs.runtime_info &&
                entry.profile == *inputs.profile && entry.host_info == *inputs.host_info) {
                hits.fetch_add(1, std::memory_order_relaxed);
                return Hit{entry.code, entry.bindings_out};
            }
        }
    }
    misses.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
}

void ShaderReuse::Insert(const Inputs& inputs, std::vector<u32> code,
                         Shader::Backend::Bindings after) {
    if (!inputs.profile || !inputs.runtime_info || !inputs.host_info || code.empty()) {
        return;
    }
    const size_t size = code.size() * sizeof(u32);
    if (bytes.load(std::memory_order_relaxed) + size > limit) {
        return;
    }
    auto shared = std::make_shared<const std::vector<u32>>(std::move(code));
    std::scoped_lock lock{mutex};
    std::vector<Entry>& bucket = buckets[BucketOf(inputs)];
    for (const Entry& entry : bucket) {
        if (entry.bindings_in == inputs.bindings && entry.runtime_info == *inputs.runtime_info &&
            entry.profile == *inputs.profile && entry.host_info == *inputs.host_info) {
            return; // Another thread emitted the same module first.
        }
    }
    bucket.push_back(Entry{*inputs.profile, *inputs.host_info, *inputs.runtime_info,
                           inputs.bindings, after, std::move(shared)});
    bytes.fetch_add(size, std::memory_order_relaxed);
}

void ShaderReuse::Clear() {
    std::scoped_lock lock{mutex};
    buckets.clear();
    bytes.store(0, std::memory_order_relaxed);
}

} // namespace VideoCommon
