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

enum class JobControlManagerDecisionKind : u32 {
  cancel_job,
  retry_job,
  suppress_job,
  refused
};

struct JobControlManagerRequest {
  std::string job_uuid;
  std::string database_uuid;
  std::string principal_uuid;
  std::string mga_transaction_uuid;
  std::string evidence_uuid;
  std::string idempotency_key;
  scratchbird::core::platform::u64 local_transaction_id = 0;
  scratchbird::core::platform::u64 catalog_generation = 0;
  bool cancel_requested = false;
  bool retry_requested = false;
  bool suppress_requested = false;
  bool job_visible = false;
  bool job_cancellable = false;
  bool retry_policy_valid = false;
  bool suppression_scope_valid = false;
  bool job_metrics_authoritative = false;
  bool durable_catalog_bound = false;
  bool transaction_inventory_bound = false;
  bool intended_state_observed = false;
  bool cluster_route_requested = false;
  bool client_authority = false;
};

struct JobControlManagerEvidenceField {
  std::string key;
  std::string value;
};

struct JobControlManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  JobControlManagerDecisionKind decision =
      JobControlManagerDecisionKind::refused;
  scratchbird::core::agents::AgentLocalWorkflowRecord workflow_record;
  std::vector<JobControlManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool workflow_record_written = false;
  bool outcome_verified = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* JobControlManagerDecisionKindName(JobControlManagerDecisionKind decision);
JobControlManagerResult EvaluateJobControlManagerRequest(
    const JobControlManagerRequest& request);
JobControlManagerResult EvaluateJobControlManagerRequest(
    scratchbird::core::agents::AgentLocalWorkflowLedger* ledger,
    const JobControlManagerRequest& request);
DiagnosticRecord MakeJobControlManagerDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

const char* job_control_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
