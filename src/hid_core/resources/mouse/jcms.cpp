// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/core_timing.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "hid_core/hid_util.h"
#include "hid_core/resources/mouse/jcms.h"

namespace Service::HID {

Jcms::Jcms(Core::HID::HIDCore& hid_core_) : ControllerBase{hid_core_} {}

Jcms::~Jcms() = default;

void Jcms::OnInit() {}
void Jcms::OnRelease() {}

void Jcms::OnUpdate(const Core::Timing::CoreTiming& core_timing) {
    if (transfer_memory == 0 || transfer_memory_owner == nullptr) {
        return;
    }

    for (std::size_t index = 0; index < JcmsCount; ++index) {
        if (!started[index] || (index + 1) * sizeof(JcmsLifo) > transfer_memory_size) {
            continue;
        }
        const auto npad_id = IndexToNpadIdType(index / 2);
        const auto* controller = hid_core.GetEmulatedController(npad_id);
        if (controller == nullptr) {
            continue;
        }

        // ponytail: no real mouse sensor input exists, so the matching side's stick drives
        // that Joy-Con's sensor deltas (left stick -> left Joy-Con, right stick -> right).
        const auto sticks = controller->GetSticks();
        const auto buttons = controller->GetNpadButtons();
        const bool is_left = (index % 2) == 0;
        const auto& stick = is_left ? sticks.left : sticks.right;
        // Holding the stick click lifts that Joy-Con off the surface.
        const bool is_lifted = is_left ? buttons.stick_l.Value() : buttons.stick_r.Value();
        constexpr s32 counts_per_full_tilt = 40;

        auto& lifo = lifos[index];
        JcmsState state{};
        state.sampling_number = lifo.ReadCurrentEntry().state.sampling_number + 1;
        // Stick up means pushing the Joy-Con away from you, which is negative sensor y.
        state.delta_x = stick.x * counts_per_full_tilt / 0x7fff;
        state.delta_y = -stick.y * counts_per_full_tilt / 0x7fff;
        // Bit 0 connected, bit 1 resting on a surface.
        state.attribute = controller->IsConnected() ? (is_lifted ? 0b01 : 0b11) : 0;
        lifo.WriteNextEntry(state);

        transfer_memory_owner->GetMemory().WriteBlock(transfer_memory + index * sizeof(JcmsLifo),
                                                      &lifo, sizeof(JcmsLifo));
    }
}

void Jcms::SetTransferMemory(Common::ProcessAddress t_mem, u64 size, Kernel::KProcess* owner) {
    transfer_memory = t_mem;
    transfer_memory_size = size;
    transfer_memory_owner = owner;
    lifos = {};
    started = {};
}

void Jcms::SetStarted(u32 handle, bool is_started) {
    const std::size_t index = (handle & 0xff) * 2 + ((handle >> 8) & 0xff);
    if (index < JcmsCount) {
        started[index] = is_started;
    }
}

bool Jcms::IsStarted(std::size_t npad_index, bool is_right) const {
    const std::size_t index = npad_index * 2 + (is_right ? 1 : 0);
    return index < JcmsCount && started[index];
}

} // namespace Service::HID
