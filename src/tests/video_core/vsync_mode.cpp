// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/vsync_mode.h"

using Settings::VSyncMode;
using VideoCore::ResolveVSyncMode;

namespace {
constexpr bool kImm = true, kMailbox = true, kRelaxed = true;
} // namespace

TEST_CASE("A supported VSync mode is the one used", "[video_core][vsync]") {
    CHECK(ResolveVSyncMode(VSyncMode::Immediate, kImm, kMailbox, kRelaxed) == VSyncMode::Immediate);
    CHECK(ResolveVSyncMode(VSyncMode::Mailbox, kImm, kMailbox, kRelaxed) == VSyncMode::Mailbox);
    CHECK(ResolveVSyncMode(VSyncMode::FifoRelaxed, kImm, kMailbox, kRelaxed) ==
          VSyncMode::FifoRelaxed);
    CHECK(ResolveVSyncMode(VSyncMode::Fifo, kImm, kMailbox, kRelaxed) == VSyncMode::Fifo);
}

TEST_CASE("FIFO is the one mode always available", "[video_core][vsync]") {
    for (const VSyncMode requested : {VSyncMode::Immediate, VSyncMode::Mailbox, VSyncMode::Fifo,
                                      VSyncMode::FifoRelaxed}) {
        CAPTURE(static_cast<int>(requested));
        CHECK(ResolveVSyncMode(requested, false, false, false) == VSyncMode::Fifo);
    }
}

// The case this machine hits: a surface offering neither FIFO Relaxed nor Mailbox. Falling back
// to strict FIFO holds late frames to the next vblank, which leaves the frame rate only whole
// divisors of the refresh rate to sit on -- 60, 30, then 20 -- and that was Kirby Air Riders at
// 20 fps while the game itself was producing 27-59.
TEST_CASE("FIFO Relaxed never falls back to the mode that holds late frames",
          "[video_core][vsync]") {
    CHECK(ResolveVSyncMode(VSyncMode::FifoRelaxed, kImm, false, false) == VSyncMode::Immediate);
    CHECK(ResolveVSyncMode(VSyncMode::FifoRelaxed, false, kMailbox, false) == VSyncMode::Mailbox);
    // Mailbox is preferred over Immediate when both are there: it does not tear.
    CHECK(ResolveVSyncMode(VSyncMode::FifoRelaxed, kImm, kMailbox, false) == VSyncMode::Mailbox);
    // Only with no non-blocking mode at all is strict FIFO the answer.
    CHECK(ResolveVSyncMode(VSyncMode::FifoRelaxed, false, false, false) == VSyncMode::Fifo);
}

TEST_CASE("Immediate falls back to the other non-blocking mode", "[video_core][vsync]") {
    CHECK(ResolveVSyncMode(VSyncMode::Immediate, false, kMailbox, kRelaxed) == VSyncMode::Mailbox);
    CHECK(ResolveVSyncMode(VSyncMode::Immediate, false, false, kRelaxed) == VSyncMode::Fifo);
}

TEST_CASE("Plain FIFO is never upgraded behind the caller's back", "[video_core][vsync]") {
    // Asking for FIFO means asking for every frame to wait for its vblank.
    CHECK(ResolveVSyncMode(VSyncMode::Fifo, kImm, kMailbox, kRelaxed) == VSyncMode::Fifo);
}
