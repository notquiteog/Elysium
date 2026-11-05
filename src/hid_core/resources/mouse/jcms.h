// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "common/common_types.h"
#include "common/typed_address.h"
#include "hid_core/resources/controller_base.h"
#include "hid_core/resources/ring_lifo.h"

namespace Kernel {
class KProcess;
}

namespace Service::HID {

// ponytail: Joy-Con 2 mouse sensor (hid 3110-3113). The SDK reads one lifo per handle straight
// from the transfer memory it passed to InitializeJcms; we feed it from the emulated sticks.
class Jcms final : public ControllerBase {
public:
    explicit Jcms(Core::HID::HIDCore& hid_core_);
    ~Jcms() override;

    void OnInit() override;
    void OnRelease() override;
    void OnUpdate(const Core::Timing::CoreTiming& core_timing) override;

    void SetTransferMemory(Common::ProcessAddress t_mem, u64 size, Kernel::KProcess* owner);
    void SetStarted(u32 handle, bool is_started);
    bool IsStarted(std::size_t npad_index, bool is_right) const;

    // npad id (byte 0, below 8) times two plus JcmsType (byte 1, 0 left or 1 right)
    static constexpr std::size_t JcmsCount = 16;

private:
    // ponytail: sampling_number is checked by the SDK reader; deltas and attribute bit 1 (on a
    // surface) are what Drag x Drive sums per frame. Field +0x00 is never read there.
    struct JcmsState {
        s64 unknown_00{};
        s64 sampling_number{};
        s32 delta_x{};
        s32 delta_y{};
        u32 attribute{};
        u32 reserved{};
    };
    static_assert(sizeof(JcmsState) == 0x20, "JcmsState is an invalid size");

    using JcmsLifo = Lifo<JcmsState, 17>;
    static_assert(sizeof(JcmsLifo) == 0x2c8, "JcmsLifo is an invalid size");

    std::array<JcmsLifo, JcmsCount> lifos{};
    std::array<bool, JcmsCount> started{};
    Common::ProcessAddress transfer_memory{};
    u64 transfer_memory_size{};
    Kernel::KProcess* transfer_memory_owner{};
};

} // namespace Service::HID
