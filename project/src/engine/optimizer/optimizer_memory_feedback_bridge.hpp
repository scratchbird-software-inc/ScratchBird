// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_feedback.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// SEARCH_KEY: OPCH_MEMORY_GRANT_FEEDBACK_SPILL_COSTING
// MMCH_OPTIMIZER_MEMORY_FEEDBACK_EVIDENCE_BRIDGE
struct OptimizerMemoryFeedbackEvidence {
  std::string schema_id = "sb.optimizer.memory_feedback_evidence.v1";
  u64 schema_version = 1;
  std::string query_uuid;
  std::string scope_uuid;
  std::string route_kind = "sql_select";
  std::string route_label;
  std::string operator_family;
  std::string plan_shape;
  std::string plan_node_id;
  std::string source_quality = "observed_runtime";
  std::string source_kind = "resource_governance_reservation_ledger";
  std::string trust_provenance = "resource_governance_reservation_ledger";
  bool trusted_provenance = true;
  std::string provenance_digest;
  std::string redaction_class = "operational";
  std::string redaction_digest;
  std::string metric_snapshot_digest;
  std::string support_snapshot_digest;
  std::string reservation_id;
  std::string reservation_token;
  u64 reservation_generation = 0;
  u64 policy_generation = 0;
  u64 feedback_generation = 0;
  u64 catalog_epoch = 0;
  u64 security_epoch = 0;
  u64 redaction_epoch = 0;
  u64 statistics_epoch = 0;
  u64 observed_timestamp_ticks = 0;
  u64 received_timestamp_ticks = 0;
  u64 max_age_ticks = 60000000;
  u64 memory_grant_bytes = 0;
  u64 peak_memory_bytes = 0;
  u64 spill_bytes = 0;
  u64 spill_passes = 0;
  u64 allocation_failure_count = 0;
  bool governed_reservation = false;
  bool reservation_token_bound = false;
  bool resource_governance_ledger_recorded = false;
  bool bounded_support_bundle = false;
  bool support_bundle_redacted = false;
  bool support_bundle_fresh = false;
  bool real_operation_metric = false;
  bool operation_metric_runtime_path = false;
  bool protected_material_redacted = true;
  bool protected_material_exposed = false;
  bool synthetic = false;
  bool test_evidence = false;
  bool local_default_evidence = false;
  bool policy_default_evidence = false;
  bool advisory_only = true;
  bool mga_visibility_recheck_preserved = true;
  bool security_recheck_preserved = true;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool parser_client_or_donor_authority = false;
  bool recovery_or_wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool index_finality_authority = false;
  bool provider_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_authority = false;
  bool agent_action_authority = false;
};

struct OptimizerMemoryFeedbackBridgeResult {
  Status status;
  bool accepted = false;
  bool fail_closed = false;
  bool ceic_059_contract_accepted = false;
  bool authority_boundaries_clean = false;
  std::string diagnostic_code;
  DiagnosticRecord diagnostic;
  OptimizerRuntimeFeedback runtime_feedback;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && accepted && !fail_closed;
  }
};

OptimizerMemoryFeedbackBridgeResult BuildOptimizerMemoryFeedbackForPlanner(
    const OptimizerMemoryFeedbackEvidence& feedback);

}  // namespace scratchbird::engine::optimizer
