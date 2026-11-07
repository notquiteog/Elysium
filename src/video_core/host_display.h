// SPDX-FileCopyrightText: Copyright 2026 Elysium Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// What the frontend knows about the host's display and video_core does not.
//
// The host frame pacer needs the display's refresh period. On Windows it asks DWM for the
// compositor's own timing, but there is no such call to make from here on Wayland, X11 or macOS:
// the window belongs to the frontend, and video_core must not reach for Qt. The frontend reports
// the period it sees instead, and the pacer falls back to it.

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace VideoCore::HostDisplay {

/// The display's refresh period in nanoseconds, or 0 while the frontend has not reported one.
extern std::atomic<s64> refresh_period_ns;

/// Reports the refresh period of the display the render window is on. 0 when it is unknown, which
/// leaves the pacer off rather than pacing to a guess.
inline void SetRefreshPeriodNs(s64 period_ns) {
    refresh_period_ns.store(period_ns, std::memory_order_relaxed);
}

[[nodiscard]] inline s64 RefreshPeriodNs() {
    return refresh_period_ns.load(std::memory_order_relaxed);
}

} // namespace VideoCore::HostDisplay
