// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC-012 planner bridge for reservation-backed temporary memory.
#include "reservation_backed_memory_resource.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::planner {

struct PlannerReservationBackedMemoryResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  scratchbird::core::platform::u64 planned_node_count = 0;
  scratchbird::core::platform::u64 allocated_bytes = 0;
  std::string digest;
  scratchbird::core::platform::DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

PlannerReservationBackedMemoryResult BuildPlannerTemporaryWorkFromReservedResource(
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    std::string planning_route_label,
    scratchbird::core::platform::u64 planned_node_count,
    bool parser_or_donor_authority,
    bool memory_plan_authority);

}  // namespace scratchbird::engine::planner
