// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/nosql_family_maintenance_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "nosql/nosql_surface_support.hpp"

#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace agents = scratchbird::core::agents::implemented_agents;

agents::NoSqlFamilyMaintenanceFamily ToAgentFamily(
    EngineNoSqlProviderFamily family) {
  switch (family) {
    case EngineNoSqlProviderFamily::kKeyValue:
      return agents::NoSqlFamilyMaintenanceFamily::key_value;
    case EngineNoSqlProviderFamily::kDocument:
      return agents::NoSqlFamilyMaintenanceFamily::document;
    case EngineNoSqlProviderFamily::kSearch:
      return agents::NoSqlFamilyMaintenanceFamily::search;
    case EngineNoSqlProviderFamily::kVector:
      return agents::NoSqlFamilyMaintenanceFamily::vector;
    case EngineNoSqlProviderFamily::kGraph:
      return agents::NoSqlFamilyMaintenanceFamily::graph;
    case EngineNoSqlProviderFamily::kTimeSeries:
      return agents::NoSqlFamilyMaintenanceFamily::time_series;
    case EngineNoSqlProviderFamily::kSpatial:
    case EngineNoSqlProviderFamily::kColumnar:
    case EngineNoSqlProviderFamily::kUnknown:
      return agents::NoSqlFamilyMaintenanceFamily::unknown;
  }
  return agents::NoSqlFamilyMaintenanceFamily::unknown;
}

agents::NoSqlFamilyMaintenanceAgentRequest ToAgentRequest(
    const EnginePlanNoSqlFamilyMaintenanceRequest& request) {
  agents::NoSqlFamilyMaintenanceAgentRequest agent_request;
  agent_request.horizon_request = request.horizon_request;
  agent_request.scheduler_policy = request.scheduler_policy;
  agent_request.engine_mga_authoritative = request.engine_mga_authoritative;
  agent_request.foreground_work_active = request.foreground_work_active;
  agent_request.now_microseconds = request.now_microseconds;
  agent_request.execute_plan = request.execute_plan;
  for (const auto& candidate : request.candidates) {
    agents::NoSqlFamilyMaintenanceCandidate mapped;
    mapped.family = ToAgentFamily(candidate.family);
    mapped.generation_id = candidate.generation_id;
    mapped.generation_kind = candidate.generation_kind;
    mapped.sealed_local_transaction_id =
        candidate.sealed_local_transaction_id;
    mapped.superseded_local_transaction_id =
        candidate.superseded_local_transaction_id;
    mapped.expires_after_local_transaction_id =
        candidate.expires_after_local_transaction_id;
    mapped.estimated_bytes = candidate.estimated_bytes;
    mapped.generation_evidence_authoritative =
        candidate.generation_evidence_authoritative;
    mapped.ttl_evidence_authoritative = candidate.ttl_evidence_authoritative;
    agent_request.candidates.push_back(std::move(mapped));
  }
  return agent_request;
}

void AddDiagnostic(EnginePlanNoSqlFamilyMaintenanceResult* result) {
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

void AddEvidence(EnginePlanNoSqlFamilyMaintenanceResult* result) {
  AddEngineNoSqlSurfaceEvidence(
      result, "family_maintenance", "mga_horizon_bound_family_maintenance_plan");
  AddApiBehaviorEvidence(result, "behavior_store_scan_selected", "false");
  AddApiBehaviorEvidence(result, "descriptor_scan_selected", "false");
  AddApiBehaviorEvidence(result, "provider_transaction_finality_authority", "false");
  AddApiBehaviorEvidence(result, "provider_visibility_authority", "false");
  AddApiBehaviorEvidence(result, "parser_transaction_finality_authority", "false");
  AddApiBehaviorEvidence(result, "client_autocommit_authority", "false");
  AddApiBehaviorEvidence(result, "row_mga_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "row_security_recheck_evidence", "required");
  for (const auto& field : result->agent_result.evidence) {
    AddApiBehaviorEvidence(result,
                           "nosql_family_maintenance",
                           field.key + "=" + field.value);
  }
  for (const auto& field : result->agent_result.scheduler_result.evidence) {
    AddApiBehaviorEvidence(result,
                           "dynamic_cleanup_debt_scheduler",
                           field.key + "=" + field.value);
  }
}

void AddActionRows(EnginePlanNoSqlFamilyMaintenanceResult* result) {
  for (const auto& action : result->agent_result.actions) {
    AddApiBehaviorRow(
        result,
        {{"row_kind", "maintenance_action"},
         {"family", agents::NoSqlFamilyMaintenanceFamilyName(action.family)},
         {"generation_id", action.generation_id},
         {"action_kind", action.action_kind},
         {"policy_kind", action.policy_kind},
         {"cleanup_horizon_local_transaction_id",
          std::to_string(action.cleanup_horizon_local_transaction_id)},
         {"governing_local_transaction_id",
          std::to_string(action.governing_local_transaction_id)},
         {"executed", action.executed ? "true" : "false"}});
  }
  for (const auto& suppression : result->agent_result.suppressions) {
    AddApiBehaviorRow(
        result,
        {{"row_kind", "maintenance_suppression"},
         {"family", agents::NoSqlFamilyMaintenanceFamilyName(suppression.family)},
         {"generation_id", suppression.generation_id},
         {"diagnostic_code", suppression.diagnostic_code},
         {"cleanup_horizon_local_transaction_id",
          std::to_string(suppression.cleanup_horizon_local_transaction_id)},
         {"governing_local_transaction_id",
          std::to_string(suppression.governing_local_transaction_id)}});
  }
  for (const auto& decision : result->agent_result.scheduler_result.decisions) {
    AddApiBehaviorRow(
        result,
        {{"row_kind", "dynamic_cleanup_assignment"},
         {"cleanup_debt_family",
          scratchbird::core::agents::DynamicCleanupDebtFamilyName(
              decision.source.family)},
         {"cleanup_debt_work_kind",
          scratchbird::core::agents::DynamicCleanupDebtWorkKindName(
              decision.source.work_kind)},
         {"stable_work_key", decision.source.stable_work_key},
         {"decision",
          scratchbird::core::agents::DynamicCleanupDebtDecisionKindName(
              decision.decision)},
         {"scheduled_work_units",
          std::to_string(decision.scheduled_work_units)},
         {"diagnostic_code", decision.diagnostic_code}});
  }
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_FAMILY_MAINTENANCE_API_BEHAVIOR
EnginePlanNoSqlFamilyMaintenanceResult EnginePlanNoSqlFamilyMaintenance(
    const EnginePlanNoSqlFamilyMaintenanceRequest& request) {
  constexpr const char* kOperation = "nosql.family_maintenance_plan";
  if (!request.context.cluster_authority_available &&
      EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<
        EnginePlanNoSqlFamilyMaintenanceResult>(request, kOperation);
  }
  EnginePlanNoSqlFamilyMaintenanceResult result;
  result.operation_id = kOperation;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.dml_summary.benchmark_clean = true;
  result.dml_summary.visible_rows_scanned = 0;
  result.agent_result = agents::RunNoSqlFamilyMaintenanceAgent(
      ToAgentRequest(request));
  result.ok = result.agent_result.ok();
  result.dml_summary.rows_changed =
      request.execute_plan ? result.agent_result.actions.size() : 0;
  AddEvidence(&result);
  AddActionRows(&result);
  AddDiagnostic(&result);
  return result;
}

}  // namespace scratchbird::engine::internal_api
