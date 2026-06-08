// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agents/nosql_family_maintenance_agent.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <utility>

namespace scratchbird::core::agents::implemented_agents {
namespace {

namespace mga = scratchbird::transaction::mga;
namespace scheduler = scratchbird::core::agents;

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

void AddEvidence(NoSqlFamilyMaintenanceAgentResult* result,
                 std::string key,
                 std::string value) {
  result->evidence.push_back({std::move(key), std::move(value)});
}

bool IsSupportedFamily(NoSqlFamilyMaintenanceFamily family) {
  return family != NoSqlFamilyMaintenanceFamily::unknown;
}

u64 GenerationRetirementLocalId(const NoSqlFamilyMaintenanceCandidate& candidate) {
  return std::max(candidate.sealed_local_transaction_id,
                  candidate.superseded_local_transaction_id);
}

std::string FamilyGenerationActionKind(NoSqlFamilyMaintenanceFamily family) {
  switch (family) {
    case NoSqlFamilyMaintenanceFamily::key_value:
      return "kv_lsm_generation_compaction";
    case NoSqlFamilyMaintenanceFamily::document:
      return "document_shape_fragment_generation_merge";
    case NoSqlFamilyMaintenanceFamily::search:
      return "search_segment_merge_and_retirement";
    case NoSqlFamilyMaintenanceFamily::vector:
      return "vector_index_generation_retirement";
    case NoSqlFamilyMaintenanceFamily::graph:
      return "graph_adjacency_generation_compaction";
    case NoSqlFamilyMaintenanceFamily::time_series:
      return "time_series_bucket_generation_merge_retirement";
    case NoSqlFamilyMaintenanceFamily::unknown:
      return "unsupported";
  }
  return "unsupported";
}

scheduler::DynamicCleanupDebtFamily SchedulerFamily(
    NoSqlFamilyMaintenanceFamily family) {
  switch (family) {
    case NoSqlFamilyMaintenanceFamily::key_value:
      return scheduler::DynamicCleanupDebtFamily::nosql_key_value;
    case NoSqlFamilyMaintenanceFamily::document:
      return scheduler::DynamicCleanupDebtFamily::nosql_document;
    case NoSqlFamilyMaintenanceFamily::search:
      return scheduler::DynamicCleanupDebtFamily::nosql_search;
    case NoSqlFamilyMaintenanceFamily::vector:
      return scheduler::DynamicCleanupDebtFamily::nosql_vector;
    case NoSqlFamilyMaintenanceFamily::graph:
      return scheduler::DynamicCleanupDebtFamily::nosql_graph;
    case NoSqlFamilyMaintenanceFamily::time_series:
      return scheduler::DynamicCleanupDebtFamily::nosql_time_series;
    case NoSqlFamilyMaintenanceFamily::unknown:
      return scheduler::DynamicCleanupDebtFamily::nosql_key_value;
  }
  return scheduler::DynamicCleanupDebtFamily::nosql_key_value;
}

scheduler::DynamicCleanupDebtWorkKind SchedulerWorkKind(
    NoSqlFamilyMaintenanceFamily family,
    bool ttl_retirement) {
  if (ttl_retirement) {
    return scheduler::DynamicCleanupDebtWorkKind::nosql_key_value_ttl_retirement;
  }
  switch (family) {
    case NoSqlFamilyMaintenanceFamily::key_value:
      return scheduler::DynamicCleanupDebtWorkKind::nosql_key_value_generation_compaction;
    case NoSqlFamilyMaintenanceFamily::document:
      return scheduler::DynamicCleanupDebtWorkKind::nosql_document_generation_merge;
    case NoSqlFamilyMaintenanceFamily::search:
      return scheduler::DynamicCleanupDebtWorkKind::nosql_search_segment_merge;
    case NoSqlFamilyMaintenanceFamily::vector:
      return scheduler::DynamicCleanupDebtWorkKind::nosql_vector_generation_retirement;
    case NoSqlFamilyMaintenanceFamily::graph:
      return scheduler::DynamicCleanupDebtWorkKind::nosql_graph_adjacency_compaction;
    case NoSqlFamilyMaintenanceFamily::time_series:
      return scheduler::DynamicCleanupDebtWorkKind::nosql_time_series_bucket_retirement;
    case NoSqlFamilyMaintenanceFamily::unknown:
      return scheduler::DynamicCleanupDebtWorkKind::nosql_key_value_ttl_retirement;
  }
  return scheduler::DynamicCleanupDebtWorkKind::nosql_key_value_ttl_retirement;
}

struct PendingActionSpec {
  NoSqlFamilyMaintenanceFamily family = NoSqlFamilyMaintenanceFamily::unknown;
  std::string generation_id;
  std::string action_kind;
  std::string policy_kind;
  u64 governing_local_transaction_id = 0;
  u64 estimated_bytes = 0;
};

std::string StableWorkKey(const PendingActionSpec& spec) {
  return std::string("nosql:") + NoSqlFamilyMaintenanceFamilyName(spec.family) +
         ":" + spec.generation_id + ":" + spec.action_kind;
}

scheduler::DynamicCleanupDebtSource MakeSchedulerSource(
    const PendingActionSpec& spec,
    u64 cleanup_horizon) {
  scheduler::DynamicCleanupDebtSource source;
  source.family = SchedulerFamily(spec.family);
  source.work_kind = SchedulerWorkKind(
      spec.family, spec.action_kind == "kv_ttl_expired_record_retirement");
  source.failure_mode =
      scheduler::DynamicCleanupDebtFailureMode::fail_closed_retain_debt;
  source.stable_work_key = StableWorkKey(spec);
  source.object_uuid = spec.generation_id;
  source.source_detail = spec.policy_kind;
  source.debt_units = 1;
  source.debt_bytes = spec.estimated_bytes;
  source.estimated_work_units = 1;
  source.max_work_units = 1;
  source.cleanup_horizon_local_transaction_id = cleanup_horizon;
  source.source_authoritative = true;
  source.requires_mga_cleanup_horizon = true;
  source.destructive_cleanup = true;
  source.recovery_proof_available = true;
  source.bounded_cleanup_available = true;
  source.evidence.push_back(
      {"nosql_family", NoSqlFamilyMaintenanceFamilyName(spec.family)});
  source.evidence.push_back({"nosql_action_kind", spec.action_kind});
  source.evidence.push_back({"nosql_policy_kind", spec.policy_kind});
  source.evidence.push_back({"generation_id", spec.generation_id});
  source.evidence.push_back(
      {"governing_local_transaction_id",
       std::to_string(spec.governing_local_transaction_id)});
  return source;
}

void AddAction(NoSqlFamilyMaintenanceAgentResult* result,
               const PendingActionSpec& spec,
               u64 cleanup_horizon,
               bool executed) {
  NoSqlFamilyMaintenanceAction action;
  action.family = spec.family;
  action.generation_id = spec.generation_id;
  action.action_kind = spec.action_kind;
  action.policy_kind = spec.policy_kind;
  action.cleanup_horizon_local_transaction_id = cleanup_horizon;
  action.governing_local_transaction_id = spec.governing_local_transaction_id;
  action.estimated_bytes = spec.estimated_bytes;
  action.executed = executed;
  result->actions.push_back(std::move(action));
}

void AddSuppressionFromSpec(NoSqlFamilyMaintenanceAgentResult* result,
                            const PendingActionSpec& spec,
                            std::string diagnostic_code,
                            u64 cleanup_horizon) {
  NoSqlFamilyMaintenanceSuppression suppression;
  suppression.family = spec.family;
  suppression.generation_id = spec.generation_id;
  suppression.diagnostic_code = std::move(diagnostic_code);
  suppression.cleanup_horizon_local_transaction_id = cleanup_horizon;
  suppression.governing_local_transaction_id =
      spec.governing_local_transaction_id;
  result->suppressions.push_back(std::move(suppression));
}

void AddSuppression(NoSqlFamilyMaintenanceAgentResult* result,
                    const NoSqlFamilyMaintenanceCandidate& candidate,
                    std::string diagnostic_code,
                    u64 cleanup_horizon,
                    u64 governing_local_transaction_id) {
  NoSqlFamilyMaintenanceSuppression suppression;
  suppression.family = candidate.family;
  suppression.generation_id = candidate.generation_id;
  suppression.diagnostic_code = std::move(diagnostic_code);
  suppression.cleanup_horizon_local_transaction_id = cleanup_horizon;
  suppression.governing_local_transaction_id = governing_local_transaction_id;
  result->suppressions.push_back(std::move(suppression));
}

NoSqlFamilyMaintenanceAgentResult Finish(
    NoSqlFamilyMaintenanceAgentResult result,
    NoSqlFamilyMaintenanceDecisionKind decision,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail,
    bool fail_closed) {
  result.status = fail_closed ? AgentErrorStatus() : AgentOkStatus();
  result.decision = decision;
  result.fail_closed = fail_closed;
  result.diagnostic = MakeNoSqlFamilyMaintenanceAgentDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      std::move(detail));
  AddEvidence(&result,
              "nosql_family_maintenance_agent",
              "odf077_family_maintenance_v1");
  AddEvidence(&result, "authority_source", "durable_mga_transaction_inventory");
  AddEvidence(&result, "decision", NoSqlFamilyMaintenanceDecisionKindName(decision));
  AddEvidence(&result, "fail_closed", BoolText(fail_closed));
  AddEvidence(&result, "action_count", std::to_string(result.actions.size()));
  AddEvidence(&result, "suppression_count", std::to_string(result.suppressions.size()));
  AddEvidence(&result,
              "dynamic_cleanup_scheduled_count",
              std::to_string(result.scheduler_result.scheduled_count));
  if (result.horizon.cleanup_horizon.valid()) {
    AddEvidence(&result,
                "cleanup_horizon_local_transaction_id",
                std::to_string(result.horizon.cleanup_horizon.value));
  }
  AddEvidence(&result, "diagnostic_code", result.diagnostic.diagnostic_code);
  return result;
}

NoSqlFamilyMaintenanceAgentResult Fail(
    NoSqlFamilyMaintenanceAgentResult result,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  return Finish(std::move(result),
                NoSqlFamilyMaintenanceDecisionKind::refused_non_authoritative,
                std::move(diagnostic_code),
                std::move(message_key),
                std::move(detail),
                true);
}

}  // namespace

