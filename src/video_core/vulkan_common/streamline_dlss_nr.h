// SPDX-FileCopyrightText: Copyright 2026 Elysium Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// NVIDIA DLSS 5 neural rendering (DLSS-NR) through Streamline. The public Streamline headers name
// the feature (kFeatureDLSS_NR) and its buffers (kBufferTypeUplift*) but not its options, which
// only NVIDIA's NDA SDK declares in sl_dlss_nr.h. They are declared here with the layout openNR
// recovered from Streamline 2.13's sl.dlss_nr.dll (https://github.com/clshortfuse/openNR, MIT,
// Copyright 2026 Carlos Lopez); the field names follow DagorEngine's integration (BSD-3-Clause).

#pragma once

#include <cstddef>
#include <cstdint>

#include <sl.h>

namespace Vulkan::Streamline {

enum class DlssNrMode : uint32_t {
    eOff = 0,
    eOn = 1,
};

// {29DFDFE0-273A-4E72-B492-2DC823D5B1AD}
struct DlssNrOptions : sl::BaseStructure {
    static constexpr sl::StructType s_structType = sl::StructType(
        {0x29dfdfe0, 0x273a, 0x4e72, {0xb4, 0x92, 0x2d, 0xc8, 0x23, 0xd5, 0xb1, 0xad}});

    DlssNrOptions() : sl::BaseStructure(s_structType, sl::kStructVersion3) {}

    DlssNrMode mode = DlssNrMode::eOff;
    float intensity = 1.0f;
    float localToneStrength = 1.0f;
    float localStructureStrength = 1.0f;
    float globalToneStrength = 1.0f;
    uint32_t style = 0;
    uint32_t preset = 1;
    sl::Boolean useAutoMask = sl::Boolean::eFalse;
    /// Negative leaves it to the model.
    float skinStructureStrength = -1.0f;
    /// An sl::DLSSMode value: 1 performance, 2 balanced, 3 quality, 6 DLAA.
    uint32_t performanceMode = 3;
};

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-offsetof"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winvalid-offsetof"
#endif
static_assert(sizeof(void*) == 8, "DLSS-NR is only available on 64-bit Windows");
static_assert(sizeof(DlssNrOptions) == 0x48);
static_assert(offsetof(DlssNrOptions, mode) == 0x20);
static_assert(offsetof(DlssNrOptions, intensity) == 0x24);
static_assert(offsetof(DlssNrOptions, globalToneStrength) == 0x30);
static_assert(offsetof(DlssNrOptions, style) == 0x34);
static_assert(offsetof(DlssNrOptions, preset) == 0x38);
static_assert(offsetof(DlssNrOptions, useAutoMask) == 0x3c);
static_assert(offsetof(DlssNrOptions, skinStructureStrength) == 0x40);
static_assert(offsetof(DlssNrOptions, performanceMode) == 0x44);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using PFun_slDLSSNRSetOptions = sl::Result(const sl::ViewportHandle& viewport,
                                           const DlssNrOptions& options);

} // namespace Vulkan::Streamline
