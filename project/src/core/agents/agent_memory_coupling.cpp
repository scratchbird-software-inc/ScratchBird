// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_memory_coupling.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents {
namespace {

void Add(std::vector<std::string>* evidence, std::string item) {
  evidence->push_back(std::move(item));
}

std::string BoolText(bool value) { return value ? "true" : "false"; }

AgentRuntimeStatus MemoryOk(std::string code, std::string detail = {}) {
  return {true, std::move(code), std::move(detail)};
}

AgentRuntimeStatus MemoryError(std::string code, std::string detail = {}) {
  return {false, std::move(code), std::move(detail)};
}

bool HasMemoryActionContract(const std::string& agent_type_id) {
  for (const auto& action_id : CanonicalAgentAllowedActionIds(agent_type_id)) {
    const auto contract = FindAgentActionContract(agent_type_id, action_id);
    if (!contract.has_value()) {
      continue;
    }
    if (contract->actuator == "memory_allocator" ||
        contract->actuator == "executor_memory" ||
        contract->actuator == "cache_manager" ||
        contract->policy_gate.find("memory_") != std::string::npos) {
      return true;
    }
    for (const auto& family : contract->metric_families) {
      if (family.rfind("sb_memory_", 0) == 0) {
        return true;
      }
    }
  }
  return false;
}

void AppendBudgetEvidence(AgentMemoryReservationResult* result,
                          const AgentMemoryReservationRequest& request) {
  Add(&result->evidence, "MMCH_AGENT_MEMORY_BUDGET_INTEGRATION");
  Add(&result->evidence, "agent_memory_budget.agent_type_id=" +
                             request.agent_type_id);
  Add(&result->evidence, "agent_memory_budget.action_id=" + request.action_id);
  Add(&result->evidence, "agent_memory_budget.operation_id=" +
                             request.operation_id);
  Add(&result->evidence,
      "agent_memory_budget.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");
}

AgentMemoryReservationResult ReservationFailure(
    const AgentMemoryReservationRequest& request,
    std::string code,
    std::string detail) {
  AgentMemoryReservationResult result;
  AppendBudgetEvidence(&result, request);
  result.status = MemoryError(std::move(code), std::move(detail));
  result.ok = false;
  result.fail_closed = true;
  Add(&result.evidence, "agent_memory_budget.fail_closed=true");
  Add(&result.evidence,
      "agent_memory_budget.diagnostic_code=" + result.status.diagnostic_code);
  return result;
}

bool ApprovedPressureAction(AgentMemoryPressureActionKind action_kind) {
  switch (action_kind) {
    case AgentMemoryPressureActionKind::page_cache_shrink:
    case AgentMemoryPressureActionKind::spill_cleanup:
    case AgentMemoryPressureActionKind::temp_cleanup:
    case AgentMemoryPressureActionKind::background_reclamation:
      return true;
  }
  return false;
}

AgentMemoryPressureActionBoundaryDecision BoundaryFailure(
    const AgentMemoryPressureActionBoundaryRequest& request,
    std::string code,
    std::string detail) {
  AgentMemoryPressureActionBoundaryDecision decision;
  decision.status = MemoryError(std::move(code), std::move(detail));
  Add(&decision.evidence, "MMCH_AGENT_MEMORY_PRESSURE_ACTION_BOUNDARY");
  Add(&decision.evidence,
      "agent_memory_action.agent_type_id=" + request.agent_type_id);
  Add(&decision.evidence,
      "agent_memory_action.action_id=" + request.action_id);
  Add(&decision.evidence,
      "agent_memory_action.kind=" +
          std::string(AgentMemoryPressureActionKindName(request.action_kind)));
  Add(&decision.evidence,
      "agent_memory_action.fail_closed=true");
  Add(&decision.evidence,
      "agent_memory_action.diagnostic_code=" + decision.status.diagnostic_code);
  Add(&decision.evidence,
      "agent_memory_action.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");
  return decision;
}

std::uint64_t EstimateEvidenceOverhead(
    const AgentMemoryEvidenceBundleRequest& request) {
  std::uint64_t total = 0;
  for (const auto& field : request.fields) {
    total += static_cast<std::uint64_t>(field.key.size() + field.value.size());
    total += 64;
  }
  return total;
}

std::string EvidenceDigest(const std::string& seed) {
  return DeterministicAgentRuntimeObjectUuidFromKey("agent_memory_digest|" + seed);
}

bool AuthorityClean(const AgentSystemProfileForbiddenAuthority& authority) {
  return !authority.transaction_finality_authority &&
         !authority.visibility_authority &&
         !authority.authorization_security_authority &&
         !authority.recovery_authority &&
         !authority.parser_authority &&
         !authority.donor_authority &&
         !authority.wal_authority &&
         !authority.benchmark_authority &&
         !authority.optimizer_plan_authority &&
         !authority.index_finality_authority &&
         !authority.provider_finality_authority &&
         !authority.cluster_authority &&
         !authority.memory_authority &&
         !authority.agent_action_authority;
}

bool PressureHasAction(
    const scratchbird::core::memory::MemoryPressureDecision& pressure,
    scratchbird::core::memory::MemoryPressureActionKind action) {
  return pressure.HasAction(action);
}

bool PressureHasAnyCleanupAction(
    const scratchbird::core::memory::MemoryPressureDecision& pressure) {
  namespace memory = scratchbird::core::memory;
  return PressureHasAction(pressure, memory::MemoryPressureActionKind::prefer_spill) ||
         PressureHasAction(pressure, memory::MemoryPressureActionKind::forced_spill) ||
         PressureHasAction(pressure, memory::MemoryPressureActionKind::shrink_page_cache) ||
         PressureHasAction(pressure, memory::MemoryPressureActionKind::background_cleanup) ||
         PressureHasAction(pressure, memory::MemoryPressureActionKind::block_large_grants);
}

bool PressureHasAnyCancelAction(
    const scratchbird::core::memory::MemoryPressureDecision& pressure) {
  namespace memory = scratchbird::core::memory;
  return PressureHasAction(pressure, memory::MemoryPressureActionKind::cancel_query) ||
         PressureHasAction(pressure, memory::MemoryPressureActionKind::forced_cancel) ||
         PressureHasAction(pressure, memory::MemoryPressureActionKind::refuse_allocation) ||
         PressureHasAction(pressure,
                           memory::MemoryPressureActionKind::
                               suspend_noncritical_agents_jobs) ||
         PressureHasAction(pressure,
                           memory::MemoryPressureActionKind::
                               emergency_admission_shutdown);
}

bool LowPriorityLane(AgentExecutionLaneKind lane) {
  return lane == AgentExecutionLaneKind::low_priority_background ||
         lane == AgentExecutionLaneKind::optimizer_advisory ||
         lane == AgentExecutionLaneKind::support_observability;
}

bool ForegroundProtectedLane(
    const AgentMemoryPressureLaneIntegrationRequest& request) {
  return request.foreground_work_critical ||
         request.lane_policy.lane == AgentExecutionLaneKind::foreground_guard ||
         (request.lane_policy.protect_foreground_work &&
          request.lane_policy.foreground_database_work_active);
}

std::uint64_t QueueBacklogDepth(const AgentExecutionLanePolicy& policy) {
  if (policy.max_queue_depth == 0 ||
      policy.current_queue_depth <= policy.max_queue_depth) {
    return 0;
  }
  return policy.current_queue_depth - policy.max_queue_depth;
}

std::uint64_t EstimateMetricRowOverheadMicroseconds(
    const std::vector<AgentMemoryPressureLaneMetricRow>& rows,
    const std::vector<std::string>& evidence) {
  std::uint64_t chars = 0;
  for (const auto& row : rows) {
    chars += static_cast<std::uint64_t>(row.key.size() + row.value.size());
  }
  for (const auto& item : evidence) {
    chars += static_cast<std::uint64_t>(item.size());
  }
  return static_cast<std::uint64_t>(rows.size() + evidence.size()) +
         ((chars + 127) / 128);
}

void AddMetricRow(AgentMemoryPressureLaneIntegrationDecision* result,
                  const AgentMemoryPressureLaneIntegrationRequest& request,
                  std::string key,
                  std::string value,
                  bool redacted = false) {
  if (result == nullptr || result->metric_rows.size() >=
                                static_cast<std::size_t>(
                                    request.max_evidence_rows)) {
    return;
  }
  AgentMemoryPressureLaneMetricRow row;
  row.key = std::move(key);
  row.value = std::move(value);
  row.redacted = redacted;
  result->metric_rows.push_back(std::move(row));
}

void AddLaneNoAuthorityRows(
    AgentMemoryPressureLaneIntegrationDecision* result,
    const AgentMemoryPressureLaneIntegrationRequest& request) {
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.transaction_finality_authority",
               "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.visibility_authority", "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.authorization_security_authority",
               "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.recovery_authority", "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.parser_authority", "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.donor_authority", "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.wal_authority", "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.benchmark_authority", "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.optimizer_plan_authority", "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.index_finality_authority", "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.provider_finality_authority",
               "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.cluster_authority", "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.memory_authority", "false");
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.agent_action_authority", "false");
}

