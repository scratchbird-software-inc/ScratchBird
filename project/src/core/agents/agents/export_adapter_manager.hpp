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

enum class ExportAdapterManagerDecisionKind : u32 {
  enable_export,
  disable_export,
  shed_export,
  refused
};

struct ExportAdapterManagerRequest {
  std::string adapter_uuid;
  std::string database_uuid;
  std::string principal_uuid;
  std::string mga_transaction_uuid;
  std::string evidence_uuid;
  std::string idempotency_key;
  u64 queue_depth = 0;
  u64 local_transaction_id = 0;
  u64 catalog_generation = 0;
  bool enable_requested = false;
  bool disable_requested = false;
  bool shed_requested = false;
  bool adapter_visible = false;
  bool config_valid = false;
  bool redaction_policy_valid = false;
  bool residency_policy_valid = false;
  bool metadata_authoritative = false;
  bool durable_catalog_bound = false;
  bool transaction_inventory_bound = false;
  bool intended_state_observed = false;
  bool cluster_route_requested = false;
  bool client_authority = false;
};

struct ExportAdapterManagerEvidenceField {
  std::string key;
  std::string value;
};

struct ExportAdapterManagerResult {
  Status status;
  DiagnosticRecord diagnostic;
  ExportAdapterManagerDecisionKind decision =
      ExportAdapterManagerDecisionKind::refused;
  scratchbird::core::agents::AgentLocalWorkflowRecord workflow_record;
  std::vector<ExportAdapterManagerEvidenceField> evidence;
  bool fail_closed = true;
  bool workflow_record_written = false;
  bool outcome_verified = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* ExportAdapterManagerDecisionKindName(
    ExportAdapterManagerDecisionKind decision);
ExportAdapterManagerResult EvaluateExportAdapterManagerRequest(
    const ExportAdapterManagerRequest& request);
ExportAdapterManagerResult EvaluateExportAdapterManagerRequest(
    scratchbird::core::agents::AgentLocalWorkflowLedger* ledger,
    const ExportAdapterManagerRequest& request);
DiagnosticRecord MakeExportAdapterManagerDiagnostic(Status status,
                                                    std::string diagnostic_code,
                                                    std::string message_key,
                                                    std::string detail = {});

const char* export_adapter_manager_implementation_anchor();

}  // namespace scratchbird::core::agents::implemented_agents
