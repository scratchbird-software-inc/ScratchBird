// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-RESIDENCY-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class IndexResidencyTarget : u32 {
  cold = 1,
  warm = 2,
  hot = 3,
  pinned = 4,
  spillable = 5
};

enum class IndexResidencyAction : u32 {
  grant = 1,
  degrade = 2,
  refuse = 3,
  evict = 4,
  restore_from_cold_start = 5
};

struct IndexResidencyRequest {
  TypedUuid index_uuid;
  IndexFamily family = IndexFamily::unknown;
  IndexResidencyTarget target = IndexResidencyTarget::warm;
  u64 estimated_hot_bytes = 0;
  u64 memory_budget_bytes = 0;
  u64 current_pressure_score = 0;
  bool policy_allows_pin = false;
  bool cold_start_image_available = false;
};

struct IndexResidencyDecision {
  Status status;
  IndexResidencyAction action = IndexResidencyAction::refuse;
  IndexResidencyTarget granted_target = IndexResidencyTarget::cold;
  u64 admitted_bytes = 0;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && action != IndexResidencyAction::refuse; }
};

IndexResidencyDecision PlanIndexResidency(const IndexResidencyRequest& request);
DiagnosticRecord MakeIndexResidencyDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {});

}  // namespace scratchbird::core::index