void AddPressureActionRows(
    AgentMemoryPressureLaneIntegrationDecision* result,
    const AgentMemoryPressureLaneIntegrationRequest& request) {
  namespace memory = scratchbird::core::memory;
  for (const auto action : request.pressure_decision.actions) {
    AddMetricRow(result, request, "agent_memory_pressure_lane.pressure_action",
                 memory::MemoryPressureActionKindName(action));
  }
}

void AddBaseLaneRows(AgentMemoryPressureLaneIntegrationDecision* result,
                     const AgentMemoryPressureLaneIntegrationRequest& request) {
  namespace memory = scratchbird::core::memory;
  AddMetricRow(result, request, "agent_memory_pressure_lane.schema_version",
               "sb.agent.memory_pressure_lane.v1");
  AddMetricRow(result, request, "agent_memory_pressure_lane.agent_type_id",
               request.agent_type_id);
  AddMetricRow(result, request, "agent_memory_pressure_lane.action_id",
               request.action_id);
  AddMetricRow(result, request, "agent_memory_pressure_lane.operation_id",
               request.operation_id);
  AddMetricRow(result, request, "agent_memory_pressure_lane.lane",
               AgentExecutionLaneKindName(request.lane_policy.lane));
  AddMetricRow(result, request, "agent_memory_pressure_lane.lane_id",
               request.lane_policy.lane_id);
  AddMetricRow(result, request, "agent_memory_pressure_lane.queue_depth",
               std::to_string(request.lane_policy.current_queue_depth));
  AddMetricRow(result, request, "agent_memory_pressure_lane.queue_max_depth",
               std::to_string(request.lane_policy.max_queue_depth));
  AddMetricRow(result, request, "agent_memory_pressure_lane.queue_backlog_depth",
               std::to_string(QueueBacklogDepth(request.lane_policy)));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.reservation_hierarchical_token_present",
               BoolText(request.reservation_result.hierarchical_reservation_created &&
                        !request.reservation_result.hierarchical_reservation
                             .token_id.empty()));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.reservation_resource_token_present",
               BoolText(request.reservation_result.resource_reservation_created &&
                        !request.reservation_result.resource_reservation.token_id
                             .empty()));
  AddMetricRow(result, request, "agent_memory_pressure_lane.metric_family",
               request.metric_snapshot.metric_family);
  AddMetricRow(result, request, "agent_memory_pressure_lane.metric_namespace",
               request.metric_snapshot.namespace_path);
  AddMetricRow(result, request, "agent_memory_pressure_lane.metric_generation",
               std::to_string(request.metric_snapshot.generation));
  AddMetricRow(result, request, "agent_memory_pressure_lane.metric_digest",
               request.metric_snapshot.digest);
  AddMetricRow(result, request, "agent_memory_pressure_lane.metric_scope_uuid",
               request.metric_snapshot.scope_uuid);
  AddMetricRow(result, request, "agent_memory_pressure_lane.metric_source_quality",
               std::to_string(static_cast<int>(
                   request.metric_snapshot.source_quality)));
  AddMetricRow(result, request, "agent_memory_pressure_lane.pressure_state",
               memory::MemoryPressureStateName(
                   request.pressure_decision.new_state));
  AddMetricRow(result, request, "agent_memory_pressure_lane.pressure_percent",
               std::to_string(request.pressure_decision.pressure_percent));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.pressure_ordinary_admission_allowed",
               BoolText(request.pressure_decision.ordinary_admission_allowed));
  AddMetricRow(result, request, "agent_memory_pressure_lane.production_environment",
               BoolText(request.production_environment));
  AddMetricRow(result, request, "agent_memory_pressure_lane.test_build",
               BoolText(request.test_build));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.local_cluster_pressure_claim",
               BoolText(request.local_cluster_pressure_claim ||
                        request.lane_policy.local_cluster_lane_claim));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.external_cluster_provider_proof_present",
               BoolText(request.external_cluster_provider_proof_present ||
                        request.lane_policy
                            .external_cluster_provider_proof_present));
  AddPressureActionRows(result, request);
  AddLaneNoAuthorityRows(result, request);
}

