// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// Memory-coupled operational agent checks. Agent memory evidence is diagnostic
// and support material only; it is not finality, visibility, recovery,
// security, parser, donor, benchmark, optimizer, index, provider, cluster,
// memory, or agent-action authority.

#include "agent_execution_lane_governance.hpp"
#include "agent_runtime.hpp"
#include "memory_pressure_response.hpp"
#include "resource_governance_admission.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::agents {

// MMCH_AGENT_MEMORY_BUDGET_INTEGRATION
struct AgentMemoryReservationRequest {
  std::string agent_type_id;
  std::string action_id;
  std::string operation_id;
  std::string owner_scope;
  std::string leaf_memory_scope_id;
  std::uint64_t memory_bytes = 0;
  bool live_action = false;
  bool allocation_or_memory_action = false;
};

struct AgentMemoryReservationResult {
  AgentRuntimeStatus status;
  bool ok = false;
  bool fail_closed = true;
  bool hierarchical_reservation_created = false;
  bool resource_reservation_created = false;
  HierarchicalMemoryBudgetReservationToken hierarchical_reservation;
  ResourceGovernanceReservationToken resource_reservation;
  std::vector<std::string> evidence;
};

bool IsMemoryAffectingAgent(const std::string& agent_type_id);
ResourceGovernanceQuotaDescriptor DefaultAgentMemoryQuotaDescriptor(
    const std::string& agent_type_id,
    std::uint64_t limit_bytes,
    std::uint64_t generation = 1);
AgentMemoryReservationResult AcquireAgentMemoryReservations(
    const AgentMemoryReservationRequest& request,
    HierarchicalMemoryBudgetLedger* memory_budget_ledger,
    ResourceGovernanceReservationLedger* resource_ledger);

// MMCH_AGENT_MEMORY_METRIC_SNAPSHOT_AUTHORITY
struct AgentMemoryMetricSnapshot {
  std::string metric_family;
  std::string namespace_path;
  std::uint64_t generation = 0;
  std::uint64_t sampled_at_microseconds = 0;
  std::string scope_uuid;
  std::string digest;
  std::uint64_t max_freshness_microseconds = 0;
  AgentMetricSourceQuality source_quality = AgentMetricSourceQuality::unknown;
  bool trusted = false;
  bool schema_compatible = true;
  bool registry_only_relaxed_path = false;
  bool test_only_relaxed_path = false;
};

struct AgentMemoryMetricSnapshotDecision {
  AgentRuntimeStatus status;
  bool ok = false;
  bool fail_closed = true;
  bool accepted_registry_relaxed_test_path = false;
  std::vector<std::string> evidence;
};

AgentMemoryMetricSnapshotDecision ValidateAgentMemoryMetricSnapshot(
    const AgentMemoryMetricSnapshot& snapshot,
    std::uint64_t now_microseconds,
    bool test_build);

// MMCH_AGENT_MEMORY_PRESSURE_ACTION_BOUNDARY
enum class AgentMemoryPressureActionKind {
  page_cache_shrink,
  spill_cleanup,
  temp_cleanup,
  background_reclamation
};

struct AgentMemoryPressureActionBoundaryRequest {
  std::string agent_type_id;
  std::string action_id;
  AgentMemoryPressureActionKind action_kind =
      AgentMemoryPressureActionKind::page_cache_shrink;
  bool action_boundary_approved = false;
  std::string durable_evidence_uuid;
  std::string outcome_verification_uuid;
  bool outcome_verified = false;
  bool sidecar_only_evidence = false;
  bool in_memory_only_decision = false;
  bool probe_only_action = false;
};

struct AgentMemoryPressureActionBoundaryDecision {
  AgentRuntimeStatus status;
  bool ok = false;
  bool fail_closed = true;
  bool action_allowed = false;
  std::vector<std::string> evidence;
};

const char* AgentMemoryPressureActionKindName(
    AgentMemoryPressureActionKind action_kind);
AgentMemoryPressureActionBoundaryDecision EvaluateAgentMemoryPressureActionBoundary(
    const AgentMemoryPressureActionBoundaryRequest& request);

// MMCH_AGENT_MEMORY_EVIDENCE_REDACTION_OVERHEAD
struct AgentMemoryEvidenceField {
  std::string key;
  std::string value;
  bool protected_material = false;
};

struct AgentMemoryEvidenceBundleRequest {
  std::string agent_type_id;
  std::string support_bundle_uuid;
  std::vector<AgentMemoryEvidenceField> fields;
  std::uint64_t overhead_budget_bytes = 0;
  bool allow_protected_material = false;
};

struct AgentMemoryEvidenceBundleRow {
  std::string key;
  std::string value;
  bool redacted = false;
  std::string tamper_evidence_digest;
};

