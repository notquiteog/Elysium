// SPDX-FileCopyrightText: Copyright 2026 Elysium Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS 5 neural rendering and DLSS frame generation (through NVIDIA Streamline) on the
// presented frame. NVIDIA's DLLs come from the user's PC; see vulkan_common/streamline.h.

#pragma once

#include <atomic>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class MemoryAllocator;
class Scheduler;
struct Frame;

/// The depth and motion vectors both features require. Games don't hand the presented frame's
/// over, so these are a flat depth and zero motion: the features fall back on their own analysis
/// of the frames.
class StreamlineSceneInputs {
public:
    explicit StreamlineSceneInputs(const Device& device, MemoryAllocator& memory_allocator);
    ~StreamlineSceneInputs();

    /// Recreates the images at `extent` when it changed (the old ones must be idle) and returns
    /// whether it did; RecordClear must then run before they are read.
    bool Resize(VkExtent2D new_extent);
    void RecordClear(vk::CommandBuffer cmdbuf) const;

    [[nodiscard]] VkImage Depth() const noexcept {
        return *depth;
    }
    [[nodiscard]] VkImageView DepthView() const noexcept {
        return *depth_view;
    }
    [[nodiscard]] VkImage MotionVectors() const noexcept {
        return *motion;
    }
    [[nodiscard]] VkImageView MotionVectorsView() const noexcept {
        return *motion_view;
    }
    [[nodiscard]] VkExtent2D Extent() const noexcept {
        return extent;
    }

    static constexpr VkFormat DEPTH_FORMAT = VK_FORMAT_R32_SFLOAT;
    static constexpr VkFormat MOTION_FORMAT = VK_FORMAT_R16G16_SFLOAT;
    static constexpr VkImageUsageFlags USAGE =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

private:
    const Device& device;
    MemoryAllocator& memory_allocator;
    VkExtent2D extent{};
    vk::Image depth;
    vk::ImageView depth_view;
    vk::Image motion;
    vk::ImageView motion_view;
};

/// DLSS 5 neural rendering (DLSS-NR) on each composited frame, before presentation.
class StreamlineNeuralRendering {
public:
    explicit StreamlineNeuralRendering(const Device& device, Scheduler& scheduler,
                                       MemoryAllocator& memory_allocator);
    ~StreamlineNeuralRendering();

    /// Whether DLSS-NR is turned on and can run.
    [[nodiscard]] static bool IsEnabled();

    /// Records DLSS-NR over the frame's image (GENERAL layout, left in GENERAL).
    void Apply(Frame* frame);

private:
    /// Everything slDLSSNRSetOptions takes from the settings, so a change of any of them is
    /// spotted while a game runs and handed over on the next frame.
    struct Options {
        u32 intensity{};
        u32 model{}; ///< An sl::DLSSMode value
        u32 preset{};
        u32 local_tone{};
        u32 local_structure{};
        u32 global_tone{};
        bool auto_mask{};

        bool operator==(const Options&) const = default;
    };

    void CreateImages(VkExtent2D extent);
    [[nodiscard]] static Options DesiredOptions();

    const Device& device;
    Scheduler& scheduler;
    MemoryAllocator& memory_allocator;
    StreamlineSceneInputs scene;
    void* set_options{}; ///< slDLSSNRSetOptions
    VkExtent2D extent{};
    vk::Image input;
    vk::ImageView input_view;
    vk::Image output;
    vk::ImageView output_view;
    u32 frame_index{};
    Options applied{};  ///< The options Streamline last took
    bool have_applied{};
    Options rejected{}; ///< Options Streamline turned down; tried again once they change
    bool have_rejected{};
    std::atomic<bool> options_rejected{};
    std::atomic<bool> failed{};
};

/// DLSS frame generation (DLSS-G): tags its inputs and sets the latency markers around each
/// present; Streamline adds the generated frames inside the hooked vkQueuePresentKHR.
class StreamlineFrameGen {
public:
    explicit StreamlineFrameGen(const Device& device, MemoryAllocator& memory_allocator);
    ~StreamlineFrameGen();

    /// Whether DLSS-G is turned on and can run.
    [[nodiscard]] static bool IsEnabled();

    /// Starts a frame: records its inputs into `cmdbuf`, which is submitted before the present.
    void BeginFrame(vk::CommandBuffer cmdbuf, VkExtent2D extent, VkFormat format,
                    u32 image_count);
    void BeforeSubmit();
    void BeforePresent();
    void AfterPresent();

private:
    void Marker(u32 marker);
    void ReportState();

    const Device& device;
    StreamlineSceneInputs scene;
    void* set_options{};  ///< slDLSSGSetOptions
    void* get_state{};    ///< slDLSSGGetState
    void* set_marker{};   ///< slPCLSetMarker
    void* frame{};        ///< This frame's sl::FrameToken
    u32 frame_index{};
    u32 frames_to_generate{};
    u32 max_frames_to_generate{1};
    VkExtent2D options_extent{};
    VkFormat options_format{};
    u32 options_images{};
    u32 last_status{~0U};
    u64 presented{};
    u64 frames_since_report{};
    bool reset{true};
    bool failed{};
};

} // namespace Vulkan