void FinalizeLaneIntegration(
    AgentMemoryPressureLaneIntegrationDecision* result,
    const AgentMemoryPressureLaneIntegrationRequest& request) {
  if (result == nullptr) {
    return;
  }
  AddMetricRow(result, request, "agent_memory_pressure_lane.decision",
               AgentMemoryPressureLaneDecisionKindName(result->decision));
  AddMetricRow(result, request, "agent_memory_pressure_lane.diagnostic_code",
               result->status.diagnostic_code);
  AddMetricRow(result, request, "agent_memory_pressure_lane.fail_closed",
               BoolText(result->fail_closed));
  AddMetricRow(result, request, "agent_memory_pressure_lane.action_allowed",
               BoolText(result->action_allowed));
  AddMetricRow(result, request, "agent_memory_pressure_lane.throttle_required",
               BoolText(result->throttle_required));
  AddMetricRow(result, request, "agent_memory_pressure_lane.cancel_required",
               BoolText(result->cancel_required));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.shed_or_defer_required",
               BoolText(result->shed_or_defer_required));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.force_spill_or_cleanup_required",
               BoolText(result->force_spill_or_cleanup_required));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.foreground_protected",
               BoolText(result->foreground_protected));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.evidence_row_count",
               std::to_string(result->metric_rows.size()));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.max_evidence_rows",
               std::to_string(request.max_evidence_rows));
  result->estimated_overhead_microseconds =
      EstimateMetricRowOverheadMicroseconds(result->metric_rows,
                                            result->evidence);
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.overhead_budget_microseconds",
               std::to_string(request.overhead_budget_microseconds));
  AddMetricRow(result, request,
               "agent_memory_pressure_lane.estimated_overhead_microseconds",
               std::to_string(result->estimated_overhead_microseconds));
  result->overhead_budget_evidence_present =
      request.overhead_budget_microseconds != 0;
  result->evidence_bounded =
      request.max_evidence_rows != 0 &&
      result->metric_rows.size() <=
          static_cast<std::size_t>(request.max_evidence_rows);
  result->evidence.push_back("agent_memory_pressure_lane.evidence_bounded=" +
                             BoolText(result->evidence_bounded));
  result->evidence.push_back(
      "agent_memory_pressure_lane.overhead_budget_evidence_present=" +
      BoolText(result->overhead_budget_evidence_present));
  result->evidence.push_back(
      "agent_memory_pressure_lane.estimated_overhead_microseconds=" +
      std::to_string(result->estimated_overhead_microseconds));
  result->evidence.push_back(
      "agent_memory_pressure_lane.authority_scope=evidence_only_not_transaction_finality_visibility_authorization_security_recovery_parser_donor_wal_benchmark_optimizer_plan_index_finality_provider_finality_cluster_memory_or_agent_action_authority");
}

AgentMemoryPressureLaneIntegrationDecision LaneIntegrationFailure(
    const AgentMemoryPressureLaneIntegrationRequest& request,
    std::string code,
    std::string detail) {
  AgentMemoryPressureLaneIntegrationDecision result;
  result.status = MemoryError(std::move(code), std::move(detail));
  result.decision = AgentMemoryPressureLaneDecisionKind::fail_closed;
  result.ok = false;
  result.fail_closed = true;
  result.action_allowed = false;
  result.evidence.push_back("CEIC_083_AGENT_MEMORY_PRESSURE_METRIC_INTEGRATION");
  result.evidence.push_back("agent_memory_pressure_lane.fail_closed=true");
  result.evidence.push_back(
      "agent_memory_pressure_lane.diagnostic_code=" +
      result.status.diagnostic_code);
  AddBaseLaneRows(&result, request);
  FinalizeLaneIntegration(&result, request);
  return result;
}

