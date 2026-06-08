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

enum class ArchiveManagerDecisionKind : u32 {
  no_action,
  seal_archive_slice,
  request_verify_slice,
  refused
};

struct ArchiveManagerRequest {
  std::string slice_uuid;
  std::string database_uuid;
  std::string principal_uuid;
  std::string mga_transaction_uuid;
  std::string evidence_uuid;
  std::string idempotency_key;
  scratchbird::core::platform::u64 local_transaction_id = 0;
  scratchbird::core::platform::u64 catalog_generation = 0;
  bool seal_requested = false;
  bool verify_requested = false;
  bool slice_complete = false;
  bool slice_available = false;
  bool metadata_authoritative = false;
  bool durable_catalog_bound = false;
  bool transaction_inventory_bound = false;
  bool intended_state_observed = false;
  bool legal_hold_active = false;
  bool cluster_route_requested = false;
  bool recovery_authority = false;
};

struct ArchiveManagerEvidenceField {
  std::string key;
  std::string value;
};

struct ArchiveManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  ArchiveManagerDecisionKind decision = ArchiveManagerDecisionKind::refused;
  scratchbird::core::agents::AgentLocalWorkflowRecord workflow_record;
  std::vector<ArchiveManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool workflow_record_written = false;
  bool outcome_verified = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* ArchiveManagerDecisionKindName(ArchiveManagerDecisionKind decision);
ArchiveManagerResult EvaluateArchiveManagerRequest(const ArchiveManagerRequest& request);
ArchiveManagerResult EvaluateArchiveManagerRequest(
    scratchbird::core::agents::AgentLocalWorkflowLedger* ledger,
    const ArchiveManagerRequest& request);
DiagnosticRecord MakeArchiveManagerDiagnostic(Status status,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string detail = {});

const char* archive_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
