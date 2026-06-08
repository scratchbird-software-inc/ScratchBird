// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT
#include "agents/index_garbage_cleanup_agent.hpp"

#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

namespace idx = scratchbird::core::index;
namespace mga = scratchbird::transaction::mga;

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status AgentOkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status AgentErrorStatus() {
  return {StatusCode::platform_required_feature_missing,
          Severity::error,
          Subsystem::engine};
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void AddEvidence(IndexGarbageCleanupAgentResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

std::string DiagnosticDetail(const DiagnosticRecord& diagnostic) {
  if (!diagnostic.remediation_hint.empty()) {
    return diagnostic.remediation_hint;
  }
  std::string detail;
  for (const auto& argument : diagnostic.arguments) {
    if (!detail.empty()) {
      detail += ";";
    }
    detail += argument.key + "=" + argument.value;
  }
  return detail;
}

IndexGarbageCleanupAgentResult Finish(
    IndexGarbageCleanupAgentResult result,
    SecondaryIndexGarbageCleanupDecisionKind decision,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    bool fail_closed) {
  result.status = fail_closed ? AgentErrorStatus() : AgentOkStatus();
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeIndexGarbageCleanupAgentDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  AddEvidence(&result,
              "index_garbage_cleanup_agent",
              "dpc033_index_garbage_cleanup_agent_v1");
  AddEvidence(&result,
              "cleanup_horizon_service",
              "dpc030_authoritative_cleanup_horizon_v1");
  AddEvidence(&result, "authority_source", "durable_mga_transaction_inventory");
  AddEvidence(&result,
              "decision",
              idx::SecondaryIndexGarbageCleanupDecisionKindName(decision));
  AddEvidence(&result, "fail_closed", BoolText(fail_closed));
  AddEvidence(&result, "bounded_batch", BoolText(result.bounded_batch));
  AddEvidence(&result, "budget_exhausted", BoolText(result.budget_exhausted));
  AddEvidence(&result, "horizon_blocked", BoolText(result.horizon_blocked));
  AddEvidence(&result,
              "validation_before_ok",
              BoolText(result.validation_before_ok));
  AddEvidence(&result,
              "validation_after_ok",
              BoolText(result.validation_after_ok));
  AddEvidence(&result, "parser_finality_authority", "false");
  AddEvidence(&result, "client_state_authority", "false");
  AddEvidence(&result, "timestamp_ordering_authority", "false");
  AddEvidence(&result, "uuid_ordering_authority", "false");
  AddEvidence(&result, "crud_event_stream_authority", "false");
  AddEvidence(&result, "cluster_private_implementation", "false");
  if (result.horizon.cleanup_horizon.valid()) {
    AddEvidence(&result,
                "cleanup_horizon_local_transaction_id",
                std::to_string(result.horizon.cleanup_horizon.value));
  }
  AddEvidence(&result,
              "before_delta_ledger_records",
              std::to_string(result.before.delta_ledger_records));
  AddEvidence(&result,
              "before_relevant_delta_records",
              std::to_string(result.before.relevant_delta_records));
  AddEvidence(&result,
              "before_eligible_garbage_records",
              std::to_string(result.before.eligible_garbage_records));
  AddEvidence(&result,
              "after_delta_ledger_records",
              std::to_string(result.after.delta_ledger_records));
  AddEvidence(&result,
              "after_cleaned_garbage_records",
              std::to_string(result.after.cleaned_garbage_records));
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

}  // namespace

IndexGarbageCleanupAgentResult RunIndexGarbageCleanupAgentBatch(
    const IndexGarbageCleanupAgentRequest& request) {
  IndexGarbageCleanupAgentResult result;
  result.cleaned_ledger = request.ledger;
  result.horizon = mga::ComputeAuthoritativeCleanupHorizon(request.horizon_request);
  result.bounded_batch =
      request.max_records_to_scan != 0 && request.max_records_to_clean != 0;

  if (!request.engine_mga_authoritative || !result.horizon.ok()) {
    return Finish(std::move(result),
                  SecondaryIndexGarbageCleanupDecisionKind::refused_non_authoritative,
                  "INDEX_GARBAGE_CLEANUP.NON_AUTHORITATIVE_REFUSAL",
                  "agents.index_garbage_cleanup.non_authoritative_refusal",
                  result.horizon.diagnostic.diagnostic_code.empty()
                      ? "local MGA engine and authoritative cleanup horizon are required"
                      : result.horizon.diagnostic.diagnostic_code,
                  true);
  }

  idx::SecondaryIndexGarbageCleanupRequest cleanup_request;
  cleanup_request.index_uuid = request.index_uuid;
  cleanup_request.table_uuid = request.table_uuid;
  cleanup_request.ledger = request.ledger;
  cleanup_request.base_entries = request.base_entries;
  cleanup_request.table_snapshot = request.table_snapshot;
  cleanup_request.authoritative_cleanup_horizon_local_transaction_id =
      result.horizon.cleanup_horizon.value;
  cleanup_request.cleanup_horizon_authoritative = true;
  cleanup_request.index_kind = request.index_kind;
  cleanup_request.max_records_to_scan = request.max_records_to_scan;
  cleanup_request.max_records_to_clean = request.max_records_to_clean;

  result.cleanup = idx::RunSecondaryIndexGarbageCleanupBatch(cleanup_request);
  result.cleaned_ledger = result.cleanup.cleaned_ledger;
  result.before = result.cleanup.before;
  result.after = result.cleanup.after;
  result.budget_exhausted = result.cleanup.budget_exhausted;
  result.horizon_blocked = result.cleanup.horizon_blocked;
  result.validation_before_ok = result.cleanup.validation_before_ok;
  result.validation_after_ok = result.cleanup.validation_after_ok;
  return Finish(std::move(result),
                result.cleanup.decision,
                result.cleanup.diagnostic.diagnostic_code,
                result.cleanup.diagnostic.message_key,
                DiagnosticDetail(result.cleanup.diagnostic),
                result.cleanup.fail_closed);
}

DiagnosticRecord MakeIndexGarbageCleanupAgentDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.agents.index_garbage_cleanup");
}

// SEARCH_KEY: SB_AGENT_IMPLEMENTATION_index_garbage_cleanup_agent
// DPC_SECONDARY_INDEX_GARBAGE_CLEANUP_AGENT
const char* index_garbage_cleanup_agent_implementation_anchor() {
  return "index_garbage_cleanup_agent";
}

}  // namespace scratchbird::core::agents::implemented_agents