AgentMemoryPressureLaneDecisionKind SelectLanePressureDecision(
    const AgentMemoryPressureLaneIntegrationRequest& request,
    bool foreground_protected) {
  namespace memory = scratchbird::core::memory;
  const auto state = request.pressure_decision.new_state;
  if (state == memory::MemoryPressureState::normal) {
    return AgentMemoryPressureLaneDecisionKind::allow;
  }
  if (state == memory::MemoryPressureState::recovery) {
    return request.pressure_decision.recovery_readmission_throttled
               ? AgentMemoryPressureLaneDecisionKind::throttle
               : AgentMemoryPressureLaneDecisionKind::allow;
  }
  if (foreground_protected) {
    return AgentMemoryPressureLaneDecisionKind::throttle;
  }
  if (state == memory::MemoryPressureState::emergency_pressure) {
    return AgentMemoryPressureLaneDecisionKind::cancel;
  }
  if (state == memory::MemoryPressureState::high_pressure &&
      LowPriorityLane(request.lane_policy.lane)) {
    return AgentMemoryPressureLaneDecisionKind::shed_defer;
  }
  if (request.lane_policy.lane == AgentExecutionLaneKind::memory_pressure &&
      PressureHasAnyCleanupAction(request.pressure_decision)) {
    return AgentMemoryPressureLaneDecisionKind::force_spill_cleanup;
  }
  if (PressureHasAnyCancelAction(request.pressure_decision) &&
      LowPriorityLane(request.lane_policy.lane)) {
    return AgentMemoryPressureLaneDecisionKind::shed_defer;
  }
  return AgentMemoryPressureLaneDecisionKind::throttle;
}

}  // namespace

bool IsMemoryAffectingAgent(const std::string& agent_type_id) {
  return agent_type_id == "memory_governor" ||
         agent_type_id == "admission_control_manager" ||
         agent_type_id == "support_bundle_triage_agent" ||
         HasMemoryActionContract(agent_type_id);
}

ResourceGovernanceQuotaDescriptor DefaultAgentMemoryQuotaDescriptor(
    const std::string& agent_type_id,
    std::uint64_t limit_bytes,
    std::uint64_t generation) {
  ResourceGovernanceQuotaDescriptor descriptor;
  descriptor.descriptor_id = "agent_memory_budget:" + agent_type_id;
  descriptor.family = ResourceGovernanceFamily::kBackgroundJob;
  descriptor.source = ResourceGovernanceDescriptorSource::kAgentRuntime;
  descriptor.source_path_or_label = "core.agents.agent_memory_coupling";
  descriptor.descriptor_generation = generation;
  descriptor.expected_generation = generation;
  descriptor.limits.memory_bytes = static_cast<std::int64_t>(limit_bytes);
  descriptor.limits.device_memory_bytes = 1;
  descriptor.limits.pinned_memory_bytes = 1;
  descriptor.limits.io_bytes = 1;
  descriptor.limits.io_ops = 1;
  descriptor.limits.worker_threads = 1;
  descriptor.limits.backlog_items = 1;
  descriptor.limits.candidate_rows = 1;
  descriptor.limits.cache_entries = 1;
  descriptor.limits.batch_rows = 1;
  descriptor.limits.fragments = 1;
  descriptor.limits.lanes = 1;
  descriptor.limits.time_budget_microseconds = 1;
  descriptor.over_limit_action = ResourceGovernanceAction::kFailClosed;
  descriptor.benchmark_clean = true;
  descriptor.runtime_dependency_present = true;
  return descriptor;
}

AgentMemoryReservationResult AcquireAgentMemoryReservations(
    const AgentMemoryReservationRequest& request,
    HierarchicalMemoryBudgetLedger* memory_budget_ledger,
    ResourceGovernanceReservationLedger* resource_ledger) {
  if (request.operation_id.empty() || request.owner_scope.empty()) {
    return ReservationFailure(
        request,
        "SB_AGENT_MEMORY_BUDGET.IDENTITY_REQUIRED",
        "operation_id_and_owner_scope_required");
  }
  if (!request.live_action && !request.allocation_or_memory_action) {
    return ReservationFailure(
        request,
        "SB_AGENT_MEMORY_BUDGET.MEMORY_ACTION_REQUIRED",
        "memory_affecting_action_required");
  }
  if (!IsMemoryAffectingAgent(request.agent_type_id)) {
    return ReservationFailure(
        request,
        "SB_AGENT_MEMORY_BUDGET.AGENT_NOT_MEMORY_AFFECTING",
        request.agent_type_id);
  }
  if (request.memory_bytes == 0) {
    return ReservationFailure(
        request,
        "SB_AGENT_MEMORY_BUDGET.UNBOUNDED_MEMORY_REFUSED",
        "positive_memory_reservation_required");
  }
  if (memory_budget_ledger == nullptr || resource_ledger == nullptr ||
      request.leaf_memory_scope_id.empty()) {
    return ReservationFailure(
        request,
        "SB_AGENT_MEMORY_BUDGET.RESERVATION_LEDGER_REQUIRED",
        "hierarchical_and_resource_ledgers_required");
  }

  AgentMemoryReservationResult result;
  AppendBudgetEvidence(&result, request);

  HierarchicalMemoryBudgetReserveRequest memory_request;
  memory_request.operation_id = request.operation_id;
  memory_request.owner_scope = request.owner_scope;
  memory_request.leaf_scope_id = request.leaf_memory_scope_id;
  memory_request.bytes = request.memory_bytes;
  auto memory_result = memory_budget_ledger->Reserve(memory_request);
  for (const auto& item : memory_result.evidence) {
    Add(&result.evidence, "agent_memory_budget.hierarchical." + item);
  }
  if (!memory_result.ok) {
    result.status = memory_result.status;
    result.ok = false;
    result.fail_closed = true;
    Add(&result.evidence, "agent_memory_budget.fail_closed=true");
    Add(&result.evidence,
        "agent_memory_budget.diagnostic_code=" +
            memory_result.diagnostic_code);
    return result;
  }

  ResourceGovernanceReservationAcquireRequest resource_request;
  resource_request.owner_scope = request.owner_scope;
  resource_request.admission.operation_id = request.operation_id;
  resource_request.admission.expected_family =
      ResourceGovernanceFamily::kBackgroundJob;
  resource_request.admission.descriptor = DefaultAgentMemoryQuotaDescriptor(
      request.agent_type_id, request.memory_bytes);
  resource_request.admission.requested.memory_bytes =
      static_cast<std::int64_t>(request.memory_bytes);
  auto resource_result = resource_ledger->Acquire(std::move(resource_request));
  for (const auto& item : resource_result.evidence) {
    Add(&result.evidence, "agent_memory_budget.resource." + item);
  }
  if (!resource_result.ok) {
    (void)memory_budget_ledger->Release(memory_result.reservation.token_id);
    result.status = resource_result.status;
    result.ok = false;
    result.fail_closed = true;
    result.hierarchical_reservation_created = false;
    Add(&result.evidence, "agent_memory_budget.fail_closed=true");
    Add(&result.evidence,
        "agent_memory_budget.diagnostic_code=" +
            resource_result.diagnostic_code);
    Add(&result.evidence,
        "agent_memory_budget.hierarchical_rolled_back=true");
    return result;
  }

  result.status = MemoryOk("SB_AGENT_MEMORY_BUDGET.RESERVATION_CREATED",
                           request.agent_type_id);
  result.ok = true;
  result.fail_closed = false;
  result.hierarchical_reservation_created = true;
  result.resource_reservation_created = true;
  result.hierarchical_reservation = memory_result.reservation;
  result.resource_reservation = resource_result.reservation;
  Add(&result.evidence, "agent_memory_budget.fail_closed=false");
  Add(&result.evidence,
      "agent_memory_budget.hierarchical_reservation_created=true");
  Add(&result.evidence,
      "agent_memory_budget.resource_reservation_created=true");
  Add(&result.evidence,
      "agent_memory_budget.diagnostic_code=" + result.status.diagnostic_code);
  return result;
}

