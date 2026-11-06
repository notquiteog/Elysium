// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/host1x/syncpoint_manager.h"

using Tegra::Host1x::SyncpointManager;

TEST_CASE("Syncpoint ids past the table are dropped, valid ones still run their actions",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    constexpr u32 Last = SyncpointManager::NUM_MAX_SYNCPOINTS - 1;
    bool fired = false;
    manager->RegisterGuestAction(Last, 1, [&fired] { fired = true; });

    // 24-bit ids as the puller's fence action can carry them.
    manager->IncrementGuest(SyncpointManager::NUM_MAX_SYNCPOINTS);
    manager->IncrementHost(0xFFFFFF);
    REQUIRE_FALSE(fired);

    manager->IncrementGuest(Last);
    REQUIRE(fired);
    REQUIRE(manager->GetGuestSyncpointValue(Last) == 1);
}

TEST_CASE("Syncpoint actions run outside the guard, in value order",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    std::vector<u32> order;
    bool nested = false;
    // An action that registers another one takes the guard itself (the composite action can block
    // on the display while other threads need it); under the guard this deadlocked.
    manager->RegisterGuestAction(0, 2, [&] {
        order.push_back(2);
        manager->RegisterGuestAction(1, 1, [&nested] { nested = true; });
    });
    manager->RegisterGuestAction(0, 1, [&order] { order.push_back(1); });
    manager->RegisterGuestAction(0, 3, [&order] { order.push_back(3); });
    manager->IncrementGuest(0);
    manager->IncrementGuest(0);
    REQUIRE(order == std::vector<u32>{1, 2});
    manager->IncrementGuest(1);
    REQUIRE(nested);
    manager->IncrementGuest(0);
    REQUIRE(order == std::vector<u32>{1, 2, 3});
}

TEST_CASE("A syncpoint action can cancel another due action from inside",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    bool second = false;
    SyncpointManager::ActionHandle handle{};
    manager->RegisterGuestAction(0, 1, [&] { manager->DeregisterGuestAction(0, handle); });
    handle = manager->RegisterGuestAction(0, 1, [&second] { second = true; });
    manager->IncrementGuest(0); // must not wait for itself
    REQUIRE(second); // already taken out when the first one ran
}

TEST_CASE("A cancel from another thread returns only after a due action ran",
          "[video_core][syncpoint_manager]") {
    const auto manager = std::make_unique<SyncpointManager>();
    std::atomic<bool> release{false};
    std::atomic<bool> entered{false};
    std::atomic<bool> second{false};
    manager->RegisterGuestAction(0, 1, [&] {
        entered = true;
        while (!release) {
            std::this_thread::yield();
        }
    });
    const auto handle = manager->RegisterGuestAction(0, 1, [&second] { second = true; });
    std::thread incrementer{[&] { manager->IncrementGuest(0); }};
    while (!entered) {
        std::this_thread::yield();
    }
    // The guard is free while the first action blocks (registering does not wait).
    manager->RegisterGuestAction(1, 1, [] {});
    std::thread releaser{[&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        release = true;
    }};
    manager->DeregisterGuestAction(0, handle);
    REQUIRE(second);
    releaser.join();
    incrementer.join();
}
