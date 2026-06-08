// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/nosql_backpressure_debt_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "nosql/nosql_surface_support.hpp"

#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents::implemented_agents;

agents::NoSqlBackpressureDebtFamily ToAgentFamily(
    EngineNoSqlProviderFamily family) {
  switch (family) {
    case EngineNoSqlProviderFamily::kKeyValue:
      return agents::NoSqlBackpressureDebtFamily::key_value;
    case EngineNoSqlProviderFamily::kDocument:
      return agents::NoSqlBackpressureDebtFamily::document;
    case EngineNoSqlProviderFamily::kSearch:
      return agents::NoSqlBackpressureDebtFamily::search;
    case EngineNoSqlProviderFamily::kVector:
      return agents::NoSqlBackpressureDebtFamily::vector;
    case EngineNoSqlProviderFamily::kGraph:
      return agents::NoSqlBackpressureDebtFamily::graph;
    case EngineNoSqlProviderFamily::kTimeSeries:
      return agents::NoSqlBackpressureDebtFamily::time_series;
    case EngineNoSqlProviderFamily::kSpatial:
    case EngineNoSqlProviderFamily::kColumnar:
    case EngineNoSqlProviderFamily::kUnknown:
      return agents::NoSqlBackpressureDebtFamily::unknown;
  }
  return agents::NoSqlBackpressureDebtFamily::unknown;
}

agents::NoSqlBackpressureDebtKind ToAgentKind(
    EngineNoSqlBackpressureDebtKind kind) {
  switch (kind) {
    case EngineNoSqlBackpressureDebtKind::kRefresh:
      return agents::NoSqlBackpressureDebtKind::refresh;
    case EngineNoSqlBackpressureDebtKind::kMergeCompaction:
      return agents::NoSqlBackpressureDebtKind::merge_compaction;
    case EngineNoSqlBackpressureDebtKind::kGenerationBuild:
      return agents::NoSqlBackpressureDebtKind::generation_build;
    case EngineNoSqlBackpressureDebtKind::kPayloadPolicy:
      return agents::NoSqlBackpressureDebtKind::payload_policy;
    case EngineNoSqlBackpressureDebtKind::kSlowdownBackpressurePolicy:
      return agents::NoSqlBackpressureDebtKind::slowdown_backpressure_policy;
    case EngineNoSqlBackpressureDebtKind::kStrictBulkRedirectPolicy:
      return agents::NoSqlBackpressureDebtKind::strict_bulk_redirect_policy;
    case EngineNoSqlBackpressureDebtKind::kResultSuppressionPolicy:
      return agents::NoSqlBackpressureDebtKind::result_suppression_policy;
    case EngineNoSqlBackpressureDebtKind::kUnknown:
      return agents::NoSqlBackpressureDebtKind::unknown;
  }
  return agents::NoSqlBackpressureDebtKind::unknown;
}

agents::NoSqlBackpressureDebtAgentRequest ToAgentRequest(
    const EnginePlanNoSqlBackpressureDebtRequest& request) {
  agents::NoSqlBackpressureDebtAgentRequest agent_request;
  agent_request.now_microseconds = request.now_microseconds;
  agent_request.authority.engine_mga_authoritative =
      request.engine_mga_authoritative;
  agent_request.authority.request_context_authoritative =
      request.request_context_authoritative;
  agent_request.authority.security_snapshot_bound =
      request.security_snapshot_bound;
  agent_request.authority.grants_proven = request.grants_proven;
  agent_request.authority.row_mga_recheck_required =
      request.row_mga_recheck_required;
  agent_request.authority.row_security_recheck_required =
      request.row_security_recheck_required;
  agent_request.authority.parser_or_donor_authority =
      request.parser_or_donor_authority;
  agent_request.authority.provider_transaction_finality_authority =
      request.provider_claims_transaction_finality_authority;
  agent_request.authority.provider_visibility_authority =
      request.provider_claims_visibility_authority;
  agent_request.authority.client_autocommit_authority =
      request.client_claims_autocommit_authority;
  agent_request.authority.wal_recovery_authority =  // wal-not-authority
      request.write_ahead_log_claims_recovery_authority;  // wal-not-authority

  for (const auto& entry : request.entries) {
    agents::NoSqlBackpressureDebtEntry mapped;
    mapped.family = ToAgentFamily(entry.family);
    mapped.debt_kind = ToAgentKind(entry.debt_kind);
    mapped.object_uuid = entry.object_uuid;
    mapped.result_id = entry.result_id;
    mapped.evidence_epoch = entry.evidence_epoch;
    mapped.required_epoch = entry.required_epoch;
    mapped.debt_units = entry.debt_units;
    mapped.observed_cost_units = entry.observed_cost_units;
    mapped.budget_cost_units = entry.budget_cost_units;
    mapped.evidence_authoritative = entry.evidence_authoritative;
    mapped.stale_result = entry.stale_result;
    mapped.over_budget_result = entry.over_budget_result;
    mapped.unsafe_result = entry.unsafe_result;
    agent_request.entries.push_back(std::move(mapped));
  }
  return agent_request;
}

