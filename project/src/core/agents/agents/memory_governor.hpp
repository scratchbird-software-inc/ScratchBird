// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_runtime.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class MemoryGovernorDecisionKind : u32 {
  allow_grant,
  deny_large_grant,
  force_spill,
  shrink_cache,
  refused
};

struct MemoryGovernorPolicy {
  bool present = true;
  bool valid = true;
  bool scope_compatible = true;
  bool deny_large_grants_allowed = true;
  bool force_spill_allowed = true;
  bool shrink_cache_allowed = true;
  u64 hard_limit_bytes = 0;
  u64 soft_limit_bytes = 0;
  u64 emergency_reserve_bytes = 0;
  u64 cache_shrink_floor_bytes = 0;
};

struct MemoryGovernorSnapshot {
  u64 current_bytes = 0;
  u64 requested_grant_bytes = 0;
  u64 spillable_bytes = 0;
  u64 cache_bytes = 0;
  u64 active_query_count = 0;
  bool memory_metrics_authoritative = false;
  bool resource_reservation_authoritative = false;
  bool grant_is_spillable = false;
  bool protected_foreground_work = true;
  bool parser_authority = false;
  bool reference_authority = false;
};

struct MemoryGovernorEvidenceField {
  std::string key;
  std::string value;
};

struct MemoryGovernorResult {
  Status status;
  DiagnosticRecord diagnostic;
  MemoryGovernorDecisionKind decision = MemoryGovernorDecisionKind::refused;
  std::vector<MemoryGovernorEvidenceField> evidence;
  bool fail_closed = true;
  bool grant_allowed = false;
  bool spill_required = false;
  bool cache_shrink_requested = false;
  u64 bytes_to_spill = 0;
  u64 bytes_to_shrink = 0;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* MemoryGovernorDecisionKindName(MemoryGovernorDecisionKind decision);
MemoryGovernorResult EvaluateMemoryGovernorGrant(
    const MemoryGovernorSnapshot& snapshot,
    const MemoryGovernorPolicy& policy);
DiagnosticRecord MakeMemoryGovernorDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {});

const char* memory_governor_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
