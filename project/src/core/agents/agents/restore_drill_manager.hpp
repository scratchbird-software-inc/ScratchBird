// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "agent_local_workflow.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents::implemented_agents {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;

enum class RestoreDrillManagerDecisionKind : u32 {
  no_action,
  run_restore_drill,
  refused
};

struct RestoreDrillManagerRequest {
  std::string drill_uuid;
  std::string database_uuid;
  std::string principal_uuid;
  std::string mga_transaction_uuid;
  std::string evidence_uuid;
  std::string idempotency_key;
  scratchbird::core::platform::u64 local_transaction_id = 0;
  scratchbird::core::platform::u64 catalog_generation = 0;
  bool run_requested = false;
  bool target_isolated = false;
  bool resources_available = false;
  bool backup_manifest_available = false;
  bool restore_inspection_open = false;
  bool metadata_authoritative = false;
  bool storage_snapshot_authoritative = false;
  bool durable_catalog_bound = false;
  bool transaction_inventory_bound = false;
  bool intended_state_observed = false;
  bool cluster_route_requested = false;
  bool parser_authority = false;
};

struct RestoreDrillManagerEvidenceField {
  std::string key;
  std::string value;
};

struct RestoreDrillManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  RestoreDrillManagerDecisionKind decision =
      RestoreDrillManagerDecisionKind::refused;
  scratchbird::core::agents::AgentLocalWorkflowRecord workflow_record;
  std::vector<RestoreDrillManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool workflow_record_written = false;
  bool outcome_verified = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* RestoreDrillManagerDecisionKindName(
    RestoreDrillManagerDecisionKind decision);
RestoreDrillManagerResult EvaluateRestoreDrillManagerRequest(
    const RestoreDrillManagerRequest& request);
RestoreDrillManagerResult EvaluateRestoreDrillManagerRequest(
    scratchbird::core::agents::AgentLocalWorkflowLedger* ledger,
    const RestoreDrillManagerRequest& request);
DiagnosticRecord MakeRestoreDrillManagerDiagnostic(Status status,
                                                   std::string diagnostic_code,
                                                   std::string message_key,
                                                   std::string detail = {});

const char* restore_drill_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