AgentMemoryMetricSnapshotDecision ValidateAgentMemoryMetricSnapshot(
    const AgentMemoryMetricSnapshot& snapshot,
    std::uint64_t now_microseconds,
    bool test_build) {
  AgentMemoryMetricSnapshotDecision decision;
  Add(&decision.evidence, "MMCH_AGENT_MEMORY_METRIC_SNAPSHOT_AUTHORITY");
  Add(&decision.evidence,
      "agent_memory_metric.metric_family=" + snapshot.metric_family);
  Add(&decision.evidence,
      "agent_memory_metric.generation=" + std::to_string(snapshot.generation));
  Add(&decision.evidence,
      "agent_memory_metric.scope_uuid=" + snapshot.scope_uuid);
  Add(&decision.evidence,
      "agent_memory_metric.digest_present=" + BoolText(!snapshot.digest.empty()));
  Add(&decision.evidence,
      "agent_memory_metric.source_quality=" +
          std::to_string(static_cast<int>(snapshot.source_quality)));

  auto fail = [&](std::string code, std::string detail) {
    decision.status = MemoryError(std::move(code), std::move(detail));
    decision.ok = false;
    decision.fail_closed = true;
    Add(&decision.evidence, "agent_memory_metric.fail_closed=true");
    Add(&decision.evidence,
        "agent_memory_metric.diagnostic_code=" +
            decision.status.diagnostic_code);
    return decision;
  };

  if (snapshot.generation == 0 || snapshot.sampled_at_microseconds == 0 ||
      snapshot.scope_uuid.empty() || snapshot.digest.empty()) {
    return fail("SB_AGENT_MEMORY_METRIC.SNAPSHOT_METADATA_REQUIRED",
                "generation_timestamp_scope_digest_required");
  }
  if (snapshot.registry_only_relaxed_path) {
    if (!test_build || !snapshot.test_only_relaxed_path) {
      return fail("SB_AGENT_MEMORY_METRIC.REGISTRY_RELAXED_PRODUCTION_REFUSED",
                  "registry_only_relaxed_path_is_test_only");
    }
    decision.accepted_registry_relaxed_test_path = true;
  }
  if (!snapshot.trusted ||
      snapshot.source_quality == AgentMetricSourceQuality::unknown) {
    return fail("SB_AGENT_MEMORY_METRIC.UNTRUSTED_SOURCE_REFUSED",
                "trusted_metric_source_required");
  }
  if (!snapshot.schema_compatible) {
    return fail("SB_AGENT_MEMORY_METRIC.SCHEMA_INCOMPATIBLE",
                "schema_compatible_metric_snapshot_required");
  }
  if (snapshot.max_freshness_microseconds != 0 &&
      (now_microseconds <= snapshot.sampled_at_microseconds ||
       now_microseconds - snapshot.sampled_at_microseconds >
           snapshot.max_freshness_microseconds)) {
    return fail("SB_AGENT_MEMORY_METRIC.STALE_REFUSED",
                "fresh_metric_snapshot_required");
  }

  decision.status =
      MemoryOk("SB_AGENT_MEMORY_METRIC.SNAPSHOT_ACCEPTED",
               snapshot.metric_family);
  decision.ok = true;
  decision.fail_closed = false;
  Add(&decision.evidence, "agent_memory_metric.fail_closed=false");
  Add(&decision.evidence,
      "agent_memory_metric.diagnostic_code=" +
          decision.status.diagnostic_code);
  return decision;
}

const char* AgentMemoryPressureActionKindName(
    AgentMemoryPressureActionKind action_kind) {
  switch (action_kind) {
    case AgentMemoryPressureActionKind::page_cache_shrink:
      return "page_cache_shrink";
    case AgentMemoryPressureActionKind::spill_cleanup:
      return "spill_cleanup";
    case AgentMemoryPressureActionKind::temp_cleanup:
      return "temp_cleanup";
    case AgentMemoryPressureActionKind::background_reclamation:
      return "background_reclamation";
  }
  return "unknown";
}

