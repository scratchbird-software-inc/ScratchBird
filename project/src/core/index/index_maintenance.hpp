// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-MAINTENANCE-CLOSURE-ANCHOR

#include "exact_index_leaf_cleanup.hpp"
#include "index_family_registry.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class IndexMaintenanceOperation : u32 {
  verify = 1,
  rebuild = 2,
  rebalance = 3,
  refresh = 4,
  compact = 5,
  resummarize = 6,
  relocate = 7,
  warm = 8,
  cool = 9
};

enum class IndexDmlMaintenanceStrategyKind : u32 {
  synchronous_physical_rewrite = 1,
  deferred_secondary_delta_ledger = 2,
  page_aware_change_buffer = 3,
  provider_specific = 4,
  rebuild_from_authoritative_base = 5,
  refused = 6
};

struct IndexMaintenanceRequest {
  TypedUuid index_uuid;
  IndexFamily family = IndexFamily::unknown;
  IndexMaintenanceOperation operation = IndexMaintenanceOperation::verify;
  u64 page_budget = 0;
  u64 byte_budget = 0;
  u64 time_budget_microseconds = 0;
  bool online = true;
  bool read_only_database = false;
  bool policy_allows_mutation = false;
  bool evaluate_exact_leaf_pressure = false;
  ExactIndexLeafPressureRequest exact_leaf_pressure;
};

struct IndexMaintenancePlan {
  Status status;
  bool admitted = false;
  bool mutation_required = false;
  bool requires_exclusive_access = false;
  bool exact_leaf_pressure_evaluated = false;
  ExactIndexLeafPressureDecision exact_leaf_pressure_decision;
  std::vector<std::string> steps;
  std::vector<ExactIndexLeafCleanupEvidenceField> selected_action_evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

struct IndexDmlMaintenanceStrategy {
  Status status;
  IndexFamily family = IndexFamily::unknown;
  IndexDmlMaintenanceStrategyKind strategy =
      IndexDmlMaintenanceStrategyKind::refused;
  bool admitted = false;
  bool fail_closed = true;
  bool dml_route_supported = false;
  bool maintenance_route_supported = false;
  bool exact_recheck_required = true;
  bool exact_recheck_strategy_bound = false;
  bool exact_recheck_gate_passed = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool synchronous = false;
  bool deferred_delta = false;
  bool buffered = false;
  bool provider_specific = false;
  bool rebuild_required = false;
  bool runtime_available = false;
  bool benchmark_clean = false;
  std::string strategy_id;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted && !fail_closed; }
};

const char* IndexDmlMaintenanceStrategyKindName(
    IndexDmlMaintenanceStrategyKind strategy);
IndexDmlMaintenanceStrategy ClassifyIndexDmlMaintenanceStrategy(
    IndexFamily family);
IndexMaintenancePlan PlanIndexMaintenance(const IndexMaintenanceRequest& request);
DiagnosticRecord MakeIndexMaintenanceDiagnostic(Status status,
                                                std::string diagnostic_code,
                                                std::string message_key,
                                                std::string detail = {});

}  // namespace scratchbird::core::index
