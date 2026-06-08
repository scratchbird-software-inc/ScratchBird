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

enum class IdentityManagerDecisionKind : u32 {
  lock_user,
  require_reauth,
  emit_identity_evidence,
  refused
};

struct IdentityManagerRequest {
  std::string principal_uuid;
  std::string database_uuid;
  std::string operator_principal_uuid;
  std::string mga_transaction_uuid;
  std::string evidence_uuid;
  std::string idempotency_key;
  u64 auth_attempts_total = 0;
  u64 local_transaction_id = 0;
  u64 catalog_generation = 0;
  bool lock_requested = false;
  bool reauth_requested = false;
  bool emit_evidence_requested = false;
  bool identity_metrics_authoritative = false;
  bool explicit_admin_request = false;
  bool anomaly_detected = false;
  bool redaction_policy_valid = false;
  bool durable_catalog_bound = false;
  bool transaction_inventory_bound = false;
  bool intended_state_observed = false;
  bool cluster_route_requested = false;
  bool parser_authority = false;
};

struct IdentityManagerEvidenceField {
  std::string key;
  std::string value;
};

struct IdentityManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  IdentityManagerDecisionKind decision = IdentityManagerDecisionKind::refused;
  scratchbird::core::agents::AgentLocalWorkflowRecord workflow_record;
  std::vector<IdentityManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool workflow_record_written = false;
  bool outcome_verified = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* IdentityManagerDecisionKindName(IdentityManagerDecisionKind decision);
IdentityManagerResult EvaluateIdentityManagerRequest(const IdentityManagerRequest& request);
IdentityManagerResult EvaluateIdentityManagerRequest(
    scratchbird::core::agents::AgentLocalWorkflowLedger* ledger,
    const IdentityManagerRequest& request);
DiagnosticRecord MakeIdentityManagerDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail = {});

const char* identity_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
