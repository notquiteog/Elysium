// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <filesystem>
#include <span>

#include "video_core/shader_environment.h"

namespace Vulkan {

/// Merges Vulkan pipeline caches, supplying the key sizes this backend writes.
///
/// Kept apart from vk_pipeline_cache.h so a caller that only wants to merge files does not have
/// to pull in the renderer: that header reaches far enough to break a build that only wanted two
/// sizeof values.
[[nodiscard]] VideoCommon::PipelineCacheMergeResult MergeVulkanPipelineCaches(
    const std::filesystem::path& output, std::span<const std::filesystem::path> inputs);

} // namespace Vulkan
