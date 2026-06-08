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

enum class SessionControlManagerDecisionKind : u32 {
  force_disconnect,
  require_reauth,
  revoke_session,
  refused
};

struct SessionControlManagerRequest {
  std::string session_uuid;
  std::string database_uuid;
  std::string principal_uuid;
  std::string mga_transaction_uuid;
  std::string evidence_uuid;
  std::string idempotency_key;
  scratchbird::core::platform::u64 local_transaction_id = 0;
  scratchbird::core::platform::u64 catalog_generation = 0;
  bool disconnect_requested = false;
  bool reauth_requested = false;
  bool revoke_requested = false;
  bool session_visible = false;
  bool disconnect_allowed = false;
  bool token_visible = false;
  bool security_metrics_authoritative = false;
  bool durable_catalog_bound = false;
  bool transaction_inventory_bound = false;
  bool intended_state_observed = false;
  bool cluster_route_requested = false;
  bool client_authority = false;
};

struct SessionControlManagerEvidenceField {
  std::string key;
  std::string value;
};

struct SessionControlManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  SessionControlManagerDecisionKind decision =
      SessionControlManagerDecisionKind::refused;
  scratchbird::core::agents::AgentLocalWorkflowRecord workflow_record;
  std::vector<SessionControlManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool workflow_record_written = false;
  bool outcome_verified = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* SessionControlManagerDecisionKindName(
    SessionControlManagerDecisionKind decision);
SessionControlManagerResult EvaluateSessionControlManagerRequest(
    const SessionControlManagerRequest& request);
SessionControlManagerResult EvaluateSessionControlManagerRequest(
    scratchbird::core::agents::AgentLocalWorkflowLedger* ledger,
    const SessionControlManagerRequest& request);
DiagnosticRecord MakeSessionControlManagerDiagnostic(Status status,
                                                     std::string diagnostic_code,
                                                     std::string message_key,
                                                     std::string detail = {});

const char* session_control_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