void AddDiagnostic(EnginePlanNoSqlBackpressureDebtResult* result) {
  if (result->agent_result.diagnostic.diagnostic_code.empty()) {
    return;
  }
  EngineApiDiagnostic diagnostic;
  diagnostic.code = result->agent_result.diagnostic.diagnostic_code;
  diagnostic.message_key = result->agent_result.diagnostic.message_key;
  diagnostic.error = result->agent_result.fail_closed;
  for (const auto& argument : result->agent_result.diagnostic.arguments) {
    if (argument.key == "detail") {
      diagnostic.detail = argument.value;
      break;
    }
  }
  result->diagnostics.push_back(std::move(diagnostic));
}

void AddSuppressionDiagnostics(EnginePlanNoSqlBackpressureDebtResult* result) {
  for (const auto& suppression : result->agent_result.suppressions) {
    EngineApiDiagnostic diagnostic;
    diagnostic.code = suppression.diagnostic_code;
    diagnostic.message_key =
        "nosql.backpressure_debt.result_suppressed";
    diagnostic.detail = suppression.diagnostic_detail;
    diagnostic.error = false;
    result->diagnostics.push_back(std::move(diagnostic));
  }
}

void AddEvidence(EnginePlanNoSqlBackpressureDebtResult* result) {
  AddEngineNoSqlSurfaceEvidence(
      result,
      "backpressure_debt",
      "family_debt_ledger_backpressure_and_result_suppression_plan");
  AddApiBehaviorEvidence(result, "behavior_store_scan_selected", "false");
  AddApiBehaviorEvidence(result, "descriptor_scan_selected", "false");
  AddApiBehaviorEvidence(result,
                         "provider_transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(result, "provider_visibility_authority", "false");
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
  AddApiBehaviorEvidence(result, "client_autocommit_authority", "false");
  AddApiBehaviorEvidence(result, "row_mga_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "row_security_recheck_evidence", "required");
  for (const auto& field : result->agent_result.evidence) {
    AddApiBehaviorEvidence(result,
                           "nosql_backpressure_debt",
                           field.key + "=" + field.value);
  }
}

void AddRows(EnginePlanNoSqlBackpressureDebtResult* result) {
  for (const auto& row : result->agent_result.ledger_rows) {
    AddApiBehaviorRow(
        result,
        {{"row_kind", "debt_ledger"},
         {"family", agents::NoSqlBackpressureDebtFamilyName(row.family)},
         {"debt_kind", agents::NoSqlBackpressureDebtKindName(row.debt_kind)},
         {"object_uuid", row.object_uuid},
         {"result_id", row.result_id},
         {"evidence_epoch", std::to_string(row.evidence_epoch)},
         {"required_epoch", std::to_string(row.required_epoch)},
         {"debt_units", std::to_string(row.debt_units)},
         {"observed_cost_units", std::to_string(row.observed_cost_units)},
         {"budget_cost_units", std::to_string(row.budget_cost_units)},
         {"stale_result", row.stale_result ? "true" : "false"},
         {"over_budget_result", row.over_budget_result ? "true" : "false"},
         {"unsafe_result", row.unsafe_result ? "true" : "false"}});
  }
  for (const auto& action : result->agent_result.actions) {
    AddApiBehaviorRow(
        result,
        {{"row_kind", "debt_plan_action"},
         {"family", agents::NoSqlBackpressureDebtFamilyName(action.family)},
         {"debt_kind",
          agents::NoSqlBackpressureDebtKindName(action.debt_kind)},
         {"object_uuid", action.object_uuid},
         {"action_kind", action.action_kind},
         {"policy_kind", action.policy_kind},
         {"planned_work_units", std::to_string(action.planned_work_units)}});
  }
  for (const auto& suppression : result->agent_result.suppressions) {
    AddApiBehaviorRow(
        result,
        {{"row_kind", "result_suppression"},
         {"family",
          agents::NoSqlBackpressureDebtFamilyName(suppression.family)},
         {"object_uuid", suppression.object_uuid},
         {"result_id", suppression.result_id},
         {"diagnostic_code", suppression.diagnostic_code},
         {"diagnostic_detail", suppression.diagnostic_detail},
         {"evidence_epoch", std::to_string(suppression.evidence_epoch)},
         {"required_epoch", std::to_string(suppression.required_epoch)},
         {"observed_cost_units",
          std::to_string(suppression.observed_cost_units)},
         {"budget_cost_units",
          std::to_string(suppression.budget_cost_units)},
         {"stale_result", suppression.stale_result ? "true" : "false"},
         {"over_budget_result",
          suppression.over_budget_result ? "true" : "false"},
         {"unsafe_result", suppression.unsafe_result ? "true" : "false"},
         {"result_returned", "false"}});
  }
}

}  // namespace

const char* EngineNoSqlBackpressureDebtKindName(
    EngineNoSqlBackpressureDebtKind kind) {
  switch (kind) {
    case EngineNoSqlBackpressureDebtKind::kRefresh:
      return "refresh";
    case EngineNoSqlBackpressureDebtKind::kMergeCompaction:
      return "merge_compaction";
    case EngineNoSqlBackpressureDebtKind::kGenerationBuild:
      return "generation_build";
    case EngineNoSqlBackpressureDebtKind::kPayloadPolicy:
      return "payload_policy";
    case EngineNoSqlBackpressureDebtKind::kSlowdownBackpressurePolicy:
      return "slowdown_backpressure_policy";
    case EngineNoSqlBackpressureDebtKind::kStrictBulkRedirectPolicy:
      return "strict_bulk_redirect_policy";
    case EngineNoSqlBackpressureDebtKind::kResultSuppressionPolicy:
      return "result_suppression_policy";
    case EngineNoSqlBackpressureDebtKind::kUnknown:
      return "unknown";
  }
  return "unknown";
}

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_BACKPRESSURE_DEBT_API_BEHAVIOR
EnginePlanNoSqlBackpressureDebtResult EnginePlanNoSqlBackpressureDebt(
    const EnginePlanNoSqlBackpressureDebtRequest& request) {
  constexpr const char* kOperation = "nosql.backpressure_debt_plan";
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EnginePlanNoSqlBackpressureDebtResult>(
        request.context,
        kOperation,
        MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (!request.context.cluster_authority_available &&
      EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<
        EnginePlanNoSqlBackpressureDebtResult>(request, kOperation);
  }

  EnginePlanNoSqlBackpressureDebtResult result;
  result.operation_id = kOperation;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.dml_summary.benchmark_clean = true;
  result.dml_summary.visible_rows_scanned = 0;
  result.agent_result = agents::RunNoSqlBackpressureDebtAgent(
      ToAgentRequest(request));
  result.ok = result.agent_result.ok();
  result.dml_summary.index_probes =
      static_cast<EngineApiU64>(result.agent_result.ledger_rows.size());
  AddEvidence(&result);
  AddRows(&result);
  AddDiagnostic(&result);
  AddSuppressionDiagnostics(&result);
  return result;
}

}  // namespace scratchbird::engine::internal_api
