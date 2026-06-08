// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-FAULT-INJECTION-MATRIX-CLOSURE-ANCHOR

#include "index_family_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

struct IndexFaultInjectionMatrixRow {
  std::string surface;
  std::string family_id;
  std::string scenario_class;
  std::string fault_point;
  std::string expected_action;
  std::string diagnostic_code;
  std::string message_key;
  std::string capability_blocker;
  bool concrete_execution_result = false;
  bool recovered = false;
  bool refused = true;
  bool planner_visible = false;
  bool fail_closed = true;
  bool old_or_new_validated_root_only = false;
  bool exactly_one_visible_generation = false;
  bool unsafe_half_published_state_refused = false;
  bool crash_before_generation_publish_validated = false;
  bool crash_after_generation_publish_validated = false;
  bool reopen_validated = false;
  bool repair_rebuild_recommendation = false;
  bool repair_validated = false;
  bool rebuild_validated = false;
  bool backup_restore_identity_validated = false;
  bool cleanup_horizon_bound = false;
  bool corruption_classified = false;
  bool concurrent_mutation_serialized = false;
  bool deterministic_diagnostics = false;
  bool donor_policy_refused = false;
  bool cluster_external_provider_only = false;
  bool runtime_dependency_free = true;
  bool parser_authority = false;
  bool donor_authority = false;
  bool provider_authority = false;
  bool storage_authority = false;
  bool visibility_authority = false;
  bool security_authority = false;
  bool transaction_finality_authority = false;
  bool recovery_authority = false;
  std::vector<std::string> evidence;
};

struct IndexFaultInjectionMatrixResult {
  Status status;
  DiagnosticRecord diagnostic;
  std::vector<IndexFaultInjectionMatrixRow> rows;

  bool ok() const {
    return status.ok();
  }
};

IndexFaultInjectionMatrixResult BuildIndexFaultInjectionCrashMatrix();

}  // namespace scratchbird::core::index