const char* NoSqlFamilyMaintenanceFamilyName(
    NoSqlFamilyMaintenanceFamily family) {
  switch (family) {
    case NoSqlFamilyMaintenanceFamily::key_value:
      return "key_value";
    case NoSqlFamilyMaintenanceFamily::document:
      return "document";
    case NoSqlFamilyMaintenanceFamily::search:
      return "search";
    case NoSqlFamilyMaintenanceFamily::vector:
      return "vector";
    case NoSqlFamilyMaintenanceFamily::graph:
      return "graph";
    case NoSqlFamilyMaintenanceFamily::time_series:
      return "time_series";
    case NoSqlFamilyMaintenanceFamily::unknown:
      return "unknown";
  }
  return "unknown";
}

const char* NoSqlFamilyMaintenanceDecisionKindName(
    NoSqlFamilyMaintenanceDecisionKind decision) {
  switch (decision) {
    case NoSqlFamilyMaintenanceDecisionKind::planned:
      return "planned";
    case NoSqlFamilyMaintenanceDecisionKind::executed:
      return "executed";
    case NoSqlFamilyMaintenanceDecisionKind::no_op:
      return "no_op";
    case NoSqlFamilyMaintenanceDecisionKind::suppressed_by_mga_horizon:
      return "suppressed_by_mga_horizon";
    case NoSqlFamilyMaintenanceDecisionKind::refused_non_authoritative:
      return "refused_non_authoritative";
    case NoSqlFamilyMaintenanceDecisionKind::refused:
      return "refused";
  }
  return "refused";
}

