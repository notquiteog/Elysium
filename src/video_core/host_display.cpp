// SPDX-FileCopyrightText: Copyright 2026 Elysium Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/host_display.h"

namespace VideoCore::HostDisplay {

std::atomic<s64> refresh_period_ns{};

} // namespace VideoCore::HostDisplay
