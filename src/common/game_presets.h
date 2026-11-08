// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/settings.h"

namespace Settings {

inline constexpr u64 MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
inline constexpr u64 DonkeyKongBananzaProgramId = 0x040074A01BF12000ULL;
inline constexpr u64 HyruleWarriorsAgeOfImprisonmentProgramId = 0x04003F40232DE000ULL;
inline constexpr u64 MarioTennisFeverProgramId = 0x0400D4201C6EA000ULL;
inline constexpr u64 OcarinaOfTimeProgramId = 0x0400486024DEC000ULL;
inline constexpr u64 KirbyAirRidersProgramId = 0x040077201BE50000ULL;

namespace Detail {
inline auto SessionSetter(bool global) {
    return [global](auto& setting, auto value) {
        setting.SetGlobal(global);
        setting.SetValue(value);
    };
}

/// What every Switch 2 preset shares: 1x keeps the comparison with the console fair, the rest
/// trades nothing visible for speed.
inline void SetSwitch2BaseSettings(Values& settings, bool global) {
    const auto session = SessionSetter(global);
    session(settings.renderer_backend, RendererBackend::Vulkan);
    session(settings.resolution_setup, ResolutionSetup::Res1X);
    session(settings.gpu_accuracy, GpuAccuracy::Low);
    session(settings.use_asynchronous_shaders, true);
    session(settings.astc_recompression, AstcRecompression::Bc3);
    session(settings.vram_usage_mode, VramUsageMode::Conservative);
    session(settings.use_reactive_flushing, false);
    session(settings.vsync_mode, VSyncMode::FifoRelaxed);
    session(settings.scaling_filter, ScalingFilter::ScaleForce);
    session(settings.anti_aliasing, AntiAliasing::Fxaa);
}

/// Reaches 60 by generating the frames the game does not produce itself. The pacer defers
/// generation while the game is already above the target (frame_gen_pacer.cpp), so a title that
/// holds 60 on its own is not paying for frames it does not need.
///
/// FSR 3 rather than DLSS because it runs on every card and platform; DLSS frame generation
/// needs NVIDIA's Streamline, which is Windows only. The type can be switched while a game runs,
/// so an RTX card can move to it without restarting.
inline void SetFrameGenerationTo60(Values& settings, bool global) {
    const auto session = SessionSetter(global);
    session(settings.frame_gen, true);
    session(settings.frame_gen_type, FrameGenType::Fsr3);
    session(settings.frame_gen_multiplier, u32{2});
    session(settings.frame_gen_target_rate, u32{60});
}
} // namespace Detail

inline void SetMarioKartWorldRecommendedSettings(Values& settings, bool global) {
    Detail::SetSwitch2BaseSettings(settings, global);
    Detail::SessionSetter(global)(settings.use_docked_mode, ConsoleMode::Handheld);
    Detail::SetFrameGenerationTo60(settings, global);
}

inline void SetOcarinaOfTimeRecommendedSettings(Values& settings, bool global) {
    Detail::SetSwitch2BaseSettings(settings, global);
    const auto session = Detail::SessionSetter(global);
    // Balanced fences stop the texture and object flickering, as they do for the two other
    // titles here that showed it.
    session(settings.gpu_fence_behavior, GpuFenceBehavior::Balanced);
    // Its own frame-rate controller gives ground rather than holding 60, and the built-in
    // Dynamic FPS patch lets it fall as far as 10, so the generator makes up the difference
    // instead of the game being left short of the display.
    Detail::SetFrameGenerationTo60(settings, global);
}

inline void SetKirbyAirRidersRecommendedSettings(Values& settings, bool global) {
    Detail::SetSwitch2BaseSettings(settings, global);
    const auto session = Detail::SessionSetter(global);
    // It streams in compute pipelines as stages load: one session here built 706 of them on the
    // GPU thread, averaging 220 ms and peaking at 1074 ms, which held the frame rate at 7 fps
    // until they were done. The base profile's asynchronous shaders are what stop those builds
    // blocking the thread that presents.
    //
    // Balanced fences for the texture and object flickering, as they do for the three other
    // titles here that showed it.
    session(settings.gpu_fence_behavior, GpuFenceBehavior::Balanced);
    // No frame generation: it runs 35-50 fps here rather than near 60, and generating from a
    // base that unstable both spends GPU time the game needs and gives FSR 3 its worst case to
    // interpolate from. The titles that get it hold a rate close enough to 60 to fill in.
}

inline void SetDonkeyKongBananzaRecommendedSettings(Values& settings, bool global) {
    Detail::SetSwitch2BaseSettings(settings, global);
    const auto session = Detail::SessionSetter(global);
    // The game picks its resolution and swap interval from GPU timestamps (game_settings.cpp).
    session(settings.gpu_clock, GpuClock::Boost);
    session(settings.fps30_patch, true);
}

inline void SetHyruleWarriorsRecommendedSettings(Values& settings, bool global) {
    Detail::SetSwitch2BaseSettings(settings, global);
    const auto session = Detail::SessionSetter(global);
    // A faster CPU clock speeds the game itself up (game_settings.cpp).
    session(settings.cpu_clock, CpuClock::Normal);
    session(settings.gpu_fence_behavior, GpuFenceBehavior::Balanced);
}

inline void SetMarioTennisFeverRecommendedSettings(Values& settings, bool global) {
    Detail::SetSwitch2BaseSettings(settings, global);
    const auto session = Detail::SessionSetter(global);
    // Balanced fences keep the characters visible; presenting from its own thread keeps 60 fps.
    session(settings.gpu_fence_behavior, GpuFenceBehavior::Balanced);
    session(settings.async_presentation, true);
}

struct GamePreset {
    u64 program_id;
    const char* name;
    /// The settings in the boot prompt's words.
    const char* summary;
    void (*apply)(Values& settings, bool global);
};

inline constexpr GamePreset GamePresets[] = {
    {MarioKartWorldProgramId, "Mario Kart World",
     "Vulkan, 1x resolution, GPU Fast, asynchronous shaders, BC3, conservative VRAM, reactive "
     "flushing off, FIFO Relaxed VSync, handheld mode, ScaleForce, FXAA, and frame generation "
     "to 60.",
     SetMarioKartWorldRecommendedSettings},
    {DonkeyKongBananzaProgramId, "Donkey Kong Bananza",
     "Vulkan, 1x resolution, GPU Fast, asynchronous shaders, BC3, conservative VRAM, reactive "
     "flushing off, FIFO Relaxed VSync, ScaleForce, FXAA, the Boost GPU clock and the 30 FPS "
     "lock.",
     SetDonkeyKongBananzaRecommendedSettings},
    {HyruleWarriorsAgeOfImprisonmentProgramId, "Hyrule Warriors: Age of Imprisonment",
     "Vulkan, 1x resolution, GPU Fast, asynchronous shaders, BC3, conservative VRAM, reactive "
     "flushing off, FIFO Relaxed VSync, ScaleForce, FXAA, the Normal CPU clock and Balanced GPU "
     "fences.",
     SetHyruleWarriorsRecommendedSettings},
    {MarioTennisFeverProgramId, "Mario Tennis Fever",
     "Vulkan, 1x resolution, GPU Fast, asynchronous shaders, BC3, conservative VRAM, reactive "
     "flushing off, FIFO Relaxed VSync, ScaleForce, FXAA, Balanced GPU fences and asynchronous "
     "presentation.",
     SetMarioTennisFeverRecommendedSettings},
    {KirbyAirRidersProgramId, "Kirby Air Riders",
     "Vulkan, 1x resolution, GPU Fast, asynchronous shaders so its compute pipelines build off "
     "the presenting thread, BC3, conservative VRAM, reactive flushing off, FIFO Relaxed VSync, "
     "ScaleForce, FXAA, and Balanced GPU fences for the texture and object flickering.",
     SetKirbyAirRidersRecommendedSettings},
    {OcarinaOfTimeProgramId, "The Legend of Zelda: Ocarina of Time",
     "Vulkan, 1x resolution, GPU Fast, asynchronous shaders, BC3, conservative VRAM, reactive "
     "flushing off, FIFO Relaxed VSync, ScaleForce, FXAA, Balanced GPU fences for the texture "
     "and object flickering, and frame generation to 60.",
     SetOcarinaOfTimeRecommendedSettings},
};

/// The title's preset, or nullptr when it has none.
inline const GamePreset* FindGamePreset(u64 program_id) {
    for (const GamePreset& preset : GamePresets) {
        if (preset.program_id == program_id) {
            return &preset;
        }
    }
    return nullptr;
}

/// Applies the title's preset as per-game values for this session (RestoreGlobalState drops them
/// when emulation stops). False when it was declined or the title has none.
inline bool ApplyGamePreset(Values& settings, u64 program_id, bool accepted) {
    const GamePreset* const preset = FindGamePreset(program_id);
    if (!accepted || preset == nullptr) {
        return false;
    }
    preset->apply(settings, false);
    return true;
}

} // namespace Settings
