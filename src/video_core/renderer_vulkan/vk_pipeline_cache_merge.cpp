// SPDX-License-Identifier: GPL-3.0-or-later

#include <vector>
#include <fmt/format.h>
#include "video_core/pipeline_cache_files.h"
#include "video_core/shader_environment.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache_merge.h"

namespace Vulkan {

VideoCommon::PipelineCacheMergeResult MergeVulkanPipelineCaches(
    const std::filesystem::path& output, std::span<const std::filesystem::path> inputs) {
    return VideoCommon::MergePipelineCaches(
        output, inputs, VideoCommon::VulkanPipelineCacheVersion,
        sizeof(ComputePipelineCacheKey), sizeof(GraphicsPipelineCacheKey));
}


MergeIntoTitleResult MergeIntoTitleCache(const std::filesystem::path& shader_dir, u64 program_id,
                                         std::span<const std::filesystem::path> inputs) {
    MergeIntoTitleResult result{};
    const auto base_dir = shader_dir / fmt::format("{:016x}", program_id);
    result.cache = VideoCommon::VersionedPipelineCachePath(
        base_dir, "vulkan", VideoCommon::VulkanPipelineCacheVersion);

    std::error_code ec;
    std::filesystem::create_directories(base_dir, ec);

    std::vector<std::filesystem::path> all;
    const bool had_cache = std::filesystem::exists(result.cache);
    if (had_cache) {
        all.emplace_back(result.cache);
    }
    all.insert(all.end(), inputs.begin(), inputs.end());

    const auto staged = result.cache.string() + ".merging";
    result.merge = MergeVulkanPipelineCaches(staged, all);
    if (result.merge.inputs_read == 0) {
        std::filesystem::remove(staged, ec);
        result.error = "no usable cache files to merge";
        return result;
    }
    if (had_cache &&
        std::filesystem::file_size(staged, ec) < std::filesystem::file_size(result.cache, ec)) {
        std::filesystem::remove(staged, ec);
        result.error = "the merged cache came out smaller than the one already here";
        return result;
    }
    if (had_cache) {
        result.previous = result.cache.string() + ".before-merge";
        std::filesystem::rename(result.cache, result.previous, ec);
    }
    std::filesystem::rename(staged, result.cache, ec);
    if (ec) {
        result.error = ec.message();
        return result;
    }
    result.replaced = true;
    return result;
}

} // namespace Vulkan
