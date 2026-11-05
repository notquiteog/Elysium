// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/settings.h"

namespace Settings {

inline constexpr u64 MarioKartWorldProgramId = 0x0400C3F00006E000ULL;
inline constexpr u64 DonkeyKongBananzaProgramId = 0x040074A01BF12000ULL;
inline constexpr u64 HyruleWarriorsAgeOfImprisonmentProgramId = 0x04003F40232DE000ULL;
inline constexpr u64 MarioTennisFeverProgramId = 0x0400D4201C6EA000ULL;

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
} // namespace Detail

inline void SetMarioKartWorldRecommendedSettings(Values& settings, bool global) {
    Detail::SetSwitch2BaseSettings(settings, global);
    Detail::SessionSetter(global)(settings.use_docked_mode, ConsoleMode::Handheld);
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
     "flushing off, FIFO Relaxed VSync, handheld mode, ScaleForce, and FXAA.",
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
