// SPDX-FileCopyrightText: Copyright 2026 Elysium Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The same stubs src/tests/common/undefined_fix.cpp provides: this benchmark links the emulator's
// libraries without a frontend, so nothing defines the play time manager it never uses.

#include "frontend_common/play_time_manager.h"

namespace PlayTime {

PlayTimeManager::PlayTimeManager() {}
PlayTimeManager::~PlayTimeManager() {}
u64 PlayTimeManager::GetPlayTime(u64 program_id) const {
    return 0;
}

} // namespace PlayTime
