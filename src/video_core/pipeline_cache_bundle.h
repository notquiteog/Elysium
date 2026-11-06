// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "common/common_types.h"

namespace VideoCommon {

/// File extension of a shareable pipeline cache bundle.
inline constexpr std::string_view PipelineCacheBundleExtension = ".elysium-shaders";

struct PipelineCacheBundleResult {
    bool ok{};
    u64 title_id{};
    size_t files{};   ///< Files written (export) or installed (import).
    size_t skipped{}; ///< Import: files kept because the local copy is as large or incompatible.
    std::string error;
};

/// Packs a title's guest pipeline caches (Vulkan, its used list and OpenGL, all compressed) into
/// one file another PC can import. The driver's own pipeline cache is left out: it only fits the
/// GPU and driver that wrote it, and the pipelines are rebuilt from the guest programs anyway.
PipelineCacheBundleResult ExportPipelineCacheBundle(const std::filesystem::path& shader_dir,
                                                    u64 title_id,
                                                    const std::filesystem::path& bundle);

/// Installs a bundle into <shader_dir>/<title id>. A cache from a compatible older version is
/// installed as the current version; one from a newer build keeps its name for that build. A
/// local cache is replaced only by a larger one. With a nonzero `expected_title_id`, a bundle of
/// another title is refused.
PipelineCacheBundleResult ImportPipelineCacheBundle(const std::filesystem::path& shader_dir,
                                                    const std::filesystem::path& bundle,
                                                    u64 expected_title_id);

} // namespace VideoCommon
