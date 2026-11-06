// SPDX-FileCopyrightText: Copyright 2026 Elysium Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS 5 neural rendering and DLSS frame generation (through NVIDIA Streamline) on the
// presented frame.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>

#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_vulkan/vk_present_manager.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_streamline_present.h"
#include "video_core/vulkan_common/streamline.h"
#include "video_core/vulkan_common/streamline_api.h"
#include "video_core/vulkan_common/streamline_dlss_nr.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan {
namespace {

// Streamline's calls are not thread safe; neural rendering records on the render thread and frame
// generation on the present thread.
std::mutex streamline_mutex;

// Each feature keeps its own frame numbering, so each gets its own viewport (DLSS super
// resolution uses 0).
constexpr u32 NEURAL_VIEWPORT = 1;
constexpr u32 FRAME_GEN_VIEWPORT = 2;

constexpr VkFormat NEURAL_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageUsageFlags NEURAL_USAGE = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                           VK_IMAGE_USAGE_TRANSFER_DST_BIT;

constexpr VkImageSubresourceRange COLOR_RANGE{
    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
    .baseMipLevel = 0,
    .levelCount = 1,
    .baseArrayLayer = 0,
    .layerCount = 1,
};

// The flat depth the features see, in [0, 1] with 1 far. EDEN_NX2_DLSS_SCENE_DEPTH overrides it.
f32 SceneDepth() {
    static const f32 depth = [] {
        const char* const value = std::getenv("EDEN_NX2_DLSS_SCENE_DEPTH");
        return value ? std::clamp(static_cast<f32>(std::atof(value)), 0.0f, 1.0f) : 0.5f;
    }();
    return depth;
}

bool SameExtent(VkExtent2D a, VkExtent2D b) {
    return a.width == b.width && a.height == b.height;
}

vk::Image CreateImage(MemoryAllocator& allocator, VkFormat format, VkExtent2D extent,
                      VkImageUsageFlags usage) {
    return allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
}

vk::ImageView CreateView(const Device& device, VkImage image, VkFormat format) {
    return device.GetLogical().CreateImageView(VkImageViewCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components{},
        .subresourceRange = COLOR_RANGE,
    });
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkAccessFlags src_access,
                                  VkAccessFlags dst_access, VkImageLayout old_layout,
                                  VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = COLOR_RANGE,
    };
}