NoSqlFamilyMaintenanceAgentResult RunNoSqlFamilyMaintenanceAgent(
    const NoSqlFamilyMaintenanceAgentRequest& request) {
  NoSqlFamilyMaintenanceAgentResult result;
  result.horizon = mga::ComputeAuthoritativeCleanupHorizon(request.horizon_request);

  if (!request.engine_mga_authoritative) {
    return Fail(std::move(result),
                kNoSqlMaintenanceCleanupHorizonNotAuthoritative,
                "agents.nosql_family_maintenance.cleanup_horizon_not_authoritative",
                "engine MGA authority is required for NoSQL maintenance");
  }
  if (!result.horizon.ok()) {
    const auto diagnostic = result.horizon.diagnostic.diagnostic_code.empty()
                                ? kNoSqlMaintenanceCleanupHorizonNotAuthoritative
                                : result.horizon.diagnostic.diagnostic_code;
    return Fail(std::move(result),
                diagnostic,
                "agents.nosql_family_maintenance.cleanup_horizon_refused",
                "authoritative MGA cleanup horizon is required");
  }

  const u64 cleanup_horizon = result.horizon.cleanup_horizon.value;
  std::set<NoSqlFamilyMaintenanceFamily> families_seen;
  std::map<std::string, PendingActionSpec> pending_actions;
  std::vector<scheduler::DynamicCleanupDebtSource> scheduler_sources;

  for (const auto& candidate : request.candidates) {
    families_seen.insert(candidate.family);
    AddEvidence(&result,
                std::string("candidate_family:") +
                    NoSqlFamilyMaintenanceFamilyName(candidate.family),
                candidate.generation_id);

    if (!IsSupportedFamily(candidate.family)) {
      return Finish(std::move(result),
                    NoSqlFamilyMaintenanceDecisionKind::refused,
                    kNoSqlMaintenanceUnsupportedFamily,
                    "agents.nosql_family_maintenance.unsupported_family",
                    candidate.generation_id,
                    true);
    }
    if (!candidate.generation_evidence_authoritative) {
      return Finish(std::move(result),
                    NoSqlFamilyMaintenanceDecisionKind::refused_non_authoritative,
                    kNoSqlMaintenanceGenerationEvidenceNotAuthoritative,
                    "agents.nosql_family_maintenance.generation_evidence_not_authoritative",
                    candidate.generation_id,
                    true);
    }

    if (candidate.family == NoSqlFamilyMaintenanceFamily::key_value &&
        candidate.expires_after_local_transaction_id != 0) {
      if (!candidate.ttl_evidence_authoritative) {
        return Finish(std::move(result),
                      NoSqlFamilyMaintenanceDecisionKind::refused_non_authoritative,
                      kNoSqlMaintenanceTtlEvidenceNotAuthoritative,
                      "agents.nosql_family_maintenance.ttl_evidence_not_authoritative",
                      candidate.generation_id,
                      true);
      }
      if (candidate.expires_after_local_transaction_id < cleanup_horizon) {
        PendingActionSpec spec;
        spec.family = candidate.family;
        spec.generation_id = candidate.generation_id;
        spec.action_kind = "kv_ttl_expired_record_retirement";
        spec.policy_kind = "ttl_retirement_below_mga_cleanup_horizon";
        spec.governing_local_transaction_id =
            candidate.expires_after_local_transaction_id;
        spec.estimated_bytes = candidate.estimated_bytes;
        const auto stable_key = StableWorkKey(spec);
        scheduler_sources.push_back(MakeSchedulerSource(spec, cleanup_horizon));
        pending_actions.emplace(stable_key, std::move(spec));
      } else {
        AddSuppression(&result,
                       candidate,
                       kNoSqlMaintenanceTtlNotBelowMgaHorizon,
                       cleanup_horizon,
                       candidate.expires_after_local_transaction_id);
      }
    }

    const u64 retirement_local_id = GenerationRetirementLocalId(candidate);
    if (retirement_local_id == 0) {
      continue;
    }
    if (retirement_local_id < cleanup_horizon) {
      PendingActionSpec spec;
      spec.family = candidate.family;
      spec.generation_id = candidate.generation_id;
      spec.action_kind = FamilyGenerationActionKind(candidate.family);
      spec.policy_kind = "generation_retirement_below_mga_cleanup_horizon";
      spec.governing_local_transaction_id = retirement_local_id;
      spec.estimated_bytes = candidate.estimated_bytes;
      const auto stable_key = StableWorkKey(spec);
      scheduler_sources.push_back(MakeSchedulerSource(spec, cleanup_horizon));
      pending_actions.emplace(stable_key, std::move(spec));
    } else {
      AddSuppression(&result,
                     candidate,
                     kNoSqlMaintenanceGenerationNotBelowMgaHorizon,
                     cleanup_horizon,
                     retirement_local_id);
    }
  }

  AddEvidence(&result, "family_count", std::to_string(families_seen.size()));

  if (!scheduler_sources.empty()) {
    scheduler::DynamicCleanupDebtSchedulerRequest scheduler_request;
    scheduler_request.policy = request.scheduler_policy;
    scheduler_request.cleanup_horizon = result.horizon;
    scheduler_request.sources = std::move(scheduler_sources);
    scheduler_request.now_microseconds = request.now_microseconds;
    scheduler_request.engine_mga_authoritative = request.engine_mga_authoritative;
    scheduler_request.foreground_work_active = request.foreground_work_active;
    result.scheduler_result = scheduler::PlanDynamicCleanupDebt(scheduler_request);

    for (const auto& decision : result.scheduler_result.decisions) {
      const auto found = pending_actions.find(decision.source.stable_work_key);
      if (found == pending_actions.end()) {
        continue;
      }
      if (decision.scheduled()) {
        AddAction(&result,
                  found->second,
                  cleanup_horizon,
                  request.execute_plan);
      } else {
        AddSuppressionFromSpec(&result,
                               found->second,
                               decision.diagnostic_code,
                               cleanup_horizon);
      }
    }
  }

  if (!result.actions.empty()) {
    return Finish(std::move(result),
                  request.execute_plan ? NoSqlFamilyMaintenanceDecisionKind::executed
                                       : NoSqlFamilyMaintenanceDecisionKind::planned,
                  request.execute_plan ? kNoSqlMaintenanceExecuted
                                       : kNoSqlMaintenancePlanned,
                  request.execute_plan
                      ? "agents.nosql_family_maintenance.executed"
                      : "agents.nosql_family_maintenance.planned",
                  std::to_string(result.actions.size()),
                  false);
  }
  if (!result.suppressions.empty()) {
    return Finish(std::move(result),
                  NoSqlFamilyMaintenanceDecisionKind::suppressed_by_mga_horizon,
                  result.suppressions.front().diagnostic_code,
                  "agents.nosql_family_maintenance.suppressed_by_mga_horizon",
                  std::to_string(result.suppressions.size()),
                  false);
  }
  return Finish(std::move(result),
                NoSqlFamilyMaintenanceDecisionKind::no_op,
                kNoSqlMaintenanceNoCandidateWork,
                "agents.nosql_family_maintenance.no_candidate_work",
                "no candidate generations were eligible",
                false);
}

DiagnosticRecord MakeNoSqlFamilyMaintenanceAgentDiagnostic(
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
                        "core.agents.nosql_family_maintenance");
}

// SEARCH_KEY: SB_AGENT_IMPLEMENTATION_nosql_family_maintenance_agent
const char* nosql_family_maintenance_agent_implementation_anchor() {
  return "nosql_family_maintenance_agent";
}

}  // namespace scratchbird::core::agents::implemented_agents