AgentMemoryPressureActionBoundaryDecision EvaluateAgentMemoryPressureActionBoundary(
    const AgentMemoryPressureActionBoundaryRequest& request) {
  if (!IsMemoryAffectingAgent(request.agent_type_id)) {
    return BoundaryFailure(request,
                           "SB_AGENT_MEMORY_ACTION.AGENT_NOT_MEMORY_AFFECTING",
                           request.agent_type_id);
  }
  if (!ApprovedPressureAction(request.action_kind) ||
      !request.action_boundary_approved) {
    return BoundaryFailure(
        request,
        "SB_AGENT_MEMORY_ACTION.BOUNDARY_NOT_APPROVED",
        "approved_memory_pressure_action_boundary_required");
  }
  if (request.durable_evidence_uuid.empty() ||
      request.sidecar_only_evidence ||
      request.in_memory_only_decision) {
    return BoundaryFailure(
        request,
        "SB_AGENT_MEMORY_ACTION.DURABLE_EVIDENCE_REQUIRED",
        "durable_non_sidecar_evidence_required");
  }
  if (request.outcome_verification_uuid.empty() ||
      !request.outcome_verified) {
    return BoundaryFailure(
        request,
        "SB_AGENT_MEMORY_ACTION.OUTCOME_VERIFICATION_REQUIRED",
        "verified_outcome_required");
  }
  if (request.probe_only_action) {
    return BoundaryFailure(
        request,
        "SB_AGENT_MEMORY_ACTION.PROBE_ONLY_REFUSED",
        "probe_only_memory_actions_are_not_live_actions");
  }

  AgentMemoryPressureActionBoundaryDecision decision;
  decision.status = MemoryOk("SB_AGENT_MEMORY_ACTION.BOUNDARY_ACCEPTED",
                             request.action_id);
  decision.ok = true;
  decision.fail_closed = false;
  decision.action_allowed = true;
  Add(&decision.evidence, "MMCH_AGENT_MEMORY_PRESSURE_ACTION_BOUNDARY");
  Add(&decision.evidence,
      "agent_memory_action.agent_type_id=" + request.agent_type_id);
  Add(&decision.evidence,
      "agent_memory_action.action_id=" + request.action_id);
  Add(&decision.evidence,
      "agent_memory_action.kind=" +
          std::string(AgentMemoryPressureActionKindName(request.action_kind)));
  Add(&decision.evidence,
      "agent_memory_action.durable_evidence_uuid=" +
          request.durable_evidence_uuid);
  Add(&decision.evidence,
      "agent_memory_action.outcome_verification_uuid=" +
          request.outcome_verification_uuid);
  Add(&decision.evidence,
      "agent_memory_action.fail_closed=false");
  Add(&decision.evidence,
      "agent_memory_action.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");
  return decision;
}

AgentMemoryEvidenceBundleResult BuildAgentMemoryEvidenceBundle(
    const AgentMemoryEvidenceBundleRequest& request) {
  AgentMemoryEvidenceBundleResult result;
  Add(&result.evidence, "MMCH_AGENT_MEMORY_EVIDENCE_REDACTION_OVERHEAD");
  Add(&result.evidence,
      "agent_memory_evidence.agent_type_id=" + request.agent_type_id);
  Add(&result.evidence,
      "agent_memory_evidence.support_bundle_uuid=" +
          request.support_bundle_uuid);
  Add(&result.evidence,
      "agent_memory_evidence.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");

  result.estimated_overhead_bytes = EstimateEvidenceOverhead(request);
  result.overhead_budget_evidence_present = request.overhead_budget_bytes != 0;
  Add(&result.evidence,
      "agent_memory_evidence.estimated_overhead_bytes=" +
          std::to_string(result.estimated_overhead_bytes));
  Add(&result.evidence,
      "agent_memory_evidence.overhead_budget_bytes=" +
          std::to_string(request.overhead_budget_bytes));

  if (request.support_bundle_uuid.empty() ||
      !result.overhead_budget_evidence_present) {
    result.status = MemoryError(
        "SB_AGENT_MEMORY_EVIDENCE.BUDGET_EVIDENCE_REQUIRED",
        "support_bundle_uuid_and_overhead_budget_required");
    Add(&result.evidence, "agent_memory_evidence.fail_closed=true");
    return result;
  }
  if (result.estimated_overhead_bytes > request.overhead_budget_bytes) {
    result.status = MemoryError(
        "SB_AGENT_MEMORY_EVIDENCE.OVERHEAD_BUDGET_EXCEEDED",
        "agent_memory_evidence_overhead_exceeded_budget");
    Add(&result.evidence, "agent_memory_evidence.fail_closed=true");
    return result;
  }

  for (const auto& field : request.fields) {
    AgentMemoryEvidenceBundleRow row;
    row.key = field.key;
    row.redacted = field.protected_material && !request.allow_protected_material;
    row.value = row.redacted ? "redacted" : field.value;
    row.tamper_evidence_digest =
        EvidenceDigest(request.support_bundle_uuid + "|" + row.key + "|" +
                       row.value);
    result.rows.push_back(std::move(row));
  }
  result.status = MemoryOk("SB_AGENT_MEMORY_EVIDENCE.BUNDLE_ACCEPTED",
                           request.support_bundle_uuid);
  result.ok = true;
  result.fail_closed = false;
  Add(&result.evidence, "agent_memory_evidence.fail_closed=false");
  Add(&result.evidence, "agent_memory_evidence.protected_material_redacted=true");
  Add(&result.evidence, "agent_memory_evidence.tamper_metadata_preserved=true");
  Add(&result.evidence, "agent_memory_evidence.overhead_budget_evidence=true");
  return result;
}

