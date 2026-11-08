// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <span>
#include <string>

#include "common/common_types.h"
#include "video_core/pipeline_cache_merge_result.h"

namespace Vulkan {

/// Merges Vulkan pipeline caches, supplying the key sizes this backend writes.
///
/// Kept apart from vk_pipeline_cache.h so a caller that only wants to merge files does not have
/// to pull in the renderer: that header reaches far enough to break a build that only wanted two
/// sizeof values.
[[nodiscard]] VideoCommon::PipelineCacheMergeResult MergeVulkanPipelineCaches(
    const std::filesystem::path& output, std::span<const std::filesystem::path> inputs);

/// What merging into a title's own cache did, and where that cache is.
struct MergeIntoTitleResult {
    VideoCommon::PipelineCacheMergeResult merge{};
    std::filesystem::path cache{};
    std::filesystem::path previous{};
    bool replaced{};
    std::string error{};
};

/// Merges `inputs` into this machine's cache for `program_id`, keeping what it already holds.
///
/// The union is built beside the cache and moved in only once whole, the previous file is kept,
/// and a union smaller than what is already there is refused: a union contains everything it was
/// built from, so a smaller one means something went wrong and the existing cache is the better
/// one to keep. Callers share this rather than repeating it, because it is the part that decides
/// whether a mistake costs someone their cache.
[[nodiscard]] MergeIntoTitleResult MergeIntoTitleCache(
    const std::filesystem::path& shader_dir, u64 program_id,
    std::span<const std::filesystem::path> inputs);

} // namespace Vulkan
