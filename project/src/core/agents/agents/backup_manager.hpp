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

enum class BackupManagerDecisionKind : u32 {
  no_action,
  start_backup,
  cancel_backup,
  verify_backup,
  refused
};

struct BackupManagerRequest {
  std::string backup_uuid;
  std::string database_uuid;
  std::string principal_uuid;
  std::string mga_transaction_uuid;
  std::string evidence_uuid;
  std::string idempotency_key;
  u64 local_transaction_id = 0;
  u64 catalog_generation = 0;
  bool start_requested = false;
  bool cancel_requested = false;
  bool verify_requested = false;
  bool blockers_clear = false;
  bool backup_cancellable = false;
  bool manifest_available = false;
  bool metadata_authoritative = false;
  bool durable_catalog_bound = false;
  bool transaction_inventory_bound = false;
  bool intended_state_observed = false;
  bool storage_snapshot_authoritative = false;
  bool mga_hold_authoritative = false;
  bool cluster_route_requested = false;
  bool parser_authority = false;
};

struct BackupManagerEvidenceField {
  std::string key;
  std::string value;
};

struct BackupManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  BackupManagerDecisionKind decision = BackupManagerDecisionKind::refused;
  scratchbird::core::agents::AgentLocalWorkflowRecord workflow_record;
  std::vector<BackupManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool local_workflow = true;
  bool workflow_record_written = false;
  bool outcome_verified = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* BackupManagerDecisionKindName(BackupManagerDecisionKind decision);
BackupManagerResult EvaluateBackupManagerRequest(const BackupManagerRequest& request);
BackupManagerResult EvaluateBackupManagerRequest(
    scratchbird::core::agents::AgentLocalWorkflowLedger* ledger,
    const BackupManagerRequest& request);
DiagnosticRecord MakeBackupManagerDiagnostic(Status status,
                                             std::string diagnostic_code,
                                             std::string message_key,
                                             std::string detail = {});

const char* backup_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
