// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/settings_enums.h"

namespace VideoCore {

/// The VSync mode to present with, given what the surface offers.
///
/// Only FIFO is guaranteed by the Vulkan specification; the other three are optional, so the
/// mode asked for is not always the mode available.
[[nodiscard]] constexpr Settings::VSyncMode ResolveVSyncMode(Settings::VSyncMode requested,
                                                             bool has_immediate, bool has_mailbox,
                                                             bool has_fifo_relaxed) {
    switch (requested) {
    case Settings::VSyncMode::Immediate:
        if (has_immediate) {
            return Settings::VSyncMode::Immediate;
        }
        // Mailbox is the other mode that never blocks on the vblank.
        return has_mailbox ? Settings::VSyncMode::Mailbox : Settings::VSyncMode::Fifo;
    case Settings::VSyncMode::Mailbox:
        return has_mailbox ? Settings::VSyncMode::Mailbox : Settings::VSyncMode::Fifo;
    case Settings::VSyncMode::FifoRelaxed:
        if (has_fifo_relaxed) {
            return Settings::VSyncMode::FifoRelaxed;
        }
        // FIFO Relaxed asks that a frame which missed its vblank go out at once instead of
        // waiting for the next one. Mailbox and Immediate both keep that; strict FIFO is the one
        // mode that does not, and holding late frames leaves the rate only the whole divisors of
        // the refresh rate to sit on (60 -> 30 -> 20 at 60 Hz). Falling back to it answers the
        // request with its opposite, so take either non-blocking mode first.
        if (has_mailbox) {
            return Settings::VSyncMode::Mailbox;
        }
        return has_immediate ? Settings::VSyncMode::Immediate : Settings::VSyncMode::Fifo;
    case Settings::VSyncMode::Fifo:
    default:
        return Settings::VSyncMode::Fifo;
    }
}

} // namespace VideoCore
