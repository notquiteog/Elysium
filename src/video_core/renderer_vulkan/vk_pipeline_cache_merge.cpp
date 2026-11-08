// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/pipeline_cache_files.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_merge.h"

namespace Vulkan {

VideoCommon::PipelineCacheMergeResult MergeVulkanPipelineCaches(
    const std::filesystem::path& output, std::span<const std::filesystem::path> inputs) {
    return VideoCommon::MergePipelineCaches(
        output, inputs, VideoCommon::VulkanPipelineCacheVersion,
        sizeof(ComputePipelineCacheKey), sizeof(GraphicsPipelineCacheKey));
}

} // namespace Vulkan