AgentMemoryCrashRestartRouteResult EvaluateAgentMemoryCrashRestartRoute(
    const AgentMemoryCrashRestartRouteRequest& request) {
  AgentMemoryCrashRestartRouteResult result;
  Add(&result.evidence, "MMCH_AGENT_MEMORY_CRASH_RESTART_ROUTE");
  Add(&result.evidence,
      "agent_memory_crash.agent_type_id=" + request.agent_type_id);
  Add(&result.evidence, "agent_memory_crash.route_id=" + request.route_id);
  Add(&result.evidence,
      "agent_memory_crash.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");

  auto fail = [&](std::string code, std::string detail) {
    result.status = MemoryError(std::move(code), std::move(detail));
    result.ok = false;
    result.fail_closed = true;
    result.refused_deterministically = true;
    Add(&result.evidence, "agent_memory_crash.fail_closed=true");
    Add(&result.evidence,
        "agent_memory_crash.diagnostic_code=" +
            result.status.diagnostic_code);
    return result;
  };

  if (!request.crash_recovery_mode) {
    return fail("SB_AGENT_MEMORY_CRASH.RECOVERY_MODE_REQUIRED",
                "crash_recovery_route_required");
  }
  if (request.memory_claims_recovery_authority) {
    return fail("SB_AGENT_MEMORY_CRASH.RECOVERY_AUTHORITY_REFUSED",
                "memory_evidence_cannot_be_recovery_authority");
  }
  if (request.route_id.empty() ||
      request.persisted_route_generation == 0 ||
      request.recovered_route_generation == 0) {
    return fail("SB_AGENT_MEMORY_CRASH.ROUTE_METADATA_REQUIRED",
                "route_id_and_generations_required");
  }
  if (!request.durable_evidence_available ||
      !request.outcome_verification_available) {
    return fail("SB_AGENT_MEMORY_CRASH.EVIDENCE_REQUIRED",
                "durable_evidence_and_outcome_verification_required");
  }
  if (request.persisted_route_generation != request.recovered_route_generation) {
    return fail("SB_AGENT_MEMORY_CRASH.ROUTE_GENERATION_MISMATCH",
                "persisted_recovered_route_generation_mismatch");
  }

  result.status =
      MemoryOk("SB_AGENT_MEMORY_CRASH.RECOVERED_DETERMINISTICALLY",
               request.route_id);
  result.ok = true;
  result.fail_closed = false;
  result.recovered = true;
  Add(&result.evidence, "agent_memory_crash.fail_closed=false");
  Add(&result.evidence, "agent_memory_crash.recovered=true");
  Add(&result.evidence,
      "agent_memory_crash.diagnostic_code=" + result.status.diagnostic_code);
  return result;
}

AgentMemoryProductionSeparationResult ValidateAgentMemoryProductionSeparation(
    const AgentMemoryProductionSeparationInput& input) {
  AgentMemoryProductionSeparationResult result;
  Add(&result.evidence, "MMCH_AGENT_MEMORY_PRODUCTION_TEST_SEPARATION");
  Add(&result.evidence,
      "agent_memory_production.production_build=" +
          BoolText(input.production_build));
  Add(&result.evidence,
      "agent_memory_production.authority_scope=evidence_only_not_transaction_finality_visibility_security_recovery_parser_donor_or_benchmark_authority");

  const bool forbidden =
      input.fixture_agent_state ||
      input.relaxed_metric_path ||
      input.sidecar_only_evidence ||
      input.in_memory_only_decision ||
      input.probe_only_memory_action;
  if (input.production_build && forbidden) {
    result.status =
        MemoryError("SB_AGENT_MEMORY_PRODUCTION.TEST_PATH_REFUSED",
                    "fixture_relaxed_sidecar_memory_only_probe_path_refused");
    result.ok = false;
    result.fail_closed = true;
    Add(&result.evidence, "agent_memory_production.fail_closed=true");
    Add(&result.evidence,
        "agent_memory_production.fixture_agent_state=" +
            BoolText(input.fixture_agent_state));
    Add(&result.evidence,
        "agent_memory_production.relaxed_metric_path=" +
            BoolText(input.relaxed_metric_path));
    Add(&result.evidence,
        "agent_memory_production.sidecar_only_evidence=" +
            BoolText(input.sidecar_only_evidence));
    Add(&result.evidence,
        "agent_memory_production.in_memory_only_decision=" +
            BoolText(input.in_memory_only_decision));
    Add(&result.evidence,
        "agent_memory_production.probe_only_memory_action=" +
            BoolText(input.probe_only_memory_action));
    return result;
  }

  result.status =
      MemoryOk("SB_AGENT_MEMORY_PRODUCTION.SEPARATION_ACCEPTED");
  result.ok = true;
  result.fail_closed = false;
  Add(&result.evidence, "agent_memory_production.fail_closed=false");
  return result;
}

const char* AgentMemoryPressureLaneDecisionKindName(
    AgentMemoryPressureLaneDecisionKind decision) {
  switch (decision) {
    case AgentMemoryPressureLaneDecisionKind::allow: return "allow";
    case AgentMemoryPressureLaneDecisionKind::throttle: return "throttle";
    case AgentMemoryPressureLaneDecisionKind::cancel: return "cancel";
    case AgentMemoryPressureLaneDecisionKind::shed_defer: return "shed_defer";
    case AgentMemoryPressureLaneDecisionKind::force_spill_cleanup:
      return "force_spill_cleanup";
    case AgentMemoryPressureLaneDecisionKind::fail_closed:
      return "fail_closed";
  }
  return "unknown";
}