VkImageBlit FullBlit(VkExtent2D extent) {
    const VkOffset3D end{static_cast<s32>(extent.width), static_cast<s32>(extent.height), 1};
    const VkImageSubresourceLayers layers{
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    return VkImageBlit{
        .srcSubresource = layers,
        .srcOffsets = {VkOffset3D{0, 0, 0}, end},
        .dstSubresource = layers,
        .dstOffsets = {VkOffset3D{0, 0, 0}, end},
    };
}

sl::Resource Resource(VkImage image, VkImageView view, VkFormat format, VkImageLayout layout,
                      VkExtent2D size, VkImageUsageFlags usage) {
    sl::Resource resource{sl::ResourceType::eTex2d, image, nullptr, view,
                          static_cast<uint32_t>(layout)};
    resource.width = size.width;
    resource.height = size.height;
    resource.nativeFormat = static_cast<uint32_t>(format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = usage;
    return resource;
}

sl::float4x4 Identity() {
    sl::float4x4 matrix;
    matrix.setRow(0, {1.0f, 0.0f, 0.0f, 0.0f});
    matrix.setRow(1, {0.0f, 1.0f, 0.0f, 0.0f});
    matrix.setRow(2, {0.0f, 0.0f, 1.0f, 0.0f});
    matrix.setRow(3, {0.0f, 0.0f, 0.0f, 1.0f});
    return matrix;
}

// No camera is known for an emulated game's frame: an identity camera, no jitter and the zero
// motion of StreamlineSceneInputs.
sl::Constants SceneConstants(VkExtent2D extent, bool reset) {
    const f32 width = static_cast<f32>(extent.width);
    const f32 height = static_cast<f32>(extent.height);
    sl::Constants constants{};
    constants.cameraViewToClip = Identity();
    constants.clipToCameraView = Identity();
    constants.clipToLensClip = Identity();
    constants.clipToPrevClip = Identity();
    constants.prevClipToClip = Identity();
    constants.jitterOffset = {0.0f, 0.0f};
    constants.mvecScale = {1.0f / width, 1.0f / height};
    constants.cameraPinholeOffset = {0.0f, 0.0f};
    constants.cameraPos = {0.0f, 0.0f, 0.0f};
    constants.cameraUp = {0.0f, 1.0f, 0.0f};
    constants.cameraRight = {1.0f, 0.0f, 0.0f};
    constants.cameraFwd = {0.0f, 0.0f, 1.0f};
    constants.cameraNear = 0.1f;
    constants.cameraFar = 1000.0f;
    constants.cameraFOV = 1.0f;
    constants.cameraAspectRatio = width / height;
    constants.depthInverted = sl::eFalse;
    constants.cameraMotionIncluded = sl::eTrue;
    constants.motionVectors3D = sl::eFalse;
    constants.reset = reset ? sl::eTrue : sl::eFalse;
    constants.motionVectorsJittered = sl::eFalse;
    return constants;
}

} // Anonymous namespace

StreamlineSceneInputs::StreamlineSceneInputs(const Device& device_,
                                             MemoryAllocator& memory_allocator_)
    : device{device_}, memory_allocator{memory_allocator_} {}

StreamlineSceneInputs::~StreamlineSceneInputs() = default;

bool StreamlineSceneInputs::Resize(VkExtent2D new_extent) {
    if (depth && SameExtent(extent, new_extent)) {
        return false;
    }
    extent = new_extent;
    depth = CreateImage(memory_allocator, DEPTH_FORMAT, extent, USAGE);
    depth_view = CreateView(device, *depth, DEPTH_FORMAT);
    motion = CreateImage(memory_allocator, MOTION_FORMAT, extent, USAGE);
    motion_view = CreateView(device, *motion, MOTION_FORMAT);
    return true;
}

void StreamlineSceneInputs::RecordClear(vk::CommandBuffer cmdbuf) const {
    const std::array to_clear{
        ImageBarrier(*depth, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        ImageBarrier(*motion, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                           {}, {}, to_clear);
    VkClearColorValue depth_value{};
    depth_value.float32[0] = SceneDepth();
    cmdbuf.ClearColorImage(*depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, depth_value,
                           COLOR_RANGE);
    cmdbuf.ClearColorImage(*motion, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VkClearColorValue{},
                           COLOR_RANGE);
    const std::array to_read{
        ImageBarrier(*depth, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        ImageBarrier(*motion, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                           {}, {}, to_read);
}

StreamlineNeuralRendering::StreamlineNeuralRendering(const Device& device_, Scheduler& scheduler_,
                                                     MemoryAllocator& memory_allocator_)
    : device{device_}, scheduler{scheduler_}, memory_allocator{memory_allocator_},
      scene{device_, memory_allocator_} {
    if (!Streamline::IsNeuralRenderingAvailable()) {
        failed = true;
        return;
    }
    set_options = reinterpret_cast<void*>(Streamline::FeatureFunction<Streamline::PFun_slDLSSNRSetOptions>(
        sl::kFeatureDLSS_NR, "slDLSSNRSetOptions"));
    if (!set_options) {
        LOG_ERROR(Render_Vulkan, "Streamline's DLSS 5 neural rendering has no slDLSSNRSetOptions");
        failed = true;
    }
}

StreamlineNeuralRendering::~StreamlineNeuralRendering() {
    if (!output) {
        return;
    }
    void(device.GetLogical().WaitIdle());
    if (Streamline::IsNeuralRenderingAvailable()) {
        std::scoped_lock lock{streamline_mutex};
        Streamline::GetApi().free_resources(sl::kFeatureDLSS_NR,
                                            sl::ViewportHandle{NEURAL_VIEWPORT});
    }
}

bool StreamlineNeuralRendering::IsEnabled() {
    return Settings::values.nvidia_dlss_nr.GetValue() && Streamline::IsNeuralRenderingAvailable();
}

void StreamlineNeuralRendering::CreateImages(VkExtent2D new_extent) {
    if (output) {
        scheduler.Finish(); // the old images may still be in use
    }
    extent = new_extent;
    input = CreateImage(memory_allocator, NEURAL_FORMAT, extent, NEURAL_USAGE);
    input_view = CreateView(device, *input, NEURAL_FORMAT);
    output = CreateImage(memory_allocator, NEURAL_FORMAT, extent, NEURAL_USAGE);
    output_view = CreateView(device, *output, NEURAL_FORMAT);
    scene.Resize(extent);
}

void StreamlineNeuralRendering::Apply(Frame* frame) {
    if (failed || !IsEnabled()) {
        return;
    }
    const VkExtent2D frame_extent{frame->width, frame->height};
    const bool recreated = !output || !SameExtent(extent, frame_extent);
    if (recreated) {
        CreateImages(frame_extent);
        LOG_INFO(Render_Vulkan, "NVIDIA DLSS 5 neural rendering on {}x{} frames",
                 frame_extent.width, frame_extent.height);
    }
    const u32 intensity = Settings::values.nvidia_dlss_nr_intensity.GetValue();
    const bool options_changed = recreated || intensity != applied_intensity;
    applied_intensity = intensity;

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, recreated, options_changed, intensity, index = frame_index++,
                      frame_image = *frame->image, size = frame_extent, input_image = *input,
                      input_handle = *input_view, output_image = *output,
                      output_handle = *output_view](vk::CommandBuffer cmdbuf) {
        if (recreated) {
            scene.RecordClear(cmdbuf);
        }
        constexpr VkAccessFlags FRAME_WRITES = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                               VK_ACCESS_SHADER_WRITE_BIT |
                                               VK_ACCESS_TRANSFER_WRITE_BIT;
        const std::array to_copy{
            ImageBarrier(frame_image, FRAME_WRITES, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            ImageBarrier(input_image, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, {}, {}, to_copy);
        cmdbuf.BlitImage(frame_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, input_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, FullBlit(size), VK_FILTER_NEAREST);
        const std::array to_evaluate{
            ImageBarrier(input_image, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            ImageBarrier(output_image, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_GENERAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, {}, {}, to_evaluate);

        const Streamline::Api& api = Streamline::GetApi();
        const sl::ViewportHandle viewport{NEURAL_VIEWPORT};
        sl::Result result = sl::Result::eOk;
        const char* stage = "slDLSSNRSetOptions";
        {
            std::scoped_lock lock{streamline_mutex};
            if (options_changed) {
                Streamline::DlssNrOptions options;
                options.mode = Streamline::DlssNrMode::eOn;
                options.intensity = static_cast<f32>(intensity) / 100.0f;
                const auto set = reinterpret_cast<Streamline::PFun_slDLSSNRSetOptions*>(set_options);
                result = set(viewport, options);
            }
            sl::FrameToken* token{};
            if (result == sl::Result::eOk) {
                stage = "slGetNewFrameToken";
                result = api.get_new_frame_token(token, &index);
            }
            if (result == sl::Result::eOk) {
                stage = "slSetConstants";
                result = api.set_constants(SceneConstants(size, recreated), *token, viewport);
            }
            sl::Resource color_in = Resource(input_image, input_handle, NEURAL_FORMAT,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, size,
                                             NEURAL_USAGE);
            sl::Resource color_out = Resource(output_image, output_handle, NEURAL_FORMAT,
                                              VK_IMAGE_LAYOUT_GENERAL, size, NEURAL_USAGE);
            sl::Resource depth = Resource(scene.Depth(), scene.DepthView(),
                                          StreamlineSceneInputs::DEPTH_FORMAT,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, size,
                                          StreamlineSceneInputs::USAGE);
            sl::Resource motion = Resource(scene.MotionVectors(), scene.MotionVectorsView(),
                                           StreamlineSceneInputs::MOTION_FORMAT,
                                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, size,
                                           StreamlineSceneInputs::USAGE);
            const sl::Extent area{0, 0, size.width, size.height};
            const std::array tags{
                sl::ResourceTag{&color_in, sl::kBufferTypeUpliftInputColor,
                                sl::ResourceLifecycle::eValidUntilEvaluate, &area},
                sl::ResourceTag{&color_out, sl::kBufferTypeUpliftOutputColor,
                                sl::ResourceLifecycle::eValidUntilEvaluate, &area},
                sl::ResourceTag{&depth, sl::kBufferTypeDepth,
                                sl::ResourceLifecycle::eValidUntilEvaluate, &area},
                sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors,
                                sl::ResourceLifecycle::eValidUntilEvaluate, &area},
            };
            const auto command_buffer = reinterpret_cast<sl::CommandBuffer*>(*cmdbuf);
            if (result == sl::Result::eOk) {
                stage = "slSetTagForFrame";
                result = api.set_tag_for_frame(*token, viewport, tags.data(),
                                               static_cast<uint32_t>(tags.size()),
                                               command_buffer);
            }
            if (result == sl::Result::eOk) {
                stage = "slEvaluateFeature";
                const sl::BaseStructure* inputs[] = {&viewport};
                result = api.evaluate_feature(sl::kFeatureDLSS_NR, *token, inputs, 1,
                                              command_buffer);
                if (result == sl::Result::eWarnOutOfVRAM) {
                    result = sl::Result::eOk;
                }
            }
        }
        if (result != sl::Result::eOk) {
            LOG_ERROR(Render_Vulkan,
                      "NVIDIA DLSS 5 neural rendering failed at {} (Streamline result {}); "
                      "turning it off",
                      stage, static_cast<int>(result));
            failed = true;
            cmdbuf.PipelineBarrier(
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                ImageBarrier(frame_image, VK_ACCESS_TRANSFER_READ_BIT,
                             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL));
            return;
        }
        const std::array to_copy_back{
            ImageBarrier(output_image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
            ImageBarrier(frame_image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, {}, {}, to_copy_back);
        cmdbuf.BlitImage(output_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, frame_image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, FullBlit(size), VK_FILTER_NEAREST);
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
            ImageBarrier(frame_image, VK_ACCESS_TRANSFER_WRITE_BIT,
                         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                             VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL));
    });
    // DLSS-NR binds its own pipelines and descriptors on the command buffer.
    scheduler.InvalidateState();
}

StreamlineFrameGen::StreamlineFrameGen(const Device& device_, MemoryAllocator& memory_allocator_)
    : device{device_}, scene{device_, memory_allocator_} {
    if (!Streamline::IsFrameGenAvailable()) {
        failed = true;
        return;
    }
    set_options = reinterpret_cast<void*>(
        Streamline::FeatureFunction<PFun_slDLSSGSetOptions>(sl::kFeatureDLSS_G, "slDLSSGSetOptions"));
    get_state = reinterpret_cast<void*>(
        Streamline::FeatureFunction<PFun_slDLSSGGetState>(sl::kFeatureDLSS_G, "slDLSSGGetState"));
    set_marker = reinterpret_cast<void*>(
        Streamline::FeatureFunction<PFun_slPCLSetMarker>(sl::kFeaturePCL, "slPCLSetMarker"));
    const auto reflex_options = Streamline::FeatureFunction<PFun_slReflexSetOptions>(
        sl::kFeatureReflex, "slReflexSetOptions");
    if (!set_options || !get_state || !set_marker || !reflex_options) {
        LOG_ERROR(Render_Vulkan, "Streamline's DLSS frame generation is missing functions");
        failed = true;
        return;
    }
    // DLSS-G requires Reflex.
    sl::ReflexOptions options{};
    options.mode = sl::ReflexMode::eLowLatency;
    if (const sl::Result result = reflex_options(options); result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan, "NVIDIA Reflex did not start (Streamline result {})",
                  static_cast<int>(result));
        failed = true;
    }
}

StreamlineFrameGen::~StreamlineFrameGen() {
    if (failed || !set_options) {
        return;
    }
    std::scoped_lock lock{streamline_mutex};
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOff;
    reinterpret_cast<PFun_slDLSSGSetOptions*>(set_options)(sl::ViewportHandle{FRAME_GEN_VIEWPORT},
                                                            options);
}

bool StreamlineFrameGen::IsEnabled() {
    return Settings::values.frame_gen.GetValue() &&
           Settings::values.frame_gen_type.GetValue() == Settings::FrameGenType::Dlss &&
           Streamline::IsFrameGenAvailable();
}

void StreamlineFrameGen::Marker(u32 marker) {
    if (!frame) {
        return;
    }
    reinterpret_cast<PFun_slPCLSetMarker*>(set_marker)(static_cast<sl::PCLMarker>(marker),
                                                       *static_cast<sl::FrameToken*>(frame));
}

void StreamlineFrameGen::BeginFrame(vk::CommandBuffer cmdbuf, VkExtent2D extent, VkFormat format,
                                    u32 image_count) {
    frame = nullptr;
    if (failed || !IsEnabled()) {
        return;
    }
    if (!SameExtent(scene.Extent(), extent)) {
        if (scene.Depth()) {
            void(device.GetLogical().WaitIdle()); // the old inputs may still be in use
        }
        scene.Resize(extent);
        scene.RecordClear(cmdbuf);
        reset = true;
        LOG_INFO(Render_Vulkan, "NVIDIA DLSS frame generation on {}x{}", extent.width,
                 extent.height);
    }
    const u32 multiplier = Settings::values.frame_gen_multiplier.GetValue();
    const u32 wanted = std::clamp(multiplier, 2U, 4U) - 1;
    const u32 generate = (std::min)(wanted, max_frames_to_generate);

    std::scoped_lock lock{streamline_mutex};
    const Streamline::Api& api = Streamline::GetApi();
    const sl::ViewportHandle viewport{FRAME_GEN_VIEWPORT};
    sl::FrameToken* token{};
    const u32 index = ++frame_index;
    sl::Result result = api.get_new_frame_token(token, &index);
    const char* stage = "slGetNewFrameToken";
    if (result == sl::Result::eOk) {
        frame = token;
        // The guest's frame is already rendered by the time it reaches the present thread.
        Marker(static_cast<u32>(sl::PCLMarker::eSimulationStart));
        Marker(static_cast<u32>(sl::PCLMarker::eSimulationEnd));
        Marker(static_cast<u32>(sl::PCLMarker::eRenderSubmitStart));
        stage = "slSetConstants";
        result = api.set_constants(SceneConstants(extent, reset), *token, viewport);
    }
    if (result == sl::Result::eOk &&
        (generate != frames_to_generate || !SameExtent(options_extent, extent) ||
         options_format != format || options_images != image_count)) {
        sl::DLSSGOptions options{};
        options.mode = sl::DLSSGMode::eOn;
        options.numFramesToGenerate = generate;
        options.numBackBuffers = image_count;
        options.mvecDepthWidth = extent.width;
        options.mvecDepthHeight = extent.height;
        options.colorWidth = extent.width;
        options.colorHeight = extent.height;
        options.colorBufferFormat = static_cast<uint32_t>(format);
        options.mvecBufferFormat = static_cast<uint32_t>(StreamlineSceneInputs::MOTION_FORMAT);
        options.depthBufferFormat = static_cast<uint32_t>(StreamlineSceneInputs::DEPTH_FORMAT);
        stage = "slDLSSGSetOptions";
        result = reinterpret_cast<PFun_slDLSSGSetOptions*>(set_options)(viewport, options);
        if (result == sl::Result::eOk) {
            LOG_INFO(Render_Vulkan, "NVIDIA DLSS frame generation: {}x ({} generated per frame)",
                     generate + 1, generate);
            frames_to_generate = generate;
            options_extent = extent;
            options_format = format;
            options_images = image_count;
        }
    }
    if (result == sl::Result::eOk) {
        sl::Resource depth = Resource(scene.Depth(), scene.DepthView(),
                                      StreamlineSceneInputs::DEPTH_FORMAT,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, extent,
                                      StreamlineSceneInputs::USAGE);
        sl::Resource motion = Resource(scene.MotionVectors(), scene.MotionVectorsView(),
                                       StreamlineSceneInputs::MOTION_FORMAT,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, extent,
                                       StreamlineSceneInputs::USAGE);
        const sl::Extent area{0, 0, extent.width, extent.height};
        const std::array tags{
            sl::ResourceTag{&depth, sl::kBufferTypeDepth,
                            sl::ResourceLifecycle::eValidUntilPresent, &area},
            sl::ResourceTag{&motion, sl::kBufferTypeMotionVectors,
                            sl::ResourceLifecycle::eValidUntilPresent, &area},
        };
        stage = "slSetTagForFrame";
        result = api.set_tag_for_frame(*token, viewport, tags.data(),
                                       static_cast<uint32_t>(tags.size()),
                                       reinterpret_cast<sl::CommandBuffer*>(*cmdbuf));
    }
    reset = false;
    if (result != sl::Result::eOk) {
        LOG_ERROR(Render_Vulkan,
                  "NVIDIA DLSS frame generation failed at {} (Streamline result {}); turning it "
                  "off",
                  stage, static_cast<int>(result));
        failed = true;
        frame = nullptr;
    }
}

void StreamlineFrameGen::BeforeSubmit() {
    std::scoped_lock lock{streamline_mutex};
    Marker(static_cast<u32>(sl::PCLMarker::eRenderSubmitEnd));
}

void StreamlineFrameGen::BeforePresent() {
    std::scoped_lock lock{streamline_mutex};
    Marker(static_cast<u32>(sl::PCLMarker::ePresentStart));
}

void StreamlineFrameGen::AfterPresent() {
    if (!frame) {
        return;
    }
    {
        std::scoped_lock lock{streamline_mutex};
        Marker(static_cast<u32>(sl::PCLMarker::ePresentEnd));
    }
    ++presented;
    // The first frames tell how many frames this GPU can generate; then a report every ~10 s.
    if (presented == 3 || ++frames_since_report >= 600) {
        ReportState();
    }
}

void StreamlineFrameGen::ReportState() {
    std::scoped_lock lock{streamline_mutex};
    sl::DLSSGState state{};
    if (reinterpret_cast<PFun_slDLSSGGetState*>(get_state)(sl::ViewportHandle{FRAME_GEN_VIEWPORT},
                                                            state, nullptr) != sl::Result::eOk) {
        return;
    }
    max_frames_to_generate = (std::max)(1U, state.numFramesToGenerateMax);
    const u32 status = static_cast<u32>(state.status);
    if (status != last_status) {
        last_status = status;
        if (state.status == sl::DLSSGStatus::eOk) {
            LOG_INFO(Render_Vulkan, "NVIDIA DLSS frame generation is running (up to {}x)",
                     max_frames_to_generate + 1);
        } else {
            LOG_WARNING(Render_Vulkan, "NVIDIA DLSS frame generation status 0x{:x}", status);
        }
    }
    if (frames_since_report >= 600) {
        LOG_INFO(Render_Vulkan, "NVIDIA DLSS frame generation: {} frames presented for {} rendered",
                 state.numFramesActuallyPresented, frames_since_report);
        frames_since_report = 0;
    }
}

} // namespace Vulkan
