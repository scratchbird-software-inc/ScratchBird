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
using scratchbird::core::platform::u64;

enum class PitrManagerDecisionKind : u32 {
  estimate_pitr,
  request_restore_plan,
  refused
};

struct PitrManagerRequest {
  std::string target_uuid;
  std::string database_uuid;
  std::string principal_uuid;
  std::string mga_transaction_uuid;
  std::string evidence_uuid;
  std::string idempotency_key;
  u64 window_available_seconds = 0;
  u64 local_transaction_id = 0;
  u64 catalog_generation = 0;
  bool estimate_requested = false;
  bool restore_plan_requested = false;
  bool target_reachable = false;
  bool archive_window_authoritative = false;
  bool storage_snapshot_authoritative = false;
  bool metadata_authoritative = false;
  bool durable_catalog_bound = false;
  bool transaction_inventory_bound = false;
  bool intended_state_observed = false;
  bool cluster_route_requested = false;
  bool recovery_authority = false;
};

struct PitrManagerEvidenceField {
  std::string key;
  std::string value;
};

struct PitrManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  PitrManagerDecisionKind decision = PitrManagerDecisionKind::refused;
  scratchbird::core::agents::AgentLocalWorkflowRecord workflow_record;
  std::vector<PitrManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool workflow_record_written = false;
  bool outcome_verified = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* PitrManagerDecisionKindName(PitrManagerDecisionKind decision);
PitrManagerResult EvaluatePitrManagerRequest(const PitrManagerRequest& request);
PitrManagerResult EvaluatePitrManagerRequest(
    scratchbird::core::agents::AgentLocalWorkflowLedger* ledger,
    const PitrManagerRequest& request);
DiagnosticRecord MakePitrManagerDiagnostic(Status status,
                                           std::string diagnostic_code,
                                           std::string message_key,
                                           std::string detail = {});

const char* pitr_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