AgentMemoryPressureLaneIntegrationDecision
EvaluateAgentMemoryPressureLaneIntegration(
    const AgentMemoryPressureLaneIntegrationRequest& request) {
  namespace memory = scratchbird::core::memory;

  if (request.max_evidence_rows == 0) {
    return LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.EVIDENCE_BOUNDS_REQUIRED",
        "bounded_evidence_row_limit_required");
  }
  if (request.overhead_budget_microseconds == 0) {
    return LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.OVERHEAD_BUDGET_REQUIRED",
        "overhead_budget_evidence_required");
  }
  if (!AuthorityClean(request.no_authority)) {
    return LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.FORBIDDEN_AUTHORITY",
        request.agent_type_id);
  }
  const bool local_cluster_claim =
      request.local_cluster_pressure_claim ||
      request.lane_policy.local_cluster_lane_claim;
  const bool external_provider_proof =
      request.external_cluster_provider_proof_present ||
      request.lane_policy.external_cluster_provider_proof_present;
  if (local_cluster_claim && !external_provider_proof) {
    auto result = LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.LOCAL_CLUSTER_PRESSURE_REFUSED",
        "cluster_pressure_requires_external_provider_proof");
    result.local_cluster_refused = true;
    return result;
  }
  if (!request.lane_decision.status.ok || !request.lane_decision.admitted) {
    return LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.LANE_NOT_ADMITTED",
        request.lane_decision.status.diagnostic_code);
  }

  const bool hierarchical_token_present =
      request.reservation_result.hierarchical_reservation_created &&
      !request.reservation_result.hierarchical_reservation.token_id.empty();
  const bool resource_token_present =
      request.reservation_result.resource_reservation_created &&
      !request.reservation_result.resource_reservation.token_id.empty();
  if (!request.reservation_result.ok || !hierarchical_token_present ||
      !resource_token_present) {
    return LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.RESERVATION_REQUIRED",
        request.reservation_result.status.diagnostic_code);
  }

  if (!request.metric_decision.ok ||
      request.metric_snapshot.metric_family.empty() ||
      request.metric_snapshot.namespace_path.empty() ||
      request.metric_snapshot.generation == 0 ||
      request.metric_snapshot.sampled_at_microseconds == 0 ||
      request.metric_snapshot.scope_uuid.empty() ||
      request.metric_snapshot.digest.empty()) {
    return LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.METRIC_SNAPSHOT_REQUIRED",
        request.metric_decision.status.diagnostic_code);
  }
  if (!request.metric_snapshot.trusted ||
      request.metric_snapshot.source_quality ==
          AgentMetricSourceQuality::unknown ||
      !request.metric_snapshot.schema_compatible ||
      (request.production_environment &&
       (request.metric_snapshot.registry_only_relaxed_path ||
        request.metric_decision.accepted_registry_relaxed_test_path))) {
    return LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.UNTRUSTED_METRIC_SNAPSHOT",
        "trusted_schema_compatible_production_metric_snapshot_required");
  }

  if (!request.pressure_evidence_present ||
      request.pressure_decision.evidence.empty()) {
    return LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.PRESSURE_EVIDENCE_REQUIRED",
        "trusted_ceic_017_pressure_decision_evidence_required");
  }
  if (!request.pressure_decision.ok() || request.pressure_decision.fail_closed) {
    return LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.PRESSURE_DECISION_REFUSED",
        request.pressure_decision.diagnostic.diagnostic_code);
  }

  AgentMemoryPressureLaneIntegrationDecision result;
  result.evidence.push_back("CEIC_083_AGENT_MEMORY_PRESSURE_METRIC_INTEGRATION");
  result.evidence.push_back("agent_memory_pressure_lane.fail_closed=false");
  result.foreground_protected = ForegroundProtectedLane(request);
  result.decision =
      SelectLanePressureDecision(request, result.foreground_protected);
  result.ok = true;
  result.fail_closed = false;

  switch (result.decision) {
    case AgentMemoryPressureLaneDecisionKind::allow:
      result.status = MemoryOk("SB_AGENT_MEMORY_PRESSURE_LANE.ALLOW",
                               request.agent_type_id);
      result.action_allowed = true;
      break;
    case AgentMemoryPressureLaneDecisionKind::throttle:
      result.status = MemoryOk("SB_AGENT_MEMORY_PRESSURE_LANE.THROTTLE",
                               request.agent_type_id);
      result.throttle_required = true;
      result.action_allowed = false;
      break;
    case AgentMemoryPressureLaneDecisionKind::cancel:
      result.status = MemoryOk(
          "SB_AGENT_MEMORY_PRESSURE_LANE.CANCEL_NONCRITICAL",
          request.agent_type_id);
      result.cancel_required = true;
      result.action_allowed = false;
      break;
    case AgentMemoryPressureLaneDecisionKind::shed_defer:
      result.status = MemoryOk("SB_AGENT_MEMORY_PRESSURE_LANE.SHED_DEFER",
                               request.agent_type_id);
      result.shed_or_defer_required = true;
      result.action_allowed = false;
      break;
    case AgentMemoryPressureLaneDecisionKind::force_spill_cleanup:
      result.status = MemoryOk(
          "SB_AGENT_MEMORY_PRESSURE_LANE.FORCE_SPILL_CLEANUP",
          request.agent_type_id);
      result.force_spill_or_cleanup_required = true;
      result.action_allowed = true;
      break;
    case AgentMemoryPressureLaneDecisionKind::fail_closed:
      return LaneIntegrationFailure(
          request,
          "SB_AGENT_MEMORY_PRESSURE_LANE.FAIL_CLOSED",
          request.agent_type_id);
  }

  if (result.foreground_protected &&
      (PressureHasAnyCancelAction(request.pressure_decision) ||
       request.pressure_decision.new_state ==
           memory::MemoryPressureState::emergency_pressure)) {
    result.evidence.push_back(
        "agent_memory_pressure_lane.foreground_cancel_suppressed=true");
  }
  AddBaseLaneRows(&result, request);
  FinalizeLaneIntegration(&result, request);
  if (result.estimated_overhead_microseconds >
      request.overhead_budget_microseconds) {
    return LaneIntegrationFailure(
        request,
        "SB_AGENT_MEMORY_PRESSURE_LANE.OVERHEAD_BUDGET_EXCEEDED",
        std::to_string(result.estimated_overhead_microseconds));
  }
  return result;
}

}  // namespace scratchbird::core::agents
