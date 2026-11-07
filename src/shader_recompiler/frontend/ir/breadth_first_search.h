// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <type_traits>
#include <queue>
#include <unordered_set>
#include <vector>

#include <boost/container/small_vector.hpp>

#include "shader_recompiler/frontend/ir/value.h"

namespace Shader::IR {

template <typename Pred, typename Resolve = std::identity>
auto BreadthFirstSearch(Value value, Pred&& pred, Resolve&& resolve = {})
    -> std::invoke_result_t<Pred, const Inst*> {
    value = resolve(value);
    if (value.IsImmediate()) {
        // Nothing to do with immediates
        return std::nullopt;
    }
    // Breadth-first search visiting the right most arguments first
    // Small vector has been determined from shaders in Super Smash Bros. Ultimate
    boost::container::small_vector<const Inst*, 2> visited;
    // Once a search grows, a linear scan of `visited` per argument made it quadratic: a 28,000
    // instruction Switch 2 compute shader took 3.4 s to translate, most of it here. Same visit
    // order, so the same result.
    std::unordered_set<const Inst*> visited_set;
    const auto first_visit = [&](const Inst* inst) {
        if (!visited_set.empty()) {
            return visited_set.insert(inst).second;
        }
        if (std::ranges::find(visited, inst) != visited.end()) {
            return false;
        }
        visited.push_back(inst);
        if (visited.size() > 64) {
            visited_set.insert(visited.begin(), visited.end());
        }
        return true;
    };
    std::queue<const Inst*> queue;
    queue.push(value.InstRecursive());

    while (!queue.empty()) {
        // Pop one instruction from the queue
        const Inst* const inst{queue.front()};
        queue.pop();
        if (const std::optional result = pred(inst)) {
            // This is the instruction we were looking for
            return result;
        }
        // Visit the right most arguments first
        for (size_t arg = inst->NumArgs(); arg--;) {
            const Value arg_value{resolve(inst->Arg(arg))};
            if (arg_value.IsImmediate()) {
                continue;
            }
            // Queue instruction if it hasn't been visited
            const Inst* const arg_inst{arg_value.InstRecursive()};
            if (first_visit(arg_inst)) {
                queue.push(arg_inst);
            }
        }
    }
    // SSA tree has been traversed and the result hasn't been found
    return std::nullopt;
}

/// BreadthFirstSearch that remembers the searches which found nothing.
///
/// A search that fails proves that nothing reachable from any instruction it visited can satisfy
/// `pred`, so a later search may skip those instructions. An instruction not reachable from a
/// skipped one is only ever queued by instructions that are not either, so the visit order, and
/// with it the result, is the one BreadthFirstSearch would have given.
///
/// The proof holds per `pred` AND per `resolve`, since `resolve` is what decides which operands
/// are followed: an `exhausted` set must not be shared between searches that differ in either.
template <typename Pred, typename Resolve = std::identity>
auto BreadthFirstSearchPruned(Value value, Pred&& pred,
                              std::unordered_set<const Inst*>& exhausted, Resolve&& resolve = {})
    -> std::invoke_result_t<Pred, const Inst*> {
    value = resolve(value);
    if (value.IsImmediate()) {
        return std::nullopt;
    }
    const Inst* const start{value.InstRecursive()};
    if (exhausted.contains(start)) {
        return std::nullopt;
    }
    // Every instruction queued, to be marked exhausted together if the search fails. Unlike
    // BreadthFirstSearch this always keeps the set as well: a failing search is the case that
    // matters here, and it needs the whole list.
    std::vector<const Inst*> visited{start};
    std::unordered_set<const Inst*> visited_set{start};
    std::queue<const Inst*> queue;
    queue.push(start);

    while (!queue.empty()) {
        const Inst* const inst{queue.front()};
        queue.pop();
        if (const std::optional result = pred(inst)) {
            return result;
        }
        // Visit the right most arguments first, as BreadthFirstSearch does
        for (size_t arg = inst->NumArgs(); arg--;) {
            const Value arg_value{resolve(inst->Arg(arg))};
            if (arg_value.IsImmediate()) {
                continue;
            }
            const Inst* const arg_inst{arg_value.InstRecursive()};
            if (exhausted.contains(arg_inst) || !visited_set.insert(arg_inst).second) {
                continue;
            }
            visited.push_back(arg_inst);
            queue.push(arg_inst);
        }
    }
    exhausted.insert(visited.begin(), visited.end());
    return std::nullopt;
}

} // namespace Shader::IR
