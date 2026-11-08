// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>

namespace VideoCommon {

/// What a pipeline cache merge did.
///
/// Kept in a header of its own so a caller that only wants the counts does not have to include
/// shader_environment.h, which reaches shader_info.h and does not survive Qt's `emit` macro.
struct PipelineCacheMergeResult {
    size_t inputs_read{};
    size_t records_total{};
    size_t records_written{};
    size_t duplicates{};
    size_t rejected_files{};
};

} // namespace VideoCommon
