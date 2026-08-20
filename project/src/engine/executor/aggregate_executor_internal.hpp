// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "descriptor_value_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace scratchbird::engine::executor::detail {

enum class CanonicalAggregateRuntimeExecutionContext : std::uint8_t {
  ordinary_non_window = 0,
  state_exchange,
  window_frame_recompute,
  window_moving_inverse,
  window_state_spill,
};

CanonicalAggregateRuntimeResult
ExecuteCanonicalAggregateRuntimeBorrowedForContext(
    const CanonicalAggregateRuntimeRequest& request,
    const TypedPhysicalNodeDag& borrowed_execution_dag,
    const DescriptorBatch& borrowed_input_batch,
    CanonicalAggregateRuntimeExecutionContext context,
    std::optional<std::size_t> exact_final_output_ceiling = std::nullopt);

}  // namespace scratchbird::engine::executor::detail