struct AgentMemoryEvidenceBundleResult {
  AgentRuntimeStatus status;
  bool ok = false;
  bool fail_closed = true;
  bool overhead_budget_evidence_present = false;
  std::uint64_t estimated_overhead_bytes = 0;
  std::vector<AgentMemoryEvidenceBundleRow> rows;
  std::vector<std::string> evidence;
};

AgentMemoryEvidenceBundleResult BuildAgentMemoryEvidenceBundle(
    const AgentMemoryEvidenceBundleRequest& request);

// MMCH_AGENT_MEMORY_CRASH_RESTART_ROUTE
struct AgentMemoryCrashRestartRouteRequest {
  std::string agent_type_id;
  std::string route_id;
  std::uint64_t persisted_route_generation = 0;
  std::uint64_t recovered_route_generation = 0;
  bool crash_recovery_mode = false;
  bool durable_evidence_available = false;
  bool outcome_verification_available = false;
  bool memory_claims_recovery_authority = false;
};

struct AgentMemoryCrashRestartRouteResult {
  AgentRuntimeStatus status;
  bool ok = false;
  bool fail_closed = true;
  bool recovered = false;
  bool refused_deterministically = false;
  std::vector<std::string> evidence;
};

AgentMemoryCrashRestartRouteResult EvaluateAgentMemoryCrashRestartRoute(
    const AgentMemoryCrashRestartRouteRequest& request);

// MMCH_AGENT_MEMORY_PRODUCTION_TEST_SEPARATION
struct AgentMemoryProductionSeparationInput {
  bool production_build = true;
  bool fixture_agent_state = false;
  bool relaxed_metric_path = false;
  bool sidecar_only_evidence = false;
  bool in_memory_only_decision = false;
  bool probe_only_memory_action = false;
};

struct AgentMemoryProductionSeparationResult {
  AgentRuntimeStatus status;
  bool ok = false;
  bool fail_closed = true;
  std::vector<std::string> evidence;
};

AgentMemoryProductionSeparationResult ValidateAgentMemoryProductionSeparation(
    const AgentMemoryProductionSeparationInput& input);

// SEARCH_KEY: CEIC_083_AGENT_MEMORY_PRESSURE_METRIC_INTEGRATION
// Integrates CEIC-073 lane admission, CEIC memory reservation evidence,
// trusted metric snapshots, and CEIC-017 pressure decisions. The integration is
// an admission/control-plane decision only; it cannot become transaction
// finality, visibility, authorization/security, recovery, parser, donor, WAL,
// benchmark, optimizer-plan, index-finality, provider-finality, cluster,
// memory, or agent-action authority.
enum class AgentMemoryPressureLaneDecisionKind {
  allow,
  throttle,
  cancel,
  shed_defer,
  force_spill_cleanup,
  fail_closed
};

struct AgentMemoryPressureLaneMetricRow {
  std::string key;
  std::string value;
  bool redacted = false;
};

struct AgentMemoryPressureLaneIntegrationRequest {
  std::string agent_type_id;
  std::string action_id;
  std::string operation_id;
  AgentExecutionLanePolicy lane_policy;
  AgentExecutionLaneDecision lane_decision;
  AgentMemoryReservationResult reservation_result;
  AgentMemoryMetricSnapshot metric_snapshot;
  AgentMemoryMetricSnapshotDecision metric_decision;
  scratchbird::core::memory::MemoryPressureDecision pressure_decision;
  AgentSystemProfileForbiddenAuthority no_authority;
  std::uint64_t overhead_budget_microseconds = 0;
  std::uint64_t max_evidence_rows = 64;
  bool production_environment = true;
  bool test_build = false;
  bool pressure_evidence_present = true;
  bool foreground_work_critical = false;
  bool local_cluster_pressure_claim = false;
  bool external_cluster_provider_proof_present = false;
};

struct AgentMemoryPressureLaneIntegrationDecision {
  AgentRuntimeStatus status;
  AgentMemoryPressureLaneDecisionKind decision =
      AgentMemoryPressureLaneDecisionKind::fail_closed;
  bool ok = false;
  bool fail_closed = true;
  bool action_allowed = false;
  bool throttle_required = false;
  bool cancel_required = false;
  bool shed_or_defer_required = false;
  bool force_spill_or_cleanup_required = false;
  bool foreground_protected = false;
  bool evidence_bounded = false;
  bool overhead_budget_evidence_present = false;
  bool local_cluster_refused = false;
  std::uint64_t estimated_overhead_microseconds = 0;
  std::vector<AgentMemoryPressureLaneMetricRow> metric_rows;
  std::vector<std::string> evidence;
};

const char* AgentMemoryPressureLaneDecisionKindName(
    AgentMemoryPressureLaneDecisionKind decision);
AgentMemoryPressureLaneIntegrationDecision
EvaluateAgentMemoryPressureLaneIntegration(
    const AgentMemoryPressureLaneIntegrationRequest& request);

}  // namespace scratchbird::core::agents
